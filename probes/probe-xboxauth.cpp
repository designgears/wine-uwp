// probe-xboxauth <what> [arg]: drives wineuwp_xboxauth.dll's XboxAuthSelfTest without the Xbox app.
//   endpoints <url> | xbl [rps] | devicecode | silent [url]   (see shims/xboxauth/xboxauth.cpp)
#include <windows.h>
#include <cstdio>
int main(int argc, char **argv)
{
    if (argc < 2) { printf("usage: probe-xboxauth endpoints URL | xbl [RPS] | devicecode | silent [URL]\n"); return 2; }
    HMODULE m = LoadLibraryA("wineuwp_xboxauth.dll");
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 3; }
    typedef int (WINAPI *fn)(const char *, const char *);
    fn f = (fn)GetProcAddress(m, "XboxAuthSelfTest");
    if (!f) { printf("no XboxAuthSelfTest\n"); return 3; }
    int r = f(argv[1], argc > 2 ? argv[2] : nullptr);
    printf("result %d\n", r);
    return r;
}
