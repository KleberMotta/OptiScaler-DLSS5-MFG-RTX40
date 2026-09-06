#include "pch.h"

#include "MfgUnlock_Patches.h"
#include "MfgUnlock_Midpoint.h"

#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace MfgUnlock
{
namespace
{

// ---------------------------------------------------------------------------
// HOW THE CAPABILITY IS DECIDED -- three gates, outermost first
//
// 1. Which GPUs the snippet claims at all. nvngx_dlssg.dll exports
//
//        NVSDK_NGX_GetGPUArchitecture:   mov eax, 0x190   ; Ada
//                                        ret
//
//    a hardcoded minimum architecture that NGX reads before anything else. It
//    matches each snippet's published hardware requirement exactly --
//    nvngx_dlss 0x160 (Turing), nvngx_dlssg 0x190 (Ada), nvngx_dlssnr 0x1b0
//    (Blackwell). A 40-series card already clears this one, so it is left
//    alone; it is documented because it is the first thing to read when a
//    feature is missing *entirely* rather than merely limited.
//
// 2. How many frames the snippet advertises, in
//    DLSSGInstanceManager::PopulateParameters:
//
//        cmp ebp, 0x1b0        ; NVAPI arch id, 0x1b0 == GB20x (RTX 50)
//        jl  <not supported>   ; anything below -->
//        mov edi, 5            ;   Blackwell: max frame count 5
//        ...
//        <not supported>: mov edi, 1
//        ... Set("DLSSG.MultiFrameCountMax", edi)
//
// 3. A second compare against the same constant, feeding a runtime capability
//    flag that drives generation itself:
//
//        cmp   eax, 0x1b0
//        setae al
//        mov   byte ptr [rdi+0x28], al
//
//    Patching (2) without (3) is the worst of both: the options appear, the
//    runtime accepts the request, and the game renders black. That is exactly
//    what "3x/4x renders black" was.
//
// So rewrite 0x1b0 -> 0x190 at every *compare*, in both encodings (3D imm32 and
// 81 /7 imm32), and deliberately leave `mov r32, 0x1b0` alone -- that is the
// arch-id lookup table returning Blackwell's own id, not a gate.
// ---------------------------------------------------------------------------

constexpr unsigned char kArchOld = 0xB0; // 0x1b0 GB20x
constexpr unsigned char kArchNew = 0x90; // 0x190 AD10x

// ------------------------------------------------ locating the DLSS-G snippet
//
// Finding it as GetModuleHandleW(L"nvngx_dlssg.dll") is the same mistake that
// already cost a silent no-op on the Streamline side: NGX can load the snippet
// from the driver's OTA store, and a game may stage it under another path. The
// name is a fast path, not a contract.
//
// The game-folder DLL and the driver's ...\models\dlssg\... OTA path are
// unambiguous. For renamed providers elsewhere, fall back to the NGX provider
// export plus the older "dlfg_kernel" descriptor. This matters in STALKER 2,
// whose active provider is an opaque .bin from the driver cache and whose
// current build no longer contains that descriptor.
constexpr char kDlssgMarker[] = "dlfg_kernel";

// ------------------------------------------- flip metering (sl.dlss_g)
//
// With the gate open, 2x works but 3x/4x freeze the display while audio keeps
// running -- frames are generated and never reach the screen. Blackwell paces
// multi-frame output with hardware flip metering; Ada has none, so the present
// queue waits on something that never happens.
//
// Streamline already ships the fallback. sl.dlss_g/ngx.cpp logs
// "FG1 DLL has been detected: forcing flip-metering off." and writes a flag on
// the DLSS-G context, dropping it onto the software RSYNC pacer in rsync.cpp.
// That is the path Ada needs.
//
// Two things make this impossible to hardcode, both learned the hard way:
//
//  1. The plugin in bin/x64 is usually NOT the one running. Streamline
//     OTA-updates its plugins into
//     C:\ProgramData\NVIDIA\NGX\models\sl_dlss_g_0\versions\<n>\files\<hash>.dll
//     so GetModuleHandleW(L"sl.dlss_g.dll") finds nothing and a name-based patch
//     silently does nothing at all -- no error, no log line, no effect.
//
//  2. The flag's offset AND ITS POLARITY differ between builds. The game-folder
//     build clears [ctx+0x38bc] to mean "flip metering off"; the OTA build sets
//     [ctx+0x44f8] to 1 to mean the same thing. A hardcoded value is a coin flip
//     that silently does the opposite half the time.
//
// So derive everything from the binary: find the module carrying the marker
// string, find the code that logs it, and read the (offset, value) the fallback
// itself writes. That pair IS the wanted state, whatever its polarity. Then flip
// every other site writing that offset to match.
constexpr char kFlipMarker[] = "FG1 DLL has been detected";

constexpr int kMaxFlipMeterAttempts = 4000;

constexpr unsigned char kCeilingTarget = 5; // generated frames == 6x

struct GateSite
{
    unsigned char* address; // the byte holding the arch id's low octet
    unsigned char original;
};

struct FlipSite
{
    unsigned char* address;
    unsigned char original[7];
    unsigned char length;
};

struct MidpointModulePatch
{
    HMODULE module;
    std::vector<Midpoint::Patch> patches;
    void* allocation;
};

// One lock over every vector below. Provider state is normally updated
// synchronously by the host's loader hook; the host's own maintenance calls may
// inspect the process at the same time, so all bookkeeping is serialized. A
// loader callback must never WAIT here: if maintenance is already in progress it
// requests another pass and returns, avoiding a lock-order inversion with the
// Windows loader lock.
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic_bool g_rescanRequested { false };

// Our own image. Both marker scans look for strings that are, necessarily,
// string literals inside this very DLL -- so without excluding ourselves the
// scan happily identifies OptiScaler as the DLSS-G provider and then fails to
// make sense of it. Harmless where the real provider is enumerated first; fatal
// where it is not loaded at all.
HMODULE g_selfModule = nullptr;

std::atomic_bool g_enabled { true };
std::atomic_bool g_temporalFix { true };
std::atomic_bool g_forceFlipMeterOff { false };
std::atomic_bool g_raiseCeiling { false };

std::vector<HMODULE> g_inspectedModules;
std::vector<HMODULE> g_dlssgModules;

std::atomic_bool g_gatePatched { false };
std::vector<GateSite> g_gateSites;
std::vector<HMODULE> g_gateModules;
std::vector<HMODULE> g_gateRejectedModules;

std::atomic_bool g_midpointPatched { false };
std::vector<MidpointModulePatch> g_midpointModules;
std::vector<HMODULE> g_midpointRejectedModules;
std::string g_midpointDetail;
std::atomic<int> g_midpointAttempts { 0 };

std::atomic_bool g_flipMeterPatched { false };
std::vector<FlipSite> g_flipMeterSites;
std::atomic<unsigned int> g_flipMeterOffset { 0 };
std::atomic<unsigned int> g_flipMeterValue { 0 };
std::atomic<int> g_flipMeterAttempts { 0 };

std::atomic_bool g_ceilingPatched { false };
unsigned char* g_ceilingSite = nullptr;
unsigned char g_ceilingOriginal = 0;
unsigned char g_ceilingCmovOriginal = 0;
unsigned int g_ceilingCompiled = 0;
unsigned int g_ceilingEffective = 0;
std::atomic<unsigned int> g_advertisedMaxGenerated { 0 };

Status g_status;

// ---------------------------------------------------------------- image helpers

bool ModuleImage(HMODULE mod, unsigned char** outBase, const IMAGE_NT_HEADERS64** outNt)
{
    if (mod == nullptr)
        return false;
    auto* base = reinterpret_cast<unsigned char*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;
    *outBase = base;
    *outNt = nt;
    return true;
}

std::string ModulePath(HMODULE mod)
{
    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(mod, modulePath, MAX_PATH);
    return std::string(modulePath);
}

bool ModuleContains(HMODULE mod, const char* needle, size_t needleLen)
{
    auto* base = reinterpret_cast<unsigned char*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (IsBadReadPtr(base, sizeof(IMAGE_DOS_HEADER)) != 0)
        return false;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return false;
    if (nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return false;

    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < needleLen)
            continue;
        for (size_t off = 0; off + needleLen <= size; ++off)
        {
            if (std::memcmp(start + off, needle, needleLen) == 0)
                return true;
        }
    }
    return false;
}

// -------------------------------------------------------------- provider list
// Every function below assumes g_lock is held exclusively.

void RememberDlssgModule(HMODULE mod)
{
    if (mod == nullptr || mod == g_selfModule)
        return;
    if (std::find(g_dlssgModules.begin(), g_dlssgModules.end(), mod) == g_dlssgModules.end())
        g_dlssgModules.push_back(mod);
}

bool HasKnownDlssgPath(HMODULE mod)
{
    wchar_t modulePath[32768] = {};
    const DWORD length = GetModuleFileNameW(mod, modulePath, ARRAYSIZE(modulePath));
    if (length == 0 || length >= ARRAYSIZE(modulePath))
        return false;
    for (DWORD i = 0; i < length; ++i)
    {
        if (modulePath[i] >= L'A' && modulePath[i] <= L'Z')
            modulePath[i] = static_cast<wchar_t>(modulePath[i] - L'A' + L'a');
    }
    return std::wcsstr(modulePath, L"nvngx_dlssg") != nullptr ||
           std::wcsstr(modulePath, L"\\models\\dlssg\\") != nullptr;
}

bool IsDlssgProvider(HMODULE mod)
{
    // Keep the established D3D/OTA path as the fast path. Vulkan-specific export
    // checks are only needed when a game has renamed or relocated the provider.
    if (HasKnownDlssgPath(mod))
        return true;

    // A renamed provider may expose either graphics backend. Streamline's
    // slDLSSGSetOptions/slDLSSGGetState interface is renderer-independent, so
    // discovery must not discard Vulkan snippets before the shared patch path
    // gets a chance to inspect them.
    const bool hasD3D12Entry = GetProcAddress(mod, "NVSDK_NGX_D3D12_PopulateDeviceParameters_Impl") != nullptr;
    const bool hasVulkanEntry = GetProcAddress(mod, "NVSDK_NGX_VULKAN_PopulateDeviceParameters_Impl") != nullptr;

    // Retain content-based discovery for games that rename or relocate the
    // snippet, but only scan modules exposing an NGX provider entry point.
    if (!hasD3D12Entry && !hasVulkanEntry)
        return false;
    return ModuleContains(mod, kDlssgMarker, sizeof(kDlssgMarker) - 1);
}

// One shared bootstrap pass for providers that were mapped before OptiScaler.
// Providers mapped later are handled directly by the host's loader hook, so
// module enumeration never has to run from the presentation thread.
void DiscoverDlssgModules()
{
    if (HMODULE fast = GetModuleHandleW(L"nvngx_dlssg.dll"))
        RememberDlssgModule(fast);

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE)
        return;
    MODULEENTRY32W me = {};
    me.dwSize = sizeof(me);
    if (Module32FirstW(snap, &me))
    {
        do
        {
            if (me.hModule == g_selfModule)
                continue;
            if (std::find(g_inspectedModules.begin(), g_inspectedModules.end(), me.hModule) !=
                g_inspectedModules.end())
            {
                continue;
            }
            g_inspectedModules.push_back(me.hModule);
            if (IsDlssgProvider(me.hModule))
                RememberDlssgModule(me.hModule);
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
}

// ------------------------------------------------------- in-memory arch gate

// Split out so the load-time trigger can patch a module it already holds a
// handle to. That path runs under the loader lock, where CreateToolhelp32Snapshot
// would deadlock -- so it must never scan.
void PatchArchGatesInModule(HMODULE mod)
{
    if (mod == nullptr)
        return;
    if (std::find(g_gateModules.begin(), g_gateModules.end(), mod) != g_gateModules.end())
        return;
    if (std::find(g_gateRejectedModules.begin(), g_gateRejectedModules.end(), mod) != g_gateRejectedModules.end())
        return;

    auto* base = reinterpret_cast<unsigned char*>(mod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return;

    std::vector<unsigned char*> found;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < 6)
            continue;
        for (size_t off = 0; off + 6 <= size; ++off)
        {
            // 3D id32            cmp eax, imm32
            if (start[off] == 0x3D && start[off + 1] == kArchOld && start[off + 2] == 0x01 && start[off + 3] == 0x00 &&
                start[off + 4] == 0x00)
            {
                found.push_back(start + off + 1);
                continue;
            }
            // 81 /7 id32         cmp r32, imm32
            if (start[off] == 0x81 && start[off + 1] >= 0xF8 && start[off + 1] <= 0xFF && start[off + 2] == kArchOld &&
                start[off + 3] == 0x01 && start[off + 4] == 0x00 && start[off + 5] == 0x00)
            {
                found.push_back(start + off + 2);
            }
        }
    }

    // One to four is what every shipped provider has. Anything else means this is
    // not the code we think it is, and a stray rewrite in an unrelated module is
    // far worse than doing nothing.
    if (found.empty() || found.size() > 4)
    {
        LOG_WARN("found {} arch-gate comparisons in {} (expected 1-4); leaving this provider alone.", found.size(),
                 ModulePath(mod));
        g_gateRejectedModules.push_back(mod);
        return;
    }

    const size_t sitesBefore = g_gateSites.size();
    for (unsigned char* site : found)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(site, 1, PAGE_EXECUTE_READWRITE, &oldProtect) == 0)
            continue;
        g_gateSites.push_back({ site, *site });
        *site = kArchNew;
        DWORD ignored = 0;
        VirtualProtect(site, 1, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), site, 1);
    }

    const size_t sitesWritten = g_gateSites.size() - sitesBefore;
    if (sitesWritten == 0)
    {
        LOG_ERROR("could not make the DLSS-G arch gates writable in {}.", ModulePath(mod));
        g_gateRejectedModules.push_back(mod);
        return;
    }
    g_gateModules.push_back(mod);
    g_gatePatched.store(true, std::memory_order_release);

    LOG_INFO("rewrote {} arch gate(s) (0x1b0 -> 0x190) in {}; multi-frame should report as supported AND generate.",
             sitesWritten, ModulePath(mod));
}

