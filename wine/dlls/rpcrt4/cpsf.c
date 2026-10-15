/*
 * COM proxy/stub factory (CStdPSFactory) implementation
 *
 * Copyright 2001 Ove Kåven, TransGaming Technologies
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
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define COBJMACROS

#include "windef.h"
#include "winbase.h"
#include "winerror.h"
#include "winreg.h"

#include "objbase.h"

#include "rpcproxy.h"

#include "wine/debug.h"

#include "cpsf.h"

#define PROXY_TABLE_VERSION_COMPACT 0x10   /* TableVersion of the compact OS proxy format */

static const unsigned short *proxy_format_offsets( const ProxyFileInfo *info, const CInterfaceProxyVtbl *vtbl );
#define IS_MARKER(p) ((ULONG_PTR)(p) >= (ULONG_PTR)-16)

WINE_DEFAULT_DEBUG_CHANNEL(ole);

static void format_clsid( WCHAR *buffer, const CLSID *clsid )
{
    swprintf( buffer, 39, L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
              clsid->Data1, clsid->Data2, clsid->Data3,
              clsid->Data4[0], clsid->Data4[1], clsid->Data4[2], clsid->Data4[3],
              clsid->Data4[4], clsid->Data4[5], clsid->Data4[6], clsid->Data4[7] );

}

static BOOL FindProxyInfo(const ProxyFileInfo **pProxyFileList, REFIID riid, const ProxyFileInfo **pProxyInfo, int *pIndex)
{
  while (*pProxyFileList) {
    /* newer MIDL output sets pIIDLookupRtn to -1 (or NULL): search the table headers ourselves */
    if ((*pProxyFileList)->pIIDLookupRtn && (*pProxyFileList)->pIIDLookupRtn != (PIIDLookup)-1) {
      if ((*pProxyFileList)->pIIDLookupRtn(riid, pIndex)) {
        *pProxyInfo = *pProxyFileList;
        TRACE("found: ProxyInfo %p Index %d\n", *pProxyInfo, *pIndex);
        return TRUE;
      }
    } else {
      int i;
      for (i = 0; i < (int)((*pProxyFileList)->TableSize & 0xffff); i++) {   /* high word holds flags in newer MIDL output */
        const CInterfaceStubVtbl *stub = (*pProxyFileList)->pStubVtblList[i];
        if (stub && stub->header.piid && IsEqualGUID(stub->header.piid, riid)) {
          *pProxyInfo = *pProxyFileList;
          *pIndex = i;
          TRACE("found by scan: ProxyInfo %p Index %d\n", *pProxyInfo, *pIndex);
          return TRUE;
        }
      }
    }
    pProxyFileList++;
  }
  TRACE("not found\n");
  return FALSE;
}

static HRESULT WINAPI CStdPSFactory_QueryInterface(LPPSFACTORYBUFFER iface,
                                                  REFIID riid,
                                                  LPVOID *obj)
{
  CStdPSFactoryBuffer *This = (CStdPSFactoryBuffer *)iface;
  TRACE("(%p)->QueryInterface(%s,%p)\n",iface,debugstr_guid(riid),obj);
  if (IsEqualGUID(&IID_IUnknown,riid) ||
      IsEqualGUID(&IID_IPSFactoryBuffer,riid)) {
    *obj = This;
    InterlockedIncrement( &This->RefCount );
    return S_OK;
  }
  return E_NOINTERFACE;
}

static ULONG WINAPI CStdPSFactory_AddRef(LPPSFACTORYBUFFER iface)
{
  CStdPSFactoryBuffer *This = (CStdPSFactoryBuffer *)iface;
  TRACE("(%p)->AddRef()\n",iface);
  return InterlockedIncrement( &This->RefCount );
}

static ULONG WINAPI CStdPSFactory_Release(LPPSFACTORYBUFFER iface)
{
  CStdPSFactoryBuffer *This = (CStdPSFactoryBuffer *)iface;
  TRACE("(%p)->Release()\n",iface);
  return InterlockedDecrement( &This->RefCount );
}

