// Put pixels into a 9x9 DXGI_FORMAT_A8_UNORM texture three ways - ClearRenderTargetView, UpdateSubresource and a
// D2D fill through a DXGI surface bitmap - and read each back through a staging copy, as dcomp does. XAML's
// nine-grid masks are 9x9 A8 surfaces it fills itself; under Wine dcomp read 126 of them back empty 120 times each.
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <stdio.h>

static ID3D11Device *device;
static ID3D11DeviceContext *context;

static ID3D11Texture2D *make_a8( UINT bind )
{
    D3D11_TEXTURE2D_DESC desc = { 9, 9, 1, 1, DXGI_FORMAT_A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, bind, 0, 0 };
    ID3D11Texture2D *texture = NULL;
    HRESULT hr = device->CreateTexture2D( &desc, NULL, &texture );
    if (FAILED(hr)) printf( "CreateTexture2D(A8, bind %#x) failed %#lx\n", bind, hr );
    return texture;
}

static void dump( const char *what, ID3D11Texture2D *texture )
{
    D3D11_TEXTURE2D_DESC desc;
    ID3D11Texture2D *staging;
    D3D11_MAPPED_SUBRESOURCE map;

    texture->GetDesc( &desc );
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    if (FAILED(device->CreateTexture2D( &desc, NULL, &staging ))) { printf( "%s: no staging\n", what ); return; }
    context->CopyResource( staging, texture );
    if (FAILED(context->Map( staging, 0, D3D11_MAP_READ, 0, &map ))) { printf( "%s: Map failed\n", what ); return; }
    printf( "%-28s", what );
    for (UINT y = 0; y < 9; y += 4)
    {
        printf( " row %u:", y );
        for (UINT x = 0; x < 9; x++) printf( " %02x", ((BYTE *)map.pData)[y * map.RowPitch + x] );
    }
    printf( "\n" );
    context->Unmap( staging, 0 );
    staging->Release();
}

int main( void )
{
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                    D3D11_SDK_VERSION, &device, NULL, &context );
    if (FAILED(hr)) { printf( "D3D11CreateDevice failed %#lx\n", hr ); return 1; }

    /* 1: clear */
    if (ID3D11Texture2D *texture = make_a8( D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE ))
    {
        ID3D11RenderTargetView *rtv;
        const float colour[4] = { 0.0f, 0.0f, 0.0f, 0.5f };
        if (SUCCEEDED(device->CreateRenderTargetView( texture, NULL, &rtv )))
        {
            context->ClearRenderTargetView( rtv, colour );
            dump( "clear alpha 0.5 (expect 80)", texture );
            rtv->Release();
        }
        else printf( "CreateRenderTargetView(A8) failed\n" );
        texture->Release();
    }

    /* 2: CPU upload */
    if (ID3D11Texture2D *texture = make_a8( D3D11_BIND_SHADER_RESOURCE ))
    {
        BYTE pixels[9 * 9];
        for (int i = 0; i < 81; i++) pixels[i] = (BYTE)(i * 3);
        context->UpdateSubresource( texture, 0, NULL, pixels, 9, 0 );
        dump( "UpdateSubresource (0,3,6..)", texture );
        texture->Release();
    }

    /* 2b: what XAML does for its nine-grid masks: fill a CPU-writable staging A8 texture, copy a box of it in */
    if (ID3D11Texture2D *texture = make_a8( D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE ))
    {
        D3D11_TEXTURE2D_DESC desc = { 13, 13, 1, 1, DXGI_FORMAT_A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0,
                                      D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0 };
        ID3D11Texture2D *staging;
        D3D11_MAPPED_SUBRESOURCE map;
        D3D11_BOX box = { 1, 1, 0, 10, 10, 1 };

        if (SUCCEEDED(device->CreateTexture2D( &desc, NULL, &staging )) &&
            SUCCEEDED(context->Map( staging, 0, D3D11_MAP_WRITE, 0, &map )))
        {
            for (UINT y = 0; y < 13; y++)
                for (UINT x = 0; x < 13; x++) ((BYTE *)map.pData)[y * map.RowPitch + x] = (BYTE)(0x10 * y + x);
            context->Unmap( staging, 0 );
            context->CopySubresourceRegion( texture, 0, 0, 0, 0, staging, 0, &box );
            dump( "staging box copy (11,12..)", texture );
            staging->Release();
        }
        else printf( "staging A8 failed\n" );
        texture->Release();
    }

    /* 3: D2D, the way a surface's BeginDraw target is made */
    if (ID3D11Texture2D *texture = make_a8( D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE ))
    {
        ID2D1Factory1 *factory;
        IDXGIDevice *dxgi;
        ID2D1Device *d2d;
        ID2D1DeviceContext *dc;
        IDXGISurface *surface;
        ID2D1Bitmap1 *bitmap;
        ID2D1SolidColorBrush *brush;
        D2D1_BITMAP_PROPERTIES1 props = { { DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED }, 96, 96,
                                          D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW, NULL };

        D2D1CreateFactory( D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), NULL, (void **)&factory );
        device->QueryInterface( __uuidof(IDXGIDevice), (void **)&dxgi );
        factory->CreateDevice( dxgi, &d2d );
        d2d->CreateDeviceContext( D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc );
        texture->QueryInterface( __uuidof(IDXGISurface), (void **)&surface );
        hr = dc->CreateBitmapFromDxgiSurface( surface, &props, &bitmap );
        if (FAILED(hr)) printf( "CreateBitmapFromDxgiSurface(A8) failed %#lx\n", hr );
        else
        {
            dc->SetTarget( bitmap );
            dc->CreateSolidColorBrush( D2D1::ColorF( 0, 0, 0, 1.0f ), &brush );
            dc->BeginDraw();
            dc->Clear( D2D1::ColorF( 0, 0, 0, 0.0f ) );
            dc->FillRectangle( D2D1::RectF( 2, 2, 7, 7 ), brush );
            hr = dc->EndDraw();
            printf( "D2D EndDraw %#lx\n", hr );
            dump( "D2D fill 2..7 (expect ff)", texture );
            dc->BeginDraw();
            dc->Clear( D2D1::ColorF( 0, 0, 0, 1.0f ) );
            hr = dc->EndDraw();
            dump( "D2D Clear alpha 1 (expect ff)", texture );
        }
        texture->Release();
    }
    return 0;
}
