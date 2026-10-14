// Executes in its own process. Does not inject, change registry, or touch the taskbar.
// Headless API/lifetime checks; memory here excludes real rendering and GPU surfaces.
#include "AeroGlassBrush.h"
#include <winrt/Windows.System.h>
#include <DispatcherQueue.h>
#include <psapi.h>
#include <cstdio>
#include <limits>
#include <stdexcept>

static void Require(bool condition, char const* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Memory(char const* phase)
{
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    winrt::check_bool(GetProcessMemoryInfo(GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)));
    printf("MEMORY phase=%s private_commit_bytes=%llu working_set_bytes=%llu\n", phase,
        static_cast<unsigned long long>(counters.PrivateUsage),
        static_cast<unsigned long long>(counters.WorkingSetSize));
}

int main()
{
    try
    {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        namespace glass = experimental_glass;
        namespace composition = glass::composition;
        namespace interop = glass::interop;

        auto gaussian = winrt::make_self<glass::EffectDescription>(glass::EffectDescription::Kind::Gaussian);
        auto abi = gaussian.as<interop::IGraphicsEffectD2D1Interop>();
        UINT count = 0, propertyIndex = 99;
        interop::GRAPHICS_EFFECT_PROPERTY_MAPPING mapping{};
        GUID effectId{};
        winrt::check_hresult(abi->GetEffectId(&effectId));
        Require(effectId == CLSID_D2D1GaussianBlur, "wrong Gaussian effect ID");
        winrt::check_hresult(abi->GetPropertyCount(&count));
        Require(count == 3, "Gaussian property count");
        winrt::check_hresult(abi->GetNamedPropertyMapping(L"Amount", &propertyIndex, &mapping));
        Require(propertyIndex == D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, "animated sigma mapping");
        winrt::com_ptr<ABI::Windows::Foundation::IPropertyValue> value;
        Require(abi->GetProperty(3, value.put()) == E_BOUNDS, "property bounds check");
        Require(abi->GetSource(1, nullptr) == E_POINTER, "source null output check");
        bool rejected = false;
        try { glass::Normalize({std::numeric_limits<float>::quiet_NaN(), {64,116,166,205}, 0.1f}); }
        catch (winrt::hresult_invalid_argument const&) { rejected = true; }
        Require(rejected, "nonfinite sigma must be rejected");
        printf("PASS: effect ABI, bounds and nonfinite configuration\n");

        winrt::Windows::System::DispatcherQueueController queue{nullptr};
        const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_STA};
        winrt::check_hresult(CreateDispatcherQueueController(options,
            reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(queue))));
        composition::Compositor compositor;
        Memory("compositor_ready");
        glass::GlassResources resources(compositor);
        resources.Update({});
        auto brush = resources.Brush();
        const auto identity = winrt::get_abi(brush);
        Require(resources.GraphBuilds() == 1, "first graph creation");
        Memory("graph_ready");

        for (int batch = 1; batch <= 2; ++batch)
        {
            for (int i = 0; i < 10000; ++i)
            {
                glass::Style style;
                style.blurSigma = 1.0f + static_cast<float>(i % 50) / 10.0f;
                style.tint.A = static_cast<uint8_t>(i % 128);
                style.reflection = static_cast<float>(i % 20) / 100.0f;
                resources.Update(style);
                Require(winrt::get_abi(resources.Brush()) == identity, "brush replaced by slider update");
            }
            Require(resources.GraphBuilds() == 1, "factory rebuilt during updates");
            Memory(batch == 1 ? "after_10000_updates" : "after_20000_updates");
        }
        float sigma = 0.0f;
        auto status = brush.Properties().TryGetScalar(L"Blur.Amount", sigma);
        Require(status == composition::CompositionGetValueStatus::Succeeded && std::abs(sigma - 5.9f) < 0.001f,
            "final blur property update");
        printf("PASS: 20000 updates reused the same effect brush and factory\n");
        brush = nullptr;
        resources.Close();
        Require(!resources.Brush(), "effect brush retained after Close");
        resources.Update({});
        Require(resources.GraphBuilds() == 2, "reconnect should create one new graph");
        resources.Close();
        Memory("closed");
        printf("PASS: disconnect/reconnect resource lifecycle\n");
        printf("LIMIT: headless only; no visual equivalence, taskbar overhead or GPU-memory claim\n");
        compositor.Close();
        return 0;
    }
    catch (winrt::hresult_error const& e)
    {
        fwprintf(stderr, L"FAIL HRESULT=0x%08X %s\n", static_cast<unsigned>(e.code()), e.message().c_str());
        return 1;
    }
    catch (std::exception const& e)
    {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