void TryPatchDlssgArchGate()
{
    for (HMODULE mod : g_dlssgModules)
        PatchArchGatesInModule(mod);
}

void RestoreDlssgArchGate()
{
    if (!g_gatePatched.load(std::memory_order_acquire))
        return;
    for (const auto& site : g_gateSites)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(site.address, 1, PAGE_EXECUTE_READWRITE, &oldProtect) != 0)
        {
            *site.address = site.original;
            DWORD ignored = 0;
            VirtualProtect(site.address, 1, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), site.address, 1);
        }
    }
    g_gateSites.clear();
    g_gateModules.clear();
    g_gateRejectedModules.clear();
    g_gatePatched.store(false, std::memory_order_release);
}

// ------------------------------------------------------ temporal (midpoint)
//
// Unlocking the multipliers gets the right NUMBER of generated frames; this
// gets the right CONTENT. Without it every generated frame is the same 0.5
// blend, so 4x shows three identical half-way frames and the motion is no
// smoother than 2x despite double the counter. See MfgUnlock_Midpoint.h.

void PatchMidpointInModule(HMODULE mod)
{
    if (mod == nullptr)
        return;
    const auto alreadyPatched = std::find_if(g_midpointModules.begin(), g_midpointModules.end(),
                                             [mod](const MidpointModulePatch& patch) { return patch.module == mod; });
    if (alreadyPatched != g_midpointModules.end())
        return;
    if (std::find(g_midpointRejectedModules.begin(), g_midpointRejectedModules.end(), mod) !=
        g_midpointRejectedModules.end())
    {
        return;
    }

    std::vector<Midpoint::Patch> patches;
    void* allocation = nullptr;
    std::string detail;
    if (!Midpoint::Apply(mod, patches, allocation, detail))
    {
        LOG_WARN("temporal fix not applied to {} -- {}.", ModulePath(mod), detail);
        g_midpointRejectedModules.push_back(mod);
        return;
    }

    g_midpointDetail = detail;
    g_midpointModules.push_back({ mod, std::move(patches), allocation });
    g_midpointPatched.store(true, std::memory_order_release);
    LOG_INFO("temporal fix applied to {} -- {}; generated frames should now land at their own time, not all at the "
             "midpoint.",
             ModulePath(mod), detail);
}

