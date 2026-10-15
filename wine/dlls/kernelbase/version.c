/*
 * Implementation of VERSION.DLL
 *
 * Copyright 1996,1997 Marcus Meissner
 * Copyright 1997 David Cuthbert
 * Copyright 1999 Ulrich Weigand
 * Copyright 2005 Paul Vriens
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 */

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

#include "ntstatus.h"
#include "windef.h"
#include "winbase.h"
#include "winver.h"
#include "winuser.h"
#include "winnls.h"
#include "winreg.h"
#include "winternl.h"
#include "winerror.h"
#include "appmodel.h"

#include "kernelbase.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(ver);

typedef struct
{
    WORD offset;
    WORD length;
    WORD flags;
    WORD id;
    WORD handle;
    WORD usage;
} NE_NAMEINFO;

typedef struct
{
    WORD  type_id;
    WORD  count;
    DWORD resloader;
} NE_TYPEINFO;

struct version_info
{
    DWORD major;
    DWORD minor;
    DWORD build;
};

/***********************************************************************
 * Version Info Structure
 */

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    CHAR  szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT16 Children[];
#endif
} VS_VERSION_INFO_STRUCT16;

typedef struct
{
    WORD  wLength;
    WORD  wValueLength;
    WORD  wType; /* 1:Text, 0:Binary */
    WCHAR szKey[1];
#if 0   /* variable length structure */
    /* DWORD aligned */
    BYTE  Value[];
    /* DWORD aligned */
    VS_VERSION_INFO_STRUCT32 Children[];
#endif
} VS_VERSION_INFO_STRUCT32;

#define VersionInfoIs16( ver ) \
    ( ((const VS_VERSION_INFO_STRUCT16 *)ver)->szKey[0] >= ' ' )

#define DWORD_ALIGN( base, ptr ) \
    ( (LPBYTE)(base) + ((((LPBYTE)(ptr) - (LPBYTE)(base)) + 3) & ~3) )

#define VersionInfo16_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + strlen((ver)->szKey) + 1 )
#define VersionInfo32_Value( ver )  \
    DWORD_ALIGN( (ver), (ver)->szKey + lstrlenW((ver)->szKey) + 1 )

#define VersionInfo16_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT16 *)( VersionInfo16_Value( ver ) + \
                           ( ( (ver)->wValueLength + 3 ) & ~3 ) )
#define VersionInfo32_Children( ver )  \
    (const VS_VERSION_INFO_STRUCT32 *)( VersionInfo32_Value( ver ) + \
                           ( ( (ver)->wValueLength * \
                               ((ver)->wType? 2 : 1) + 3 ) & ~3 ) )

#define VersionInfo16_Next( ver ) \
    (VS_VERSION_INFO_STRUCT16 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )
#define VersionInfo32_Next( ver ) \
    (VS_VERSION_INFO_STRUCT32 *)( (LPBYTE)ver + (((ver)->wLength + 3) & ~3) )


/***********************************************************************
 * Win8 info, reported if the app doesn't provide compat GUID in the manifest and
 * doesn't have higher OS version in PE header.
 */
static const struct version_info windows8_version_info = { 6, 2, 9200 };

/***********************************************************************
 * Win8.1 info, reported if the app doesn't provide compat GUID in the manifest and
 * OS version in PE header is 8.1 or higher but below 10.
 */
static const struct version_info windows8_1_version_info = { 6, 3, 9600 };


/***********************************************************************
 * Windows versions that need compatibility GUID specified in manifest
 * in order to be reported by the APIs.
 */
static const struct
{
    struct version_info info;
    GUID guid;
} version_data[] =
{
    /* Windows 8.1 */
    {
        { 6, 3, 9600 },
        {0x1f676c76,0x80e1,0x4239,{0x95,0xbb,0x83,0xd0,0xf6,0xd0,0xda,0x78}}
    },
    /* Windows 10 */
    {
        { 10, 0, 19045 },
        {0x8e0f7a12,0xbfb3,0x4fe8,{0xb9,0xa5,0x48,0xfd,0x50,0xa1,0x5a,0x9a}}
    }
};


/******************************************************************************
 *  init_current_version
 *
 * Initialize the current_version variable.
 *
 * For compatibility, Windows 8.1 and later report Win8 version unless the app
 * has a manifest or higher OS version in the PE optional header
 * that confirms its compatibility with newer versions of Windows.
 *
 */
static RTL_OSVERSIONINFOEXW current_version;

static BOOL CALLBACK init_current_version(PINIT_ONCE init_once, PVOID parameter, PVOID *context)
{
    struct acci
    {
        DWORD ElementCount;
        COMPATIBILITY_CONTEXT_ELEMENT Elements[1];
    } *acci;
    BOOL have_os_compat_elements = FALSE;
    const struct version_info *ver;
    IMAGE_NT_HEADERS *nt;
    SIZE_T req;
    int idx;

    current_version.dwOSVersionInfoSize = sizeof(current_version);
    if (!set_ntstatus( RtlGetVersion(&current_version) )) return FALSE;

    for (idx = ARRAY_SIZE(version_data); idx--;)
        if ( current_version.dwMajorVersion >  version_data[idx].info.major ||
            (current_version.dwMajorVersion == version_data[idx].info.major &&
             current_version.dwMinorVersion >= version_data[idx].info.minor))
            break;

    if (idx < 0) return TRUE;
    ver = &windows8_version_info;

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, NULL, 0, &req) != STATUS_BUFFER_TOO_SMALL
        || !req)
        goto done;

    if (!(acci = HeapAlloc(GetProcessHeap(), 0, req)))
    {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return FALSE;
    }

    if (RtlQueryInformationActivationContext(0, NtCurrentTeb()->Peb->ActivationContextData, NULL,
            CompatibilityInformationInActivationContext, acci, req, &req) == STATUS_SUCCESS)
    {
        do
        {
            DWORD i;

            for (i = 0; i < acci->ElementCount; i++)
            {
                if (acci->Elements[i].Type != ACTCTX_COMPATIBILITY_ELEMENT_TYPE_OS)
                    continue;

                have_os_compat_elements = TRUE;

                if (IsEqualGUID(&acci->Elements[i].Id, &version_data[idx].guid))
                {
                    ver = &version_data[idx].info;

                    if (ver->major == current_version.dwMajorVersion &&
                        ver->minor == current_version.dwMinorVersion)
                        ver = NULL;

                    idx = 0;  /* break from outer loop */
                    break;
                }
            }
        } while (idx--);
    }
    HeapFree(GetProcessHeap(), 0, acci);

done:
    if (!have_os_compat_elements && current_version.dwMajorVersion >= 10
            && (nt = RtlImageNtHeader(NtCurrentTeb()->Peb->ImageBaseAddress))
            && (nt->OptionalHeader.MajorOperatingSystemVersion > 6
            || (nt->OptionalHeader.MajorOperatingSystemVersion == 6
            && nt->OptionalHeader.MinorOperatingSystemVersion >= 3)))
    {
        if (current_version.dwMajorVersion > 10)
            FIXME("Unsupported current_version.dwMajorVersion %lu.\n", current_version.dwMajorVersion);

        ver = nt->OptionalHeader.MajorOperatingSystemVersion >= 10 ? NULL : &windows8_1_version_info;
    }

    if (ver)
    {
        current_version.dwMajorVersion = ver->major;
        current_version.dwMinorVersion = ver->minor;
        current_version.dwBuildNumber  = ver->build;
    }
    return TRUE;
}


/**********************************************************************
 *  find_entry_by_id
 *
 * Find an entry by id in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_by_id( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                         WORD id, const void *root,
                                                         DWORD root_size )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;
    int min, max, pos;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    min = dir->NumberOfNamedEntries;
    max = min + dir->NumberOfIdEntries - 1;

    if (max >= (root_size - ((INT_PTR)dir - (INT_PTR)root) - sizeof(*dir)) / sizeof(*entry))
        return NULL;

    while (min <= max)
    {
        pos = (min + max) / 2;
        if (entry[pos].Id == id)
        {
            DWORD offset = entry[pos].OffsetToDirectory;
            if (offset > root_size - sizeof(*dir)) return NULL;
            return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + offset);
        }
        if (entry[pos].Id > id) max = pos - 1;
        else min = pos + 1;
    }
    return NULL;
}


/**********************************************************************
 *  find_entry_default
 *
 * Find a default entry in a resource directory
 * Copied from loader/pe_resource.c
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_default( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                           const void *root )
{
    const IMAGE_RESOURCE_DIRECTORY_ENTRY *entry;

    entry = (const IMAGE_RESOURCE_DIRECTORY_ENTRY *)(dir + 1);
    return (const IMAGE_RESOURCE_DIRECTORY *)((const char *)root + entry->OffsetToDirectory);
}


/**********************************************************************
 *  push_language
 *
 * push a language onto the list of languages to try
 */
static inline int push_language( WORD *list, int pos, WORD lang )
{
    int i;
    for (i = 0; i < pos; i++) if (list[i] == lang) return pos;
    list[pos++] = lang;
    return pos;
}


/**********************************************************************
 *  find_entry_language
 */
static const IMAGE_RESOURCE_DIRECTORY *find_entry_language( const IMAGE_RESOURCE_DIRECTORY *dir,
                                                            const void *root, DWORD root_size,
                                                            DWORD flags )
{
    const IMAGE_RESOURCE_DIRECTORY *ret;
    WORD list[9];
    int i, pos = 0;

    if (flags & FILE_VER_GET_LOCALISED)
    {
        /* cf. LdrFindResource_U */
        pos = push_language( list, pos, MAKELANGID( LANG_NEUTRAL, SUBLANG_NEUTRAL ) );
        pos = push_language( list, pos, LANGIDFROMLCID( NtCurrentTeb()->CurrentLocale ) );
        pos = push_language( list, pos, GetUserDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetUserDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, GetSystemDefaultLangID() );
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_NEUTRAL ));
        pos = push_language( list, pos, MAKELANGID( PRIMARYLANGID(GetSystemDefaultLangID()), SUBLANG_DEFAULT ));
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }
    else
    {
        /* FIXME: resolve LN file here */
        pos = push_language( list, pos, MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT ) );
    }

    for (i = 0; i < pos; i++) if ((ret = find_entry_by_id( dir, list[i], root, root_size ))) return ret;
    return find_entry_default( dir, root );
}


static DWORD read_data( HANDLE handle, DWORD offset, void *data, DWORD len )
{
    DWORD res;

    SetFilePointer( handle, offset, NULL, FILE_BEGIN );
    if (!ReadFile( handle, data, len, &res, NULL )) res = 0;
    return res;
}

/***********************************************************************
 *           find_ne_resource         [internal]
 */
