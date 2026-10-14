#pragma once

// Experimental glass renderer. Uses only Windows SDK / OS Composition APIs.
// This is an Aero-style approximation, not the Windows 7 renderer or its assets.
// All methods must run on the compositor / element's owning UI thread.
#include <windows.h>
#undef GetCurrentTime
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Xaml.Media.h>
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace experimental_glass
{
    namespace composition = winrt::Windows::UI::Composition;
    namespace effects = winrt::Windows::Graphics::Effects;
    namespace foundation = winrt::Windows::Foundation;
    namespace interop = ABI::Windows::Graphics::Effects;
    using Color = winrt::Windows::UI::Color;

    struct Style
    {
        float blurSigma = 2.5f;
        Color tint{64, 116, 166, 205};
        float reflection = 0.12f;
    };

    inline Style Normalize(Style value)
    {
        if (!std::isfinite(value.blurSigma) || !std::isfinite(value.reflection))
            throw winrt::hresult_invalid_argument();
        value.blurSigma = std::clamp(value.blurSigma, 0.0f, 16.0f);
        value.reflection = std::clamp(value.reflection, 0.0f, 0.3f);
        return value;
    }

    // A fixed-size SDK effect description. No Win2D runtime, bitmap, or vector
    // allocation per slider update. Composition owns the actual GPU resources.
    struct EffectDescription : winrt::implements<EffectDescription,
        effects::IGraphicsEffect, effects::IGraphicsEffectSource,
        interop::IGraphicsEffectD2D1Interop>
    {
        enum class Kind { Gaussian, SourceOver };
        explicit EffectDescription(Kind kind) : m_kind(kind) {}

        winrt::hstring Name() const { return m_name; }
        void Name(winrt::hstring const& name) { m_name = name; }
        std::array<effects::IGraphicsEffectSource, 3> sources{nullptr, nullptr, nullptr};
        float sigma = 2.5f;

        HRESULT STDMETHODCALLTYPE GetEffectId(GUID* result) noexcept override
        {
            if (!result) return E_POINTER;
            *result = m_kind == Kind::Gaussian ? CLSID_D2D1GaussianBlur : CLSID_D2D1Composite;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetNamedPropertyMapping(LPCWSTR name, UINT* index,
            interop::GRAPHICS_EFFECT_PROPERTY_MAPPING* mapping) noexcept override
        {
            if (!name || !index || !mapping) return E_POINTER;
            if (m_kind != Kind::Gaussian || wcscmp(name, L"Amount") != 0) return E_INVALIDARG;
            *index = D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION;
            *mapping = interop::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetPropertyCount(UINT* count) noexcept override
        {
            if (!count) return E_POINTER;
            *count = m_kind == Kind::Gaussian ? 3u : 1u;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetProperty(UINT index,
            ABI::Windows::Foundation::IPropertyValue** result) noexcept override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            try
            {
                foundation::IInspectable property{nullptr};
                if (m_kind == Kind::Gaussian)
                {
                    switch (index)
                    {
                    case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION:
                        property = foundation::PropertyValue::CreateSingle(sigma); break;
                    case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION:
                        property = foundation::PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED); break;
                    case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE:
                        property = foundation::PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;
                    default: return E_BOUNDS;
                    }
                }
                else
                {
                    if (index != D2D1_COMPOSITE_PROP_MODE) return E_BOUNDS;
                    property = foundation::PropertyValue::CreateUInt32(D2D1_COMPOSITE_MODE_SOURCE_OVER);
                }
                *result = property.as<ABI::Windows::Foundation::IPropertyValue>().detach();
                return S_OK;
            }
            catch (...) { return winrt::to_hresult(); }
        }

        HRESULT STDMETHODCALLTYPE GetSourceCount(UINT* count) noexcept override
        {
            if (!count) return E_POINTER;
            *count = m_kind == Kind::Gaussian ? 1u : 3u;
            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE GetSource(UINT index, interop::IGraphicsEffectSource** result) noexcept override
        {
            if (!result) return E_POINTER;
            *result = nullptr;
            if (index >= (m_kind == Kind::Gaussian ? 1u : 3u)) return E_BOUNDS;
            try
            {
                winrt::copy_to_abi(sources[index], *reinterpret_cast<void**>(result));
                return S_OK;
            }
            catch (...) { return winrt::to_hresult(); }
        }

    private:
        Kind m_kind;
        winrt::hstring m_name;
    };

    class GlassResources
    {
    public:
        explicit GlassResources(composition::Compositor const& compositor) : m_compositor(compositor)
        {
            if (!compositor) throw winrt::hresult_invalid_argument();
        }
        GlassResources(GlassResources const&) = delete;
        GlassResources& operator=(GlassResources const&) = delete;
        ~GlassResources() { Close(); }

        composition::CompositionEffectBrush Brush() const { return m_brush; }
        uint32_t GraphBuilds() const { return m_builds; }

        void Update(Style requested)
        {
            const Style style = Normalize(requested);
            Ensure(style);
            if (!m_hasStyle || style.blurSigma != m_style.blurSigma)
                m_brush.Properties().InsertScalar(L"Blur.Amount", style.blurSigma);
            if (!m_hasStyle || !SameColor(style.tint, m_style.tint)) m_tint.Color(style.tint);
            if (!m_hasStyle || style.reflection != m_style.reflection)
            {
                for (uint32_t i = 0; i < m_stops.size(); ++i)
                {
                    const auto alpha = static_cast<uint8_t>(std::lround(style.reflection * kStrength[i] * 255.0f));
                    m_stops[i].Color(Color{alpha, 255, 255, 255});
                }
            }
            m_style = style;
            m_hasStyle = true;
        }

        void Close() noexcept
        {
            // Disconnect consumers before calling Close. Do not close the
            // compositor borrowed from the taskbar visual.
            CloseObject(m_brush);
            CloseObject(m_backdrop);
            CloseObject(m_tint);
            CloseObject(m_reflection);
            for (auto& stop : m_stops) CloseObject(stop);
            CloseObject(m_factory);
            m_hasStyle = false;
        }

    private:
        template<class T> static void CloseObject(T& object) noexcept
        {
            if (object) { try { object.Close(); } catch (...) {} object = nullptr; }
        }

        static bool SameColor(Color a, Color b)
        {
            return a.A == b.A && a.R == b.R && a.G == b.G && a.B == b.B;
        }

        void Ensure(Style const& style)
        {
            if (m_brush) return;
            try
            {
                auto blur = winrt::make_self<EffectDescription>(EffectDescription::Kind::Gaussian);
                blur->Name(L"Blur");
                blur->sigma = style.blurSigma;
                blur->sources[0] = composition::CompositionEffectSourceParameter(L"Backdrop");

                auto blend = winrt::make_self<EffectDescription>(EffectDescription::Kind::SourceOver);
                blend->sources[0] = blur.as<effects::IGraphicsEffectSource>();
                blend->sources[1] = composition::CompositionEffectSourceParameter(L"Tint");
                blend->sources[2] = composition::CompositionEffectSourceParameter(L"Reflection");

                m_factory = m_compositor.CreateEffectFactory(blend.as<effects::IGraphicsEffect>(), {L"Blur.Amount"});
                m_backdrop = m_compositor.CreateBackdropBrush();
                m_tint = m_compositor.CreateColorBrush(style.tint);
                m_reflection = m_compositor.CreateLinearGradientBrush();
                m_reflection.StartPoint({0.0f, 0.0f});
                m_reflection.EndPoint({0.0f, 1.0f});
                for (uint32_t i = 0; i < m_stops.size(); ++i)
                {
                    m_stops[i] = m_compositor.CreateColorGradientStop(kOffsets[i], Color{0,255,255,255});
                    m_reflection.ColorStops().Append(m_stops[i]);
                }
                m_brush = m_factory.CreateBrush();
                m_brush.SetSourceParameter(L"Backdrop", m_backdrop);
                m_brush.SetSourceParameter(L"Tint", m_tint);
                m_brush.SetSourceParameter(L"Reflection", m_reflection);
                ++m_builds;
            }
            catch (...) { Close(); throw; }
        }

        static constexpr std::array<float, 4> kOffsets{0.0f, 0.42f, 0.43f, 1.0f};
        static constexpr std::array<float, 4> kStrength{1.0f, 0.7f, 0.1f, 0.25f};
        composition::Compositor m_compositor;
        composition::CompositionEffectFactory m_factory{nullptr};
        composition::CompositionEffectBrush m_brush{nullptr};
        composition::CompositionBackdropBrush m_backdrop{nullptr};
        composition::CompositionColorBrush m_tint{nullptr};
        composition::CompositionLinearGradientBrush m_reflection{nullptr};
        std::array<composition::CompositionColorGradientStop, 4> m_stops{nullptr,nullptr,nullptr,nullptr};
        Style m_style{};
        bool m_hasStyle = false;
        uint32_t m_builds = 0;
    };

    struct XamlGlassBrush : winrt::Windows::UI::Xaml::Media::XamlCompositionBrushBaseT<XamlGlassBrush>
    {
        using Reporter = void (*)(HRESULT);
        XamlGlassBrush(composition::Compositor const& compositor, Style style, Reporter reporter = nullptr) :
            m_resources(compositor), m_style(Normalize(style)), m_reporter(reporter) {}

        void Update(Style style)
        {
            m_style = Normalize(style);
            if (CompositionBrush()) m_resources.Update(m_style);
        }
        void OnConnected()
        {
            try
            {
                m_resources.Update(m_style);
                CompositionBrush(m_resources.Brush());
                if (m_reporter) m_reporter(S_OK);
            }
            catch (...)
            {
                const HRESULT error = winrt::to_hresult();
                try { CompositionBrush(nullptr); FallbackColor(m_style.tint); } catch (...) {}
                m_resources.Close();
                if (m_reporter) m_reporter(error);
            }
        }
        void OnDisconnected()
        {
            CompositionBrush(nullptr);
            m_resources.Close();
        }
    private:
        GlassResources m_resources;
        Style m_style;
        Reporter m_reporter = nullptr;
    };
}
