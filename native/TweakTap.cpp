// TweakTap.dll - XAML diagnostics TAP in explorer.exe; ordinary DLL bootstrap in Settings.
// Settings must not initialize XAML diagnostics (Background crashes on build 26300).
//
// explorer.exe      : sets the opacity of the taskbar background (Rectangle#BackgroundFill/#BackgroundStroke)
//                     from HKCU\Software\TransparencyTweak\TaskbarTransparency (0-100 %) and follows changes
//                     live. While the value is above 0 the taskbar window's own backdrop is made clear so the
//                     desktop shows through. Everything returns to the original values at 0 % or when
//                     Windows' "Transparency effects" is off. Also loads this DLL into Settings when it opens.
// SystemSettings.exe: adds a "Taskbar transparency" slider under Personalization > Colors >
//                     Transparency effects. The slider writes the registry value above.
//
// Debug: HKCU\Software\TransparencyTweak\DebugDump = 1 records the visual tree to
// %LOCALAPPDATA%\TransparencyTweak\logs.

#include <windows.h>
#include <tlhelp32.h>
#include <unknwn.h>
#include <ocidl.h>
#include <xamlOM.h>
#undef GetCurrentTime
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Data.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Shapes.h>

#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <share.h>
#include <mutex>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "SettingsInjection.h"

#pragma comment(linker, "/EXPORT:DllGetClassObject,PRIVATE")
#pragma comment(linker, "/EXPORT:DllCanUnloadNow,PRIVATE")

// {1B5D6C2E-6E0A-4C55-9C35-7A0E3B8F21D4}
static constexpr CLSID CLSID_TweakTap = {0x1b5d6c2e, 0x6e0a, 0x4c55, {0x9c, 0x35, 0x7a, 0x0e, 0x3b, 0x8f, 0x21, 0xd4}};

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = winrt::Windows::UI::Xaml::Controls;
using winrt::Windows::System::DispatcherQueue;

namespace
{
    HMODULE g_module;
    constexpr wchar_t kAppKey[] = L"Software\\TransparencyTweak";
    constexpr wchar_t kPersonalizeKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";

