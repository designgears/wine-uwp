// Exercise IDWritePrivateTextAnalyzer the way XAML's DWriteFontAndScriptServices does: create a
// text analyzer, QI it for the private IID, and call its only method, GetContentReadingDirection.
// Wine used to answer that IID with the public analyzer, so this call landed on AnalyzeScript and
// crashed dereferencing the direction out parameter as a sink. Green means every case below
// returns what Windows 11's DWrite.dll does.
#include <windows.h>
#include <initguid.h>
#include <dwrite_3.h>
#include <stdio.h>

static int failures;

DEFINE_GUID(IID_IDWritePrivateTextAnalyzer, 0xb7e6163e, 0x7f46, 0x43b4, 0x84, 0xb3, 0xe4, 0xe6, 0x24, 0x9c, 0x36, 0x5e);

struct PrivateTextAnalyzerVtbl
{
    void *unknown[3];
    HRESULT (STDMETHODCALLTYPE *GetContentReadingDirection)(void *, IDWriteTextAnalysisSource *, UINT32, UINT32,
            DWRITE_READING_DIRECTION *, BOOL *);
};
struct PrivateTextAnalyzer { const PrivateTextAnalyzerVtbl *lpVtbl; };

/* IDWriteTextFormatDebug: XAML's CTextBlock::ConfigureDWriteTextLayout QIs its default text
 * format for this and calls SetLocaleNameList (slot 11); an E_NOINTERFACE fails the measure. */
DEFINE_GUID(IID_IDWriteTextFormatDebug, 0x28d5197f, 0x3940, 0x4535, 0xb3, 0x15, 0xe3, 0x27, 0xe1, 0x3d, 0x84, 0x5b);

struct TextFormatDebugVtbl
{
    void *unknown[3];
    HRESULT (STDMETHODCALLTYPE *SetForceSlowPath)(void *, BOOL);
    BOOL (STDMETHODCALLTYPE *GetForceSlowPath)(void *);
    HRESULT (STDMETHODCALLTYPE *SetForceReshape)(void *, BOOL);
    BOOL (STDMETHODCALLTYPE *GetForceReshape)(void *);
    HRESULT (STDMETHODCALLTYPE *SetDisableFontFallback)(void *, BOOL);
    BOOL (STDMETHODCALLTYPE *GetDisableFontFallback)(void *);
    UINT32 (STDMETHODCALLTYPE *GetLocaleNameListLength)(void *);
    HRESULT (STDMETHODCALLTYPE *GetLocaleNameList)(void *, WCHAR *, UINT32);
    HRESULT (STDMETHODCALLTYPE *SetLocaleNameList)(void *, const WCHAR *);
};
struct TextFormatDebug { const TextFormatDebugVtbl *lpVtbl; };

class Source : public IDWriteTextAnalysisSource
{
public:
    Source(const WCHAR *text, DWRITE_READING_DIRECTION paragraph) : text(text), paragraph(paragraph) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **obj) override
    {
        if (riid == IID_IUnknown || riid == __uuidof(IDWriteTextAnalysisSource)) { *obj = this; return S_OK; }
        *obj = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 pos, const WCHAR **out, UINT32 *len) override
    {
        UINT32 n = (UINT32)wcslen(text);
        *out = pos < n ? text + pos : nullptr; *len = pos < n ? n - pos : 0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 pos, const WCHAR **out, UINT32 *len) override
    {
        *out = pos ? text : nullptr; *len = pos; return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override { return paragraph; }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32, UINT32 *len, const WCHAR **name) override
    {
        *len = 0; *name = L""; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32, UINT32 *len, IDWriteNumberSubstitution **s) override
    {
        *len = 0; *s = nullptr; return S_OK;
    }
private:
    const WCHAR *text;
    DWRITE_READING_DIRECTION paragraph;
};

static void check(PrivateTextAnalyzer *analyzer, const char *what, const WCHAR *text,
        DWRITE_READING_DIRECTION paragraph, int want_dir, BOOL want_ambiguous)
{
    Source source(text, paragraph);
    DWRITE_READING_DIRECTION dir = (DWRITE_READING_DIRECTION)0xdead;
    BOOL ambiguous = 0xdead;
    HRESULT hr = analyzer->lpVtbl->GetContentReadingDirection(analyzer, &source, 0, (UINT32)wcslen(text),
            &dir, &ambiguous);
    if (FAILED(hr) || (int)dir != want_dir || ambiguous != want_ambiguous)
    {
        printf("FAIL %s: hr %#lx dir %d ambiguous %d, want dir %d ambiguous %d\n", what, hr, (int)dir,
                ambiguous, want_dir, want_ambiguous);
        failures++;
    }
    else printf("ok   %s: dir %d ambiguous %d\n", what, (int)dir, ambiguous);
}

