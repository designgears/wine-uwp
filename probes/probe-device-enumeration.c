/* Check display devices and driver versions through the WinRT Device-kind query. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define COBJMACROS
#include "windef.h"
#include "winbase.h"
#include "winstring.h"
#include "initguid.h"
#include "roapi.h"
#define WIDL_using_Windows_Foundation
#define WIDL_using_Windows_Foundation_Collections
#include "windows.foundation.h"
#define WIDL_using_Windows_Devices_Enumeration
#include "windows.devices.enumeration.h"

static const WCHAR *driver_property = L"{a8b865dd-2e3d-4094-ad97-e593a70c75d6} 3";

/* A one-string iterable for the driver-version property. */
struct property_iterator { IIterator_HSTRING iface; LONG ref; boolean valid; };
static HRESULT WINAPI iter_query(IIterator_HSTRING *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IInspectable) &&
        !IsEqualGUID(iid, &IID_IIterator_HSTRING)) return E_NOINTERFACE;
    *out = iface;
    IIterator_HSTRING_AddRef(iface);
    return S_OK;
}
static ULONG WINAPI iter_addref(IIterator_HSTRING *iface)
{ return InterlockedIncrement(&((struct property_iterator *)iface)->ref); }
static ULONG WINAPI iter_release(IIterator_HSTRING *iface)
{
    ULONG ref = InterlockedDecrement(&((struct property_iterator *)iface)->ref);
    if (!ref) free(iface);
    return ref;
}
static HRESULT WINAPI iter_iids(IIterator_HSTRING *iface, ULONG *n, IID **ids) { return E_NOTIMPL; }
static HRESULT WINAPI iter_class(IIterator_HSTRING *iface, HSTRING *name) { return E_NOTIMPL; }
static HRESULT WINAPI iter_trust(IIterator_HSTRING *iface, TrustLevel *level) { return E_NOTIMPL; }
static HRESULT WINAPI iter_current(IIterator_HSTRING *iface, HSTRING *value)
{
    *value = NULL;
    if (!((struct property_iterator *)iface)->valid) return E_BOUNDS;
    return WindowsCreateString(driver_property, wcslen(driver_property), value);
}
static HRESULT WINAPI iter_has(IIterator_HSTRING *iface, boolean *valid)
{ *valid = ((struct property_iterator *)iface)->valid; return S_OK; }
static HRESULT WINAPI iter_next(IIterator_HSTRING *iface, boolean *valid)
{ *valid = ((struct property_iterator *)iface)->valid = FALSE; return S_OK; }
static HRESULT WINAPI iter_many(IIterator_HSTRING *iface, UINT32 size, HSTRING *items, UINT32 *count)
{
    *count = 0;
    if (!size || !((struct property_iterator *)iface)->valid) return S_OK;
    if (FAILED(iter_current(iface, items))) return E_OUTOFMEMORY;
    ((struct property_iterator *)iface)->valid = FALSE;
    *count = 1;
    return S_OK;
}
static const IIterator_HSTRINGVtbl iterator_vtbl = {
    iter_query, iter_addref, iter_release, iter_iids, iter_class, iter_trust,
    iter_current, iter_has, iter_next, iter_many
};
static HRESULT WINAPI list_query(IIterable_HSTRING *iface, REFIID iid, void **out)
{
    *out = NULL;
    if (!IsEqualGUID(iid, &IID_IUnknown) && !IsEqualGUID(iid, &IID_IInspectable) &&
        !IsEqualGUID(iid, &IID_IIterable_HSTRING)) return E_NOINTERFACE;
    *out = iface;
    return S_OK;
}
static ULONG WINAPI list_ref(IIterable_HSTRING *iface) { return 1; }
static HRESULT WINAPI list_iids(IIterable_HSTRING *iface, ULONG *n, IID **ids) { return E_NOTIMPL; }
static HRESULT WINAPI list_class(IIterable_HSTRING *iface, HSTRING *name) { return E_NOTIMPL; }
static HRESULT WINAPI list_trust(IIterable_HSTRING *iface, TrustLevel *level) { return E_NOTIMPL; }
static HRESULT WINAPI list_first(IIterable_HSTRING *iface, IIterator_HSTRING **out)
{
    struct property_iterator *iter = calloc(1, sizeof(*iter));
    if (!iter) return E_OUTOFMEMORY;
    iter->iface.lpVtbl = &iterator_vtbl;
    iter->ref = 1;
    iter->valid = TRUE;
    *out = &iter->iface;
    return S_OK;
}
static const IIterable_HSTRINGVtbl list_vtbl = {
    list_query, list_ref, list_ref, list_iids, list_class, list_trust, list_first
};
static IIterable_HSTRING properties = { &list_vtbl };

