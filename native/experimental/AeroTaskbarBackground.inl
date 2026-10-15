// Included inside TweakTap.cpp's anonymous namespace, experimental build only.
// Registry settings and Explorer/Settings bootstrap reuse the existing DLL.
class AeroTaskbarBackground
{
    struct State
    {
        wux::Media::Brush original{nullptr};
        wux::Media::SolidColorBrush solid{nullptr};
        winrt::com_ptr<experimental_glass::XamlGlassBrush> glass;
        std::atomic<bool> active{true};
        bool applying = false;
        int64_t callback = 0;
        bool hasCallback = false;
    };
    struct Entry
    {
        InstanceHandle handle = 0;
        winrt::weak_ref<wux::UIElement> element;
        DispatcherQueue queue{nullptr};
        winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
        double originalOpacity = 1.0;
        bool background = false;
        std::shared_ptr<State> state;
    };

public:
    static bool InsideTaskbar(wux::UIElement const& element)
    {
        auto current = element.as<wux::DependencyObject>();
        for (unsigned depth = 0; current && depth < 64; ++depth)
        {
            if (auto fe = current.try_as<wux::FrameworkElement>(); fe && fe.Name() == L"TaskbarFrame") return true;
            current = wux::Media::VisualTreeHelper::GetParent(current);
        }
        return false;
    }

    void Track(wux::UIElement const& element, InstanceHandle handle, bool background)
    {
        if (!InsideTaskbar(element)) return;
        {
            std::lock_guard lock(m_mutex);
            if (m_entries.count(handle)) return;
        }
        Entry entry{handle, winrt::make_weak(element), DispatcherQueue::GetForCurrentThread(),
            element.Dispatcher(), element.Opacity(), background, std::make_shared<State>()};
        if (auto shape = element.try_as<wux::Shapes::Shape>())
        {
            entry.state->original = shape.Fill();
            entry.state->callback = shape.RegisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(),
                [entry](auto const& sender, auto const&)
                {
                    if (!entry.state->active || entry.state->applying) return;
                    try
                    {
                        entry.state->original = sender.template as<wux::Shapes::Shape>().Fill();
                        Apply(entry);
                    }
                    catch (...) { g_log.Write(L"glass fill callback failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
                });
            entry.state->hasCallback = true;
        }
        {
            std::lock_guard lock(m_mutex);
            m_entries.emplace(handle, entry);
            g_log.Write(L"glass track handle=%llX background=%d entries=%zu", handle, background, m_entries.size());
        }
        g_backdrop.Update(WantedTaskbarOpacity() >= 0);
        Apply(entry);
        StartWatching();
    }

    void Untrack(InstanceHandle handle)
    {
        Entry entry;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_entries.find(handle);
            if (found == m_entries.end()) return;
            entry = found->second;
            entry.state->active = false;
            m_entries.erase(found);
            g_log.Write(L"glass untrack handle=%llX entries=%zu", handle, m_entries.size());
        }
        Dispatch(entry, [entry] { Dispose(entry); });
    }

    void ApplyAll()
    {
        g_trayShadows.ApplyAll();
        g_backdrop.Update(WantedTaskbarOpacity() >= 0);
        std::vector<Entry> entries;
        {
            std::lock_guard lock(m_mutex);
            entries.reserve(m_entries.size());
            for (auto const& [handle, entry] : m_entries) entries.push_back(entry);
        }
        for (auto const& entry : entries) Dispatch(entry, [entry] { Apply(entry); });
    }