void TryPatchMidpoint()
{
    ++g_midpointAttempts;
    for (HMODULE mod : g_dlssgModules)
        PatchMidpointInModule(mod);
}

void RestoreMidpoint()
{
    if (!g_midpointPatched.load(std::memory_order_acquire))
        return;
    for (auto& module : g_midpointModules)
        Midpoint::Restore(module.patches, module.allocation);
    g_midpointModules.clear();
    g_midpointRejectedModules.clear();
    g_midpointPatched.store(false, std::memory_order_release);
}

// ------------------------------------------- Streamline's own frame ceiling
//
// The plugin starts with its own compiled maximum, then lowers it to the value
// reported by NGX:
//
//     BA 03 00 00 00   mov   edx, 3
//     3B CA            cmp   ecx, edx
//     0F 42 D1         cmovb edx, ecx      ; edx = min(count, 3)
//
// `ecx` is the device maximum cached by the Streamline wrapper, not the game's
// current request. Most games observe the rewritten NGX gates early enough for
// it to be 5. STALKER 2 does not: its wrapper caches 1, so this CMOV reduces the
// otherwise-valid compiled maximum back to one generated frame. The native UI
// can then expose 3x/4x, but slDLSSGSetOptions rejects either with
// eErrorInvalidState (38).
//
// Turn the conditional move into `cmovb edx, edx` by changing only its ModRM
// byte (D1 -> D2). This is an atomic one-byte code patch and leaves the
// instruction boundary intact. The immediate stays in place as a hard bound:
// old plugins remain capped at their compiled 3 generated frames (4x), while
// newer plugins keep their compiled 5 (6x). The opt-in raiseCeiling setting may
// still raise an old plugin's immediate, but is deliberately separate because
// doing so is not safe in every game.

