/*
 * Standalone UWP audio diagnostic, adapted from Wine's
 * dlls/windows.media.devices/tests/devices.c async activation test.
 * Copyright 2021 Andrew Eikum and Remi Bernon for CodeWeavers.
 * Adaptation distributed under LGPL-2.1-or-later, as the original test.
 *
 * Build: x86_64-w64-mingw32-gcc -std=c11 -O2 -Wall -Wextra
 *        probe-uwp-audio.c -o probe-uwp-audio.exe
 *        -lole32 -lruntimeobject -luuid
 *
 * Plays only a WASAPI buffer marked AUDCLNT_BUFFERFLAGS_SILENT.
 * No capture, configuration changes, registry writes or file writes.
 * Optional: --factory-dll Z:\\path\\appxportable.dll obtains the MediaDevice
 * factory directly from an existing shim, preserving OS class registration.
 * Use APPX_PORTABLE_MODE=trace to keep that DLL's broad activation hook passive.
 */
#define _WIN32_WINNT 0x0a00
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <stdio.h>
#include <string.h>

/* The public IMediaDeviceStatics ABI from windows.media.devices.idl.
 * Using its two method slots avoids depending on generated WinRT headers. */
typedef struct media_statics { void **vtable; } media_statics;
static const GUID media_statics_iid =
    {0xaa2d9a40,0x909f,0x4bba,{0xbf,0x8b,0x0c,0x0d,0x29,0x6f,0x14,0xf0}};
static const GUID agile_iid =
    {0x94ea2b94,0xe9cc,0x49e0,{0xc0,0xff,0xee,0x64,0xca,0x8f,0x5b,0x90}};
static const GUID device_statics_iid =
    {0xc17f100e,0x3a46,0x4a78,{0x80,0x13,0x76,0x9d,0xc9,0xb9,0x73,0x90}};
static const GUID async_info_iid =
    {0x36,0,0,{0xc0,0,0,0,0,0,0,0x46}};

static HRESULT (WINAPI *activate_audio)(const WCHAR *, REFIID, PROPVARIANT *,
    IActivateAudioInterfaceCompletionHandler *, IActivateAudioInterfaceAsyncOperation **);

static struct {
    LONG refs;
    HANDLE completed;
    HRESULT get_result_hr;
    HRESULT activation_hr;
    IUnknown *result;
} completion;

static void report(const char *stage, HRESULT hr)
{
    printf("UWP_AUDIO stage=%s hr=%08lx\n", stage, (unsigned long)hr);
    fflush(stdout);
}

static HRESULT WINAPI completion_qi(IActivateAudioInterfaceCompletionHandler *iface,
    REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    *out = NULL;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &agile_iid) ||
        IsEqualIID(iid, &IID_IActivateAudioInterfaceCompletionHandler))
    {
        *out = iface;
        IActivateAudioInterfaceCompletionHandler_AddRef(iface);
        return S_OK;
    }
    return E_NOINTERFACE;
}

static ULONG WINAPI completion_addref(IActivateAudioInterfaceCompletionHandler *iface)
{
    (void)iface;
    return InterlockedIncrement(&completion.refs);
}

static ULONG WINAPI completion_release(IActivateAudioInterfaceCompletionHandler *iface)
{
    (void)iface;
    return InterlockedDecrement(&completion.refs);
}

static HRESULT WINAPI completion_done(IActivateAudioInterfaceCompletionHandler *iface,
    IActivateAudioInterfaceAsyncOperation *op)
{
    (void)iface;
    completion.get_result_hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(
        op, &completion.activation_hr, &completion.result);
    report("GetActivateResult", completion.get_result_hr);
    report("activation_result", completion.activation_hr);
    printf("UWP_AUDIO activated_interface=%d\n", completion.result != NULL);
    fflush(stdout);
    SetEvent(completion.completed);
    return S_OK;
}

static IActivateAudioInterfaceCompletionHandlerVtbl completion_vtable = {
    completion_qi, completion_addref, completion_release, completion_done
};
static IActivateAudioInterfaceCompletionHandler completion_handler = { &completion_vtable };

static void release_object(media_statics *object)
{
    if (object) ((ULONG (WINAPI *)(media_statics *))object->vtable[2])(object);
}

/* Match Microsoft's WindowsAudioSession sample: enumerate the actual selector,
 * then require that the current default render endpoint appears in the result. */