static BOOL find_ne_resource( HANDLE handle, DWORD *resLen, DWORD *resOff )
{
    const WORD typeid = VS_FILE_INFO | 0x8000;
    const WORD resid = VS_VERSION_INFO | 0x8000;
    IMAGE_OS2_HEADER nehd;
    NE_TYPEINFO *typeInfo;
    NE_NAMEINFO *nameInfo;
    DWORD nehdoffset = *resOff;
    LPBYTE resTab;
    DWORD resTabSize;
    int count;

    /* Read in NE header */
    if (read_data( handle, nehdoffset, &nehd, sizeof(nehd) ) != sizeof(nehd)) return FALSE;

    resTabSize = nehd.ne_restab - nehd.ne_rsrctab;
    if ( !resTabSize )
    {
        TRACE("No resources in NE dll\n" );
        return FALSE;
    }

    /* Read in resource table */
    resTab = HeapAlloc( GetProcessHeap(), 0, resTabSize );
    if ( !resTab ) return FALSE;

    if (read_data( handle, nehd.ne_rsrctab + nehdoffset, resTab, resTabSize ) != resTabSize)
    {
        HeapFree( GetProcessHeap(), 0, resTab );
        return FALSE;
    }

    /* Find resource */
    typeInfo = (NE_TYPEINFO *)(resTab + 2);
    while (typeInfo->type_id)
    {
        if (typeInfo->type_id == typeid) goto found_type;
        typeInfo = (NE_TYPEINFO *)((char *)(typeInfo + 1) +
                                   typeInfo->count * sizeof(NE_NAMEINFO));
    }
    TRACE("No typeid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_type:
    nameInfo = (NE_NAMEINFO *)(typeInfo + 1);

    for (count = typeInfo->count; count > 0; count--, nameInfo++)
        if (nameInfo->id == resid) goto found_name;

    TRACE("No resid entry found\n" );
    HeapFree( GetProcessHeap(), 0, resTab );
    return FALSE;

 found_name:
    /* Return resource data */
    *resLen = nameInfo->length << *(WORD *)resTab;
    *resOff = nameInfo->offset << *(WORD *)resTab;

    HeapFree( GetProcessHeap(), 0, resTab );
    return TRUE;
}

/***********************************************************************
 *           find_pe_resource         [internal]
 */
static BOOL find_pe_resource( HANDLE handle, DWORD *resLen, DWORD *resOff, DWORD flags )
{
    union
    {
        IMAGE_NT_HEADERS32 nt32;
        IMAGE_NT_HEADERS64 nt64;
    } pehd;
    DWORD pehdoffset = *resOff;
    PIMAGE_DATA_DIRECTORY resDataDir;
    PIMAGE_SECTION_HEADER sections;
    LPBYTE resSection;
    DWORD len, section_size, data_size, resDirSize;
    const void *resDir;
    const IMAGE_RESOURCE_DIRECTORY *resPtr;
    const IMAGE_RESOURCE_DATA_ENTRY *resData;
    int i, nSections;
    BOOL ret = FALSE;

    /* Read in PE header */
    len = read_data( handle, pehdoffset, &pehd, sizeof(pehd) );
    if (len < sizeof(pehd.nt32.FileHeader)) return FALSE;
    if (len < sizeof(pehd)) memset( (char *)&pehd + len, 0, sizeof(pehd) - len );

    switch (pehd.nt32.OptionalHeader.Magic)
    {
    case IMAGE_NT_OPTIONAL_HDR32_MAGIC:
        resDataDir = pehd.nt32.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    case IMAGE_NT_OPTIONAL_HDR64_MAGIC:
        resDataDir = pehd.nt64.OptionalHeader.DataDirectory + IMAGE_DIRECTORY_ENTRY_RESOURCE;
        break;
    default:
        return FALSE;
    }

    if ( !resDataDir->Size )
    {
        TRACE("No resources in PE dll\n" );
        return FALSE;
    }

    /* Read in section table */
    nSections = pehd.nt32.FileHeader.NumberOfSections;
    sections = HeapAlloc( GetProcessHeap(), 0,
                          nSections * sizeof(IMAGE_SECTION_HEADER) );
    if ( !sections ) return FALSE;

    len = FIELD_OFFSET( IMAGE_NT_HEADERS32, OptionalHeader ) + pehd.nt32.FileHeader.SizeOfOptionalHeader;
    if (read_data( handle, pehdoffset + len, sections, nSections * sizeof(IMAGE_SECTION_HEADER) ) !=
        nSections * sizeof(IMAGE_SECTION_HEADER))
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    /* Find resource section */
    for ( i = 0; i < nSections; i++ )
        if (    resDataDir->VirtualAddress >= sections[i].VirtualAddress
             && resDataDir->VirtualAddress <  sections[i].VirtualAddress +
                                              sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        TRACE("Couldn't find resource section\n" );
        return FALSE;
    }

    /* Read in resource section */
    data_size = sections[i].SizeOfRawData;
    section_size = max( data_size, sections[i].Misc.VirtualSize );
    resSection = HeapAlloc( GetProcessHeap(), 0, section_size );
    if ( !resSection )
    {
        HeapFree( GetProcessHeap(), 0, sections );
        return FALSE;
    }

    if (read_data( handle, sections[i].PointerToRawData, resSection, data_size ) != data_size) goto done;
    if (data_size < section_size) memset( (char *)resSection + data_size, 0, section_size - data_size );

    /* Find resource */
    resDir = resSection + (resDataDir->VirtualAddress - sections[i].VirtualAddress);
    resDirSize = section_size - (resDataDir->VirtualAddress - sections[i].VirtualAddress);

    resPtr = resDir;
    resPtr = find_entry_by_id( resPtr, VS_FILE_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No typeid entry found\n" );
        goto done;
    }
    resPtr = find_entry_by_id( resPtr, VS_VERSION_INFO, resDir, resDirSize );
    if ( !resPtr )
    {
        TRACE("No resid entry found\n" );
        goto done;
    }
    resPtr = find_entry_language( resPtr, resDir, resDirSize, flags );
    if ( !resPtr )
    {
        TRACE("No default language entry found\n" );
        goto done;
    }

    /* Find resource data section */
    resData = (const IMAGE_RESOURCE_DATA_ENTRY*)resPtr;
    for ( i = 0; i < nSections; i++ )
        if (    resData->OffsetToData >= sections[i].VirtualAddress
             && resData->OffsetToData <  sections[i].VirtualAddress +
                                         sections[i].SizeOfRawData )
            break;

    if ( i == nSections )
    {
        TRACE("Couldn't find resource data section\n" );
        goto done;
    }

    /* Return resource data */
    *resLen = resData->Size;
    *resOff = resData->OffsetToData - sections[i].VirtualAddress + sections[i].PointerToRawData;
    ret = TRUE;

 done:
    HeapFree( GetProcessHeap(), 0, resSection );
    HeapFree( GetProcessHeap(), 0, sections );
    return ret;
}


/***********************************************************************
 *           find_version_resource         [internal]
 */
static DWORD find_version_resource( HANDLE handle, DWORD *reslen, DWORD *offset, DWORD flags )
{
    IMAGE_DOS_HEADER mzh;
    WORD magic;

    if (read_data( handle, 0, &mzh, sizeof(mzh) ) != sizeof(mzh)) return 0;
    if (mzh.e_magic != IMAGE_DOS_SIGNATURE) return 0;

    if (read_data( handle, mzh.e_lfanew, &magic, sizeof(magic) ) != sizeof(magic)) return 0;
    *offset = mzh.e_lfanew;

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        if (!find_ne_resource( handle, reslen, offset )) magic = 0;
        break;
    case IMAGE_NT_SIGNATURE:
        if (!find_pe_resource( handle, reslen, offset, flags )) magic = 0;
        break;
    }
    WARN( "Can't handle %04x files.\n", magic );
    return magic;
}

/******************************************************************************
 *   This function will print via standard TRACE, debug info regarding
 *   the file info structure vffi.
 */
static void print_vffi_debug(const VS_FIXEDFILEINFO *vffi)
{
    BOOL    versioned_printer = FALSE;

    if((vffi->dwFileType == VFT_DLL) || (vffi->dwFileType == VFT_DRV))
    {
        if(vffi->dwFileSubtype == VFT2_DRV_VERSIONED_PRINTER)
            /* this is documented for newer w2k Drivers and up */
            versioned_printer = TRUE;
        else if( (vffi->dwFileSubtype == VFT2_DRV_PRINTER) &&
                 (vffi->dwFileVersionMS != vffi->dwProductVersionMS) &&
                 (vffi->dwFileVersionMS > 0) &&
                 (vffi->dwFileVersionMS <= 3) )
            /* found this on NT 3.51, NT4.0 and old w2k Drivers */
            versioned_printer = TRUE;
    }

    TRACE("structversion=%u.%u, ",
            HIWORD(vffi->dwStrucVersion),LOWORD(vffi->dwStrucVersion));
    if(versioned_printer)
    {
        WORD mode = LOWORD(vffi->dwFileVersionMS);
        WORD ver_rev = HIWORD(vffi->dwFileVersionLS);
        TRACE("fileversion=%lu.%u.%u.%u (%s.major.minor.release), ",
            (vffi->dwFileVersionMS),
            HIBYTE(ver_rev), LOBYTE(ver_rev), LOWORD(vffi->dwFileVersionLS),
            (mode == 3) ? "Usermode" : ((mode <= 2) ? "Kernelmode" : "?") );
    }
    else
    {
        TRACE("fileversion=%u.%u.%u.%u, ",
            HIWORD(vffi->dwFileVersionMS),LOWORD(vffi->dwFileVersionMS),
            HIWORD(vffi->dwFileVersionLS),LOWORD(vffi->dwFileVersionLS));
    }
    TRACE("productversion=%u.%u.%u.%u\n",
          HIWORD(vffi->dwProductVersionMS),LOWORD(vffi->dwProductVersionMS),
          HIWORD(vffi->dwProductVersionLS),LOWORD(vffi->dwProductVersionLS));

    TRACE("flagmask=0x%lx, flags=0x%lx %s%s%s%s%s%s\n",
          vffi->dwFileFlagsMask, vffi->dwFileFlags,
          (vffi->dwFileFlags & VS_FF_DEBUG) ? "DEBUG," : "",
          (vffi->dwFileFlags & VS_FF_PRERELEASE) ? "PRERELEASE," : "",
          (vffi->dwFileFlags & VS_FF_PATCHED) ? "PATCHED," : "",
          (vffi->dwFileFlags & VS_FF_PRIVATEBUILD) ? "PRIVATEBUILD," : "",
          (vffi->dwFileFlags & VS_FF_INFOINFERRED) ? "INFOINFERRED," : "",
          (vffi->dwFileFlags & VS_FF_SPECIALBUILD) ? "SPECIALBUILD," : "");

    TRACE("(");

    TRACE("OS=0x%x.0x%x ", HIWORD(vffi->dwFileOS), LOWORD(vffi->dwFileOS));

    switch (vffi->dwFileOS&0xFFFF0000)
    {
    case VOS_DOS:TRACE("DOS,");break;
    case VOS_OS216:TRACE("OS/2-16,");break;
    case VOS_OS232:TRACE("OS/2-32,");break;
    case VOS_NT:TRACE("NT,");break;
    case VOS_UNKNOWN:
    default:
        TRACE("UNKNOWN(0x%lx),",vffi->dwFileOS&0xFFFF0000);break;
    }

    switch (LOWORD(vffi->dwFileOS))
    {
    case VOS__BASE:TRACE("BASE");break;
    case VOS__WINDOWS16:TRACE("WIN16");break;
    case VOS__WINDOWS32:TRACE("WIN32");break;
    case VOS__PM16:TRACE("PM16");break;
    case VOS__PM32:TRACE("PM32");break;
    default:
        TRACE("UNKNOWN(0x%x)",LOWORD(vffi->dwFileOS));break;
    }

    TRACE(")\n");

    switch (vffi->dwFileType)
    {
    case VFT_APP:TRACE("filetype=APP");break;
    case VFT_DLL:
        TRACE("filetype=DLL");
        if(vffi->dwFileSubtype != 0)
        {
            if(versioned_printer) /* NT3.x/NT4.0 or old w2k Driver  */
                TRACE(",PRINTER");
            TRACE(" (subtype=0x%lx)", vffi->dwFileSubtype);
        }
        break;
    case VFT_DRV:
        TRACE("filetype=DRV,");
        switch(vffi->dwFileSubtype)
        {
        case VFT2_DRV_PRINTER:TRACE("PRINTER");break;
        case VFT2_DRV_KEYBOARD:TRACE("KEYBOARD");break;
        case VFT2_DRV_LANGUAGE:TRACE("LANGUAGE");break;
        case VFT2_DRV_DISPLAY:TRACE("DISPLAY");break;
        case VFT2_DRV_MOUSE:TRACE("MOUSE");break;
        case VFT2_DRV_NETWORK:TRACE("NETWORK");break;
        case VFT2_DRV_SYSTEM:TRACE("SYSTEM");break;
        case VFT2_DRV_INSTALLABLE:TRACE("INSTALLABLE");break;
        case VFT2_DRV_SOUND:TRACE("SOUND");break;
        case VFT2_DRV_COMM:TRACE("COMM");break;
        case VFT2_DRV_INPUTMETHOD:TRACE("INPUTMETHOD");break;
        case VFT2_DRV_VERSIONED_PRINTER:TRACE("VERSIONED_PRINTER");break;
        case VFT2_UNKNOWN:
        default:
            TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_FONT:
        TRACE("filetype=FONT,");
        switch (vffi->dwFileSubtype)
        {
        case VFT2_FONT_RASTER:TRACE("RASTER");break;
        case VFT2_FONT_VECTOR:TRACE("VECTOR");break;
        case VFT2_FONT_TRUETYPE:TRACE("TRUETYPE");break;
        default:TRACE("UNKNOWN(0x%lx)",vffi->dwFileSubtype);break;
        }
        break;
    case VFT_VXD:TRACE("filetype=VXD");break;
    case VFT_STATIC_LIB:TRACE("filetype=STATIC_LIB");break;
    case VFT_UNKNOWN:
    default:
        TRACE("filetype=Unknown(0x%lx)",vffi->dwFileType);break;
    }

    TRACE("\n");
    TRACE("filedate=0x%lx.0x%lx\n",vffi->dwFileDateMS,vffi->dwFileDateLS);
}

/***********************************************************************
 *           GetFileVersionInfoSizeW         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeW( LPCWSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExW( FILE_VER_GET_LOCALISED, filename, handle );
}

/***********************************************************************
 *           GetFileVersionInfoSizeA         (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeA( LPCSTR filename, LPDWORD handle )
{
    return GetFileVersionInfoSizeExA( FILE_VER_GET_LOCALISED, filename, handle );
}

/******************************************************************************
 *           GetFileVersionInfoSizeExW       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExW( DWORD flags, LPCWSTR filename, LPDWORD ret_handle )
{
    DWORD len, offset, magic = 1;
    HMODULE hModule;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_w(filename), ret_handle );

    if (ret_handle) *ret_handle = 0;

    if (!filename)
    {
        SetLastError(ERROR_INVALID_PARAMETER);
        return 0;
    }
    if (!*filename)
    {
        SetLastError(ERROR_BAD_PATHNAME);
        return 0;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            magic = IMAGE_NT_SIGNATURE;
            len = SizeofResource( hModule, hRsrc );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        magic = find_version_resource( handle, &len, &offset, flags );
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is more than the actual needed space:
         *
         * (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4
         *
         * This extra buffer is used for ANSI to Unicode conversions in W-Calls.
         * info->wLength should be the same as len. Currently it isn't but that
         * doesn't seem to be a problem (len is bigger than info->wLength).
         */
        SetLastError(0);
        return (len - sizeof(VS_FIXEDFILEINFO)) * 4;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        SetLastError(0);
        return (len * 2) + 4;

    default:
        if (GetVersion() & 0x80000000) /* Windows 95/98 */
            SetLastError(ERROR_FILE_NOT_FOUND);
        else
            SetLastError(ERROR_RESOURCE_DATA_NOT_FOUND);
        return 0;
    }
}

