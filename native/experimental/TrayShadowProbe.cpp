// Isolated XAML island: exercises real shadow attachment/update/cleanup without
// injecting Explorer, changing registry, or showing a window.
#include <windows.h>
#undef GetCurrentTime
#include <windows.ui.xaml.hosting.desktopwindowxamlsource.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Core.h>
#include <winrt/Windows.UI.Xaml.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Hosting.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <winrt/Windows.UI.Xaml.Media.Imaging.h>
#include <atomic>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
#include <cstdio>
#include <stdexcept>

namespace wux = winrt::Windows::UI::Xaml;
namespace wuxc = wux::Controls;
using winrt::Windows::System::DispatcherQueue;
using InstanceHandle = uint64_t;
static double g_opacity = 0.5;
static DWORD g_enabled = 1;
static constexpr wchar_t kAppKey[] = L"probe";
static double WantedTaskbarOpacity() { return g_opacity; }
static DWORD ReadDword(wchar_t const*, wchar_t const*, DWORD) { return g_enabled; }
struct ProbeLog
{
    unsigned failures = 0;
    void Write(wchar_t const* format, ...)
    {
        if (wcsstr(format, L"failed") || wcsstr(format, L"rejected")) ++failures;
        va_list args;
        va_start(args, format);
        vwprintf(format, args);
        wprintf(L"\n");
        va_end(args);
    }
} g_log;
#include "TrayContentShadow.inl"

