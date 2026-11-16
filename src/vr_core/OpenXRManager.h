#pragma once
// src/vr_core/OpenXRManager.h
// Standalone OpenXR integration + Stereo Swapchain & Asymmetric Frustum Projection (Phase 3).
// MUST NOT depend on anything in ue3_bridge/.

#include "vr_core/XrMath.h"
#include "vr_core/DX9DX11Bridge.h"
#include "vr_core/VrSettingsMenu.h"

#include <windows.h>
#include <d3d11.h>
#include <openxr/openxr.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>

namespace vr {

struct TrackedPose {
    XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
    bool positionValid      = false;
    bool orientationValid   = false;
    bool positionTracked    = false;
    bool orientationTracked = false;
};

struct EyeViewInfo {
    XrPosef    pose{{0, 0, 0, 1}, {0, 0, 0}};
    XrFovf     fov{-0.85f, 0.85f, 0.85f, -0.85f};
    math::Mat4 projMatrix = math::Mat4::Identity();
    uint32_t   width      = 0;
    uint32_t   height     = 0;
};

// Phase 4: OpenXR controller action states (polled every frame after xrSyncActions)
struct ControllerInputState {
    XrVector2f leftStick{0.0f, 0.0f};      // Left Thumbstick (X=strafe, Y=forward locomotion)
    XrVector2f rightStick{0.0f, 0.0f};     // Right Thumbstick (X=turn, Y=pitch/look)
    float      rightTrigger        = 0.0f; // Right Trigger [0..1] (fire grapple)
    float      leftTrigger         = 0.0f; // Left Trigger [0..1] (jet boots / jump)
    float      rightGrip           = 0.0f; // Right Grip [0..1] (power jump charge)
    float      leftGrip            = 0.0f; // Left Grip [0..1] (sprint / interact)
    bool       rightTriggerPressed = false;
    bool       leftTriggerPressed  = false;
    bool       rightGripPressed    = false;
    bool       leftGripPressed     = false;
    bool       jumpButtonPressed   = false;
    bool       menuButtonPressed   = false; // Left menu / B buttons (pause, back)
    bool       vrMenuButtonPressed = false; // Right thumbstick click (R3) -> in-headset VR Settings Menu
    bool       leftThumbstickClickPressed = false; // Left thumbstick click (L3) -> sprint when pushing forward
    bool       active              = false;
};

struct TrackingState {
    XrTime      displayTime = 0;
    uint64_t    sampleId    = 0;   // Monotonic id of this pose sample (render-pose matching on submit)
    TrackedPose hmd;
    TrackedPose leftAim, leftGrip;
    TrackedPose rightAim, rightGrip;

    // Phase 3: Stereo eye views + asymmetric frustum projection matrices
    EyeViewInfo views[2]; // 0 = Left Eye, 1 = Right Eye
    bool        viewsValid = false;
    float       ipdMeters  = 0.063f;

    // Phase 4: Controller input action states
    ControllerInputState input;
};

class OpenXRManager {
public:
    OpenXRManager() = default;
    ~OpenXRManager();
    OpenXRManager(const OpenXRManager&) = delete;
    OpenXRManager& operator=(const OpenXRManager&) = delete;

    // Creates instance, system, D3D11 device, session, reference spaces,
    // pose actions, stereo swapchains, and DX9-to-DX11 shared texture bridge.
    bool Initialize();
    void Shutdown();

    // Drains xrPollEvent and handles session state transitions.
    bool PollEvents();

    // xrWaitFrame/xrBeginFrame -> sync actions + locate spaces & stereo views
    // -> copy DX9-to-DX11 shared eye textures into swapchains -> xrEndFrame(projectionLayer).
    bool RunFrame();

    bool IsInitialized() const { return m_instance != XR_NULL_HANDLE && m_session != XR_NULL_HANDLE; }
    bool IsSessionRunning() const { return m_sessionRunning; }
    XrSessionState SessionState() const { return m_state; }
    TrackingState GetTrackingState() const;

    // Accessors for stereo rendering & DX9-to-DX11 bridge.
    XrInstance Instance() const { return m_instance; }
    XrSession Session() const { return m_session; }
    ID3D11Device* D3D11Device() const { return m_d3d11Device; }
    ID3D11DeviceContext* D3D11Context() const { return m_d3d11Context; }
    uint64_t SubmittedStereoFrames() const { return m_submittedStereoFrames.load(std::memory_order_relaxed); }
    uint64_t PoseMatchedFrames() const { return m_poseMatchedFrames; }   // mod thread only
    uint64_t AfrStereoFrames() const { return m_afrStereoFrames; }       // mod thread only

