// List DirectWrite's system font collection and resolve the families UWP apps ask for.
// Usage: wine probe-fonts.exe [family...]   (default: the Segoe families XAML and Shapr3D use)
#include <windows.h>
#include <dwrite_3.h>
#include <cstdio>

static void family_name(IDWriteFontFamily *family, wchar_t *out, UINT32 size)
{
    IDWriteLocalizedStrings *names;
    out[0] = 0;
    if (SUCCEEDED(family->GetFamilyNames(&names)))
    {
        names->GetString(0, out, size);
        names->Release();
    }
}

int wmain(int argc, wchar_t **argv)
{
    static const wchar_t *defaults[] = { L"Segoe UI", L"Segoe UI Variable", L"Segoe Fluent Icons",
                                         L"Segoe MDL2 Assets", L"Segoe UI Symbol" };
    IDWriteFactory *factory;
    IDWriteFontCollection *collection;
    wchar_t name[256];

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory)))
        return 1;
    factory->GetSystemFontCollection(&collection, FALSE);
    UINT32 count = collection->GetFontFamilyCount();
    wprintf(L"%u families:", count);
    for (UINT32 i = 0; i < count; i++)
    {
        IDWriteFontFamily *family;
        if (FAILED(collection->GetFontFamily(i, &family))) continue;
        family_name(family, name, 256);
        wprintf(L" [%s]", name);
        family->Release();
    }
    wprintf(L"\n");

    int n = argc > 1 ? argc - 1 : (int)(sizeof(defaults) / sizeof(defaults[0]));
    for (int i = 0; i < n; i++)
    {
        const wchar_t *want = argc > 1 ? argv[i + 1] : defaults[i];
        UINT32 index;
        BOOL exists = FALSE;
        collection->FindFamilyName(want, &index, &exists);
        wprintf(L"%s -> %s\n", want, exists ? L"found" : L"MISSING");
        IDWriteFontFamily *family;
        if (exists && SUCCEEDED(collection->GetFontFamily(index, &family)))
        {
            for (UINT32 f = 0; f < family->GetFontCount(); f++)
            {
                IDWriteFont *font; IDWriteLocalizedStrings *faces; wchar_t face[128] = L"";
                if (FAILED(family->GetFont(f, &font))) continue;
                if (SUCCEEDED(font->GetFaceNames(&faces))) { faces->GetString(0, face, 128); faces->Release(); }
                wprintf(L"    %-24s weight %u stretch %u style %u\n", face, font->GetWeight(), font->GetStretch(), font->GetStyle());
                font->Release();
            }
            family->Release();
        }
    }
    collection->Release();
    factory->Release();
    return 0;
}
