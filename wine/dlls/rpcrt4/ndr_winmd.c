/*
 * Metadata-based marshaling: proxies and stubs for Windows Runtime interfaces built from .winmd
 *
 * Windows marshals a WinRT interface that has no registered proxy/stub DLL with the
 * MetadataPSFactory, CLSID {00000355-0000-0000-C000-000000000046}: it reads the interface out of
 * the package's and the system's metadata and generates NDR format strings for it. Packaged apps
 * with an out-of-process COM server (the Xbox app's XboxPcAppFT.exe) depend on it for every one of
 * their own interfaces, and for every generic instance of Windows.Foundation interfaces over their
 * types (IAsyncOperation<T>, IVector<T>, their delegates and so on).
 *
 * This is that factory. The format strings follow what widl generates for the equivalent IDL, and
 * the proxy and stub objects are the ones ndr_typelib.c builds for typelib marshaling.
 *
 * Copyright 2026 wine-uwp contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#define COBJMACROS
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "objbase.h"
#include "rpcproxy.h"
#include "ndrtypes.h"
#include "corhdr.h"

#include "cpsf.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(winmdps);

static const GUID IID_IInspectable_ = {0xaf86e2e0, 0xb12d, 0x4c6a, {0x9c,0x5a,0xd7,0xaa,0x65,0x10,0x1e,0x90}};

/* ------------------------------------------------------------------------------------------- */
/* ECMA-335 metadata reader                                                                    */

enum { T_U1 = -6, T_U2 = -1, T_U4 = -2, T_STR = -3, T_GUID = -4, T_BLOB = -5, T_CODED = 100 };

enum coded
{
    CI_TypeDefOrRef, CI_HasConstant, CI_HasCustomAttribute, CI_HasFieldMarshal, CI_HasDeclSecurity,
    CI_MemberRefParent, CI_HasSemantics, CI_MethodDefOrRef, CI_MemberForwarded, CI_Implementation,
    CI_CustomAttributeType, CI_ResolutionScope, CI_TypeOrMethodDef, CI_COUNT
};

static const struct { int bits; int count; BYTE tags[22]; } coded_index[CI_COUNT] =
{
    {2, 3, {0x02, 0x01, 0x1b}},
    {2, 3, {0x04, 0x08, 0x17}},
    {5, 22, {0x06, 0x04, 0x01, 0x02, 0x08, 0x09, 0x0a, 0x00, 0x0e, 0x17, 0x14, 0x11, 0x1a, 0x1b,
             0x20, 0x23, 0x26, 0x27, 0x28, 0x2a, 0x2c, 0x2b}},
    {1, 2, {0x04, 0x08}},
    {2, 3, {0x02, 0x06, 0x20}},
    {3, 5, {0x02, 0x01, 0x1a, 0x06, 0x1b}},
    {1, 2, {0x14, 0x17}},
    {1, 2, {0x06, 0x0a}},
    {1, 2, {0x04, 0x06}},
    {2, 3, {0x26, 0x23, 0x27}},
    {3, 5, {0xff, 0xff, 0x06, 0x0a, 0xff}},
    {2, 4, {0x00, 0x1a, 0x23, 0x01}},
    {1, 2, {0x02, 0x06}},
};

#define C(x) (T_CODED + CI_##x)
static const short schema[0x2d][10] =
{
    [0x00] = {T_U2, T_STR, T_GUID, T_GUID, T_GUID, 0x7f},
    [0x01] = {C(ResolutionScope), T_STR, T_STR, 0x7f},
    [0x02] = {T_U4, T_STR, T_STR, C(TypeDefOrRef), 0x04, 0x06, 0x7f},
    [0x03] = {0x04, 0x7f},
    [0x04] = {T_U2, T_STR, T_BLOB, 0x7f},
    [0x05] = {0x06, 0x7f},
    [0x06] = {T_U4, T_U2, T_U2, T_STR, T_BLOB, 0x08, 0x7f},
    [0x07] = {0x08, 0x7f},
    [0x08] = {T_U2, T_U2, T_STR, 0x7f},
    [0x09] = {0x02, C(TypeDefOrRef), 0x7f},
    [0x0a] = {C(MemberRefParent), T_STR, T_BLOB, 0x7f},
    [0x0b] = {T_U1, T_U1, C(HasConstant), T_BLOB, 0x7f},
    [0x0c] = {C(HasCustomAttribute), C(CustomAttributeType), T_BLOB, 0x7f},
    [0x0d] = {C(HasFieldMarshal), T_BLOB, 0x7f},
    [0x0e] = {T_U2, C(HasDeclSecurity), T_BLOB, 0x7f},
    [0x0f] = {T_U2, T_U4, 0x02, 0x7f},
    [0x10] = {T_U4, 0x04, 0x7f},
    [0x11] = {T_BLOB, 0x7f},
    [0x12] = {0x02, 0x14, 0x7f},
    [0x13] = {0x14, 0x7f},
    [0x14] = {T_U2, T_STR, C(TypeDefOrRef), 0x7f},
    [0x15] = {0x02, 0x17, 0x7f},
    [0x16] = {0x17, 0x7f},
    [0x17] = {T_U2, T_STR, T_BLOB, 0x7f},
    [0x18] = {T_U2, 0x06, C(HasSemantics), 0x7f},
    [0x19] = {0x02, C(MethodDefOrRef), C(MethodDefOrRef), 0x7f},
    [0x1a] = {T_STR, 0x7f},
    [0x1b] = {T_BLOB, 0x7f},
    [0x1c] = {T_U2, C(MemberForwarded), T_STR, 0x1a, 0x7f},
    [0x1d] = {T_U4, 0x04, 0x7f},
    [0x1e] = {0x7f}, [0x1f] = {0x7f},
    [0x20] = {T_U4, T_U2, T_U2, T_U2, T_U2, T_U4, T_BLOB, T_STR, T_STR, 0x7f},
    [0x21] = {T_U4, 0x7f},
    [0x22] = {T_U4, T_U4, T_U4, 0x7f},
    [0x23] = {T_U2, T_U2, T_U2, T_U2, T_U4, T_BLOB, T_STR, T_STR, T_BLOB},
    [0x24] = {T_U4, 0x23, 0x7f},
    [0x25] = {T_U4, T_U4, T_U4, 0x23, 0x7f},
    [0x26] = {T_U4, T_STR, T_BLOB, 0x7f},
    [0x27] = {T_U4, T_U4, T_STR, T_STR, C(Implementation), 0x7f},
    [0x28] = {T_U4, T_U4, T_STR, C(Implementation), 0x7f},
    [0x29] = {0x02, 0x02, 0x7f},
    [0x2a] = {T_U2, T_U2, C(TypeOrMethodDef), T_STR, 0x7f},
    [0x2b] = {C(MethodDefOrRef), T_BLOB, 0x7f},
    [0x2c] = {0x2a, C(TypeDefOrRef), 0x7f},
};
#undef C

enum { TBL_TypeRef = 0x01, TBL_TypeDef = 0x02, TBL_Field = 0x04, TBL_MethodDef = 0x06, TBL_Param = 0x08,
       TBL_InterfaceImpl = 0x09, TBL_MemberRef = 0x0a, TBL_CustomAttribute = 0x0c, TBL_TypeSpec = 0x1b,
       TBL_GenericParam = 0x2a };

struct winmd
{
    const BYTE *base;
    const BYTE *strings, *blobs, *guids;
    UINT32 rows[0x2d];
    const BYTE *table[0x2d];
    UINT32 rowsize[0x2d];
    BYTE colsize[0x2d][10], coloff[0x2d][10];
    BOOL in_package;
    BYTE *guid_ctor_ref, *guid_ctor_def, *default_ctor_ref, *default_ctor_def;
};

static UINT32 rd(const BYTE *p, int size)
{
    if (size == 1) return p[0];
    if (size == 2) return p[0] | (p[1] << 8);
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((UINT32)p[3] << 24);
}

