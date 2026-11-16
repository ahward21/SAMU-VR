#include "vr_core/DX9DX11Bridge.h"
#include "vr_core/OpenXRManager.h"
#include "common/Config.h"
#include "common/Log.h"

#include <MinHook.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace vr {
namespace {

template <typename T>
void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

using PFN_D3D9_CreateDevice = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
using PFN_D3D9_EndScene = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PFN_D3D9_BeginScene = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using PFN_D3D9_Present  = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using PFN_D3D9_Reset    = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);

PFN_D3D9_CreateDevice g_origCreateDevice = nullptr;
PFN_D3D9_EndScene     g_origEndScene     = nullptr;
PFN_D3D9_BeginScene   g_origBeginScene   = nullptr;
void*                 g_targetBeginScene = nullptr;
std::atomic<HWND>     g_gameWindow{nullptr};
PFN_D3D9_Present      g_origPresent      = nullptr;
PFN_D3D9_Reset        g_origReset        = nullptr;

void* g_targetCreateDevice = nullptr;
void* g_targetEndScene     = nullptr;
void* g_targetPresent      = nullptr;
void* g_targetReset        = nullptr;

std::atomic<bool>     g_d3d9CreateHooked{false};
std::atomic<bool>     g_d3d9DeviceHooked{false};
std::atomic<uint64_t> g_endSceneCalls{0};
std::atomic<uint64_t> g_presentCalls{0};
std::atomic<uint64_t> g_stereoCaptured{0};
std::atomic<OpenXRManager*> g_boundXrManager{nullptr};
std::atomic<DX9DX11Bridge::FrameTagProvider> g_tagProvider{nullptr};

// Forces immediate presentation so the desktop monitor's refresh can't cap the game below the HMD rate.
void ApplyVSyncOverride(D3DPRESENT_PARAMETERS* pp, const char* where) {
    if (!pp || !samu::config::Get().disableVSync) return;
    if (pp->PresentationInterval != D3DPRESENT_INTERVAL_IMMEDIATE) {
        LOG_INFO("DX9DX11Bridge: %s PresentationInterval 0x%08lX -> IMMEDIATE (vsync off for VR)",
                 where, static_cast<unsigned long>(pp->PresentationInterval));
        pp->PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    }
}

DX9DX11Bridge g_bridgeInstance;

HRESULT STDMETHODCALLTYPE Detour_BeginScene(IDirect3DDevice9* dev) {
    GetDX9DX11Bridge().ResolvePendingReadback(dev);
    return g_origBeginScene(dev);
}

HRESULT STDMETHODCALLTYPE Detour_EndScene(IDirect3DDevice9* dev) {
    uint64_t count = ++g_endSceneCalls;
    if (count == 1) {
        LOG_INFO("DX9DX11Bridge: intercepted first IDirect3DDevice9::EndScene (dev=%p)", static_cast<void*>(dev));
        GetDX9DX11Bridge().BindD3D9Device(dev);
    }
    return g_origEndScene(dev);
}

HRESULT STDMETHODCALLTYPE Detour_Present(IDirect3DDevice9* dev,
                                         const RECT* pSourceRect,
                                         const RECT* pDestRect,
                                         HWND hDestWindowOverride,
                                         const RGNDATA* pDirtyRegion) {
    uint64_t count = ++g_presentCalls;
    if (count == 1) {
        LOG_INFO("DX9DX11Bridge: intercepted first IDirect3DDevice9::Present (dev=%p)", static_cast<void*>(dev));
    }

    const auto& cfg = samu::config::Get();
    if (cfg.enableStereo) {
        if (!GetDX9DX11Bridge().IsD3D11Ready()) {
            GetDX9DX11Bridge().EnsureD3D11Resources(1440, 1440);
        }
        uint64_t sampleId = 0;
        int      eye      = -1;
        bool     tagged   = false;
        if (auto provider = g_tagProvider.load(std::memory_order_acquire)) {
            tagged = provider(sampleId, eye);
        }
        if (!tagged) eye = -1;
        const int slot = (eye == 1) ? 1 : 0;
        GetDX9DX11Bridge().CaptureFrameFromD3D9(dev, slot, tagged, sampleId, eye, pSourceRect);
    }

    return g_origPresent(dev, pSourceRect, pDestRect, hDestWindowOverride, pDirtyRegion);
}

