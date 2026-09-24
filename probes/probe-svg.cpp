// Render SVG files through Direct2D's ID2D1DeviceContext5::CreateSvgDocument/DrawSvgDocument and
// save each as a PNG, to see what an icon looks like without running the app.
// Usage: probe-svg.exe <out-dir> <size> <file.svg>...   (Windows paths)
#include <windows.h>
#include <d2d1_3.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <cstdio>

static HRESULT save_png(IWICImagingFactory *wic, IWICBitmap *bitmap, const wchar_t *path)
{
    IWICStream *stream; IWICBitmapEncoder *encoder; IWICBitmapFrameEncode *frame;
    HRESULT hr = wic->CreateStream(&stream);
    if (FAILED(hr)) return hr;
    if (SUCCEEDED(hr = stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
        SUCCEEDED(hr = wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)))
    {
        if (SUCCEEDED(hr = encoder->Initialize(stream, WICBitmapEncoderNoCache)) &&
            SUCCEEDED(hr = encoder->CreateNewFrame(&frame, nullptr)))
        {
            UINT w, h; bitmap->GetSize(&w, &h);
            WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
            frame->Initialize(nullptr); frame->SetSize(w, h); frame->SetPixelFormat(&fmt);
            hr = frame->WriteSource(bitmap, nullptr);
            if (SUCCEEDED(hr)) hr = frame->Commit();
            if (SUCCEEDED(hr)) hr = encoder->Commit();
            frame->Release();
        }
        encoder->Release();
    }
    stream->Release();
    return hr;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: probe-svg <out-dir> <size> <svg>...\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UINT size = _wtoi(argv[2]);
    ID2D1Factory *factory; IWICImagingFactory *wic;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **)&factory);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    for (int i = 3; i < argc; i++)
    {
        IWICBitmap *bitmap; ID2D1RenderTarget *rt; ID2D1DeviceContext5 *dc; IStream *in; ID2D1SvgDocument *doc;
        wic->CreateBitmap(size, size, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT hr = factory->CreateWicBitmapRenderTarget(bitmap, &props, &rt);
        if (FAILED(hr) || FAILED(hr = rt->QueryInterface(IID_PPV_ARGS(&dc))))
        { fprintf(stderr, "no device context 5: %#lx\n", hr); return 1; }
        if (FAILED(hr = SHCreateStreamOnFileEx(argv[i], STGM_READ, 0, FALSE, nullptr, &in)))
        { fwprintf(stderr, L"%s: open %#lx\n", argv[i], hr); continue; }
        hr = dc->CreateSvgDocument(in, D2D1::SizeF((float)size, (float)size), &doc);
        in->Release();
        if (FAILED(hr)) { fwprintf(stderr, L"%s: CreateSvgDocument %#lx\n", argv[i], hr); continue; }
        // Draw into an offscreen bitmap target first, the way Win2D's CanvasRenderTarget does, then
        // composite it over grey.
        ID2D1Bitmap1 *offscreen = nullptr; ID2D1Image *wic_target = nullptr;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        dc->BeginDraw();
        dc->GetTarget(&wic_target);
        if (SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(size, size), nullptr, 0, &bp, &offscreen)))
        {
            dc->SetTarget(offscreen);
            dc->Clear(D2D1::ColorF(0, 0, 0, 0));
            dc->DrawSvgDocument(doc);
            dc->SetTarget(wic_target);
            dc->Clear(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1.0f));
            dc->DrawImage(offscreen);
            offscreen->Release();
        }
        else fprintf(stderr, "no offscreen bitmap\n");
        if (wic_target) wic_target->Release();
        hr = dc->EndDraw();
        wchar_t out[MAX_PATH];
        const wchar_t *name = wcsrchr(argv[i], L'\\'); name = name ? name + 1 : argv[i];
        swprintf(out, MAX_PATH, L"%s\\%s.png", argv[1], name);
        hr = save_png(wic, bitmap, out);
        wprintf(L"%s -> %s %#lx\n", name, out, hr);
        doc->Release(); dc->Release(); rt->Release(); bitmap->Release();
    }
    return 0;
}