int main()
{
    IDWriteFactory *factory;
    IDWriteTextAnalyzer *analyzer;
    PrivateTextAnalyzer *priv;
    HRESULT hr;

    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory)))
        { printf("FAIL DWriteCreateFactory: %#lx\n", hr); return 1; }
    if (FAILED(hr = factory->CreateTextAnalyzer(&analyzer)))
        { printf("FAIL CreateTextAnalyzer: %#lx\n", hr); return 1; }
    if (FAILED(hr = analyzer->QueryInterface(IID_IDWritePrivateTextAnalyzer, (void **)&priv)))
        { printf("FAIL QI IDWritePrivateTextAnalyzer: %#lx\n", hr); return 1; }
    printf("ok   QI IDWritePrivateTextAnalyzer\n");
    if ((void *)priv == (void *)analyzer)
        { printf("FAIL the private interface is the public analyzer's vtable\n"); failures++; }

    const DWRITE_READING_DIRECTION ltr = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    const DWRITE_READING_DIRECTION rtl = DWRITE_READING_DIRECTION_RIGHT_TO_LEFT;
    check(priv, "latin text", L"Calculator", rtl, ltr, FALSE);
    check(priv, "hebrew text", L"\x05e9\x05dc\x05d5\x05dd", ltr, rtl, FALSE);
    check(priv, "digits then arabic", L"12 \x0627\x0628", ltr, rtl, FALSE);
    check(priv, "neutral only, ltr paragraph", L"123 + 4", ltr, ltr, TRUE);
    check(priv, "neutral only, rtl paragraph", L"123 + 4", rtl, rtl, TRUE);

    DWRITE_READING_DIRECTION dir;
    BOOL ambiguous;
    hr = priv->lpVtbl->GetContentReadingDirection(priv, nullptr, 0, 1, &dir, &ambiguous);
    if (hr != E_INVALIDARG) { printf("FAIL NULL source: %#lx\n", hr); failures++; }
    else printf("ok   NULL source is E_INVALIDARG\n");

    IDWriteTextFormat *format;
    TextFormatDebug *debug;
    if (FAILED(hr = factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 15.0f, L"en-us", &format)))
        { printf("FAIL CreateTextFormat: %#lx\n", hr); return 1; }
    if (FAILED(hr = format->QueryInterface(IID_IDWriteTextFormatDebug, (void **)&debug)))
        { printf("FAIL QI IDWriteTextFormatDebug: %#lx\n", hr); failures++; }
    else
    {
        WCHAR list[32];
        printf("ok   QI IDWriteTextFormatDebug\n");
        hr = debug->lpVtbl->SetLocaleNameList(debug, L"en-US;de-DE");
        UINT32 len = debug->lpVtbl->GetLocaleNameListLength(debug);
        HRESULT get = debug->lpVtbl->GetLocaleNameList(debug, list, ARRAYSIZE(list));
        if (FAILED(hr) || len != 11 || FAILED(get) || wcscmp(list, L"en-US;de-DE"))
            { printf("FAIL SetLocaleNameList round trip: hr %#lx len %u get %#lx\n", hr, len, get); failures++; }
        else printf("ok   SetLocaleNameList round trip\n");
        if (debug->lpVtbl->GetLocaleNameList(debug, list, 4) != E_NOT_SUFFICIENT_BUFFER)
            { printf("FAIL short buffer is not E_NOT_SUFFICIENT_BUFFER\n"); failures++; }
        else printf("ok   short buffer is E_NOT_SUFFICIENT_BUFFER\n");
        debug->lpVtbl->SetDisableFontFallback(debug, TRUE);
        if (!debug->lpVtbl->GetDisableFontFallback(debug))
            { printf("FAIL DisableFontFallback did not stick\n"); failures++; }
        else printf("ok   DisableFontFallback round trip\n");
    }

    /* IDWriteTextLayout4::SetFontAxisValues: XAML's CTextBlock::ConfigureDWriteTextLayout sets the font axes of
     * the whole text and fail-fasts on E_NOTIMPL. */
    {
        IDWriteFactory5 *factory5;
        IDWriteTextLayout *layout;
        IDWriteTextLayout4 *layout4;
        static const WCHAR text[] = L"Calculator";
        DWRITE_FONT_AXIS_VALUE axes[2] = { { DWRITE_FONT_AXIS_TAG_WEIGHT, 700.0f }, { DWRITE_FONT_AXIS_TAG_WIDTH, 100.0f } };
        DWRITE_FONT_AXIS_VALUE out[4];
        DWRITE_TEXT_RANGE range;

        if (FAILED(hr = factory->CreateTextLayout(text, ARRAYSIZE(text) - 1, format, 200.0f, 50.0f, &layout)))
            { printf("FAIL CreateTextLayout: %#lx\n", hr); failures++; }
        else if (FAILED(hr = layout->QueryInterface(&layout4)))
            { printf("FAIL QI IDWriteTextLayout4: %#lx\n", hr); failures++; layout->Release(); }
        else
        {
            DWRITE_TEXT_RANGE middle = { 2, 3 };
            hr = layout4->SetFontAxisValues(axes, 2, middle);
            if (FAILED(hr)) { printf("FAIL SetFontAxisValues: %#lx\n", hr); failures++; }
            else printf("ok   SetFontAxisValues on 2..5\n");

            UINT32 inside = layout4->GetFontAxisValueCount(3), outside = layout4->GetFontAxisValueCount(0);
            if (inside != 2 || outside != 0)
                { printf("FAIL axis counts inside %u outside %u, want 2 and 0\n", inside, outside); failures++; }
            else printf("ok   axis counts: 2 inside, 0 outside\n");

            hr = layout4->GetFontAxisValues(3, out, ARRAYSIZE(out), &range);
            if (FAILED(hr) || range.startPosition != 2 || range.length != 3 ||
                out[0].axisTag != DWRITE_FONT_AXIS_TAG_WEIGHT || out[0].value != 700.0f || out[1].value != 100.0f)
                { printf("FAIL GetFontAxisValues: hr %#lx range %u+%u\n", hr, range.startPosition, range.length); failures++; }
            else printf("ok   GetFontAxisValues round trip, range 2+3\n");

            if (layout4->GetFontAxisValues(3, out, 1, &range) != E_NOT_SUFFICIENT_BUFFER)
                { printf("FAIL short axis buffer is not E_NOT_SUFFICIENT_BUFFER\n"); failures++; }
            else printf("ok   short axis buffer is E_NOT_SUFFICIENT_BUFFER\n");

            layout4->Release();
            layout->Release();
        }
        (void)factory5;
    }

    printf(failures ? "RED: %d failures\n" : "GREEN: the DirectWrite calls XAML text needs behave like Windows\n", failures);
    return failures;
}
