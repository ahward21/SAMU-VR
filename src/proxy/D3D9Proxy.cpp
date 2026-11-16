// d3d9.dll export forwarder (x86 only).
// Pass-through exports use naked JMP thunks so the original stack/calling
// convention is preserved exactly. Direct3DCreate9/Ex are typed wrappers so
// later phases can intercept the IDirect3D9 / device objects.

#include "proxy/D3D9Proxy.h"
#include "vr_core/DX9DX11Bridge.h"
#include "common/Log.h"

#include <d3d9.h>
#include <MinHook.h>
#include <atomic>

#if !defined(_M_IX86)
#error "SAMU-VR d3d9 proxy must be built for Win32 (x86)."
#endif

namespace {

enum ExportIndex : int {
    E_Ord16, E_Ord17, E_Ord18, E_Ord19, E_Ord22, E_Ord23,
    E_Direct3DCreate9On12, E_Direct3DCreate9On12Ex, E_Direct3DShaderValidatorCreate9,
    E_PSGPError, E_PSGPSampleTexture,
    E_D3DPERF_BeginEvent, E_D3DPERF_EndEvent, E_D3DPERF_GetStatus, E_D3DPERF_QueryRepeatFrame,
    E_D3DPERF_SetMarker, E_D3DPERF_SetOptions, E_D3DPERF_SetRegion,
    E_DebugSetLevel, E_DebugSetMute, E_Direct3D9EnableMaximizedWindowedModeShim,
    E_Direct3DCreate9, E_Direct3DCreate9Ex,
    E_Count
};

struct ExportDesc { const char* name; WORD ordinal; };

constexpr ExportDesc kExports[E_Count] = {
    {nullptr, 16}, {nullptr, 17}, {nullptr, 18}, {nullptr, 19}, {nullptr, 22}, {nullptr, 23},
    {"Direct3DCreate9On12", 0}, {"Direct3DCreate9On12Ex", 0}, {"Direct3DShaderValidatorCreate9", 0},
    {"PSGPError", 0}, {"PSGPSampleTexture", 0},
    {"D3DPERF_BeginEvent", 0}, {"D3DPERF_EndEvent", 0}, {"D3DPERF_GetStatus", 0},
    {"D3DPERF_QueryRepeatFrame", 0}, {"D3DPERF_SetMarker", 0}, {"D3DPERF_SetOptions", 0},
    {"D3DPERF_SetRegion", 0}, {"DebugSetLevel", 0}, {"DebugSetMute", 0},
    {"Direct3D9EnableMaximizedWindowedModeShim", 0},
    {"Direct3DCreate9", 0}, {"Direct3DCreate9Ex", 0},
};

HMODULE g_realD3D9 = nullptr;
std::atomic<long> g_createCount{0};

// Fallback target when an export is missing on this Windows build.
void __stdcall MissingExport() {}

} // namespace

// Must have C linkage + fixed name so the inline asm can reference it.
extern "C" FARPROC g_realProcs[E_Count] = {};

namespace samu::proxy {

bool LoadRealD3D9() {
    if (g_realD3D9) return true;
    wchar_t sysDir[MAX_PATH] = {};
    // In a 32-bit process GetSystemDirectory -> "System32", which WOW64
    // file-system redirection maps to SysWOW64 automatically.
    GetSystemDirectoryW(sysDir, MAX_PATH);
    wchar_t path[MAX_PATH] = {};
    wsprintfW(path, L"%s\\d3d9.dll", sysDir);
    g_realD3D9 = LoadLibraryW(path);
    if (!g_realD3D9) return false;

    for (int i = 0; i < E_Count; ++i) {
        const auto& e = kExports[i];
        FARPROC p = e.name ? GetProcAddress(g_realD3D9, e.name)
                           : GetProcAddress(g_realD3D9, MAKEINTRESOURCEA(e.ordinal));
        g_realProcs[i] = p ? p : reinterpret_cast<FARPROC>(&MissingExport);
    }
    return true;
}

void UnloadRealD3D9() {
    // Intentionally do not FreeLibrary during process detach.
    g_realD3D9 = nullptr;
}

long Direct3DCreateCount() { return g_createCount.load(); }

} // namespace samu::proxy

// ---------------------------------------------------------------------------
// Naked forwarding thunks
// ---------------------------------------------------------------------------
#define SAMU_FORWARD(fn, idx)                                       \
    extern "C" __declspec(naked) void Proxy_##fn() {                \
        __asm { jmp dword ptr [g_realProcs + (idx) * 4] }           \
    }

