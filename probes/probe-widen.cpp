// probe-widen: ID2D1Geometry::Widen for a rectangle and a rounded rectangle, checked by hit-testing the result.
// The band of a 100x60 shape at (10,10) widened by 8 must contain points on the outline and none at the centre or
// well outside; a rounded rectangle's corner point outside its rounded outline must stay outside.
#include <windows.h>
#include <d2d1.h>
#include <cstdio>

static int failures;

static void expect(ID2D1Geometry *g, float x, float y, BOOL want, const char *what)
{
    BOOL got = FALSE;
    HRESULT hr = g->FillContainsPoint(D2D1::Point2F(x, y), nullptr, 0.1f, &got);
    printf("%-28s (%6.1f,%6.1f) %s%s\n", what, x, y, got ? "inside" : "outside",
           SUCCEEDED(hr) && got == want ? "" : "   <-- WRONG");
    if (FAILED(hr) || got != want) failures++;
}

static ID2D1PathGeometry *widen(ID2D1Factory *factory, ID2D1Geometry *source, float width)
{
    ID2D1PathGeometry *path = nullptr;
    ID2D1GeometrySink *sink = nullptr;
    factory->CreatePathGeometry(&path);
    path->Open(&sink);
    HRESULT hr = source->Widen(width, nullptr, nullptr, 0.25f, sink);
    sink->Close();
    sink->Release();
    printf("Widen -> %#lx\n", hr);
    if (FAILED(hr)) failures++;
    return path;
}

int main()
{
    ID2D1Factory *factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) return 1;

    ID2D1RectangleGeometry *rect;
    factory->CreateRectangleGeometry(D2D1::RectF(10, 10, 110, 70), &rect);
    ID2D1PathGeometry *band = widen(factory, rect, 8);
    expect(band, 10, 40, TRUE, "rect: left edge");
    expect(band, 60, 13, TRUE, "rect: inside top band");
    expect(band, 60, 40, FALSE, "rect: centre");
    expect(band, 60, 80, FALSE, "rect: well below");
    expect(band, 7, 7, TRUE, "rect: miter corner");
    band->Release();

    ID2D1RoundedRectangleGeometry *rounded;
    factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(10, 10, 110, 70), 20, 20), &rounded);
    band = widen(factory, rounded, 8);
    expect(band, 60, 10, TRUE, "rounded: top edge");
    expect(band, 60, 40, FALSE, "rounded: centre");
    expect(band, 8, 8, FALSE, "rounded: outside corner");
    expect(band, 15.9f, 15.9f, TRUE, "rounded: on corner arc");
    band->Release();

    printf("%s\n", failures ? "FAILED" : "PASSED");
    return failures;
}
