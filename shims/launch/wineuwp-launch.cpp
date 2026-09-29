// wineuwp-launch.exe: start a staged UWP app with a plain `wine wineuwp-launch.exe`, the way any Wine launcher
// (Bottles, Lutris, a desktop entry) runs a Windows program.
//
// run.sh does the same job from the host side, but a launcher that only knows "run this exe in this prefix" cannot
// run a shell script first. So the steps that make a package start move into the Windows side:
//
//   1. the package identity: identity.env next to this exe (install-app.py writes it; the same file run.sh
//      sources), "export NAME=VALUE" lines, becomes the environment. Our Wine reads WINE_UWP_PACKAGE_FULL_NAME and
//      friends from the environment in ntdll, kernelbase, combase and wineuwp, and a child's Unix environment is
//      built from its Windows environment block, so setting them here reaches every one of them.
//   2. launch.env, same format: the rest of what run.sh exports (DLL overrides, the framework search path,
//      renderer, loader switches). WINE_UWP_PACKAGE_PATH is this directory.
//   3. the per-package AppContainer storage key Windows.Globalization writes to (without it the first launch dies
//      with 0x80070032; Findings-M4 2026-09-29), created with the registry API.
//   4. CoreMessagingRegistrar started and RUNNING: CoreMessaging.dll waits for it before the first CoreDispatcher.
//   5. the app, from this directory, with this process's command line tail; its exit code is ours.
//
// Failures are shown in a message box, since a launcher started from a menu has no console.
#include <windows.h>
#include <cwchar>
#include <string>
#include <vector>

static std::wstring g_dir;

static void fail(const std::wstring &what)
{
    MessageBoxW(nullptr, what.c_str(), L"wine-uwp", MB_OK | MB_ICONERROR);
}

static std::wstring widen(const std::string &s)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// "export NAME=VALUE" / "NAME=VALUE" lines; single or double quotes around VALUE are removed; # starts a comment.
static bool load_env_file(const std::wstring &path, bool required)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        if (required) fail(L"Cannot read " + path + L"\nReinstall the app with the wine-uwp setup.");
        return !required;
    }
    std::string data;
    char buf[4096];
    DWORD got;
    while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got) data.append(buf, got);
    CloseHandle(f);

    size_t pos = 0;
    while (pos < data.size())
    {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) end = data.size();
        std::string line = data.substr(pos, end - pos);
        pos = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t start = line.find_first_not_of(" \t");
        if (start == std::string::npos || line[start] == '#') continue;
        line = line.substr(start);
        if (line.rfind("export ", 0) == 0) line = line.substr(7);
        size_t eq = line.find('=');
        if (eq == std::string::npos || !eq) continue;
        std::string name = line.substr(0, eq), value = line.substr(eq + 1);
        if (value.size() >= 2 && (value.front() == '\'' || value.front() == '"') && value.back() == value.front())
            value = value.substr(1, value.size() - 2);
        SetEnvironmentVariableW(widen(name).c_str(), widen(value).c_str());
    }
    return true;
}

static std::wstring env(const wchar_t *name)
{
    wchar_t buf[32768];
    DWORD n = GetEnvironmentVariableW(name, buf, ARRAYSIZE(buf));
    return n && n < ARRAYSIZE(buf) ? std::wstring(buf, n) : std::wstring();
}

static void ensure_appcontainer_key(const std::wstring &family)
{
    if (family.empty()) return;
    std::wstring key = L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\"
                       L"AppContainer\\Storage\\" + family + L"\\ResourcesConfig";
    HKEY hkey;
    if (!RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_READ, nullptr, &hkey, nullptr))
        RegCloseKey(hkey);
}

static void start_coremessaging(void)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE svc = OpenServiceW(scm, L"CoreMessagingRegistrar", SERVICE_START | SERVICE_QUERY_STATUS);
    if (svc)
    {
        SERVICE_STATUS status;
        StartServiceW(svc, 0, nullptr);   // ERROR_SERVICE_ALREADY_RUNNING is fine
        for (int i = 0; i < 150; i++)     // 15 s, as run.sh waits
        {
            if (QueryServiceStatus(svc, &status) && status.dwCurrentState == SERVICE_RUNNING) break;
            Sleep(100);
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
}

// the command line after our own executable name, to pass through to the app
static const wchar_t *args_tail(void)
{
    const wchar_t *p = GetCommandLineW();
    if (*p == L'"')
    {
        p = wcschr(p + 1, L'"');
        p = p ? p + 1 : L"";
    }
    else
        while (*p && *p != L' ' && *p != L'\t') p++;
    while (*p == L' ' || *p == L'\t') p++;
    return p;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    g_dir = self;
    g_dir.resize(g_dir.find_last_of(L'\\'));

    if (!load_env_file(g_dir + L"\\identity.env", true)) return 1;
    load_env_file(g_dir + L"\\launch.env", false);
    SetEnvironmentVariableW(L"WINE_UWP_PACKAGE_PATH", g_dir.c_str());

    std::wstring exe = env(L"UWP_EXE");
    if (exe.empty())
    {
        fail(L"identity.env names no UWP_EXE");
        return 1;
    }
    for (auto &c : exe) if (c == L'/') c = L'\\';
    ensure_appcontainer_key(env(L"WINE_UWP_PACKAGE_FAMILY_NAME"));
    start_coremessaging();

    std::wstring path = g_dir + L"\\" + exe;
    std::wstring cmdline = L"\"" + path + L"\"";
    const wchar_t *tail = args_tail();
    if (*tail) cmdline += std::wstring(L" ") + tail;
    std::vector<wchar_t> cmd(cmdline.begin(), cmdline.end());
    cmd.push_back(0);

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(path.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT, nullptr,
                        g_dir.c_str(), &si, &pi))
    {
        fail(L"Cannot start " + path + L" (error " + std::to_wstring(GetLastError()) + L")");
        return 1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return (int)code;
}