HRESULT STDMETHODCALLTYPE Detour_Reset(IDirect3DDevice9* dev,
                                       D3DPRESENT_PARAMETERS* pPresentationParameters) {
    LOG_INFO("DX9DX11Bridge: IDirect3DDevice9::Reset called (dev=%p, %ux%u); releasing D3D9 stereo surfaces.",
             static_cast<void*>(dev),
             pPresentationParameters ? pPresentationParameters->BackBufferWidth : 0,
             pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0);
    ApplyVSyncOverride(pPresentationParameters, "Reset");
    GetDX9DX11Bridge().ReleaseD3D9Resources();
    HRESULT hr = g_origReset(dev, pPresentationParameters);
    if (SUCCEEDED(hr)) {
        GetDX9DX11Bridge().BindD3D9Device(dev);
    }
    return hr;
}

void InstallDeviceHooks(IDirect3DDevice9* dev) {
    if (!dev || g_d3d9DeviceHooked.load(std::memory_order_relaxed)) return;

    void** vtbl = *reinterpret_cast<void***>(dev);
    if (!vtbl) return;

    MH_Initialize();

    // IDirect3DDevice9 vtable indices:
    //   16 = Reset
    //   17 = Present
    //   41 = BeginScene
    //   42 = EndScene
    g_targetBeginScene = vtbl[41];
    g_targetReset    = vtbl[16];
    g_targetPresent  = vtbl[17];
    g_targetEndScene = vtbl[42];

    MH_STATUS s1 = MH_CreateHook(g_targetEndScene, reinterpret_cast<LPVOID>(&Detour_EndScene),
                                 reinterpret_cast<LPVOID*>(&g_origEndScene));
    MH_STATUS s2 = MH_CreateHook(g_targetPresent, reinterpret_cast<LPVOID>(&Detour_Present),
                                 reinterpret_cast<LPVOID*>(&g_origPresent));
    MH_STATUS s3 = MH_CreateHook(g_targetReset, reinterpret_cast<LPVOID>(&Detour_Reset),
                                 reinterpret_cast<LPVOID*>(&g_origReset));

    MH_STATUS s0 = MH_CreateHook(g_targetBeginScene, reinterpret_cast<LPVOID>(&Detour_BeginScene),
                                 reinterpret_cast<LPVOID*>(&g_origBeginScene));
    if (s0 == MH_OK) MH_EnableHook(g_targetBeginScene);
    if (s1 == MH_OK) MH_EnableHook(g_targetEndScene);
    if (s2 == MH_OK) MH_EnableHook(g_targetPresent);
    if (s3 == MH_OK) MH_EnableHook(g_targetReset);

    g_d3d9DeviceHooked.store(true, std::memory_order_release);
    LOG_INFO("DX9DX11Bridge: hooked IDirect3DDevice9 (%p): EndScene=%p (st=%d), Present=%p (st=%d), Reset=%p (st=%d)",
             static_cast<void*>(dev), g_targetEndScene, static_cast<int>(s1),
             g_targetPresent, static_cast<int>(s2), g_targetReset, static_cast<int>(s3));
}

HRESULT STDMETHODCALLTYPE Detour_CreateDevice(IDirect3D9* d3d9,
                                              UINT Adapter,
                                              D3DDEVTYPE DeviceType,
                                              HWND hFocusWindow,
                                              DWORD BehaviorFlags,
                                              D3DPRESENT_PARAMETERS* pPresentationParameters,
                                              IDirect3DDevice9** ppReturnedDeviceInterface) {
    BehaviorFlags |= D3DCREATE_MULTITHREADED;
    ApplyVSyncOverride(pPresentationParameters, "CreateDevice");
    HRESULT hr = g_origCreateDevice(d3d9, Adapter, DeviceType, hFocusWindow,
                                    BehaviorFlags, pPresentationParameters, ppReturnedDeviceInterface);
    LOG_INFO("DX9DX11Bridge: IDirect3D9::CreateDevice(Adapter=%u, Type=%d, HWND=%p, Flags=0x%08lX, %ux%u) -> hr=0x%08lX dev=%p",
             Adapter, static_cast<int>(DeviceType), static_cast<void*>(hFocusWindow), BehaviorFlags,
             pPresentationParameters ? pPresentationParameters->BackBufferWidth : 0,
             pPresentationParameters ? pPresentationParameters->BackBufferHeight : 0,
             static_cast<unsigned long>(hr),
             (SUCCEEDED(hr) && ppReturnedDeviceInterface) ? static_cast<void*>(*ppReturnedDeviceInterface) : nullptr);

    if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
        HWND wnd = (pPresentationParameters && pPresentationParameters->hDeviceWindow)
                       ? pPresentationParameters->hDeviceWindow : hFocusWindow;
        g_gameWindow.store(wnd, std::memory_order_relaxed);
        InstallDeviceHooks(*ppReturnedDeviceInterface);
        GetDX9DX11Bridge().RecordD3D9Device(*ppReturnedDeviceInterface);
    }
    return hr;
}

} // namespace

DX9DX11Bridge& GetDX9DX11Bridge() {
    return g_bridgeInstance;
}

