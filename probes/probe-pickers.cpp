// Windows.Storage.Pickers through the platform. With WINE_UWP_PICKER_SAVE_FILE set, the save picker
// must hand back that file; without it the real save dialog shows, listing the FileTypeChoices in
// the order they were added (capture it, then Escape: a cancelled pick is a NULL file).
#include <windows.h>
#include <roapi.h>
#include <windows.storage.pickers.h>
#include <wrl.h>
#include <wrl/wrappers/corewrappers.h>
#include <cstdio>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Foundation;
using namespace ABI::Windows::Foundation::Collections;
using namespace ABI::Windows::Storage;
using namespace ABI::Windows::Storage::Pickers;

template <class Op> static HRESULT wait(Op *op)
{
    ComPtr<IAsyncInfo> info;
    AsyncStatus status = AsyncStatus::Started;
    HRESULT hr = op->QueryInterface(IID_PPV_ARGS(&info));
    for (int i = 0; SUCCEEDED(hr) && status == AsyncStatus::Started && i < 6000; i++)
    {
        Sleep(10);
        hr = info->get_Status(&status);
    }
    return SUCCEEDED(hr) && status != AsyncStatus::Completed ? E_FAIL : hr;
}

static HRESULT add_choice(IMap<HSTRING, IVector<HSTRING> *> *map, const WCHAR *name, const WCHAR *ext)
{
    ComPtr<IInspectable> insp;
    ComPtr<IVector<HSTRING>> vector;
    ComPtr<IFileOpenPicker> scratch;     /* a picker's FileTypeFilter is a fresh IVector<HSTRING> */
    boolean replaced;
    HRESULT hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_Storage_Pickers_FileOpenPicker).Get(), &insp);
    if (SUCCEEDED(hr)) hr = insp.As(&scratch);
    if (SUCCEEDED(hr)) hr = scratch->get_FileTypeFilter(&vector);
    if (SUCCEEDED(hr)) hr = vector->Append(HStringReference(ext).Get());
    if (SUCCEEDED(hr)) hr = map->Insert(HStringReference(name).Get(), vector.Get(), &replaced);
    return hr;
}

int main()
{
    int bad = 0;
    WCHAR staged[MAX_PATH];
    BOOL have_staged = GetEnvironmentVariableW(L"WINE_UWP_PICKER_SAVE_FILE", staged, MAX_PATH);
    RoInitialize(RO_INIT_SINGLETHREADED);
    {
        ComPtr<IInspectable> insp;
        ComPtr<IFileSavePicker> picker;
        ComPtr<IMap<HSTRING, IVector<HSTRING> *>> choices;
        HRESULT hr = RoActivateInstance(HStringReference(RuntimeClass_Windows_Storage_Pickers_FileSavePicker).Get(), &insp);
        if (SUCCEEDED(hr)) hr = insp.As(&picker);
        printf("FileSavePicker: %#lx\n", hr);
        if (FAILED(hr)) return 1;
        picker->get_FileTypeChoices(&choices);
        hr = add_choice(choices.Get(), L"Zeta image", L".zzz");
        if (SUCCEEDED(hr)) hr = add_choice(choices.Get(), L"Alpha image", L".aaa");
        UINT32 size = 0;
        choices->get_Size(&size);
        printf("FileTypeChoices: %#lx, %u choices\n", hr, size);
        bad += FAILED(hr) || size != 2;
        picker->put_SuggestedFileName(HStringReference(L"probe-pickers").Get());

        ComPtr<IAsyncOperation<StorageFile *>> op;
        ComPtr<IStorageFile> file;
        hr = picker->PickSaveFileAsync(&op);
        if (SUCCEEDED(hr)) hr = wait(op.Get());
        if (SUCCEEDED(hr)) hr = op->GetResults(&file);
        HString path;
        if (file)
        {
            ComPtr<IStorageItem> item;
            file.As(&item);
            item->get_Path(path.GetAddressOf());
        }
        printf("PickSaveFileAsync: %#lx -> %ls\n", hr, file ? path.GetRawBuffer(nullptr) : L"(cancelled)");
        if (have_staged) bad += FAILED(hr) || !file || _wcsicmp(path.GetRawBuffer(nullptr), staged);
        else bad += FAILED(hr) || file;   /* the dialog was dismissed */
    }
    RoUninitialize();
    printf("%s: pickers\n", bad ? "RED" : "GREEN");
    return bad;
}
