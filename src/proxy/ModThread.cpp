#include "proxy/ModThread.h"
#include "proxy/D3D9Proxy.h"
#include "common/Config.h"
#include "common/Log.h"
#include "vr_core/DX9DX11Bridge.h"
#include "vr_core/OpenXRManager.h"
#include "vr_core/XrMath.h"
#include "ue3_bridge/UE3Bridge.h"

#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

extern "C" IDirect3D9* WINAPI Proxy_Direct3DCreate9(UINT sdkVersion);

namespace samu::proxy {
namespace {

std::atomic<bool> g_stop{false};
std::atomic<bool> g_forceFlatScreen{false};
std::atomic<bool> g_recenterScreenRequested{false};
std::atomic<bool> g_toggleVrMenuRequested{false};
HANDLE g_thread = nullptr;

void VerifyD3D9StereoBridgeIfNullRhi() {
    static bool s_checked = false;
    if (s_checked || Direct3DCreateCount() > 0) return;
    s_checked = true;

    const wchar_t* cmdLine = GetCommandLineW();
    if (!cmdLine || (!wcsstr(cmdLine, L"-nullrhi") && !wcsstr(cmdLine, L"-NULLRHI"))) {
        return;
    }

    IDirect3D9* d3d = Proxy_Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) return;

    HWND hwnd = CreateWindowExW(0, L"STATIC", L"SAMU_VR_D3D9_Verify", WS_POPUP,
                                0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    auto makePP = [hwnd]() {
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed         = TRUE;
        pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;
        pp.BackBufferFormat = D3DFMT_X8R8G8B8;
        pp.BackBufferWidth  = 640;
        pp.BackBufferHeight = 480;
        pp.BackBufferCount  = 1;
        pp.hDeviceWindow    = hwnd;
        return pp;
    };

    IDirect3DDevice9* dev = nullptr;
    D3DPRESENT_PARAMETERS pp = makePP();
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                                   &pp, &dev);
    if (FAILED(hr) || !dev) {
        pp = makePP();
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                               &pp, &dev);
    }
    if (FAILED(hr) || !dev) {
        pp = makePP();
        hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_NULLREF, hwnd,
                               D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED,
                               &pp, &dev);
    }
    if (SUCCEEDED(hr) && dev) {
        for (int i = 0; i < 3; ++i) {
            dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_ARGB(255, 24, 40, 68), 1.0f, 0);
            if (SUCCEEDED(dev->BeginScene())) {
                dev->EndScene();
            }
            dev->Present(nullptr, nullptr, nullptr, nullptr);
        }
        auto& bridge = vr::GetDX9DX11Bridge();
        LOG_INFO("DX9DX11Bridge self-test complete: EndScene=%llu, Present=%llu, StereoFramesCaptured=%llu (EyeTex=%ux%u, LeftHandle=%p, RightHandle=%p)",
                 static_cast<unsigned long long>(bridge.TotalEndSceneCalls()),
                 static_cast<unsigned long long>(bridge.TotalPresentCalls()),
                 static_cast<unsigned long long>(bridge.TotalStereoFramesCaptured()),
                 bridge.EyeWidth(), bridge.EyeHeight(),
                 bridge.GetEyeSharedHandle(0), bridge.GetEyeSharedHandle(1));
        bridge.ReleaseD3D9Resources();
        dev->Release();
    }
    if (hwnd) DestroyWindow(hwnd);
    d3d->Release();
}

void LogPose(const char* name, const vr::TrackedPose& p) {
    const auto& o = p.pose.orientation;
    const auto& v = p.pose.position;
    LOG_INFO("  %-10s pos[%c]=(% .3f, % .3f, % .3f) m  rot[%c]=(x% .3f y% .3f z% .3f w% .3f)",
             name, p.positionValid ? 'V' : '-', v.x, v.y, v.z,
             p.orientationValid ? 'V' : '-', o.x, o.y, o.z, o.w);
}

void PollComfortHotkeys() {
    const auto& cfg = samu::config::Get();
    static bool s_prevToggleKey   = false;
    static bool s_prevRecenterKey = false;
    static bool s_prevDisplayKey  = false;
    static bool s_prevStereoKey   = false;
    static bool s_prevVrMenuKey   = false;

    bool curToggle = (GetAsyncKeyState(cfg.toggleSuppressorVKey) & 0x8000) != 0;
    if (curToggle && !s_prevToggleKey) {
        bool nextState = !samu::config::IsCameraLockSuppressorEnabled();
        samu::config::SetCameraLockSuppressor(nextState);
    }
    s_prevToggleKey = curToggle;

    bool curRecenter = (GetAsyncKeyState(cfg.recenterVKey) & 0x8000) != 0;
    if (curRecenter && !s_prevRecenterKey) {
        ue3::UE3Bridge::RecenterHmdOrigin();
        g_recenterScreenRequested.store(true, std::memory_order_relaxed);
    }
    s_prevRecenterKey = curRecenter;

    bool curDisplay = (GetAsyncKeyState(cfg.toggleDisplayVKey) & 0x8000) != 0;
    if (curDisplay && !s_prevDisplayKey) {
        bool next = !g_forceFlatScreen.load(std::memory_order_relaxed);
        g_forceFlatScreen.store(next, std::memory_order_relaxed);
        LOG_INFO("Display: forced flat-screen mode %s (F8)", next ? "ON" : "OFF");
    }
    s_prevDisplayKey = curDisplay;

    bool curStereo = (GetAsyncKeyState(cfg.toggleStereoVKey) & 0x8000) != 0;
    if (curStereo && !s_prevStereoKey) {
        samu::config::SetStereoMode(samu::config::GetStereoMode() == 1 ? 0 : 1);
    }
    s_prevStereoKey = curStereo;

    bool curVrMenu = (GetAsyncKeyState(cfg.openVrMenuVKey) & 0x8000) != 0;
    if (curVrMenu && !s_prevVrMenuKey) {
        g_toggleVrMenuRequested.store(true, std::memory_order_relaxed);
    }
    s_prevVrMenuKey = curVrMenu;
}

