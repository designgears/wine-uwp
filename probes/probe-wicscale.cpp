// Scale an image through IWICBitmapScaler and write a PNG, to judge Wine's interpolation modes.
// Usage: wine probe-wicscale.exe <in> <out.png> <width> <height> [mode]   (mode: 0 nearest .. 4 hq cubic)
#include <windows.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>

int wmain(int argc, wchar_t **argv)
{
    if (argc < 5) { wprintf(L"usage: probe-wicscale in out.png w h [mode]\n"); return 1; }
    UINT w = _wtoi(argv[3]), h = _wtoi(argv[4]);
    auto mode = (WICBitmapInterpolationMode)(argc > 5 ? _wtoi(argv[5]) : WICBitmapInterpolationModeFant);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory *wic; IWICBitmapDecoder *dec; IWICBitmapFrameDecode *frame; IWICBitmapScaler *scaler;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    HRESULT hr = wic->CreateDecoderFromFilename(argv[1], nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) { wprintf(L"decode %#lx\n", hr); return 1; }
    dec->GetFrame(0, &frame);
    wic->CreateBitmapScaler(&scaler);
    hr = scaler->Initialize(frame, w, h, mode);
    wprintf(L"Initialize %#lx\n", hr);
    IWICStream *stream; IWICBitmapEncoder *enc; IWICBitmapFrameEncode *out;
    wic->CreateStream(&stream);
    stream->InitializeFromFilename(argv[2], GENERIC_WRITE);
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(stream, WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&out, nullptr);
    out->Initialize(nullptr);
    hr = out->WriteSource(scaler, nullptr);
    out->Commit(); enc->Commit();
    wprintf(L"WriteSource %#lx\n", hr);
    return 0;
}
