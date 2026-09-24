// Render sample strings through Direct2D + DirectWrite into a PNG, one line per font family, to judge
// Wine's text rendering the way XAML drives it (DrawTextLayout, grayscale antialiasing).
// Usage: wine probe-text.exe <out.png> [size] [family[:weight[:opsz]]...]
// With an opsz the format is made through IDWriteFactory6 with wght and opsz axis values, as XAML does.
#include <windows.h>
#include <d2d1_1.h>
#include <dwrite_3.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t **argv)
{
    static const wchar_t *defaults[] = { L"Segoe UI Variable", L"Segoe UI", L"Tahoma" };
    static const wchar_t text[] = L"Modeling  Visualization  Drawings  Items  Ctrl Shift";
    const wchar_t **families = argc > 3 ? (const wchar_t **)argv + 3 : defaults;
    int family_count = argc > 3 ? argc - 3 : 3;
    float size = argc > 2 ? (float)_wtof(argv[2]) : 14.0f;
    UINT width = 520, line = (UINT)(size * 2.2f), height = line * family_count + 8;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic; ID2D1Factory *d2d; IDWriteFactory *dw;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **)&d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dw);

    IWICBitmap *bitmap; ID2D1RenderTarget *rt; ID2D1SolidColorBrush *brush;
    wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    d2d->CreateWicBitmapRenderTarget(bitmap, &props, &rt);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &brush);
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);

    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0.12f, 0.12f, 0.12f, 1));
    for (int i = 0; i < family_count; i++)
    {
        IDWriteTextFormat *format; IDWriteTextLayout *layout;
        wchar_t family[128]; int weight = 400; float opsz = 0;
        lstrcpynW(family, families[i], 128);
        if (wchar_t *colon = wcschr(family, L':'))
        {
            *colon = 0; weight = _wtoi(colon + 1);
            if (wchar_t *second = wcschr(colon + 1, L':')) opsz = (float)_wtof(second + 1);
        }
        if (opsz > 0)
        {
            IDWriteFactory6 *dw6; IDWriteTextFormat3 *format3;
            DWRITE_FONT_AXIS_VALUE axes[2] = { { DWRITE_FONT_AXIS_TAG_WEIGHT, (float)weight },
                                               { DWRITE_FONT_AXIS_TAG_OPTICAL_SIZE, opsz } };
            if (FAILED(dw->QueryInterface(IID_PPV_ARGS(&dw6)))) continue;
            HRESULT fhr = dw6->CreateTextFormat(family, nullptr, axes, 2, size, L"en-us", &format3);
            dw6->Release();
            if (FAILED(fhr)) continue;
            format = format3;
        }
        else if (FAILED(dw->CreateTextFormat(family, nullptr, (DWRITE_FONT_WEIGHT)weight, DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &format))) continue;
        dw->CreateTextLayout(text, (UINT32)wcslen(text), format, (float)width, (float)line, &layout);
        rt->DrawTextLayout(D2D1::Point2F(6.0f, 4.0f + i * line), layout, brush);
        DWRITE_TEXT_METRICS m; layout->GetMetrics(&m);
        wprintf(L"%s: width %.2f\n", families[i], m.widthIncludingTrailingWhitespace);
        layout->Release(); format->Release();
    }
    HRESULT hr = rt->EndDraw();

    IWICStream *stream; IWICBitmapEncoder *encoder; IWICBitmapFrameEncode *frame;
    wic->CreateStream(&stream);
    stream->InitializeFromFilename(argv[1], GENERIC_WRITE);
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream, WICBitmapEncoderNoCache);
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    frame->WriteSource(bitmap, nullptr);
    frame->Commit(); encoder->Commit();
    wprintf(L"EndDraw %#lx\n", hr);
    return 0;
}