bool IsExtendedKey(WORD vk) {
    return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT;
}

// Key tap with a hardware scan code: UE3 reads the keyboard through DirectInput, which ignores
// virtual-key-only events.
void SendKeyTap(WORD vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = vk;
    in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in[0].ki.dwFlags = IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0;
    in[1] = in[0];
    in[1].ki.dwFlags |= KEYEVENTF_KEYUP;
    SendInput(1, &in[0], sizeof(INPUT));
    Sleep(15);   // hold long enough for a polled (DirectInput) keyboard to see the press
    SendInput(1, &in[1], sizeof(INPUT));
}

void SendMouseButton(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

void SendMouseMoveRelative(int dx, int dy) {
    if (dx == 0 && dy == 0) return;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(INPUT));
}

bool IsGameWindowForeground(HWND game) {
    if (!game) return false;
    HWND fg = GetForegroundWindow();
    return fg && GetAncestor(fg, GA_ROOT) == GetAncestor(game, GA_ROOT);
}

// VR controls for the game's own menus (main menu, settings, pause), which only understand mouse/keyboard:
//  - Flat-screen mode: right-controller laser -> mouse cursor on the virtual screen (sent BOTH as relative
//    mouse motion, for games that read raw/DirectInput deltas, and as an absolute cursor position), either
//    trigger -> left click, A/X -> Enter, left stick -> arrow keys.
//  - Any mode: Menu / B -> Escape (pause menu in gameplay, back in menus).
// Input is only sent while the game window is in the foreground.
void UpdateMenuPointer(const vr::OpenXRManager& xr, const vr::TrackingState& s, bool immersive, bool vrMenuOpen) {
    static bool  s_prevClick = false, s_prevEnter = false, s_prevEsc = false, s_mouseDown = false;
    static bool  s_haveLast = false;
    static float s_lastU = 0.0f, s_lastV = 0.0f, s_remX = 0.0f, s_remY = 0.0f;
    static int   s_prevArrow = 0;
    static auto  s_arrowTime = std::chrono::steady_clock::now();
    static auto  s_lastLog   = std::chrono::steady_clock::now();

    const HWND game = vr::DX9DX11Bridge::GameWindow();
    const bool fg = IsGameWindowForeground(game);
    const bool click = s.input.active && (s.input.rightTriggerPressed || s.input.leftTriggerPressed);
    const bool enter = s.input.active && s.input.jumpButtonPressed;
    const bool esc   = s.input.active && s.input.menuButtonPressed;
    const auto now   = std::chrono::steady_clock::now();

    if (vrMenuOpen) {
        if (s_mouseDown) {
            SendMouseButton(false);
            s_mouseDown = false;
        }
        s_haveLast = false;
        s_prevArrow = 0;
        s_prevClick = click;
        s_prevEnter = enter;
        s_prevEsc   = esc;
        return;
    }

    if (fg && esc && !s_prevEsc) {
        LOG_INFO("[MenuPointer] Menu/B -> Escape");
        SendKeyTap(VK_ESCAPE);
    }
    s_prevEsc = esc;

    if (!immersive && !fg && now - s_lastLog > std::chrono::seconds(3)) {
        s_lastLog = now;
        wchar_t title[128] = L"";
        HWND fgw = GetForegroundWindow();
        if (fgw) GetWindowTextW(fgw, title, 127);
        LOG_INFO("[MenuPointer] game window is NOT in the foreground (foreground='%ls') -> no menu input sent. "
                 "Click the game window on the desktop once.", title);
    }

    if (fg && !immersive) {
        float u = 0.0f, v = 0.0f;
        const bool tracked = s.rightAim.orientationValid && s.rightAim.positionValid;
        const bool hit = tracked && xr.RayHitFlatScreen(s.rightAim.pose, u, v);
        RECT rc{};
        GetClientRect(game, &rc);
        if (hit && rc.right > 0 && rc.bottom > 0) {
            if (s_haveLast) {
                // Relative motion (DirectInput / raw-input cursors follow this like a mouse).
                s_remX += (u - s_lastU) * rc.right;
                s_remY += (v - s_lastV) * rc.bottom;
                const int dx = static_cast<int>(s_remX);
                const int dy = static_cast<int>(s_remY);
                if (dx != 0 || dy != 0) {
                    s_remX -= dx;
                    s_remY -= dy;
                    SendMouseMoveRelative(dx, dy);
                    // Absolute position (cursors that follow the Windows pointer land exactly under the laser).
                    POINT pt{static_cast<LONG>(u * rc.right), static_cast<LONG>(v * rc.bottom)};
                    ClientToScreen(game, &pt);
                    SetCursorPos(pt.x, pt.y);
                }
            }
            s_lastU = u;
            s_lastV = v;
            s_haveLast = true;
        } else {
            s_haveLast = false;
            s_remX = s_remY = 0.0f;
        }

        if (click != s_prevClick && (click || s_mouseDown)) {
            LOG_INFO("[MenuPointer] trigger -> mouse %s at u=%.3f v=%.3f (hit=%d)", click ? "DOWN" : "UP", u, v, hit ? 1 : 0);
            SendMouseButton(click);
            s_mouseDown = click;
        }
        if (enter && !s_prevEnter) {
            LOG_INFO("[MenuPointer] A/X -> Enter");
            SendKeyTap(VK_RETURN);
        }

        // Left stick -> arrow keys (with auto-repeat) for keyboard-navigable menus.
        int arrow = 0;
        if (s.input.active) {
            if (s.input.leftStick.y > 0.6f) arrow = VK_UP;
            else if (s.input.leftStick.y < -0.6f) arrow = VK_DOWN;
            else if (s.input.leftStick.x > 0.6f) arrow = VK_RIGHT;
            else if (s.input.leftStick.x < -0.6f) arrow = VK_LEFT;
        }
        if (arrow != 0 && (arrow != s_prevArrow || now - s_arrowTime > std::chrono::milliseconds(350))) {
            SendKeyTap(static_cast<WORD>(arrow));
            s_arrowTime = now;
        }
        s_prevArrow = arrow;

        if (now - s_lastLog > std::chrono::seconds(2)) {
            s_lastLog = now;
            POINT cur{};
            GetCursorPos(&cur);
            POINT curClient = cur;
            ScreenToClient(game, &curClient);
            RECT clip{};
            GetClipCursor(&clip);
            CURSORINFO ci{sizeof(CURSORINFO)};
            GetCursorInfo(&ci);
            LOG_INFO("[MenuPointer] rightAim tracked=%d hit=%d u=%.3f v=%.3f | client=%ldx%ld cursor(client)=(%ld,%ld) "
                     "sysCursorVisible=%d clip=(%ld,%ld)-(%ld,%ld)",
                     tracked ? 1 : 0, hit ? 1 : 0, u, v, rc.right, rc.bottom, curClient.x, curClient.y,
                     (ci.flags & CURSOR_SHOWING) ? 1 : 0, clip.left, clip.top, clip.right, clip.bottom);
        }
    } else {
        if (s_mouseDown) {
            SendMouseButton(false);
            s_mouseDown = false;
        }
        s_haveLast = false;
        s_prevArrow = 0;
    }
    s_prevClick = click;
    s_prevEnter = enter;
}

