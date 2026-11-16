#pragma once
// src/vr_core/DX9DX11Bridge.h
// DirectX 9 to DirectX 11 shared-texture stereo bridge + IDirect3DDevice9 hooks
// (EndScene, Present, Reset) for OpenXR (XR_KHR_D3D11_enable) stereo frame submission.
// Strictly decoupled from src/ue3_bridge/.

#include "vr_core/XrMath.h"

#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace vr {

class OpenXRManager;

class DX9DX11Bridge {
public:
    // Supplies, at Present time, which HMD pose sample (and eye: -1 mono, 0 left, 1 right) the frame being
    // presented was rendered with. Installed by the proxy layer (keeps vr_core decoupled from ue3_bridge).
    using FrameTagProvider = bool (*)(uint64_t& sampleId, int& eye);
    static void SetFrameTagProvider(FrameTagProvider provider);

    // What a captured image slot currently holds.
    struct SlotInfo {
        uint64_t sampleId = 0;     // pose sample the image was rendered with (valid if tagged)
        uint64_t serial   = 0;     // global capture counter value when written (0 = never)
        int      eye      = -1;    // -1 mono, 0 left, 1 right
        bool     tagged   = false;
    };
    SlotInfo GetSlotInfo(int slot) const {
        std::lock_guard lock(m_mutex);
        return (slot >= 0 && slot < 2) ? m_slotInfo[slot] : SlotInfo{};
    }
    DX9DX11Bridge() = default;
    ~DX9DX11Bridge();
    DX9DX11Bridge(const DX9DX11Bridge&) = delete;
    DX9DX11Bridge& operator=(const DX9DX11Bridge&) = delete;

    // Hooks IDirect3D9::CreateDevice on a newly created IDirect3D9 instance so we
    // automatically intercept IDirect3DDevice9::EndScene, Present, and Reset.
    static void OnDirect3DCreate9(IDirect3D9* d3d9);

    // Binds the active OpenXRManager instance that receives stereo eye textures.
    static void BindOpenXRManager(OpenXRManager* xrMgr);

    // Initializes the D3D11 shared textures (and WDDM shared HANDLEs) on the OpenXR D3D11 device.
    // Can be called even before a D3D9 device exists (creates the D3D11 shared textures + handles immediately).
    bool InitializeD3D11Resources(ID3D11Device* d3d11Dev, ID3D11DeviceContext* d3d11Ctx,
                                  uint32_t eyeWidth, uint32_t eyeHeight);

    // Ensures a hardware D3D11 device + shared stereo textures exist even before OpenXR connects,
    // so IDirect3DDevice9::EndScene / Present can immediately bind and capture stereo frames.
    bool EnsureD3D11Resources(uint32_t eyeWidth = 1440, uint32_t eyeHeight = 1600);

    // Records the newly created IDirect3DDevice9* without allocating render-thread surfaces yet.
    void RecordD3D9Device(IDirect3DDevice9* d3d9Dev);

    // Binds or re-binds the D3D9 side of the shared textures on the render thread.
    bool BindD3D9Device(IDirect3DDevice9* d3d9Dev);

    // Thread-safe copy from the shared eye texture to an acquired OpenXR D3D11 swapchain texture.
    // outInfo (optional) receives the slot's info captured atomically with the copy.
    bool CopySharedEyeToD3D11Texture(int eye, ID3D11Texture2D* dstTex, SlotInfo* outInfo = nullptr);

    // Releases D3D9-side resources on Device::Reset.
    void ReleaseD3D9Resources();
    void Shutdown();

    // Captures the active D3D9 backbuffer region (or pSourceRect when smaller than the backbuffer) into
    // image slot `slot` (0 = mono/left, 1 = right in AFR) and records which pose sample / eye rendered it.
    bool CaptureFrameFromD3D9(IDirect3DDevice9* d3d9Dev, int slot, bool tagged, uint64_t sampleId, int eye,
                              const RECT* pSourceRect = nullptr);

    // Sysmem fallback: the GPU->CPU readback of the frame captured at Present is deferred until the next
    // BeginScene (or Present), when the GPU has normally finished it, instead of stalling Present on it.
    void ResolvePendingReadback(IDirect3DDevice9* d3d9Dev);