static HRESULT WINAPI CStdPSFactory_CreateProxy(LPPSFACTORYBUFFER iface,
                                               LPUNKNOWN pUnkOuter,
                                               REFIID riid,
                                               LPRPCPROXYBUFFER *ppProxy,
                                               LPVOID *ppv)
{
  CStdPSFactoryBuffer *This = (CStdPSFactoryBuffer *)iface;
  const ProxyFileInfo *ProxyInfo;
  int Index;
  TRACE("(%p)->CreateProxy(%p,%s,%p,%p)\n",iface,pUnkOuter,
       debugstr_guid(riid),ppProxy,ppv);
  if (!FindProxyInfo(This->pProxyFileList,riid,&ProxyInfo,&Index))
    return E_NOINTERFACE;
  return StdProxy_Construct(riid, pUnkOuter, ProxyInfo, Index, iface, ppProxy, ppv);
}

static HRESULT WINAPI CStdPSFactory_CreateStub(LPPSFACTORYBUFFER iface,
                                              REFIID riid,
                                              LPUNKNOWN pUnkServer,
                                              LPRPCSTUBBUFFER *ppStub)
{
  CStdPSFactoryBuffer *This = (CStdPSFactoryBuffer *)iface;
  const ProxyFileInfo *ProxyInfo;
  int Index;
  TRACE("(%p)->CreateStub(%s,%p,%p)\n",iface,debugstr_guid(riid),
       pUnkServer,ppStub);
  if (!FindProxyInfo(This->pProxyFileList,riid,&ProxyInfo,&Index))
    return E_NOINTERFACE;

  if(ProxyInfo->pDelegatedIIDs && ProxyInfo->pDelegatedIIDs[Index])
    return  CStdStubBuffer_Delegating_Construct(riid, pUnkServer, (ProxyInfo->pNamesArray ? ProxyInfo->pNamesArray[Index] : "?"),
                                                get_stub_vtbl(ProxyInfo, Index), ProxyInfo->pDelegatedIIDs[Index],
                                                iface, ppStub);

  return CStdStubBuffer_Construct(riid, pUnkServer, (ProxyInfo->pNamesArray ? ProxyInfo->pNamesArray[Index] : "?"),
                                  get_stub_vtbl(ProxyInfo, Index), iface, ppStub);
}

static const IPSFactoryBufferVtbl CStdPSFactory_Vtbl =
{
  CStdPSFactory_QueryInterface,
  CStdPSFactory_AddRef,
  CStdPSFactory_Release,
  CStdPSFactory_CreateProxy,
  CStdPSFactory_CreateStub
};


/* Modern proxy DLLs (Windows 8+ MIDL output) keep their tables and even the factory buffer in read-only
 * memory and ship complete stub vtables, so only patch what is actually writable. */