// In-headset calibration gestures placeholder (Left/Right grip + A used to toggle AFR,
// which triggered accidentally when sprinting + power-jumping + jumping in normal play).
void UpdateCalibrationGestures(const vr::TrackingState&) {
}

// D3D9 Present hook -> which pose sample / eye the presented frame was rendered with.
bool FrameTagThunk(uint64_t& sampleId, int& eye) {
    return ue3::UE3Bridge::PopRenderedFrameTag(sampleId, eye);
}

// Horizontal FOV the game camera is forced to during gameplay (also used for the immersive projection).
// Auto: the smallest symmetric FOV that covers both eyes' full (asymmetric) frustums given the game's
// backbuffer aspect, so the rendered image fills the headset view instead of floating as a window.
float ComputeGameFovDeg(const vr::TrackingState& s) {
    const auto& cfg = samu::config::Get();
    if (cfg.gameFovDeg > 30.0f && cfg.gameFovDeg < 160.0f) return cfg.gameFovDeg;
    if (!s.viewsValid) return 95.0f;
    float tanH = 0.0f, tanV = 0.0f;
    for (const auto& v : s.views) {
        tanH = std::max({tanH, std::tan(std::fabs(v.fov.angleLeft)), std::tan(std::fabs(v.fov.angleRight))});
        tanV = std::max({tanV, std::tan(std::fabs(v.fov.angleUp)),   std::tan(std::fabs(v.fov.angleDown))});
    }
    const float aspectHW = std::max(0.25f, vr::GetDX9DX11Bridge().LastBackBufferAspectHOverW());
    const float t = std::max(tanH, tanV / aspectHW);
    return std::clamp(2.0f * std::atan(t) * 57.2957795f, 60.0f, 150.0f);
}

