// probe-activate: RoInitialize + RoGetActivationFactory for each class name given.
// Answers: does Wine's combase find and load the registered activatable class?
#include <windows.h>
#include <roapi.h>
#include <winstring.h>
#include <inspectable.h>
#include <cstdio>
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { fwprintf(stderr, L"usage: probe-activate <ActivatableClassId>...\n"); return 2; }
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    wprintf(L"RoInitialize -> 0x%08lx\n", hr);
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        HSTRING cls; WindowsCreateString(argv[i], (UINT32)wcslen(argv[i]), &cls);
        IInspectable* factory = nullptr;
        hr = RoGetActivationFactory(cls, __uuidof(IInspectable), (void**)&factory);
        if (SUCCEEDED(hr)) {
            HSTRING rc = nullptr; factory->GetRuntimeClassName(&rc);
            wprintf(L"OK   %ls (factory class %ls)\n", argv[i], rc ? WindowsGetStringRawBuffer(rc, nullptr) : L"?");
            if (rc) WindowsDeleteString(rc);
            IInspectable* inst = nullptr;
            HRESULT hr2 = RoActivateInstance(cls, &inst);
            wprintf(L"     RoActivateInstance -> 0x%08lx\n", hr2);
            if (inst) inst->Release();
            factory->Release();
        } else { wprintf(L"FAIL %ls -> 0x%08lx\n", argv[i], hr); ++failures; }
        WindowsDeleteString(cls);
    }
    RoUninitialize();
    return failures ? 1 : 0;
}