void PatchFrameCountCeiling(HMODULE mod)
{
    if (g_ceilingPatched.load(std::memory_order_acquire))
        return;

    unsigned char* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!ModuleImage(mod, &base, &nt))
        return;

    const unsigned char tail[] = { 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
    unsigned char* found = nullptr;
    size_t hits = 0;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < 10)
            continue;
        for (size_t off = 0; off + 10 <= size; ++off)
        {
            if (start[off] != 0xBA)
                continue;
            if (start[off + 2] != 0 || start[off + 3] != 0 || start[off + 4] != 0)
                continue;
            if (std::memcmp(start + off + 5, tail, sizeof(tail)) != 0)
                continue;
            const unsigned char ceiling = start[off + 1];
            if (ceiling == 0 || ceiling > 8)
                continue;
            if (found == nullptr)
                found = start + off;
            ++hits;
        }
    }

    if (hits != 1 || found == nullptr)
    {
        LOG_WARN("found {} frame-count clamps in the DLSS-G plugin (expected 1); leaving them alone.", hits);
        return;
    }

    DWORD oldProtect = 0;
    if (VirtualProtect(found, 10, PAGE_EXECUTE_READWRITE, &oldProtect) == 0)
        return;
    g_ceilingSite = found;
    g_ceilingOriginal = found[1];
    g_ceilingCmovOriginal = found[9];
    g_ceilingCompiled = found[1];
    g_ceilingEffective = g_ceilingCompiled;
    if (g_raiseCeiling.load(std::memory_order_relaxed) && found[1] < kCeilingTarget)
    {
        found[1] = kCeilingTarget;
        g_ceilingEffective = kCeilingTarget;
    }
    // cmovb edx, ecx -> cmovb edx, edx: same three-byte instruction, no lowering.
    found[9] = 0xD2;
    DWORD ignored = 0;
    VirtualProtect(found, 10, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), found, 10);
    g_ceilingPatched.store(true, std::memory_order_release);
    g_advertisedMaxGenerated.store(g_ceilingEffective, std::memory_order_release);

    if (g_ceilingEffective != g_ceilingCompiled)
    {
        LOG_INFO("stopped the DLSS-G plugin from lowering its compiled ceiling of {} generated frame(s) to the stale "
                 "NGX device value; RaiseFrameCeiling also changed the hard bound to {} (effective maximum {}x).",
                 g_ceilingCompiled, g_ceilingEffective, g_ceilingEffective + 1);
    }
    else
    {
        LOG_INFO("stopped the DLSS-G plugin from lowering its compiled ceiling of {} generated frame(s) to the stale "
                 "NGX device value (effective maximum {}x).",
                 g_ceilingCompiled, g_ceilingEffective + 1);
    }
}