static void Require(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

int main()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        auto manager = wux::Hosting::WindowsXamlManager::InitializeForCurrentThread();
        HWND window = CreateWindowExW(0, L"STATIC", L"Tray shadow isolated probe", WS_OVERLAPPED,
            0, 0, 400, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        winrt::check_bool(window != nullptr);
        wux::Hosting::DesktopWindowXamlSource island;
        auto interop = island.as<IDesktopWindowXamlSourceNative>();
        winrt::check_hresult(interop->AttachToWindow(window));
        HWND child = nullptr;
        winrt::check_hresult(interop->get_WindowHandle(&child));
        SetWindowPos(child, nullptr, 0, 0, 400, 48, SWP_NOZORDER | SWP_SHOWWINDOW);
        wuxc::Grid root;
        root.Name(L"TaskbarFrame");
        root.Width(400); root.Height(48);
        wuxc::Grid tray;
        tray.Name(L"SystemTrayFrame");
        root.Children().Append(tray);
        wuxc::TextBlock clock;
        clock.Name(L"Clock");
        clock.Text(L"20:30\n15/10/2026");
        clock.Foreground(wux::Media::SolidColorBrush({255, 255, 255, 255}));
        clock.FontSize(12);
        tray.Children().Append(clock);
        island.Content(root);
        // Let XAML connect its visual tree; the native top-level window stays hidden.
        for (unsigned attempt = 0; attempt < 50 && !clock.IsLoaded(); ++attempt)
        {
            MSG message{};
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        }
        root.Measure({400, 48});
        root.Arrange({0, 0, 400, 48});
        root.UpdateLayout();
        using Preview = wux::Hosting::ElementCompositionPreview;
        Require(TrayContentShadow::InsideTray(clock), "tray scope rejected clock");
        Require(!TrayContentShadow::InsideTray(root), "taskbar background accepted as tray");
        g_trayShadows.Track(clock, 1);
        root.UpdateLayout();
        Require(tray.Children().Size() == 2, "missing shadow host");
        auto host = tray.Children().GetAt(0).as<wuxc::Canvas>();
        Require(!host.IsHitTestVisible() && host.DesiredSize().Width == 0 && host.DesiredSize().Height == 0,
            "shadow host intercepts input or requests layout space");
        Require(host.ActualWidth() >= clock.ActualWidth() && host.ActualHeight() >= clock.ActualHeight(),
            "shadow host was arranged to zero size");
        auto visual = Preview::GetElementChildVisual(host).as<winrt::Windows::UI::Composition::SpriteVisual>();
        auto shadow = visual.Shadow().as<winrt::Windows::UI::Composition::DropShadow>();
        Require(shadow.Mask() != nullptr && !visual.Brush(), "shadow needs glyph mask and no foreground overlay");
        Require(clock.Opacity() == 1 && tray.Children().GetAt(1) == clock, "foreground changed or shadow is above text");
        g_trayShadows.Track(clock, 1);
        Require(tray.Children().Size() == 2, "duplicate tracking creates extra shadow");
        clock.Text(L"20:31\n16/10/2026");
        root.UpdateLayout();
        Require(Preview::GetElementChildVisual(host) == visual, "clock tick recreated visual");
        g_enabled = 0;
        g_trayShadows.ApplyAll();
        Require(tray.Children().Size() == 1, "disable did not remove shadow");
        g_enabled = 1;
        g_trayShadows.ApplyAll();
        Require(tray.Children().Size() == 2, "reenable did not restore shadow");
        g_opacity = -1;
        g_trayShadows.ApplyAll();
        Require(tray.Children().Size() == 1, "native mode retained shadow");
        g_opacity = 0.5;
        g_trayShadows.ApplyAll();
        g_trayShadows.Untrack(1);
        Require(tray.Children().Size() == 1, "untrack retained shadow host");
        clock.Text(L"20:32"); root.UpdateLayout();
        Require(tray.Children().Size() == 1, "layout callback survived untrack");
        wuxc::TextBlock unrelated;
        root.Children().Append(unrelated);
        g_trayShadows.Track(unrelated, 2);
        Require(root.Children().Size() == 2, "non-tray text received shadow");
        wuxc::Grid iconPanel;
        tray.Children().Append(iconPanel);
        wuxc::Image icon;
        icon.Width(16); icon.Height(16);
        icon.Source(wux::Media::Imaging::WriteableBitmap(16, 16));
        iconPanel.Children().Append(icon);
        root.UpdateLayout();
        g_trayShadows.Track(icon, 4);
        root.UpdateLayout();
        Require(iconPanel.Children().Size() == 2, "image icon missing shadow");
        auto iconHost = iconPanel.Children().GetAt(0).as<wuxc::Canvas>();
        auto iconVisual = Preview::GetElementChildVisual(iconHost).as<winrt::Windows::UI::Composition::SpriteVisual>();
        Require(iconVisual.Shadow().as<winrt::Windows::UI::Composition::DropShadow>().Mask() != nullptr,
            "image icon missing alpha mask");
        icon.Source(wux::Media::Imaging::WriteableBitmap(16, 16));
        root.UpdateLayout();
        Require(Preview::GetElementChildVisual(iconHost) == iconVisual, "image change recreated visual");
        g_trayShadows.Track(clock, 3);
        root.UpdateLayout();
        g_trayShadows.Stop();
        Require(tray.Children().Size() == 2 && iconPanel.Children().Size() == 1, "Stop retained shadow");
        // Regression: separate right-aligned clock/date rows, with a StackPanel
        // around the clock. A zero-size host or insertion into the StackPanel
        // does not provide the arranged bounds needed for this layout.
        wuxc::Grid twoLine;
        twoLine.Width(96); twoLine.Height(32);
        for (unsigned row = 0; row < 2; ++row)
        {
            wuxc::RowDefinition definition;
            definition.Height({16, wux::GridUnitType::Pixel});
            twoLine.RowDefinitions().Append(definition);
        }
        wuxc::StackPanel timeRow;
        wuxc::TextBlock time;
        time.Name(L"TimeInnerTextBlock");
        time.Text(L"20:59"); time.FontSize(12);
        time.HorizontalAlignment(wux::HorizontalAlignment::Right);
        timeRow.Children().Append(time);
        wuxc::TextBlock date;
        date.Name(L"DateInnerTextBlock");
        date.Text(L"15/10/2026"); date.FontSize(12);
        date.HorizontalAlignment(wux::HorizontalAlignment::Right);
        wuxc::Grid::SetRow(date, 1);
        twoLine.Children().Append(timeRow);
        twoLine.Children().Append(date);
        tray.Children().Append(twoLine);
        root.UpdateLayout();
        const auto beforeTime = time.TransformToVisual(twoLine).TransformPoint({0, 0});
        const auto beforeDate = date.TransformToVisual(twoLine).TransformPoint({0, 0});
        g_trayShadows.Track(time, 5);
        auto timeHost = twoLine.Children().GetAt(0).as<wuxc::Canvas>();
        g_trayShadows.Track(date, 6);
        auto dateHost = twoLine.Children().GetAt(0).as<wuxc::Canvas>();
        root.UpdateLayout();
        Require(timeRow.Children().Size() == 1, "shadow changed the clock StackPanel layout");
        Require(timeHost.ActualWidth() == 96 && timeHost.ActualHeight() == 32 &&
            dateHost.ActualWidth() == 96 && dateHost.ActualHeight() == 32,
            "two-line shadow hosts do not span both rows");
        const auto afterTime = time.TransformToVisual(twoLine).TransformPoint({0, 0});
        const auto afterDate = date.TransformToVisual(twoLine).TransformPoint({0, 0});
        Require(beforeTime == afterTime && beforeDate == afterDate, "shadow shifted clock/date positions");
        auto timeVisual = Preview::GetElementChildVisual(timeHost).as<winrt::Windows::UI::Composition::SpriteVisual>();
        auto dateVisual = Preview::GetElementChildVisual(dateHost).as<winrt::Windows::UI::Composition::SpriteVisual>();
        Require(timeVisual.Offset().x > dateVisual.Offset().x && timeVisual.Offset().y < dateVisual.Offset().y,
            "right-aligned time and date shadows overlap or have wrong offsets");
        // ActualWidth reports ink width for TextBlock, even when its arranged
        // alpha-mask area is wider. This is the tray clock's Stretch + right
        // text-alignment case: using ink width compresses/displaces its shadow.
        time.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
        time.TextAlignment(wux::TextAlignment::Right);
        root.UpdateLayout();
        printf("CLOCK SIZE: ink=%.1f arranged=%.1f shadow=%.1f\n", time.ActualWidth(),
            time.ActualSize().x, timeVisual.Size().x);
        Require(time.ActualSize().x > time.ActualWidth() + 20,
            "fixture did not reproduce clock ink/arranged-size mismatch");
        Require(timeVisual.Size() == time.ActualSize(),
            "clock shadow is compressed to ink bounds instead of arranged mask bounds");
        time.Visibility(wux::Visibility::Collapsed); root.UpdateLayout();
        Require(timeVisual.Opacity() == 0, "hidden clock retained its shadow");
        time.Visibility(wux::Visibility::Visible);
        time.Text(L"21:00"); root.UpdateLayout();
        Require(timeVisual.Opacity() == 1 && Preview::GetElementChildVisual(timeHost) == timeVisual,
            "clock update lost its shadow or recreated visual");
        g_trayShadows.Stop();
        Require(twoLine.Children().Size() == 2, "two-line cleanup retained hosts");
        Require(g_log.failures == 0, "shadow reported runtime failure");
        island.Content(nullptr);
        island.Close();
        manager.Close();
        DestroyWindow(window);
        puts("PASS: real XAML mask, z-order, deduplication, clock update, disable/enable, native mode, cleanup and scope");
        puts("PASS: separate right-aligned clock/date rows, nonzero hosts, StackPanel layout, hidden clock and minute update");
        puts("PASS: stretched TextBlock with right-aligned ink keeps shadow at arranged mask size");
        puts("LIMIT: hidden test island; taskbar pixels still need visual confirmation");
        return 0;
    }
    catch (winrt::hresult_error const& error)
    {
        fwprintf(stderr, L"FAIL: 0x%08X %s\n", static_cast<unsigned>(error.code()), error.message().c_str());
        return 1;
    }
    catch (std::exception const& error) { fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