static int test_enumeration(HSTRING selector, HSTRING default_id)
{
    static const WCHAR class_name[] = L"Windows.Devices.Enumeration.DeviceInformation";
    media_statics *factory = NULL, *op = NULL, *info = NULL, *view = NULL;
    HSTRING name = NULL;
    HRESULT hr;
    INT32 status = 0;
    UINT32 count = 0;
    int good = 0, found_default = 0;

    hr = WindowsCreateString(class_name, ARRAYSIZE(class_name) - 1, &name);
    report("enum_WindowsCreateString", hr);
    if (FAILED(hr)) goto done;
    hr = RoGetActivationFactory(name, &device_statics_iid, (void **)&factory);
    report("enum_RoGetActivationFactory", hr);
    if (FAILED(hr) || !factory) goto done;
    hr = ((HRESULT (WINAPI *)(media_statics *, HSTRING, media_statics **))factory->vtable[10])(
        factory, selector, &op);
    report("FindAllAsync_selector", hr);
    if (FAILED(hr) || !op) goto done;
    hr = ((HRESULT (WINAPI *)(media_statics *, REFIID, void **))op->vtable[0])(
        op, &async_info_iid, (void **)&info);
    report("enum_QueryInterface_IAsyncInfo", hr);
    if (FAILED(hr) || !info) goto done;
    for (unsigned int i = 0; i < 500; ++i)
    {
        hr = ((HRESULT (WINAPI *)(media_statics *, INT32 *))info->vtable[7])(info, &status);
        if (FAILED(hr) || status != 0) break;
        Sleep(10);
    }
    report("enum_Status", hr);
    printf("UWP_AUDIO enumeration_status=%ld\n", (long)status);
    if (FAILED(hr)) goto done;
    if (status != 1)
    {
        if (status == 3)
        {
            HRESULT error = E_FAIL;
            hr = ((HRESULT (WINAPI *)(media_statics *, HRESULT *))info->vtable[8])(info, &error);
            report("enum_GetErrorCode", hr);
            report("enum_operation_error", error);
        }
        goto done;
    }
    hr = ((HRESULT (WINAPI *)(media_statics *, media_statics **))op->vtable[8])(op, &view);
    report("enum_GetResults", hr);
    if (FAILED(hr) || !view) goto done;
    hr = ((HRESULT (WINAPI *)(media_statics *, UINT32 *))view->vtable[7])(view, &count);
    report("enum_GetSize", hr);
    if (FAILED(hr)) goto done;
    printf("UWP_AUDIO render_device_count=%u\n", count);
    for (UINT32 i = 0; i < count; ++i)
    {
        media_statics *device = NULL;
        HSTRING id = NULL, label = NULL;
        hr = ((HRESULT (WINAPI *)(media_statics *, UINT32, media_statics **))view->vtable[6])(view, i, &device);
        report("enum_GetAt", hr);
        if (FAILED(hr) || !device) goto done;
        hr = ((HRESULT (WINAPI *)(media_statics *, HSTRING *))device->vtable[6])(device, &id);
        report("enum_DeviceId", hr);
        if (SUCCEEDED(hr) && id)
        {
            const WCHAR *text = WindowsGetStringRawBuffer(id, NULL);
            printf("UWP_AUDIO render_device_index=%u id=%ls\n", i, text);
            if (default_id && !lstrcmpiW(text, WindowsGetStringRawBuffer(default_id, NULL))) found_default = 1;
        }
        hr = ((HRESULT (WINAPI *)(media_statics *, HSTRING *))device->vtable[7])(device, &label);
        report("enum_DeviceName", hr);
        if (SUCCEEDED(hr)) printf("UWP_AUDIO render_device_index=%u name=%ls\n", i, WindowsGetStringRawBuffer(label, NULL));
        if (label) WindowsDeleteString(label);
        if (id) WindowsDeleteString(id);
        release_object(device);
    }
    printf("UWP_AUDIO default_render_device_found=%d\n", found_default);
    good = count > 0 && found_default;
done:
    release_object(view);
    release_object(info);
    release_object(op);
    release_object(factory);
    if (name) WindowsDeleteString(name);
    return good;
}