DX9DX11Bridge::~DX9DX11Bridge() {
    Shutdown();
}

void DX9DX11Bridge::OnDirect3DCreate9(IDirect3D9* d3d9) {
    if (!d3d9 || g_d3d9CreateHooked.load(std::memory_order_relaxed)) return;

    void** vtbl = *reinterpret_cast<void***>(d3d9);
    if (!vtbl) return;

    MH_Initialize();

    // IDirect3D9::CreateDevice is at vtable index 16.
    g_targetCreateDevice = vtbl[16];
    MH_STATUS st = MH_CreateHook(g_targetCreateDevice,
                                 reinterpret_cast<LPVOID>(&Detour_CreateDevice),
                                 reinterpret_cast<LPVOID*>(&g_origCreateDevice));
    if (st == MH_OK) {
        st = MH_EnableHook(g_targetCreateDevice);
    }
    if (st == MH_OK) {
        g_d3d9CreateHooked.store(true, std::memory_order_release);
        LOG_INFO("DX9DX11Bridge: hooked IDirect3D9::CreateDevice @ %p (trampoline=%p)",
                 g_targetCreateDevice, reinterpret_cast<void*>(g_origCreateDevice));
    } else {
        LOG_WARN("DX9DX11Bridge: MH_CreateHook/EnableHook(IDirect3D9::CreateDevice @ %p) returned %d",
                 g_targetCreateDevice, static_cast<int>(st));
    }
}

HWND DX9DX11Bridge::GameWindow() {
    return g_gameWindow.load(std::memory_order_relaxed);
}

void DX9DX11Bridge::ResolvePendingReadback(IDirect3DDevice9* d3d9Dev) {
    std::lock_guard lock(m_mutex);
    ResolvePendingReadbackLocked(d3d9Dev);
}

void DX9DX11Bridge::ResolvePendingReadbackLocked(IDirect3DDevice9* d3d9Dev) {
    const int slot = m_pendingSlot;
    if (slot < 0 || slot > 1) return;
    m_pendingSlot = -1;
    if (!d3d9Dev || !m_d3d9SharedSurf[slot] || !m_d3d9SysMemSurf[slot] || !m_d3d11Ctx || !m_d3d11SharedTex[slot]) return;
    if (SUCCEEDED(d3d9Dev->GetRenderTargetData(m_d3d9SharedSurf[slot], m_d3d9SysMemSurf[slot]))) {
        D3DLOCKED_RECT locked{};
        if (SUCCEEDED(m_d3d9SysMemSurf[slot]->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
            m_d3d11Ctx->UpdateSubresource(m_d3d11SharedTex[slot], 0, nullptr,
                                          locked.pBits, static_cast<UINT>(locked.Pitch), 0);
            m_d3d9SysMemSurf[slot]->UnlockRect();
            m_slotInfo[slot] = m_pendingInfo;   // publish only once the pixels are in the D3D11 texture
        }
    }
}

void DX9DX11Bridge::SetFrameTagProvider(FrameTagProvider provider) {
    g_tagProvider.store(provider, std::memory_order_release);
}

void DX9DX11Bridge::BindOpenXRManager(OpenXRManager* xrMgr) {
    g_boundXrManager.store(xrMgr, std::memory_order_release);
}

bool DX9DX11Bridge::InitializeD3D11Resources(ID3D11Device* d3d11Dev,
                                             ID3D11DeviceContext* d3d11Ctx,
                                             uint32_t eyeWidth,
                                             uint32_t eyeHeight) {
    std::lock_guard lock(m_mutex);
    if (!d3d11Dev || !d3d11Ctx || eyeWidth == 0 || eyeHeight == 0) return false;

    ReleaseSharpenPipelineLocked();
    for (int eye = 0; eye < 2; ++eye) {
        SafeRelease(m_d3d11SharedTex[eye]);
        m_sharedHandles[eye] = nullptr;
    }

    m_d3d11Dev  = d3d11Dev;
    m_d3d11Ctx  = d3d11Ctx;
    m_eyeWidth  = eyeWidth;
    m_eyeHeight = eyeHeight;

    D3D11_TEXTURE2D_DESC desc{};
    desc.Width              = eyeWidth;
    desc.Height             = eyeHeight;
    desc.MipLevels          = 1;
    desc.ArraySize          = 1;
    desc.Format             = DXGI_FORMAT_B8G8R8A8_UNORM; // Matches D3DFMT_A8R8G8B8
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.Usage              = D3D11_USAGE_DEFAULT;
    desc.BindFlags          = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags     = 0;
    desc.MiscFlags          = D3D11_RESOURCE_MISC_SHARED;

    for (int eye = 0; eye < 2; ++eye) {
        HRESULT hr = d3d11Dev->CreateTexture2D(&desc, nullptr, &m_d3d11SharedTex[eye]);
        if (FAILED(hr) || !m_d3d11SharedTex[eye]) {
            LOG_ERROR("DX9DX11Bridge: CreateTexture2D(shared eye=%d, %ux%u) failed hr=0x%08lX",
                      eye, eyeWidth, eyeHeight, static_cast<unsigned long>(hr));
            return false;
        }

        IDXGIResource* dxgiRes = nullptr;
        hr = m_d3d11SharedTex[eye]->QueryInterface(__uuidof(IDXGIResource), reinterpret_cast<void**>(&dxgiRes));
        if (SUCCEEDED(hr) && dxgiRes) {
            hr = dxgiRes->GetSharedHandle(&m_sharedHandles[eye]);
            SafeRelease(dxgiRes);
        }
        if (FAILED(hr) || !m_sharedHandles[eye]) {
            LOG_WARN("DX9DX11Bridge: GetSharedHandle(eye=%d) failed hr=0x%08lX", eye, static_cast<unsigned long>(hr));
        }

        // Clear the newly created shared texture to a dark slate color so before D3D9 attaches,
        // OpenXR stereo submission has a clean initialized surface.
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(d3d11Dev->CreateRenderTargetView(m_d3d11SharedTex[eye], nullptr, &rtv)) && rtv) {
            const float clearColor[4] = {0.04f, 0.06f, 0.10f, 1.0f};
            d3d11Ctx->ClearRenderTargetView(rtv, clearColor);
            SafeRelease(rtv);
        }
    }

    LOG_INFO("DX9DX11Bridge: D3D11 shared stereo textures initialized (%ux%u BGRA8, LeftHandle=%p, RightHandle=%p)",
             eyeWidth, eyeHeight, m_sharedHandles[0], m_sharedHandles[1]);
    InitSharpenPipelineLocked();

    // If a D3D9 device was already bound before OpenXR finished initializing, bind its surfaces now.
    if (m_d3d9Dev) {
        IDirect3DDevice9* dev = m_d3d9Dev;
        m_d3d9Dev = nullptr;
        // Unlock is not needed if we call an internal helper, so do it inline:
        for (int eye = 0; eye < 2; ++eye) {
            SafeRelease(m_d3d9SharedSurf[eye]);
            SafeRelease(m_d3d9SharedTex[eye]);
            SafeRelease(m_d3d9SysMemSurf[eye]);
        }
        SafeRelease(m_d3d9EventQuery);
        m_d3d9Dev = dev;
    }
    return true;
}