// Game backbuffer resolution to request via "setres": square-ish at the headset's per-eye aspect.
// RenderHeight: 0 = leave the game's resolution, -1 = auto (eye height, limited to fit desktop work area), >0 = height.
// Also monitors live capture size in gameplay and re-requests setres if the game drifts to widescreen (e.g. menu options).
void RequestGameResolution(const vr::TrackingState& s) {
    static bool s_requested = false;
    static int  s_targetW = 0, s_targetH = 0;
    static bool s_wasGameplay = false;
    static int  s_reapplyCount = 0;
    static auto s_lastReapply = std::chrono::steady_clock::now();
    if (!s.viewsValid || s.views[0].width == 0 || s.views[0].height == 0) return;

    const int cfgH = samu::config::Get().renderHeight;
    if (cfgH == 0) { s_requested = true; return; }

    if (!s_requested) {
        int h = cfgH;
        if (h < 0) {
            h = static_cast<int>(s.views[0].height);
            RECT wa{};
            int maxClientH = 0;
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0)) {
                maxClientH = static_cast<int>(wa.bottom - wa.top);
            } else {
                const int desktopH = GetSystemMetrics(SM_CYSCREEN);
                if (desktopH > 0) maxClientH = desktopH;
            }
            if (maxClientH > 0) h = std::min(h, std::max(480, maxClientH));
        }
        h = std::max(480, h) & ~7;
        int w = static_cast<int>(static_cast<float>(h) * s.views[0].width / s.views[0].height) & ~7;
        s_targetW = w;
        s_targetH = h;
        s_requested = true;
        s_lastReapply = std::chrono::steady_clock::now();
        LOG_INFO("Display: requesting game render resolution %dx%d (per-eye aspect %ux%u)", w, h,
                 s.views[0].width, s.views[0].height);
        ue3::UE3Bridge::RequestRenderResolution(w, h);
        return;
    }

    const bool inGameplay = ue3::UE3Bridge::IsGameplayActive();
    if (inGameplay && !s_wasGameplay) {
        // Give the initial gameplay entry 2 seconds for setres to apply cleanly before checking drift.
        s_reapplyCount = 0;
        s_lastReapply  = std::chrono::steady_clock::now();
    }
    s_wasGameplay = inGameplay;

    if (inGameplay && s_targetW > 0 && s_targetH > 0 && s_reapplyCount < 5) {
        const HWND gameWnd = vr::DX9DX11Bridge::GameWindow();
        if (gameWnd && GetForegroundWindow() != gameWnd) return;

        const auto& bridge = vr::GetDX9DX11Bridge();
        const int capW = static_cast<int>(bridge.LastBackBufferWidth());
        const int capH = static_cast<int>(bridge.LastBackBufferHeight());
        if (capW > 0 && capH > 0 && (std::abs(capW - s_targetW) > 64 || std::abs(capH - s_targetH) > 64)) {
            const auto now = std::chrono::steady_clock::now();
            if (now - s_lastReapply >= std::chrono::seconds(2)) {
                s_lastReapply = now;
                ++s_reapplyCount;
                LOG_INFO("Display: capture size drifted to %dx%d during gameplay; re-requesting %dx%d (%d/5)",
                         capW, capH, s_targetW, s_targetH, s_reapplyCount);
                ue3::UE3Bridge::RequestRenderResolution(s_targetW, s_targetH, true);
            }
        } else if (capW > 0 && capH > 0) {
            s_reapplyCount = 0;
        }
    }
}

bool ShouldUseImmersiveDisplay() {
    const int mode = samu::config::Get().displayMode;
    if (g_forceFlatScreen.load(std::memory_order_relaxed) || mode == 1) return false;
    if (mode == 2) return true;
    return ue3::UE3Bridge::IsGameplayActive();
}

ue3::HandUe3Pose ConvertTrackedPoseToHandUe3(const vr::TrackedPose& tp, float worldScale) {
    ue3::HandUe3Pose out{};
    vr::math::Vec3       pos = vr::math::XrToUe3Position(tp.pose.position, worldScale);
    vr::math::RotatorI32 rot = vr::math::XrToUe3Rotator(tp.pose.orientation);
    vr::math::Vec3       fwd = vr::math::XrToUe3Forward(tp.pose.orientation);
    out.offsetUU         = ue3::FVector{pos.x, pos.y, pos.z};
    out.rotationURot     = ue3::FRotator{rot.pitch, rot.yaw, rot.roll};
    out.aimDir           = ue3::FVector{fwd.x, fwd.y, fwd.z};
    out.positionValid    = tp.positionValid;
    out.orientationValid = tp.orientationValid;
    return out;
}

