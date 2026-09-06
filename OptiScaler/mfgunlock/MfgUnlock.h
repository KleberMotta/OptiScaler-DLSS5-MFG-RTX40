#pragma once

// DLSS Multi Frame Generation unlock for Ada (RTX 40) -- host glue.
//
// The in-memory patches themselves live in MfgUnlock_Patches.{h,cpp} (a port of the MFG Unlock ReShade
// addon by Dreamt / mavismmg, technique by dashdogy). This header is what the rest of OptiScaler talks
// to: configuration, the Streamline call filters, the module-load notifications and the overlay.
//
// Where the host has to call in:
//   LibraryLoad_Hooks   nvngx_dlssg.dll (or a \models\dlssg\ OTA provider) mapped   -> OnDlssgProviderLoaded
//   Streamline_Proxy    OptiScaler's own streamline\nvngx_dlssg.dll loaded          -> OnDlssgProviderLoaded
//   Streamline_Hooks    sl.dlss_g.dll hooked                                        -> OnStreamlineDlssgPluginLoaded
//   Streamline_Hooks    slInit                                                       -> OnSlInit
//   Streamline_Hooks    slDLSSGSetOptions                                            -> ForceSetOptions / ObserveNativeSetOptions
//   Streamline_Hooks    slDLSSGGetState                                              -> AdjustGetState
//   menu_common         overlay                                                      -> RenderMenu

#include "MfgUnlock_Patches.h"

#include <sl.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <cstdint>

class Config;

namespace MfgUnlock
{

// Everything the overlay shows that is not a patch result: what the game asked for, what we forced,
// what Streamline answered. All atomics because the hooks run on the game's threads.
struct Telemetry
{
    std::atomic_bool intercepted { false };
    std::atomic<unsigned int> lastRequested { 0 };
    std::atomic<unsigned int> lastForced { 0 };
    std::atomic<unsigned int> lastResult { 0 };
    std::atomic<unsigned int> forceFailedFor { 0 };
    std::atomic_bool declinedNoPacing { false };

    std::atomic_bool nativeRequestSeen { false };
    std::atomic<unsigned int> nativeRequested { 0 };
    std::atomic<unsigned int> nativeResult { 0 };

    std::atomic_bool stateSeen { false };
    std::atomic<unsigned int> stateResult { 0 };
    std::atomic<unsigned int> dlssgStatus { 0 };
    std::atomic<unsigned int> actualPresented { 0 };
    std::atomic<unsigned int> maxActualPresented { 0 };
    std::atomic<unsigned long long> stateSamples { 0 };
    std::atomic<unsigned int> runtimeMaxGenerated { 0 };
    std::atomic_bool capacityAdvertised { false };

    std::atomic<uint32_t> detectedArch { 0 };
    std::atomic<uint32_t> detectedDeviceId { 0 };
    std::atomic_bool skippedNotAda { false };

    std::atomic_bool otaForced { false };
    std::atomic<unsigned long long> otaFlagsBefore { 0 };

    std::atomic_bool bootstrapped { false };
};

Telemetry& GetTelemetry();

// Called once from DllMain after the logger and config exist. Records OptiScaler's own module so the
// provider scan never mistakes it for a DLSS-G snippet, and pushes the config into the patcher.
void Init(HMODULE self);

// Re-reads [MfgUnlock] from Config into the patcher. Cheap; the overlay calls it after every change.
void ApplyConfig();

// [MfgUnlock] Enabled.
bool Enabled();

// Enabled AND the primary GPU is not Blackwell. On RTX 50 the gates are already open and the temporal
// kernel is correct, so the patches would be pure risk; on anything below Ada the snippet refuses at
// its own arch gate before we could help. Unknown hardware is allowed through -- the user asked.
bool Active();

// 0 = leave the game's request alone; 2..6 = force that multiplier.
unsigned int ForceMultiplier();

// One-time bootstrap for providers mapped before our load hook was installed. Uses a module
// snapshot, so it must NOT run under the loader lock (never from DllMain or a LoadLibrary hook);
// the Streamline hooks call it from the game's own threads. Idempotent.
void Bootstrap();

// slInit: put the OTA flags back when [MfgUnlock] ForceOTAPlugins is set.
void OnSlInit(sl::Preferences& pref);

// slDLSSGSetOptions. `options` is OptiScaler's own copy of the game's struct, already carrying the
// [DLSSG] overrides. Returns true when the call was made here (result in `result`); false means the
// caller should make the call itself and hand the outcome to ObserveNativeSetOptions.
bool ForceSetOptions(const sl::ViewportHandle& viewport, sl::DLSSGOptions& options, sl::Result& result,
                     decltype(&slDLSSGSetOptions) real);
void ObserveNativeSetOptions(const sl::DLSSGOptions& options, sl::Result result);

// slDLSSGGetState: telemetry, and raising numFramesToGenerateMax to the verified plugin ceiling so a
// game that builds its multiplier menu from it (STALKER 2) can show 3x/4x.
void AdjustGetState(sl::DLSSGState& state, sl::Result result);

// The overlay section.
void RenderMenu(Config* config, float menuResScale);

} // namespace MfgUnlock