SAMU_FORWARD(Ord16, E_Ord16)
SAMU_FORWARD(Ord17, E_Ord17)
SAMU_FORWARD(Ord18, E_Ord18)
SAMU_FORWARD(Ord19, E_Ord19)
SAMU_FORWARD(Ord22, E_Ord22)
SAMU_FORWARD(Ord23, E_Ord23)
SAMU_FORWARD(Direct3DCreate9On12, E_Direct3DCreate9On12)
SAMU_FORWARD(Direct3DCreate9On12Ex, E_Direct3DCreate9On12Ex)
SAMU_FORWARD(Direct3DShaderValidatorCreate9, E_Direct3DShaderValidatorCreate9)
SAMU_FORWARD(PSGPError, E_PSGPError)
SAMU_FORWARD(PSGPSampleTexture, E_PSGPSampleTexture)
SAMU_FORWARD(D3DPERF_BeginEvent, E_D3DPERF_BeginEvent)
SAMU_FORWARD(D3DPERF_EndEvent, E_D3DPERF_EndEvent)
SAMU_FORWARD(D3DPERF_GetStatus, E_D3DPERF_GetStatus)
SAMU_FORWARD(D3DPERF_QueryRepeatFrame, E_D3DPERF_QueryRepeatFrame)
SAMU_FORWARD(D3DPERF_SetMarker, E_D3DPERF_SetMarker)
SAMU_FORWARD(D3DPERF_SetOptions, E_D3DPERF_SetOptions)
SAMU_FORWARD(D3DPERF_SetRegion, E_D3DPERF_SetRegion)
SAMU_FORWARD(DebugSetLevel, E_DebugSetLevel)
SAMU_FORWARD(DebugSetMute, E_DebugSetMute)
SAMU_FORWARD(Direct3D9EnableMaximizedWindowedModeShim, E_Direct3D9EnableMaximizedWindowedModeShim)

// ---------------------------------------------------------------------------
// Typed wrappers (interception points for Phase 3)
// ---------------------------------------------------------------------------
using PFN_Direct3DCreate9   = IDirect3D9*(WINAPI*)(UINT);
using PFN_Direct3DCreate9Ex = HRESULT(WINAPI*)(UINT, IDirect3D9Ex**);

namespace {
PFN_Direct3DCreate9 g_origDirect3DCreate9Hook = nullptr;
std::atomic<bool>   g_hookedSysD3D9Create{false};

IDirect3D9* WINAPI Detour_Direct3DCreate9(UINT sdkVersion) {
    auto fn = g_origDirect3DCreate9Hook
                  ? g_origDirect3DCreate9Hook
                  : reinterpret_cast<PFN_Direct3DCreate9>(g_realProcs[E_Direct3DCreate9]);
    IDirect3D9* d3d = fn ? fn(sdkVersion) : nullptr;
    long n = ++g_createCount;
    LOG_INFO("Direct3DCreate9(SDK=%u) -> %p (call #%ld)", sdkVersion, static_cast<void*>(d3d), n);
    if (d3d) {
        vr::DX9DX11Bridge::OnDirect3DCreate9(d3d);
    }
    return d3d;
}
} // namespace

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion) {
    return Detour_Direct3DCreate9(sdkVersion);
}

extern "C" HRESULT WINAPI Proxy_Direct3DCreate9Ex(UINT sdkVersion, IDirect3D9Ex** out) {
    auto real = reinterpret_cast<PFN_Direct3DCreate9Ex>(g_realProcs[E_Direct3DCreate9Ex]);
    HRESULT hr = real(sdkVersion, out);
    long n = ++g_createCount;
    LOG_INFO("Direct3DCreate9Ex(SDK=%u) -> hr=0x%08lX obj=%p (call #%ld)", sdkVersion,
             static_cast<unsigned long>(hr), out ? static_cast<void*>(*out) : nullptr, n);
    if (SUCCEEDED(hr) && out && *out) {
        vr::DX9DX11Bridge::OnDirect3DCreate9(*out);
    }
    return hr;
}

namespace samu::proxy {

void EnsureSystemDirect3DCreate9Hooked() {
    if (g_hookedSysD3D9Create.load(std::memory_order_relaxed)) return;
    if (!g_realD3D9 && !LoadRealD3D9()) return;

    void* target = reinterpret_cast<void*>(g_realProcs[E_Direct3DCreate9]);
    if (!target || target == reinterpret_cast<void*>(&MissingExport)) return;

    MH_Initialize();
    MH_STATUS st = MH_CreateHook(target,
                                 reinterpret_cast<LPVOID>(&Detour_Direct3DCreate9),
                                 reinterpret_cast<LPVOID*>(&g_origDirect3DCreate9Hook));
    if (st == MH_OK) {
        st = MH_EnableHook(target);
    }
    if (st == MH_OK) {
        g_hookedSysD3D9Create.store(true, std::memory_order_release);
        LOG_INFO("D3D9Proxy: hooked system d3d9.dll!Direct3DCreate9 @ %p (trampoline=%p)",
                 target, reinterpret_cast<void*>(g_origDirect3DCreate9Hook));
    }
}

} // namespace samu::proxy