static BOOL is_writable( const void *ptr )
{
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery( ptr, &mbi, sizeof(mbi) )) return FALSE;
    return (mbi.State == MEM_COMMIT) &&
           (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

static void init_psfactory( CStdPSFactoryBuffer *psfac, const ProxyFileInfo **file_list )
{
    DWORD i, j, k;

    psfac->lpVtbl = &CStdPSFactory_Vtbl;
    psfac->RefCount = 0;
    psfac->pProxyFileList = file_list;
    for (i = 0; file_list[i]; i++)
    {
        const PCInterfaceProxyVtblList *proxies = file_list[i]->pProxyVtblList;
        const PCInterfaceStubVtblList *stubs = file_list[i]->pStubVtblList;

        if (file_list[i]->TableVersion >= PROXY_TABLE_VERSION_COMPACT) continue;   /* handled by get_*_vtbl() */
        for (j = 0; j < (file_list[i]->TableSize & 0xffff); j++)
        {
            /* FIXME: i think that different vtables should be copied for
             * async interfaces */
            void * const *pSrcRpcStubVtbl = (void * const *)&CStdStubBuffer_Vtbl;
            void **pRpcStubVtbl = (void **)&stubs[j]->Vtbl;

            if (file_list[i]->pDelegatedIIDs && file_list[i]->pDelegatedIIDs[j])
            {
                void **vtbl = proxies[j]->Vtbl;
                if (file_list[i]->TableVersion > 1) vtbl++;
                if (is_writable( vtbl ))
                    fill_delegated_proxy_table( (IUnknownVtbl *)vtbl, stubs[j]->header.DispatchTableCount,
                                                proxy_format_offsets( file_list[i], proxies[j] ) );
                pSrcRpcStubVtbl = (void * const *)&CStdStubBuffer_Delegating_Vtbl;
            }

            if (!is_writable( pRpcStubVtbl )) continue;   /* read-only tables are already complete */
            for (k = 0; k < sizeof(IRpcStubBufferVtbl)/sizeof(void *); k++)
                if (!pRpcStubVtbl[k] || pRpcStubVtbl[k] == (void *)-1) pRpcStubVtbl[k] = pSrcRpcStubVtbl[k];
        }
    }
}

/* factories for proxy DLLs whose CStdPSFactoryBuffer is read-only */
static struct
{
    const ProxyFileInfo **file_list;
    CStdPSFactoryBuffer   factory;
} *ro_factories;
static unsigned int ro_factories_count, ro_factories_size;
static CRITICAL_SECTION ro_factories_cs;
static CRITICAL_SECTION_DEBUG ro_factories_cs_debug =
{
    0, 0, &ro_factories_cs,
    { &ro_factories_cs_debug.ProcessLocksList, &ro_factories_cs_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": ro_factories_cs") }
};
static CRITICAL_SECTION ro_factories_cs = { &ro_factories_cs_debug, -1, 0, 0, 0, 0 };


/* Writable shadow copies of proxy/stub vtables that the DLL keeps in read-only memory. */
static struct { const void *orig; void *copy; } *shadow_tables;
static unsigned int shadow_count, shadow_size;

static void *find_shadow( const void *orig )
{
    unsigned int i;
    for (i = 0; i < shadow_count; i++) if (shadow_tables[i].orig == orig) return shadow_tables[i].copy;
    return NULL;
}

static BOOL add_shadow_copy( const void *orig, void *copy )
{
    if (shadow_count == shadow_size)
    {
        unsigned int new_size = shadow_size ? shadow_size * 2 : 32;
        void *tmp = shadow_tables ? HeapReAlloc( GetProcessHeap(), 0, shadow_tables, new_size * sizeof(*shadow_tables) )
                                  : HeapAlloc( GetProcessHeap(), 0, new_size * sizeof(*shadow_tables) );
        if (!tmp) return FALSE;
        shadow_tables = tmp; shadow_size = new_size;
    }
    shadow_tables[shadow_count].orig = orig;
    shadow_tables[shadow_count].copy = copy;
    shadow_count++;
    return TRUE;
}

static void *add_shadow( const void *orig, size_t size )
{
    void *copy;
    if (!(copy = HeapAlloc( GetProcessHeap(), 0, size ))) return NULL;
    memcpy( copy, orig, size );
    if (!add_shadow_copy( orig, copy )) { HeapFree( GetProcessHeap(), 0, copy ); return NULL; }
    return copy;
}

static void fill_stub_vtbl( IRpcStubBufferVtbl *vtbl, BOOL delegating )
{
    void * const *src = delegating ? (void * const *)&CStdStubBuffer_Delegating_Vtbl : (void * const *)&CStdStubBuffer_Vtbl;
    void **dst = (void **)vtbl;
    unsigned int k;
    for (k = 0; k < sizeof(IRpcStubBufferVtbl)/sizeof(void *); k++) if (!dst[k] || dst[k] == (void *)-1) dst[k] = src[k];
}

/* Compact tables (MIDL for Windows 10+ OS proxies, TableVersion 0x10):
 *   proxy entry: { MIDL_STUBLESS_PROXY_INFO *info; const IID *piid; void *vtbl_or_marker; }
 *   stub entry:  { CInterfaceStubHeader header; void *marker; }
 * The per-method tables are not stored: every method is stubless, inherited methods (format string
 * offset 0xffff) forward to the base interface, and the IRpcStubBuffer vtable is the standard one.
 * pDispatchTable is -2 when the stub delegates and must forward inherited methods. */

