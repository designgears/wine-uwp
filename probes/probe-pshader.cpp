// probe-pshader: run a pixel shader blob (as an app hands Direct2D a custom effect's shader) over an 8x4
// B8G8R8A8 target with Direct2D's effect vertex interface (SV_POSITION, SCENE_POSITION, TEXCOORD0-7) and print what
// it wrote. Tells a shader translation problem from a Direct2D one.
//
//   probe-pshader.exe SHADER.bin [cb floats...]
//   probe-pshader.exe --hlsl 'return float4(k.y, k.y, k.y, k.x);' [cb floats...]
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>

static const char vs_code[] =
    "struct o { float4 p : SV_POSITION; float4 scene : SCENE_POSITION;\n"
    "    float4 t0 : TEXCOORD0; float4 t1 : TEXCOORD1; float4 t2 : TEXCOORD2; float4 t3 : TEXCOORD3;\n"
    "    float4 t4 : TEXCOORD4; float4 t5 : TEXCOORD5; float4 t6 : TEXCOORD6; float4 t7 : TEXCOORD7; };\n"
    "o main(uint id : SV_VertexID)\n"
    "{\n"
    "    o r; float2 q = float2(id & 1, id >> 1), s = q * float2(8.0, 4.0);\n"
    "    r.p = float4(q.x * 2.0 - 1.0, 1.0 - q.y * 2.0, 0.0, 1.0);\n"
    "    r.scene = float4(s, 0.0, 0.0);\n"
    "    r.t0 = r.t1 = r.t2 = r.t3 = r.t4 = r.t5 = r.t6 = r.t7 = float4(q, 0.125, 0.25);\n"
    "    return r;\n"
    "}\n";

static ID3DBlob *compile(const std::string &code, const char *profile)
{
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = D3DCompile(code.data(), code.size(), "probe", nullptr, nullptr, "main", profile, 0, 0, &blob, &errors);
    if (FAILED(hr))
    {
        printf("compile %s failed %#lx: %s\n", profile, hr, errors ? (const char *)errors->GetBufferPointer() : "");
        exit(1);
    }
    return blob;
}

int main(int argc, char **argv)
{
    std::vector<char> ps_code;
    int first_cb = 2;

    if (argc < 2)
    {
        printf("usage: probe-pshader SHADER.bin|--hlsl BODY [cb floats...]\n");
        return 1;
    }
    if (!strcmp(argv[1], "--hlsl"))
    {
        std::string src = "cbuffer c : register(b0) { float4 k; }\n"
            "float4 main(float4 p : SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target { ";
        src += argv[2];
        src += " }\n";
        ID3DBlob *blob = compile(src, "ps_4_0");
        ps_code.assign((char *)blob->GetBufferPointer(), (char *)blob->GetBufferPointer() + blob->GetBufferSize());
        first_cb = 3;
    }
    else
    {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { printf("cannot open %s\n", argv[1]); return 1; }
        char buf[65536];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        ps_code.assign(buf, buf + n);
    }
    float cb[16] = {0};
    for (int i = first_cb; i < argc && i - first_cb < 16; i++) cb[i - first_cb] = (float)atof(argv[i]);

    ID3D11Device *device;
    ID3D11DeviceContext *context;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device, nullptr, &context);
    if (FAILED(hr)) { printf("D3D11CreateDevice %#lx\n", hr); return 1; }

    ID3DBlob *vs_blob = compile(vs_code, "vs_4_0");
    ID3D11VertexShader *vs;
    ID3D11PixelShader *ps;
    if (FAILED(hr = device->CreateVertexShader(vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs)))
    { printf("CreateVertexShader %#lx\n", hr); return 1; }
    if (FAILED(hr = device->CreatePixelShader(ps_code.data(), ps_code.size(), nullptr, &ps)))
    { printf("CreatePixelShader %#lx\n", hr); return 1; }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 8; desc.Height = 4; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ID3D11Texture2D *target, *staging;
    device->CreateTexture2D(&desc, nullptr, &target);
    desc.Usage = D3D11_USAGE_STAGING; desc.BindFlags = 0; desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&desc, nullptr, &staging);
    ID3D11RenderTargetView *rtv;
    device->CreateRenderTargetView(target, nullptr, &rtv);

    D3D11_BUFFER_DESC bd = { sizeof(cb), D3D11_USAGE_IMMUTABLE, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
    D3D11_SUBRESOURCE_DATA data = { cb, 0, 0 };
    ID3D11Buffer *buffer;
    device->CreateBuffer(&bd, &data, &buffer);

    D3D11_VIEWPORT vp = { 0, 0, 8, 4, 0, 1 };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context->VSSetShader(vs, nullptr, 0);
    context->PSSetShader(ps, nullptr, 0);
    context->PSSetConstantBuffers(0, 1, &buffer);
    context->RSSetViewports(1, &vp);
    context->Draw(4, 0);
    context->CopyResource(staging, target);

    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &map))) { printf("Map %#lx\n", hr); return 1; }
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            const BYTE *p = (const BYTE *)map.pData + y * map.RowPitch + x * 4;
            printf(" r%3u g%3u b%3u a%3u", p[2], p[1], p[0], p[3]);
            if (x == 3) printf("\n");
        }
        printf("\n");
    }
    context->Unmap(staging, 0);
    return 0;
}