/******************************************************************************
 *           GetFileVersionInfoSizeExA       (kernelbase.@)
 */
DWORD WINAPI GetFileVersionInfoSizeExA( DWORD flags, LPCSTR filename, LPDWORD handle )
{
    UNICODE_STRING filenameW;
    DWORD retval;

    TRACE("(0x%lx,%s,%p)\n", flags, debugstr_a(filename), handle );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoSizeExW(flags, filenameW.Buffer, handle);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoExW           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExW( DWORD flags, LPCWSTR filename, DWORD ignored, DWORD datasize, LPVOID data )
{
    static const char signature[4] = "FE2X";
    DWORD len, offset, magic = 1;
    HMODULE hModule;
    VS_VERSION_INFO_STRUCT32* vvis = data;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_w(filename), ignored, datasize, data );

    if (!data)
    {
        SetLastError(ERROR_INVALID_DATA);
        return FALSE;
    }
    if (flags & ~FILE_VER_GET_LOCALISED)
        FIXME("flags 0x%lx ignored\n", flags & ~FILE_VER_GET_LOCALISED);

    if ((hModule = LoadLibraryExW( filename, 0, LOAD_LIBRARY_AS_IMAGE_RESOURCE )))
    {
        HRSRC hRsrc = NULL;
        if (!(flags & FILE_VER_GET_LOCALISED))
        {
            LANGID english = MAKELANGID( LANG_ENGLISH, SUBLANG_DEFAULT );
            hRsrc = FindResourceExW( hModule, (LPWSTR)VS_FILE_INFO,
                                     MAKEINTRESOURCEW(VS_VERSION_INFO), english );
        }
        if (!hRsrc)
            hRsrc = FindResourceW( hModule, MAKEINTRESOURCEW(VS_VERSION_INFO),
                                   (LPWSTR)VS_FILE_INFO );
        if (hRsrc)
        {
            HGLOBAL hMem = LoadResource( hModule, hRsrc );
            magic = IMAGE_NT_SIGNATURE;
            len = min( SizeofResource(hModule, hRsrc), datasize );
            memcpy( data, LockResource( hMem ), len );
            FreeResource( hMem );
        }
        FreeLibrary( hModule );
    }
    else
    {
        HANDLE handle = CreateFileW( filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     NULL, OPEN_EXISTING, 0, 0 );
        if (handle == INVALID_HANDLE_VALUE) return 0;
        if ((magic = find_version_resource( handle, &len, &offset, flags )))
            len = read_data( handle, offset, data, min( len, datasize ));
        CloseHandle( handle );
    }

    switch (magic)
    {
    case IMAGE_OS2_SIGNATURE:
        /* We have a 16bit resource. */
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo16_Value( (VS_VERSION_INFO_STRUCT16 *)data ));
        SetLastError(0);
        return TRUE;

    case IMAGE_NT_SIGNATURE:
        /* We have a 32bit resource.
         *
         * XP/W2K/W2K3 uses a buffer which is 2 times the actual needed space + 4 bytes "FE2X"
         * This extra buffer is used for Unicode to ANSI conversions in A-Calls
         */
        len = vvis->wLength + sizeof(signature);
        if (datasize >= len) memcpy( (char*)data + vvis->wLength, signature, sizeof(signature) );
        if (TRACE_ON(ver))
            print_vffi_debug( (VS_FIXEDFILEINFO *)VersionInfo32_Value( vvis ));
        SetLastError(0);
        return TRUE;

    default:
        SetLastError( ERROR_RESOURCE_DATA_NOT_FOUND );
        return FALSE;
    }
}

/***********************************************************************
 *           GetFileVersionInfoExA           (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoExA( DWORD flags, LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    UNICODE_STRING filenameW;
    BOOL retval;

    TRACE("(0x%lx,%s,%ld,size=%ld,data=%p)\n",
          flags, debugstr_a(filename), handle, datasize, data );

    if(filename)
        RtlCreateUnicodeStringFromAsciiz(&filenameW, filename);
    else
        filenameW.Buffer = NULL;

    retval = GetFileVersionInfoExW(flags, filenameW.Buffer, handle, datasize, data);

    RtlFreeUnicodeString(&filenameW);

    return retval;
}

/***********************************************************************
 *           GetFileVersionInfoW             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoW( LPCWSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExW(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           GetFileVersionInfoA             (kernelbase.@)
 */
BOOL WINAPI GetFileVersionInfoA( LPCSTR filename, DWORD handle, DWORD datasize, LPVOID data )
{
    return GetFileVersionInfoExA(FILE_VER_GET_LOCALISED, filename, handle, datasize, data);
}

/***********************************************************************
 *           VersionInfo16_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT16 *VersionInfo16_FindChild( const VS_VERSION_INFO_STRUCT16 *info,
                                                                LPCSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT16 *child = VersionInfo16_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!strnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo16_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo32_FindChild             [internal]
 */
static const VS_VERSION_INFO_STRUCT32 *VersionInfo32_FindChild( const VS_VERSION_INFO_STRUCT32 *info,
                                                                LPCWSTR key, UINT len )
{
    const VS_VERSION_INFO_STRUCT32 *child = VersionInfo32_Children( info );

    while ((char *)child < (char *)info + info->wLength )
    {
        if (!wcsnicmp( child->szKey, key, len ) && !child->szKey[len])
            return child;

        if (!(child->wLength)) return NULL;
        child = VersionInfo32_Next( child );
    }

    return NULL;
}

/***********************************************************************
 *           VersionInfo16_QueryValue              [internal]
 *
 *    Gets a value from a 16-bit NE resource
 */
static BOOL VersionInfo16_QueryValue( const VS_VERSION_INFO_STRUCT16 *info, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, UINT *puLen )
{
    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo16_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    *lplpBuffer = VersionInfo16_Value( info );
    if (puLen)
        *puLen = info->wValueLength;

    return TRUE;
}

/***********************************************************************
 *           VersionInfo32_QueryValue              [internal]
 *
 *    Gets a value from a 32-bit PE resource
 */
static BOOL VersionInfo32_QueryValue( const VS_VERSION_INFO_STRUCT32 *info, LPCWSTR lpSubBlock,
                                      LPVOID *lplpBuffer, UINT *puLen, BOOL *pbText )
{
    PVOID ptr;
    TRACE("lpSubBlock : (%s)\n", debugstr_w(lpSubBlock));

    while ( *lpSubBlock )
    {
        /* Find next path component */
        LPCWSTR lpNextSlash;
        for ( lpNextSlash = lpSubBlock; *lpNextSlash; lpNextSlash++ )
            if ( *lpNextSlash == '\\' )
                break;

        /* Skip empty components */
        if ( lpNextSlash == lpSubBlock )
        {
            lpSubBlock++;
            continue;
        }

        /* We have a non-empty component: search info for key */
        info = VersionInfo32_FindChild( info, lpSubBlock, lpNextSlash-lpSubBlock );
        if ( !info )
        {
            if (puLen) *puLen = 0 ;
            SetLastError( ERROR_RESOURCE_TYPE_NOT_FOUND );
            return FALSE;
        }

        /* Skip path component */
        lpSubBlock = lpNextSlash;
    }

    /* Return value */
    ptr = VersionInfo32_Value(info);
    if ((PBYTE)ptr >= ((PBYTE)info + info->wLength))  /* empty value */
        ptr = (WCHAR*)info->szKey + wcslen(info->szKey);

    *lplpBuffer = ptr;
    if (puLen)
        *puLen = info->wValueLength;
    if (pbText)
        *pbText = info->wType;

    return TRUE;
}

/***********************************************************************
 *           VerQueryValueA              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueA( LPCVOID pBlock, LPCSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    static const char rootA[] = "\\";
    const VS_VERSION_INFO_STRUCT16 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_a(lpSubBlock), lplpBuffer, puLen );

     if (!pBlock)
        return FALSE;

    if (lpSubBlock == NULL || lpSubBlock[0] == '\0')
        lpSubBlock = rootA;

    if ( !VersionInfoIs16( info ) )
    {
        BOOL ret, isText;
        INT len;
        LPWSTR lpSubBlockW;
        UINT value_len;

        len  = MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, NULL, 0);
        lpSubBlockW = HeapAlloc(GetProcessHeap(), 0, len * sizeof(WCHAR));

        if (!lpSubBlockW)
            return FALSE;

        MultiByteToWideChar(CP_ACP, 0, lpSubBlock, -1, lpSubBlockW, len);

        ret = VersionInfo32_QueryValue(pBlock, lpSubBlockW, lplpBuffer, &value_len, &isText);
        if (puLen) *puLen = value_len;

        HeapFree(GetProcessHeap(), 0, lpSubBlockW);

        if (ret && isText)
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPSTR lpBufferA = (LPSTR)pBlock + info->wLength + 4;
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            len = WideCharToMultiByte(CP_ACP, 0, *lplpBuffer, value_len,
                                      lpBufferA + pos, info->wLength - pos, NULL, NULL);
            *lplpBuffer = lpBufferA + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo16_QueryValue(info, lpSubBlock, lplpBuffer, puLen);
}

/***********************************************************************
 *           VerQueryValueW              (kernelbase.@)
 */
BOOL WINAPI VerQueryValueW( LPCVOID pBlock, LPCWSTR lpSubBlock,
                               LPVOID *lplpBuffer, PUINT puLen )
{
    const VS_VERSION_INFO_STRUCT32 *info = pBlock;

    TRACE("(%p,%s,%p,%p)\n",
                pBlock, debugstr_w(lpSubBlock), lplpBuffer, puLen );

    if (!pBlock)
        return FALSE;

    if (!lpSubBlock || !lpSubBlock[0])
        lpSubBlock = L"\\";

    if ( VersionInfoIs16( info ) )
    {
        BOOL ret;
        int len;
        LPSTR lpSubBlockA;

        len = WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, NULL, 0, NULL, NULL);
        lpSubBlockA = HeapAlloc(GetProcessHeap(), 0, len * sizeof(char));

        if (!lpSubBlockA)
            return FALSE;

        WideCharToMultiByte(CP_ACP, 0, lpSubBlock, -1, lpSubBlockA, len, NULL, NULL);

        ret = VersionInfo16_QueryValue(pBlock, lpSubBlockA, lplpBuffer, puLen);

        HeapFree(GetProcessHeap(), 0, lpSubBlockA);

        if (ret && wcscmp( lpSubBlock, L"\\" ) && wcsicmp( lpSubBlock, L"\\VarFileInfo\\Translation" ))
        {
            /* Set lpBuffer so it points to the 'empty' area where we store
             * the converted strings
             */
            LPWSTR lpBufferW = (LPWSTR)((LPSTR)pBlock + info->wLength);
            DWORD pos = (LPCSTR)*lplpBuffer - (LPCSTR)pBlock;
            DWORD max = (info->wLength - sizeof(VS_FIXEDFILEINFO)) * 4 - info->wLength;

            len = MultiByteToWideChar(CP_ACP, 0, *lplpBuffer, -1,
                                      lpBufferW + pos, max/sizeof(WCHAR) - pos );
            *lplpBuffer = lpBufferW + pos;
            if (puLen) *puLen = len;
        }
        return ret;
    }

    return VersionInfo32_QueryValue(info, lpSubBlock, lplpBuffer, puLen, NULL);
}