struct compact_proxy_entry
{
    const MIDL_STUBLESS_PROXY_INFO *info;
    const IID *piid;
    void *const *vtbl;   /* -1 delegated, -2 plain, or a real function table */
};

static BOOL is_compact( const ProxyFileInfo *info )
{
    return info->TableVersion >= PROXY_TABLE_VERSION_COMPACT;
}

/* format string offsets of a non-compact stubless proxy, indexed by vtable slot: the
 * MIDL_STUBLESS_PROXY_INFO pointer is the word before the CInterfaceProxyHeader we see */
static const unsigned short *proxy_format_offsets( const ProxyFileInfo *info, const CInterfaceProxyVtbl *vtbl )
{
    const MIDL_STUBLESS_PROXY_INFO *proxy_info;

    if (info->TableVersion <= 1) return NULL;
    if (!(proxy_info = *(const MIDL_STUBLESS_PROXY_INFO * const *)vtbl)) return NULL;
    return proxy_info->FormatStringOffset;
}

static BOOL method_is_inherited( const unsigned short *offsets, DWORD i )
{
    return offsets && offsets[i] == 0xffff;
}

/* stub vtable to use for a table entry: the DLL's own if writable (patched in place), else a filled copy */
const CInterfaceStubVtbl *get_stub_vtbl( const ProxyFileInfo *info, int index )
{
    const CInterfaceStubVtbl *orig = info->pStubVtblList[index];
    BOOL delegating = info->pDelegatedIIDs && info->pDelegatedIIDs[index];
    CInterfaceStubVtbl *copy;

    if (is_compact( info ))
    {
        DWORD count = orig->header.DispatchTableCount;
        EnterCriticalSection( &ro_factories_cs );
        if (!(copy = find_shadow( orig )) &&
            (copy = HeapAlloc( GetProcessHeap(), 0, sizeof(*copy) + count * sizeof(void *) )))
        {
            void **dst = (void **)&copy->Vtbl;
            void * const *src = delegating ? (void * const *)&CStdStubBuffer_Delegating_Vtbl : (void * const *)&CStdStubBuffer_Vtbl;
            unsigned int k;

            copy->header = orig->header;
            for (k = 0; k < sizeof(IRpcStubBufferVtbl)/sizeof(void *); k++) dst[k] = src[k];
            copy->Vtbl.Release = delegating ? CStdStubBuffer_Delegating_Release_Compact : CStdStubBuffer_Release_Compact;
            if (IS_MARKER( copy->header.pDispatchTable ))
            {
                /* -2: synthesize a dispatch table that forwards inherited methods to the base stub */
                PRPC_STUB_FUNCTION *table = (PRPC_STUB_FUNCTION *)(copy + 1);
                const unsigned short *offsets = copy->header.pServerInfo ? copy->header.pServerInfo->FmtStringOffset : NULL;
                for (k = 0; k < count; k++)
                    table[k] = (delegating && method_is_inherited( offsets, k )) ? (PRPC_STUB_FUNCTION)NdrStubForwardingFunction
                                                                         : (PRPC_STUB_FUNCTION)NdrStubCall2;
                copy->header.pDispatchTable = table;
            }
            if (!add_shadow_copy( orig, copy )) { HeapFree( GetProcessHeap(), 0, copy ); copy = NULL; }
            TRACE( "compact stub %s: %lu methods, delegating %u -> %p\n", debugstr_guid(orig->header.piid), count, delegating, copy );
        }
        LeaveCriticalSection( &ro_factories_cs );
        return copy ? copy : orig;
    }

    if (is_writable( &orig->Vtbl )) return orig;
    EnterCriticalSection( &ro_factories_cs );
    if (!(copy = find_shadow( orig )) && (copy = add_shadow( orig, sizeof(*orig) )))
        fill_stub_vtbl( &copy->Vtbl, delegating );
    LeaveCriticalSection( &ro_factories_cs );
    return copy ? copy : orig;
}