    // Display mode for the captured (mono) game frame:
    //  - immersive=false: flat virtual cinema screen (XrCompositionLayerQuad) floating in front of the user
    //    (menus, intro/legal screens, loading).
    //  - immersive=true : full-view projection layer with zero disparity whose FOV matches the FOV the game
    //    rendered with (gameHorizontalFovDeg) and the backbuffer aspect, so scale is correct and not doubled.
    void SetDisplayMode(bool immersive, float gameHorizontalFovDeg);
    bool IsImmersiveDisplay() const { return m_immersive.load(std::memory_order_relaxed); }
    // Shows a world-space aiming reticle along the right controller's aim ray (immersive mode only).
    // distanceM: how far along the ray the aim point is. lit: the game reports that point as grapple-able.
    void SetHandReticle(bool visible, float distanceM, bool lit) {
        m_handReticleVisible.store(visible, std::memory_order_relaxed);
        m_handReticleDistM.store(distanceM, std::memory_order_relaxed);
        m_handReticleLit.store(lit, std::memory_order_relaxed);
    }
    // The game's measured projection (tan of the half FOVs). When set (> 0) it replaces the FOV/aspect estimate.
    void SetMeasuredProjection(float tanHalfH, float tanHalfV) {
        m_measTanH.store(tanHalfH, std::memory_order_relaxed);
        m_measTanV.store(tanHalfV, std::memory_order_relaxed);
    }
    // Re-places the flat virtual screen in front of the current head pose on the next frame.
    void RequestRecenterVirtualScreen() { m_quadNeedsPlacement.store(true, std::memory_order_relaxed); }

    // Intersects a controller aim ray with the flat virtual screen. u,v in [0,1] (0,0 = top-left).
    // Mod thread only (reads the quad placement written by RunFrame).
    bool RayHitFlatScreen(const XrPosef& aimPose, float& u, float& v) const;

    // In-headset VR Settings Menu overlay (XrCompositionLayerQuad).
    void SetVrMenuOpen(bool open);
    void ToggleVrMenu() { SetVrMenuOpen(!IsVrMenuOpen()); }
    bool IsVrMenuOpen() const { return m_vrMenuOpen.load(std::memory_order_relaxed); }
    void RequestRecenterVrMenu() { m_vrMenuNeedsPlacement.store(true, std::memory_order_relaxed); }
    VrMenuUpdateResult UpdateVrMenu(const TrackingState& s);

    static const char* SessionStateName(XrSessionState s);

private:
    static bool RayHitQuad(const XrPosef& aimPose, const XrPosef& quadPose,
                           float widthM, float heightM, float& u, float& v);
    bool CreateInstance();
    bool CreateD3D11Device();
    bool CreateSession();
    bool CreateSwapchains();
    bool CreateReticleSwapchain(XrSwapchain& outSwapchain, bool lit);
    bool CreateVrMenuSwapchain();
    bool CreateActions();
    bool CreateSpaces();
    void HandleStateChange(const XrEventDataSessionStateChanged& ev);
    void LocateAll(XrTime time);
    void Locate(XrSpace space, XrTime time, TrackedPose& out);
    // Copies captured image slot `slot` (0/1) into swapchain `swapIdx` (0/1).
    bool AcquireAndBlitEyeSwapchain(int swapIdx, int slot, DX9DX11Bridge::SlotInfo* copied = nullptr);

    // Render-pose history: the exact head / eye poses handed to the game, keyed by sample id.
    struct PoseSample {
        uint64_t id = 0;
        XrPosef  head{{0, 0, 0, 1}, {0, 0, 0}};
        XrPosef  eye[2]{{{0, 0, 0, 1}, {0, 0, 0}}, {{0, 0, 0, 1}, {0, 0, 0}}};
        bool     headValid = false;
        bool     eyesValid = false;
    };
    static constexpr size_t kPoseHistory = 256;
    bool LookupPoseSample(uint64_t id, PoseSample& out) const;
    bool Check(XrResult r, const char* what) const;

    XrInstance m_instance       = XR_NULL_HANDLE;
    XrSystemId m_systemId       = XR_NULL_SYSTEM_ID;
    XrSession  m_session        = XR_NULL_HANDLE;
    XrSessionState m_state      = XR_SESSION_STATE_UNKNOWN;
    bool       m_sessionRunning = false;