static int test_stream(const WCHAR *device_path)
{
    IActivateAudioInterfaceAsyncOperation *op = NULL;
    IAudioClient *client = NULL;
    IAudioRenderClient *render = NULL;
    WAVEFORMATEX *format = NULL;
    BYTE *buffer = NULL;
    UINT32 frames = 0, padding = 0;
    HRESULT hr;
    DWORD wait;
    int good = 0;
    BOOL started = FALSE;

    completion.refs = 1;
    completion.get_result_hr = E_PENDING;
    completion.activation_hr = E_PENDING;
    completion.result = NULL;
    completion.completed = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!completion.completed)
    {
        report("CreateEvent", HRESULT_FROM_WIN32(GetLastError()));
        return 0;
    }

    hr = activate_audio(device_path, &IID_IAudioClient, NULL, &completion_handler, &op);
    report("ActivateAudioInterfaceAsync", hr);
    if (FAILED(hr) || !op) goto done;
    wait = WaitForSingleObject(completion.completed, 10000);
    printf("UWP_AUDIO callback_wait=%lu\n", (unsigned long)wait);
    if (wait != WAIT_OBJECT_0)
    {
        report("callback_wait", wait == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) : HRESULT_FROM_WIN32(GetLastError()));
        /* The callback owns this static handler and its event until process exit.
         * Do not close an event on which a late callback might still signal. */
        IActivateAudioInterfaceAsyncOperation_Release(op);
        return 0;
    }
    if (FAILED(completion.get_result_hr) || FAILED(completion.activation_hr) || !completion.result) goto done;

    hr = IUnknown_QueryInterface(completion.result, &IID_IAudioClient, (void **)&client);
    report("QueryInterface_IAudioClient", hr);
    if (FAILED(hr) || !client) goto done;
    hr = IAudioClient_GetMixFormat(client, &format);
    report("GetMixFormat", hr);
    if (FAILED(hr) || !format) goto done;
    printf("UWP_AUDIO format_tag=%u channels=%u rate=%lu bits=%u block_align=%u\n",
        format->wFormatTag, format->nChannels, (unsigned long)format->nSamplesPerSec,
        format->wBitsPerSample, format->nBlockAlign);

    hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, NULL);
    report("Initialize_shared", hr);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_GetBufferSize(client, &frames);
    report("GetBufferSize", hr);
    if (FAILED(hr) || !frames) goto done;
    printf("UWP_AUDIO buffer_frames=%u\n", frames);
    hr = IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render);
    report("GetService_IAudioRenderClient", hr);
    if (FAILED(hr) || !render) goto done;
    hr = IAudioRenderClient_GetBuffer(render, frames, &buffer);
    report("GetBuffer", hr);
    if (FAILED(hr)) goto done;
    hr = IAudioRenderClient_ReleaseBuffer(render, frames, AUDCLNT_BUFFERFLAGS_SILENT);
    report("ReleaseBuffer_silent", hr);
    if (FAILED(hr)) goto done;
    hr = IAudioClient_Start(client);
    report("Start", hr);
    if (FAILED(hr)) goto done;
    started = TRUE;
    Sleep(250);
    hr = IAudioClient_GetCurrentPadding(client, &padding);
    report("GetCurrentPadding", hr);
    if (FAILED(hr)) goto done;
    printf("UWP_AUDIO remaining_frames=%u consumed_frames=%u\n", padding,
        padding <= frames ? frames - padding : 0);
    good = padding < frames;

done:
    if (started)
    {
        hr = IAudioClient_Stop(client);
        report("Stop", hr);
        if (FAILED(hr)) good = 0;
    }
    if (render) IAudioRenderClient_Release(render);
    if (client) IAudioClient_Release(client);
    if (format) CoTaskMemFree(format);
    if (completion.result) IUnknown_Release(completion.result);
    if (op) IActivateAudioInterfaceAsyncOperation_Release(op);
    CloseHandle(completion.completed);
    return good;
}