void RestoreFrameCountCeiling()
{
    if (!g_ceilingPatched.load(std::memory_order_acquire))
        return;
    if (g_ceilingSite == nullptr)
        return;
    DWORD oldProtect = 0;
    if (VirtualProtect(g_ceilingSite, 10, PAGE_EXECUTE_READWRITE, &oldProtect) != 0)
    {
        g_ceilingSite[9] = g_ceilingCmovOriginal;
        g_ceilingSite[1] = g_ceilingOriginal;
        DWORD ignored = 0;
        VirtualProtect(g_ceilingSite, 10, oldProtect, &ignored);
        FlushInstructionCache(GetCurrentProcess(), g_ceilingSite, 10);
    }
    g_ceilingSite = nullptr;
    g_ceilingOriginal = 0;
    g_ceilingCmovOriginal = 0;
    g_ceilingCompiled = 0;
    g_ceilingEffective = 0;
    g_advertisedMaxGenerated.store(0, std::memory_order_release);
    g_ceilingPatched.store(false, std::memory_order_release);
}

// -------------------------------------------------------------- flip metering

// Records the original bytes before writing, so the instruction can be put back
// exactly as it was. Patches here are either one byte (an immediate flipped in
// place) or seven (a whole store rewritten), never anything else.
bool WriteFlipSite(unsigned char* at, const unsigned char* bytes, size_t length)
{
    if (length == 0 || length > sizeof(FlipSite::original))
        return false;
    DWORD oldProtect = 0;
    if (VirtualProtect(at, length, PAGE_EXECUTE_READWRITE, &oldProtect) == 0)
        return false;
    FlipSite site = {};
    site.address = at;
    site.length = static_cast<unsigned char>(length);
    std::memcpy(site.original, at, length);
    g_flipMeterSites.push_back(site);
    std::memcpy(at, bytes, length);
    DWORD ignored = 0;
    VirtualProtect(at, length, oldProtect, &ignored);
    FlushInstructionCache(GetCurrentProcess(), at, length);
    return true;
}

