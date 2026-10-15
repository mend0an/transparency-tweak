// Included inside TweakTap.cpp's anonymous namespace, Aero builds only.
// Each shadow lives in its own non-interactive Canvas BEFORE the foreground
// in an existing Grid, spanning its rows/columns without requesting space.
// Canvas measures to zero but arranges to the Grid's bounds; an explicitly
// zero-sized host can clip/misplace shadows in aligned two-line clock layouts.
// Never put the shadow over the glyphs or
// replace an Explorer-owned Composition child visual.
class TrayContentShadow
{
    using Preview = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview;
    struct State
    {
        winrt::weak_ref<wux::FrameworkElement> target;
        wuxc::Canvas host{nullptr};
        winrt::Windows::UI::Composition::SpriteVisual visual{nullptr};
        winrt::Windows::UI::Composition::DropShadow shadow{nullptr};
        winrt::Windows::UI::Composition::CompositionBrush mask{nullptr};
        winrt::event_token layout{};
        bool subscribed = false;
        bool refreshing = false;
        bool enabled = true;
        std::atomic<bool> active{true};
        float width = -1, height = -1;
        float hostWidth = -1, hostHeight = -1;
        float x = -1, y = -1;
        winrt::hstring text;
        wux::Media::ImageSource image{nullptr};
    };
    struct Entry
    {
        std::shared_ptr<State> state;
        DispatcherQueue queue{nullptr};
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    };

public:
    static bool IsContent(wux::UIElement const& element)
    {
        return element.try_as<wuxc::TextBlock>() || element.try_as<wuxc::Image>();
    }

    static bool InsideTray(wux::UIElement const& element)
    {
        auto current = element.as<wux::DependencyObject>();
        for (unsigned depth = 0; current && depth < 64; ++depth)
        {
            const auto type = winrt::get_class_name(current);
            if (type == L"SystemTray.SystemTrayFrame") return true;
            if (auto fe = current.try_as<wux::FrameworkElement>())
            {
                if (fe.Name() == L"SystemTrayFrame") return true;
                if (fe.Name() == L"TaskbarFrame") return false;
            }
            current = wux::Media::VisualTreeHelper::GetParent(current);
        }
        return false; // Excludes notification flyouts and overflow popup windows.
    }

