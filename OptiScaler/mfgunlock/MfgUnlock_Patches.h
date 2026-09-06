/*
 * DLSS multi-frame generation unlock for Ada (RTX 40) -- memory patches only.
 * SPDX-License-Identifier: MIT
 *
 * Ported into OptiScaler from the RenoDX "MFG Unlock" addon. This module is the
 * patching half of that addon and nothing else: it installs no hooks, starts no
 * threads, reads no config, and draws no interface. The OptiScaler host owns all
 * of that and drives this module through the API below.
 *
 * ===========================================================================
 * WHAT THE HOST MUST DO
 *
 *  0. SetSelfModule(dllModule) once, before anything else.
 *     Both marker scans look for strings that are, necessarily, string literals
 *     inside OptiScaler itself -- so without excluding our own image the scan
 *     happily identifies OptiScaler as the DLSS-G provider and then fails to
 *     make sense of it.
 *
 *  1. SetSettings({enabled, temporalFix, forceFlipMeterOff, raiseCeiling}) from
 *     wherever the host keeps its config. Everything is applied once, at patch
 *     time; changing a setting after a provider has been patched does not undo
 *     it.
 *
 *  2. OnDlssgProviderLoaded(mod) whenever the DLSS-G snippet is mapped, i.e.
 *     from the host's LoadLibrary hook when the new module's path contains
 *     "nvngx_dlssg" or "\models\dlssg\" (the driver's OTA store). Safe to call
 *     under the loader lock: it never enumerates modules, never calls
 *     LoadLibrary, and never blocks on a lock.
 *     Timing matters -- NGX runs PopulateParameters within milliseconds of the
 *     snippet being mapped, so patching on the next present is a race we only
 *     win by accident.
 *
 *  3. OnStreamlineDlssgPluginLoaded(mod) when sl.dlss_g.dll is mapped. The
 *     plugin is usually NOT the copy in the game's bin/x64: Streamline OTA
 *     updates it into
 *     C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\<n>\files\<hash>.dll
 *     so a name-based lookup silently finds nothing. Whatever the host's load
 *     hook actually sees mapped is the module to pass here.
 *
 *  4. RunProviderMaintenance() once from a normal thread at startup (it uses
 *     CreateToolhelp32Snapshot, so never from a loader callback) to catch
 *     providers mapped before OptiScaler, and again on the host's own retry
 *     cadence. TryPatchFlipMetering() likewise, when forceFlipMeterOff is on.
 *
 *  5. Wrap slDLSSGSetOptions (obtained through slGetFeatureFunction on
 *     sl.interposer.dll; it is not exported) if the host wants to force a
 *     multiplier. DLSSGOptions::numFramesToGenerate counts GENERATED frames:
 *     2x -> 1, 3x -> 2, 4x -> 3. The semantics that matter, from the original
 *     framecount.hpp:
 *       - Never lower the game's own request; only raise it.
 *       - Asking for more than ONE generated frame while hardware flip metering
 *         is still on freezes presentation. So before raising past 1, call
 *         TryPatchFlipMetering() and then check PacingReady(); if it is still
 *         false, put the game's own request through untouched and say so.
 *       - The options struct is passed by const reference and the caller owns
 *         it: raise the field, call through, then put it back exactly as found.
 *       - On failure retry ONCE with the same raised count.
 *         eErrorFeatureManagerInvalidState often just means DLSS-G was not ready
 *         yet, and dropping straight back to the game's request would silently
 *         give up a working 4x. Only if the retry also fails is the count really
 *         being refused -- then call through with the original request so the
 *         player still gets the frame generation they asked for.
 *
 *  6. Wrap slDLSSGGetState the same way and, when state.structVersion >=
 *     kStructVersion2 and AdvertisedMaxGenerated() is >= 2 and greater than
 *     state.numFramesToGenerateMax, raise numFramesToGenerateMax to it. Games
 *     such as STALKER 2 build their native 2x/3x/4x selector from that field
 *     rather than from the NGX parameter block, so the SetOptions wrapper alone
 *     cannot expose the extra choices.
 *
 *  7. Optionally, at slInit, OR eAllowOTA | eLoadDownloadedPlugins into
 *     Preferences::flags (both are in the SDK default; the app has to actively
 *     drop them) so the driver supplies a matched, signed, current plugin set --
 *     then restore the caller's flags. Off by default: GTA V Enhanced drops
 *     those flags and crashes if a newer plugin loads.
 *
 *  8. RestoreAll() at shutdown. Nothing on disk is ever modified; every patch
 *     lives in the mapped image and is reverted here.
 *
 * ===========================================================================
 * WHY THE MAPPED IMAGE AND NEVER THE FILE
 *
 * NGX verifies the snippet's Authenticode signature when it LOADS it, so the
 * same bytes changed on disk make frame generation disappear altogether. The
 * mapped copy is never re-checked.
 *
 * ===========================================================================
 * WHAT IS DELIBERATELY NOT HERE
 *
 * The original addon also hooked NVSDK_NGX_*_GetParameters and replaced slot 11
 * of the returned parameter object's vtable so "DLSSG.MultiFrameCountMax" could
 * be answered directly. That was the original approach and it does not work with
 * Streamline: sl.dlss_g builds its own NVSDK_NGX_Parameter rather than passing
 * NGX's along, so the patch arms and never fires. The arch gates below are what
 * actually does the job in every game tested, so the vtable override was not
 * ported.
 * ===========================================================================
 */