int main(int argc, char **argv)
{
    static const WCHAR class_name[] = L"Windows.Media.Devices.MediaDevice";
    media_statics *factory = NULL;
    HSTRING name = NULL, default_id = NULL, selector = NULL;
    HMODULE module, factory_module = NULL;
    HRESULT hr, default_hr = E_FAIL, selector_hr = E_FAIL;
    int stream_good = 0, selector_good = 0, enumeration_good = -1;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 1 && (argc != 3 || strcmp(argv[1], "--factory-dll")))
    {
        fprintf(stderr, "Usage: probe-uwp-audio.exe [--factory-dll WINDOWS_DLL_PATH]\n");
        return 2;
    }
    hr = RoInitialize(RO_INIT_MULTITHREADED);
    report("RoInitialize_MTA", hr);
    if (FAILED(hr)) return 2;
    module = LoadLibraryW(L"mmdevapi.dll");
    if (!module)
    {
        report("LoadLibrary_mmdevapi", HRESULT_FROM_WIN32(GetLastError()));
        goto done;
    }
    activate_audio = (void *)GetProcAddress(module, "ActivateAudioInterfaceAsync");
    report("GetProcAddress_ActivateAudioInterfaceAsync", activate_audio ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
    if (!activate_audio) goto done;
    hr = WindowsCreateString(class_name, ARRAYSIZE(class_name) - 1, &name);
    report("WindowsCreateString", hr);
    if (FAILED(hr)) goto done;
    if (argc == 3)
    {
        WCHAR path[32768];
        IUnknown *activation_factory = NULL;
        HRESULT (WINAPI *get_factory)(HSTRING, IUnknown **);
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, argv[2], -1, path, ARRAYSIZE(path)))
        {
            report("factory_dll_path", HRESULT_FROM_WIN32(GetLastError()));
            goto done;
        }
        factory_module = LoadLibraryW(path);
        report("LoadLibrary_factory_dll", factory_module ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        if (!factory_module) goto done;
        get_factory = (void *)GetProcAddress(factory_module, "DllGetActivationFactory");
        report("GetProcAddress_DllGetActivationFactory", get_factory ? S_OK : HRESULT_FROM_WIN32(GetLastError()));
        if (!get_factory) goto done;
        hr = get_factory(name, &activation_factory);
        report("direct_DllGetActivationFactory", hr);
        if (SUCCEEDED(hr) && activation_factory)
        {
            hr = IUnknown_QueryInterface(activation_factory, &media_statics_iid, (void **)&factory);
            report("direct_QueryInterface_IMediaDeviceStatics", hr);
            IUnknown_Release(activation_factory);
        }
    }
    else
    {
        hr = RoGetActivationFactory(name, &media_statics_iid, (void **)&factory);
        report("RoGetActivationFactory_IMediaDeviceStatics", hr);
    }
    if (FAILED(hr) || !factory) goto done;

    /* AudioDeviceRole_Default == 0; these method positions follow Wine's IDL. */
    default_hr = ((HRESULT (WINAPI *)(media_statics *, INT32, HSTRING *))factory->vtable[10])(
        factory, 0, &default_id);
    report("GetDefaultAudioRenderId", default_hr);
    if (SUCCEEDED(default_hr))
        printf("UWP_AUDIO default_id=%ls\n", WindowsGetStringRawBuffer(default_id, NULL));
    selector_hr = ((HRESULT (WINAPI *)(media_statics *, HSTRING *))factory->vtable[7])(factory, &selector);
    report("GetAudioRenderSelector", selector_hr);
    if (SUCCEEDED(selector_hr))
    {
        UINT32 length = 0;
        const WCHAR *text = WindowsGetStringRawBuffer(selector, &length);
        printf("UWP_AUDIO selector=%ls\n", text);
        selector_good = length != 0;
    }
    if (selector_good) enumeration_good = test_enumeration(selector, default_id);
    /* Continue after a selector failure to separate it from endpoint activation. */
    if (SUCCEEDED(default_hr) && default_id && WindowsGetStringLen(default_id))
        stream_good = test_stream(WindowsGetStringRawBuffer(default_id, NULL));

done:
    printf("UWP_AUDIO_RESULT selector=%s enumeration=%s stream=%s\n",
        selector_good ? "PASS" : "FAIL", enumeration_good < 0 ? "SKIP" : enumeration_good ? "PASS" : "FAIL",
        stream_good ? "PASS" : "FAIL");
    if (selector) WindowsDeleteString(selector);
    if (default_id) WindowsDeleteString(default_id);
    if (name) WindowsDeleteString(name);
    if (factory) ((ULONG (WINAPI *)(media_statics *))factory->vtable[2])(factory);
    /* Keep mmdevapi loaded until process exit if an async callback timed out. */
    RoUninitialize();
    return selector_good && enumeration_good == 1 && stream_good ? 0 : 1;
}