    XrSpace m_baseSpace = XR_NULL_HANDLE;   // LOCAL reference space
    XrSpace m_viewSpace = XR_NULL_HANDLE;   // VIEW (HMD) space

    XrActionSet m_actionSet          = XR_NULL_HANDLE;
    XrAction    m_aimPoseAction      = XR_NULL_HANDLE;
    XrAction    m_gripPoseAction     = XR_NULL_HANDLE;
    XrAction    m_thumbstickAction   = XR_NULL_HANDLE;
    XrAction    m_triggerValueAction = XR_NULL_HANDLE;
    XrAction    m_squeezeValueAction = XR_NULL_HANDLE;
    XrAction    m_jumpButtonAction   = XR_NULL_HANDLE;
    XrAction    m_menuButtonAction   = XR_NULL_HANDLE;
    XrAction    m_vrMenuButtonAction = XR_NULL_HANDLE;
    XrAction    m_sprintClickAction  = XR_NULL_HANDLE;
    XrPath      m_handPaths[2]       = {XR_NULL_PATH, XR_NULL_PATH};
    XrSpace     m_aimSpaces[2]       = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    XrSpace     m_gripSpaces[2]      = {XR_NULL_HANDLE, XR_NULL_HANDLE};

    // Stereo view configurations & D3D11 swapchains (0 = Left, 1 = Right)
    XrViewConfigurationView m_viewConfigs[2]{};
    XrSwapchain             m_swapchains[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    int64_t                 m_swapchainFormat = 0;
    std::vector<ID3D11Texture2D*> m_swapchainImages[2];

    ID3D11Device*        m_d3d11Device  = nullptr;
    ID3D11DeviceContext* m_d3d11Context = nullptr;

    mutable std::mutex    m_stateMutex;
    TrackingState         m_tracking;
    PoseSample            m_poseHistory[kPoseHistory]{};
    uint64_t              m_sampleCounter = 0;
    uint64_t              m_poseMatchedFrames = 0;
    uint64_t              m_afrStereoFrames   = 0;
    uint64_t              m_frameCount = 0;
    std::atomic<uint64_t> m_submittedStereoFrames{0};

    std::atomic<bool>     m_immersive{false};
    std::atomic<float>    m_gameFovDeg{100.0f};
    std::atomic<float>    m_measTanH{0.0f};
    std::atomic<float>    m_measTanV{0.0f};
    std::atomic<bool>     m_quadNeedsPlacement{true};
    bool                  m_lastSubmittedImmersive = false;
    bool                  m_prevImmersiveTarget    = false;
    float                 m_displayBlend           = 0.0f;   // 0 = flat menu quad, 1 = full immersive projection
    float                 m_quadCurWidthM          = 3.2f;
    XrPosef               m_quadPose{{0, 0, 0, 1}, {0.0f, 0.0f, -2.5f}};
    XrPosef               m_quadStartPose{{0, 0, 0, 1}, {0.0f, 0.0f, -2.5f}};
    XrPosef               m_quadTargetPose{{0, 0, 0, 1}, {0.0f, 0.0f, -2.5f}};
    std::chrono::steady_clock::time_point m_lastBlendTime{};
    XrSwapchain           m_reticleSwapchain    = XR_NULL_HANDLE;   // pale ring: nothing to grapple there
    XrSwapchain           m_reticleLitSwapchain = XR_NULL_HANDLE;   // bright ring: grapple-able
    std::atomic<bool>     m_handReticleVisible{false};
    std::atomic<bool>     m_handReticleLit{false};
    std::atomic<float>    m_handReticleDistM{30.0f};
    float                 m_reticleDistSmooth = 30.0f;              // frame thread only

    // In-headset VR Settings Menu quad layer
    XrSwapchain                   m_vrMenuSwapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> m_vrMenuImages;
    std::vector<uint8_t>          m_vrMenuPixels;
    mutable std::mutex            m_vrMenuMutex;
    VrSettingsMenu                m_vrMenu;
    std::atomic<bool>             m_vrMenuOpen{false};
    std::atomic<bool>             m_vrMenuNeedsPlacement{true};
    XrPosef                       m_vrMenuPose{{0, 0, 0, 1}, {0.0f, 0.0f, -1.45f}};
};

} // namespace vr