// Handles the DLSS-G Streamline plugin wherever it was loaded from. Returns true
// once a module has been dealt with, so the caller stops scanning.
bool TryPatchFlipMeteringInModule(HMODULE mod)
{
    unsigned char* base = nullptr;
    const IMAGE_NT_HEADERS64* nt = nullptr;
    if (!ModuleImage(mod, &base, &nt))
        return false;

    // 1. Is this the DLSS-G plugin? The marker string identifies it regardless of
    //    what the OTA layer decided to call the file.
    const size_t markerLen = sizeof(kFlipMarker) - 1;
    const unsigned char* marker = nullptr;
    const auto* section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && marker == nullptr; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_READ) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < markerLen)
            continue;
        for (size_t off = 0; off + markerLen <= size; ++off)
        {
            if (std::memcmp(start + off, kFlipMarker, markerLen) == 0)
            {
                marker = start + off;
                break;
            }
        }
    }
    if (marker == nullptr)
        return false;

    ++g_flipMeterAttempts;

    // 2. Find the code referencing it, then read the (offset, value) the fallback
    //    writes: C6 /r disp32 imm8 == mov byte ptr [reg+disp32], imm8.
    unsigned int wantOffset = 0;
    int wantValue = -1;
    section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections && wantValue < 0; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < 8)
            continue;
        for (size_t off = 0; off + 8 <= size && wantValue < 0; ++off)
        {
            // lea reg, [rip+disp32] pointing at the marker string
            if (!(start[off] == 0x48 || start[off] == 0x4C))
                continue;
            if (start[off + 1] != 0x8D)
                continue;
            if ((start[off + 2] & 0xC7) != 0x05)
                continue;
            int disp = 0;
            std::memcpy(&disp, start + off + 3, sizeof(disp));
            if (start + off + 7 + disp != marker)
                continue;

            const size_t window = 0x200;
            const size_t limit = (off + window < size) ? (off + window) : size;
            for (size_t w = off; w + 7 <= limit; ++w)
            {
                if (start[w] != 0xC6)
                    continue;
                if (start[w + 1] < 0x80 || start[w + 1] > 0xBF)
                    continue; // mod=10, disp32
                unsigned int field = 0;
                std::memcpy(&field, start + w + 2, sizeof(field));
                const unsigned char imm = start[w + 6];
                if (field <= 0x100 || field >= 0x20000)
                    continue;
                if (imm > 1)
                    continue;
                wantOffset = field;
                wantValue = imm;
                break;
            }
        }
    }

    if (wantValue < 0)
    {
        // Name the module. Which DLSS-G plugin is actually loaded varies wildly --
        // game-bundled, driver OTA, or an NVIDIA App override copy -- and without
        // the path this warning says nothing actionable.
        LOG_WARN("located a DLSS-G plugin ({}) but could not read its flip-metering fallback state; leaving it alone.",
                 ModulePath(mod));
        g_flipMeterAttempts = kMaxFlipMeterAttempts;
        return true;
    }

    // 3. Pin the field to that value everywhere it is written.
    //
    //    Two encodings appear in the wild, and sl.dlss_g 2.13.0.0 introduced the
    //    second:
    //
    //      C6 /0 disp32 imm8    mov byte ptr [reg+disp32], imm8    (7 bytes)
    //      40 88 /r  disp32     mov byte ptr [reg+disp32], reg8    (7 bytes)
    //
    //    The first is flipped by rewriting its immediate. The second stores a
    //    runtime value, so there is no immediate to change -- but its REX prefix
    //    is present only to name a byte register (spl/bpl/sil/dil), which makes it
    //    exactly seven bytes: the same length as the C6 form with that same base
    //    register. That equivalence is the only reason it is patchable in place,
    //    so it is deliberately the only register store handled. A bare
    //    88 /r disp32 is six bytes and a REX.B one needs eight, and rewriting
    //    either would run past the end of the instruction.
    const unsigned char opposite = static_cast<unsigned char>(1 - wantValue);
    section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0)
            continue;
        unsigned char* start = base + section->VirtualAddress;
        const size_t size = section->Misc.VirtualSize;
        if (size < 7)
            continue;
        for (size_t off = 0; off + 7 <= size; ++off)
        {
            // C6 /0 disp32 imm8 -- flip the immediate. Unchanged from the form that
            // has been working; every other encoding is handled after it.
            if (start[off] == 0xC6)
            {
                if (start[off + 1] < 0x80 || start[off + 1] > 0xBF)
                    continue;
                unsigned int field = 0;
                std::memcpy(&field, start + off + 2, sizeof(field));
                if (field != wantOffset)
                    continue;
                if (start[off + 6] != opposite)
                    continue;
                const unsigned char imm = static_cast<unsigned char>(wantValue);
                WriteFlipSite(start + off + 6, &imm, 1);
                continue;
            }

            // 40 88 /r disp32 -- rewrite the whole store into the C6 form, keeping the
            // same base register. REX must be exactly 0x40: any of the B/R/X/W bits
            // set would change either the encoding length or the base register.
            if (start[off] != 0x40 || start[off + 1] != 0x88)
                continue;
            const unsigned char modrm = start[off + 2];
            if (modrm < 0x80 || modrm > 0xBF)
                continue; // mod=10, disp32
            const unsigned char rm = static_cast<unsigned char>(modrm & 7);
            if (rm == 4)
                continue; // rm=100 means a SIB byte follows
            unsigned int field = 0;
            std::memcpy(&field, start + off + 3, sizeof(field));
            if (field != wantOffset)
                continue;

            unsigned char replacement[7] = { 0xC6, static_cast<unsigned char>(0x80 | rm), 0, 0, 0, 0,
                                             static_cast<unsigned char>(wantValue) };
            std::memcpy(replacement + 2, &wantOffset, sizeof(wantOffset));
            WriteFlipSite(start + off, replacement, sizeof(replacement));
        }
    }

    // Deriving the field is not the same as changing anything. If no site wrote
    // the opposite value there was nothing to flip, and claiming success here
    // would report a patch that never happened -- which is exactly how a stale
    // DLL once looked like a working one.
    if (g_flipMeterSites.empty())
    {
        LOG_WARN("flip-metering field +0x{:x} derived from {}, but nothing writes it in a form this can patch -- no "
                 "immediate store of {}, and no 7-byte register store. Nothing changed.",
                 wantOffset, ModulePath(mod), 1 - wantValue);
        g_flipMeterAttempts = kMaxFlipMeterAttempts;
        return true;
    }

    g_flipMeterOffset.store(wantOffset, std::memory_order_relaxed);
    g_flipMeterValue.store(static_cast<unsigned int>(wantValue), std::memory_order_relaxed);
    g_flipMeterPatched.store(true, std::memory_order_release);

    LOG_INFO("forced flip-metering off in {} -- field +0x{:x} pinned to {} at {} site(s); multi-frame should pace in "
             "software (RSYNC).",
             ModulePath(mod), wantOffset, wantValue, g_flipMeterSites.size());

    // Same module, and by now we know it is the right one.
    PatchFrameCountCeiling(mod);
    return true;
}

