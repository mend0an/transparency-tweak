// TransparencyTweak.exe - loads TweakTap.dll into the taskbar's explorer.exe via InitializeXamlDiagnosticsEx,
// then exits. The DLL itself takes care of Settings, so nothing stays running.
//
//   TransparencyTweak.exe            inject into explorer (waits for the taskbar; does nothing if already loaded)
//   TransparencyTweak.exe <pid>      bootstrap Settings without XAML diagnostics
//
// Results go to %LOCALAPPDATA%\TransparencyTweak\logs\injector.log.

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <string>
#include "SettingsInjection.h"

// Must match TweakTap.cpp.
static constexpr CLSID CLSID_TweakTap = {0x1b5d6c2e, 0x6e0a, 0x4c55, {0x9c, 0x35, 0x7a, 0x0e, 0x3b, 0x8f, 0x21, 0xd4}};

using InitializeXamlDiagnosticsExFn = HRESULT(WINAPI*)(LPCWSTR endPointName, DWORD pid, LPCWSTR wszDllXamlDiagnostics,
                                                       LPCWSTR wszTAPDllName, CLSID tapClsid, LPCWSTR wszInitializationData);

static std::wstring DataDir()
{
    wchar_t dir[MAX_PATH]{};
    ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\TransparencyTweak", dir, MAX_PATH);
    CreateDirectoryW(dir, nullptr);
    return dir;
}

static void Log(const wchar_t* fmt, ...)
{
    std::wstring logs = DataDir() + L"\\logs";
    CreateDirectoryW(logs.c_str(), nullptr);
    FILE* file = _wfsopen((logs + L"\\injector.log").c_str(), L"a, ccs=UTF-8", 0x40 /* _SH_DENYNO */);
    if (!file) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(file, L"%04d-%02d-%02d %02d:%02d:%02d ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    va_list args;
    va_start(args, fmt);
    vfwprintf(file, fmt, args);
    va_end(args);
    fputwc(L'\n', file);
    fclose(file);
}

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir = path;
    return dir.substr(0, dir.find_last_of(L'\\'));
}

// A loaded DLL stays locked inside the target until it exits, so every build is copied
// to its own file name; updating never collides with a copy that is still in use.
static std::wstring StageTap()
{
    std::wstring source = ExeDir() + L"\\TweakTap.dll";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &info)) return L"";

    std::wstring binDir = DataDir() + L"\\bin";
    CreateDirectoryW(binDir.c_str(), nullptr);
    wchar_t name[64];
    swprintf_s(name, L"\\TweakTap-%08lX%08lX.dll", info.ftLastWriteTime.dwHighDateTime, info.ftLastWriteTime.dwLowDateTime);
    std::wstring target = binDir + name;
    if (GetFileAttributesW(target.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(source.c_str(), target.c_str(), TRUE))
        return L"";
    return target;
}

static DWORD WaitForTaskbar(DWORD timeoutMs)
{
    for (DWORD waited = 0; waited <= timeoutMs; waited += 500)
    {
        DWORD pid = 0;
        if (HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr)) GetWindowThreadProcessId(tray, &pid);
        if (pid) return pid;
        Sleep(500);
    }
    return 0;
}

static bool TapLoadedIn(DWORD pid)
{
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    MODULEENTRY32W module{sizeof(module)};
    for (BOOL ok = Module32FirstW(snapshot, &module); ok && !found; ok = Module32NextW(snapshot, &module))
        found = _wcsnicmp(module.szModule, L"TweakTap-", 9) == 0;
    CloseHandle(snapshot);
    return found;
}

static HRESULT Inject(DWORD pid, const std::wstring& tap)
{
    HMODULE xaml = LoadLibraryExW(L"Windows.UI.Xaml.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto initialize = xaml ? reinterpret_cast<InitializeXamlDiagnosticsExFn>(GetProcAddress(xaml, "InitializeXamlDiagnosticsEx")) : nullptr;
    if (!initialize) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

    // Other tools (Visual Studio, TranslucentTB, ...) may already own a connection name; try the next ones.
    HRESULT hr = E_FAIL;
    for (int i = 1; i <= 10 && FAILED(hr); ++i)
    {
        wchar_t endpoint[64];
        swprintf_s(endpoint, L"VisualDiagConnection%d", i);
        hr = initialize(endpoint, pid, nullptr, tap.c_str(), CLSID_TweakTap, nullptr);
    }
    return hr;
}

int wmain(int argc, wchar_t** argv)
{
    bool explorer = argc < 2 || _wcsicmp(argv[1], L"explorer") == 0;
    DWORD pid = explorer ? WaitForTaskbar(120000) : wcstoul(argv[1], nullptr, 10);
    if (!pid)
    {
        Log(L"target process not found");
        return 3;
    }
    if (TapLoadedIn(pid))
    {
        Log(L"pid %lu: already active, nothing to do", pid);
        return 0;
    }

    std::wstring tap = StageTap();
    if (tap.empty())
    {
        Log(L"could not stage TweakTap.dll (error %lu)", GetLastError());
        return 4;
    }

    if (explorer) Sleep(2000); // let the taskbar finish building its XAML right after sign-in
    HRESULT hr = E_FAIL;
    for (int attempt = 1; attempt <= 5 && FAILED(hr); ++attempt)
    {
        hr = explorer ? Inject(pid, tap) : settings_injection::Inject(pid, tap);
        Log(L"pid %lu: inject attempt %d hr=0x%08X", pid, attempt, static_cast<unsigned>(hr));
        if (FAILED(hr)) Sleep(3000);
    }
    return SUCCEEDED(hr) ? 0 : 1;
}