bool DX9DX11Bridge::EnsureD3D11Resources(uint32_t eyeWidth, uint32_t eyeHeight) {
    if (IsD3D11Ready()) return true;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL got{};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                   levels, ARRAYSIZE(levels), D3D11_SDK_VERSION,
                                   &dev, &got, &ctx);
    if (FAILED(hr) || !dev || !ctx) {
        LOG_ERROR("DX9DX11Bridge: EnsureD3D11Resources D3D11CreateDevice failed hr=0x%08lX",
                  static_cast<unsigned long>(hr));
        SafeRelease(ctx);
        SafeRelease(dev);
        return false;
    }

    SafeRelease(m_ownedD3d11Ctx);
    SafeRelease(m_ownedD3d11Dev);
    m_ownedD3d11Dev = dev;
    m_ownedD3d11Ctx = ctx;

    return InitializeD3D11Resources(m_ownedD3d11Dev, m_ownedD3d11Ctx, eyeWidth, eyeHeight);
}

bool DX9DX11Bridge::BindD3D9Device(IDirect3DDevice9* d3d9Dev) {
    std::lock_guard lock(m_mutex);
    if (!d3d9Dev) return false;
    m_d3d9Dev = d3d9Dev;

    if (!IsD3D11Ready() || m_eyeWidth == 0 || m_eyeHeight == 0) {
        LOG_INFO("DX9DX11Bridge: recorded D3D9 device %p (will bind stereo surfaces once OpenXR swapchain is ready).",
                 static_cast<void*>(d3d9Dev));
        return true;
    }

    if (m_d3d9SharedSurf[0] && m_d3d9SharedSurf[1]) {
        return true;
    }

    for (int eye = 0; eye < 2; ++eye) {
        SafeRelease(m_d3d9SharedSurf[eye]);
        SafeRelease(m_d3d9SharedTex[eye]);
        SafeRelease(m_d3d9SysMemSurf[eye]);
    }
    SafeRelease(m_d3d9EventQuery);

    // 1. Try zero-copy WDDM shared HANDLE binding on D3D9 first (supported on D3D9Ex / D3D9On12).
    bool sharedOk = true;
    for (int eye = 0; eye < 2; ++eye) {
        HANDLE hShared = m_sharedHandles[eye];
        if (!hShared) {
            sharedOk = false;
            break;
        }
        HRESULT hr = d3d9Dev->CreateTexture(m_eyeWidth, m_eyeHeight, 1,
                                            D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                            D3DPOOL_DEFAULT, &m_d3d9SharedTex[eye], &hShared);
        if (FAILED(hr) || !m_d3d9SharedTex[eye]) {
            sharedOk = false;
            break;
        }
        m_d3d9SharedTex[eye]->GetSurfaceLevel(0, &m_d3d9SharedSurf[eye]);
    }

    if (sharedOk && m_d3d9SharedSurf[0] && m_d3d9SharedSurf[1]) {
        m_usingZeroCopyHandles = true;
        d3d9Dev->CreateQuery(D3DQUERYTYPE_EVENT, &m_d3d9EventQuery);
        LOG_INFO("DX9DX11Bridge: bound zero-copy WDDM shared D3D9<->D3D11 eye textures (%ux%u, handles=%p/%p)",
                 m_eyeWidth, m_eyeHeight, m_sharedHandles[0], m_sharedHandles[1]);
        return true;
    }

    // 2. Fallback for legacy non-Ex IDirect3DDevice9: allocate D3DPOOL_DEFAULT render targets
    //    + D3DPOOL_SYSTEMMEM staging surfaces and stream into m_d3d11SharedTex[eye].
    for (int eye = 0; eye < 2; ++eye) {
        SafeRelease(m_d3d9SharedSurf[eye]);
        SafeRelease(m_d3d9SharedTex[eye]);

        HRESULT hr = d3d9Dev->CreateTexture(m_eyeWidth, m_eyeHeight, 1,
                                            D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                                            D3DPOOL_DEFAULT, &m_d3d9SharedTex[eye], nullptr);
        if (FAILED(hr) || !m_d3d9SharedTex[eye]) {
            LOG_ERROR("DX9DX11Bridge: fallback CreateTexture(eye=%d, %ux%u) failed hr=0x%08lX",
                      eye, m_eyeWidth, m_eyeHeight, static_cast<unsigned long>(hr));
            return false;
        }
        m_d3d9SharedTex[eye]->GetSurfaceLevel(0, &m_d3d9SharedSurf[eye]);

        hr = d3d9Dev->CreateOffscreenPlainSurface(m_eyeWidth, m_eyeHeight,
                                                  D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                  &m_d3d9SysMemSurf[eye], nullptr);
        if (FAILED(hr) || !m_d3d9SysMemSurf[eye]) {
            LOG_ERROR("DX9DX11Bridge: CreateOffscreenPlainSurface(eye=%d) failed hr=0x%08lX",
                      eye, static_cast<unsigned long>(hr));
            return false;
        }
    }

    m_usingZeroCopyHandles = false;
    LOG_INFO("DX9DX11Bridge: bound D3D9 stereo surfaces (%ux%u) with system-memory bridge into D3D11 shared textures.",
             m_eyeWidth, m_eyeHeight);
    return true;
}

