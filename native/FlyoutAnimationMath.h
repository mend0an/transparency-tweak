#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

namespace flyout_animation
{
    enum class Edge { Left, Top, Right, Bottom };
    struct Point { int x, y; };
    struct Rect { int left, top, right, bottom; };

    inline bool Horizontal(Edge edge) { return edge == Edge::Left || edge == Edge::Right; }

    inline Edge TaskbarEdge(Rect monitor, Rect taskbar)
    {
        // Use the long axis and nearest edge, including an auto-hidden bar
        // just outside the screen. Coordinates may be negative on any monitor.
        if (taskbar.right - taskbar.left >= taskbar.bottom - taskbar.top)
        {
            double center = (static_cast<double>(taskbar.top) + taskbar.bottom) / 2;
            return std::abs(center - monitor.top) < std::abs(center - monitor.bottom) ? Edge::Top : Edge::Bottom;
        }
        double center = (static_cast<double>(taskbar.left) + taskbar.right) / 2;
        return std::abs(center - monitor.left) < std::abs(center - monitor.right) ? Edge::Left : Edge::Right;
    }

    // Hide the whole panel beyond the physical screen edge, including taskbar
    // space. Left/top must subtract the panel size, unlike right/bottom.
    inline Point StartPoint(Rect panel, Rect monitor, Edge edge)
    {
        switch (edge)
        {
        case Edge::Left: return {monitor.left - (panel.right - panel.left), panel.top};
        case Edge::Top: return {panel.left, monitor.top - (panel.bottom - panel.top)};
        case Edge::Right: return {monitor.right, panel.top};
        default: return {panel.left, monitor.bottom};
        }
    }

    inline int SlideCoordinate(int finalValue, int startValue, double progress)
    {
        progress = std::clamp(progress, 0.0, 1.0);
        double remaining = 1.0 - progress;
        return static_cast<int>(std::lround(finalValue +
            (static_cast<double>(startValue) - finalValue) * remaining * remaining * remaining));
    }

    inline Point Slide(Point finalPoint, Point startPoint, double progress)
    {
        return {SlideCoordinate(finalPoint.x, startPoint.x, progress),
                SlideCoordinate(finalPoint.y, startPoint.y, progress)};
    }

    inline const wchar_t* EdgeName(Edge edge)
    {
        switch (edge)
        {
        case Edge::Left: return L"left";
        case Edge::Top: return L"top";
        case Edge::Right: return L"right";
        default: return L"bottom";
        }
    }

    // Strict class/process pairs; never move arbitrary apps or other CoreWindows.
    inline bool IsTarget(std::wstring_view process, std::wstring_view windowClass,
                         std::wstring_view threadName)
    {
        if (process == L"StartMenuExperienceHost.exe")
            return windowClass == L"Windows.UI.Core.CoreWindow";
        if (process == L"ShellHost.exe")
            return windowClass == L"ControlCenterWindow";
        if (process == L"ShellExperienceHost.exe")
            return windowClass == L"Windows.UI.Core.CoreWindow" && threadName == L"ActionCenter";
        if (process == L"explorer.exe")
            return windowClass == L"TopLevelWindowForOverflowXamlIsland";
        return false;
    }
}
