#include "pch.h"

#include "MfgUnlock.h"

#include <Config.h>
#include <State.h>
#include <Util.h>
#include <misc/IdentifyGpu.h>
#include <hooks/Streamline_Hooks.h>

#include <nvapi.h>

namespace MfgUnlock
{

namespace
{

Telemetry g_telemetry;
std::atomic_bool g_initialised { false };
std::atomic_bool g_bootstrapping { false };
std::atomic_bool g_declineLogged { false };
std::atomic_bool g_statusOkLogged { false };
std::atomic_bool g_statusFailLogged { false };
std::atomic<unsigned int> g_seenPresentCounts { 0 };

// 0 = not decided yet, 1 = Ada (or unknown but allowed), 2 = not a target for the unlock.
std::atomic<int> g_hardwareVerdict { 0 };

// Ada device ids sit in 0x2600..0x28FF (AD102..AD107); Blackwell consumer parts start at 0x2B00.
// Used only when Streamline has not told us the NVAPI architecture yet.
constexpr uint32_t kAdaDeviceIdFirst = 0x2600;
constexpr uint32_t kAdaDeviceIdLast = 0x28FF;

Settings SettingsFromConfig()
{
    auto* cfg = Config::Instance();
    Settings s {};
    s.enabled = cfg->MfgUnlockEnabled.value_or_default();
    s.temporalFix = cfg->MfgUnlockTemporalFix.value_or_default();
    s.forceFlipMeterOff = cfg->MfgUnlockForceFlipMeterOff.value_or_default();
    s.raiseCeiling = cfg->MfgUnlockRaiseCeiling.value_or_default();
    return s;
}

} // namespace

Telemetry& GetTelemetry() { return g_telemetry; }

void Init(HMODULE self)
{
    SetSelfModule(self);
    ApplyConfig();
    g_initialised.store(true, std::memory_order_release);

    if (Enabled())
        LOG_INFO("DLSS MFG unlock (RTX 40) enabled: force multiplier {}, temporal fix {}, legacy pacing {}",
                 ForceMultiplier(), Config::Instance()->MfgUnlockTemporalFix.value_or_default(),
                 Config::Instance()->MfgUnlockForceFlipMeterOff.value_or_default());
}

void ApplyConfig() { SetSettings(SettingsFromConfig()); }

bool Enabled() { return Config::Instance()->MfgUnlockEnabled.value_or_default(); }

// Called from LoadLibrary hooks and from DllMain, so it must be cheap and must never load anything or
// touch DXGI. It reads what Streamline already reported (a cached struct) and a verdict that
// Bootstrap() fills in from a normal thread. Until either exists the answer is "allowed": the
// snippet's own arch gate already refuses anything below Ada, and a Blackwell owner has no reason to
// switch this on.
bool Active()
{
    if (!Enabled())
        return false;

    const int verdict = g_hardwareVerdict.load(std::memory_order_acquire);
    if (verdict == 2)
        return false;
    if (verdict == 1)
        return true;

    // Streamline's own view of the adapter is authoritative once it exists. GP100 is the placeholder
    // getSystemCapsArch returns when it knows nothing yet.
    const uint32_t arch = StreamlineHooks::reportedSystemCapsArch();
    if (arch != 0 && arch != NV_GPU_ARCHITECTURE_GP100)
    {
        g_telemetry.detectedArch.store(arch, std::memory_order_relaxed);
        if (arch >= NV_GPU_ARCHITECTURE_GB200)
        {
            g_hardwareVerdict.store(2, std::memory_order_release);
            if (!g_telemetry.skippedNotAda.exchange(true, std::memory_order_relaxed))
                LOG_INFO("Streamline reports architecture 0x{:x} (Blackwell or newer); the unlock is not needed "
                         "and stays inactive.",
                         arch);
            return false;
        }
        if (arch < NV_GPU_ARCHITECTURE_AD100)
        {
            g_hardwareVerdict.store(2, std::memory_order_release);
            if (!g_telemetry.skippedNotAda.exchange(true, std::memory_order_relaxed))
                LOG_WARN("Streamline reports architecture 0x{:x} (below Ada); the DLSS-G snippet refuses those "
                         "itself, so the unlock stays inactive.",
                         arch);
            return false;
        }
        g_hardwareVerdict.store(1, std::memory_order_release);
        return true;
    }

    return true;
}

// The DXGI-based fallback, for games where Streamline never tells us the architecture. Creates a
// factory, so normal threads only -- Bootstrap() is the one caller.
static void IdentifyHardware()
{
    if (g_hardwareVerdict.load(std::memory_order_acquire) != 0)
        return;

    const auto gpu = IdentifyGpu::getPrimaryGpu();
    g_telemetry.detectedDeviceId.store(gpu.deviceId, std::memory_order_relaxed);
    if (gpu.vendorId != VendorId::Nvidia && gpu.vendorId != VendorId::Invalid)
    {
        // No DLSS-G snippet can ever load here.
        g_hardwareVerdict.store(2, std::memory_order_release);
        if (!g_telemetry.skippedNotAda.exchange(true, std::memory_order_relaxed))
            LOG_INFO("Primary GPU is not NVIDIA; the unlock stays inactive.");
        return;
    }
    if (gpu.vendorId == VendorId::Nvidia && gpu.deviceId != 0 &&
        (gpu.deviceId < kAdaDeviceIdFirst || gpu.deviceId > kAdaDeviceIdLast))
    {
        // Not a device id we recognise as Ada. Below Ada the snippet's own arch gate refuses anyway;
        // above it there is nothing to unlock. Say so once and stand down.
        g_hardwareVerdict.store(2, std::memory_order_release);
        if (!g_telemetry.skippedNotAda.exchange(true, std::memory_order_relaxed))
            LOG_WARN("Primary GPU device id 0x{:x} is not an RTX 40-series part; the unlock stays inactive.",
                     gpu.deviceId);
        return;
    }

    if (gpu.vendorId == VendorId::Nvidia && gpu.deviceId != 0)
        g_hardwareVerdict.store(1, std::memory_order_release);
}

unsigned int ForceMultiplier()
{
    const int v = Config::Instance()->MfgUnlockForceMultiplier.value_or_default();
    if (v < 2 || v > 6)
        return 0;
    return static_cast<unsigned int>(v);
}

void Bootstrap()
{
    if (g_telemetry.bootstrapped.load(std::memory_order_acquire))
        return;
    if (!g_initialised.load(std::memory_order_acquire) || !Active())
        return;
    bool expected = false;
    if (!g_bootstrapping.compare_exchange_strong(expected, true))
        return;

    IdentifyHardware();
    if (!Active())
    {
        g_telemetry.bootstrapped.store(true, std::memory_order_release);
        g_bootstrapping.store(false, std::memory_order_release);
        return;
    }

    RunProviderMaintenance();
    if (Config::Instance()->MfgUnlockForceFlipMeterOff.value_or_default())
        TryPatchFlipMetering();

    g_telemetry.bootstrapped.store(true, std::memory_order_release);
    g_bootstrapping.store(false, std::memory_order_release);
}

void OnSlInit(sl::Preferences& pref)
{
    if (!Enabled() || !Config::Instance()->MfgUnlockForceOta.value_or_default())
        return;

    constexpr uint64_t kOta = static_cast<uint64_t>(sl::PreferenceFlags::eAllowOTA) |
                              static_cast<uint64_t>(sl::PreferenceFlags::eLoadDownloadedPlugins);

    const uint64_t before = static_cast<uint64_t>(pref.flags);
    g_telemetry.otaFlagsBefore.store(before, std::memory_order_relaxed);
    if ((before & kOta) == kOta)
    {
        LOG_INFO("slInit already requests OTA plugins; nothing to change.");
        return;
    }

    pref.flags = static_cast<sl::PreferenceFlags>(before | kOta);
    g_telemetry.otaForced.store(true, std::memory_order_relaxed);
    LOG_INFO("slInit flags 0x{:x} -> 0x{:x} (eAllowOTA | eLoadDownloadedPlugins) so the driver's OTA plugin set is "
             "used",
             before, before | kOta);
}

bool ForceSetOptions(const sl::ViewportHandle& viewport, sl::DLSSGOptions& options, sl::Result& result,
                     decltype(&slDLSSGSetOptions) real)
{
    if (real == nullptr)
        return false;

    const unsigned int multiplier = ForceMultiplier();
    if (multiplier < 2 || options.mode == sl::DLSSGMode::eOff || !Active())
        return false;

    Bootstrap();
    if (RescanRequested())
        RunProviderMaintenance();

    const uint32_t desired = multiplier - 1; // generated frames, not total
    const uint32_t requested = options.numFramesToGenerate;
    g_telemetry.lastRequested.store(requested, std::memory_order_relaxed);
    if (requested >= desired)
        return false;

    // More than one generated frame needs software pacing on Ada. The plugin is loaded by now, so this
    // is the last and best chance to patch it -- and the reason to refuse if that fails.
    if (desired > 1)
    {
        if (!PacingReady())
            TryPatchFlipMetering();

        if (!PacingReady())
        {
            g_telemetry.declinedNoPacing.store(true, std::memory_order_relaxed);
            if (!g_declineLogged.exchange(true, std::memory_order_relaxed))
                LOG_WARN("NOT forcing the multiplier -- flip metering is still enabled, and asking for more than "
                         "one generated frame without software pacing freezes presentation. Leaving the game's "
                         "own request alone.");
            return false;
        }
    }

    options.numFramesToGenerate = desired;
    result = real(viewport, options);

    if (result != sl::Result::eOk)
    {
        // eErrorFeatureManagerInvalidState is not "your count is too high" -- it can simply mean DLSS-G
        // was not ready yet. Retry once with the SAME raised count before giving the game's request back.
        const sl::Result retry = real(viewport, options);
        if (retry == sl::Result::eOk)
        {
            if (!g_telemetry.intercepted.exchange(true, std::memory_order_relaxed))
                LOG_INFO("slDLSSGSetOptions returned {} on the first attempt but accepted numFramesToGenerate={} "
                         "on retry -- that first failure was feature-manager state, not the count.",
                         static_cast<unsigned int>(result), desired);
            g_telemetry.lastResult.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
            g_telemetry.lastForced.store(desired, std::memory_order_relaxed);
            result = retry;
            return true;
        }

        if (g_telemetry.forceFailedFor.exchange(desired, std::memory_order_relaxed) != desired)
            LOG_WARN("slDLSSGSetOptions refused numFramesToGenerate={} twice (sl::Result {} then {}); falling back "
                     "to the game's own request of {}. The count itself is being refused, not a transient state.",
                     desired, static_cast<unsigned int>(result), static_cast<unsigned int>(retry), requested);

        g_telemetry.lastResult.store(static_cast<unsigned int>(retry), std::memory_order_relaxed);
        options.numFramesToGenerate = requested;
        result = real(viewport, options);
        return true;
    }

    if (!g_telemetry.intercepted.exchange(true, std::memory_order_relaxed))
        LOG_INFO("raising DLSS-G numFramesToGenerate from {} to {} ({}x) -- the game only ever asks for {}x. "
                 "slDLSSGSetOptions accepted it.",
                 requested, desired, multiplier, requested + 1);

    g_telemetry.lastResult.store(static_cast<unsigned int>(result), std::memory_order_relaxed);
    g_telemetry.lastForced.store(desired, std::memory_order_relaxed);
    return true;
}

void ObserveNativeSetOptions(const sl::DLSSGOptions& options, sl::Result result)
{
    if (!Enabled() || options.mode == sl::DLSSGMode::eOff)
        return;

    Bootstrap();
    if (Active() && RescanRequested())
        RunProviderMaintenance();

    const unsigned int requested = options.numFramesToGenerate;
    const unsigned int previous = g_telemetry.nativeRequested.exchange(requested, std::memory_order_relaxed);
    const unsigned int raw = static_cast<unsigned int>(result);
    const unsigned int previousResult = g_telemetry.nativeResult.exchange(raw, std::memory_order_relaxed);
    const bool first = !g_telemetry.nativeRequestSeen.exchange(true, std::memory_order_relaxed);

    if (first || previous != requested || previousResult != raw)
    {
        if (result == sl::Result::eOk)
            LOG_INFO("native slDLSSGSetOptions requested numFramesToGenerate={} ({}x) and returned sl::Result {}.",
                     requested, requested + 1, raw);
        else
            LOG_WARN("native slDLSSGSetOptions requested numFramesToGenerate={} ({}x) and returned sl::Result {}.",
                     requested, requested + 1, raw);
    }
}

void AdjustGetState(sl::DLSSGState& state, sl::Result result)
{
    if (!Enabled())
        return;

    const unsigned int raw = static_cast<unsigned int>(result);
    g_telemetry.stateResult.store(raw, std::memory_order_relaxed);

    if (result == sl::Result::eOk)
    {
        const unsigned int status = static_cast<unsigned int>(state.status);
        g_telemetry.dlssgStatus.store(status, std::memory_order_relaxed);
        const unsigned int presented = state.numFramesActuallyPresented;
        const unsigned int previous = g_telemetry.actualPresented.exchange(presented, std::memory_order_relaxed);
        unsigned int observedMax = g_telemetry.maxActualPresented.load(std::memory_order_relaxed);
        while (presented > observedMax &&
               !g_telemetry.maxActualPresented.compare_exchange_weak(observedMax, presented,
                                                                     std::memory_order_relaxed))
        {
        }
        g_telemetry.stateSamples.fetch_add(1, std::memory_order_relaxed);
        const bool first = !g_telemetry.stateSeen.exchange(true, std::memory_order_relaxed);

        if (status == 0)
        {
            if (!g_statusOkLogged.exchange(true, std::memory_order_relaxed))
                LOG_INFO("slDLSSGGetState runtime status is 0 (OK).");
        }
        else if (!g_statusFailLogged.exchange(true, std::memory_order_relaxed))
        {
            LOG_WARN("slDLSSGGetState runtime status is 0x{:x} (DLSS-G reported one or more failure flags).",
                     status);
        }

        if (first || previous != presented)
        {
            const unsigned int bit = presented < 32 ? (1u << presented) : 0u;
            const unsigned int seen =
                bit == 0 ? ~0u : g_seenPresentCounts.fetch_or(bit, std::memory_order_relaxed);
            if (first || (seen & bit) == 0)
                LOG_INFO("slDLSSGGetState reports {} frame(s) actually presented since its previous call.",
                         presented);
        }
    }

    if (result != sl::Result::eOk || state.structVersion < sl::kStructVersion2)
        return;

    const unsigned int reported = state.numFramesToGenerateMax;
    g_telemetry.runtimeMaxGenerated.store(reported, std::memory_order_relaxed);

    if (!Active())
        return;

    const unsigned int wanted = AdvertisedMaxGenerated();
    if (wanted < 2 || reported >= wanted)
        return;

    state.numFramesToGenerateMax = wanted;
    if (!g_telemetry.capacityAdvertised.exchange(true, std::memory_order_relaxed))
        LOG_INFO("slDLSSGGetState reported a maximum of {} generated frame(s); advertising the verified Streamline "
                 "ceiling of {} so the game's native multiplier selector can expose up to {}x.",
                 reported, wanted, wanted + 1);
}

} // namespace MfgUnlock