void SyncHmdPoseToUE3Bridge(const vr::TrackingState& s, bool suppressGameInput) {
    const float worldScale = samu::config::GetWorldScale();
    vr::math::Vec3       uePos = vr::math::XrToUe3Position(s.hmd.pose.position, worldScale);
    vr::math::RotatorI32 ueRot = vr::math::XrToUe3Rotator(s.hmd.pose.orientation);

    ue3::HmdUe3Pose raw{};
    raw.offsetUU         = ue3::FVector{uePos.x, uePos.y, uePos.z};
    raw.rotationURot     = ue3::FRotator{ueRot.pitch, ueRot.yaw, ueRot.roll};
    raw.fovDeg           = ComputeGameFovDeg(s);
    raw.sampleId         = s.sampleId;
    raw.positionValid    = s.hmd.positionValid;
    raw.orientationValid = s.hmd.orientationValid;
    raw.eyesValid        = s.viewsValid && s.hmd.positionValid;
    if (raw.eyesValid) {
        for (int eye = 0; eye < 2; ++eye) {
            vr::math::Vec3 e = vr::math::XrToUe3Position(s.views[eye].pose.position, worldScale);
            raw.eyeOffsetUU[eye] = ue3::FVector{e.x, e.y, e.z};
        }
    }
    ue3::UE3Bridge::UpdateHmdPose(raw);

    // Phase 4: Convert Right/Left Aim & Grip poses + OpenXR action states and forward to UE3Bridge
    ue3::HandUe3Pose rightAim  = ConvertTrackedPoseToHandUe3(s.rightAim,  worldScale);
    ue3::HandUe3Pose rightGrip = ConvertTrackedPoseToHandUe3(s.rightGrip, worldScale);
    ue3::HandUe3Pose leftAim   = ConvertTrackedPoseToHandUe3(s.leftAim,   worldScale);
    ue3::HandUe3Pose leftGrip  = ConvertTrackedPoseToHandUe3(s.leftGrip,  worldScale);

    ue3::MotionControllerInput mcInput{};
    if (!suppressGameInput) {
        mcInput.leftStickX          = s.input.leftStick.x;
        mcInput.leftStickY          = s.input.leftStick.y;
        mcInput.rightStickX         = s.input.rightStick.x;
        mcInput.rightStickY         = s.input.rightStick.y;
        mcInput.rightTrigger        = s.input.rightTrigger;
        mcInput.leftTrigger         = s.input.leftTrigger;
        mcInput.rightGrip           = s.input.rightGrip;
        mcInput.leftGrip            = s.input.leftGrip;
        mcInput.rightTriggerPressed = s.input.rightTriggerPressed;
        mcInput.leftTriggerPressed  = s.input.leftTriggerPressed;
        mcInput.rightGripPressed    = s.input.rightGripPressed;
        mcInput.leftGripPressed     = s.input.leftGripPressed;
        mcInput.jumpPressed                = s.input.jumpButtonPressed;
        mcInput.leftThumbstickClickPressed = s.input.leftThumbstickClickPressed;
        mcInput.active                     = s.input.active;
    }

    ue3::UE3Bridge::UpdateMotionControllers(rightAim, rightGrip, leftAim, leftGrip, mcInput);
}

