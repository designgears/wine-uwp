// How long does reading back N small A8 textures take, and which step costs it? dcomp reads XAML's glyph and
// nine-grid masks back through one staging atlas; with Shapr3D's View menu (67 masks) the Map took 31 ms, about
// 0.45 ms a surface, whether the masks were copied straight into the staging atlas or gathered on the GPU first.
// Usage: probe-a8perf [count] [bgra]
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>

static double ms( LARGE_INTEGER a, LARGE_INTEGER b )
{
    LARGE_INTEGER f; QueryPerformanceFrequency( &f );
    return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}

int main( int argc, char **argv )
{
    int count = argc > 1 ? atoi( argv[1] ) : 67;
    BOOL bgra = argc > 2;
    DXGI_FORMAT format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_A8_UNORM;
    UINT bpp = bgra ? 4 : 1, w = 40, h = 15;
    ID3D11Device *device; ID3D11DeviceContext *context;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice( NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1,
                                  D3D11_SDK_VERSION, &device, NULL, &context ))) { printf( "no device\n" ); return 1; }
    ID3D11Texture2D **tex = new ID3D11Texture2D *[count];
    D3D11_TEXTURE2D_DESC desc = { w, h, 1, 1, format, { 1, 0 }, D3D11_USAGE_DEFAULT,
                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, 0 };
    D3D11_TEXTURE2D_DESC sdesc = { 64, 64, 1, 1, format, { 1, 0 }, D3D11_USAGE_STAGING, 0,
                                   D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE, 0 };
    ID3D11Texture2D *src; device->CreateTexture2D( &sdesc, NULL, &src );
    for (int round = 0; round < 3; round++)
    {
        LARGE_INTEGER t0, t1, t2, t3, t4;
        D3D11_MAPPED_SUBRESOURCE map;
        QueryPerformanceCounter( &t0 );
        /* fill, as XAML does: write a staging texture, box-copy it in */
        for (int i = 0; i < count; i++)
        {
            if (!round) device->CreateTexture2D( &desc, NULL, &tex[i] );
            context->Map( src, 0, D3D11_MAP_WRITE, 0, &map );
            memset( map.pData, i + round, map.RowPitch * 20 );
            context->Unmap( src, 0 );
            D3D11_BOX box = { 0, 0, 0, w, h, 1 };
            context->CopySubresourceRegion( tex[i], 0, 0, 0, 0, src, 0, &box );
        }
        QueryPerformanceCounter( &t1 );
        D3D11_TEXTURE2D_DESC adesc = { 1024, 256, 1, 1, format, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0 };
        static ID3D11Texture2D *atlas, *gatlas;
        if (!atlas) device->CreateTexture2D( &adesc, NULL, &atlas );
        adesc.Usage = D3D11_USAGE_DEFAULT; adesc.CPUAccessFlags = 0;
        if (!gatlas) device->CreateTexture2D( &adesc, NULL, &gatlas );
        /* straight into the staging atlas */
        for (int i = 0; i < count; i++) context->CopySubresourceRegion( atlas, 0, (i % 25) * w, (i / 25) * h, 0, tex[i], 0, NULL );
        context->Map( atlas, 0, D3D11_MAP_READ, 0, &map ); context->Unmap( atlas, 0 );
        QueryPerformanceCounter( &t2 );
        /* again, nothing new written: is it the copies or the fills? */
        for (int i = 0; i < count; i++) context->CopySubresourceRegion( atlas, 0, (i % 25) * w, (i / 25) * h, 0, tex[i], 0, NULL );
        context->Map( atlas, 0, D3D11_MAP_READ, 0, &map ); context->Unmap( atlas, 0 );
        QueryPerformanceCounter( &t3 );
        /* through the GPU atlas */
        for (int i = 0; i < count; i++) context->CopySubresourceRegion( gatlas, 0, (i % 25) * w, (i / 25) * h, 0, tex[i], 0, NULL );
        context->CopyResource( atlas, gatlas );
        context->Map( atlas, 0, D3D11_MAP_READ, 0, &map ); context->Unmap( atlas, 0 );
        QueryPerformanceCounter( &t4 );
        printf( "round %d, %d %s: fill %.2f ms, readback after fill %.2f ms, readback again %.2f ms, via gpu atlas %.2f ms\n",
                round, count, bgra ? "BGRA" : "A8", ms( t0, t1 ), ms( t1, t2 ), ms( t2, t3 ), ms( t3, t4 ) );
    }
    return 0;
}
