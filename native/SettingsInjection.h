#pragma once

// Settings on Windows build 26300 crashes on the Background page as soon as a
// XAML diagnostics session has been initialized, even without a subscriber or
// slider. Load our DLL normally and call its UI-dispatcher bootstrap instead.
#include <windows.h>
#include <tlhelp32.h>
#include <string>

namespace settings_injection
{
    class Handle
    {
    public:
        explicit Handle(HANDLE value) : m_value(value) {}
        ~Handle() { if (m_value && m_value != INVALID_HANDLE_VALUE) CloseHandle(m_value); }
        Handle(Handle const&) = delete;
        Handle& operator=(Handle const&) = delete;
        HANDLE get() const { return m_value; }
    private:
        HANDLE m_value;
    };

    inline uintptr_t ModuleBase(DWORD pid, const wchar_t* name, bool fullPath = false)
    {
        HANDLE snapshot;
        do { snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, pid); }
        while (snapshot == INVALID_HANDLE_VALUE && GetLastError() == ERROR_BAD_LENGTH);
        Handle guard(snapshot);
        if (snapshot == INVALID_HANDLE_VALUE) return 0;
        MODULEENTRY32W entry{sizeof(entry)};
        for (BOOL ok = Module32FirstW(snapshot, &entry); ok; ok = Module32NextW(snapshot, &entry))
            if (_wcsicmp(fullPath ? entry.szExePath : entry.szModule, name) == 0)
                return reinterpret_cast<uintptr_t>(entry.modBaseAddr);
        return 0;
    }

    inline HRESULT Call(HANDLE process, uintptr_t address, void* argument, DWORD& result, bool& finished)
    {
        finished = true; // safe to free the argument if no thread was created
        Handle thread(CreateRemoteThread(process, nullptr, 0,
            reinterpret_cast<LPTHREAD_START_ROUTINE>(address), argument, 0, nullptr));
        if (!thread.get()) return HRESULT_FROM_WIN32(GetLastError());
        finished = false;
        DWORD wait = WaitForSingleObject(thread.get(), 10000);
        if (wait != WAIT_OBJECT_0)
            return HRESULT_FROM_WIN32(wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError());
        finished = true;
        return GetExitCodeThread(thread.get(), &result) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
    }

    inline HRESULT Inject(DWORD pid, std::wstring const& path, HMODULE localTap = nullptr)
    {
        Handle process(OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION |
            PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid));
        if (!process.get()) return HRESULT_FROM_WIN32(GetLastError());
        wchar_t image[MAX_PATH]{};
        DWORD length = MAX_PATH;
        if (!QueryFullProcessImageNameW(process.get(), 0, image, &length))
            return HRESULT_FROM_WIN32(GetLastError());
        const wchar_t* leaf = wcsrchr(image, L'\\');
        if (_wcsicmp(leaf ? leaf + 1 : image, L"SystemSettings.exe") != 0) return E_INVALIDARG;

        // Resolve the bootstrap RVA without running DllMain in the injector.
        // Explorer already has this module loaded and passes its own HMODULE.
        HMODULE mapped = localTap ? localTap : LoadLibraryExW(path.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
        if (!mapped) return HRESULT_FROM_WIN32(GetLastError());
        auto bootstrap = GetProcAddress(mapped, "StartSettingsTweak");
        uintptr_t bootstrapRva = bootstrap ? reinterpret_cast<uintptr_t>(bootstrap) - reinterpret_cast<uintptr_t>(mapped) : 0;
        if (!localTap) FreeLibrary(mapped);
        if (!bootstrapRva) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);

        uintptr_t remoteTap = ModuleBase(pid, path.c_str(), true);
        if (!remoteTap)
        {
            auto loadLibrary = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
            if (!loadLibrary) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
            // A forwarded export can belong to KernelBase. Resolve its owning
            // module and relocate the RVA; never assume identical ASLR bases.
            HMODULE owner = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(loadLibrary), &owner)) return HRESULT_FROM_WIN32(GetLastError());
            wchar_t ownerPath[MAX_PATH]{};
            if (!GetModuleFileNameW(owner, ownerPath, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
            const wchar_t* ownerLeaf = wcsrchr(ownerPath, L'\\');
            uintptr_t remoteOwner = ModuleBase(pid, ownerLeaf ? ownerLeaf + 1 : ownerPath);
            if (!remoteOwner) return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
            uintptr_t remoteLoad = remoteOwner + reinterpret_cast<uintptr_t>(loadLibrary) - reinterpret_cast<uintptr_t>(owner);

            SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);
            void* argument = VirtualAllocEx(process.get(), nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (!argument) return HRESULT_FROM_WIN32(GetLastError());
            if (!WriteProcessMemory(process.get(), argument, path.c_str(), bytes, nullptr))
            {
                HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
                VirtualFreeEx(process.get(), argument, 0, MEM_RELEASE);
                return hr;
            }
            DWORD result = 0;
            bool finished = false;
            HRESULT hr = Call(process.get(), remoteLoad, argument, result, finished);
            // On timeout the loader may still be reading this buffer. Leave it
            // allocated until process exit instead of introducing a use-after-free.
            if (finished) VirtualFreeEx(process.get(), argument, 0, MEM_RELEASE);
            if (FAILED(hr)) return hr;
            // GetExitCodeThread is only 32 bits; enumerate the full 64-bit base.
            remoteTap = ModuleBase(pid, path.c_str(), true);
            if (!remoteTap) return HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED);
        }

        DWORD result = 0;
        bool finished = false;
        HRESULT hr = Call(process.get(), remoteTap + bootstrapRva, nullptr, result, finished);
        return FAILED(hr) ? hr : static_cast<HRESULT>(result);
    }
}