/******************************************************************************
 *   file_existsA
 */
static BOOL file_existsA( char const * path, char const * file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    char filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        strcpy( filename, path );
        len = strlen(filename);
        if (len && filename[len - 1] != '\\') strcat( filename, "\\" );
        strcat( filename, file );
    }
    else if (!SearchPathA( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileA( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/******************************************************************************
 *   file_existsW
 */
static BOOL file_existsW( const WCHAR *path, const WCHAR *file, BOOL excl )
{
    DWORD sharing = excl ? 0 : FILE_SHARE_READ | FILE_SHARE_WRITE;
    WCHAR filename[MAX_PATH];
    int len;
    HANDLE handle;

    if (path)
    {
        lstrcpyW( filename, path );
        len = lstrlenW(filename);
        if (len && filename[len - 1] != '\\') lstrcatW( filename, L"\\" );
        lstrcatW( filename, file );
    }
    else if (!SearchPathW( NULL, file, NULL, MAX_PATH, filename, NULL )) return FALSE;

    handle = CreateFileW( filename, 0, sharing, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0 );
    if (handle == INVALID_HANDLE_VALUE) return FALSE;
    CloseHandle( handle );
    return TRUE;
}

/*****************************************************************************
 *   VerFindFileA (kernelbase.@)
 *
 *   Determines where to install a file based on whether it locates another
 *   version of the file in the system.  The values VerFindFile returns are
 *   used in a subsequent call to the VerInstallFile function.
 */
DWORD WINAPI VerFindFileA( DWORD flags, LPCSTR filename, LPCSTR win_dir, LPCSTR app_dir,
                           LPSTR cur_dir, PUINT curdir_len, LPSTR dest, PUINT dest_len )
{
    DWORD  retval = 0;
    const char *curDir;
    const char *destDir;
    char winDir[MAX_PATH], systemDir[MAX_PATH];

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_a(filename), debugstr_a(win_dir), debugstr_a(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    GetSystemDirectoryA(systemDir, sizeof(systemDir));
    curDir = "";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = systemDir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsA(app_dir, filename, FALSE))
                curDir = app_dir;

            if(!file_existsA(systemDir, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : "";
        if(filename)
        {
            GetWindowsDirectoryA( winDir, MAX_PATH );
            if(file_existsA(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsA(winDir, filename, FALSE))
                curDir = winDir;
            else if(file_existsA(systemDir, filename, FALSE))
                curDir = systemDir;

            if (app_dir && app_dir[0])
            {
                if(!file_existsA(app_dir, filename, FALSE))
                    retval |= VFF_CURNEDEST;
            }
            else if(file_existsA(NULL, filename, FALSE))
                retval |= VFF_CURNEDEST;
        }
    }

    /* Check to see if the file exists and is in use by another application */
    if (filename && file_existsA(curDir, filename, FALSE))
    {
        if (filename && !file_existsA(curDir, filename, TRUE))
           retval |= VFF_FILEINUSE;
    }

    if (dest_len && dest)
    {
        UINT len = strlen(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = strlen(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynA(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_a(cur_dir), debugstr_a(dest));

    return retval;
}

/*****************************************************************************
 * VerFindFileW (kernelbase.@)
 */
DWORD WINAPI VerFindFileW( DWORD flags, LPCWSTR filename, LPCWSTR win_dir, LPCWSTR app_dir,
                           LPWSTR cur_dir, PUINT curdir_len, LPWSTR dest, PUINT dest_len )
{
    DWORD retval = 0;
    const WCHAR *curDir;
    const WCHAR *destDir;

    TRACE("flags = %lx filename=%s windir=%s appdir=%s curdirlen=%p(%u) destdirlen=%p(%u)\n",
          flags, debugstr_w(filename), debugstr_w(win_dir), debugstr_w(app_dir),
          curdir_len, curdir_len ? *curdir_len : 0, dest_len, dest_len ? *dest_len : 0 );

    /* Figure out where the file should go; shared files default to the
       system directory */

    curDir = L"";

    if(flags & VFFF_ISSHAREDFILE)
    {
        destDir = system_dir;
        /* Were we given a filename?  If so, try to find the file. */
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(app_dir && file_existsW(app_dir, filename, FALSE))
            {
                curDir = app_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }
    else /* not a shared file */
    {
        destDir = app_dir ? app_dir : L"";
        if(filename)
        {
            if(file_existsW(destDir, filename, FALSE)) curDir = destDir;
            else if(file_existsW(windows_dir, filename, FALSE))
            {
                curDir = windows_dir;
                retval |= VFF_CURNEDEST;
            }
            else if (file_existsW(system_dir, filename, FALSE))
            {
                curDir = system_dir;
                retval |= VFF_CURNEDEST;
            }
        }
    }

    if (filename && !file_existsW(curDir, filename, TRUE))
        retval |= VFF_FILEINUSE;

    if (dest_len && dest)
    {
        UINT len = lstrlenW(destDir) + 1;
        if (*dest_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(dest, destDir, *dest_len);
        *dest_len = len;
    }
    if (curdir_len && cur_dir)
    {
        UINT len = lstrlenW(curDir) + 1;
        if (*curdir_len < len) retval |= VFF_BUFFTOOSMALL;
        lstrcpynW(cur_dir, curDir, *curdir_len);
        *curdir_len = len;
    }

    TRACE("ret = %lu (%s%s%s) curdir=%s destdir=%s\n", retval,
          (retval & VFF_CURNEDEST) ? "VFF_CURNEDEST " : "",
          (retval & VFF_FILEINUSE) ? "VFF_FILEINUSE " : "",
          (retval & VFF_BUFFTOOSMALL) ? "VFF_BUFFTOOSMALL " : "",
          debugstr_w(cur_dir), debugstr_w(dest));
    return retval;
}


/***********************************************************************
 *         GetProductInfo   (kernelbase.@)
 */
BOOL WINAPI DECLSPEC_HOTPATCH GetProductInfo( DWORD os_major, DWORD os_minor,
                                              DWORD sp_major, DWORD sp_minor, DWORD *type )
{
    return RtlGetProductInfo( os_major, os_minor, sp_major, sp_minor, type );
}


/***********************************************************************
 *         GetVersion   (kernelbase.@)
 */
DWORD WINAPI GetVersion(void)
{
    OSVERSIONINFOEXW info;
    DWORD result;

    info.dwOSVersionInfoSize = sizeof(info);
    if (!GetVersionExW( (OSVERSIONINFOW *)&info )) return 0;

    result = MAKELONG( MAKEWORD( info.dwMajorVersion, info.dwMinorVersion ),
                       (info.dwPlatformId ^ 2) << 14 );

    if (info.dwPlatformId == VER_PLATFORM_WIN32_NT)
        result |= LOWORD(info.dwBuildNumber) << 16;
    return result;
}


/***********************************************************************
 *         GetVersionExA   (kernelbase.@)
 */
BOOL WINAPI GetVersionExA( OSVERSIONINFOA *info )
{
    OSVERSIONINFOEXW infoW;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOA) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXA))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return FALSE;
    }

    infoW.dwOSVersionInfoSize = sizeof(infoW);
    if (!GetVersionExW( (OSVERSIONINFOW *)&infoW )) return FALSE;

    info->dwMajorVersion = infoW.dwMajorVersion;
    info->dwMinorVersion = infoW.dwMinorVersion;
    info->dwBuildNumber  = infoW.dwBuildNumber;
    info->dwPlatformId   = infoW.dwPlatformId;
    WideCharToMultiByte( CP_ACP, 0, infoW.szCSDVersion, -1,
                         info->szCSDVersion, sizeof(info->szCSDVersion), NULL, NULL );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXA))
    {
        OSVERSIONINFOEXA *vex = (OSVERSIONINFOEXA *)info;
        vex->wServicePackMajor = infoW.wServicePackMajor;
        vex->wServicePackMinor = infoW.wServicePackMinor;
        vex->wSuiteMask        = infoW.wSuiteMask;
        vex->wProductType      = infoW.wProductType;
    }
    return TRUE;
}


/***********************************************************************
 *         GetVersionExW   (kernelbase.@)
 */
BOOL WINAPI GetVersionExW( OSVERSIONINFOW *info )
{
    static INIT_ONCE init_once = INIT_ONCE_STATIC_INIT;

    if (info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOW) &&
        info->dwOSVersionInfoSize != sizeof(OSVERSIONINFOEXW))
    {
        WARN( "wrong OSVERSIONINFO size from app (got: %ld)\n", info->dwOSVersionInfoSize );
        return FALSE;
    }

    if (!InitOnceExecuteOnce(&init_once, init_current_version, NULL, NULL)) return FALSE;

    info->dwMajorVersion = current_version.dwMajorVersion;
    info->dwMinorVersion = current_version.dwMinorVersion;
    info->dwBuildNumber  = current_version.dwBuildNumber;
    info->dwPlatformId   = current_version.dwPlatformId;
    wcscpy( info->szCSDVersion, current_version.szCSDVersion );

    if (info->dwOSVersionInfoSize == sizeof(OSVERSIONINFOEXW))
    {
        OSVERSIONINFOEXW *vex = (OSVERSIONINFOEXW *)info;
        vex->wServicePackMajor = current_version.wServicePackMajor;
        vex->wServicePackMinor = current_version.wServicePackMinor;
        vex->wSuiteMask        = current_version.wSuiteMask;
        vex->wProductType      = current_version.wProductType;
    }
    return TRUE;
}

/* Package identity for hosted UWP apps. The wine-uwp launcher sets these variables:
 *   WINE_UWP_PACKAGE_FULL_NAME    e.g. Microsoft.WindowsCalculator_11.2607.0.0_x64__8wekyb3d8bbwe
 *   WINE_UWP_PACKAGE_FAMILY_NAME  e.g. Microsoft.WindowsCalculator_8wekyb3d8bbwe
 *   WINE_UWP_PACKAGE_PATH         Windows path of the unpacked package
 *   WINE_UWP_AUMID                e.g. Microsoft.WindowsCalculator_8wekyb3d8bbwe!App
 */
static LONG get_package_string( const WCHAR *var, UINT32 *length, WCHAR *buffer, LONG no_package_error )
{
    WCHAR value[1024];
    DWORD len;

    if (!length) return ERROR_INVALID_PARAMETER;
    len = GetEnvironmentVariableW( var, value, ARRAY_SIZE(value) );
    if (!len || len >= ARRAY_SIZE(value)) return no_package_error;
    len++;  /* include terminator */
    if (*length < len || !buffer)
    {
        *length = len;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    *length = len;
    lstrcpyW( buffer, value );
    return ERROR_SUCCESS;
}

static BOOL is_packaged_process(void)
{
    return GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", NULL, 0 ) != 0;
}

/***********************************************************************
 *         GetCurrentApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentApplicationUserModelId( UINT32 *length, WCHAR *id )
{
    TRACE( "(%p %p)\n", length, id );
    return get_package_string( L"WINE_UWP_AUMID", length, id, APPMODEL_ERROR_NO_APPLICATION );
}

/***********************************************************************
 *         GetCurrentPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFamilyName( UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p)\n", length, name );
    return get_package_string( L"WINE_UWP_PACKAGE_FAMILY_NAME", length, name, APPMODEL_ERROR_NO_PACKAGE );
}


/***********************************************************************
 *         GetCurrentPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageFullName( UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p)\n", length, name );
    return get_package_string( L"WINE_UWP_PACKAGE_FULL_NAME", length, name, APPMODEL_ERROR_NO_PACKAGE );
}


/***********************************************************************
 *         GetCurrentPackageId   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackageId( UINT32 *len, BYTE *buffer )
{
    WCHAR full_name[1024];

    TRACE( "(%p %p)\n", len, buffer );
    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", full_name, ARRAY_SIZE(full_name) ))
        return APPMODEL_ERROR_NO_PACKAGE;
    return PackageIdFromFullName( full_name, 0, len, buffer );
}

#define PACKAGE_FILTER_HEAD       0x00000010
#define PACKAGE_FILTER_DIRECT     0x00000020
#define PACKAGE_FILTER_STATIC     0x00080000
#define PACKAGE_PROPERTY_FRAMEWORK 0x00000001

struct package_info
{
    UINT32 reserved;
    UINT32 flags;
    WCHAR *path;
    WCHAR *packageFullName;
    WCHAR *packageFamilyName;
    PACKAGE_ID packageId;
};

/* Copy a string into the tail of the PACKAGE_INFO buffer, or just account for it. */
static WCHAR *pkg_put_string( const WCHAR *str, BYTE *buffer, UINT32 *offset, UINT32 size )
{
    UINT32 len = (lstrlenW( str ) + 1) * sizeof(WCHAR);
    WCHAR *ret = NULL;
    if (buffer && *offset + len <= size)
    {
        ret = (WCHAR *)(buffer + *offset);
        memcpy( ret, str, len );
    }
    *offset += len;
    return ret;
}

/* Read one UTF-8 element or attribute out of a package manifest. The manifest is the only source of
 * package metadata here: there is no deployment database to read it back from. */
static char *read_package_manifest( const WCHAR *package_path )
{
    WCHAR path[MAX_PATH];
    HANDLE file;
    DWORD size, read;
    char *data;

    if (!package_path || !package_path[0]) return NULL;
    if (_snwprintf( path, ARRAY_SIZE(path), L"%s\\AppxManifest.xml", package_path ) < 0) return NULL;
    if ((file = CreateFileW( path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL )) == INVALID_HANDLE_VALUE)
        return NULL;
    size = GetFileSize( file, NULL );
    if (size == INVALID_FILE_SIZE || size >= 0x100000 || !(data = HeapAlloc( GetProcessHeap(), 0, size + 1 )))
    {
        CloseHandle( file );
        return NULL;
    }
    if (!ReadFile( file, data, size, &read, NULL ))
    {
        HeapFree( GetProcessHeap(), 0, data );
        data = NULL;
    }
    else data[read] = 0;
    CloseHandle( file );
    return data;
}

static BOOL copy_utf8_range( const char *begin, const char *end, WCHAR *out, DWORD count )
{
    DWORD i;

    for (i = 0; i < count - 1 && begin + i < end; i++) out[i] = (unsigned char)begin[i];
    out[i] = 0;
    return i > 0;
}

/* The publisher ("CN=...") is the one identity field a package full name does not encode, so it is
 * read from the package manifest. Package.Current fails with E_POINTER without it. */
static BOOL get_package_publisher( const WCHAR *package_path, WCHAR *out, DWORD count )
{
    char *data = read_package_manifest( package_path );
    const char *p, *end;
    BOOL ret = FALSE;

    if (!data) return FALSE;
    if ((p = strstr( data, "<Identity" )) && (end = strchr( p, '>' )) && (p = strstr( p, "Publisher=\"" )) && p < end)
    {
        p += 11;
        if ((end = strchr( p, '"' ))) ret = copy_utf8_range( p, end, out, count );
    }
    HeapFree( GetProcessHeap(), 0, data );
    return ret;
}

/* Decode the five predefined XML entities and numeric character references in place. */
static void decode_xml_entities( WCHAR *str )
{
    static const struct { const WCHAR *name; WCHAR ch; } entities[] =
    {
        { L"&amp;", '&' }, { L"&lt;", '<' }, { L"&gt;", '>' }, { L"&quot;", '"' }, { L"&apos;", '\'' },
    };
    WCHAR *src = str, *dst = str;
    unsigned int i;

    while (*src)
    {
        if (*src == '&')
        {
            for (i = 0; i < ARRAY_SIZE(entities); i++)
            {
                size_t len = wcslen( entities[i].name );
                if (!wcsncmp( src, entities[i].name, len ))
                {
                    *dst++ = entities[i].ch;
                    src += len;
                    break;
                }
            }
            if (i < ARRAY_SIZE(entities)) continue;
            if (src[1] == '#')
            {
                WCHAR *end;
                ULONG ch = (src[2] == 'x' || src[2] == 'X') ? wcstoul( src + 3, &end, 16 ) : wcstoul( src + 2, &end, 10 );
                if (*end == ';' && ch && ch < 0x10000)
                {
                    *dst++ = ch;
                    src = end + 1;
                    continue;
                }
            }
        }
        *dst++ = *src++;
    }
    *dst = 0;
}

/* Text of <element>...</element> in the manifest. */
static BOOL get_package_manifest_element( const WCHAR *package_path, const char *element, WCHAR *out, DWORD count )
{
    char open_tag[64], close_tag[64];
    char *data = read_package_manifest( package_path );
    const char *p, *end;
    BOOL ret = FALSE;

    if (!data) return FALSE;
    snprintf( open_tag, sizeof(open_tag), "<%s>", element );
    snprintf( close_tag, sizeof(close_tag), "</%s>", element );
    if ((p = strstr( data, open_tag )) && (end = strstr( p, close_tag )))
        ret = copy_utf8_range( p + strlen( open_tag ), end, out, count );
    HeapFree( GetProcessHeap(), 0, data );
    /* element text is XML: "Solitaire &amp; Casual Games" is "Solitaire & Casual Games" */
    if (ret) decode_xml_entities( out );
    return ret;
}

/***********************************************************************
 *         GetCurrentPackageInfo2   (kernelbase.@)
 *
 * Package graph for hosted UWP apps: the main package from WINE_UWP_PACKAGE_*, plus framework
 * dependencies listed in WINE_UWP_PACKAGE_DEPENDENCIES as "fullname|family|path;fullname|family|path".
 */
LONG WINAPI GetCurrentPackageInfo2( UINT32 flags, UINT32 path_type, UINT32 *buffer_size, BYTE *buffer, UINT32 *count )
{
    WCHAR full[1024], family[1024], path[1024], deps[4096], publisher[512];
    struct { const WCHAR *full, *family, *path; UINT32 flags; } pkgs[16];
    UINT32 n = 0, i, needed, offset, ids_size, want_head, want_deps;
    struct package_info *infos = (struct package_info *)buffer;
    WCHAR *p;

    TRACE( "(%#x %u %p %p %p)\n", flags, path_type, buffer_size, buffer, count );

    if (!buffer_size || !count) return ERROR_INVALID_PARAMETER;
    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", full, ARRAY_SIZE(full) ))
        return APPMODEL_ERROR_NO_PACKAGE;
    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FAMILY_NAME", family, ARRAY_SIZE(family) )) family[0] = 0;
    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, ARRAY_SIZE(path) )) path[0] = 0;

    want_head = !(flags & (PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT)) || (flags & PACKAGE_FILTER_HEAD);
    want_deps = !(flags & (PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT)) || (flags & PACKAGE_FILTER_DIRECT);

    if (want_head)
    {
        pkgs[n].full = full; pkgs[n].family = family; pkgs[n].path = path;
        pkgs[n].flags = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT | PACKAGE_FILTER_STATIC;
        n++;
    }
    if (want_deps && GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", deps, ARRAY_SIZE(deps) ))
    {
        for (p = deps; *p && n < ARRAY_SIZE(pkgs); )
        {
            WCHAR *end = wcschr( p, ';' ), *f, *pa;
            if (end) *end++ = 0;
            if ((f = wcschr( p, '|' )) && (pa = wcschr( f + 1, '|' )))
            {
                *f++ = 0; *pa++ = 0;
                pkgs[n].full = p; pkgs[n].family = f; pkgs[n].path = pa;
                pkgs[n].flags = PACKAGE_FILTER_DIRECT | PACKAGE_FILTER_STATIC;
                n++;
            }
            p = end ? end : p + lstrlenW( p );
        }
    }

    /* size pass; it must count everything the fill pass below writes, including the publisher
     * string read from each package's manifest. Undercounting it made the last package's
     * ids_size underflow and PackageIdFromFullName write past the caller's buffer. */
    needed = n * sizeof(*infos);
    for (i = 0; i < n; i++)
    {
        UINT32 id_size = 0;
        needed += (lstrlenW( pkgs[i].full ) + 1 + lstrlenW( pkgs[i].family ) + 1 + lstrlenW( pkgs[i].path ) + 1) * sizeof(WCHAR);
        PackageIdFromFullName( pkgs[i].full, 0, &id_size, NULL );
        needed += id_size;
        if (!get_package_publisher( pkgs[i].path, publisher, ARRAY_SIZE(publisher) ))
            lstrcpyW( publisher, L"CN=Wine" );
        needed += (lstrlenW( publisher ) + 1) * sizeof(WCHAR);
    }
    if (!buffer || *buffer_size < needed)
    {
        /* the count is reported on the size query too: MrmCoreR sizes its array from it and treats
         * a zero count as an allocation failure (E_OUTOFMEMORY) */
        *buffer_size = needed;
        *count = n;
        return ERROR_INSUFFICIENT_BUFFER;
    }

    offset = n * sizeof(*infos);
    for (i = 0; i < n; i++)
    {
        memset( &infos[i], 0, sizeof(infos[i]) );
        infos[i].flags = pkgs[i].flags;
        infos[i].path = pkg_put_string( pkgs[i].path, buffer, &offset, *buffer_size );
        infos[i].packageFullName = pkg_put_string( pkgs[i].full, buffer, &offset, *buffer_size );
        infos[i].packageFamilyName = pkg_put_string( pkgs[i].family, buffer, &offset, *buffer_size );
        ids_size = offset < *buffer_size ? *buffer_size - offset : 0;
        if (ids_size && PackageIdFromFullName( pkgs[i].full, 0, &ids_size, buffer + offset ) == ERROR_SUCCESS)
        {
            infos[i].packageId = *(PACKAGE_ID *)(buffer + offset);
            offset += ids_size;
            if (!get_package_publisher( pkgs[i].path, publisher, ARRAY_SIZE(publisher) ))
                lstrcpyW( publisher, L"CN=Wine" );
            infos[i].packageId.publisher = pkg_put_string( publisher, buffer, &offset, *buffer_size );
        }
    }
    *buffer_size = needed;
    *count = n;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetCurrentPackageInfo   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageInfo( const UINT32 flags, UINT32 *buffer_size, BYTE *buffer, UINT32 *count )
{
    return GetCurrentPackageInfo2( flags, 0 /* PackagePathType_Install */, buffer_size, buffer, count );
}

/***********************************************************************
 *         GetCurrentPackagePath   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetCurrentPackagePath( UINT32 *length, WCHAR *path )
{
    TRACE( "(%p %p)\n", length, path );
    return get_package_string( L"WINE_UWP_PACKAGE_PATH", length, path, APPMODEL_ERROR_NO_PACKAGE );
}


/***********************************************************************
 *         GetPackageFullName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFullName( HANDLE process, UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p %p)\n", process, length, name );
    if (process != GetCurrentProcess() && GetProcessId( process ) != GetCurrentProcessId())
        FIXME( "other process %p, answering for the current one\n", process );
    return get_package_string( L"WINE_UWP_PACKAGE_FULL_NAME", length, name, APPMODEL_ERROR_NO_PACKAGE );
}


/***********************************************************************
 *         GetPackageFamilyName   (kernelbase.@)
 */
LONG WINAPI /* DECLSPEC_HOTPATCH */ GetPackageFamilyName( HANDLE process, UINT32 *length, WCHAR *name )
{
    TRACE( "(%p %p %p)\n", process, length, name );
    if (process != GetCurrentProcess() && GetProcessId( process ) != GetCurrentProcessId())
        FIXME( "other process %p, answering for the current one\n", process );
    return get_package_string( L"WINE_UWP_PACKAGE_FAMILY_NAME", length, name, APPMODEL_ERROR_NO_PACKAGE );
}

/***********************************************************************
 *         GetPackagesByPackageFamily   (kernelbase.@)
 */
LONG WINAPI DECLSPEC_HOTPATCH GetPackagesByPackageFamily(const WCHAR *family_name, UINT32 *count,
                                                         WCHAR *full_names, UINT32 *buffer_len, WCHAR *buffer)
{
    FIXME( "(%s %p %p %p %p): stub\n", debugstr_w(family_name), count, full_names, buffer_len, buffer );

    if (!count || !buffer_len)
        return ERROR_INVALID_PARAMETER;

    *count = 0;
    *buffer_len = 0;
    return ERROR_SUCCESS;
}


static const struct
{
    UINT32 code;
    const WCHAR *name;
}
arch_names[] =
{
    {PROCESSOR_ARCHITECTURE_INTEL,         L"x86"},
    {PROCESSOR_ARCHITECTURE_ARM,           L"arm"},
    {PROCESSOR_ARCHITECTURE_AMD64,         L"x64"},
    {PROCESSOR_ARCHITECTURE_NEUTRAL,       L"neutral"},
    {PROCESSOR_ARCHITECTURE_ARM64,         L"arm64"},
    {PROCESSOR_ARCHITECTURE_UNKNOWN,       L"unknown"},
};

/* path of a package in the hosted package graph (main package or WINE_UWP_PACKAGE_DEPENDENCIES) */
static BOOL find_package_path( const WCHAR *name, WCHAR *path, DWORD size )
{
    WCHAR full[1024], deps[4096], *p;

    if (!is_packaged_process()) return FALSE;
    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", full, ARRAY_SIZE(full) ) && !wcsicmp( full, name ))
        return GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", path, size ) != 0;
    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", deps, ARRAY_SIZE(deps) )) return FALSE;
    for (p = deps; *p; )
    {
        WCHAR *end = wcschr( p, ';' ), *f, *pa;
        if (end) *end++ = 0;
        if ((f = wcschr( p, '|' )) && (pa = wcschr( f + 1, '|' )))
        {
            *f = 0;
            if (!wcsicmp( p, name ))
            {
                lstrcpynW( path, pa + 1, size );
                return TRUE;
            }
        }
        p = end ? end : p + lstrlenW( p );
    }
    return FALSE;
}

static LONG return_package_string( const WCHAR *value, UINT32 *len, WCHAR *buffer )
{
    UINT32 needed = lstrlenW( value ) + 1;

    if (!len) return ERROR_INVALID_PARAMETER;
    if (!buffer || *len < needed)
    {
        *len = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    lstrcpyW( buffer, value );
    *len = needed;
    return ERROR_SUCCESS;
}

LONG WINAPI GetPackagePathByFullName(const WCHAR *name, UINT32 *len, WCHAR *path)
{
    WCHAR pkg_path[1024];

    TRACE( "(%s %p %p)\n", debugstr_w(name), len, path );
    if (!len || !name) return ERROR_INVALID_PARAMETER;
    if (find_package_path( name, pkg_path, ARRAY_SIZE(pkg_path) )) return return_package_string( pkg_path, len, path );
    FIXME( "(%s %p %p): unknown package\n", debugstr_w(name), len, path );
    return ERROR_NOT_FOUND;
}

/***********************************************************************
 *         GetStagedPackagePathByFullName   (kernelbase.@)
 */
LONG WINAPI GetStagedPackagePathByFullName(const WCHAR *name, UINT32 *len, WCHAR *path)
{
    return GetPackagePathByFullName( name, len, path );
}

/***********************************************************************
 *         GetPackagePathByFullName2   (kernelbase.@)
 */
LONG WINAPI GetPackagePathByFullName2(const WCHAR *name, UINT32 path_type, UINT32 *len, WCHAR *path)
{
    TRACE( "(%s %u %p %p)\n", debugstr_w(name), path_type, len, path );
    return GetPackagePathByFullName( name, len, path );
}

/***********************************************************************
 *         GetStagedPackagePathByFullName2   (kernelbase.@)
 */
LONG WINAPI GetStagedPackagePathByFullName2(const WCHAR *name, UINT32 path_type, UINT32 *len, WCHAR *path)
{
    TRACE( "(%s %u %p %p)\n", debugstr_w(name), path_type, len, path );
    return GetPackagePathByFullName( name, len, path );
}

/***********************************************************************
 *         GetCurrentPackagePath2   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackagePath2(UINT32 path_type, UINT32 *len, WCHAR *path)
{
    TRACE( "(%u %p %p)\n", path_type, len, path );
    return GetCurrentPackagePath( len, path );
}

/***********************************************************************
 *         PackageFamilyNameFromFullName   (kernelbase.@)
 *
 * Full name: Name_Version_Architecture_ResourceId_PublisherId -> family Name_PublisherId
 */
LONG WINAPI PackageFamilyNameFromFullName(const WCHAR *full_name, UINT32 *len, WCHAR *family)
{
    WCHAR buffer[1024];
    const WCHAR *first, *last;
    UINT32 name_len;

    TRACE( "(%s %p %p)\n", debugstr_w(full_name), len, family );
    if (!full_name || !len) return ERROR_INVALID_PARAMETER;
    if (!(first = wcschr( full_name, '_' )) || !(last = wcsrchr( full_name, '_' )) || first == last) return ERROR_INVALID_PARAMETER;
    name_len = first - full_name;
    if (name_len + 1 + lstrlenW( last + 1 ) + 1 > ARRAY_SIZE(buffer)) return ERROR_INVALID_PARAMETER;
    memcpy( buffer, full_name, name_len * sizeof(WCHAR) );
    buffer[name_len] = '_';
    lstrcpyW( buffer + name_len + 1, last + 1 );
    return return_package_string( buffer, len, family );
}

/***********************************************************************
 *         PackageFamilyNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFamilyNameFromId(const PACKAGE_ID *id, UINT32 *len, WCHAR *family)
{
    WCHAR buffer[1024];

    TRACE( "(%p %p %p)\n", id, len, family );
    if (!id || !id->name || !id->publisherId || !len) return ERROR_INVALID_PARAMETER;
    swprintf( buffer, ARRAY_SIZE(buffer), L"%s_%s", id->name, id->publisherId );
    return return_package_string( buffer, len, family );
}

/***********************************************************************
 *         PackageFullNameFromId   (kernelbase.@)
 */
LONG WINAPI PackageFullNameFromId(const PACKAGE_ID *id, UINT32 *len, WCHAR *full_name)
{
    WCHAR buffer[1024];
    const WCHAR *arch = L"neutral";
    unsigned int i;

    TRACE( "(%p %p %p)\n", id, len, full_name );
    if (!id || !id->name || !id->publisherId || !len) return ERROR_INVALID_PARAMETER;
    for (i = 0; i < ARRAY_SIZE(arch_names); i++) if (arch_names[i].code == id->processorArchitecture) arch = arch_names[i].name;
    swprintf( buffer, ARRAY_SIZE(buffer), L"%s_%u.%u.%u.%u_%s_%s_%s", id->name, id->version.Major, id->version.Minor,
              id->version.Build, id->version.Revision, arch, id->resourceId ? id->resourceId : L"", id->publisherId );
    return return_package_string( buffer, len, full_name );
}

/***********************************************************************
 *         PackageNameAndPublisherIdFromFamilyName   (kernelbase.@)
 */
LONG WINAPI PackageNameAndPublisherIdFromFamilyName(const WCHAR *family, UINT32 *name_len, WCHAR *name,
                                                    UINT32 *publisher_len, WCHAR *publisher)
{
    const WCHAR *sep;
    UINT32 need_name, need_pub;
    LONG ret = ERROR_SUCCESS;

    TRACE( "(%s %p %p %p %p)\n", debugstr_w(family), name_len, name, publisher_len, publisher );
    if (!family || !name_len || !publisher_len || !(sep = wcsrchr( family, '_' ))) return ERROR_INVALID_PARAMETER;
    need_name = sep - family + 1;
    need_pub = lstrlenW( sep + 1 ) + 1;
    if (!name || !publisher || *name_len < need_name || *publisher_len < need_pub) ret = ERROR_INSUFFICIENT_BUFFER;
    else
    {
        memcpy( name, family, (need_name - 1) * sizeof(WCHAR) );
        name[need_name - 1] = 0;
        lstrcpyW( publisher, sep + 1 );
    }
    *name_len = need_name;
    *publisher_len = need_pub;
    return ret;
}

/***********************************************************************
 *         ParseApplicationUserModelId   (kernelbase.@)
 *
 * AUMID: PackageFamilyName!PackageRelativeApplicationId
 */
LONG WINAPI ParseApplicationUserModelId(const WCHAR *aumid, UINT32 *family_len, WCHAR *family,
                                        UINT32 *praid_len, WCHAR *praid)
{
    const WCHAR *sep;
    UINT32 need_family, need_praid;
    LONG ret = ERROR_SUCCESS;

    TRACE( "(%s %p %p %p %p)\n", debugstr_w(aumid), family_len, family, praid_len, praid );
    if (!aumid || !family_len || !praid_len) return ERROR_INVALID_PARAMETER;
    /* the separator is the only '!' in an AUMID, and neither half may be empty */
    if (!(sep = wcschr( aumid, '!' )) || sep == aumid || !sep[1]) return ERROR_INVALID_PARAMETER;
    need_family = sep - aumid + 1;
    need_praid = lstrlenW( sep + 1 ) + 1;
    if (!family || !praid || *family_len < need_family || *praid_len < need_praid) ret = ERROR_INSUFFICIENT_BUFFER;
    else
    {
        memcpy( family, aumid, (need_family - 1) * sizeof(WCHAR) );
        family[need_family - 1] = 0;
        lstrcpyW( praid, sep + 1 );
    }
    *family_len = need_family;
    *praid_len = need_praid;
    return ret;
}

/***********************************************************************
 *         FormatApplicationUserModelId   (kernelbase.@)
 */
LONG WINAPI FormatApplicationUserModelId(const WCHAR *family, const WCHAR *praid, UINT32 *len, WCHAR *aumid)
{
    WCHAR buffer[1024];

    TRACE( "(%s %s %p %p)\n", debugstr_w(family), debugstr_w(praid), len, aumid );
    if (!family || !praid || !*family || !*praid || !len) return ERROR_INVALID_PARAMETER;
    if (lstrlenW( family ) + 1 + lstrlenW( praid ) + 1 > ARRAY_SIZE(buffer)) return ERROR_INVALID_PARAMETER;
    swprintf( buffer, ARRAY_SIZE(buffer), L"%s!%s", family, praid );
    return return_package_string( buffer, len, aumid );
}

/***********************************************************************
 *         GetPackageFullNameFromToken   (kernelbase.@)
 */
LONG WINAPI GetPackageFullNameFromToken(HANDLE token, UINT32 *len, WCHAR *name)
{
    TRACE( "(%p %p %p)\n", token, len, name );
    return GetCurrentPackageFullName( len, name );
}

/***********************************************************************
 *         GetApplicationUserModelIdFromToken   (kernelbase.@)
 */
LONG WINAPI GetApplicationUserModelIdFromToken(HANDLE token, UINT32 *len, WCHAR *aumid)
{
    TRACE( "(%p %p %p)\n", token, len, aumid );
    return GetCurrentApplicationUserModelId( len, aumid );
}

/***********************************************************************
 *         GetStagedPackageOrigin   (kernelbase.@)
 */
LONG WINAPI GetStagedPackageOrigin(const WCHAR *name, PackageOrigin *origin)
{
    WCHAR path[1024];

    TRACE( "(%s %p)\n", debugstr_w(name), origin );
    if (!name || !origin) return ERROR_INVALID_PARAMETER;
    if (!find_package_path( name, path, ARRAY_SIZE(path) )) return ERROR_NOT_FOUND;
    *origin = PackageOrigin_Store;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetPackageStatusForUser   (kernelbase.@)
 */
LONG WINAPI GetPackageStatusForUser(const WCHAR *name, PSID sid, UINT32 *status)
{
    WCHAR path[1024];

    TRACE( "(%s %p %p)\n", debugstr_w(name), sid, status );
    if (!name || !status) return ERROR_INVALID_PARAMETER;
    if (!find_package_path( name, path, ARRAY_SIZE(path) )) return ERROR_NOT_FOUND;
    *status = 0;   /* PACKAGE_STATUS_OK */
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetEffectivePackageStatusForUser   (kernelbase.@)
 */
LONG WINAPI GetEffectivePackageStatusForUser(PSID sid, const WCHAR *name, UINT32 *status)
{
    /* The user comes first here, unlike GetPackageStatusForUser: Windows.ApplicationModel's
     * Package.Status and AppXDeploymentClient both call it as (NULL, full name, &status). */
    return GetPackageStatusForUser( name, sid, status );
}

/***********************************************************************
 * Package context objects
 *
 * Opaque handles naming one package (or one application in it) in the current package graph.
 * Windows.ApplicationModel uses them to reach a package's resource and globalization data;
 * Package.Current fails outright if the resources context cannot be created.
 */
struct package_context
{
    UINT32 kind;   /* 0 package, 1 application, 2 resources, 3 globalization */
    WCHAR full_name[PACKAGE_FULL_NAME_MAX_LENGTH + 1];
    WCHAR path[MAX_PATH];
};

static LONG create_package_context( UINT32 kind, void **context )
{
    struct package_context *ctx;

    if (!context) return ERROR_INVALID_PARAMETER;
    *context = NULL;
    if (!is_packaged_process()) return APPMODEL_ERROR_NO_PACKAGE;
    if (!(ctx = HeapAlloc( GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*ctx) ))) return ERROR_OUTOFMEMORY;
    ctx->kind = kind;
    GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", ctx->full_name, ARRAY_SIZE(ctx->full_name) );
    GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_PATH", ctx->path, ARRAY_SIZE(ctx->path) );
    *context = ctx;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetCurrentPackageContext   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageContext(UINT32 reserved, void *reserved2, void **context)
{
    TRACE( "%u %p %p\n", reserved, reserved2, context );
    return create_package_context( 0, context );
}

/***********************************************************************
 *         GetCurrentPackageApplicationContext   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageApplicationContext(UINT32 reserved, void *reserved2, void **context)
{
    TRACE( "%u %p %p\n", reserved, reserved2, context );
    return create_package_context( 1, context );
}

/***********************************************************************
 *         GetCurrentPackageResourcesContext   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageResourcesContext(UINT32 reserved, void *reserved2, void **context)
{
    TRACE( "%u %p %p\n", reserved, reserved2, context );
    return create_package_context( 2, context );
}

/***********************************************************************
 *         GetCurrentPackageApplicationResourcesContext   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageApplicationResourcesContext(UINT32 reserved, void *reserved2, void **context)
{
    TRACE( "%u %p %p\n", reserved, reserved2, context );
    return create_package_context( 2, context );
}

/***********************************************************************
 *         GetCurrentPackageGlobalizationContext   (kernelbase.@)
 */
LONG WINAPI GetCurrentPackageGlobalizationContext(UINT32 reserved, void *reserved2, void **context)
{
    TRACE( "%u %p %p\n", reserved, reserved2, context );
    return create_package_context( 3, context );
}

/* Localizable package properties (DisplayName, PublisherDisplayName, Description, Logo), which
 * Windows serializes at deployment time into MRT "reference blobs" and MrmCoreR's
 * GetStringValueForManifestField (ResourceReference::Set / ValidateReferenceBlob) parses back.
 * Handing MRT the bare manifest string failed that validation, and every Package.DisplayName read
 * came back ERROR_INVALID_DATA - which Shapr3D's welcome page binds to. The layout, recovered from
 * MrmCoreR's ReferenceBuilder::BuildReferenceBlob:
 *
 *   WORD type     0x100 inline string, 0x200 resource reference by name, 0x400 resource index
 *   WORD total    8 + payload rounded up to 4
 *   WORD reserved 0 (Set rejects anything else)
 *   WORD length   payload bytes
 *   payload       for 0x100: the UTF-16 string including its terminator
 *
 * A literal manifest value is stored inline, which is what deployment does for any single-valued
 * string (ReferenceBuilder::TryGetOnlyValue). An `ms-resource:` value is left to MRT to resolve by
 * name. The values come straight out of the manifest; unknown ids report an empty value rather
 * than failing, because the caller treats a failure as fatal. */
struct reference_blob_header
{
    WORD type;
    WORD total;
    WORD reserved;
    WORD length;
};

LONG WINAPI GetPackageResourcesProperty(void *context, UINT32 property, UINT32 *size, void *buffer, UINT32 *flags)
{
    /* the ids windows.applicationmodel's PackageServer::get_* pass; 0 is not used */
    static const char * const elements[] = { NULL, "DisplayName", "PublisherDisplayName", "Description", "Logo" };
    struct package_context *ctx = context;
    struct reference_blob_header *header = buffer;
    WCHAR value[512];
    UINT32 length, needed;

    TRACE( "%p %u %p %p %p\n", context, property, size, buffer, flags );

    if (!ctx || !size) return ERROR_INVALID_PARAMETER;
    if (*size && !buffer) return ERROR_INVALID_PARAMETER;

    value[0] = 0;
    if (property >= ARRAY_SIZE(elements) || !elements[property] ||
        !get_package_manifest_element( ctx->path, elements[property], value, ARRAY_SIZE(value) ))
    {
        static int once;
        if (!once++) FIXME( "no manifest value for property %u\n", property );
    }

    /* A 0x200 reference is looked up as a resource URI, so the manifest's shorthand has to be
     * spelt out: "ms-resource:Name" is Name in the package's Resources map, "ms-resource:/x/y"
     * is package-relative, and "ms-resource://..." is already absolute. */
    if (!wcsnicmp( value, L"ms-resource:", 12 ) && wcsncmp( value + 12, L"//", 2 ))
    {
        const WCHAR *name = value + 12, *end = wcschr( ctx->full_name, '_' );
        WCHAR uri[ARRAY_SIZE(value)];
        int len = end ? end - ctx->full_name : lstrlenW( ctx->full_name );

        if (swprintf( uri, ARRAY_SIZE(uri), L"ms-resource://%.*s%s%s", len, ctx->full_name,
                      *name == '/' ? L"" : L"/Resources/", name ) > 0)
            lstrcpyW( value, uri );
    }

    length = (lstrlenW( value ) + 1) * sizeof(WCHAR);
    needed = sizeof(*header) + ((length + 3) & ~3);
    if (*size < needed)
    {
        *size = needed;
        return ERROR_INSUFFICIENT_BUFFER;
    }
    memset( buffer, 0, needed );
    header->type = wcsnicmp( value, L"ms-resource:", 12 ) ? 0x100 : 0x200;
    header->total = needed;
    header->length = length;
    memcpy( header + 1, value, length );
    TRACE( "property %u -> %#x %s\n", property, header->type, debugstr_w(value) );
    *size = needed;
    if (flags) *flags = 0;
    return ERROR_SUCCESS;
}

LONG WINAPI GetPackageGlobalizationProperty(void *context, UINT32 property, UINT32 *size, void *buffer)
{
    static int once;
    if (!once++) FIXME( "%p %u %p %p: stub\n", context, property, size, buffer );
    if (size) *size = 0;
    return ERROR_NOT_FOUND;
}

LONG WINAPI GetSystemMetadataPathForPackage(const WCHAR *name, UINT32 *len, WCHAR *path)
{
    FIXME( "%s %p %p: stub\n", debugstr_w(name), len, path );
    return ERROR_NOT_FOUND;
}

LONG WINAPI OpenGlobalizationUserSettingsKey(DWORD access, void *reserved, HKEY *key)
{
    TRACE( "%#lx %p %p\n", access, reserved, key );

    if (!key) return ERROR_INVALID_PARAMETER;
    *key = NULL;
    return RegOpenKeyExW( HKEY_CURRENT_USER, L"Control Panel\\International", 0, access, key );
}

BOOL WINAPI QuirkIsEnabledForPackage(const WCHAR *name, ULONG quirk)
{
    static int once;
    if (!once++) FIXME( "%s %#lx: stub\n", debugstr_w(name), quirk );
    return FALSE;
}

LONG WINAPI RefreshPackageInfo(DWORD flags)
{
    TRACE( "%#lx\n", flags );
    return ERROR_SUCCESS;
}

LONG WINAPI PsmGetKeyFromProcess(HANDLE process, UINT32 *len, void *key)
{
    FIXME( "%p %p %p: stub\n", process, len, key );
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG WINAPI PsmGetKeyFromToken(HANDLE token, UINT32 *len, void *key)
{
    FIXME( "%p %p %p: stub\n", token, len, key );
    return APPMODEL_ERROR_NO_PACKAGE;
}

LONG WINAPI PsmGetPackageFullNameFromKey(const void *key, UINT32 *len, WCHAR *name)
{
    FIXME( "%p %p %p: stub\n", key, len, name );
    return APPMODEL_ERROR_NO_PACKAGE;
}

/* MUI string cache used by MrmCoreR; without a cache every lookup goes to the resource file */
HRESULT WINAPI _OpenMuiStringCache(void *a, void *b, void *c, void *d)
{
    static int once;
    if (!once++) FIXME( "%p %p %p %p: stub\n", a, b, c, d );
    return E_NOTIMPL;
}

HRESULT WINAPI _GetMUIStringFromCache(void *a, void *b, void *c, void *d, void *e, void *f)
{
    return E_NOTIMPL;
}

HRESULT WINAPI _AddMUIStringToCache(void *a, void *b, void *c, void *d, void *e, void *f)
{
    return E_NOTIMPL;
}


static UINT32 processor_arch_from_string(const WCHAR *str, unsigned int len)
{
    unsigned int i;

    for (i = 0; i < ARRAY_SIZE(arch_names); ++i)
        if (lstrlenW(arch_names[i].name) == len && !wcsnicmp(str, arch_names[i].name, len))
            return arch_names[i].code;
    return ~0u;
}

/***********************************************************************
 *         PackageIdFromFullName   (kernelbase.@)
 */
LONG WINAPI PackageIdFromFullName(const WCHAR *full_name, UINT32 flags, UINT32 *buffer_length, BYTE *buffer)
{
    const WCHAR *name, *version_str, *arch_str, *resource_id, *publisher_id, *s;
    PACKAGE_ID *id = (PACKAGE_ID *)buffer;
    UINT32 size, buffer_size, len;

    TRACE("full_name %s, flags %#x, buffer_length %p, buffer %p.\n",
            debugstr_w(full_name), flags, buffer_length, buffer);

    if (flags)
        FIXME("Flags %#x are not supported.\n", flags);

    if (!full_name || !buffer_length)
        return ERROR_INVALID_PARAMETER;

    if (!buffer && *buffer_length)
        return ERROR_INVALID_PARAMETER;

    name = full_name;
    if (!(version_str = wcschr(name, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++version_str;

    if (!(arch_str = wcschr(version_str, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++arch_str;

    if (!(resource_id = wcschr(arch_str, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++resource_id;

    if (!(publisher_id = wcschr(resource_id, L'_')))
        return ERROR_INVALID_PARAMETER;
    ++publisher_id;

    /* Publisher id length should be 13. */
    size = sizeof(*id) + sizeof(WCHAR) * ((version_str - name) + (publisher_id - resource_id) + 13 + 1);
    buffer_size = *buffer_length;
    *buffer_length = size;
    if (buffer_size < size)
        return ERROR_INSUFFICIENT_BUFFER;

    memset(id, 0, sizeof(*id));
    if ((id->processorArchitecture = processor_arch_from_string(arch_str, resource_id - arch_str - 1)) == ~0u)
    {
        FIXME("Unrecognized arch %s.\n", debugstr_w(arch_str));
        return ERROR_INVALID_PARAMETER;
    }
    buffer += sizeof(*id);

    id->version.Major = wcstol(version_str, NULL, 10);
    if (!(s = wcschr(version_str, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Minor = wcstol(s, NULL, 10);
    if (!(s = wcschr(s, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Build = wcstol(s, NULL, 10);
    if (!(s = wcschr(s, L'.')))
        return ERROR_INVALID_PARAMETER;
    ++s;
    id->version.Revision = wcstol(s, NULL, 10);

    id->name = (WCHAR *)buffer;
    len = version_str - name - 1;
    memcpy(id->name, name, sizeof(*id->name) * len);
    id->name[len] = 0;
    buffer += sizeof(*id->name) * (len + 1);

    id->resourceId = (WCHAR *)buffer;
    len = publisher_id - resource_id - 1;
    memcpy(id->resourceId, resource_id, sizeof(*id->resourceId) * len);
    id->resourceId[len] = 0;
    buffer += sizeof(*id->resourceId) * (len + 1);

    id->publisherId = (WCHAR *)buffer;
    len = lstrlenW(publisher_id);
    if (len != 13)
        return ERROR_INVALID_PARAMETER;
    memcpy(id->publisherId, publisher_id, sizeof(*id->publisherId) * len);
    id->publisherId[len] = 0;

    return ERROR_SUCCESS;
}

/***********************************************************************
 *         AppXGetOSMaxVersionTested   (kernelbase.@)
 *
 * Undocumented. Reports the MaxVersionTested a package declared; hosted packages get the Windows
 * version we report (10.0.26100.0) so version-gated behaviour follows the system.
 */
HRESULT WINAPI AppXGetOSMaxVersionTested( const WCHAR *package_full_name, UINT64 *version )
{
    FIXME( "%s %p: semi-stub\n", debugstr_w(package_full_name), version );
    if (!version) return E_POINTER;
    *version = ((UINT64)10 << 48) | ((UINT64)0 << 32) | ((UINT64)26100 << 16);
    return S_OK;
}

/***********************************************************************
 *         GetCurrentPackageInfo3   (kernelbase.@)
 *
 * Like GetCurrentPackageInfo2 with a PackageInfoType selector; only PackageInfoType_PackageInfo (0)
 * is meaningful here.
 */
LONG WINAPI GetCurrentPackageInfo3( UINT32 flags, UINT32 type, UINT32 *buffer_size, void *buffer, UINT32 *count )
{
    TRACE( "(%#x %u %p %p %p)\n", flags, type, buffer_size, buffer, count );

    if (!buffer_size) return ERROR_INVALID_PARAMETER;

    switch (type)
    {
    case PackageInfoType3_PackageInfoGeneration:
        /* a counter that changes whenever the package graph changes; ours never does. KernelBase
         * clears the count, needs four bytes of buffer, and reports the size it wrote. */
        if (count) *count = 0;
        if (*buffer_size < sizeof(UINT32) || !buffer)
        {
            *buffer_size = sizeof(UINT32);
            return ERROR_INSUFFICIENT_BUFFER;
        }
        *(UINT32 *)buffer = 1;
        *buffer_size = sizeof(UINT32);
        return ERROR_SUCCESS;

    case PackageInfoType3_PackageAliases:
        /* package aliases come from the deployment database, which does not exist here */
        if (count) *count = 0;
        *buffer_size = 0;
        return ERROR_SUCCESS;

    default:
        if (type) FIXME( "PackageInfoType %u treated as PackageInfo\n", type );
        return GetCurrentPackageInfo2( flags, 0, buffer_size, buffer, count );
    }
}

/***********************************************************************
 *         FindPackagesByPackageFamily   (kernelbase.@)
 *
 * Packages of a family in the current package graph. Only the graph is visible here: there is no
 * deployment database listing packages the process does not depend on.
 */
LONG WINAPI FindPackagesByPackageFamily( const WCHAR *family, UINT32 filters, UINT32 *count, WCHAR **full_names,
                                         UINT32 *buffer_len, WCHAR *buffer, UINT32 *properties )
{
    WCHAR full[1024], fam[1024], deps[4096], *p;
    const WCHAR *matches[16];
    UINT32 n = 0, i, needed = 0, offset = 0;

    TRACE( "%s %#x %p %p %p %p %p\n", debugstr_w(family), filters, count, full_names, buffer_len, buffer, properties );

    if (!family || !count || !buffer_len) return ERROR_INVALID_PARAMETER;

    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FAMILY_NAME", fam, ARRAY_SIZE(fam) ) && !wcsicmp( fam, family )
        && GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FULL_NAME", full, ARRAY_SIZE(full) ))
        matches[n++] = full;

    if (GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_DEPENDENCIES", deps, ARRAY_SIZE(deps) ))
    {
        for (p = deps; *p && n < ARRAY_SIZE(matches); )
        {
            WCHAR *end = wcschr( p, ';' ), *f, *pa;
            if (end) *end++ = 0;
            if ((f = wcschr( p, '|' )) && (pa = wcschr( f + 1, '|' )))
            {
                *f++ = 0; *pa = 0;
                if (!wcsicmp( f, family )) matches[n++] = p;
            }
            p = end ? end : p + lstrlenW( p );
        }
    }

    for (i = 0; i < n; i++) needed += lstrlenW( matches[i] ) + 1;
    if (!full_names || !buffer || *count < n || *buffer_len < needed)
    {
        *count = n;
        *buffer_len = needed;
        return n ? ERROR_INSUFFICIENT_BUFFER : ERROR_SUCCESS;
    }

    for (i = 0; i < n; i++)
    {
        full_names[i] = buffer + offset;
        lstrcpyW( buffer + offset, matches[i] );
        offset += lstrlenW( matches[i] ) + 1;
        if (properties) properties[i] = 0;
    }
    *count = n;
    *buffer_len = needed;
    return ERROR_SUCCESS;
}

/***********************************************************************
 *         GetPackageApplicationPropertyString   (kernelbase.@)
 */
LONG WINAPI GetPackageApplicationPropertyString( void *context, UINT32 property, UINT32 *length, WCHAR *buffer )
{
    static int once;
    if (!once++) FIXME( "%p %u %p %p: stub\n", context, property, length, buffer );
    if (length) *length = 0;
    return ERROR_NOT_FOUND;
}

/***********************************************************************
 *         GetPackageOSMaxVersionTested   (kernelbase.@)
 */
HRESULT WINAPI GetPackageOSMaxVersionTested( const WCHAR *package_full_name, UINT64 *version )
{
    return AppXGetOSMaxVersionTested( package_full_name, version );
}

/***********************************************************************
 *         TerminateProcessOnMemoryExhaustion   (kernelbase.@)
 */
void WINAPI TerminateProcessOnMemoryExhaustion( SIZE_T failed_allocation_size )
{
    FIXME( "%Iu: stub\n", failed_allocation_size );
}

/***********************************************************************
 *         PsmQueryBackgroundActivationType   (kernelbase.@)
 *
 * Hosted for ext-ms-win-com-psmregister-l1-1-0. Native twinapi.appcore
 * delay-loads it from InitializeSuspendCallbacks. *type < 2 means the
 * process is not in a background activation; CoreApplication then skips
 * the suspend-callback registration that needs a full PSM service.
 */
HRESULT WINAPI PsmQueryBackgroundActivationType( HANDLE app, DWORD *type )
{
    TRACE( "%p %p\n", app, type );
    /* *type >= 2 skips RegisterWithPSM (no PSM service here). Activate still
     * WaitOnAddress's gPsmRegistered, which wineuwp sets before it activates
     * (mark_psm_registered, found through the symbol table). */
    if (type) *type = 2;
    return S_OK;
}

/***********************************************************************
 * Application state (api-ms-win-appmodel-state-l1-2-0)
 *
 * Signatures recovered from KernelBase.dll 10.0.26200 and its Windows.UI.Xaml caller:
 *   HANDLE OpenState(void)                                 NULL + last error on failure
 *   HANDLE OpenStateExplicit(PCWSTR family, HANDLE token)
 *   BOOL   CloseState(HANDLE)
 *   BOOL   GetStateFolder(HANDLE, DWORD type, WCHAR *buf, DWORD *len)   len in/out, chars
 * State lives under %LOCALAPPDATA%\Packages\<family>\<folder>, matching Windows.
 */
struct app_state
{
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];
};

/* The type is 1-based: KernelBase's table has exactly these four entries, and anything else fails with
 * ERROR_INTERNAL_ERROR. Windows.UI.Xaml resolves ms-appdata:///local, /roaming and /temp as 1, 2, 3. */
static const WCHAR *state_folder_name( DWORD type )
{
    static const WCHAR *names[] = { L"LocalState", L"RoamingState", L"TempState", L"LocalCache" };
    return type - 1 < ARRAY_SIZE(names) ? names[type - 1] : NULL;
}

static HANDLE open_state_for_family( const WCHAR *family )
{
    struct app_state *state;

    if (!family || !family[0])
    {
        SetLastError( APPMODEL_ERROR_NO_PACKAGE );
        return NULL;
    }
    if (!(state = HeapAlloc( GetProcessHeap(), 0, sizeof(*state) )))
    {
        SetLastError( ERROR_OUTOFMEMORY );
        return NULL;
    }
    lstrcpynW( state->family, family, ARRAY_SIZE(state->family) );
    return state;
}

/***********************************************************************
 *         OpenState   (kernelbase.@)
 */
HANDLE WINAPI OpenState( void )
{
    WCHAR family[PACKAGE_FAMILY_NAME_MAX_LENGTH + 1];

    if (!GetEnvironmentVariableW( L"WINE_UWP_PACKAGE_FAMILY_NAME", family, ARRAY_SIZE(family) )) family[0] = 0;
    TRACE( "-> %s\n", debugstr_w(family) );
    return open_state_for_family( family );
}

/***********************************************************************
 *         OpenStateExplicit   (kernelbase.@)
 */
HANDLE WINAPI OpenStateExplicit( const WCHAR *family, HANDLE token )
{
    TRACE( "%s %p\n", debugstr_w(family), token );
    return open_state_for_family( family );
}

/***********************************************************************
 *         CloseState   (kernelbase.@)
 */
BOOL WINAPI CloseState( HANDLE handle )
{
    TRACE( "%p\n", handle );
    if (!handle) { SetLastError( ERROR_INVALID_HANDLE ); return FALSE; }
    HeapFree( GetProcessHeap(), 0, handle );
    return TRUE;
}

static BOOL get_state_root( WCHAR *path, DWORD size )
{
    DWORD len = GetEnvironmentVariableW( L"LOCALAPPDATA", path, size );
    if (!len || len >= size)
    {
        len = GetEnvironmentVariableW( L"USERPROFILE", path, size );
        if (!len || len + 20 >= size) return FALSE;
        lstrcatW( path, L"\\AppData\\Local" );
    }
    if (lstrlenW( path ) + 10 >= size) return FALSE;
    lstrcatW( path, L"\\Packages" );
    return TRUE;
}

/* create every missing component of a directory path */
static void create_path( const WCHAR *path )
{
    WCHAR tmp[MAX_PATH];
    WCHAR *p;

    lstrcpynW( tmp, path, ARRAY_SIZE(tmp) );
    for (p = tmp + 3; *p; p++)
    {
        if (*p != '\\') continue;
        *p = 0;
        CreateDirectoryW( tmp, NULL );
        *p = '\\';
    }
    CreateDirectoryW( tmp, NULL );
}

static BOOL return_state_path( const WCHAR *path, WCHAR *buffer, DWORD *length )
{
    DWORD needed = lstrlenW( path ) + 1;

    if (!length) { SetLastError( ERROR_INVALID_PARAMETER ); return FALSE; }
    if (!buffer || *length < needed)
    {
        *length = needed;
        SetLastError( ERROR_INSUFFICIENT_BUFFER );
        return FALSE;
    }
    lstrcpyW( buffer, path );
    *length = needed;
    return TRUE;
}

/***********************************************************************
 *         GetStateRootFolder   (kernelbase.@)
 */
BOOL WINAPI GetStateRootFolder( HANDLE handle, WCHAR *buffer, DWORD *length )
{
    struct app_state *state = handle;
    WCHAR path[MAX_PATH];

    TRACE( "%p %p %p\n", handle, buffer, length );
    if (!state) { SetLastError( ERROR_INVALID_HANDLE ); return FALSE; }
    if (!get_state_root( path, ARRAY_SIZE(path) )) { SetLastError( ERROR_PATH_NOT_FOUND ); return FALSE; }
    if (lstrlenW( path ) + lstrlenW( state->family ) + 2 >= ARRAY_SIZE(path)) { SetLastError( ERROR_BUFFER_OVERFLOW ); return FALSE; }
    lstrcatW( path, L"\\" );
    lstrcatW( path, state->family );
    create_path( path );
    return return_state_path( path, buffer, length );
}

/***********************************************************************
 *         GetStateFolder   (kernelbase.@)
 */
BOOL WINAPI GetStateFolder( HANDLE handle, DWORD type, WCHAR *buffer, DWORD *length )
{
    struct app_state *state = handle;
    const WCHAR *name = state_folder_name( type );
    WCHAR path[MAX_PATH];

    TRACE( "%p %lu %p %p\n", handle, type, buffer, length );
    if (!state) { SetLastError( ERROR_INVALID_HANDLE ); return FALSE; }
    if (!name)
    {
        FIXME( "unknown state folder type %lu\n", type );
        SetLastError( ERROR_INTERNAL_ERROR );
        return FALSE;
    }
    if (!get_state_root( path, ARRAY_SIZE(path) )) { SetLastError( ERROR_PATH_NOT_FOUND ); return FALSE; }
    if (lstrlenW( path ) + lstrlenW( state->family ) + lstrlenW( name ) + 3 >= ARRAY_SIZE(path))
    {
        SetLastError( ERROR_BUFFER_OVERFLOW );
        return FALSE;
    }
    lstrcatW( path, L"\\" );
    lstrcatW( path, state->family );
    lstrcatW( path, L"\\" );
    lstrcatW( path, name );
    create_path( path );
    return return_state_path( path, buffer, length );
}

/***********************************************************************
 *         GetSystemAppDataKey   (kernelbase.@)
 */
BOOL WINAPI GetSystemAppDataKey( HANDLE handle, DWORD flags, HKEY *key )
{
    FIXME( "%p %#lx %p: stub\n", handle, flags, key );
    if (key) *key = NULL;
    SetLastError( ERROR_NOT_SUPPORTED );
    return FALSE;
}