    DWORD ReadDword(const wchar_t* key, const wchar_t* name, DWORD fallback)
    {
        DWORD value = 0, size = sizeof(value);
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS)
            return value;
        return fallback;
    }

    std::wstring ProcessName()
    {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring name = path;
        size_t slash = name.find_last_of(L'\\');
        if (slash != std::wstring::npos) name = name.substr(slash + 1);
        size_t dot = name.find_last_of(L'.');
        if (dot != std::wstring::npos) name = name.substr(0, dot);
        return name;
    }

    std::wstring LogPath(const wchar_t* suffix)
    {
        wchar_t dir[MAX_PATH]{};
        ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\TransparencyTweak\\logs", dir, MAX_PATH);
        CreateDirectoryW(dir, nullptr); // parent is created by the installer
        // One file per load: an older copy of this DLL may still hold its own log open.
        static const std::wstring stamp = []
        {
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t buf[48];
            swprintf_s(buf, L"-%lu-%02d%02d%02d", GetCurrentProcessId(), t.wHour, t.wMinute, t.wSecond);
            return std::wstring(buf);
        }();
        return std::wstring(dir) + L"\\" + ProcessName() + stamp + suffix;
    }

    // Small append-only UTF-8 log; the tap must never throw into the host process.
    class Log
    {
    public:
        void Open(const std::wstring& path)
        {
            std::lock_guard lock(m_mutex);
            if (!m_file) m_file = _wfsopen(path.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        }

        void Write(const wchar_t* fmt, ...)
        {
            std::lock_guard lock(m_mutex);
            if (!m_file) return;
            SYSTEMTIME t;
            GetLocalTime(&t);
            fwprintf(m_file, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
            va_list args;
            va_start(args, fmt);
            vfwprintf(m_file, fmt, args);
            va_end(args);
            fputwc(L'\n', m_file);
            fflush(m_file);
        }

    private:
        std::mutex m_mutex;
        FILE* m_file = nullptr;
    };

    Log g_log;
    Log g_tree;

    bool Is(const wchar_t* a, const wchar_t* b) { return a && wcscmp(a, b) == 0; }

    // ------------------------------------------------------------------ taskbar

    // Opacity for the taskbar background, or < 0 for "leave Windows' own value".
    double WantedTaskbarOpacity()
    {
        if (ReadDword(kPersonalizeKey, L"EnableTransparency", 1) == 0) return -1;
        DWORD percent = ReadDword(kAppKey, L"TaskbarTransparency", 0);
        if (percent == 0) return -1;
        return 1.0 - (percent > 100 ? 100 : percent) / 100.0;
    }

    // The XAML background sits on top of the taskbar window's own DWM backdrop, which is opaque.
    // While a transparency level is active that backdrop is switched to a fully clear gradient;
    // the value explorer had before is remembered per window and put back afterwards.
    class TrayBackdrop
    {
    public:
        void Update(bool clear)
        {
            std::lock_guard lock(m_mutex);
            if (!s_set) return;
            m_clear = clear;
            EnumWindows([](HWND hwnd, LPARAM self) -> BOOL
            {
                DWORD pid = 0;
                GetWindowThreadProcessId(hwnd, &pid);
                wchar_t cls[64]{};
                GetClassNameW(hwnd, cls, 64);
                if (pid == GetCurrentProcessId() && (Is(cls, L"Shell_TrayWnd") || Is(cls, L"Shell_SecondaryTrayWnd")))
                    reinterpret_cast<TrayBackdrop*>(self)->UpdateWindow(hwnd);
                return TRUE;
            }, reinterpret_cast<LPARAM>(this));
        }

    private:
        struct AccentPolicy { int state; int flags; DWORD color; int animation; };
        struct CompositionData { int attribute; void* data; ULONG size; };
        using CompositionFn = BOOL(WINAPI*)(HWND, CompositionData*);
        static constexpr int kAccentPolicy = 19;
        static constexpr int kTransparentGradient = 2;

        void UpdateWindow(HWND hwnd)
        {
            if (m_clear)
            {
                if (!m_original.count(hwnd))
                {
                    AccentPolicy current{};
                    CompositionData query{kAccentPolicy, &current, sizeof(current)};
                    m_original[hwnd] = (s_get && s_get(hwnd, &query)) ? current : AccentPolicy{};
                }
                AccentPolicy clear{kTransparentGradient, 2, 0x00000000, 0};
                CompositionData data{kAccentPolicy, &clear, sizeof(clear)};
                BOOL applied = s_set(hwnd, &data);
                g_log.Write(L"taskbar backdrop hwnd=%p clear=%d result=%d error=%lu", hwnd, 1, applied, applied ? 0 : GetLastError());
            }
            else if (auto it = m_original.find(hwnd); it != m_original.end())
            {
                AccentPolicy original = it->second;
                CompositionData data{kAccentPolicy, &original, sizeof(original)};
                s_set(hwnd, &data);
                m_original.erase(it);
            }
        }

        static inline CompositionFn s_set = reinterpret_cast<CompositionFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute"));
        static inline CompositionFn s_get = reinterpret_cast<CompositionFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetWindowCompositionAttribute"));

        std::mutex m_mutex;
        std::map<HWND, AccentPolicy> m_original;
        bool m_clear = false;
    };

    TrayBackdrop g_backdrop;

    class TaskbarBackground
    {
    public:
        // Called on the XAML thread that owns the element.
        void Track(wux::UIElement const& element)
        {
            Entry entry{winrt::make_weak(element), DispatcherQueue::GetForCurrentThread(), element.Dispatcher(), element.Opacity(), std::make_shared<BrushState>()};
            if (auto shape = element.try_as<wux::Shapes::Shape>())
            {
                entry.brush->original = shape.Fill();
                shape.RegisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(), [entry](auto const& sender, auto const&)
                {
                    try
                    {
                        if (entry.brush->applying) return;
                        // Windows can replace the brush after loading or changing
                        // theme. Keep that brush for restoration, then reapply.
                        entry.brush->original = sender.template as<wux::Shapes::Shape>().Fill();
                        Apply(entry);
                    }
                    catch (...)
                    {
                        g_log.Write(L"taskbar brush callback failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                    }
                });
            }
            g_log.Write(L"taskbar dispatchers queue=%d core=%d original=%.3f", !!entry.queue, !!entry.dispatcher, entry.original);
            {
                std::lock_guard lock(m_mutex);
                m_entries.push_back(entry);
            }
            Apply(entry);
            g_backdrop.Update(WantedTaskbarOpacity() >= 0);
            StartWatching();
        }

        void ApplyAll()
        {
            g_backdrop.Update(WantedTaskbarOpacity() >= 0);
            std::vector<Entry> entries;
            {
                std::lock_guard lock(m_mutex);
                entries = m_entries;
            }
            for (auto const& entry : entries)
            {
                try
                {
                    if (entry.queue)
                    {
                        if (!entry.queue.TryEnqueue([entry] { Apply(entry); }))
                            g_log.Write(L"taskbar dispatcher rejected update");
                    }
                    else if (entry.dispatcher)
                        entry.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [entry] { Apply(entry); });
                    else
                        g_log.Write(L"taskbar has no dispatcher: update skipped");
                }
                catch (...)
                {
                    g_log.Write(L"taskbar dispatch failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
            }
        }

    private:
        struct BrushState
        {
            // Accessed only on the element's UI thread.
            wux::Media::Brush original{nullptr};
            bool applying = false;
        };

        struct Entry
        {
            winrt::weak_ref<wux::UIElement> element;
            DispatcherQueue queue{nullptr};
            winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
            double original = 1.0;
            std::shared_ptr<BrushState> brush;
        };

        static void Apply(Entry const& entry)
        {
            try
            {
                auto element = entry.element.get();
                if (!element) return;
                double wanted = WantedTaskbarOpacity();
                auto shape = element.try_as<wux::Shapes::Shape>();
                if (shape && entry.brush->original)
                {
                    entry.brush->applying = true;
                    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{entry.brush->applying};
                    element.Opacity(entry.original);
                    if (wanted < 0)
                        shape.Fill(entry.brush->original);
                    else
                    {
                        // A solid brush with explicit alpha avoids depending on
                        // the native Acrylic brush's backdrop/composition effect.
                        winrt::Windows::UI::Color color = ReadDword(kPersonalizeKey, L"SystemUsesLightTheme", 0)
                            ? winrt::Windows::UI::Color{255, 243, 243, 243}
                            : winrt::Windows::UI::Color{255, 32, 32, 32};
                        if (auto solid = entry.brush->original.try_as<wux::Media::SolidColorBrush>()) color = solid.Color();
                        else if (auto acrylic = entry.brush->original.try_as<wux::Media::AcrylicBrush>()) color = acrylic.TintColor();
                        wux::Media::SolidColorBrush brush(color);
                        brush.Opacity(wanted * entry.brush->original.Opacity());
                        shape.Fill(brush);
                    }
                    auto fe = element.as<wux::FrameworkElement>();
                    g_log.Write(L"taskbar brush %s requested=%.3f actual=%.3f size=%.0fx%.0f", fe.Name().c_str(),
                        wanted, shape.Fill().Opacity(), fe.ActualWidth(), fe.ActualHeight());
                }
                else
                    g_log.Write(L"taskbar waiting for native fill brush");
            }
            catch (...)
            {
                g_log.Write(L"apply failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        }

        // One background thread waits for registry changes; no polling, no timers.
        void StartWatching()
        {
            std::call_once(m_watchOnce, [this]
            {
                std::thread([this]
                {
                    HKEY app = nullptr, personalize = nullptr;
                    RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &app, nullptr);
                    RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_NOTIFY | KEY_READ, &personalize);
                    HANDLE events[2] = {CreateEventW(nullptr, FALSE, FALSE, nullptr), CreateEventW(nullptr, FALSE, FALSE, nullptr)};
                    for (;;)
                    {
                        if (app) RegNotifyChangeKeyValue(app, FALSE, REG_NOTIFY_CHANGE_LAST_SET, events[0], TRUE);
                        if (personalize) RegNotifyChangeKeyValue(personalize, FALSE, REG_NOTIFY_CHANGE_LAST_SET, events[1], TRUE);
                        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_FAILED) break;
                        ApplyAll();
                    }
                }).detach();
            });
        }

        std::mutex m_mutex;
        std::vector<Entry> m_entries;
        std::once_flag m_watchOnce;
    };

    TaskbarBackground g_taskbar;

    // ------------------------------------------------------- reaching Settings

    // A UWP app's CoreWindow is either top-level (while starting) or a child of
    // ApplicationFrameWindow, which belongs to ApplicationFrameHost.exe.
    bool HasCoreWindow(DWORD pid)
    {
        auto owned = [pid](HWND hwnd)
        {
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            return owner == pid;
        };
        for (HWND w = nullptr; (w = FindWindowExW(nullptr, w, L"Windows.UI.Core.CoreWindow", nullptr));)
            if (owned(w)) return true;
        for (HWND frame = nullptr; (frame = FindWindowExW(nullptr, frame, L"ApplicationFrameWindow", nullptr));)
            for (HWND w = nullptr; (w = FindWindowExW(frame, w, L"Windows.UI.Core.CoreWindow", nullptr));)
                if (owned(w)) return true;
        return false;
    }

    // Lives inside explorer, so no extra process is needed: every new SystemSettings.exe gets this
    // DLL as soon as its window exists.
    void WatchForSettings()
    {
        static std::once_flag once;
        std::call_once(once, []
        {
            std::thread([]
            {
                wchar_t self[MAX_PATH]{};
                GetModuleFileNameW(g_module, self, MAX_PATH);
                std::map<DWORD, int> attempts; // pid -> tries so far; kept until the process exits
                for (;;)
                {
                    Sleep(1500);
                    std::set<DWORD> alive;
                    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
                    if (snapshot == INVALID_HANDLE_VALUE) continue;
                    PROCESSENTRY32W entry{sizeof(entry)};
                    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
                        if (_wcsicmp(entry.szExeFile, L"SystemSettings.exe") == 0) alive.insert(entry.th32ProcessID);
                    CloseHandle(snapshot);

                    for (auto it = attempts.begin(); it != attempts.end();)
                        it = alive.count(it->first) ? std::next(it) : attempts.erase(it);

                    for (DWORD pid : alive)
                    {
                        int& tries = attempts[pid];
                        if (tries < 0 || tries >= 10 || !HasCoreWindow(pid)) continue;
                        ++tries;
                        HRESULT hr = settings_injection::Inject(pid, self, g_module);
                        g_log.Write(L"SystemSettings pid %lu: inject hr=0x%08X (try %d)", pid, static_cast<unsigned>(hr), tries);
                        if (SUCCEEDED(hr)) tries = -1; // done for this process
                    }
                }
            }).detach();
        });
    }

    // ------------------------------------------------------- the Settings slider

    constexpr wchar_t kTransparencyToggleId[] = L"SystemSettings_Personalize_Color_EnableTransparency_ToggleSwitch";
    constexpr wchar_t kSliderRowName[] = L"TransparencyTweak_TaskbarRow";

    void WriteTaskbarTransparency(int percent)
    {
        DWORD value = static_cast<DWORD>(percent);
        RegSetKeyValueW(HKEY_CURRENT_USER, kAppKey, L"TaskbarTransparency", REG_DWORD, &value, sizeof(value));
    }

    // Row layout:  [Transparency effects / Windows and surfaces appear translucent]      On [toggle]
    //              [Taskbar transparency  ----o----  40%]                       <- added here
    void AttachSlider(wuxc::ToggleSwitch const& toggle)
    {
        using wux::Automation::AutomationProperties;
        using wux::Media::VisualTreeHelper;

        if (AutomationProperties::GetAutomationId(toggle) != kTransparencyToggleId) return;
        auto presenter = VisualTreeHelper::GetParent(toggle);
        auto reflow = presenter ? VisualTreeHelper::GetParent(presenter).try_as<wuxc::Panel>() : nullptr;
        if (!reflow || reflow.Children().Size() == 0) return;
        auto text = reflow.Children().GetAt(0).try_as<wuxc::StackPanel>();
        if (!text) return;
        for (auto const& child : text.Children())
            if (auto fe = child.try_as<wux::FrameworkElement>(); fe && fe.Name() == kSliderRowName) return;

        wuxc::StackPanel row;
        row.Name(kSliderRowName);
        row.Orientation(wuxc::Orientation::Horizontal);
        row.Margin({0, 6, 0, 2});

        auto secondary = wux::Application::Current().Resources().TryLookup(winrt::box_value(L"TextFillColorSecondaryBrush"));

        wuxc::TextBlock label;
        label.Text(L"Taskbar transparency");
        label.VerticalAlignment(wux::VerticalAlignment::Center);
        label.Margin({0, 0, 16, 0});
        if (auto brush = secondary.try_as<wux::Media::Brush>()) label.Foreground(brush);

        DWORD savedPercent = ReadDword(kAppKey, L"TaskbarTransparency", 0);
        int percent = static_cast<int>(savedPercent > 100 ? 100 : savedPercent);
        wuxc::Slider slider;
        slider.Minimum(0);
        slider.Maximum(100);
        slider.StepFrequency(1);
        slider.SmallChange(1);
        slider.LargeChange(10);
        slider.Width(220);
        slider.VerticalAlignment(wux::VerticalAlignment::Center);
        slider.Value(percent);
        AutomationProperties::SetName(slider, L"Taskbar transparency");

        wuxc::TextBlock value;
        value.Text(winrt::to_hstring(static_cast<int>(slider.Value())) + L"%");
        value.MinWidth(40);
        value.Margin({12, 0, 0, 0});
        value.VerticalAlignment(wux::VerticalAlignment::Center);

        slider.ValueChanged([value](auto const&, wuxc::Primitives::RangeBaseValueChangedEventArgs const& e)
        {
            try
            {
                int v = static_cast<int>(std::lround(e.NewValue()));
                value.Text(winrt::to_hstring(v) + L"%");
                WriteTaskbarTransparency(v);
            }
            catch (...)
            {
                g_log.Write(L"slider update failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        });

        // Follows the Transparency effects switch next to it. A binding rather than reading IsOn now:
        // the switch may not have its value yet when it is first loaded.
        wux::Data::Binding enabled;
        enabled.Source(toggle);
        enabled.Path(wux::PropertyPath(L"IsOn"));
        enabled.Mode(wux::Data::BindingMode::OneWay);
        slider.SetBinding(wuxc::Control::IsEnabledProperty(), enabled);

        row.Children().Append(label);
        row.Children().Append(slider);
        row.Children().Append(value);
        text.Children().Append(row);
        g_log.Write(L"slider attached (value %d%%)", percent);
    }

    wuxc::ToggleSwitch FindTransparencyToggle(wux::DependencyObject const& node, int depth)
    {
        using wux::Media::VisualTreeHelper;
        if (auto toggle = node.try_as<wuxc::ToggleSwitch>())
            return wux::Automation::AutomationProperties::GetAutomationId(toggle) == kTransparencyToggleId ? toggle : nullptr;
        if (depth > 64) return nullptr;
        int count = VisualTreeHelper::GetChildrenCount(node);
        for (int i = 0; i < count; ++i)
            if (auto found = FindTransparencyToggle(VisualTreeHelper::GetChild(node, i), depth + 1)) return found;
        return nullptr;
    }

    // Runs only on the Settings UI thread, reached through CoreApplication's
    // dispatcher without initializing any XAML diagnostics session.
    void StartSettingsScanner()
    {
        struct Holder { wux::DispatcherTimer timer{nullptr}; };
        static auto* holder = new Holder(); // never destroyed: no XAML calls at process exit
        if (holder->timer) return;
        holder->timer = wux::DispatcherTimer();
        auto& timer = holder->timer;
        timer.Interval(std::chrono::seconds(1));
        timer.Tick([](auto const&, auto const&)
        {
            try
            {
                auto window = wux::Window::Current();
                auto content = window ? window.Content() : nullptr;
                if (!content) return;
                if (auto toggle = FindTransparencyToggle(content, 0)) AttachSlider(toggle);
            }
            catch (...)
            {
                g_log.Write(L"scan failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        });
        timer.Start();
        g_log.Write(L"settings scanner started");
    }

    // ---------------------------------------------------------------- the tap

    enum class Role { None, Explorer, Settings };

    Role CurrentRole()
    {
        std::wstring name = ProcessName();
        if (_wcsicmp(name.c_str(), L"explorer") == 0) return Role::Explorer;
        if (_wcsicmp(name.c_str(), L"SystemSettings") == 0) return Role::Settings;
        return Role::None;
    }

    struct VisualTreeWatcher : winrt::implements<VisualTreeWatcher, IVisualTreeServiceCallback2, winrt::non_agile>
    {
        explicit VisualTreeWatcher(winrt::com_ptr<IUnknown> const& site) : m_diag(site.as<IXamlDiagnostics>()) {}

        void Start()
        {
            m_role = CurrentRole();
            if (m_role == Role::Explorer) WatchForSettings();
            if (m_role == Role::Settings)
            {
                g_log.Write(L"Settings must be loaded through StartSettingsTweak, not XAML diagnostics");
                return;
            }
            if (m_role != Role::Explorer) return;
            m_dump = ReadDword(kAppKey, L"DebugDump", 0) != 0;
            if (m_dump) g_tree.Open(LogPath(L"-tree.log"));
            // AdviseVisualTreeChange replays the existing tree synchronously on the UI thread,
            // so it must not be called from the thread that delivered SetSite.
            std::thread([self = get_strong()]
            {
                try
                {
                    winrt::init_apartment(winrt::apartment_type::multi_threaded);
                    auto service = self->m_diag.as<IVisualTreeService3>();
                    HRESULT hr = service->AdviseVisualTreeChange(self.get());
                    g_log.Write(L"AdviseVisualTreeChange hr=0x%08X", static_cast<unsigned>(hr));
                }
                catch (...)
                {
                    g_log.Write(L"visual tree subscription failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
                }
            }).detach();
        }

        void Stop()
        {
            if (m_role == Role::Explorer)
            {
                if (auto service = m_diag.try_as<IVisualTreeService3>())
                    service->UnadviseVisualTreeChange(this);
            }
        }

        HRESULT STDMETHODCALLTYPE OnVisualTreeChange(ParentChildRelation relation, VisualElement element,
                                                     VisualMutationType mutation) noexcept override
        {
            try
            {
                if (m_dump)
                    g_tree.Write(L"%c parent=%llX child=%llX idx=%u type=%s name=%s",
                                 mutation == Add ? L'A' : L'R',
                                 relation.Parent, relation.Child, relation.ChildIndex,
                                 element.Type ? element.Type : L"", element.Name ? element.Name : L"");
                if (mutation != Add) return S_OK;
                if (m_role == Role::Explorer && Is(element.Type, L"Windows.UI.Xaml.Shapes.Rectangle") &&
                    (Is(element.Name, L"BackgroundFill") || Is(element.Name, L"BackgroundStroke")))
                {
                    if (auto ui = ElementFromHandle(element.Handle))
                    {
                        g_log.Write(L"tracking taskbar %s", element.Name);
                        g_taskbar.Track(ui);
                    }
                }
            }
            catch (...)
            {
                g_log.Write(L"OnVisualTreeChange failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE OnElementStateChanged(InstanceHandle, VisualElementState, LPCWSTR) noexcept override
        {
            return S_OK;
        }

    private:
        wux::UIElement ElementFromHandle(InstanceHandle handle)
        {
            winrt::com_ptr<::IInspectable> object;
            if (FAILED(m_diag->GetIInspectableFromHandle(handle, object.put())) || !object) return nullptr;
            return object.try_as<wux::UIElement>();
        }

        winrt::com_ptr<IXamlDiagnostics> m_diag;
        Role m_role = Role::None;
        bool m_dump = false;
    };

    winrt::com_ptr<VisualTreeWatcher> g_watcher;

    struct Tap : winrt::implements<Tap, IObjectWithSite, winrt::non_agile>
    {
        HRESULT STDMETHODCALLTYPE SetSite(IUnknown* site) noexcept override
        {
            try
            {
                if (g_watcher)
                {
                    g_watcher->Stop();
                    g_watcher = nullptr;
                }
                m_site.copy_from(site);
                if (m_site)
                {
                    g_log.Write(L"SetSite: attached to %s (pid %lu)", ProcessName().c_str(), GetCurrentProcessId());
                    g_watcher = winrt::make_self<VisualTreeWatcher>(m_site);
                    g_watcher->Start();
                }
                return S_OK;
            }
            catch (...)
            {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE GetSite(REFIID riid, void** site) noexcept override
        {
            return m_site ? m_site->QueryInterface(riid, site) : E_FAIL;
        }

    private:
        winrt::com_ptr<IUnknown> m_site;
    };

    struct TapFactory : winrt::implements<TapFactory, IClassFactory, winrt::non_agile>
    {
        HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID riid, void** result) noexcept override
        {
            try
            {
                *result = nullptr;
                if (outer) return CLASS_E_NOAGGREGATION;
                return winrt::make_self<Tap>()->QueryInterface(riid, result);
            }
            catch (...)
            {
                return winrt::to_hresult();
            }
        }

        HRESULT STDMETHODCALLTYPE LockServer(BOOL) noexcept override { return S_OK; }
    };
}

// Invoked on a worker thread after ordinary LoadLibrary has completed. Keep
// WinRT activation and dispatcher access outside DllMain's loader lock.
extern "C" __declspec(dllexport) DWORD WINAPI StartSettingsTweak(void*) noexcept
{
    try
    {
        if (CurrentRole() != Role::Settings) return static_cast<DWORD>(E_INVALIDARG);
        if (ReadDword(kAppKey, L"SettingsSlider", 1) == 0) return S_OK;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        struct Apartment { ~Apartment() { winrt::uninit_apartment(); } } apartment;
        auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().CoreWindow().Dispatcher();
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, []
        {
            try { StartSettingsScanner(); }
            catch (...)
            {
                g_log.Write(L"settings scanner startup failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            }
        });
        g_log.Write(L"settings scanner queued without XAML diagnostics");
        return S_OK;
    }
    catch (...)
    {
        HRESULT hr = winrt::to_hresult();
        g_log.Write(L"Settings bootstrap failed hr=0x%08X", static_cast<unsigned>(hr));
        return static_cast<DWORD>(hr);
    }
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** result)
{
    try
    {
        *result = nullptr;
        if (clsid != CLSID_TweakTap) return CLASS_E_CLASSNOTAVAILABLE;
        return winrt::make_self<TapFactory>()->QueryInterface(riid, result);
    }
    catch (...)
    {
        return winrt::to_hresult();
    }
}

// Stays loaded for the lifetime of the host; the visual tree callbacks point into this module.
STDAPI DllCanUnloadNow() { return S_FALSE; }

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(instance);
        g_module = instance;
        g_log.Open(LogPath(L".log"));
        g_log.Write(L"loaded into %s (pid %lu)", ProcessName().c_str(), GetCurrentProcessId());
    }
    return TRUE;
}