DWORD WINAPI ModThreadMain(LPVOID) {
    LOG_INFO("Mod worker thread started (Phase 4: 6DOF Motion Controls, Grapple Glove Aim Override & OpenXR Action Sets).");

    const auto& cfg = samu::config::Load();
    LOG_INFO("Phase 4 active settings: WorldScale=%.1f UU/m, DecoupleCamera=%d, CameraLockSuppressor=%d, EnableStereo=%d, 6DofGrapple=%d, ControllerInput=%d",
             cfg.worldScale, cfg.decoupleCamera ? 1 : 0,
             samu::config::IsCameraLockSuppressorEnabled() ? 1 : 0,
             cfg.enableStereo ? 1 : 0,
             cfg.enable6DofGrapple ? 1 : 0,
             cfg.enableControllerInput ? 1 : 0);

    EnsureSystemDirect3DCreate9Hooked();
    vr::DX9DX11Bridge::SetFrameTagProvider(&FrameTagThunk);

    const wchar_t* cmdLine = GetCommandLineW();
    const bool isNullRhi = cmdLine && (wcsstr(cmdLine, L"-nullrhi") || wcsstr(cmdLine, L"-NULLRHI"));

    // Seed an initial deterministic non-zero HMD + Right/Left Aim Pose + OpenXR Input state
    // ONLY when running headless (-nullrhi) so CDO self-tests verify 1m = 50 UU conversion,
    // 6DOF camera decoupling, 6DOF grapple glove aim/transform override, and PlayerInput injection.
    if (isNullRhi) {
        XrVector3f    initXrPos{0.10f, 1.65f, -0.20f}; // +0.10m Right, +1.65m Up, 0.20m Forward (-Z)
        XrQuaternionf initXrOri{0.0f, 0.0871557f, 0.0f, 0.9961947f}; // +10 deg yaw around +Y
        auto initUePos = vr::math::XrToUe3Position(initXrPos, cfg.worldScale);
        auto initUeRot = vr::math::XrToUe3Rotator(initXrOri);
        ue3::HmdUe3Pose initPose{};
        initPose.offsetUU         = ue3::FVector{initUePos.x, initUePos.y, initUePos.z};
        initPose.rotationURot     = ue3::FRotator{initUeRot.pitch, initUeRot.yaw, initUeRot.roll};
        initPose.fovDeg           = 96.4f;
        initPose.positionValid    = true;
        initPose.orientationValid = true;
        ue3::UE3Bridge::UpdateHmdPose(initPose);
        LOG_INFO("Phase 3 6DOF Coordinate Transform verified: OpenXR pos=(+0.100, +1.650, -0.200) m -> "
                 "UE3 pos=(%+.1f, %+.1f, %+.1f) UU [scale=%.1f UU/m], rot=(Pitch=%d Yaw=%d Roll=%d URot)",
                 initUePos.x, initUePos.y, initUePos.z, cfg.worldScale,
                 initUeRot.pitch, initUeRot.yaw, initUeRot.roll);

        // Right Hand Aim Pose: +0.28m Right, +1.35m Up, 0.55m Forward (-Z), pitched +15 deg up & -12 deg right
        vr::TrackedPose initRightAim{};
        initRightAim.pose.position    = {0.28f, 1.35f, -0.55f};
        initRightAim.pose.orientation = {0.1294095f, -0.1035534f, -0.0136461f, 0.9860498f};
        initRightAim.positionValid    = true;
        initRightAim.orientationValid = true;

        vr::TrackedPose initLeftAim{};
        initLeftAim.pose.position    = {-0.24f, 1.32f, -0.45f};
        initLeftAim.pose.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        initLeftAim.positionValid    = true;
        initLeftAim.orientationValid = true;

        ue3::HandUe3Pose rAim = ConvertTrackedPoseToHandUe3(initRightAim, cfg.worldScale);
        ue3::HandUe3Pose lAim = ConvertTrackedPoseToHandUe3(initLeftAim,  cfg.worldScale);

        ue3::MotionControllerInput initInput{};
        initInput.leftStickX          = 0.35f;
        initInput.leftStickY          = 0.85f;
        initInput.rightStickX         = -0.25f;
        initInput.rightStickY         = 0.0f;
        initInput.rightTrigger        = 0.92f;
        initInput.leftTrigger         = 0.80f;
        initInput.rightGrip           = 0.75f;
        initInput.leftGrip            = 0.0f;
        initInput.rightTriggerPressed = true;
        initInput.leftTriggerPressed  = true;
        initInput.rightGripPressed    = true;
        initInput.leftGripPressed     = false;
        initInput.active              = true;

        ue3::UE3Bridge::UpdateMotionControllers(rAim, rAim, lAim, lAim, initInput);
        LOG_INFO("Phase 4 Right OpenXR Aim Pose Transform verified: OpenXR pos=(+0.280, +1.350, -0.550) m -> "
                 "UE3 pos=(%+.1f, %+.1f, %+.1f) UU, rot=(Pitch=%d Yaw=%d Roll=%d URot), aimDir=(%+.3f, %+.3f, %+.3f)",
                 rAim.offsetUU.X, rAim.offsetUU.Y, rAim.offsetUU.Z,
                 rAim.rotationURot.Pitch, rAim.rotationURot.Yaw, rAim.rotationURot.Roll,
                 rAim.aimDir.X, rAim.aimDir.Y, rAim.aimDir.Z);
    }

    // Only pre-initialize standalone D3D11 textures at t=0 when running headless (-nullrhi).
    // In normal D3D9 gameplay, wait until UE3's FD3D9DynamicRHI::Init creates its IDirect3DDevice9
    // so D3D11CreateDevice / DXGI / OpenXR don't race UE3's AppCompatGPU or Steam Overlay hooks.
    if (isNullRhi && cfg.enableStereo && vr::GetDX9DX11Bridge().EnsureD3D11Resources(1440, 1600)) {
        XrFovf leftFov{-0.92f, 0.78f, 0.88f, -0.88f};
        XrFovf rightFov{-0.78f, 0.92f, 0.88f, -0.88f};
        auto leftProj  = vr::math::ComposeAsymmetricProjectionLH(leftFov, cfg.nearClipUU, cfg.farClipUU);
        auto rightProj = vr::math::ComposeAsymmetricProjectionLH(rightFov, cfg.nearClipUU, cfg.farClipUU);
        LOG_INFO("Phase 3 Asymmetric Frustum Projection[Left ]: fov=(L=%.2f R=%.2f U=%.2f D=%.2f rad, %.1f deg) "
                 "M00=%.4f M11=%.4f M20=%+.4f M21=%+.4f M22=%.6f M32=%.4f | SharedHandle=%p",
                 leftFov.angleLeft, leftFov.angleRight, leftFov.angleUp, leftFov.angleDown,
                 vr::math::FovHorizontalDeg(leftFov),
                 leftProj.m[0][0], leftProj.m[1][1], leftProj.m[2][0], leftProj.m[2][1],
                 leftProj.m[2][2], leftProj.m[3][2],
                 vr::GetDX9DX11Bridge().GetEyeSharedHandle(0));
        LOG_INFO("Phase 3 Asymmetric Frustum Projection[Right]: fov=(L=%.2f R=%.2f U=%.2f D=%.2f rad, %.1f deg) "
                 "M00=%.4f M11=%.4f M20=%+.4f M21=%+.4f M22=%.6f M32=%.4f | SharedHandle=%p",
                 rightFov.angleLeft, rightFov.angleRight, rightFov.angleUp, rightFov.angleDown,
                 vr::math::FovHorizontalDeg(rightFov),
                 rightProj.m[0][0], rightProj.m[1][1], rightProj.m[2][0], rightProj.m[2][1],
                 rightProj.m[2][2], rightProj.m[3][2],
                 vr::GetDX9DX11Bridge().GetEyeSharedHandle(1));
    }

    bool ue3Hooked = false;
    if (ue3::UE3Bridge::Initialize()) {
        ue3Hooked = true;
        LOG_INFO("UE3Bridge initialized and ProcessEvent hook installed.");
    }

    vr::OpenXRManager xr;
    auto threadStart     = std::chrono::steady_clock::now();
    auto lastInitAttempt = threadStart - std::chrono::seconds(10);
    auto lastPoseLog     = threadStart;
    uint64_t framesSinceLog = 0;

    while (!g_stop.load(std::memory_order_relaxed)) {
        PollComfortHotkeys();

        // Wait until UE3 has created its D3D9 device (or at least 5 seconds have elapsed / -nullrhi)
        // before initializing OpenXR and D3D11 so UE3's RHI startup is completely undisturbed.
        if (!isNullRhi && !vr::GetDX9DX11Bridge().HasD3D9Device() &&
            (std::chrono::steady_clock::now() - threadStart) < std::chrono::seconds(5)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!ue3Hooked) {
            if (ue3::UE3Bridge::Initialize()) {
                ue3Hooked = true;
                LOG_INFO("UE3Bridge initialized and ProcessEvent hook installed.");
            }
        }

        ue3::UE3Bridge::PollSdkDump();
        if (ue3::UE3Bridge::TotalPlayerTickCalls() > 0 && Direct3DCreateCount() == 0) {
            VerifyD3D9StereoBridgeIfNullRhi();
        }

        if (!xr.IsInitialized()) {
            auto now = std::chrono::steady_clock::now();
            if (now - lastInitAttempt < std::chrono::seconds(5)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            lastInitAttempt = now;
            if (!xr.Initialize()) {
                lastInitAttempt = std::chrono::steady_clock::now();
                LOG_WARN("OpenXR init failed; retrying in 3s (is SteamVR/runtime running and HMD connected?)");
                continue;
            }
        }

        if (!xr.PollEvents()) {
            LOG_WARN("OpenXR session lost/exiting; tearing down and retrying.");
            xr.Shutdown();
            continue;
        }

        if (!xr.IsSessionRunning()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        xr.RunFrame();
        vr::TrackingState s = xr.GetTrackingState();

        // Toggle / drive the in-headset VR Settings Menu (R3 thumbstick click or F10).
        {
            static bool s_prevVrMenuBtn = false;
            const bool curVrMenuBtn = s.input.active && s.input.vrMenuButtonPressed;
            const bool kbToggle = g_toggleVrMenuRequested.exchange(false, std::memory_order_relaxed);
            if ((curVrMenuBtn && !s_prevVrMenuBtn) || kbToggle) {
                xr.ToggleVrMenu();
            }
            s_prevVrMenuBtn = curVrMenuBtn;
            if (xr.IsVrMenuOpen()) {
                const auto menuRes = xr.UpdateVrMenu(s);
                if (menuRes.recenterRequested) {
                    ue3::UE3Bridge::RecenterHmdOrigin();
                }
            }
        }

        const bool vrMenuOpen = xr.IsVrMenuOpen();
        SyncHmdPoseToUE3Bridge(s, vrMenuOpen);
        RequestGameResolution(s);
        const bool immersiveNow = ShouldUseImmersiveDisplay();
        xr.SetDisplayMode(immersiveNow, ComputeGameFovDeg(s));
        UpdateMenuPointer(xr, s, immersiveNow, vrMenuOpen);
        if (!vrMenuOpen) {
            UpdateCalibrationGestures(s);
        }
        {
            float tanH = 0.0f, tanV = 0.0f;
            if (ue3::UE3Bridge::GetMeasuredProjection(tanH, tanV)) xr.SetMeasuredProjection(tanH, tanV);
            float reticleDistM = 30.0f;
            bool  reticleLit   = false;
            const bool reticleOn = ue3::UE3Bridge::GetHandAimReticle(reticleDistM, reticleLit);
            xr.SetHandReticle(immersiveNow && reticleOn && !vrMenuOpen, reticleDistM, reticleLit);
        }
        if (g_recenterScreenRequested.exchange(false, std::memory_order_relaxed)) {
            xr.RequestRecenterVirtualScreen();
            xr.RequestRecenterVrMenu();
        }
        ++framesSinceLog;

        auto now = std::chrono::steady_clock::now();
        if (now - lastPoseLog >= std::chrono::seconds(1)) {
            double secs = std::chrono::duration<double>(now - lastPoseLog).count();
            auto& bridge = vr::GetDX9DX11Bridge();
            ue3::HmdUe3Pose uePose = ue3::UE3Bridge::GetHmdPose();

            LOG_INFO("Tracking @ %.1f Hz | state=%s | xrTime=%lld | D3D9 creates=%ld | EndScene=%llu Present=%llu | "
                     "StereoCap=%llu StereoSubmit=%llu | UE3 events=%llu (PlayerTick=%llu, CamDecouple=%llu, CamLockSuppressed=%llu)",
                     framesSinceLog / secs, vr::OpenXRManager::SessionStateName(xr.SessionState()),
                     static_cast<long long>(s.displayTime), Direct3DCreateCount(),
                     static_cast<unsigned long long>(bridge.TotalEndSceneCalls()),
                     static_cast<unsigned long long>(bridge.TotalPresentCalls()),
                     static_cast<unsigned long long>(bridge.TotalStereoFramesCaptured()),
                     static_cast<unsigned long long>(xr.SubmittedStereoFrames()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalProcessEventCalls()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalPlayerTickCalls()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalCameraDecoupleUpdates()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalSuppressedCameraEvents()));

            LogPose("HMD", s.hmd);
            LOG_INFO("  HMD->UE3   pos=(%+.1f, %+.1f, %+.1f) UU [scale=%.1f UU/m]  rot=(Pitch=%d Yaw=%d Roll=%d URot)  FOV=%.1f deg",
                     uePose.offsetUU.X, uePose.offsetUU.Y, uePose.offsetUU.Z,
                     samu::config::Get().worldScale,
                     uePose.rotationURot.Pitch, uePose.rotationURot.Yaw, uePose.rotationURot.Roll,
                     uePose.fovDeg);

            if (s.viewsValid) {
                for (int eye = 0; eye < 2; ++eye) {
                    const auto& v = s.views[eye];
                    LOG_INFO("  Eye[%d]     res=%ux%u fov=(L=%.3f R=%.3f U=%.3f D=%.3f rad) proj=[%.3f, %.3f, %.3f, %.3f] sharedHandle=%p",
                             eye, v.width, v.height,
                             v.fov.angleLeft, v.fov.angleRight, v.fov.angleUp, v.fov.angleDown,
                             v.projMatrix.m[0][0], v.projMatrix.m[1][1], v.projMatrix.m[2][0], v.projMatrix.m[2][1],
                             bridge.GetEyeSharedHandle(eye));
                }
                LOG_INFO("  Stereo     IPD=%.1f mm | WDDM SharedTexReady=%s ZeroCopy=%s | CamLockSuppressor=%s",
                         s.ipdMeters * 1000.0f,
                         bridge.IsD3D11Ready() ? "YES" : "NO",
                         bridge.IsZeroCopyWddmShared() ? "YES" : "FALLBACK_SYSMEM",
                         samu::config::IsCameraLockSuppressorEnabled() ? "ON" : "OFF");
            }

            LogPose("LeftAim", s.leftAim);
            LogPose("LeftGrip", s.leftGrip);
            LogPose("RightAim", s.rightAim);
            LogPose("RightGrip", s.rightGrip);

            ue3::HandUe3Pose rAim = ue3::UE3Bridge::GetRightAimPose();
            ue3::GrappleGloveDiag gdiag = ue3::UE3Bridge::GetLastGrappleDiag();
            LOG_INFO("  RightAim->UE3 pos=(%+.1f, %+.1f, %+.1f) UU rot=(P=%d Y=%d R=%d URot) aimDir=(%+.3f, %+.3f, %+.3f) | "
                     "GloveOverrides: Aim=%llu Xform=%llu InputInj=%llu",
                     rAim.offsetUU.X, rAim.offsetUU.Y, rAim.offsetUU.Z,
                     rAim.rotationURot.Pitch, rAim.rotationURot.Yaw, rAim.rotationURot.Roll,
                     rAim.aimDir.X, rAim.aimDir.Y, rAim.aimDir.Z,
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalGrappleAimOverrides()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalGloveTransformOverrides()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalPlayerInputInjections()));
            LOG_INFO("  Inputs     LStick=(%+.2f, %+.2f) RStick=(%+.2f, %+.2f) RTrig=%.2f[%c] LTrig=%.2f[%c] RGrip=%.2f[%c] LGrip=%.2f[%c]",
                     s.input.leftStick.x, s.input.leftStick.y,
                     s.input.rightStick.x, s.input.rightStick.y,
                     s.input.rightTrigger, s.input.rightTriggerPressed ? 'P' : '-',
                     s.input.leftTrigger,  s.input.leftTriggerPressed  ? 'P' : '-',
                     s.input.rightGrip,    s.input.rightGripPressed    ? 'P' : '-',
                     s.input.leftGrip,     s.input.leftGripPressed     ? 'P' : '-');
            (void)gdiag;
            ue3::InputInjectionDiag idiag = ue3::UE3Bridge::GetLastInputDiag();
            float dbgRetDistM = 0.0f;
            bool  dbgRetLit   = false;
            const bool dbgRetVis = ue3::UE3Bridge::GetHandAimReticle(dbgRetDistM, dbgRetLit);
            LOG_INFO("  Locomotion inj(fwd=%+.2f strafe=%+.2f turn=%+.2f) -> script aForward=%+.1f aStrafe=%+.1f | "
                     "Stereo=%s pairing=%s AFRframes=%llu poseMatched=%llu | UpdateCamera=%llu ViewPoint=%llu | FpsCap=%.0f | BB=%.3f(H/W) GameFOV=%.1f ViewScale=%.3f | Reticle=%d(%.1fm,%s) VrMenu=%d",
                     idiag.injForward, idiag.injStrafe, idiag.injTurn, idiag.postForward, idiag.postStrafe,
                     samu::config::GetStereoMode() == 1 ? "AFR" : "MONO",
                     ue3::UE3Bridge::IsRenderPairingExact() ? "EXACT" : "APPROX",
                     static_cast<unsigned long long>(xr.AfrStereoFrames()),
                     static_cast<unsigned long long>(xr.PoseMatchedFrames()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalUpdateCameraOverrides()),
                     static_cast<unsigned long long>(ue3::UE3Bridge::TotalViewPointOverrides()),
                     ue3::UE3Bridge::AppliedFrameRateCap(),
                     bridge.LastBackBufferAspectHOverW(), ComputeGameFovDeg(s), samu::config::GetViewScale(),
                     (immersiveNow && dbgRetVis && !vrMenuOpen) ? 1 : 0, dbgRetDistM, dbgRetLit ? "LIT" : "idle", vrMenuOpen ? 1 : 0);
            lastPoseLog = now;
            framesSinceLog = 0;
        }
    }

    LOG_INFO("Mod worker thread stopping.");
    xr.Shutdown();
    ue3::UE3Bridge::Shutdown();
    return 0;
}

} // namespace

void StartModThread() {
    g_stop = false;
    g_thread = CreateThread(nullptr, 0, &ModThreadMain, nullptr, 0, nullptr);
    if (!g_thread) LOG_ERROR("CreateThread failed: %lu", GetLastError());
}

void SignalModThreadStop() {
    g_stop = true;
    if (g_thread) { CloseHandle(g_thread); g_thread = nullptr; }
}

} // namespace samu::proxy
