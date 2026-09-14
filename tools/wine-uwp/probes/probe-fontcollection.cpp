// Hammer IDWriteFactory::GetSystemFontCollection from several threads on one shared factory, the way
// XAML's view thread and a background thread both reach it during MainPage's first measure. Wine's
// factory caches the system collection behind a weak pointer; a lost race there released a reference
// it never owned, freeing a live collection and double-releasing the system font set's entries. The
// app then crashed reading a freed IDWriteFontFile in create_font_collection_from_set. Green means
// every thread got a usable collection every time and the process survived.
#include <windows.h>
#include <dwrite_3.h>
#include <stdio.h>

static IDWriteFactory *factory;
static LONG failures;
static HANDLE start;
static volatile LONG stop_bumping;

/* dwrite rebuilds the factory's system font set whenever the Fonts key's last-write time moves
 * forward; in the app run Wine rewrote that key while XAML's threads were using dwrite. */
static DWORD WINAPI bump_fonts_key(void *)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts", 0,
            KEY_SET_VALUE, &key))
    {
        printf("FAIL cannot open the Fonts key for writing\n");
        InterlockedIncrement(&failures);
        return 1;
    }
    unsigned bumps = 0;
    while (!stop_bumping)
    {
        RegSetValueExW(key, L"wine-uwp probe", 0, REG_SZ, (const BYTE *)L"x", 2 * sizeof(WCHAR));
        RegDeleteValueW(key, L"wine-uwp probe");
        ++bumps;
        Sleep(5);
    }
    RegCloseKey(key);
    printf("\nok   bumped the Fonts key %u times\n", bumps);
    return 0;
}

enum { THREADS = 8, ITERATIONS = 12 };

static DWORD WINAPI worker(void *arg)
{
    WaitForSingleObject(start, INFINITE);
    for (int i = 0; i < ITERATIONS; ++i)
    {
        IDWriteFontCollection *collection;
        HRESULT hr = factory->GetSystemFontCollection(&collection, FALSE);
        if (FAILED(hr))
        {
            printf("FAIL thread %u iteration %d: GetSystemFontCollection %#lx\n", (unsigned)(UINT_PTR)arg, i, hr);
            InterlockedIncrement(&failures);
            return 1;
        }
        /* Touch the collection so a freed one crashes here rather than silently passing. */
        UINT32 count = collection->GetFontFamilyCount();
        if (count)
        {
            IDWriteFontFamily *family;
            if (SUCCEEDED(collection->GetFontFamily(0, &family)))
                family->Release();
        }
        collection->Release();
        printf(".");
    }
    return 0;
}

int main()
{
    HANDLE threads[THREADS];
    HRESULT hr;

    setvbuf(stdout, nullptr, _IONBF, 0);
    if (FAILED(hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory)))
        { printf("FAIL DWriteCreateFactory: %#lx\n", hr); return 1; }

    ULONGLONG t0 = GetTickCount64();
    IDWriteFontCollection *first;
    if (FAILED(hr = factory->GetSystemFontCollection(&first, FALSE)))
        { printf("FAIL first GetSystemFontCollection: %#lx\n", hr); return 1; }
    printf("ok   one system collection: %u families in %llu ms\n", first->GetFontFamilyCount(), GetTickCount64() - t0);
    first->Release();

    start = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE bumper = CreateThread(nullptr, 0, bump_fonts_key, nullptr, 0, nullptr);
    for (UINT_PTR t = 0; t < THREADS; ++t)
        threads[t] = CreateThread(nullptr, 0, worker, (void *)t, 0, nullptr);
    SetEvent(start);
    WaitForMultipleObjects(THREADS, threads, TRUE, INFINITE);
    InterlockedExchange(&stop_bumping, 1);
    WaitForSingleObject(bumper, INFINITE);
    printf("\nok   %d threads x %d GetSystemFontCollection calls returned\n", THREADS, ITERATIONS);

    /* After the storm, the factory's cached system font set must still be intact. */
    IDWriteFactory3 *factory3;
    if (SUCCEEDED(factory->QueryInterface(&factory3)))
    {
        IDWriteFontSet *set;
        IDWriteFontCollection1 *collection;
        if (FAILED(hr = factory3->GetSystemFontSet(&set)))
            { printf("FAIL GetSystemFontSet: %#lx\n", hr); failures++; }
        else
        {
            if (FAILED(hr = factory3->CreateFontCollectionFromFontSet(set, &collection)))
                { printf("FAIL CreateFontCollectionFromFontSet: %#lx\n", hr); failures++; }
            else
            {
                printf("ok   collection from the system font set: %u families\n", collection->GetFontFamilyCount());
                collection->Release();
            }
            set->Release();
        }
        factory3->Release();
    }

    printf(failures ? "RED: %ld failures\n" : "GREEN: the system font collection survives concurrent use\n", failures);
    return failures;
}