#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace MfgUnlock
{

// Plain data, refreshed on demand for the overlay. Nothing here is load bearing.
struct Status
{
    bool gatePatched = false;
    size_t gateSites = 0;
    size_t gateModules = 0;

    bool midpointPatched = false;
    std::string midpointDetail;
    int midpointAttempts = 0;

    bool flipMeterPatched = false;
    unsigned flipMeterOffset = 0;
    unsigned flipMeterValue = 0;
    size_t flipMeterSites = 0;
    int flipMeterAttempts = 0;
    // "Found the plugin, could not patch it" -- not "never found it". The attempt
    // counter only advances once a DLSS-G plugin HAS been identified, and both
    // give-up paths slam it to the maximum.
    bool flipMeterGaveUp = false;

    bool ceilingPatched = false;
    unsigned ceilingCompiled = 0;
    unsigned ceilingEffective = 0;

    unsigned providersSeen = 0;
    std::vector<std::string> providerPaths;
};

struct Settings
{
    // Rewrite the arch gates so Ada takes the Blackwell path.
    bool enabled = true;
    // Rebuild the interpolation kernel so generated frames land at their own
    // time instead of all at the midpoint.
    bool temporalFix = true;
    // Force Streamline's software (RSYNC) pacer. Leave off with current builds;
    // needed only where 3x+ freezes presentation.
    bool forceFlipMeterOff = false;
    // Raise an old plugin's own compiled frame ceiling. Off by default: it broke
    // GTA V Enhanced, whose 2.9.1.0 plugin was only ever shipped bounded at 3,
    // and lifting that bound is not the same as it being able to cope.
    bool raiseCeiling = false;
};

// OptiScaler's own module handle, excluded from every scan.
void SetSelfModule(HMODULE self);

void SetSettings(const Settings& settings);
Settings GetSettings();

// Loader-callback safe. Never scans, never loads, never blocks.
void OnDlssgProviderLoaded(HMODULE mod);

// Loader-callback safe, same rules. Runs the flip-metering derivation and the
// frame-ceiling patch against this one module.
void OnStreamlineDlssgPluginLoaded(HMODULE mod);

// Enumerates the process's modules. Normal threads only -- never from inside a
// LoadLibrary hook, where CreateToolhelp32Snapshot would deadlock.
void RunProviderMaintenance();

// Scans for the DLSS-G Streamline plugin and forces flip metering off. Normal
// threads only, for the same reason. Returns whether pacing is patched.
bool TryPatchFlipMetering();

// True once software pacing is in place, i.e. once it is safe to ask for more
// than one generated frame.
bool PacingReady();

// The verified Streamline wrapper ceiling in GENERATED frames (so 4 means 5x),
// or 0 while unknown. Feed this to slDLSSGGetState::numFramesToGenerateMax.
unsigned AdvertisedMaxGenerated();

// Refreshed under the module lock on each call. Reads of the returned reference
// are only stable until the next call, which is fine for a once-a-frame overlay.
Status GetStatus();

// True once (test-and-clear) when a provider was mapped while a scan held the lock and could not be
// handled at map time; the host should answer with RunProviderMaintenance() from a normal thread.
bool RescanRequested();

// Reverts every patch this module made, in the reverse order of the risk they
// carry. Safe to call when nothing was patched.
void RestoreAll();

} // namespace MfgUnlock