bool DX9DX11Bridge::CaptureFrameFromD3D9(IDirect3DDevice9* d3d9Dev, int slot, bool tagged, uint64_t sampleId,
                                         int eye, const RECT* pSourceRect) {
    if (!d3d9Dev || !IsD3D11Ready() || slot < 0 || slot > 1) return false;
    if (!m_d3d9SharedSurf[0] || !m_d3d9SharedSurf[1]) {
        if (!BindD3D9Device(d3d9Dev)) return false;
    }

    std::lock_guard lock(m_mutex);
    if (!m_d3d9SharedSurf[slot]) return false;
    // A frame captured earlier but never resolved (no BeginScene in between): finish it first.
    ResolvePendingReadbackLocked(d3d9Dev);
    IDirect3DSurface9* backBuffer = nullptr;
    if (FAILED(d3d9Dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backBuffer)) || !backBuffer) {
        return false;
    }

    D3DSURFACE_DESC bbDesc{};
    backBuffer->GetDesc(&bbDesc);
    const int bbW = static_cast<int>(bbDesc.Width);
    const int bbH = static_cast<int>(bbDesc.Height);

    // UE3 shrinks Viewport/Present pSourceRect without calling Device::Reset whenever the new size fits inside
    // its existing high-water backbuffer. Capture and measure only the presented pSourceRect so we don't copy
    // unrendered pixels or report a mismatched aspect ratio.
    RECT srcRect{0, 0, bbW, bbH};
    bool cropped = false;
    if (pSourceRect &&
        pSourceRect->left >= 0 && pSourceRect->top >= 0 &&
        pSourceRect->right > pSourceRect->left && pSourceRect->bottom > pSourceRect->top &&
        pSourceRect->right <= bbW && pSourceRect->bottom <= bbH &&
        (pSourceRect->right - pSourceRect->left) >= 64 &&
        (pSourceRect->bottom - pSourceRect->top) >= 64) {
        srcRect = *pSourceRect;
        cropped = (srcRect.left != 0 || srcRect.top != 0 || srcRect.right != bbW || srcRect.bottom != bbH);
    }
    const int capW = srcRect.right - srcRect.left;
    const int capH = srcRect.bottom - srcRect.top;

    static int s_lastLogBbW = 0, s_lastLogBbH = 0, s_lastLogCapW = 0, s_lastLogCapH = 0;
    if (bbW != s_lastLogBbW || bbH != s_lastLogBbH || capW != s_lastLogCapW || capH != s_lastLogCapH) {
        s_lastLogBbW = bbW;
        s_lastLogBbH = bbH;
        s_lastLogCapW = capW;
        s_lastLogCapH = capH;
        LOG_INFO("DX9DX11Bridge: capture geometry %dx%d%s (backbuffer %dx%d, pSourceRect=%s)",
                 capW, capH, cropped ? " [cropped]" : "", bbW, bbH,
                 pSourceRect ? "set" : "null");
    }

    m_lastBackBufferW.store(static_cast<uint32_t>(capW), std::memory_order_relaxed);
    m_lastBackBufferH.store(static_cast<uint32_t>(capH), std::memory_order_relaxed);

    // The game renders one full view per frame (mono, or one eye in AFR stereo); capture the presented rect.
    // Avoid bilinear blurring when the game output already matches the OpenXR swapchain 1:1.
    const D3DTEXTUREFILTERTYPE filter =
        (capW == static_cast<int>(m_eyeWidth) && capH == static_cast<int>(m_eyeHeight))
            ? D3DTEXF_NONE
            : D3DTEXF_LINEAR;
    d3d9Dev->StretchRect(backBuffer, cropped ? &srcRect : nullptr, m_d3d9SharedSurf[slot], nullptr, filter);

    SlotInfo info;
    info.sampleId = sampleId;
    info.serial   = ++m_captureSerial;
    info.eye      = eye;
    info.tagged   = tagged;

    if (m_usingZeroCopyHandles) {
        if (m_d3d9EventQuery) {
            m_d3d9EventQuery->Issue(D3DISSUE_END);
            m_d3d9EventQuery->GetData(nullptr, 0, D3DGETDATA_FLUSH);
        }
        m_slotInfo[slot] = info;
    } else {
        // Defer the GPU->CPU copy to the next BeginScene so Present doesn't wait for the GPU to finish.
        m_pendingSlot = slot;
        m_pendingInfo = info;
    }

    // Optional Side-by-Side desktop preview (left slot | right slot) when configured in samu_vr.ini
    if (samu::config::Get().sbsCompanionWindow && bbW >= 2 && bbH >= 2) {
        RECT leftDst{0, 0, bbW / 2, bbH};
        RECT rightDst{bbW / 2, 0, bbW, bbH};
        IDirect3DSurface9* rightSrc = m_slotInfo[1].serial ? m_d3d9SharedSurf[1] : m_d3d9SharedSurf[0];
        d3d9Dev->StretchRect(m_d3d9SharedSurf[0], nullptr, backBuffer, &leftDst, D3DTEXF_LINEAR);
        d3d9Dev->StretchRect(rightSrc, nullptr, backBuffer, &rightDst, D3DTEXF_LINEAR);
    }

    SafeRelease(backBuffer);
    ++g_stereoCaptured;
    return true;
}