    void Stop()
    {
        std::vector<InstanceHandle> handles;
        {
            std::lock_guard lock(m_mutex);
            for (auto const& [handle, entry] : m_entries) handles.push_back(handle);
        }
        for (auto handle : handles) Untrack(handle);
        g_backdrop.Update(false);
    }

private:
    template<class Callback> static void Dispatch(Entry const& entry, Callback callback)
    {
        try
        {
            if (entry.queue)
            {
                if (!entry.queue.TryEnqueue(std::move(callback))) g_log.Write(L"glass dispatcher rejected update");
            }
            else if (entry.dispatcher)
                entry.dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, std::move(callback));
            else g_log.Write(L"glass dispatcher unavailable");
        }
        catch (...) { g_log.Write(L"glass dispatch failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
    }

    static void Dispose(Entry const& entry)
    {
        try
        {
            auto element = entry.element.get();
            auto shape = element ? element.try_as<wux::Shapes::Shape>() : nullptr;
            if (shape)
            {
                entry.state->applying = true;
                if (entry.state->hasCallback)
                    shape.UnregisterPropertyChangedCallback(wux::Shapes::Shape::FillProperty(), entry.state->callback);
                entry.state->callback = 0;
                entry.state->hasCallback = false;
                if (entry.state->original) shape.Fill(entry.state->original);
                element.Opacity(entry.originalOpacity);
            }
        }
        catch (...) { g_log.Write(L"glass dispose failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
        try { if (entry.state->glass) entry.state->glass->OnDisconnected(); }
        catch (...) { g_log.Write(L"glass resource release failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
        entry.state->glass = nullptr;
        entry.state->solid = nullptr;
        entry.state->original = nullptr;
    }

    static void Apply(Entry const& entry)
    {
        if (!entry.state->active) return;
        try
        {
            auto element = entry.element.get();
            auto shape = element ? element.try_as<wux::Shapes::Shape>() : nullptr;
            if (!shape || !entry.state->original) return;
            entry.state->applying = true;
            struct Reset { bool& flag; ~Reset() { flag = false; } } reset{entry.state->applying};
            const double wanted = WantedTaskbarOpacity();
            element.Opacity(entry.originalOpacity);
            const bool glassEnabled = wanted >= 0 && ReadDword(kAppKey, L"AeroGlass", 1) != 0 && entry.background;
            if (wanted < 0)
                shape.Fill(entry.state->original);
            else if (glassEnabled)
            {
                const DWORD rgb = ReadDword(kAppKey, L"GlassTintArgb", 0xFF74A6CD);
                experimental_glass::Style style;
                style.blurSigma = static_cast<float>(ReadDword(kAppKey, L"GlassBlurSigmaMilli", 2500)) / 1000.0f;
                style.tint = {static_cast<uint8_t>(std::lround(255.0 * wanted * 0.55)),
                    static_cast<uint8_t>(rgb >> 16), static_cast<uint8_t>(rgb >> 8), static_cast<uint8_t>(rgb)};
                style.reflection = static_cast<float>(ReadDword(kAppKey, L"GlassReflectionPercent", 12)) / 100.0f;
                if (!entry.state->glass)
                {
                    auto compositor = winrt::Windows::UI::Xaml::Hosting::ElementCompositionPreview::GetElementVisual(element).Compositor();
                    entry.state->glass = winrt::make_self<experimental_glass::XamlGlassBrush>(compositor, style,
                        [](HRESULT result)
                        {
                            if (FAILED(result)) g_log.Write(L"glass composition failed hr=0x%08X", static_cast<unsigned>(result));
                            else g_log.Write(L"glass composition connected");
                        });
                    g_log.Write(L"glass brush created handle=%llX", entry.handle);
                }
                else entry.state->glass->Update(style);
                shape.Fill(entry.state->glass.as<wux::Media::Brush>());
            }
            else
            {
                auto color = ReadDword(kPersonalizeKey, L"SystemUsesLightTheme", 0)
                    ? winrt::Windows::UI::Color{255,243,243,243} : winrt::Windows::UI::Color{255,32,32,32};
                if (auto original = entry.state->original.try_as<wux::Media::SolidColorBrush>()) color = original.Color();
                else if (auto originalAcrylic = entry.state->original.try_as<wux::Media::AcrylicBrush>()) color = originalAcrylic.TintColor();
                if (!entry.state->solid) entry.state->solid = wux::Media::SolidColorBrush(color);
                else entry.state->solid.Color(color);
                entry.state->solid.Opacity(wanted * entry.state->original.Opacity());
                shape.Fill(entry.state->solid);
            }
            if (!glassEnabled && entry.state->glass)
            {
                entry.state->glass->OnDisconnected();
                entry.state->glass = nullptr;
            }
            auto fe = element.as<wux::FrameworkElement>();
            g_log.Write(L"taskbar brush %s requested=%.3f actual=%.3f size=%.0fx%.0f glass=%d",
                fe.Name().c_str(), wanted, shape.Fill().Opacity(), fe.ActualWidth(), fe.ActualHeight(), glassEnabled);
        }
        catch (...) { g_log.Write(L"glass apply failed hr=0x%08X", static_cast<unsigned>(winrt::to_hresult())); }
    }

    void StartWatching()
    {
        std::call_once(m_watchOnce, [this]
        {
            std::thread([this]
            {
                HKEY app = nullptr, personalize = nullptr;
                RegCreateKeyExW(HKEY_CURRENT_USER, kAppKey, 0, nullptr, 0, KEY_NOTIFY | KEY_READ, nullptr, &app, nullptr);
                RegOpenKeyExW(HKEY_CURRENT_USER, kPersonalizeKey, 0, KEY_NOTIFY | KEY_READ, &personalize);
                HANDLE events[2]{CreateEventW(nullptr,FALSE,FALSE,nullptr), CreateEventW(nullptr,FALSE,FALSE,nullptr)};
                for (;;)
                {
                    if (app) RegNotifyChangeKeyValue(app,FALSE,REG_NOTIFY_CHANGE_LAST_SET,events[0],TRUE);
                    if (personalize) RegNotifyChangeKeyValue(personalize,FALSE,REG_NOTIFY_CHANGE_LAST_SET,events[1],TRUE);
                    if (WaitForMultipleObjects(2,events,FALSE,INFINITE) == WAIT_FAILED) break;
                    ApplyAll();
                }
                if (app) RegCloseKey(app);
                if (personalize) RegCloseKey(personalize);
                for (auto event : events) if (event) CloseHandle(event);
            }).detach();
        });
    }

    std::mutex m_mutex;
    std::map<InstanceHandle, Entry> m_entries;
    std::once_flag m_watchOnce;
};
