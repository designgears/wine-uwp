// probe-glyph: does the font a UWP app asks for actually have a glyph for a given character, and
// if not, does DWrite's fallback find one? Feedback Hub's "(c) 2026 Microsoft Corp." renders the
// copyright sign as a missing-glyph box, and the question is whether the family resolves, whether
// its cmap has the character, and what MapCharacters falls back to.
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>

struct source : IDWriteTextAnalysisSource
{
    const WCHAR *text; UINT32 len; LONG ref;
    source( const WCHAR *t, UINT32 l ) : text(t), len(l), ref(1) {}
    HRESULT STDMETHODCALLTYPE QueryInterface( REFIID iid, void **out ) override
    {
        if (IsEqualGUID( iid, __uuidof(IUnknown) ) || IsEqualGUID( iid, __uuidof(IDWriteTextAnalysisSource) ))
        { *out = this; AddRef(); return S_OK; }
        *out = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement( &ref ); }
    ULONG STDMETHODCALLTYPE Release() override { return InterlockedDecrement( &ref ); }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition( UINT32 pos, const WCHAR **str, UINT32 *length ) override
    { if (pos >= len) { *str = nullptr; *length = 0; } else { *str = text + pos; *length = len - pos; } return S_OK; }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition( UINT32 pos, const WCHAR **str, UINT32 *length ) override
    { if (!pos || pos > len) { *str = nullptr; *length = 0; } else { *str = text; *length = pos; } return S_OK; }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override
    { return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT; }
    HRESULT STDMETHODCALLTYPE GetLocaleName( UINT32 pos, UINT32 *length, const WCHAR **locale ) override
    { *length = len - pos; *locale = L"en-us"; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution( UINT32 pos, UINT32 *length, IDWriteNumberSubstitution **sub ) override
    { *length = len - pos; *sub = nullptr; return S_OK; }
};

static void check( IDWriteFactory2 *factory, IDWriteFontCollection *sysfonts, IDWriteFontFallback *fallback,
                   const WCHAR *family, UINT32 codepoint )
{
    UINT32 index = 0; BOOL exists = FALSE;
    wprintf( L"  %-28ls U+%04X: ", family, codepoint );
    if (FAILED(sysfonts->FindFamilyName( family, &index, &exists )) || !exists)
        wprintf( L"family not found; " );
    else
    {
        IDWriteFontFamily *fam = nullptr; IDWriteFont *font = nullptr; IDWriteFontFace *face = nullptr;
        sysfonts->GetFontFamily( index, &fam );
        fam->GetFirstMatchingFont( DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                   DWRITE_FONT_STYLE_NORMAL, &font );
        BOOL has = FALSE;
        font->HasCharacter( codepoint, &has );
        font->CreateFontFace( &face );
        UINT16 glyph = 0;
        if (face) face->GetGlyphIndices( &codepoint, 1, &glyph );
        wprintf( L"HasCharacter %d glyph %u; ", has, glyph );
        if (face) face->Release();
        if (font) font->Release();
        if (fam) fam->Release();
    }

    WCHAR text[2] = { (WCHAR)codepoint, 0 };
    source src( text, 1 );
    UINT32 mapped = 0; IDWriteFont *mapped_font = nullptr; FLOAT scale = 0;
    HRESULT hr = fallback->MapCharacters( &src, 0, 1, sysfonts, family, DWRITE_FONT_WEIGHT_NORMAL,
                                          DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                          &mapped, &mapped_font, &scale );
    wprintf( L"MapCharacters hr 0x%08lx mapped %u font %p", hr, mapped, mapped_font );
    if (mapped_font)
    {
        IDWriteFontFamily *fam = nullptr; IDWriteLocalizedStrings *names = nullptr;
        if (SUCCEEDED(mapped_font->GetFontFamily( &fam )) && SUCCEEDED(fam->GetFamilyNames( &names )))
        {
            WCHAR name[128] = L"?";
            names->GetString( 0, name, ARRAYSIZE(name) );
            wprintf( L" -> %ls", name );
            names->Release();
        }
        if (fam) fam->Release();
        mapped_font->Release();
    }
    wprintf( L"\n" );
}

int wmain( void )
{
    IDWriteFactory2 *factory = nullptr;
    HRESULT hr = DWriteCreateFactory( DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), (IUnknown **)&factory );
    wprintf( L"DWriteCreateFactory -> 0x%08lx\n", hr );
    if (FAILED(hr)) return 1;

    IDWriteFontCollection *sysfonts = nullptr;
    IDWriteFontFallback *fallback = nullptr;
    factory->GetSystemFontCollection( &sysfonts, FALSE );
    factory->GetSystemFontFallback( &fallback );
    wprintf( L"system collection %p, fallback %p, families %u\n", sysfonts, fallback,
             sysfonts ? sysfonts->GetFontFamilyCount() : 0 );
    if (!sysfonts || !fallback) return 1;

    /* PROBE_GLYPH_LIST=1 dumps every family DWrite can see, which is how you find out whether Wine
     * picked up a font file at all. */
    WCHAR var[8];
    if (GetEnvironmentVariableW( L"PROBE_GLYPH_LIST", var, ARRAYSIZE(var) ))
    {
        UINT32 count = sysfonts->GetFontFamilyCount();
        for (UINT32 i = 0; i < count; i++)
        {
            IDWriteFontFamily *fam = nullptr; IDWriteLocalizedStrings *names = nullptr;
            WCHAR name[128] = L"?";
            if (SUCCEEDED(sysfonts->GetFontFamily( i, &fam )) && SUCCEEDED(fam->GetFamilyNames( &names )))
                names->GetString( 0, name, ARRAYSIZE(name) );
            wprintf( L"family %u: %ls\n", i, name );
            if (names) names->Release();
            if (fam) fam->Release();
        }
    }

    static const WCHAR *families[] = { L"Segoe UI", L"Segoe UI Variable", L"Segoe UI Variable Text",
                                       L"Segoe UI Variable Small", L"Segoe UI Variable Display" };
    static const UINT32 chars[] = { 0x00a9 /* copyright */, 0x0041 /* A */, 0x2122 /* trademark */ };
    for (auto f : families)
        for (auto c : chars)
            check( factory, sysfonts, fallback, f, c );

    fallback->Release();
    sysfonts->Release();
    factory->Release();
    return 0;
}
