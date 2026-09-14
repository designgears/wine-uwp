// Exercise dcomp's drawing-surface path without the app: XAML asks the interop compositor for a
// surface factory, makes a surface, and draws into it through BeginDraw/EndDraw. This does the
// same thing in a few dozen lines, so the surface work can be tested without winning the
// activation race first. Green means the object BeginDraw hands back is a real D2D device
// context and the pixels it drew come back out of the texture.
#include <windows.h>
#include <initguid.h>
#include <d3d11.h>
#include <d2d1_1.h>
#include <roapi.h>
#include <winstring.h>
#include <stdio.h>

static int failures;
#define CHECK(hr, what) do { HRESULT _hr = (hr); \
    if (FAILED(_hr)) { printf("FAIL %s: %#lx\n", (what), _hr); failures++; return failures; } \
    else printf("ok   %s\n", (what)); } while (0)

/* Windows.UI.Composition.Compositor's private interop factory: slot 6 creates a compositor over
 * an existing rendering device (ICompositorInterop-shaped, see dlls/dcomp). */
DEFINE_GUID(IID_ICompositorFactoryInterop, 0x22118adf, 0x23f1, 0x4801, 0xbc, 0xfa, 0x66, 0xcb, 0xf4, 0x8c, 0xc5, 0x1b);
/* The interop device iid XAML actually asks for (seen in DCompTreeHost::EnsureDCompDevice). */
DEFINE_GUID(IID_IDCompositionDesktopDevice_, 0xd14b6158, 0xc3fa, 0x4bce, 0x9c, 0x1f, 0xb6, 0x1d, 0x86, 0x65, 0xea, 0xb0);

struct CompositorFactoryInteropVtbl
{
    void *unknown[6];
    HRESULT (STDMETHODCALLTYPE *CreateInteropDevice)(void *, DWORD, IUnknown *, REFIID, void **);
};
struct CompositorFactoryInterop { const CompositorFactoryInteropVtbl *lpVtbl; };

/* IDCompositionDevice-shaped slots the dcomp implementation uses. */
struct DeviceVtbl
{
    void *unknown[3];
    HRESULT (STDMETHODCALLTYPE *Commit)(void *);
    void *pad[2];
    HRESULT (STDMETHODCALLTYPE *CreateVisual)(void *, void **);
    HRESULT (STDMETHODCALLTYPE *CreateSurfaceFactory)(void *, IUnknown *, void **);
};
struct Device { const DeviceVtbl *lpVtbl; };

struct SurfaceFactoryVtbl
{
    void *unknown[3];
    HRESULT (STDMETHODCALLTYPE *CreateSurface)(void *, UINT, UINT, DXGI_FORMAT, UINT, void **);
};
struct SurfaceFactory { const SurfaceFactoryVtbl *lpVtbl; };

struct SurfaceVtbl
{
    void *unknown[3];
    HRESULT (STDMETHODCALLTYPE *BeginDraw)(void *, const RECT *, REFIID, void **, POINT *);
    HRESULT (STDMETHODCALLTYPE *EndDraw)(void *);
};
struct Surface { const SurfaceVtbl *lpVtbl; };

/* IDCompositionVisual as dlls/dcomp lays it out: slot 15 SetContent. */
struct VisualVtbl
{
    void *unknown[8];
    HRESULT (STDMETHODCALLTYPE *SetTransform)(void *, const D2D_MATRIX_3X2_F *);   /* slot 8 */
    void *pad[5];
    HRESULT (STDMETHODCALLTYPE *SetClip)(void *, const D2D_RECT_F *);              /* slot 14 */
    HRESULT (STDMETHODCALLTYPE *SetContent)(void *, IUnknown *);                   /* slot 15 */
};
struct Visual { const VisualVtbl *lpVtbl; };