void DX9DX11Bridge::RecordD3D9Device(IDirect3DDevice9* d3d9Dev) {
    std::lock_guard lock(m_mutex);
    if (!d3d9Dev) return;
    m_d3d9Dev = d3d9Dev;
    LOG_INFO("DX9DX11Bridge: recorded D3D9 device %p (deferring stereo surface binding to render thread).",
             static_cast<void*>(d3d9Dev));
}

void DX9DX11Bridge::ReleaseSharpenPipelineLocked() {
    m_sharpReady = false;
    SafeRelease(m_sharpCbuf);
    SafeRelease(m_sharpSampler);
    SafeRelease(m_sharpPs);
    SafeRelease(m_sharpVs);
    SafeRelease(m_sharpRtv);
    SafeRelease(m_sharpTex);
    SafeRelease(m_sharedSrv[0]);
    SafeRelease(m_sharedSrv[1]);
}

void DX9DX11Bridge::InitSharpenPipelineLocked() {
    ReleaseSharpenPipelineLocked();
    if (!m_d3d11Dev || m_eyeWidth == 0 || m_eyeHeight == 0) return;

    HMODULE compMod = GetModuleHandleA("d3dcompiler_47.dll");
    if (!compMod) compMod = LoadLibraryA("d3dcompiler_47.dll");
    if (!compMod) {
        LOG_WARN("DX9DX11Bridge: d3dcompiler_47.dll not found; D3D11 CAS sharpening disabled.");
        return;
    }
    auto fnCompile = reinterpret_cast<pD3DCompile>(GetProcAddress(compMod, "D3DCompile"));
    if (!fnCompile) return;

    static const char* kVsSrc = R"(
        struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
        VSOut VS_Main(uint id : SV_VertexID) {
            VSOut o;
            o.uv = float2((id << 1) & 2, id & 2);
            o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
            return o;
        }
    )";

    // Lightweight 5-tap Contrast-Adaptive Sharpen (operates directly on sRGB-encoded BGRA8 bytes in scratch UNORM RT
    // so there is zero double-gamma shift and minimal sky/noise ringing).
    static const char* kPsSrc = R"(
        Texture2D texSrc : register(t0);
        SamplerState sampPt : register(s0);
        cbuffer SharpenCB : register(b0) { float4 Params; }; // x=invW, y=invH, z=sharpness
        float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
        float4 PS_Main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {
            float2 dx = float2(Params.x, 0.0);
            float2 dy = float2(0.0, Params.y);
            float4 c  = texSrc.SampleLevel(sampPt, uv, 0);
            float3 n  = texSrc.SampleLevel(sampPt, uv - dy, 0).rgb;
            float3 s  = texSrc.SampleLevel(sampPt, uv + dy, 0).rgb;
            float3 w  = texSrc.SampleLevel(sampPt, uv - dx, 0).rgb;
            float3 e  = texSrc.SampleLevel(sampPt, uv + dx, 0).rgb;
            float3 mn = min(c.rgb, min(min(n, s), min(w, e)));
            float3 mx = max(c.rgb, max(max(n, s), max(w, e)));
            // Contrast-adaptive weight: suppresses sharpening on extreme hard aliases / flat areas
            float lc  = Luma(c.rgb);
            float4 nb = float4(Luma(n), Luma(s), Luma(w), Luma(e));
            float avg = dot(nb, 0.25);
            float detail = lc - avg;
            float range = max(0.01, max(max(nb.x, nb.y), max(nb.z, nb.w)) - min(min(nb.x, nb.y), min(nb.z, nb.w)));
            float weight = Params.z * saturate(1.25 - range * 1.1) * 1.15;
            float3 sharp = clamp(c.rgb + detail * weight, mn, mx);
            return float4(sharp, c.a);
        }
    )";

    ID3DBlob* vsBlob = nullptr;
    ID3DBlob* psBlob = nullptr;
    ID3DBlob* errBlob = nullptr;
    HRESULT hr = fnCompile(kVsSrc, std::strlen(kVsSrc), nullptr, nullptr, nullptr, "VS_Main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
    if (FAILED(hr) || !vsBlob) {
        SafeRelease(errBlob);
        SafeRelease(vsBlob);
        return;
    }
    SafeRelease(errBlob);
    hr = fnCompile(kPsSrc, std::strlen(kPsSrc), nullptr, nullptr, nullptr, "PS_Main", "ps_4_0", 0, 0, &psBlob, &errBlob);
    if (FAILED(hr) || !psBlob) {
        SafeRelease(errBlob);
        SafeRelease(vsBlob);
        SafeRelease(psBlob);
        return;
    }
    SafeRelease(errBlob);

    hr = m_d3d11Dev->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &m_sharpVs);
    if (SUCCEEDED(hr)) {
        hr = m_d3d11Dev->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &m_sharpPs);
    }
    SafeRelease(vsBlob);
    SafeRelease(psBlob);
    if (FAILED(hr) || !m_sharpVs || !m_sharpPs) { ReleaseSharpenPipelineLocked(); return; }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = m_eyeWidth;
    td.Height = m_eyeHeight;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(m_d3d11Dev->CreateTexture2D(&td, nullptr, &m_sharpTex)) ||
        FAILED(m_d3d11Dev->CreateRenderTargetView(m_sharpTex, nullptr, &m_sharpRtv))) {
        ReleaseSharpenPipelineLocked();
        return;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvd{};
    srvd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    srvd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvd.Texture2D.MipLevels = 1;
    for (int eye = 0; eye < 2; ++eye) {
        if (!m_d3d11SharedTex[eye] || FAILED(m_d3d11Dev->CreateShaderResourceView(m_d3d11SharedTex[eye], &srvd, &m_sharedSrv[eye]))) {
            ReleaseSharpenPipelineLocked();
            return;
        }
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(m_d3d11Dev->CreateSamplerState(&sd, &m_sharpSampler))) {
        ReleaseSharpenPipelineLocked();
        return;
    }

    D3D11_BUFFER_DESC cbd{};
    cbd.ByteWidth = 16;
    cbd.Usage = D3D11_USAGE_DEFAULT;
    cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(m_d3d11Dev->CreateBuffer(&cbd, nullptr, &m_sharpCbuf))) {
        ReleaseSharpenPipelineLocked();
        return;
    }

    m_sharpReady = true;
    LOG_INFO("DX9DX11Bridge: D3D11 Contrast-Adaptive Sharpen (CAS) shader ready (%ux%u).", m_eyeWidth, m_eyeHeight);
}