void RestoreFlipMetering()
{
    if (!g_flipMeterPatched.load(std::memory_order_acquire))
        return;
    for (const auto& site : g_flipMeterSites)
    {
        DWORD oldProtect = 0;
        if (VirtualProtect(site.address, site.length, PAGE_EXECUTE_READWRITE, &oldProtect) != 0)
        {
            std::memcpy(site.address, site.original, site.length);
            DWORD ignored = 0;
            VirtualProtect(site.address, site.length, oldProtect, &ignored);
            FlushInstructionCache(GetCurrentProcess(), site.address, site.length);
        }
    }
    g_flipMeterSites.clear();
    g_flipMeterPatched.store(false, std::memory_order_release);
}

void RefreshStatusLocked()
{
    g_status.gatePatched = g_gatePatched.load(std::memory_order_acquire);
    g_status.gateSites = g_gateSites.size();
    g_status.gateModules = g_gateModules.size();

    g_status.midpointPatched = g_midpointPatched.load(std::memory_order_acquire);
    g_status.midpointDetail = g_midpointDetail;
    g_status.midpointAttempts = g_midpointAttempts;

    g_status.flipMeterPatched = g_flipMeterPatched.load(std::memory_order_acquire);
    g_status.flipMeterOffset = g_flipMeterOffset.load(std::memory_order_relaxed);
    g_status.flipMeterValue = g_flipMeterValue.load(std::memory_order_relaxed);
    g_status.flipMeterSites = g_flipMeterSites.size();
    g_status.flipMeterAttempts = g_flipMeterAttempts;
    g_status.flipMeterGaveUp = g_flipMeterAttempts >= kMaxFlipMeterAttempts;

    g_status.ceilingPatched = g_ceilingPatched.load(std::memory_order_acquire);
    g_status.ceilingCompiled = g_ceilingCompiled;
    g_status.ceilingEffective = g_ceilingEffective;

    g_status.providersSeen = static_cast<unsigned>(g_dlssgModules.size());
    g_status.providerPaths.clear();
    g_status.providerPaths.reserve(g_dlssgModules.size());
    for (HMODULE mod : g_dlssgModules)
        g_status.providerPaths.push_back(ModulePath(mod));
}

} // namespace

// ---------------------------------------------------------------- public API

void SetSelfModule(HMODULE self) { g_selfModule = self; }

void SetSettings(const Settings& settings)
{
    g_enabled.store(settings.enabled, std::memory_order_relaxed);
    g_temporalFix.store(settings.temporalFix, std::memory_order_relaxed);
    g_forceFlipMeterOff.store(settings.forceFlipMeterOff, std::memory_order_relaxed);
    g_raiseCeiling.store(settings.raiseCeiling, std::memory_order_relaxed);
}