static UINT32 col(const struct winmd *md, int table, UINT32 row, int c)
{
    if (!row || row > md->rows[table]) return 0;
    return rd(md->table[table] + (row - 1) * md->rowsize[table] + md->coloff[table][c], md->colsize[table][c]);
}

static const char *str(const struct winmd *md, UINT32 idx) { return (const char *)md->strings + idx; }

static UINT32 read_compressed(const BYTE **p)
{
    const BYTE *b = *p;
    if (!(b[0] & 0x80)) { *p += 1; return b[0]; }
    if (!(b[0] & 0x40)) { *p += 2; return ((b[0] & 0x3f) << 8) | b[1]; }
    *p += 4;
    return ((b[0] & 0x1f) << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

static const BYTE *blob(const struct winmd *md, UINT32 idx, UINT32 *len)
{
    const BYTE *p = md->blobs + idx;
    UINT32 l = read_compressed(&p);
    if (len) *len = l;
    return p;
}

static const BYTE *rva_ptr(const BYTE *base, UINT32 rva)
{
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + ((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    unsigned int i;

    for (i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        UINT32 size = max(sec[i].Misc.VirtualSize, sec[i].SizeOfRawData);
        if (rva >= sec[i].VirtualAddress && rva < sec[i].VirtualAddress + size)
            return base + sec[i].PointerToRawData + (rva - sec[i].VirtualAddress);
    }
    return NULL;
}

static BOOL parse_winmd(struct winmd *md)
{
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)md->base;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(md->base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY *dir;
    const BYTE *cli, *root, *p, *tables = NULL;
    UINT32 vlen, heapsizes, i, n;
    UINT64 valid;
    int strsz, guidsz, blobsz;

    if (dos->e_magic != IMAGE_DOS_SIGNATURE || nt->Signature != IMAGE_NT_SIGNATURE) return FALSE;
    if (nt->OptionalHeader.Magic == IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        dir = &((const IMAGE_NT_HEADERS64 *)nt)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
    else
        dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR];
    if (!(cli = rva_ptr(md->base, dir->VirtualAddress))) return FALSE;
    if (!(root = rva_ptr(md->base, rd(cli + 8, 4)))) return FALSE;
    if (memcmp(root, "BSJB", 4)) return FALSE;
    vlen = rd(root + 12, 4);
    p = root + 16 + vlen;
    n = rd(p + 2, 2);
    p += 4;
    for (i = 0; i < n; i++)
    {
        UINT32 off = rd(p, 4);
        const char *name = (const char *)p + 8;
        if (!strcmp(name, "#~") || !strcmp(name, "#-")) tables = root + off;
        else if (!strcmp(name, "#Strings")) md->strings = root + off;
        else if (!strcmp(name, "#Blob")) md->blobs = root + off;
        else if (!strcmp(name, "#GUID")) md->guids = root + off;
        p += 8 + ((strlen(name) + 4) & ~3);
    }
    if (!tables || !md->strings || !md->blobs) return FALSE;

    heapsizes = tables[6];
    strsz = heapsizes & 1 ? 4 : 2;
    guidsz = heapsizes & 2 ? 4 : 2;
    blobsz = heapsizes & 4 ? 4 : 2;
    valid = rd(tables + 8, 4) | ((UINT64)rd(tables + 12, 4) << 32);
    p = tables + 24;
    for (i = 0; i < 64; i++)
    {
        if (!(valid & ((UINT64)1 << i))) continue;
        if (i >= 0x2d) return FALSE;
        md->rows[i] = rd(p, 4);
        p += 4;
    }
    for (i = 0; i < 0x2d; i++)
    {
        int c, off = 0;
        for (c = 0; c < 10 && schema[i][c] != 0x7f; c++)
        {
            short t = schema[i][c];
            int size;

            if (t == T_U1) size = 1;
            else if (t == T_U2) size = 2;
            else if (t == T_U4) size = 4;
            else if (t == T_STR) size = strsz;
            else if (t == T_GUID) size = guidsz;
            else if (t == T_BLOB) size = blobsz;
            else if (t >= T_CODED)
            {
                int k = t - T_CODED, j;
                UINT32 maxrows = 0;
                for (j = 0; j < coded_index[k].count; j++)
                    if (coded_index[k].tags[j] < 0x2d && md->rows[coded_index[k].tags[j]] > maxrows)
                        maxrows = md->rows[coded_index[k].tags[j]];
                size = maxrows < (1u << (16 - coded_index[k].bits)) ? 2 : 4;
            }
            else size = md->rows[t] < 0x10000 ? 2 : 4;
            md->colsize[i][c] = size;
            md->coloff[i][c] = off;
            off += size;
        }
        md->rowsize[i] = off;
    }
    for (i = 0; i < 0x2d; i++)
    {
        if (!md->rows[i]) continue;
        md->table[i] = p;
        p += md->rows[i] * md->rowsize[i];
    }
    return TRUE;
}

/* the table and row a coded index refers to */
static UINT32 decode(int k, UINT32 v, int *table)
{
    *table = coded_index[k].tags[v & ((1 << coded_index[k].bits) - 1)];
    return v >> coded_index[k].bits;
}

static UINT32 encode(int k, int table, UINT32 row)
{
    int j;
    for (j = 0; j < coded_index[k].count; j++)
        if (coded_index[k].tags[j] == table) return (row << coded_index[k].bits) | j;
    return 0;
}

/* range end of a list column (FieldList, MethodList, ParamList) */
static UINT32 list_end(const struct winmd *md, int table, UINT32 row, int c, int target)
{
    if (row < md->rows[table]) return col(md, table, row + 1, c);
    return md->rows[target] + 1;
}

/* the TypeDef owning a MethodDef */
static UINT32 method_owner(const struct winmd *md, UINT32 method)
{
    UINT32 lo = 1, hi = md->rows[TBL_TypeDef], best = 0;
    while (lo <= hi)
    {
        UINT32 mid = (lo + hi) / 2;
        if (col(md, TBL_TypeDef, mid, 5) <= method) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    return best;
}

static void typedef_name(const struct winmd *md, UINT32 row, char *buf, size_t len)
{
    const char *ns = str(md, col(md, TBL_TypeDef, row, 2)), *name = str(md, col(md, TBL_TypeDef, row, 1));
    if (*ns) snprintf(buf, len, "%s.%s", ns, name);
    else snprintf(buf, len, "%s", name);
}

static void typeref_name(const struct winmd *md, UINT32 row, char *buf, size_t len)
{
    const char *ns = str(md, col(md, TBL_TypeRef, row, 2)), *name = str(md, col(md, TBL_TypeRef, row, 1));
    if (*ns) snprintf(buf, len, "%s.%s", ns, name);
    else snprintf(buf, len, "%s", name);
}

/* name of the type a CustomAttribute's constructor belongs to */
static BOOL attribute_is(const struct winmd *md, UINT32 type, const char *name)
{
    char buf[256];
    int table;
    UINT32 row = decode(CI_CustomAttributeType, type, &table);

    if (table == TBL_MemberRef)
    {
        UINT32 parent = decode(CI_MemberRefParent, col(md, TBL_MemberRef, row, 0), &table);
        if (table == TBL_TypeRef) typeref_name(md, parent, buf, sizeof(buf));
        else if (table == TBL_TypeDef) typedef_name(md, parent, buf, sizeof(buf));
        else return FALSE;
    }
    else if (table == TBL_MethodDef)
        typedef_name(md, method_owner(md, row), buf, sizeof(buf));
    else return FALSE;
    return !strcmp(buf, name);
}

/* first CustomAttribute row with the given parent; the table is sorted by parent */
static UINT32 find_attribute(const struct winmd *md, UINT32 parent, const char *name)
{
    UINT32 lo = 1, hi = md->rows[TBL_CustomAttribute], first = 0;

    while (lo <= hi)
    {
        UINT32 mid = (lo + hi) / 2, v = col(md, TBL_CustomAttribute, mid, 0);
        if (v >= parent) { if (v == parent) first = mid; hi = mid - 1; }
        else lo = mid + 1;
    }
    for (; first && first <= md->rows[TBL_CustomAttribute] && col(md, TBL_CustomAttribute, first, 0) == parent; first++)
        if (attribute_is(md, col(md, TBL_CustomAttribute, first, 1), name)) return first;
    return 0;
}

static BOOL typedef_guid(const struct winmd *md, UINT32 row, GUID *guid)
{
    UINT32 ca = find_attribute(md, encode(CI_HasCustomAttribute, TBL_TypeDef, row),
                               "Windows.Foundation.Metadata.GuidAttribute");
    const BYTE *b;
    UINT32 len;

    if (!ca) return FALSE;
    b = blob(md, col(md, TBL_CustomAttribute, ca, 2), &len);
    if (len < 18) return FALSE;
    guid->Data1 = rd(b + 2, 4);
    guid->Data2 = rd(b + 6, 2);
    guid->Data3 = rd(b + 8, 2);
    memcpy(guid->Data4, b + 10, 8);
    return TRUE;
}

/* ------------------------------------------------------------------------------------------- */
/* Types                                                                                       */

enum kind { K_BASE, K_STRING, K_IFACE, K_STRUCT, K_ENUM, K_GUID, K_ARRAY, K_BAD };

struct wtype
{
    enum kind kind;
    BYTE fc;                 /* K_BASE, K_ENUM */
    UINT32 size, align;      /* memory layout */
    GUID iid;                /* K_IFACE */
    struct winmd *md;        /* K_STRUCT */
    UINT32 row;
    struct wtype *elem;      /* K_ARRAY */
    char *sig;               /* WinRT type signature, for PIIDs */
    struct wtype **fields;   /* K_STRUCT */
    UINT32 nfields;
    BOOL complex;            /* contains HSTRING */
};

struct tdentry
{
    char *name;
    struct winmd *md;
    UINT32 row;
    struct wtype *type;
};

struct ifentry
{
    GUID iid;
    struct winmd *md;
    UINT32 row;              /* the interface or delegate TypeDef, the generic one for an instance */
    UINT32 nargs;
    struct wtype *args[4];
    BOOL delegate;
    BOOL expanded;
};

static CRITICAL_SECTION winmd_cs;
static CRITICAL_SECTION_DEBUG winmd_cs_debug =
{
    0, 0, &winmd_cs,
    { &winmd_cs_debug.ProcessLocksList, &winmd_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": winmd_cs") }
};
static CRITICAL_SECTION winmd_cs = { &winmd_cs_debug, -1, 0, 0, 0, 0 };

static BOOL initialized;
static struct winmd **files;
static UINT32 nfiles;
static struct tdentry *types;      /* open addressing by name */
static UINT32 types_cap, ntypes;
static struct ifentry **ifaces;    /* open addressing by IID */
static UINT32 ifaces_cap, nifaces;
static struct ifentry **pending;   /* instances still to expand */
static UINT32 npending, pending_cap;

static UINT32 hash_str(const char *s)
{
    UINT32 h = 2166136261u;
    while (*s) h = (h ^ (BYTE)*s++) * 16777619u;
    return h;
}

static UINT32 hash_guid(const GUID *g) { return g->Data1 ^ (g->Data2 << 16) ^ g->Data3 ^ *(const UINT32 *)g->Data4; }

static void add_typedef(struct winmd *md, UINT32 row)
{
    char name[512];
    UINT32 h;

    typedef_name(md, row, name, sizeof(name));
    if (ntypes * 2 >= types_cap)
    {
        struct tdentry *old = types;
        UINT32 i, cap = types_cap;
        types_cap = cap ? cap * 2 : 4096;
        types = calloc(types_cap, sizeof(*types));
        ntypes = 0;
        for (i = 0; i < cap; i++)
        {
            if (!old[i].name) continue;
            h = hash_str(old[i].name) & (types_cap - 1);
            while (types[h].name) h = (h + 1) & (types_cap - 1);
            types[h] = old[i];
            ntypes++;
        }
        free(old);
    }
    h = hash_str(name) & (types_cap - 1);
    while (types[h].name)
    {
        if (!strcmp(types[h].name, name)) return;   /* first file wins */
        h = (h + 1) & (types_cap - 1);
    }
    types[h].name = strdup(name);
    types[h].md = md;
    types[h].row = row;
    ntypes++;
}

static struct tdentry *find_typedef(const char *name)
{
    UINT32 h;
    if (!types_cap) return NULL;
    h = hash_str(name) & (types_cap - 1);
    while (types[h].name)
    {
        if (!strcmp(types[h].name, name)) return &types[h];
        h = (h + 1) & (types_cap - 1);
    }
    return NULL;
}

static struct ifentry *find_iface(const GUID *iid)
{
    UINT32 h;
    if (!ifaces_cap) return NULL;
    h = hash_guid(iid) & (ifaces_cap - 1);
    while (ifaces[h])
    {
        if (IsEqualGUID(&ifaces[h]->iid, iid)) return ifaces[h];
        h = (h + 1) & (ifaces_cap - 1);
    }
    return NULL;
}

static struct ifentry *add_iface(const GUID *iid, struct winmd *md, UINT32 row, BOOL delegate,
                                 UINT32 nargs, struct wtype **args)
{
    struct ifentry *e;
    UINT32 h;

    if ((e = find_iface(iid))) return e;
    if (nifaces * 2 >= ifaces_cap)
    {
        struct ifentry **old = ifaces;
        UINT32 i, cap = ifaces_cap;
        ifaces_cap = cap ? cap * 2 : 4096;
        ifaces = calloc(ifaces_cap, sizeof(*ifaces));
        for (i = 0; i < cap; i++)
        {
            if (!old[i]) continue;
            h = hash_guid(&old[i]->iid) & (ifaces_cap - 1);
            while (ifaces[h]) h = (h + 1) & (ifaces_cap - 1);
            ifaces[h] = old[i];
        }
        free(old);
    }
    e = calloc(1, sizeof(*e));
    e->iid = *iid;
    e->md = md;
    e->row = row;
    e->delegate = delegate;
    e->nargs = min(nargs, ARRAY_SIZE(e->args));
    if (nargs) memcpy(e->args, args, e->nargs * sizeof(*args));
    h = hash_guid(iid) & (ifaces_cap - 1);
    while (ifaces[h]) h = (h + 1) & (ifaces_cap - 1);
    ifaces[h] = e;
    nifaces++;
    if (nargs)
    {
        if (npending == pending_cap)
        {
            pending_cap = pending_cap ? pending_cap * 2 : 256;
            pending = realloc(pending, pending_cap * sizeof(*pending));
        }
        pending[npending++] = e;
    }
    return e;
}

typedef struct { ULONG Unknown[6]; ULONG State[5]; ULONG Count[2]; UCHAR Buffer[64]; } SHA_CTX;
void WINAPI A_SHAInit(SHA_CTX *);
void WINAPI A_SHAUpdate(SHA_CTX *, const unsigned char *, UINT);
void WINAPI A_SHAFinal(SHA_CTX *, PULONG);

static void piid_from_signature(const char *sig, GUID *guid)
{
    static const BYTE ns[16] = {0x11,0xf4,0x7a,0xd5,0x7b,0x73,0x42,0xc0,0xab,0xae,0x87,0x8b,0x1e,0x16,0xad,0xee};
    ULONG digest[5];
    BYTE *d = (BYTE *)digest;
    SHA_CTX ctx;

    A_SHAInit(&ctx);
    A_SHAUpdate(&ctx, ns, sizeof(ns));
    A_SHAUpdate(&ctx, (const unsigned char *)sig, strlen(sig));
    A_SHAFinal(&ctx, digest);
    guid->Data1 = (d[0] << 24) | (d[1] << 16) | (d[2] << 8) | d[3];
    guid->Data2 = (d[4] << 8) | d[5];
    guid->Data3 = (((d[6] << 8) | d[7]) & 0x0fff) | 0x5000;
    memcpy(guid->Data4, d + 8, 8);
    guid->Data4[0] = (guid->Data4[0] & 0x3f) | 0x80;
}

static char *guid_sig(const GUID *g)
{
    char *s = malloc(40);
    sprintf(s, "{%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x}", (unsigned long)g->Data1, g->Data2, g->Data3,
            g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3], g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    return s;
}

static struct wtype *new_type(enum kind kind, UINT32 size, const char *sig)
{
    struct wtype *t = calloc(1, sizeof(*t));
    t->kind = kind;
    t->size = t->align = size;
    t->sig = sig ? strdup(sig) : NULL;
    return t;
}

static struct wtype *base_type(BYTE elem)
{
    static struct wtype *cache[0x20];
    static const struct { BYTE fc; BYTE size; const char *sig; } base[0x0f] =
    {
        [ELEMENT_TYPE_BOOLEAN] = {FC_BYTE, 1, "b1"},  [ELEMENT_TYPE_CHAR] = {FC_WCHAR, 2, "c2"},
        [ELEMENT_TYPE_I1] = {FC_SMALL, 1, "i1"},      [ELEMENT_TYPE_U1] = {FC_BYTE, 1, "u1"},
        [ELEMENT_TYPE_I2] = {FC_SHORT, 2, "i2"},      [ELEMENT_TYPE_U2] = {FC_USHORT, 2, "u2"},
        [ELEMENT_TYPE_I4] = {FC_LONG, 4, "i4"},       [ELEMENT_TYPE_U4] = {FC_ULONG, 4, "u4"},
        [ELEMENT_TYPE_I8] = {FC_HYPER, 8, "i8"},      [ELEMENT_TYPE_U8] = {FC_HYPER, 8, "u8"},
        [ELEMENT_TYPE_R4] = {FC_FLOAT, 4, "f4"},      [ELEMENT_TYPE_R8] = {FC_DOUBLE, 8, "f8"},
    };

    if (elem >= ARRAY_SIZE(base) || !base[elem].fc) return NULL;
    if (!cache[elem])
    {
        cache[elem] = new_type(K_BASE, base[elem].size, base[elem].sig);
        cache[elem]->fc = base[elem].fc;
    }
    return cache[elem];
}

static struct wtype *string_type(void)
{
    static struct wtype *t;
    if (!t) { t = new_type(K_STRING, sizeof(void *), "string"); t->complex = TRUE; }
    return t;
}

static struct wtype *iface_type(const GUID *iid, const char *sig)
{
    struct wtype *t = new_type(K_IFACE, sizeof(void *), sig);
    t->iid = *iid;
    t->complex = TRUE;
    return t;
}

static struct wtype *bad_type(void)
{
    static struct wtype *t;
    if (!t) t = new_type(K_BAD, 0, "?");
    return t;
}

static struct wtype *parse_type(struct winmd *md, const BYTE **p, struct wtype **gargs, UINT32 nargs);
static struct wtype *typedef_type(struct tdentry *td);

static struct tdentry *resolve_tdor(struct winmd *md, UINT32 table, UINT32 row)
{
    char name[512];

    if (table == TBL_TypeDef) typedef_name(md, row, name, sizeof(name));
    else if (table == TBL_TypeRef) typeref_name(md, row, name, sizeof(name));
    else return NULL;
    return find_typedef(name);
}

static struct wtype *generic_instance(struct tdentry *def, struct wtype **args, UINT32 nargs)
{
    size_t len;
    char *sig, *gs;
    GUID guid, iid;
    UINT32 i;
    BOOL delegate;
    struct wtype *t;
    int table;
    char base[256];

    if (!def || !typedef_guid(def->md, def->row, &guid)) return bad_type();
    for (i = 0; i < nargs; i++) if (args[i]->kind == K_BAD) return bad_type();
    gs = guid_sig(&guid);
    len = strlen(gs) + 16;
    for (i = 0; i < nargs; i++) len += strlen(args[i]->sig) + 1;
    sig = malloc(len);
    sprintf(sig, "pinterface(%s", gs);
    for (i = 0; i < nargs; i++) { strcat(sig, ";"); strcat(sig, args[i]->sig); }
    strcat(sig, ")");
    free(gs);
    piid_from_signature(sig, &iid);

    decode(CI_TypeDefOrRef, col(def->md, TBL_TypeDef, def->row, 3), &table);
    base[0] = 0;
    {
        UINT32 r = decode(CI_TypeDefOrRef, col(def->md, TBL_TypeDef, def->row, 3), &table);
        if (table == TBL_TypeRef) typeref_name(def->md, r, base, sizeof(base));
    }
    delegate = !strcmp(base, "System.MulticastDelegate");
    add_iface(&iid, def->md, def->row, delegate, nargs, args);
    t = iface_type(&iid, sig);
    free(sig);
    return t;
}

static struct wtype *parse_type(struct winmd *md, const BYTE **p, struct wtype **gargs, UINT32 nargs)
{
    BYTE elem;
    int table;
    UINT32 row;

    for (;;)
    {
        elem = *(*p)++;
        if (elem == ELEMENT_TYPE_CMOD_REQD || elem == ELEMENT_TYPE_CMOD_OPT) { read_compressed(p); continue; }
        break;
    }

    switch (elem)
    {
    case ELEMENT_TYPE_STRING: return string_type();
    case ELEMENT_TYPE_OBJECT: return iface_type(&IID_IInspectable_, "cinterface(IInspectable)");
    case ELEMENT_TYPE_VAR:
        row = read_compressed(p);
        return row < nargs ? gargs[row] : bad_type();
    case ELEMENT_TYPE_SZARRAY:
    {
        struct wtype *t = new_type(K_ARRAY, sizeof(void *), NULL);
        t->elem = parse_type(md, p, gargs, nargs);
        if (t->elem->kind == K_BAD) return bad_type();
        return t;
    }
    case ELEMENT_TYPE_CLASS:
    case ELEMENT_TYPE_VALUETYPE:
    {
        struct tdentry *td;
        char name[512];

        row = decode(CI_TypeDefOrRef, read_compressed(p), &table);
        if (table == TBL_TypeRef)
        {
            typeref_name(md, row, name, sizeof(name));
            if (!strcmp(name, "System.Guid"))
            {
                static struct wtype *g;
                if (!g) { g = new_type(K_GUID, 16, "g16"); g->align = 4; }
                return g;
            }
        }
        if (table == TBL_TypeSpec)
        {
            const BYTE *b = blob(md, col(md, TBL_TypeSpec, row, 0), NULL);
            return parse_type(md, &b, gargs, nargs);
        }
        if (!(td = resolve_tdor(md, table, row))) return bad_type();
        return typedef_type(td);
    }
    case ELEMENT_TYPE_GENERICINST:
    {
        struct wtype *args[8];
        struct tdentry *def;
        UINT32 i, n;

        (*p)++;  /* CLASS or VALUETYPE */
        row = decode(CI_TypeDefOrRef, read_compressed(p), &table);
        def = resolve_tdor(md, table, row);
        n = read_compressed(p);
        for (i = 0; i < n; i++)
        {
            struct wtype *a = parse_type(md, p, gargs, nargs);
            if (i < ARRAY_SIZE(args)) args[i] = a;
        }
        if (n > ARRAY_SIZE(args)) return bad_type();
        return generic_instance(def, args, n);
    }
    default:
    {
        struct wtype *t = base_type(elem);
        return t ? t : bad_type();
    }
    }
}

static struct wtype *typedef_type(struct tdentry *td)
{
    struct winmd *md = td->md;
    UINT32 row = td->row, flags = col(md, TBL_TypeDef, row, 0), r;
    char base[256] = "", sig[1024];
    int table;
    GUID iid;

    if (td->type) return td->type;
    r = decode(CI_TypeDefOrRef, col(md, TBL_TypeDef, row, 3), &table);
    if (table == TBL_TypeRef) typeref_name(md, r, base, sizeof(base));
    else if (table == TBL_TypeDef && r) typedef_name(md, r, base, sizeof(base));

    if (flags & 0x20)  /* interface */
    {
        if (!typedef_guid(md, row, &iid)) return td->type = bad_type();
        add_iface(&iid, md, row, FALSE, 0, NULL);
        return td->type = iface_type(&iid, guid_sig(&iid));
    }
    if (!strcmp(base, "System.MulticastDelegate"))
    {
        char *gs;
        if (!typedef_guid(md, row, &iid)) return td->type = bad_type();
        add_iface(&iid, md, row, TRUE, 0, NULL);
        gs = guid_sig(&iid);
        snprintf(sig, sizeof(sig), "delegate(%s)", gs);
        free(gs);
        return td->type = iface_type(&iid, sig);
    }
    if (!strcmp(base, "System.Enum"))
    {
        UINT32 f = col(md, TBL_TypeDef, row, 4), end = list_end(md, TBL_TypeDef, row, 4, TBL_Field);
        BOOL is_unsigned = FALSE;
        struct wtype *t;
        for (; f < end; f++)
        {
            if (col(md, TBL_Field, f, 0) & 0x10) continue;  /* static */
            is_unsigned = blob(md, col(md, TBL_Field, f, 2), NULL)[1] == ELEMENT_TYPE_U4;
            break;
        }
        snprintf(sig, sizeof(sig), "enum(%s;%s)", td->name, is_unsigned ? "u4" : "i4");
        t = new_type(K_ENUM, 4, sig);
        t->fc = is_unsigned ? FC_ULONG : FC_LONG;
        return td->type = t;
    }
    if (!strcmp(base, "System.ValueType"))
    {
        UINT32 f = col(md, TBL_TypeDef, row, 4), end = list_end(md, TBL_TypeDef, row, 4, TBL_Field);
        struct wtype *t = new_type(K_STRUCT, 0, NULL), *fields[64];
        size_t len = strlen(td->name) + 16;
        UINT32 n = 0, i, off = 0;

        t->align = 1;
        t->md = md;
        t->row = row;
        td->type = t;   /* before recursing */
        for (; f < end && n < ARRAY_SIZE(fields); f++)
        {
            const BYTE *b;
            if (col(md, TBL_Field, f, 0) & 0x10) continue;
            b = blob(md, col(md, TBL_Field, f, 2), NULL) + 1;
            fields[n] = parse_type(md, &b, NULL, 0);
            if (fields[n]->kind == K_BAD || fields[n]->kind == K_ARRAY || fields[n]->kind == K_IFACE)
            {
                /* interface pointers in structs (IReference<T> fields) are not supported yet */
                FIXME("unsupported field in %s\n", td->name);
                return td->type = bad_type();
            }
            if (fields[n]->complex) t->complex = TRUE;
            off = (off + fields[n]->align - 1) & ~(fields[n]->align - 1);
            off += fields[n]->size;
            t->align = max(t->align, fields[n]->align);
            len += strlen(fields[n]->sig) + 1;
            n++;
        }
        t->size = (off + t->align - 1) & ~(t->align - 1);
        t->nfields = n;
        t->fields = malloc(n * sizeof(*fields));
        memcpy(t->fields, fields, n * sizeof(*fields));
        t->sig = malloc(len);
        sprintf(t->sig, "struct(%s", td->name);
        for (i = 0; i < n; i++) { strcat(t->sig, ";"); strcat(t->sig, fields[i]->sig); }
        strcat(t->sig, ")");
        return t;
    }

    /* a runtime class: marshaled as its default interface */
    {
        UINT32 i;
        for (i = 1; i <= md->rows[TBL_InterfaceImpl]; i++)
        {
            const BYTE *b;
            BYTE spec[8];
            struct wtype *def;
            UINT32 tok;

            if (col(md, TBL_InterfaceImpl, i, 0) != row) continue;
            if (!find_attribute(md, encode(CI_HasCustomAttribute, TBL_InterfaceImpl, i),
                                "Windows.Foundation.Metadata.DefaultAttribute"))
                continue;
            tok = col(md, TBL_InterfaceImpl, i, 1);
            r = decode(CI_TypeDefOrRef, tok, &table);
            if (table == TBL_TypeSpec)
            {
                b = blob(md, col(md, TBL_TypeSpec, r, 0), NULL);
                def = parse_type(md, &b, NULL, 0);
            }
            else
            {
                /* build a CLASS signature for the token */
                BYTE *q = spec;
                *q++ = ELEMENT_TYPE_CLASS;
                if (tok < 0x80) *q++ = tok;
                else if (tok < 0x4000) { *q++ = 0x80 | (tok >> 8); *q++ = tok & 0xff; }
                else { *q++ = 0xc0 | (tok >> 24); *q++ = tok >> 16; *q++ = tok >> 8; *q++ = tok; }
                b = spec;
                def = parse_type(md, &b, NULL, 0);
            }
            if (def->kind != K_IFACE) break;
            snprintf(sig, sizeof(sig), "rc(%s;%s)", td->name, def->sig);
            return td->type = iface_type(&def->iid, sig);
        }
    }
    return td->type = bad_type();
}

/* walk a method signature, registering every generic instance it names */
static void scan_signature(struct winmd *md, const BYTE *b, struct wtype **gargs, UINT32 nargs)
{
    UINT32 n, i;
    BYTE cc = *b++;
    if (cc & 0x10) read_compressed(&b);  /* generic method */
    n = read_compressed(&b);
    for (i = 0; i <= n; i++)
    {
        while (*b == ELEMENT_TYPE_BYREF) b++;
        if (*b == ELEMENT_TYPE_VOID) { b++; continue; }
        parse_type(md, &b, gargs, nargs);
    }
}

static void expand_instance(struct ifentry *e)
{
    struct winmd *md = e->md;
    UINT32 m = col(md, TBL_TypeDef, e->row, 5), end = list_end(md, TBL_TypeDef, e->row, 5, TBL_MethodDef), i;

    e->expanded = TRUE;
    for (; m < end; m++)
        scan_signature(md, blob(md, col(md, TBL_MethodDef, m, 4), NULL), e->args, e->nargs);
    for (i = 1; i <= md->rows[TBL_InterfaceImpl]; i++)
    {
        int table;
        UINT32 r;
        if (col(md, TBL_InterfaceImpl, i, 0) != e->row) continue;
        r = decode(CI_TypeDefOrRef, col(md, TBL_InterfaceImpl, i, 1), &table);
        if (table == TBL_TypeSpec)
        {
            const BYTE *b = blob(md, col(md, TBL_TypeSpec, r, 0), NULL);
            parse_type(md, &b, e->args, e->nargs);
        }
    }
}

static void drain_pending(void)
{
    while (npending) expand_instance(pending[--npending]);
}

static void load_file(const WCHAR *path, BOOL in_package)
{
    HANDLE file, mapping;
    struct winmd *md;
    const BYTE *base;

    file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(file);
    if (!mapping) return;
    base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    if (!base) return;

    md = calloc(1, sizeof(*md));
    md->base = base;
    md->in_package = in_package;
    if (!parse_winmd(md))
    {
        WARN("%s is not metadata\n", debugstr_w(path));
        UnmapViewOfFile(base);
        free(md);
        return;
    }
    files = realloc(files, (nfiles + 1) * sizeof(*files));
    files[nfiles++] = md;
    TRACE("loaded %s, %u types\n", debugstr_w(path), md->rows[TBL_TypeDef]);
}

static void load_dir(const WCHAR *dir, BOOL in_package)
{
    WCHAR path[MAX_PATH];
    WIN32_FIND_DATAW data;
    HANDLE find;

    swprintf(path, ARRAY_SIZE(path), L"%s\\*.winmd", dir);
    if ((find = FindFirstFileW(path, &data)) == INVALID_HANDLE_VALUE) return;
    do
    {
        swprintf(path, ARRAY_SIZE(path), L"%s\\%s", dir, data.cFileName);
        load_file(path, in_package);
    } while (FindNextFileW(find, &data));
    FindClose(find);
}

static void initialize(void)
{
    WCHAR dir[MAX_PATH];
    UINT32 f, r;

    initialized = TRUE;
    if (GetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", dir, ARRAY_SIZE(dir)))
        load_dir(dir, TRUE);
    GetSystemDirectoryW(dir, ARRAY_SIZE(dir));
    wcscat(dir, L"\\WinMetadata");
    load_dir(dir, FALSE);

    for (f = 0; f < nfiles; f++)
        for (r = 1; r <= files[f]->rows[TBL_TypeDef]; r++)
            add_typedef(files[f], r);

    /* every non-generic interface and delegate with a GuidAttribute */
    for (f = 0; f < nfiles; f++)
    {
        struct winmd *md = files[f];
        for (r = 1; r <= md->rows[TBL_TypeDef]; r++)
        {
            UINT32 flags = col(md, TBL_TypeDef, r, 0);
            char name[512];
            struct tdentry *td;

            if (strchr(str(md, col(md, TBL_TypeDef, r, 1)), '`')) continue;
            typedef_name(md, r, name, sizeof(name));
            if (!(td = find_typedef(name)) || td->md != md) continue;
            if (flags & 0x20) typedef_type(td);
            else
            {
                char base[256] = "";
                int table;
                UINT32 b = decode(CI_TypeDefOrRef, col(md, TBL_TypeDef, r, 3), &table);
                if (table == TBL_TypeRef) typeref_name(md, b, base, sizeof(base));
                if (!strcmp(base, "System.MulticastDelegate")) typedef_type(td);
            }
        }
    }

    /* generic instances named in the package's own metadata */
    for (f = 0; f < nfiles; f++)
    {
        struct winmd *md = files[f];
        if (!md->in_package) continue;
        for (r = 1; r <= md->rows[TBL_MethodDef]; r++)
            scan_signature(md, blob(md, col(md, TBL_MethodDef, r, 4), NULL), NULL, 0);
        for (r = 1; r <= md->rows[TBL_TypeSpec]; r++)
        {
            const BYTE *b = blob(md, col(md, TBL_TypeSpec, r, 0), NULL);
            if (*b == ELEMENT_TYPE_GENERICINST) parse_type(md, &b, NULL, 0);
        }
    }
    drain_pending();
    TRACE("%u files, %u types, %u interfaces\n", nfiles, ntypes, nifaces);
}

static struct ifentry *lookup(const GUID *iid)
{
    struct ifentry *e;

    EnterCriticalSection(&winmd_cs);
    if (!initialized) initialize();
    e = find_iface(iid);
    LeaveCriticalSection(&winmd_cs);
    return e;
}

/* ------------------------------------------------------------------------------------------- */
/* Format strings                                                                              */

struct buf { BYTE *data; size_t len, cap; };

static void put(struct buf *b, const void *p, size_t n)
{
    if (b->len + n > b->cap)
    {
        b->cap = max(b->cap * 2, b->len + n + 256);
        b->data = realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}
static void put1(struct buf *b, BYTE v) { put(b, &v, 1); }
static void put2(struct buf *b, USHORT v) { put(b, &v, 2); }
static void put4(struct buf *b, UINT32 v) { put(b, &v, 4); }
/* a relative offset to an earlier position, measured from where the offset itself is stored */
static void put_rel(struct buf *b, size_t target) { put2(b, (USHORT)(short)((long)target - (long)b->len)); }

/* widl's prelude: the wire BSTR layout and the HSTRING user marshal at 28 */
static const BYTE prelude[] =
{
    0x00, 0x00,
    0x1b, 0x01, 0x02, 0x00, 0x09, 0x00, 0xfc, 0xff, 0x01, 0x00, 0x06, 0x5b,
    0x17, 0x03, 0x08, 0x00, 0xf0, 0xff, 0x08, 0x08, 0x5c, 0x5b,
    0x12, 0x00, 0xf4, 0xff,
    0xb4, 0x83, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0xf4, 0xff,
};
#define HSTRING_TFS 28

struct emitted { struct wtype *t; size_t off; };
struct gen
{
    struct buf type, proc;
    struct emitted done[64];
    UINT32 ndone;
};

static BOOL type_fc(struct wtype *t, BYTE *fc)
{
    if (t->kind == K_BASE || t->kind == K_ENUM) { *fc = t->fc; return TRUE; }
    return FALSE;
}

static size_t emit(struct gen *g, struct wtype *t);

static size_t emit_ip(struct gen *g, const GUID *iid)
{
    size_t off = g->type.len;
    put1(&g->type, FC_IP);
    put1(&g->type, FC_CONSTANT_IID);
    put(&g->type, iid, sizeof(*iid));
    return off;
}

static const BYTE guid_members[] = {FC_LONG, FC_SHORT, FC_SHORT, FC_BYTE, FC_BYTE, FC_BYTE, FC_BYTE,
                                    FC_BYTE, FC_BYTE, FC_BYTE, FC_BYTE};

static size_t emit_struct(struct gen *g, struct wtype *t)
{
    size_t deps[64], off;
    UINT32 i, n = 0, pos = 0;

    for (i = 0; i < t->nfields; i++)
        deps[i] = (t->fields[i]->kind == K_BASE || t->fields[i]->kind == K_ENUM) ? 0 : emit(g, t->fields[i]);

    off = g->type.len;
    put1(&g->type, FC_BOGUS_STRUCT);
    put1(&g->type, t->align - 1);
    put2(&g->type, t->size);
    put2(&g->type, 0);  /* no conformant array */
    put2(&g->type, 0);  /* no pointer layout */
    for (i = 0; i < t->nfields; i++)
    {
        struct wtype *f = t->fields[i];
        UINT32 aligned = (pos + f->align - 1) & ~(f->align - 1);
        BYTE fc;
        if (aligned > pos) { put1(&g->type, FC_STRUCTPAD1 + aligned - pos - 1); n++; }
        pos = aligned + f->size;
        if (type_fc(f, &fc)) { put1(&g->type, fc); n++; }
        else
        {
            put1(&g->type, FC_EMBEDDED_COMPLEX);
            put1(&g->type, 0);
            put_rel(&g->type, deps[i]);
            n += 4;
        }
    }
    if (t->size > pos) { put1(&g->type, FC_STRUCTPAD1 + t->size - pos - 1); n++; }
    if (!(n % 2)) put1(&g->type, FC_PAD);
    put1(&g->type, FC_END);
    return off;
}

static size_t emit(struct gen *g, struct wtype *t)
{
    size_t off;
    UINT32 i;

    if (t->kind == K_STRING) return HSTRING_TFS;
    for (i = 0; i < g->ndone; i++) if (g->done[i].t == t) return g->done[i].off;

    switch (t->kind)
    {
    case K_IFACE: off = emit_ip(g, &t->iid); break;
    case K_GUID:
        off = g->type.len;
        put1(&g->type, FC_STRUCT);
        put1(&g->type, 3);
        put2(&g->type, 16);
        put(&g->type, guid_members, sizeof(guid_members));
        put1(&g->type, FC_END);
        break;
    case K_STRUCT: off = emit_struct(g, t); break;
    default: return 0;
    }
    if (g->ndone < ARRAY_SIZE(g->done)) { g->done[g->ndone].t = t; g->done[g->ndone].off = off; g->ndone++; }
    return off;
}

/* a conformant array sized by the UINT32 at stack offset count_off */
static size_t emit_array(struct gen *g, struct wtype *elem, USHORT count_off, BOOL deref)
{
    size_t dep = 0, off;
    BYTE fc;
    BOOL simple = type_fc(elem, &fc);

    if (!simple) dep = emit(g, elem);
    off = g->type.len;
    if (simple)
    {
        put1(&g->type, FC_CARRAY);
        put1(&g->type, elem->align - 1);
        put2(&g->type, elem->size);
    }
    else
    {
        put1(&g->type, FC_BOGUS_ARRAY);
        put1(&g->type, elem->align - 1);
        put2(&g->type, 0);
    }
    put1(&g->type, FC_TOP_LEVEL_CONFORMANCE | FC_ULONG);
    put1(&g->type, deref ? FC_DEREFERENCE : 0);
    put2(&g->type, count_off);
    put2(&g->type, 1);
    if (simple)
    {
        put1(&g->type, fc);
        put1(&g->type, FC_END);
    }
    else
    {
        put4(&g->type, 0xffffffff);
        put2(&g->type, 0);
        put1(&g->type, FC_EMBEDDED_COMPLEX);
        put1(&g->type, 0);
        put_rel(&g->type, dep);
        put1(&g->type, FC_PAD);
        put1(&g->type, FC_END);
    }
    return off;
}

static size_t emit_pointer(struct gen *g, BYTE kind, BYTE flags, size_t target)
{
    size_t off = g->type.len;
    put1(&g->type, kind);
    put1(&g->type, flags);
    put_rel(&g->type, target);
    return off;
}

/* one ABI parameter of a method */
struct param { USHORT flags; BYTE fc; size_t type; BYTE fpu; UINT32 srv_size; };

#define P_MustSize 0x01
#define P_MustFree 0x02
#define P_In       0x08
#define P_Out      0x10
#define P_Return   0x20
#define P_Basetype 0x40
#define P_ByValue  0x80
#define P_SimpleRef 0x100

static BOOL add_in(struct gen *g, struct wtype *t, struct param *p)
{
    BYTE fc;
    if (type_fc(t, &fc))
    {
        p->flags = P_In | P_Basetype;
        p->fc = fc;
        p->fpu = fc == FC_FLOAT ? 1 : fc == FC_DOUBLE ? 2 : 0;
        return TRUE;
    }
    switch (t->kind)
    {
    case K_STRING: p->flags = P_MustSize | P_MustFree | P_In | P_ByValue; p->type = HSTRING_TFS; return TRUE;
    case K_IFACE: p->flags = P_MustSize | P_MustFree | P_In; p->type = emit(g, t); return TRUE;
    case K_GUID:
    case K_STRUCT:
        p->type = emit(g, t);
        if (t->size == 1 || t->size == 2 || t->size == 4 || t->size == 8)
            p->flags = P_MustSize | P_MustFree | P_In | P_ByValue;
        else
            p->flags = P_MustSize | P_MustFree | P_In | P_SimpleRef;
        return TRUE;
    default: return FALSE;
    }
}

static BOOL add_out(struct gen *g, struct wtype *t, struct param *p)
{
    BYTE fc;
    p->srv_size = t->kind == K_ARRAY ? sizeof(void *) : t->size;
    if (type_fc(t, &fc))
    {
        p->flags = P_Out | P_Basetype | P_SimpleRef;
        p->fc = fc;
        return TRUE;
    }
    switch (t->kind)
    {
    case K_STRING: p->flags = P_MustSize | P_MustFree | P_Out | P_SimpleRef; p->type = HSTRING_TFS; return TRUE;
    case K_IFACE:
        p->flags = P_MustSize | P_MustFree | P_Out;
        p->type = emit_pointer(g, FC_RP, FC_POINTER_DEREF, emit(g, t));
        return TRUE;
    case K_GUID:
    case K_STRUCT:
        p->flags = P_MustSize | P_MustFree | P_Out | P_SimpleRef;
        p->type = emit(g, t);
        return TRUE;
    default: return FALSE;
    }
}

static BOOL write_method(struct gen *g, struct winmd *md, UINT32 method, struct wtype **gargs, UINT32 nargs,
                         USHORT index)
{
    struct param params[64];
    UINT32 n, i, np = 0, pflags[32] = {0}, p, pend;
    const BYTE *b = blob(md, col(md, TBL_MethodDef, method, 4), NULL);
    struct wtype *ret;
    BOOL ret_byref;
    USHORT stack;
    BYTE cc = *b++;

    if (cc & 0x10) read_compressed(&b);
    n = read_compressed(&b);
    if (n > 30) return FALSE;

    p = col(md, TBL_MethodDef, method, 5);
    pend = list_end(md, TBL_MethodDef, method, 5, TBL_Param);
    for (; p < pend; p++)
    {
        UINT32 seq = col(md, TBL_Param, p, 1);
        if (seq <= n) pflags[seq] = col(md, TBL_Param, p, 0);
    }

    ret_byref = FALSE;
    if (*b == ELEMENT_TYPE_VOID) { b++; ret = NULL; }
    else ret = parse_type(md, &b, gargs, nargs);

    for (i = 1; i <= n; i++)
    {
        BOOL byref = FALSE;
        struct wtype *t;
        USHORT off = (np + 1) * sizeof(void *);

        while (*b == ELEMENT_TYPE_BYREF) { b++; byref = TRUE; }
        t = parse_type(md, &b, gargs, nargs);
        if (t->kind == K_BAD) return FALSE;
        memset(&params[np], 0, sizeof(params[np]) * 2);
        if (t->kind == K_ARRAY)
        {
            if (byref)        /* receive array: UINT32 *count, T **value */
            {
                params[np].flags = P_Out | P_Basetype | P_SimpleRef;
                params[np].fc = FC_ULONG;
                params[np].srv_size = sizeof(UINT32);
                params[np + 1].srv_size = sizeof(void *);
                params[np + 1].flags = P_MustSize | P_MustFree | P_Out;
                params[np + 1].type = emit_pointer(g, FC_RP, FC_ALLOCED_ON_STACK | FC_POINTER_DEREF,
                        emit_pointer(g, FC_OP, 0, emit_array(g, t->elem, off, TRUE)));
            }
            else              /* pass array (in) or fill array (out): UINT32 count, T *value */
            {
                params[np].flags = P_In | P_Basetype;
                params[np].fc = FC_ULONG;
                params[np + 1].type = emit_array(g, t->elem, off, FALSE);
                params[np + 1].flags = P_MustSize | P_MustFree | P_SimpleRef |
                        ((pflags[i] & 2) ? P_Out : P_In);
            }
            np += 2;
        }
        else
        {
            if (!(byref ? add_out(g, t, &params[np]) : add_in(g, t, &params[np]))) return FALSE;
            np++;
        }
    }
    if (ret)
    {
        USHORT off = (np + 1) * sizeof(void *);
        if (ret->kind == K_BAD) return FALSE;
        memset(&params[np], 0, sizeof(params[np]) * 2);
        if (ret->kind == K_ARRAY)
        {
            params[np].flags = P_Out | P_Basetype | P_SimpleRef;
            params[np].fc = FC_ULONG;
            params[np].srv_size = sizeof(UINT32);
            params[np + 1].srv_size = sizeof(void *);
            params[np + 1].flags = P_MustSize | P_MustFree | P_Out;
            params[np + 1].type = emit_pointer(g, FC_RP, FC_ALLOCED_ON_STACK | FC_POINTER_DEREF,
                    emit_pointer(g, FC_OP, 0, emit_array(g, ret->elem, off, TRUE)));
            np += 2;
        }
        else
        {
            if (!add_out(g, ret, &params[np])) return FALSE;
            np++;
        }
    }
    (void)ret_byref;

    stack = (np + 2) * sizeof(void *);
    put1(&g->proc, FC_AUTO_HANDLE);
    put1(&g->proc, Oi_OBJECT_PROC | Oi_OBJ_USE_V2_INTERPRETER);
    put2(&g->proc, index);
    put2(&g->proc, stack);
    put2(&g->proc, 0);
    put2(&g->proc, 0);
    put1(&g->proc, 0x47);
    put1(&g->proc, np + 1);
#ifdef __x86_64__
    {
        USHORT mask = 0;
        for (i = 0; i < np && i < 3; i++) mask |= params[i].fpu << ((i + 1) * 2);
        put1(&g->proc, 10);
        put1(&g->proc, 1);
        put2(&g->proc, 0);
        put2(&g->proc, 0);
        put2(&g->proc, 0);
        put2(&g->proc, mask);
    }
#else
    put1(&g->proc, 8);
    put1(&g->proc, 1);
    put2(&g->proc, 0);
    put2(&g->proc, 0);
    put2(&g->proc, 0);
#endif
    for (i = 0; i < np; i++)
    {
        /* the stub allocates an out parameter's memory itself: say how much, as widl and MIDL do
         * (bits 13-15, in 8-byte units). Without it a [out] boolean reached the server as NULL. */
        if ((params[i].flags & P_Out) && !(params[i].flags & P_In) && params[i].srv_size &&
            params[i].srv_size <= 7 * 8)
            params[i].flags |= ((params[i].srv_size + 7) / 8) << 13;
        put2(&g->proc, params[i].flags);
        put2(&g->proc, (i + 1) * sizeof(void *));
        if (params[i].flags & P_Basetype) { put1(&g->proc, params[i].fc); put1(&g->proc, 0); }
        else put2(&g->proc, params[i].type);
    }
    put2(&g->proc, P_Out | P_Return | P_Basetype);
    put2(&g->proc, (np + 1) * sizeof(void *));
    put1(&g->proc, FC_LONG);
    put1(&g->proc, 0);
    return TRUE;
}

static HRESULT build(struct ifentry *e, WORD *funcs, WORD *parentfuncs, GUID *parentiid,
                     unsigned char **type, unsigned char **proc, unsigned short **offset)
{
    struct winmd *md = e->md;
    UINT32 m = col(md, TBL_TypeDef, e->row, 5), end = list_end(md, TBL_TypeDef, e->row, 5, TBL_MethodDef), i;
    struct gen g = {0};
    char name[512];
    HRESULT hr = S_OK;

    typedef_name(md, e->row, name, sizeof(name));
    *parentfuncs = e->delegate ? 3 : 6;
    *parentiid = e->delegate ? IID_IUnknown : IID_IInspectable_;
    *funcs = 0;
    for (i = m; i < end; i++)
    {
        const char *mname = str(md, col(md, TBL_MethodDef, i, 3));
        if (e->delegate && strcmp(mname, "Invoke")) continue;   /* .ctor */
        (*funcs)++;
    }
    *offset = malloc((*parentfuncs + *funcs - 3) * sizeof(**offset));
    for (i = 0; i < *parentfuncs - 3u; i++) (*offset)[i] = 0xffff;

    put(&g.type, prelude, sizeof(prelude));
    EnterCriticalSection(&winmd_cs);
    for (i = 0; m < end; m++)
    {
        const char *mname = str(md, col(md, TBL_MethodDef, m, 3));
        if (e->delegate && strcmp(mname, "Invoke")) continue;
        (*offset)[*parentfuncs - 3 + i] = g.proc.len;
        if (!write_method(&g, md, m, e->args, e->nargs, *parentfuncs + i))
        {
            FIXME("cannot marshal %s::%s\n", name, mname);
            hr = E_NOTIMPL;
            break;
        }
        i++;
    }
    drain_pending();
    LeaveCriticalSection(&winmd_cs);
    put2(&g.type, 0);

    if (FAILED(hr))
    {
        free(g.type.data);
        free(g.proc.data);
        free(*offset);
        return hr;
    }
    TRACE("%s (%s): %u methods, %Iu type bytes, %Iu proc bytes\n", name, debugstr_guid(&e->iid), *funcs,
          g.type.len, g.proc.len);
    *type = g.type.data;
    *proc = g.proc.data;
    return S_OK;
}

/* ------------------------------------------------------------------------------------------- */
/* The factory                                                                                 */

static USER_MARSHAL_ROUTINE_QUADRUPLE user_marshal[1];

static const USER_MARSHAL_ROUTINE_QUADRUPLE *get_user_marshal(void)
{
    if (!user_marshal[0].pfnFree)
    {
        HMODULE combase = GetModuleHandleW(L"combase.dll");
        user_marshal[0].pfnBufferSize = (void *)GetProcAddress(combase, "HSTRING_UserSize");
        user_marshal[0].pfnMarshall = (void *)GetProcAddress(combase, "HSTRING_UserMarshal");
        user_marshal[0].pfnUnmarshall = (void *)GetProcAddress(combase, "HSTRING_UserUnmarshal");
        user_marshal[0].pfnFree = (void *)GetProcAddress(combase, "HSTRING_UserFree");
    }
    return user_marshal;
}

static HRESULT WINAPI factory_QueryInterface(IPSFactoryBuffer *iface, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IPSFactoryBuffer))
    {
        *out = iface;
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI factory_AddRef(IPSFactoryBuffer *iface) { return 2; }
static ULONG WINAPI factory_Release(IPSFactoryBuffer *iface) { return 1; }

static HRESULT WINAPI factory_CreateProxy(IPSFactoryBuffer *iface, IUnknown *outer, REFIID riid,
                                          IRpcProxyBuffer **proxy, void **out)
{
    unsigned char *type, *proc;
    unsigned short *offset;
    WORD funcs, parentfuncs;
    struct ifentry *e;
    GUID parentiid;
    HRESULT hr;

    TRACE("%s\n", debugstr_guid(riid));
    if (!(e = lookup(riid))) return E_NOINTERFACE;
    if (FAILED(hr = build(e, &funcs, &parentfuncs, &parentiid, &type, &proc, &offset))) return hr;
    return ndr_create_generated_proxy(riid, &parentiid, funcs, parentfuncs, type, proc, offset,
                                      get_user_marshal(), outer, proxy, out);
}

static HRESULT WINAPI factory_CreateStub(IPSFactoryBuffer *iface, REFIID riid, IUnknown *server,
                                         IRpcStubBuffer **stub)
{
    unsigned char *type, *proc;
    unsigned short *offset;
    WORD funcs, parentfuncs;
    struct ifentry *e;
    GUID parentiid;
    HRESULT hr;

    TRACE("%s\n", debugstr_guid(riid));
    if (!(e = lookup(riid))) return E_NOINTERFACE;
    if (FAILED(hr = build(e, &funcs, &parentfuncs, &parentiid, &type, &proc, &offset))) return hr;
    return ndr_create_generated_stub(riid, &parentiid, funcs, parentfuncs, type, proc, offset,
                                     get_user_marshal(), server, stub);
}

static const IPSFactoryBufferVtbl factory_vtbl =
{
    factory_QueryInterface,
    factory_AddRef,
    factory_Release,
    factory_CreateProxy,
    factory_CreateStub,
};

static IPSFactoryBuffer factory = { &factory_vtbl };

/***********************************************************************
 *           __wine_metadata_ps_lookup   (RPCRT4.@)
 *
 * Whether the metadata-based marshaler can build a proxy for an interface.
 */
BOOL CDECL __wine_metadata_ps_lookup(const GUID *iid)
{
    return lookup(iid) != NULL;
}

/***********************************************************************
 *           __wine_metadata_ps_factory   (RPCRT4.@)
 */
HRESULT CDECL __wine_metadata_ps_factory(REFIID riid, void **out)
{
    return IPSFactoryBuffer_QueryInterface(&factory, riid, out);
}