    // Game window (from IDirect3D9::CreateDevice) for the VR menu pointer.
    static HWND GameWindow();

    ID3D11Texture2D* GetEyeTextureD3D11(int eye) const {
        return (eye >= 0 && eye < 2) ? m_d3d11SharedTex[eye] : nullptr;
    }
    HANDLE GetEyeSharedHandle(int eye) const {
        return (eye >= 0 && eye < 2) ? m_sharedHandles[eye] : nullptr;
    }
    uint32_t EyeWidth() const { return m_eyeWidth; }
    uint32_t EyeHeight() const { return m_eyeHeight; }
    bool IsD3D11Ready() const { return m_d3d11SharedTex[0] != nullptr && m_d3d11SharedTex[1] != nullptr; }
    bool HasD3D9Device() const {
        std::lock_guard lock(m_mutex);
        return m_d3d9Dev != nullptr;
    }
    bool IsZeroCopyWddmShared() const { return m_usingZeroCopyHandles; }

    uint32_t LastBackBufferWidth() const { return m_lastBackBufferW.load(std::memory_order_relaxed); }
    uint32_t LastBackBufferHeight() const { return m_lastBackBufferH.load(std::memory_order_relaxed); }

    // Aspect ratio (height / width) of the most recently captured game backbuffer (default 9:16).
    float LastBackBufferAspectHOverW() const {
        const uint32_t w = m_lastBackBufferW.load(std::memory_order_relaxed);
        const uint32_t h = m_lastBackBufferH.load(std::memory_order_relaxed);
        return (w > 0 && h > 0) ? static_cast<float>(h) / static_cast<float>(w) : 9.0f / 16.0f;
    }

    static uint64_t TotalEndSceneCalls();
    static uint64_t TotalPresentCalls();
    static uint64_t TotalStereoFramesCaptured();

private:
    ID3D11Device*        m_ownedD3d11Dev        = nullptr;
    ID3D11DeviceContext* m_ownedD3d11Ctx        = nullptr;
    ID3D11Device*        m_d3d11Dev             = nullptr;
    ID3D11DeviceContext* m_d3d11Ctx             = nullptr;
    ID3D11Texture2D*     m_d3d11SharedTex[2]    = {nullptr, nullptr};
    ID3D11ShaderResourceView* m_sharedSrv[2]    = {nullptr, nullptr};
    ID3D11Texture2D*     m_sharpTex             = nullptr;
    ID3D11RenderTargetView* m_sharpRtv          = nullptr;
    ID3D11VertexShader*  m_sharpVs              = nullptr;
    ID3D11PixelShader*   m_sharpPs              = nullptr;
    ID3D11SamplerState*  m_sharpSampler         = nullptr;
    ID3D11Buffer*        m_sharpCbuf            = nullptr;
    bool                 m_sharpReady           = false;
    void                 InitSharpenPipelineLocked();
    void                 ReleaseSharpenPipelineLocked();
    HANDLE               m_sharedHandles[2]     = {nullptr, nullptr};

    IDirect3DDevice9*    m_d3d9Dev              = nullptr;
    IDirect3DTexture9*   m_d3d9SharedTex[2]     = {nullptr, nullptr};
    IDirect3DSurface9*   m_d3d9SharedSurf[2]    = {nullptr, nullptr};
    IDirect3DSurface9*   m_d3d9SysMemSurf[2]    = {nullptr, nullptr};
    IDirect3DQuery9*     m_d3d9EventQuery       = nullptr;

    SlotInfo             m_slotInfo[2]{};
    SlotInfo             m_pendingInfo{};
    int                  m_pendingSlot          = -1;
    void                 ResolvePendingReadbackLocked(IDirect3DDevice9* d3d9Dev);
    uint64_t             m_captureSerial        = 0;
    uint32_t             m_eyeWidth             = 0;
    uint32_t             m_eyeHeight            = 0;
    bool                 m_usingZeroCopyHandles = false;
    std::atomic<uint32_t> m_lastBackBufferW{0};
    std::atomic<uint32_t> m_lastBackBufferH{0};
    mutable std::mutex   m_mutex;
};

// Global singleton accessor used by D3D9 hooks and OpenXRManager.
DX9DX11Bridge& GetDX9DX11Bridge();

} // namespace vr