bool DX9DX11Bridge::CopySharedEyeToD3D11Texture(int eye, ID3D11Texture2D* dstTex, SlotInfo* outInfo) {
    std::lock_guard lock(m_mutex);
    if (eye < 0 || eye >= 2 || !dstTex || !m_d3d11Ctx || !m_d3d11SharedTex[eye]) {
        return false;
    }
    const float sharp = samu::config::GetImageSharpness();
    if (m_sharpReady && sharp > 0.01f && m_sharpTex && m_sharpRtv && m_sharedSrv[eye]) {
        const float params[4] = {
            1.0f / static_cast<float>(std::max(1u, m_eyeWidth)),
            1.0f / static_cast<float>(std::max(1u, m_eyeHeight)),
            sharp, 0.0f
        };
        m_d3d11Ctx->UpdateSubresource(m_sharpCbuf, 0, nullptr, params, 0, 0);

        D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(m_eyeWidth), static_cast<float>(m_eyeHeight), 0.0f, 1.0f};
        m_d3d11Ctx->RSSetViewports(1, &vp);
        m_d3d11Ctx->OMSetRenderTargets(1, &m_sharpRtv, nullptr);
        m_d3d11Ctx->IASetInputLayout(nullptr);
        m_d3d11Ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        m_d3d11Ctx->VSSetShader(m_sharpVs, nullptr, 0);
        m_d3d11Ctx->PSSetShader(m_sharpPs, nullptr, 0);
        m_d3d11Ctx->PSSetConstantBuffers(0, 1, &m_sharpCbuf);
        m_d3d11Ctx->PSSetShaderResources(0, 1, &m_sharedSrv[eye]);
        m_d3d11Ctx->PSSetSamplers(0, 1, &m_sharpSampler);
        m_d3d11Ctx->Draw(3, 0);

        ID3D11ShaderResourceView* nullSrv = nullptr;
        ID3D11RenderTargetView*   nullRtv = nullptr;
        m_d3d11Ctx->PSSetShaderResources(0, 1, &nullSrv);
        m_d3d11Ctx->OMSetRenderTargets(1, &nullRtv, nullptr);
        m_d3d11Ctx->CopyResource(dstTex, m_sharpTex);
    } else {
        m_d3d11Ctx->CopyResource(dstTex, m_d3d11SharedTex[eye]);
    }
    if (outInfo) *outInfo = m_slotInfo[eye];
    return true;
}

