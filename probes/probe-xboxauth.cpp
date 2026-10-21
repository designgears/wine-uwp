// probe-xboxauth <what> [arg]: drives winexboxauth.dll's XboxAuthSelfTest without the Xbox app.
//   endpoints <url> | xbl [rps] | devicecode | silent [url] | sigtest | devicetoken [Win32|Android|...] | sisu | policy <url>
//   (see dlls/winexboxauth in Wine)
#include <windows.h>
#include <cstdio>
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: probe-xboxauth endpoints URL | xbl [RPS] | devicecode | silent [URL] | sigtest | devicetoken [TYPE] | sisu | policy URL\n"); return 2; }
    HMODULE m = LoadLibraryA("winexboxauth.dll");
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 3; }
    typedef int (WINAPI *fn)(const char *, const char *);
    fn f = (fn)GetProcAddress(m, "XboxAuthSelfTest");
    if (!f) { printf("no XboxAuthSelfTest\n"); return 3; }
    int r = f(argv[1], argc > 2 ? argv[2] : nullptr);
    printf("result %d\n", r);
    return r;
}