BOOL proxy_info_is_stubless( const ProxyFileInfo *info )
{
    return info->TableVersion > 1 || is_compact( info );
}

/* proxy vtable to use: delegated proxies need their IUnknown slots filled, which needs a copy if read-only */
const CInterfaceProxyVtbl *get_proxy_vtbl( const ProxyFileInfo *info, int index )
{
    const CInterfaceProxyVtbl *orig = info->pProxyVtblList[index];
    DWORD count = info->pStubVtblList[index]->header.DispatchTableCount;
    unsigned int extra = info->TableVersion > 1 ? 1 : 0;
    CInterfaceProxyVtbl *copy;

    if (is_compact( info ))
    {
        const struct compact_proxy_entry *entry = (const struct compact_proxy_entry *)orig;
        BOOL delegating = info->pDelegatedIIDs && info->pDelegatedIIDs[index];
        void **table;

        EnterCriticalSection( &ro_factories_cs );
        if (!(table = find_shadow( orig )) &&
            (table = HeapAlloc( GetProcessHeap(), 0, (2 + count) * sizeof(void *) )))
        {
            const unsigned short *offsets = entry->info ? entry->info->FormatStringOffset : NULL;
            void **slots = table + 2;
            DWORD k;

            table[0] = (void *)entry->info;   /* stubless thunks find MIDL_STUBLESS_PROXY_INFO just before the vtable */
            table[1] = (void *)entry->piid;
            for (k = 0; k < count; k++)
                slots[k] = IS_MARKER( entry->vtbl ) ? (void *)-1 : entry->vtbl[k];
            if (delegating)
            {
                /* IUnknown and every inherited method go to the base proxy, the rest stay stubless (-1) */
                fill_delegated_proxy_table( (IUnknownVtbl *)slots, 3, NULL );
                for (k = 3; k < count; k++)
                    if (slots[k] == (void *)-1 && method_is_inherited( offsets, k ))
                        slots[k] = get_delegating_vtbl_method( k - 3 );
            }
            else
            {
                IUnknownVtbl *unk = (IUnknownVtbl *)slots;
                unk->QueryInterface = IUnknown_QueryInterface_Proxy;
                unk->AddRef = IUnknown_AddRef_Proxy;
                unk->Release = IUnknown_Release_Proxy;
            }
            if (!add_shadow_copy( orig, table )) { HeapFree( GetProcessHeap(), 0, table ); table = NULL; }
            TRACE( "compact proxy %s: %lu methods, delegating %u -> %p\n", debugstr_guid(entry->piid), count, delegating, table );
        }
        LeaveCriticalSection( &ro_factories_cs );
        /* shaped like a TableVersion 2 entry: caller skips the info pointer */
        return table ? (const CInterfaceProxyVtbl *)table : orig;
    }

    if (!(info->pDelegatedIIDs && info->pDelegatedIIDs[index])) return orig;
    if (is_writable( orig->Vtbl )) return orig;
    EnterCriticalSection( &ro_factories_cs );
    if (!(copy = find_shadow( orig )) && (copy = add_shadow( orig, sizeof(*orig) + (count + extra) * sizeof(void *) )))
        fill_delegated_proxy_table( (IUnknownVtbl *)(copy->Vtbl + extra), count, proxy_format_offsets( info, orig ) );
    LeaveCriticalSection( &ro_factories_cs );
    return copy ? copy : orig;
}