Settings GetSettings()
{
    Settings settings;
    settings.enabled = g_enabled.load(std::memory_order_relaxed);
    settings.temporalFix = g_temporalFix.load(std::memory_order_relaxed);
    settings.forceFlipMeterOff = g_forceFlipMeterOff.load(std::memory_order_relaxed);
    settings.raiseCeiling = g_raiseCeiling.load(std::memory_order_relaxed);
    return settings;
}

void OnDlssgProviderLoaded(HMODULE mod)
{
    // A loader callback must not wait here: if maintenance is already in progress
    // it requests another pass and returns, avoiding a lock-order inversion with
    // the Windows loader lock. The module stays mapped, so the next maintenance
    // pass finds it by enumeration anyway.
    if (!TryAcquireSRWLockExclusive(&g_lock))
    {
        g_rescanRequested.store(true, std::memory_order_release);
        return;
    }
    RememberDlssgModule(mod);
    if (g_enabled.load(std::memory_order_relaxed))
        PatchArchGatesInModule(mod);
    if (g_temporalFix.load(std::memory_order_relaxed))
        PatchMidpointInModule(mod);
    ReleaseSRWLockExclusive(&g_lock);
}

void OnStreamlineDlssgPluginLoaded(HMODULE mod)
{
    if (mod == nullptr || mod == g_selfModule)
        return;
    if (g_flipMeterPatched.load(std::memory_order_acquire))
        return;
    if (!g_forceFlipMeterOff.load(std::memory_order_relaxed))
        return;

    // Same loader-lock rule as above. TryPatchFlipMetering() rescans later.
    if (!TryAcquireSRWLockExclusive(&g_lock))
    {
        g_rescanRequested.store(true, std::memory_order_release);
        return;
    }
    if (!g_flipMeterPatched.load(std::memory_order_acquire) && g_flipMeterAttempts < kMaxFlipMeterAttempts)
        TryPatchFlipMeteringInModule(mod);
    ReleaseSRWLockExclusive(&g_lock);
}

void RunProviderMaintenance()
{
    AcquireSRWLockExclusive(&g_lock);
    g_rescanRequested.store(false, std::memory_order_release);
    DiscoverDlssgModules();
    if (g_enabled.load(std::memory_order_relaxed))
        TryPatchDlssgArchGate();
    if (g_temporalFix.load(std::memory_order_relaxed))
        TryPatchMidpoint();
    ReleaseSRWLockExclusive(&g_lock);
}

bool TryPatchFlipMetering()
{
    if (g_flipMeterPatched.load(std::memory_order_acquire))
        return true;
    if (g_flipMeterAttempts >= kMaxFlipMeterAttempts)
        return false;

    AcquireSRWLockExclusive(&g_lock);
    if (!g_flipMeterPatched.load(std::memory_order_acquire) && g_flipMeterAttempts < kMaxFlipMeterAttempts)
    {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
        if (snap != INVALID_HANDLE_VALUE)
        {
            MODULEENTRY32W me = {};
            me.dwSize = sizeof(me);
            if (Module32FirstW(snap, &me))
            {
                do
                {
                    if (me.hModule == g_selfModule)
                        continue;
                    if (TryPatchFlipMeteringInModule(me.hModule))
                        break;
                } while (Module32NextW(snap, &me));
            }
            CloseHandle(snap);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return g_flipMeterPatched.load(std::memory_order_acquire);
}

bool PacingReady() { return g_flipMeterPatched.load(std::memory_order_acquire); }

unsigned AdvertisedMaxGenerated() { return g_advertisedMaxGenerated.load(std::memory_order_acquire); }

Status GetStatus()
{
    AcquireSRWLockExclusive(&g_lock);
    RefreshStatusLocked();
    Status copy = g_status;
    ReleaseSRWLockExclusive(&g_lock);
    return copy;
}

bool RescanRequested() { return g_rescanRequested.exchange(false, std::memory_order_acq_rel); }

void RestoreAll()
{
    // Reached from DLL_PROCESS_DETACH, i.e. under the loader lock. A scan on another thread may be
    // holding g_lock while it waits for that very lock (GetProcAddress, a module snapshot), so never
    // block here: try briefly, then leave the mapped patches in place rather than hang the unload.
    bool locked = false;
    for (int i = 0; i < 200; ++i)
    {
        if (TryAcquireSRWLockExclusive(&g_lock))
        {
            locked = true;
            break;
        }
        Sleep(1);
    }

    if (!locked)
    {
        LOG_WARN("could not take the patch lock at shutdown; leaving the mapped patches in place");
        return;
    }

    RestoreMidpoint();
    RestoreDlssgArchGate();
    RestoreFrameCountCeiling();
    RestoreFlipMetering();
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace MfgUnlock
