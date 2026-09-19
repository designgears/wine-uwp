// probe-langs: exercise Windows.Globalization.ApplicationLanguages member by member.
// Answers: which language API is the one that fails - Feedback Hub throws 0x80070032 out of
// Bcp47Langs.dll right after activating this class, and the app's stack says nothing else.
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <cstdio>

struct __declspec(uuid("75b40847-0a4c-4a92-9565-fd63c95f7aed")) IApplicationLanguagesStatics : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_PrimaryLanguageOverride( HSTRING *value ) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_PrimaryLanguageOverride( HSTRING value ) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Languages( IInspectable **value ) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_ManifestLanguages( IInspectable **value ) = 0;
};

/* IVectorView<HSTRING> - only the two members we need, after IInspectable */
struct IVectorViewHSTRING : public IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt( UINT32 index, HSTRING *item ) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size( UINT32 *size ) = 0;
};

static void dump_vector( const wchar_t *what, IInspectable *insp )
{
    IVectorViewHSTRING *view = (IVectorViewHSTRING *)insp;
    UINT32 size = 0;
    HRESULT hr;

    if (!insp) { wprintf( L"     %ls: (null)\n", what ); return; }
    hr = view->get_Size( &size );
    wprintf( L"     %ls: get_Size -> 0x%08lx, %u\n", what, hr, size );
    for (UINT32 i = 0; i < size && i < 8; ++i)
    {
        HSTRING s = nullptr;
        hr = view->GetAt( i, &s );
        wprintf( L"       [%u] 0x%08lx %ls\n", i, hr, s ? WindowsGetStringRawBuffer( s, nullptr ) : L"?" );
        if (s) WindowsDeleteString( s );
    }
}

int wmain( void )
{
    HRESULT hr = RoInitialize( RO_INIT_MULTITHREADED );
    wprintf( L"RoInitialize -> 0x%08lx\n", hr );

    HSTRING cls;
    const wchar_t *name = L"Windows.Globalization.ApplicationLanguages";
    WindowsCreateString( name, (UINT32)wcslen( name ), &cls );
    IApplicationLanguagesStatics *statics = nullptr;
    hr = RoGetActivationFactory( cls, __uuidof(IApplicationLanguagesStatics), (void **)&statics );
    wprintf( L"RoGetActivationFactory(ApplicationLanguages) -> 0x%08lx\n", hr );
    WindowsDeleteString( cls );
    if (FAILED(hr)) return 1;

    HSTRING over = nullptr;
    hr = statics->get_PrimaryLanguageOverride( &over );
    wprintf( L"get_PrimaryLanguageOverride -> 0x%08lx %ls\n", hr,
             over ? WindowsGetStringRawBuffer( over, nullptr ) : L"(empty)" );
    if (over) WindowsDeleteString( over );

    IInspectable *langs = nullptr;
    hr = statics->get_Languages( &langs );
    wprintf( L"get_Languages -> 0x%08lx\n", hr );
    dump_vector( L"Languages", langs );
    if (langs) langs->Release();

    IInspectable *manifest = nullptr;
    hr = statics->get_ManifestLanguages( &manifest );
    wprintf( L"get_ManifestLanguages -> 0x%08lx\n", hr );
    dump_vector( L"ManifestLanguages", manifest );
    if (manifest) manifest->Release();

    /* the setter is the interesting one: Feedback Hub throws right after taking this factory */
    const wchar_t *set = L"en-US";
    HSTRING want = nullptr;
    WindowsCreateString( set, (UINT32)wcslen( set ), &want );
    hr = statics->put_PrimaryLanguageOverride( want );
    wprintf( L"put_PrimaryLanguageOverride(en-US) -> 0x%08lx\n", hr );
    WindowsDeleteString( want );

    HSTRING empty = nullptr;
    hr = statics->put_PrimaryLanguageOverride( empty );
    wprintf( L"put_PrimaryLanguageOverride(empty) -> 0x%08lx\n", hr );

    statics->Release();
    RoUninitialize();
    return 0;
}
