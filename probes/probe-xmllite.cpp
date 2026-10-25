// probe-xmllite: read an XML file through xmllite as UTF-16 (the way XAML's XamlReader.Load hands it a string) and
// as the file's own bytes, and report how many nodes and characters each read saw.
//
//   probe-xmllite.exe FILE [utf16|raw|both]
//
// A UTF-16 input that is read twice, or never ends, shows up as twice the nodes, or as the node cap being hit.
#include <windows.h>
#include <objbase.h>
#include <shlwapi.h>
#include <xmllite.h>
#include <stdio.h>
#include <string>

static int read_all(IStream *stream, const char *what)
{
    IXmlReader *reader;
    XmlNodeType type;
    unsigned long nodes = 0, elements = 0;
    unsigned long long chars = 0;
    DWORD start = GetTickCount();
    HRESULT hr;

    if (FAILED(hr = CreateXmlReader(__uuidof(IXmlReader), (void **)&reader, NULL)))
    {
        printf("CreateXmlReader %#lx\n", hr);
        return 1;
    }
    reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (FAILED(hr = reader->SetInput(stream))) { printf("SetInput %#lx\n", hr); return 1; }
    while ((hr = reader->Read(&type)) == S_OK)
    {
        const WCHAR *value;
        UINT len;
        if (++nodes > 2000000) { printf("%s: node cap hit, the reader does not end\n", what); break; }
        if (type == XmlNodeType_Element) elements++;
        if (SUCCEEDED(reader->GetValue(&value, &len))) chars += len;
        while (reader->MoveToNextAttribute() == S_OK)
            if (SUCCEEDED(reader->GetValue(&value, &len))) chars += len;
    }
    printf("%s: Read ended %#lx after %lu nodes, %lu elements, %llu value chars, %lu ms\n",
           what, hr, nodes, elements, chars, GetTickCount() - start);
    reader->Release();
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: probe-xmllite FILE [utf16|raw|both]\n"); return 1; }
    HANDLE file = CreateFileA(argv[1], GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) { printf("cannot open %s\n", argv[1]); return 1; }
    DWORD size = GetFileSize(file, NULL), got;
    std::string bytes(size, 0);
    ReadFile(file, &bytes[0], size, &got, NULL);
    CloseHandle(file);

    CoInitialize(NULL);
    const char *mode = argc > 2 ? argv[2] : "utf16";
    if (strcmp(mode, "raw"))
    {
        int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), size, NULL, 0);
        std::wstring wide(n, 0);
        MultiByteToWideChar(CP_UTF8, 0, bytes.data(), size, &wide[0], n);
        IStream *stream = SHCreateMemStream((const BYTE *)wide.data(), n * sizeof(WCHAR));
        read_all(stream, "utf16");
        stream->Release();
    }
    if (strcmp(mode, "utf16"))
    {
        IStream *stream = SHCreateMemStream((const BYTE *)bytes.data(), size);
        read_all(stream, "raw");
        stream->Release();
    }
    return 0;
}
