// Experimental, standalone controller. No DLL injection or persistent settings.
// Start / Quick Settings / Notification Center / tray overflow slide in from
// their monitor's taskbar edge and finish at Windows' original coordinates.
#include <windows.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <map>
#include <string>
#include <vector>
#include "FlyoutAnimationMath.h"

namespace
{
    constexpr wchar_t kWindowClass[] = L"TransparencyTweak.ShellFlyoutAnimation.Experimental";
    constexpr UINT kStop = WM_APP + 1;
    constexpr UINT_PTR kTick = 1;
    constexpr ULONGLONG kDurationMs = 280;
    constexpr UINT kPositionFlags = SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
        SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS;
    HWND g_control = nullptr;
    FILE* g_log = nullptr;
    bool g_timer = false;
    using flyout_animation::Edge;
    using flyout_animation::Point;

    flyout_animation::Rect MathRect(RECT const& rect)
    { return {rect.left, rect.top, rect.right, rect.bottom}; }

    void Log(const wchar_t* fmt, ...)
    {
        wchar_t message[1024]{};
        va_list args;
        va_start(args, fmt);
        vswprintf_s(message, fmt, args);
        va_end(args);
        if (g_log) { fwprintf(g_log, L"%llu %s\n", GetTickCount64(), message); fflush(g_log); }
        fwprintf(stdout, L"%s\n", message);
    }

    struct Identity { DWORD pid = 0; DWORD thread = 0; std::wstring cls; };