int main(void)
{
    ID3D11Device *d3d = NULL;
    ID3D11DeviceContext *d3d_context = NULL;
    IActivationFactory *factory = NULL;
    CompositorFactoryInterop *interop = NULL;
    Device *device = NULL;
    SurfaceFactory *surface_factory = NULL;
    Surface *surface = NULL;
    ID2D1DeviceContext *d2d = NULL;
    HSTRING name;
    POINT offset = { -1, -1 };
    D2D1_COLOR_F red = { 1.0f, 0.0f, 0.0f, 1.0f };
    const UINT size = 64;

    RoInitialize(RO_INIT_MULTITHREADED);

    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                            NULL, 0, D3D11_SDK_VERSION, &d3d, NULL, &d3d_context),
          "D3D11CreateDevice");

    CHECK(WindowsCreateString(L"Windows.UI.Composition.Compositor", 33, &name), "HSTRING");
    CHECK(RoGetActivationFactory(name, IID_IActivationFactory, (void **)&factory),
          "Compositor activation factory");
    CHECK(factory->QueryInterface(IID_ICompositorFactoryInterop, (void **)&interop),
          "ICompositorFactoryInterop");
    CHECK(interop->lpVtbl->CreateInteropDevice(interop, 0, (IUnknown *)d3d,
                                               IID_IDCompositionDesktopDevice_, (void **)&device),
          "CreateInteropDevice");
    CHECK(device->lpVtbl->CreateSurfaceFactory(device, (IUnknown *)d3d, (void **)&surface_factory),
          "CreateSurfaceFactory");
    CHECK(surface_factory->lpVtbl->CreateSurface(surface_factory, size, size,
                                                 DXGI_FORMAT_B8G8R8A8_UNORM, 1, (void **)&surface),
          "CreateSurface 64x64 B8G8R8A8");

    CHECK(surface->lpVtbl->BeginDraw(surface, NULL, __uuidof(ID2D1DeviceContext),
                                     (void **)&d2d, &offset), "BeginDraw");
    printf("     update offset %ld,%ld\n", offset.x, offset.y);

    /* If BeginDraw handed back a stub rather than a device context, this is where it shows. */
    {
        ID2D1Image *target = NULL;
        d2d->GetTarget(&target);
        if (!target) { printf("FAIL the object from BeginDraw has no D2D target\n"); failures++; }
        else { printf("ok   D2D target %p\n", target); target->Release(); }
    }
    d2d->Clear(&red);
    CHECK(surface->lpVtbl->EndDraw(surface), "EndDraw");

    /* Now the renderer half: put the surface on a visual the way XAML does
     * (DCompVisual::SetContentInternal), give dcomp a window, and read the pixel back. */
    {
        Visual *visual = NULL;
        HWND hwnd;
        HDC hdc;
        COLORREF pixel;
        void (WINAPI *set_target)(HWND) = (void (WINAPI *)(HWND))
            GetProcAddress(GetModuleHandleW(L"dcomp.dll"), "__wine_dcomp_set_target_window");

        CHECK(device->lpVtbl->CreateVisual(device, (void **)&visual), "CreateVisual");
        CHECK(visual->lpVtbl->SetContent(visual, (IUnknown *)surface), "SetContent(surface)");
        if (!set_target) { printf("FAIL no __wine_dcomp_set_target_window export\n"); return ++failures; }

        hwnd = CreateWindowExW(0, L"STATIC", L"probe-surface", WS_POPUP | WS_VISIBLE,
                               0, 0, 200, 200, NULL, NULL, NULL, NULL);
        set_target(hwnd);   /* presents immediately */
        hdc = GetDC(hwnd);
        pixel = GetPixel(hdc, 10, 10);
        ReleaseDC(hwnd, hdc);
        printf("     pixel at 10,10 = %06lx\n", pixel);
        if (pixel == RGB(255, 0, 0)) printf("ok   the drawn red reached the window\n");
        else { printf("FAIL expected red %06lx\n", RGB(255, 0, 0)); failures++; }

        /* Positioning: translate the visual and the red square must move with it. */
        {
            D2D_MATRIX_3X2_F move = { 1.0f, 0.0f, 0.0f, 1.0f, 100.0f, 100.0f };
            COLORREF old_spot, new_spot;

            CHECK(visual->lpVtbl->SetTransform(visual, &move), "SetTransform translate 100,100");
            set_target(hwnd);
            hdc = GetDC(hwnd);
            old_spot = GetPixel(hdc, 10, 10);
            new_spot = GetPixel(hdc, 110, 110);
            ReleaseDC(hwnd, hdc);
            printf("     pixel at 10,10 = %06lx, at 110,110 = %06lx\n", old_spot, new_spot);
            if (new_spot == RGB(255, 0, 0) && old_spot != RGB(255, 0, 0))
                printf("ok   the surface moved with the transform\n");
            else { printf("FAIL the transform did not move the surface\n"); failures++; }
        }

        /* Clipping: keep only the top-left 32x32 of the square, in the visual's own space. */
        {
            D2D_RECT_F clip = { 0.0f, 0.0f, 32.0f, 32.0f };
            COLORREF inside, outside;

            CHECK(visual->lpVtbl->SetClip(visual, &clip), "SetClip 0,0-32,32");
            set_target(hwnd);
            hdc = GetDC(hwnd);
            inside = GetPixel(hdc, 110, 110);
            outside = GetPixel(hdc, 150, 150);
            ReleaseDC(hwnd, hdc);
            printf("     pixel at 110,110 = %06lx, at 150,150 = %06lx\n", inside, outside);
            if (inside == RGB(255, 0, 0) && outside != RGB(255, 0, 0))
                printf("ok   the clip cut the surface\n");
            else { printf("FAIL the clip did not cut the surface\n"); failures++; }
        }

        /* Scale: lift the clip, drop the translation, scale 2x - the 64px square becomes 128px. */
        {
            D2D_RECT_F no_clip = { 0.0f, 0.0f, 1000.0f, 1000.0f };
            D2D_MATRIX_3X2_F twice = { 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f };
            COLORREF inside, outside;

            CHECK(visual->lpVtbl->SetClip(visual, &no_clip), "SetClip lifted");
            CHECK(visual->lpVtbl->SetTransform(visual, &twice), "SetTransform scale 2");
            set_target(hwnd);
            hdc = GetDC(hwnd);
            inside = GetPixel(hdc, 100, 100);    /* outside 64px, inside 128px */
            outside = GetPixel(hdc, 140, 140);   /* outside both */
            ReleaseDC(hwnd, hdc);
            printf("     pixel at 100,100 = %06lx, at 140,140 = %06lx\n", inside, outside);
            if (inside == RGB(255, 0, 0) && outside != RGB(255, 0, 0))
                printf("ok   the surface scaled with the transform\n");
            else { printf("FAIL the scale did not stretch the surface\n"); failures++; }
        }
        DestroyWindow(hwnd);
    }

    printf(failures ? "\nRED: %d check(s) failed\n" : "\nGREEN: the surface draws and reaches the window\n", failures);
    return failures;
}
