#ifndef STORAGE_PROBE_H
#define STORAGE_PROBE_H
#define COBJMACROS
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <stdio.h>
#include <string.h>

typedef struct object { void **vtable; } object;
static const GUID info_iid = {0x36,0,0,{0xc0,0,0,0,0,0,0,0x46}};

static void release(object *p)
{
    if (p) ((ULONG (WINAPI *)(object *))p->vtable[2])(p);
}

static HRESULT qi(object *p, const GUID *iid, object **out)
{
    return ((HRESULT (WINAPI *)(object *, const GUID *, void **))p->vtable[0])(p, iid, (void **)out);
}

static HRESULT factory(const WCHAR *name, const GUID *iid, object **out)
{
    HSTRING str = NULL;
    HRESULT hr = WindowsCreateString(name, lstrlenW(name), &str);
    if (FAILED(hr)) return hr;
    hr = RoGetActivationFactory(str, iid, (void **)out);
    WindowsDeleteString(str);
    return hr;
}

static HRESULT await_op(object *op)
{
    object *info = NULL;
    HRESULT hr;
    INT32 status = 0;
    if (!op) return E_POINTER;
    if (FAILED(hr = qi(op, &info_iid, &info))) return hr;
    for (unsigned i = 0; i < 500 && status == 0; i++)
    {
        hr = ((HRESULT (WINAPI *)(object *, INT32 *))info->vtable[7])(info, &status);
        if (FAILED(hr)) break;
        if (status == 0) Sleep(10);
    }
    if (SUCCEEDED(hr) && status != 1)
    {
        hr = E_FAIL;
        if (status == 3) ((HRESULT (WINAPI *)(object *, HRESULT *))info->vtable[8])(info, &hr);
    }
    release(info);
    return hr;
}
#endif