static CStdPSFactoryBuffer *get_writable_factory( CStdPSFactoryBuffer *psfac, const ProxyFileInfo **file_list )
{
    unsigned int i;
    CStdPSFactoryBuffer *ret = NULL;

    if (is_writable( psfac )) return psfac;
    EnterCriticalSection( &ro_factories_cs );
    for (i = 0; i < ro_factories_count; i++)
        if (ro_factories[i].file_list == file_list) { ret = &ro_factories[i].factory; break; }
    if (!ret)
    {
        if (ro_factories_count == ro_factories_size)
        {
            unsigned int new_size = ro_factories_size ? ro_factories_size * 2 : 8;
            void *tmp = ro_factories ? HeapReAlloc( GetProcessHeap(), 0, ro_factories, new_size * sizeof(*ro_factories) )
                                     : HeapAlloc( GetProcessHeap(), 0, new_size * sizeof(*ro_factories) );
            if (tmp) { ro_factories = tmp; ro_factories_size = new_size; }
        }
        if (ro_factories_count < ro_factories_size)
        {
            ro_factories[ro_factories_count].file_list = file_list;
            memset( &ro_factories[ro_factories_count].factory, 0, sizeof(CStdPSFactoryBuffer) );
            ret = &ro_factories[ro_factories_count++].factory;
            WARN( "proxy factory buffer %p is read-only, using %p\n", psfac, ret );
        }
    }
    LeaveCriticalSection( &ro_factories_cs );
    return ret;
}

/***********************************************************************
 *           NdrDllGetClassObject [RPCRT4.@]
 */
HRESULT WINAPI NdrDllGetClassObject(REFCLSID rclsid, REFIID iid, LPVOID *ppv,
                                   const ProxyFileInfo **pProxyFileList,
                                   const CLSID *pclsid,
                                   CStdPSFactoryBuffer *pPSFactoryBuffer)
{
  TRACE("(%s, %s, %p, %p, %s, %p)\n", debugstr_guid(rclsid),
    debugstr_guid(iid), ppv, pProxyFileList, debugstr_guid(pclsid),
    pPSFactoryBuffer);

  *ppv = NULL;
  if (!(pPSFactoryBuffer = get_writable_factory( pPSFactoryBuffer, pProxyFileList ))) return E_OUTOFMEMORY;
  if (!pPSFactoryBuffer->lpVtbl) init_psfactory( pPSFactoryBuffer, pProxyFileList );

  if (pclsid && IsEqualGUID(rclsid, pclsid))
    return IPSFactoryBuffer_QueryInterface((LPPSFACTORYBUFFER)pPSFactoryBuffer, iid, ppv);
  else {
    const ProxyFileInfo *info;
    int index;
    /* otherwise, the dll may be using the iid as the clsid, so
     * search for it in the proxy file list */
    if (FindProxyInfo(pProxyFileList, rclsid, &info, &index))
      return IPSFactoryBuffer_QueryInterface((LPPSFACTORYBUFFER)pPSFactoryBuffer, iid, ppv);

    WARN("class %s not available\n", debugstr_guid(rclsid));
    return CLASS_E_CLASSNOTAVAILABLE;
  }
}

/***********************************************************************
 *           NdrDllCanUnloadNow [RPCRT4.@]
 */
HRESULT WINAPI NdrDllCanUnloadNow(CStdPSFactoryBuffer *pPSFactoryBuffer)
{
  return pPSFactoryBuffer->RefCount != 0 ? S_FALSE : S_OK;
}


/***********************************************************************
 *           NdrDllRegisterProxy [RPCRT4.@]
 */