    bool GetIdentity(HWND hwnd, Identity& identity)
    {
        if (!IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) return false;
        wchar_t cls[128]{};
        if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls))) return false;
        std::wstring windowClass = cls;
        if (windowClass != L"Windows.UI.Core.CoreWindow" && windowClass != L"ControlCenterWindow" &&
            windowClass != L"TopLevelWindowForOverflowXamlIsland") return false;
        DWORD pid = 0;
        DWORD threadId = GetWindowThreadProcessId(hwnd, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        wchar_t path[32768]{};
        DWORD length = ARRAYSIZE(path);
        BOOL queried = QueryFullProcessImageNameW(process, 0, path, &length);
        CloseHandle(process);
        if (!queried) return false;
        const wchar_t* leaf = wcsrchr(path, L'\\');
        std::wstring processName = leaf ? leaf + 1 : path;
        // Executable names are case-insensitive; match against canonical names.
        for (const wchar_t* known : {L"explorer.exe", L"StartMenuExperienceHost.exe", L"ShellHost.exe", L"ShellExperienceHost.exe"})
            if (_wcsicmp(processName.c_str(), known) == 0) { processName = known; break; }
        std::wstring threadName;
        if (processName == L"ShellExperienceHost.exe")
        {
            HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, threadId);
            if (thread)
            {
                PWSTR description = nullptr;
                if (SUCCEEDED(GetThreadDescription(thread, &description)) && description)
                { threadName = description; LocalFree(description); }
                CloseHandle(thread);
            }
        }
        if (!flyout_animation::IsTarget(processName, windowClass, threadName)) return false;
        identity = {pid, threadId, windowClass};
        return true;
    }

    bool IsOpen(HWND hwnd)
    {
        DWORD cloaked = 0;
        return IsWindowVisible(hwnd) && SUCCEEDED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED,
            &cloaked, sizeof(cloaked))) && !cloaked;
    }

    struct TaskbarPlacement
    {
        HWND hwnd = nullptr;
        Edge edge = Edge::Bottom;
    };
    // Only used by --integration-test, with a window owned by that test.
    TaskbarPlacement const* g_testPlacement = nullptr;

    bool IsTaskbar(HWND hwnd)
    {
        wchar_t cls[64]{};
        if (!GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) ||
            (wcscmp(cls, L"Shell_TrayWnd") != 0 && wcscmp(cls, L"Shell_SecondaryTrayWnd") != 0)) return false;
        DWORD pid = 0;
        GetWindowThreadProcessId(hwnd, &pid);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!process) return false;
        wchar_t path[MAX_PATH]{};
        DWORD size = ARRAYSIZE(path);
        BOOL queried = QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
        const wchar_t* name = wcsrchr(path, L'\\');
        return queried && _wcsicmp(name ? name + 1 : path, L"explorer.exe") == 0;
    }

    bool FindTaskbar(HMONITOR monitor, RECT const& monitorRect, TaskbarPlacement& placement)
    {
        if (g_testPlacement) { placement = *g_testPlacement; return true; }
        struct Search
        {
            HMONITOR monitor;
            RECT rect;
            TaskbarPlacement result;
        } search{monitor, monitorRect, {}};
        EnumWindows([](HWND hwnd, LPARAM param) -> BOOL
        {
            auto& found = *reinterpret_cast<Search*>(param);
            if (!IsTaskbar(hwnd)) return TRUE;
            // Explorer supplies monitor affinity on modern taskbars. The
            // geometric fallback also supports older taskbars and moved HWNDs.
            auto affinity = reinterpret_cast<HMONITOR>(GetPropW(hwnd, L"TaskbarMonitor"));
            if (!affinity) affinity = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
            if (affinity != found.monitor) return TRUE;
            RECT rect{};
            if (!GetWindowRect(hwnd, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) return TRUE;
            found.result = {hwnd, flyout_animation::TaskbarEdge(MathRect(found.rect), MathRect(rect))};
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (search.result.hwnd) { placement = search.result; return true; }
        // A fully hidden bar may geometrically overlap an adjacent monitor.
        // Query its registered monitor and edge instead of using that overlap.
        for (UINT edge : {ABE_LEFT, ABE_TOP, ABE_RIGHT, ABE_BOTTOM})
        {
            APPBARDATA data{sizeof(data)};
            data.uEdge = edge;
            data.rc = monitorRect;
            auto hwnd = reinterpret_cast<HWND>(SHAppBarMessage(ABM_GETAUTOHIDEBAREX, &data));
            if (hwnd && IsTaskbar(hwnd))
            {
                placement = {hwnd, static_cast<Edge>(edge)};
                return true;
            }
        }
        return false;
    }

    struct State
    {
        Identity identity;
        bool open = false;
        bool animating = false;
        RECT finalRect{};
        HMONITOR monitor = nullptr;
        RECT monitorRect{};
        TaskbarPlacement taskbar;
        Point from{};
        ULONGLONG start = 0;
        Point last{};
    };
    std::map<HWND, State> g_states;

    bool StillOwned(HWND hwnd, State const& state)
    {
        DWORD pid = 0;
        DWORD thread = GetWindowThreadProcessId(hwnd, &pid);
        wchar_t cls[128]{};
        return thread == state.identity.thread && pid == state.identity.pid &&
            GetClassNameW(hwnd, cls, ARRAYSIZE(cls)) && state.identity.cls == cls;
    }

    void Restore(HWND hwnd, State& state)
    {
        if (state.animating && StillOwned(hwnd, state))
        {
            BOOL moved = SetWindowPos(hwnd, nullptr, state.finalRect.left, state.finalRect.top, 0, 0, kPositionFlags);
            Log(L"restore hwnd=%p queued=%d", hwnd, moved);
        }
        state.animating = false;
    }

    void UpdateTimer()
    {
        bool needed = false;
        for (auto const& [hwnd, state] : g_states) if (state.animating) { needed = true; break; }
        if (needed && !g_timer)
        {
            g_timer = SetTimer(g_control, kTick, 15, nullptr) != 0;
            if (!g_timer) for (auto& [hwnd, state] : g_states) { Restore(hwnd, state); }
        }
        else if (!needed && g_timer) { KillTimer(g_control, kTick); g_timer = false; }
    }

    void Begin(HWND hwnd, State& state)
    {
        BOOL animations = FALSE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) || !animations)
        { Log(L"skip hwnd=%p: Windows animations disabled", hwnd); return; }
        RECT rect{};
        MONITORINFO monitorInfo{sizeof(monitorInfo)};
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        if (!monitor || !GetMonitorInfoW(monitor, &monitorInfo) || !GetWindowRect(hwnd, &rect) ||
            rect.right <= rect.left || rect.bottom <= rect.top)
            return;
        TaskbarPlacement taskbar;
        if (!FindTaskbar(monitor, monitorInfo.rcMonitor, taskbar))
        { Log(L"skip hwnd=%p: no taskbar on panel monitor", hwnd); return; }
        state.finalRect = rect;
        state.monitor = monitor;
        state.monitorRect = monitorInfo.rcMonitor;
        state.taskbar = taskbar;
        state.from = flyout_animation::StartPoint(MathRect(rect), MathRect(monitorInfo.rcMonitor), taskbar.edge);
        state.start = GetTickCount64();
        state.last = state.from;
        if (!SetWindowPos(hwnd, nullptr, state.from.x, state.from.y, 0, 0, kPositionFlags))
        { Log(L"slide failed hwnd=%p error=%lu", hwnd, GetLastError()); return; }
        state.animating = true;
        Log(L"slide start hwnd=%p class=%s edge=%s from=%d,%d to=%ld,%ld duration=%llu", hwnd,
            state.identity.cls.c_str(), flyout_animation::EdgeName(taskbar.edge), state.from.x, state.from.y,
            rect.left, rect.top, kDurationMs);
        UpdateTimer();
    }

    void CALLBACK OnEvent(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD eventTime)
    {
        // Out-of-context callbacks are serialized on our message-loop thread.
        if (!hwnd || object != OBJID_WINDOW || child != CHILDID_SELF) return;
        try
        {
            auto it = g_states.find(hwnd);
            if (event == EVENT_OBJECT_DESTROY)
            { if (it != g_states.end()) g_states.erase(it); UpdateTimer(); return; }
            if (it != g_states.end() && !StillOwned(hwnd, it->second))
            { g_states.erase(it); it = g_states.end(); }
            if (it == g_states.end())
            {
                Identity identity;
                if (!GetIdentity(hwnd, identity)) return;
                State state;
                state.identity = identity;
                it = g_states.emplace(hwnd, state).first;
            }
            auto& state = it->second;
            bool open = IsOpen(hwnd);
            if (!open)
            {
                Restore(hwnd, state);
                state.open = false;
                UpdateTimer();
                return;
            }
            if (!state.open)
            {
                state.open = true;
                // Ignore queued old opens; otherwise a slow/hung desktop flashes.
                if ((event == EVENT_OBJECT_SHOW || event == EVENT_OBJECT_UNCLOAKED) &&
                    static_cast<DWORD>(GetTickCount() - eventTime) < 150) Begin(hwnd, state);
            }
        }
        catch (...)
        {
            Log(L"event callback failed hwnd=%p", hwnd);
        }
    }

    void Tick()
    {
        ULONGLONG now = GetTickCount64();
        for (auto it = g_states.begin(); it != g_states.end();)
        {
            HWND hwnd = it->first;
            State& state = it->second;
            if (!StillOwned(hwnd, state)) { it = g_states.erase(it); continue; }
            ++it;
            if (!state.animating) continue;
            RECT rect{};
            if (!IsOpen(hwnd))
            {
                Restore(hwnd, state);
                state.open = false;
                continue;
            }
            if (!GetWindowRect(hwnd, &rect)) { Restore(hwnd, state); continue; }
            MONITORINFO monitorInfo{sizeof(monitorInfo)};
            if (!GetMonitorInfoW(state.monitor, &monitorInfo) ||
                !EqualRect(&monitorInfo.rcMonitor, &state.monitorRect))
            {
                // Do not drag a panel back onto removed/reconfigured monitors.
                // If Windows has already repositioned it, keep that placement.
                if (rect.top == state.last.y && rect.left == state.last.x) Restore(hwnd, state);
                else state.animating = false;
                Log(L"slide cancelled hwnd=%p: monitor changed", hwnd);
                continue;
            }
            RECT taskbarRect{};
            bool taskbarChanged = g_testPlacement ? g_testPlacement->edge != state.taskbar.edge :
                !GetWindowRect(state.taskbar.hwnd, &taskbarRect) ||
                flyout_animation::TaskbarEdge(MathRect(state.monitorRect), MathRect(taskbarRect)) != state.taskbar.edge;
            if (taskbarChanged)
            {
                if (rect.top == state.last.y && rect.left == state.last.x) Restore(hwnd, state);
                else state.animating = false;
                Log(L"slide cancelled hwnd=%p: taskbar edge changed", hwnd);
                continue;
            }
            // Allow movement on the animated axis. Preserve native changes
            // on the other axis and new panel dimensions when cancelling.
            bool horizontal = flyout_animation::Horizontal(state.taskbar.edge);
            if ((horizontal ? rect.top != state.finalRect.top : rect.left != state.finalRect.left) ||
                rect.right - rect.left != state.finalRect.right - state.finalRect.left ||
                rect.bottom - rect.top != state.finalRect.bottom - state.finalRect.top)
            {
                if (horizontal) state.finalRect.top = rect.top;
                else state.finalRect.left = rect.left;
                Restore(hwnd, state);
                Log(L"slide cancelled hwnd=%p: layout changed", hwnd);
                continue;
            }
            double progress = static_cast<double>(now - state.start) / kDurationMs;
            if (progress >= 1.0) { Restore(hwnd, state); continue; }
            Point point = flyout_animation::Slide({state.finalRect.left, state.finalRect.top}, state.from, progress);
            if (point.x != state.last.x || point.y != state.last.y)
            {
                if (!SetWindowPos(hwnd, nullptr, point.x, point.y, 0, 0, kPositionFlags))
                { Restore(hwnd, state); Log(L"slide frame failed hwnd=%p", hwnd); }
                else state.last = point;
            }
        }
        UpdateTimer();
    }

    LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        if (message == kStop) { PostQuitMessage(0); return 0; }
        if (message == WM_TIMER && wparam == kTick)
        {
            try { Tick(); }
            catch (...) { for (auto& [window, state] : g_states) { Restore(window, state); } UpdateTimer(); }
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    BOOL CALLBACK Inventory(HWND hwnd, LPARAM)
    {
        Identity identity;
        if (!GetIdentity(hwnd, identity)) return TRUE;
        State state;
        state.identity = identity;
        state.open = IsOpen(hwnd); // Never move panels that were already open at startup.
        g_states[hwnd] = state;
        RECT rect{};
        GetWindowRect(hwnd, &rect);
        Log(L"target hwnd=%p pid=%lu class=%s open=%d rect=%ld,%ld,%ld,%ld", hwnd,
            identity.pid, identity.cls.c_str(), state.open, rect.left, rect.top, rect.right, rect.bottom);
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONULL);
        MONITORINFO info{sizeof(info)};
        TaskbarPlacement taskbar;
        if (monitor && GetMonitorInfoW(monitor, &info) && FindTaskbar(monitor, info.rcMonitor, taskbar))
        {
            Log(L"target hwnd=%p taskbar=%p edge=%s", hwnd, taskbar.hwnd, flyout_animation::EdgeName(taskbar.edge));
        }
        return TRUE;
    }

    int SelfTest()
    {
        using namespace flyout_animation;
        bool pass = true;
        for (Rect monitor : {Rect{0, 0, 1920, 1080}, Rect{-1920, -1080, 0, 0}, Rect{1920, 0, 4480, 1440}})
        {
            for (int scale : {1, 2})
            {
                Rect panel{monitor.left + 100, monitor.top + 100,
                    monitor.left + 100 + 300 * scale, monitor.top + 100 + 200 * scale};
                Point finalPoint{panel.left, panel.top};
                for (Edge edge : {Edge::Left, Edge::Top, Edge::Right, Edge::Bottom})
                {
                    Point from = StartPoint(panel, monitor, edge);
                    pass = pass && (edge != Edge::Left || from.x + panel.right - panel.left == monitor.left) &&
                        (edge != Edge::Top || from.y + panel.bottom - panel.top == monitor.top) &&
                        (edge != Edge::Right || from.x == monitor.right) &&
                        (edge != Edge::Bottom || from.y == monitor.bottom);
                    Point begin = Slide(finalPoint, from, -1);
                    Point finish = Slide(finalPoint, from, 2);
                    pass = pass && begin.x == from.x && begin.y == from.y &&
                        finish.x == finalPoint.x && finish.y == finalPoint.y;
                    Point previous = from;
                    for (int i = 0; i <= 280; ++i)
                    {
                        Point p = Slide(finalPoint, from, i / 280.0);
                        bool increasing = edge == Edge::Left || edge == Edge::Top;
                        int value = Horizontal(edge) ? p.x : p.y;
                        int last = Horizontal(edge) ? previous.x : previous.y;
                        int target = Horizontal(edge) ? finalPoint.x : finalPoint.y;
                        pass = pass && (increasing ? value >= last && value <= target : value <= last && value >= target) &&
                            (Horizontal(edge) ? p.y == finalPoint.y : p.x == finalPoint.x);
                        previous = p;
                    }
                }
            }
            pass = pass && TaskbarEdge(monitor, {monitor.left, monitor.top, monitor.left + 48, monitor.bottom}) == Edge::Left &&
                TaskbarEdge(monitor, {monitor.left, monitor.top, monitor.right, monitor.top + 48}) == Edge::Top &&
                TaskbarEdge(monitor, {monitor.right - 48, monitor.top, monitor.right, monitor.bottom}) == Edge::Right &&
                TaskbarEdge(monitor, {monitor.left, monitor.bottom - 48, monitor.right, monitor.bottom}) == Edge::Bottom &&
                // Fully hidden taskbars just outside all four physical edges.
                TaskbarEdge(monitor, {monitor.left - 48, monitor.top, monitor.left, monitor.bottom}) == Edge::Left &&
                TaskbarEdge(monitor, {monitor.left, monitor.top - 48, monitor.right, monitor.top}) == Edge::Top &&
                TaskbarEdge(monitor, {monitor.right, monitor.top, monitor.right + 48, monitor.bottom}) == Edge::Right &&
                TaskbarEdge(monitor, {monitor.left, monitor.bottom, monitor.right, monitor.bottom + 48}) == Edge::Bottom;
        }
        pass = pass && IsTarget(L"StartMenuExperienceHost.exe", L"Windows.UI.Core.CoreWindow", L"") &&
            IsTarget(L"ShellHost.exe", L"ControlCenterWindow", L"") &&
            IsTarget(L"ShellExperienceHost.exe", L"Windows.UI.Core.CoreWindow", L"ActionCenter") &&
            IsTarget(L"explorer.exe", L"TopLevelWindowForOverflowXamlIsland", L"") &&
            !IsTarget(L"SystemSettings.exe", L"Windows.UI.Core.CoreWindow", L"") &&
            !IsTarget(L"ShellExperienceHost.exe", L"Windows.UI.Core.CoreWindow", L"Share") &&
            !IsTarget(L"explorer.exe", L"Shell_TrayWnd", L"") &&
            !IsTarget(L"notepad.exe", L"ControlCenterWindow", L"");
        Log(pass ? L"PASS: four taskbar edges, negative monitor coordinates, scaled panels, auto-hide geometry, easing and target isolation" : L"FAIL: self-test");
        return pass ? 0 : 1;
    }

    // Exercise real Win32 positioning and the event/timer state machine using
    // only a window created by this test. Never move existing shell windows.
    int IntegrationTest()
    {
        BOOL animations = FALSE;
        if (!SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0) || !animations)
        { Log(L"SKIP: integration test requires Windows Animation effects on"); return 3; }
        HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSW controlClass{};
        controlClass.lpfnWndProc = WindowProc;
        controlClass.hInstance = instance;
        controlClass.lpszClassName = kWindowClass;
        RegisterClassW(&controlClass);
        g_control = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, instance, nullptr);
        constexpr wchar_t testClassName[] = L"TransparencyTweak.FlyoutAnimation.Test";
        WNDCLASSW testClass{};
        testClass.lpfnWndProc = DefWindowProcW;
        testClass.hInstance = instance;
        testClass.lpszClassName = testClassName;
        RegisterClassW(&testClass);
        HWND hwnd = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, testClassName,
            L"Shell animation lifecycle test", WS_POPUP, 100, 100, 300, 200,
            nullptr, nullptr, instance, nullptr);
        if (!g_control || !hwnd)
        {
            if (hwnd) DestroyWindow(hwnd);
            if (g_control) DestroyWindow(g_control);
            UnregisterClassW(testClassName, instance);
            UnregisterClassW(kWindowClass, instance);
            Log(L"SKIP: test desktop cannot create windows");
            return 3;
        }
        State state;
        state.identity = {GetCurrentProcessId(), GetCurrentThreadId(), testClassName};
        g_states.emplace(hwnd, state);
        RECT original{};
        GetWindowRect(hwnd, &original);
        TaskbarPlacement testPlacement{};
        g_testPlacement = &testPlacement;
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        if (!IsOpen(hwnd))
        {
            g_states.clear();
            g_testPlacement = nullptr;
            DestroyWindow(hwnd);
            DestroyWindow(g_control);
            UnregisterClassW(testClassName, instance);
            UnregisterClassW(kWindowClass, instance);
            Log(L"SKIP: test desktop cannot expose an uncloaked window");
            return 3;
        }
        auto& active = g_states.at(hwnd);
        RECT rect{};
        bool pass = true;
        auto check = [&](bool ok, wchar_t const* label)
        {
            if (!ok) Log(L"CHECK FAILED: %s rect=%ld,%ld,%ld,%ld opening=%d timer=%d", label,
                rect.left, rect.top, rect.right, rect.bottom, active.animating, g_timer);
            pass = pass && ok;
        };
        auto event = [&](DWORD type) { OnEvent(nullptr, type, hwnd, OBJID_WINDOW, CHILDID_SELF, 0, GetTickCount()); };
        auto reopen = [&]
        {
            ShowWindow(hwnd, SW_HIDE); event(EVENT_OBJECT_HIDE);
            ShowWindow(hwnd, SW_SHOWNOACTIVATE); event(EVENT_OBJECT_SHOW);
        };
        for (Edge edge : {Edge::Left, Edge::Top, Edge::Right, Edge::Bottom})
        {
            testPlacement.edge = edge;
            reopen();
            GetWindowRect(hwnd, &rect);
            check(active.animating && g_timer && rect.left == active.from.x && rect.top == active.from.y, L"opening start");
            ULONGLONG firstStart = active.start;
            event(EVENT_OBJECT_UNCLOAKED);
            check(active.start == firstStart, L"duplicate open"); // SHOW + UNCLOAK must not restart.
            active.start = GetTickCount64() - kDurationMs / 2;
            Tick();
            GetWindowRect(hwnd, &rect);
            bool horizontal = flyout_animation::Horizontal(edge);
            int coordinate = horizontal ? rect.left : rect.top;
            int target = horizontal ? original.left : original.top;
            int source = horizontal ? active.from.x : active.from.y;
            check(coordinate > std::min(target, source) && coordinate < std::max(target, source) &&
                (horizontal ? rect.top == original.top : rect.left == original.left) &&
                rect.right - rect.left == original.right - original.left &&
                rect.bottom - rect.top == original.bottom - original.top, L"opening midpoint");
            active.start = GetTickCount64() - kDurationMs;
            Tick();
            GetWindowRect(hwnd, &rect);
            check(!active.animating && !g_timer && EqualRect(&rect, &original), L"opening complete");
            reopen();
            check(active.animating, L"early close start");
            ShowWindow(hwnd, SW_HIDE); event(EVENT_OBJECT_HIDE);
            GetWindowRect(hwnd, &rect);
            check(!active.open && !active.animating && !g_timer && EqualRect(&rect, &original), L"early close restores source");
            // If taskbar side changes during the slide, restore before the next open.
            reopen();
            testPlacement.edge = edge == Edge::Left ? Edge::Right : Edge::Left;
            Tick();
            GetWindowRect(hwnd, &rect);
            check(!active.animating && !g_timer && EqualRect(&rect, &original), L"edge change");
        }
        // Shutdown restoration shares the production Restore path.
        reopen();
        Restore(hwnd, active);
        UpdateTimer();
        GetWindowRect(hwnd, &rect);
        check(!g_timer && EqualRect(&rect, &original), L"shutdown restoration");
        DestroyWindow(hwnd);
        OnEvent(nullptr, EVENT_OBJECT_DESTROY, hwnd, OBJID_WINDOW, CHILDID_SELF, 0, GetTickCount());
        // active is invalid after the erase, so don't use the check lambda here.
        pass = pass && g_states.empty();
        g_testPlacement = nullptr;
        DestroyWindow(g_control);
        UnregisterClassW(testClassName, instance);
        UnregisterClassW(kWindowClass, instance);
        Log(pass ? L"PASS: Win32 slides on all four edges, edge changes, duplicate open, completion, hide, shutdown, destruction and idle timer" :
            L"FAIL: Win32 animation lifecycle");
        return pass ? 0 : 1;
    }

}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 2 && wcscmp(argv[1], L"--self-test") == 0) return SelfTest();
    if (argc == 2 && wcscmp(argv[1], L"--integration-test") == 0)
    {
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return IntegrationTest();
    }
    bool stop = argc == 2 && wcscmp(argv[1], L"--stop") == 0;
    bool diagnose = argc == 2 && wcscmp(argv[1], L"--diagnose") == 0;
    if (argc > 1 && !stop && !diagnose) return 2;
    if (stop)
    {
        HWND control = FindWindowExW(HWND_MESSAGE, nullptr, kWindowClass, nullptr);
        return !control || PostMessageW(control, kStop, 0, 0) ? 0 : 1;
    }
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, ARRAYSIZE(path)))
    {
        std::wstring logPath = std::wstring(path) + L".log";
        g_log = _wfsopen(logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    }
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    Log(L"Experimental shell slide controller starting; duration=%llu", kDurationMs);
    EnumWindows(Inventory, 0);
    if (diagnose) { Log(L"recognized targets=%zu", g_states.size()); return g_states.empty() ? 3 : 0; }
    // A sandbox desktop cannot control shell windows on the user's desktop.
    // Refuse to leave a useless background process there.
    if (g_states.empty()) { Log(L"No shell targets on this desktop; controller not started."); return 3; }
    HANDLE singleton = CreateMutexW(nullptr, TRUE, L"Local\\TransparencyTweak.ShellFlyoutAnimation.Experimental");
    if (!singleton) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(singleton); return 0; }
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kWindowClass;
    RegisterClassW(&cls);
    g_control = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, cls.hInstance, nullptr);
    if (!g_control) { CloseHandle(singleton); return 1; }
    std::vector<HWINEVENTHOOK> hooks;
    for (auto range : {std::pair{EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE},
                       std::pair{EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED}})
    {
        HWINEVENTHOOK hook = SetWinEventHook(range.first, range.second, nullptr, OnEvent, 0, 0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
        if (!hook) { Log(L"event hook failed error=%lu", GetLastError()); break; }
        hooks.push_back(hook);
    }
    int exitCode = 1;
    if (hooks.size() == 2)
    {
        Log(L"controller ready; --stop restores active slides and exits");
        MSG message{};
        BOOL received;
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0)
        { TranslateMessage(&message); DispatchMessageW(&message); }
        exitCode = received == 0 ? 0 : 1;
    }
    for (HWINEVENTHOOK hook : hooks) UnhookWinEvent(hook);
    if (g_timer) KillTimer(g_control, kTick);
    for (auto& [hwnd, state] : g_states) { Restore(hwnd, state); }
    DestroyWindow(g_control);
    UnregisterClassW(kWindowClass, cls.hInstance);
    ReleaseMutex(singleton);
    CloseHandle(singleton);
    Log(L"controller stopped");
    if (g_log) fclose(g_log);
    return exitCode;
}