    void Track(wux::UIElement const& element, InstanceHandle handle)
    {
        if (!IsContent(element) || !InsideTray(element)) return;
        auto target = element.try_as<wux::FrameworkElement>();
        if (!target) return;
        Entry entry{std::make_shared<State>(), DispatcherQueue::GetForCurrentThread(), element.Dispatcher()};
        entry.state->target = winrt::make_weak(target);
        entry.state->enabled = Enabled();
        {
            std::lock_guard lock(m_mutex);
            if (!m_entries.emplace(handle, entry).second) return;
        }
        try
        {
            std::weak_ptr<State> weak = entry.state;
            entry.state->layout = target.LayoutUpdated([weak](auto const&, auto const&)
            {
                if (auto state = weak.lock()) Refresh(state);
            });
            entry.state->subscribed = true;
            Refresh(entry.state);
            g_log.Write(L"tray shadow track handle=%llX type=%s name=%s", handle,
                winrt::get_class_name(target).c_str(), target.Name().c_str());
        }
        catch (...)
        {
            g_log.Write(L"tray shadow track failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            Untrack(handle);
        }
    }

    void Untrack(InstanceHandle handle)
    {
        Entry entry;
        {
            std::lock_guard lock(m_mutex);
            auto found = m_entries.find(handle);
            if (found == m_entries.end()) return;
            entry = found->second;
            entry.state->active = false;
            m_entries.erase(found);
        }
        Dispatch(entry, [state = entry.state]
        {
            try
            {
                if (auto target = state->target.get(); target && state->subscribed)
                    target.LayoutUpdated(state->layout);
                state->subscribed = false;
                Detach(state);
            }
            catch (...) { g_log.Write(L"tray shadow dispose failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
        });
    }

    void ApplyAll()
    {
        const bool enabled = Enabled();
        std::vector<Entry> entries;
        {
            std::lock_guard lock(m_mutex);
            for (auto const& [handle, entry] : m_entries) entries.push_back(entry);
        }
        for (auto const& entry : entries) Dispatch(entry, [state = entry.state, enabled]
        {
            state->enabled = enabled;
            Refresh(state);
        });
    }

    void Stop()
    {
        std::vector<InstanceHandle> handles;
        {
            std::lock_guard lock(m_mutex);
            for (auto const& [handle, entry] : m_entries) handles.push_back(handle);
        }
        for (auto handle : handles) Untrack(handle);
    }

private:
    static bool Enabled()
    {
        return WantedTaskbarOpacity() >= 0 && ReadDword(kAppKey, L"TrayContentShadow", 1) != 0;
    }

    template<class Callback> static void Dispatch(Entry const& entry, Callback callback)
    {
        try
        {
            if (entry.queue && entry.queue.HasThreadAccess()) callback();
            else if (entry.queue)
            {
                if (!entry.queue.TryEnqueue(std::move(callback))) g_log.Write(L"tray shadow dispatcher rejected update");
            }
            else if (entry.dispatcher) entry.dispatcher.RunAsync(
                winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, std::move(callback));
        }
        catch (...) { g_log.Write(L"tray shadow dispatch failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
    }

    static void Detach(std::shared_ptr<State> const& state)
    {
        if (state->host)
        {
            Preview::SetElementChildVisual(state->host, nullptr);
            if (auto parent = wux::Media::VisualTreeHelper::GetParent(state->host).try_as<wuxc::Panel>())
            {
                uint32_t index;
                if (parent.Children().IndexOf(state->host, index)) parent.Children().RemoveAt(index);
            }
        }
        if (state->visual) { state->visual.Shadow(nullptr); state->visual.Close(); }
        if (state->shadow) { state->shadow.Mask(nullptr); state->shadow.Close(); }
        // GetAlphaMask returns a XAML-owned brush; release it, do not Close it.
        state->mask = nullptr;
        state->visual = nullptr;
        state->shadow = nullptr;
        state->host = nullptr;
        state->image = nullptr;
        state->text = L"";
        state->width = state->height = -1;
        state->hostWidth = state->hostHeight = state->x = state->y = -1;
    }

    static void Refresh(std::shared_ptr<State> const& state)
    {
        if (!state->active || state->refreshing) return;
        state->refreshing = true;
        struct Reset { bool& flag; ~Reset() { flag = false; } } reset{state->refreshing};
        try
        {
            auto target = state->target.get();
            if (!target) return;
            if (!state->enabled)
            {
                if (state->host) Detach(state);
                return;
            }
            // TextBlock overrides ActualWidth/Height to report ink extents.
            // GetAlphaMask covers the arranged element, including text alignment.
            // Sizing that mask to ink extents shrinks and displaces right-aligned
            // clock glyphs. ActualSize preserves the mask's layout coordinates.
            const auto arranged = target.ActualSize();
            const auto width = arranged.x;
            const auto height = arranged.y;
            if (width <= 0 || height <= 0)
            {
                // Wait for layout, and do not leave a previous clock/icon
                // shadow visible when its source collapses to zero size.
                if (state->visual) { state->visual.IsVisible(false); state->visual.Opacity(0); }
                return;
            }
            if (!state->host)
            {
                auto parent = wux::Media::VisualTreeHelper::GetParent(target);
                wuxc::Grid panel{nullptr};
                for (unsigned depth = 0; parent && depth < 16; ++depth)
                {
                    panel = parent.try_as<wuxc::Grid>();
                    if (panel) break;
                    parent = wux::Media::VisualTreeHelper::GetParent(parent);
                }
                if (!panel) return;
                auto compositor = Preview::GetElementVisual(target).Compositor();
                state->shadow = compositor.CreateDropShadow();
                state->shadow.Color({255, 0, 0, 0});
                state->shadow.BlurRadius(2.5f);
                state->shadow.Opacity(0.85f);
                state->shadow.Offset({0.0f, 1.0f, 0.0f});
                state->visual = compositor.CreateSpriteVisual();
                state->visual.Shadow(state->shadow);
                state->host = wuxc::Canvas();
                state->host.Name(L"TransparencyTweak_TrayShadow");
                state->host.HorizontalAlignment(wux::HorizontalAlignment::Stretch);
                state->host.VerticalAlignment(wux::VerticalAlignment::Stretch);
                wuxc::Grid::SetRowSpan(state->host, std::max(1, static_cast<int>(panel.RowDefinitions().Size())));
                wuxc::Grid::SetColumnSpan(state->host, std::max(1, static_cast<int>(panel.ColumnDefinitions().Size())));
                state->host.IsHitTestVisible(false);
                Preview::SetElementChildVisual(state->host, state->visual);
                panel.Children().InsertAt(0, state->host);
            }
            // TransformToVisual includes Grid rows, margins, alignment and DPI
            // in XAML coordinates. No cached screen positions or pixel polling.
            const auto point = target.TransformToVisual(state->host).TransformPoint({0, 0});
            state->visual.Offset({point.X, point.Y, 0});
            const auto hostParent = wux::Media::VisualTreeHelper::GetParent(state->host);
            auto branch = target.as<wux::DependencyObject>();
            double opacity = 1.0;
            for (unsigned depth = 0; branch && branch != hostParent && depth < 16; ++depth)
            {
                if (auto ui = branch.try_as<wux::UIElement>())
                    opacity *= ui.Visibility() == wux::Visibility::Visible ? ui.Opacity() : 0.0;
                branch = wux::Media::VisualTreeHelper::GetParent(branch);
            }
            state->visual.Opacity(static_cast<float>(opacity));
            state->visual.IsVisible(true);
            const auto hostWidth = static_cast<float>(state->host.ActualWidth());
            const auto hostHeight = static_cast<float>(state->host.ActualHeight());
            if (point.X != state->x || point.Y != state->y || hostWidth != state->hostWidth || hostHeight != state->hostHeight)
            {
                state->x = point.X; state->y = point.Y;
                state->hostWidth = hostWidth; state->hostHeight = hostHeight;
                g_log.Write(L"tray shadow positioned name=%s offset=%.1f,%.1f host=%.0fx%.0f opacity=%.3f",
                    target.Name().c_str(), point.X, point.Y, hostWidth, hostHeight, opacity);
            }
            winrt::hstring text;
            wux::Media::ImageSource image{nullptr};
            auto label = target.try_as<wuxc::TextBlock>();
            auto icon = target.try_as<wuxc::Image>();
            if (label) text = label.Text();
            if (icon) image = icon.Source();
            if (!state->mask || width != state->width || height != state->height || text != state->text || image != state->image)
            {
                auto mask = label ? label.GetAlphaMask() : icon.GetAlphaMask();
                if (!mask) { state->visual.IsVisible(false); return; } // Never fall back to a rectangular shadow.
                state->shadow.Mask(mask);
                state->mask = mask;
                state->visual.Size({width, height});
                state->width = width;
                state->height = height;
                state->text = text;
                state->image = image;
                g_log.Write(L"tray shadow applied type=%s name=%s size=%.0fx%.0f ink=%.1fx%.1f offset=%.1f,%.1f host=%.0fx%.0f", winrt::get_class_name(target).c_str(),
                    target.Name().c_str(), width, height, target.ActualWidth(), target.ActualHeight(), point.X, point.Y,
                    state->host.ActualWidth(), state->host.ActualHeight());
            }
        }
        catch (...)
        {
            g_log.Write(L"tray shadow refresh failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult()));
            try { Detach(state); } catch (...) {}
        }
    }

    std::mutex m_mutex;
    std::map<InstanceHandle, Entry> m_entries;
};

TrayContentShadow g_trayShadows;