HRESULT WINAPI NdrDllRegisterProxy(HMODULE hDll,
                                  const ProxyFileInfo **pProxyFileList,
                                  const CLSID *pclsid)
{
  WCHAR clsid[39], keyname[50], module[MAX_PATH];
  HKEY key, subkey;
  DWORD len;

  TRACE("(%p,%p,%s)\n", hDll, pProxyFileList, debugstr_guid(pclsid));

  if (!hDll) return E_HANDLE;
  if (!*pProxyFileList) return E_NOINTERFACE;

  if (pclsid)
      format_clsid( clsid, pclsid );
  else if ((*pProxyFileList)->TableSize > 0)
      format_clsid( clsid,(*pProxyFileList)->pStubVtblList[0]->header.piid);
  else
      return E_NOINTERFACE;

  /* register interfaces to point to clsid */
  while (*pProxyFileList) {
    unsigned u;
    for (u=0; u<(*pProxyFileList)->TableSize; u++) {
      CInterfaceStubVtbl *proxy = (*pProxyFileList)->pStubVtblList[u];
      PCInterfaceName name = (*pProxyFileList)->pNamesArray[u];

      TRACE("registering %s %s => %s\n",
            debugstr_a(name), debugstr_guid(proxy->header.piid), debugstr_w(clsid));

      lstrcpyW( keyname, L"Interface\\" );
      format_clsid( keyname + lstrlenW(keyname), proxy->header.piid );
      if (RegCreateKeyW(HKEY_CLASSES_ROOT, keyname, &key) == ERROR_SUCCESS) {
        WCHAR num[10];
        if (name)
          RegSetValueExA(key, NULL, 0, REG_SZ, (const BYTE *)name, strlen(name)+1);
        RegSetValueW( key, L"ProxyStubClsid32", REG_SZ, clsid, 0 );
        swprintf(num, ARRAY_SIZE(num), L"%u", proxy->header.DispatchTableCount);
        RegSetValueW( key, L"NumMethods", REG_SZ, num, 0 );
        RegCloseKey(key);
      }
    }
    pProxyFileList++;
  }

  /* register clsid to point to module */
  lstrcpyW( keyname, L"CLSID\\" );
  lstrcatW( keyname, clsid );
  len = GetModuleFileNameW(hDll, module, ARRAY_SIZE(module));
  if (len && len < sizeof(module)) {
      TRACE("registering CLSID %s => %s\n", debugstr_w(clsid), debugstr_w(module));
      if (RegCreateKeyW(HKEY_CLASSES_ROOT, keyname, &key) == ERROR_SUCCESS) {
          RegSetValueExW(key, NULL, 0, REG_SZ, (const BYTE *)L"PSFactoryBuffer", sizeof(L"PSFactoryBuffer"));
          if (RegCreateKeyW(key, L"InProcServer32", &subkey) == ERROR_SUCCESS) {
              RegSetValueExW(subkey, NULL, 0, REG_SZ, (LPBYTE)module, (lstrlenW(module)+1)*sizeof(WCHAR));
              RegSetValueExW(subkey, L"ThreadingModel", 0, REG_SZ, (const BYTE *)L"Both", sizeof(L"Both"));
              RegCloseKey(subkey);
          }
          RegCloseKey(key);
      }
  }

  return S_OK;
}

/***********************************************************************
 *           NdrDllUnregisterProxy [RPCRT4.@]
 */
HRESULT WINAPI NdrDllUnregisterProxy(HMODULE hDll,
                                    const ProxyFileInfo **pProxyFileList,
                                    const CLSID *pclsid)
{
  WCHAR keyname[50];
  WCHAR clsid[39];

  TRACE("(%p,%p,%s)\n", hDll, pProxyFileList, debugstr_guid(pclsid));
  if (pclsid)
      format_clsid( clsid, pclsid );
  else if ((*pProxyFileList)->TableSize > 0)
      format_clsid( clsid,(*pProxyFileList)->pStubVtblList[0]->header.piid);
  else
      return E_NOINTERFACE;

  /* unregister interfaces */
  while (*pProxyFileList) {
    unsigned u;
    for (u=0; u<(*pProxyFileList)->TableSize; u++) {
      CInterfaceStubVtbl *proxy = (*pProxyFileList)->pStubVtblList[u];
      PCInterfaceName name = (*pProxyFileList)->pNamesArray[u];

      TRACE("unregistering %s %s\n", debugstr_a(name), debugstr_guid(proxy->header.piid));

      lstrcpyW( keyname, L"Interface\\" );
      format_clsid( keyname + lstrlenW(keyname), proxy->header.piid );
      RegDeleteTreeW(HKEY_CLASSES_ROOT, keyname);
    }
    pProxyFileList++;
  }

  /* unregister clsid */
  lstrcpyW( keyname, L"CLSID\\" );
  lstrcatW( keyname, clsid );
  RegDeleteTreeW(HKEY_CLASSES_ROOT, keyname);

  return S_OK;
}