int main(int argc, char **argv)
{
    const WCHAR *class_name = L"Windows.Devices.Enumeration.DeviceInformation";
    const WCHAR *query = L"System.Devices.ClassGuid:=\"{4d36e968-e325-11ce-bfc1-08002be10318}\"";
    IDeviceInformationStatics2 *factory = NULL;
    IAsyncOperation_DeviceInformationCollection *op = NULL;
    IVectorView_DeviceInformation *devices = NULL;
    IAsyncInfo *async = NULL;
    HSTRING name = NULL, filter = NULL;
    AsyncStatus status = Started;
    UINT32 count = 0, i;
    HRESULT hr;
    int result = 1;
    int expect_empty = argc > 1 && !strcmp(argv[1], "empty");

    if (expect_empty) query = L"System.Devices.ClassGuid:=\"{00000000-0000-0000-0000-000000000001}\"";

    hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) goto done;
    WindowsCreateString(class_name, wcslen(class_name), &name);
    WindowsCreateString(query, wcslen(query), &filter);
    hr = RoGetActivationFactory(name, &IID_IDeviceInformationStatics2, (void **)&factory);
    printf("DEVICE_ENUM activation=%#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = IDeviceInformationStatics2_FindAllAsyncWithKindAqsFilterAndAdditionalProperties(
        factory, filter, &properties, DeviceInformationKind_Device, NULL);
    printf("DEVICE_ENUM null_output=%#lx\n", hr);
    if (hr != E_POINTER) { hr = E_FAIL; goto done; }
    hr = IDeviceInformationStatics2_FindAllAsyncWithKindAqsFilterAndAdditionalProperties(
        factory, filter, &properties, DeviceInformationKind_Device, &op);
    printf("DEVICE_ENUM FindAllAsync=%#lx\n", hr);
    if (FAILED(hr)) goto done;
    hr = IAsyncOperation_DeviceInformationCollection_QueryInterface(op, &IID_IAsyncInfo, (void **)&async);
    if (FAILED(hr)) goto done;
    for (i = 0; i < 500 && status == Started; ++i)
    {
        hr = IAsyncInfo_get_Status(async, &status);
        if (FAILED(hr)) goto done;
        if (status == Started) Sleep(10);
    }
    hr = IAsyncOperation_DeviceInformationCollection_GetResults(op, &devices);
    printf("DEVICE_ENUM status=%u GetResults=%#lx\n", status, hr);
    if (FAILED(hr)) goto done;
    hr = IVectorView_DeviceInformation_get_Size(devices, &count);
    if (FAILED(hr)) goto done;
    printf("DEVICE_ENUM count=%u\n", count);
    for (i = 0; i < count; ++i)
    {
        IDeviceInformation *device;
        IMapView_HSTRING_IInspectable *map = NULL;
        IInspectable *value = NULL;
        IPropertyValue *property = NULL;
        HSTRING id = NULL, version = NULL, key = NULL, label = NULL;
        hr = IVectorView_DeviceInformation_GetAt(devices, i, &device);
        if (FAILED(hr)) goto done;
        hr = IDeviceInformation_get_Id(device, &id);
        if (SUCCEEDED(hr)) hr = IDeviceInformation_get_Properties(device, &map);
        WindowsCreateString(driver_property, wcslen(driver_property), &key);
        if (SUCCEEDED(hr)) hr = IMapView_HSTRING_IInspectable_Lookup(map, key, &value);
        if (SUCCEEDED(hr)) hr = IInspectable_QueryInterface(value, &IID_IPropertyValue, (void **)&property);
        if (SUCCEEDED(hr)) hr = IPropertyValue_GetString(property, &version);
        printf("DEVICE_ENUM device=%u hr=%#lx id=%ls driver=%ls\n", i, hr,
               WindowsGetStringRawBuffer(id, NULL), WindowsGetStringRawBuffer(version, NULL));
        if (SUCCEEDED(hr) && !WindowsGetStringLen(version)) hr = E_FAIL;
        if (SUCCEEDED(hr)) hr = IDeviceInformation_get_Name(device, &label);
        printf("DEVICE_ENUM device=%u name_hr=%#lx name=%ls\n", i, hr, WindowsGetStringRawBuffer(label, NULL));
        if (SUCCEEDED(hr) && !WindowsGetStringLen(label)) hr = E_FAIL;
        WindowsDeleteString(id);
        WindowsDeleteString(version);
        WindowsDeleteString(key);
        WindowsDeleteString(label);
        if (property) IPropertyValue_Release(property);
        if (value) IInspectable_Release(value);
        if (map) IMapView_HSTRING_IInspectable_Release(map);
        IDeviceInformation_Release(device);
        if (FAILED(hr)) goto done;
    }
    if (expect_empty ? !count : !!count) result = 0;
done:
    if (devices) IVectorView_DeviceInformation_Release(devices);
    if (async) IAsyncInfo_Release(async);
    if (op) IAsyncOperation_DeviceInformationCollection_Release(op);
    if (factory) IDeviceInformationStatics2_Release(factory);
    WindowsDeleteString(name);
    WindowsDeleteString(filter);
    RoUninitialize();
    printf("DEVICE_ENUM result=%s hr=%#lx\n", result ? "FAIL" : "PASS", hr);
    return result;
}
