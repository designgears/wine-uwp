// Render SVG files through Direct2D's ID2D1DeviceContext5::CreateSvgDocument/DrawSvgDocument and
// save each as a PNG, to see what an icon looks like without running the app.
// Usage: probe-svg.exe <out-dir> <size> <file.svg>...   (Windows paths)
//        probe-svg.exe <out-dir> 64 --geometry        (AA, aliasing, transform, clip and DPI checks)
#include <windows.h>
#include <d2d1_3.h>
#include <wincodec.h>
#include <shlwapi.h>
#include <cstdio>
#include <cstring>

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

// Exercises the direct Win2D geometry path, including state restored by supersampling.
static int geometry_checks(ID2D1Factory *factory, IWICImagingFactory *wic, const wchar_t *out_dir)
{
    int failures = 0;
    for (int test = 0; test < 3; test++)
    {
        const bool aliased = test == 1;
        const float dpi = test == 2 ? 192.0f : 96.0f;
        IWICBitmap *bitmap = nullptr;
        ID2D1RenderTarget *rt = nullptr;
        ID2D1DeviceContext5 *dc = nullptr;
        ID2D1Bitmap1 *offscreen = nullptr;
        ID2D1Image *target = nullptr, *restored = nullptr;
        ID2D1SolidColorBrush *brush = nullptr;
        ID2D1PathGeometry *geometry = nullptr;
        ID2D1GeometrySink *sink = nullptr;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        HRESULT hr = wic->CreateBitmap(64, 64, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
        if (SUCCEEDED(hr)) hr = factory->CreateWicBitmapRenderTarget(bitmap, &props, &rt);
        if (SUCCEEDED(hr)) hr = rt->QueryInterface(IID_PPV_ARGS(&dc));
        auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, props.pixelFormat, dpi, dpi);
        if (SUCCEEDED(hr)) hr = dc->CreateBitmap(D2D1::SizeU(64, 64), nullptr, 0, &bp, &offscreen);
        if (SUCCEEDED(hr)) hr = dc->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
        if (SUCCEEDED(hr)) hr = factory->CreatePathGeometry(&geometry);
        if (SUCCEEDED(hr)) hr = geometry->Open(&sink);
        if (FAILED(hr)) { fprintf(stderr, "geometry setup %#lx\n", hr); return 1; }
        sink->BeginFigure(D2D1::Point2F(0, 0), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLine(D2D1::Point2F(20, 0));
        sink->AddLine(D2D1::Point2F(0, 14));
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        sink->Close(); sink->Release();
        dc->BeginDraw();
        dc->GetTarget(&target);
        dc->SetTarget(offscreen);
        dc->SetDpi(dpi, dpi);
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->PushAxisAlignedClip(D2D1::RectF(0, 0, 22, 30), D2D1_ANTIALIAS_MODE_ALIASED);
        auto transform = D2D1::Matrix3x2F::Translation(5, 7);
        dc->SetTransform(transform);
        dc->SetAntialiasMode(aliased ? D2D1_ANTIALIAS_MODE_ALIASED : D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        dc->FillGeometry(geometry, brush);
        dc->DrawGeometry(geometry, brush, 1.0f);
        D2D1_MATRIX_3X2_F actual;
        dc->GetTransform(&actual);
        dc->GetTarget(&restored);
        bool state_ok = !memcmp(&actual, &transform, sizeof(actual)) && restored == offscreen;
        restored->Release();
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        // A second draw must still honor the original caller clip.
        dc->FillRectangle(D2D1::RectF(24, 0, 30, 4), brush);
        dc->PopAxisAlignedClip();
        dc->SetTarget(target);
        dc->SetDpi(96, 96);
        dc->Clear(D2D1::ColorF(0.5f, 0.5f, 0.5f, 1));
        dc->DrawImage(offscreen);
        hr = dc->EndDraw();
        UINT stride, bytes; BYTE *pixels;
        IWICBitmapLock *lock = nullptr;
        bitmap->Lock(nullptr, WICBitmapLockRead, &lock);
        lock->GetStride(&stride); lock->GetDataPointer(&bytes, &pixels);
        unsigned int partial = 0;
        for (unsigned int y = 0; y < 64; y++)
            for (unsigned int x = 0; x < 64; x++)
                if (pixels[y * stride + x * 4] > 128 && pixels[y * stride + x * 4] < 255) partial++;
        unsigned int factor = 1; // DrawImage uses the source bitmap DPI when compositing to WIC.
        bool inside = pixels[(9 * factor) * stride + (7 * factor) * 4] > 240;
        bool clipped = pixels[2 * factor * stride + 25 * factor * 4] <= 128;
        bool passed = SUCCEEDED(hr) && state_ok && inside && clipped && (aliased ? partial == 0 : partial > 10);
        printf("geometry dpi=%.0f aliased=%d partial=%u state=%d placement=%d clip=%d: %s\n",
            dpi, aliased, partial, state_ok, inside, clipped, passed ? "PASS" : "FAIL");
        failures += !passed;
        lock->Release();
        wchar_t path[MAX_PATH];
        swprintf(path, MAX_PATH, L"%s\\geometry-%d.png", out_dir, test);
        save_png(wic, bitmap, path);
        target->Release(); offscreen->Release(); brush->Release(); geometry->Release();
        dc->Release(); rt->Release(); bitmap->Release();
    }
    return failures ? 1 : 0;
}

int wmain(int argc, wchar_t **argv)
{
    if (argc < 4) { fprintf(stderr, "usage: probe-svg <out-dir> <size> <svg>...\n"); return 1; }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    UINT size = _wtoi(argv[2]);
    ID2D1Factory *factory; IWICImagingFactory *wic;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **)&factory);
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!wcscmp(argv[3], L"--geometry")) return geometry_checks(factory, wic, argv[1]);
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