void DX9DX11Bridge::ReleaseD3D9Resources() {
    std::lock_guard lock(m_mutex);
    m_pendingSlot = -1;
    for (int eye = 0; eye < 2; ++eye) {
        SafeRelease(m_d3d9SharedSurf[eye]);
        SafeRelease(m_d3d9SharedTex[eye]);
        SafeRelease(m_d3d9SysMemSurf[eye]);
    }
    SafeRelease(m_d3d9EventQuery);
}

void DX9DX11Bridge::Shutdown() {
    ReleaseD3D9Resources();
    std::lock_guard lock(m_mutex);
    ReleaseSharpenPipelineLocked();
    for (int eye = 0; eye < 2; ++eye) {
        SafeRelease(m_d3d11SharedTex[eye]);
        m_sharedHandles[eye] = nullptr;
    }
    m_d3d11Dev = nullptr;
    m_d3d11Ctx = nullptr;
    m_d3d9Dev  = nullptr;
    SafeRelease(m_ownedD3d11Ctx);
    SafeRelease(m_ownedD3d11Dev);
}

uint64_t DX9DX11Bridge::TotalEndSceneCalls() {
    return g_endSceneCalls.load(std::memory_order_relaxed);
}

uint64_t DX9DX11Bridge::TotalPresentCalls() {
    return g_presentCalls.load(std::memory_order_relaxed);
}

uint64_t DX9DX11Bridge::TotalStereoFramesCaptured() {
    return g_stereoCaptured.load(std::memory_order_relaxed);
}

} // namespace vr
