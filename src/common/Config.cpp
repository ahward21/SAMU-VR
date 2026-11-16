#include "common/Config.h"
#include "common/Log.h"

#include <windows.h>
#include <atomic>
#include <cstdio>
#include <share.h>

namespace samu::config {
namespace {

Settings           g_settings{};
std::atomic<bool>  g_loaded{false};
std::atomic<bool>  g_cameraLockSuppressor{true};
std::atomic<int>   g_stereoMode{0};
std::atomic<bool>  g_matchRenderPose{true};
std::atomic<float> g_viewScale{1.0f};
std::atomic<float> g_imageSharpness{0.55f};
std::atomic<float> g_hudTextLift{0.82f};
std::atomic<float> g_worldScale{72.0f};
std::atomic<float> g_eyeHeightOffsetCm{0.0f};
std::atomic<float> g_turnSpeedScale{1.0f};
std::atomic<int>   g_locomotionDirection{1};
std::atomic<bool>  g_aimFromHand{true};
std::atomic<bool>  g_hideGameCrosshair{true};
std::atomic<float> g_reticleScale{1.0f};
std::wstring       g_iniPath;

void WriteDefaultIniIfMissing(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES) return;

    FILE* fp = _wfsopen(path.c_str(), L"w", _SH_DENYNO);
    if (!fp) return;

    std::fprintf(fp,
        "; ===========================================================================\n"
        "; SAMU-VR Configuration (A Story About My Uncle - OpenXR 6DOF VR Mod)\n"
        "; ===========================================================================\n"
        "\n"
        "[VR]\n"
        "; Conversion scale from OpenXR meters (right-handed Y-up) to UE3 Unreal Units (left-handed Z-up).\n"
        "; Default: 1.0 meter = 72.0 UU.\n"
        "WorldScale=72.0\n"
        "IpdScale=1.0\n"
        "NearClipUU=2.0\n"
        "FarClipUU=100000.0\n"
        "DecoupleCamera=1\n"
        "; Recenter head position/yaw/height automatically when each gameplay level starts (F7 recenters manually)\n"
        "AutoRecenterOnLevel=1\n"
        "; Extra camera height in centimeters on top of the recentered head height (negative = lower)\n"
        "EyeHeightOffsetCm=0\n"
        "\n"
        "[Comfort]\n"
        "; Blocks scripted camera pitch/roll animations and shakes during grapple swings, charges, and landings.\n"
        "CameraLockSuppressor=1\n"
        "SuppressPitchRoll=1\n"
        "; Virtual-Key codes: 117 = F6 (toggle Camera-Lock Suppressor), 118 = F7 (recenter HMD), 121 = F10 (VR settings menu)\n"
        "ToggleSuppressorVKey=117\n"
        "RecenterVKey=118\n"
        "OpenVrMenuVKey=121\n"
        "\n"
        "[Stereo]\n"
        "; Enables the DX9-to-DX11 shared texture stereo pipeline and OpenXR xrEndFrame layer submission.\n"
        "EnableStereo=1\n"
        "SideBySideCompanionWindow=0\n"
        "; 0 = Mono (full-rate smooth both eyes), 1 = AFR alternate-frame stereo (real depth; each eye updates every other frame)\n"
        "StereoMode=0\n"
        "; 120 = F9 toggles Mono <-> AFR live\n"
        "ToggleStereoVKey=120\n"
        "; Submit each frame with the exact head pose it was rendered with so the compositor can reproject it (1 = on)\n"
        "MatchRenderPose=1\n"
        "\n"
        "[Performance]\n"
        "; Raises UE3's smoothed frame-rate cap (game default 62) above the HMD rate; 0 = leave the game's setting\n"
        "TargetFrameRate=120\n"
        "; Disable desktop vsync so the monitor refresh rate can't cap the game below the headset rate\n"
        "DisableVSync=1\n"
        "; Game render resolution height, applied via 'setres' at the headset's per-eye aspect (square on most HMDs).\n"
        "; -1 = auto (eye height, max 90%% of desktop height), 0 = keep the game's own resolution, >0 = explicit height\n"
        "RenderHeight=-1\n"
        "\n"
        "[Display]\n"
        "; Mode: 0 = Auto (flat virtual screen in menus/loading, immersive in gameplay), 1 = Always flat, 2 = Always immersive\n"
        "Mode=0\n"
        "FlatScreenDistanceM=2.5\n"
        "FlatScreenWidthM=3.2\n"
        "; Horizontal FOV forced on the game camera during gameplay (0 = auto from headset)\n"
        "GameFovDeg=0\n"
        "; Trim for how large the game image is displayed (1.0 = exact 1:1 head-turn angular match)\n"
        "ViewScale=1.0\n"
        "; D3D11 contrast-adaptive sharpening intensity (0.0 .. 1.0, default 0.55)\n"
        "ImageSharpness=0.55\n"
        "; Lifts subtitles and Flash tutorial popups up into the comfortable VR view area (0.0 .. 1.0)\n"
        "HudTextLift=0.82\n"
        "; 119 = F8 toggles forced flat-screen mode\n"
        "ToggleFlatScreenVKey=119\n"
        "\n"
        "[MotionControls]\n"
        "; Phase 4: 6DOF Right Aim Pose grapple glove override & OpenXR action set PlayerInput injection.\n"
        "Enable6DofGrapple=1\n"
        "EnableControllerInput=1\n"
        "TriggerThreshold=0.55\n"
        "GripThreshold=0.55\n"
        "ThumbstickDeadzone=0.15\n"
        "GrappleRangeUU=4500.0\n"
        "AimFromHand=1\n"
        "HideGameCrosshair=1\n"
        "TrueScaleHand=1\n"
        "EnableLeftHand=0\n"
        "ReticleScale=1.0\n"
        "; Left stick moves relative to: 0 = body, 1 = head direction, 2 = left controller direction\n"
        "LocomotionDirection=1\n"
        "; Right-stick smooth-turn speed multiplier (0 disables stick turning)\n"
        "TurnSpeedScale=1.0\n");
    std::fclose(fp);
}

float ReadFloat(const wchar_t* section, const wchar_t* key, float defVal, const wchar_t* path) {
    wchar_t defBuf[64]{};
    wchar_t outBuf[64]{};
    std::swprintf(defBuf, 64, L"%.4f", defVal);
    GetPrivateProfileStringW(section, key, defBuf, outBuf, 64, path);
    wchar_t* endPtr = nullptr;
    float v = std::wcstof(outBuf, &endPtr);
    return (endPtr != outBuf) ? v : defVal;
}

int ReadInt(const wchar_t* section, const wchar_t* key, int defVal, const wchar_t* path) {
    return static_cast<int>(GetPrivateProfileIntW(section, key, defVal, path));
}

} // namespace

const Settings& Load() {
    std::wstring localIni = samu::log::ModuleDirectory() + L"\\samu_vr.ini";
    WriteDefaultIniIfMissing(localIni);

    // Also mirror default samu_vr.ini to workspace root if running from game_bin/build.
    std::wstring repoIni = L"C:\\Users\\alexa\\Desktop\\SAMU-VR\\samu_vr.ini";
    DWORD repoAttr = GetFileAttributesW(L"C:\\Users\\alexa\\Desktop\\SAMU-VR");
    if (repoAttr != INVALID_FILE_ATTRIBUTES && (repoAttr & FILE_ATTRIBUTE_DIRECTORY)) {
        WriteDefaultIniIfMissing(repoIni);
    }

    // If localIni could not be written (e.g., read-only Program Files directory), fall back to repoIni
    DWORD localAttr = GetFileAttributesW(localIni.c_str());
    const wchar_t* p = (localAttr != INVALID_FILE_ATTRIBUTES) ? localIni.c_str() : repoIni.c_str();
    g_settings.worldScale           = ReadFloat(L"VR", L"WorldScale", 72.0f, p);
    g_settings.ipdScale             = ReadFloat(L"VR", L"IpdScale", 1.0f, p);
    g_settings.nearClipUU           = ReadFloat(L"VR", L"NearClipUU", 2.0f, p);
    g_settings.farClipUU            = ReadFloat(L"VR", L"FarClipUU", 100000.0f, p);
    g_settings.decoupleCamera       = ReadInt(L"VR", L"DecoupleCamera", 1, p) != 0;
    g_settings.autoRecenterOnLevel  = ReadInt(L"VR", L"AutoRecenterOnLevel", 1, p) != 0;
    g_settings.eyeHeightOffsetCm    = ReadFloat(L"VR", L"EyeHeightOffsetCm", 0.0f, p);

    g_settings.cameraLockSuppressor = ReadInt(L"Comfort", L"CameraLockSuppressor", 1, p) != 0;
    g_settings.suppressPitchRoll    = ReadInt(L"Comfort", L"SuppressPitchRoll", 1, p) != 0;
    g_settings.toggleSuppressorVKey = ReadInt(L"Comfort", L"ToggleSuppressorVKey", 0x75, p);
    g_settings.recenterVKey         = ReadInt(L"Comfort", L"RecenterVKey", 0x76, p);
    g_settings.openVrMenuVKey       = ReadInt(L"Comfort", L"OpenVrMenuVKey", 0x79, p);

    g_settings.enableStereo         = ReadInt(L"Stereo", L"EnableStereo", 1, p) != 0;
    g_settings.sbsCompanionWindow   = ReadInt(L"Stereo", L"SideBySideCompanionWindow", 0, p) != 0;
    g_settings.stereoMode           = ReadInt(L"Stereo", L"StereoMode", 0, p) == 1 ? 1 : 0;
    g_settings.toggleStereoVKey     = ReadInt(L"Stereo", L"ToggleStereoVKey", 0x78, p);
    g_settings.matchRenderPose      = ReadInt(L"Stereo", L"MatchRenderPose", 1, p) != 0;

    g_settings.targetFrameRate      = ReadFloat(L"Performance", L"TargetFrameRate", 120.0f, p);
    g_settings.disableVSync         = ReadInt(L"Performance", L"DisableVSync", 1, p) != 0;
    g_settings.renderHeight         = ReadInt(L"Performance", L"RenderHeight", -1, p);

    g_settings.displayMode          = ReadInt(L"Display", L"Mode", 0, p);
    g_settings.flatScreenDistanceM  = ReadFloat(L"Display", L"FlatScreenDistanceM", 2.5f, p);
    g_settings.flatScreenWidthM     = ReadFloat(L"Display", L"FlatScreenWidthM", 3.2f, p);
    g_settings.gameFovDeg           = ReadFloat(L"Display", L"GameFovDeg", 0.0f, p);
    g_settings.viewScale            = ReadFloat(L"Display", L"ViewScale", 1.0f, p);
    if (g_settings.viewScale < 0.3f || g_settings.viewScale > 2.5f) g_settings.viewScale = 1.0f;
    g_settings.imageSharpness       = ReadFloat(L"Display", L"ImageSharpness", 0.55f, p);
    if (g_settings.imageSharpness < 0.0f) g_settings.imageSharpness = 0.0f;
    if (g_settings.imageSharpness > 1.0f) g_settings.imageSharpness = 1.0f;
    g_settings.hudTextLift          = ReadFloat(L"Display", L"HudTextLift", 0.82f, p);
    if (g_settings.hudTextLift < 0.0f) g_settings.hudTextLift = 0.0f;
    if (g_settings.hudTextLift > 1.0f) g_settings.hudTextLift = 1.0f;
    g_iniPath = p;
    g_settings.toggleDisplayVKey    = ReadInt(L"Display", L"ToggleFlatScreenVKey", 0x77, p);

    g_settings.enable6DofGrapple    = ReadInt(L"MotionControls", L"Enable6DofGrapple", 1, p) != 0;
    g_settings.enableControllerInput= ReadInt(L"MotionControls", L"EnableControllerInput", 1, p) != 0;
    g_settings.triggerThreshold     = ReadFloat(L"MotionControls", L"TriggerThreshold", 0.55f, p);
    g_settings.gripThreshold        = ReadFloat(L"MotionControls", L"GripThreshold", 0.55f, p);
    g_settings.thumbstickDeadzone   = ReadFloat(L"MotionControls", L"ThumbstickDeadzone", 0.15f, p);
    g_settings.grappleRangeUU       = ReadFloat(L"MotionControls", L"GrappleRangeUU", 4500.0f, p);
    g_settings.locomotionDirection  = ReadInt(L"MotionControls", L"LocomotionDirection", 1, p);
    g_settings.aimFromHand          = ReadInt(L"MotionControls", L"AimFromHand", 1, p) != 0;
    g_settings.hideGameCrosshair    = ReadInt(L"MotionControls", L"HideGameCrosshair", 1, p) != 0;
    g_settings.trueScaleHand        = ReadInt(L"MotionControls", L"TrueScaleHand", 1, p) != 0;
    g_settings.disableMotionBlur    = ReadInt(L"Performance", L"DisableMotionBlur", 1, p) != 0;
    g_settings.enableLeftHand       = ReadInt(L"MotionControls", L"EnableLeftHand", 0, p) != 0;
    g_settings.turnSpeedScale       = ReadFloat(L"MotionControls", L"TurnSpeedScale", 1.0f, p);
    g_settings.reticleScale         = ReadFloat(L"MotionControls", L"ReticleScale", 1.0f, p);
    if (g_settings.reticleScale < 0.3f) g_settings.reticleScale = 0.3f;
    if (g_settings.reticleScale > 3.0f) g_settings.reticleScale = 3.0f;

    g_cameraLockSuppressor.store(g_settings.cameraLockSuppressor, std::memory_order_release);
    g_stereoMode.store(g_settings.stereoMode, std::memory_order_release);
    g_matchRenderPose.store(g_settings.matchRenderPose, std::memory_order_release);
    g_viewScale.store(g_settings.viewScale, std::memory_order_release);
    g_imageSharpness.store(g_settings.imageSharpness, std::memory_order_release);
    g_hudTextLift.store(g_settings.hudTextLift, std::memory_order_release);
    g_worldScale.store(g_settings.worldScale, std::memory_order_release);
    g_eyeHeightOffsetCm.store(g_settings.eyeHeightOffsetCm, std::memory_order_release);
    g_turnSpeedScale.store(g_settings.turnSpeedScale, std::memory_order_release);
    g_locomotionDirection.store(g_settings.locomotionDirection, std::memory_order_release);
    g_aimFromHand.store(g_settings.aimFromHand, std::memory_order_release);
    g_hideGameCrosshair.store(g_settings.hideGameCrosshair, std::memory_order_release);
    g_reticleScale.store(g_settings.reticleScale, std::memory_order_release);
    g_loaded.store(true, std::memory_order_release);

    LOG_INFO("Config loaded from '%ls': WorldScale=%.1f UU/m, DecoupleCamera=%d, CameraLockSuppressor=%d, EnableStereo=%d, 6DofGrapple=%d, ControllerInput=%d",
             p, g_settings.worldScale,
             g_settings.decoupleCamera ? 1 : 0,
             g_settings.cameraLockSuppressor ? 1 : 0,
             g_settings.enableStereo ? 1 : 0,
             g_settings.enable6DofGrapple ? 1 : 0,
             g_settings.enableControllerInput ? 1 : 0);
    LOG_INFO("Config: StereoMode=%s, MatchRenderPose=%d, TargetFrameRate=%.0f, DisableVSync=%d, AutoRecenterOnLevel=%d, "
             "EyeHeightOffsetCm=%.1f, LocomotionDirection=%d, TurnSpeedScale=%.2f | ViewScale=%.2f HudTextLift=%.2f ReticleScale=%.2f",
             g_settings.stereoMode == 1 ? "AFR" : "MONO", g_settings.matchRenderPose ? 1 : 0,
             g_settings.targetFrameRate, g_settings.disableVSync ? 1 : 0,
             g_settings.autoRecenterOnLevel ? 1 : 0, g_settings.eyeHeightOffsetCm,
             g_settings.locomotionDirection, g_settings.turnSpeedScale,
             g_settings.viewScale, g_settings.hudTextLift, g_settings.reticleScale);
    return g_settings;
}

const Settings& Get() {
    if (!g_loaded.load(std::memory_order_acquire)) {
        return Load();
    }
    return g_settings;
}

void SetCameraLockSuppressor(bool enabled) {
    g_cameraLockSuppressor.store(enabled, std::memory_order_release);
    g_settings.cameraLockSuppressor = enabled;
    LOG_INFO("Config: Camera-Lock Suppressor toggled -> %s", enabled ? "ENABLED" : "DISABLED");
}

bool IsCameraLockSuppressorEnabled() {
    return g_cameraLockSuppressor.load(std::memory_order_relaxed);
}

void SetStereoMode(int mode) {
    mode = (mode == 1) ? 1 : 0;
    g_stereoMode.store(mode, std::memory_order_release);
    g_settings.stereoMode = mode;
    LOG_INFO("Config: stereo mode -> %s", mode == 1 ? "AFR (alternate-frame stereo)" : "MONO");
}

int GetStereoMode() {
    return g_stereoMode.load(std::memory_order_relaxed);
}

void SetMatchRenderPose(bool enabled) {
    g_matchRenderPose.store(enabled, std::memory_order_release);
    g_settings.matchRenderPose = enabled;
}

bool IsMatchRenderPoseEnabled() {
    return g_matchRenderPose.load(std::memory_order_relaxed);
}

void SetViewScale(float scale) {
    if (scale < 0.50f) scale = 0.50f;
    if (scale > 1.30f) scale = 1.30f;
    g_viewScale.store(scale, std::memory_order_release);
    g_settings.viewScale = scale;
}

float GetViewScale() {
    return g_viewScale.load(std::memory_order_relaxed);
}

void SetImageSharpness(float sharpness) {
    if (sharpness < 0.0f) sharpness = 0.0f;
    if (sharpness > 1.0f) sharpness = 1.0f;
    g_imageSharpness.store(sharpness, std::memory_order_release);
    g_settings.imageSharpness = sharpness;
}

float GetImageSharpness() {
    return g_imageSharpness.load(std::memory_order_relaxed);
}

void SetHudTextLift(float lift) {
    if (lift < 0.0f) lift = 0.0f;
    if (lift > 1.0f) lift = 1.0f;
    g_hudTextLift.store(lift, std::memory_order_release);
    g_settings.hudTextLift = lift;
}

float GetHudTextLift() {
    return g_hudTextLift.load(std::memory_order_relaxed);
}

void SetWorldScale(float uuPerMeter) {
    if (uuPerMeter < 20.0f) uuPerMeter = 20.0f;
    if (uuPerMeter > 120.0f) uuPerMeter = 120.0f;
    g_worldScale.store(uuPerMeter, std::memory_order_release);
    g_settings.worldScale = uuPerMeter;
}

float GetWorldScale() {
    return g_worldScale.load(std::memory_order_relaxed);
}

void SetEyeHeightOffsetCm(float cm) {
    if (cm < -50.0f) cm = -50.0f;
    if (cm > 50.0f)  cm = 50.0f;
    g_eyeHeightOffsetCm.store(cm, std::memory_order_release);
    g_settings.eyeHeightOffsetCm = cm;
}

float GetEyeHeightOffsetCm() {
    return g_eyeHeightOffsetCm.load(std::memory_order_relaxed);
}

void SetTurnSpeedScale(float scale) {
    if (scale < 0.0f) scale = 0.0f;
    if (scale > 3.0f) scale = 3.0f;
    g_turnSpeedScale.store(scale, std::memory_order_release);
    g_settings.turnSpeedScale = scale;
}

float GetTurnSpeedScale() {
    return g_turnSpeedScale.load(std::memory_order_relaxed);
}

void SetLocomotionDirection(int mode) {
    if (mode < 0) mode = 0;
    if (mode > 2) mode = 2;
    g_locomotionDirection.store(mode, std::memory_order_release);
    g_settings.locomotionDirection = mode;
}

int GetLocomotionDirection() {
    return g_locomotionDirection.load(std::memory_order_relaxed);
}

void SetAimFromHand(bool enabled) {
    g_aimFromHand.store(enabled, std::memory_order_release);
    g_settings.aimFromHand = enabled;
}

bool IsAimFromHandEnabled() {
    return g_aimFromHand.load(std::memory_order_relaxed);
}

void SetHideGameCrosshair(bool enabled) {
    g_hideGameCrosshair.store(enabled, std::memory_order_release);
    g_settings.hideGameCrosshair = enabled;
}

bool IsHideGameCrosshairEnabled() {
    return g_hideGameCrosshair.load(std::memory_order_relaxed);
}

void SetReticleScale(float scale) {
    if (scale < 0.3f) scale = 0.3f;
    if (scale > 3.0f) scale = 3.0f;
    g_reticleScale.store(scale, std::memory_order_release);
    g_settings.reticleScale = scale;
}

float GetReticleScale() {
    return g_reticleScale.load(std::memory_order_relaxed);
}

bool SaveSettings() {
    if (g_iniPath.empty()) return false;
    auto writeFloat = [&](const wchar_t* sec, const wchar_t* key, float v, const wchar_t* fmt) {
        wchar_t buf[32]{};
        std::swprintf(buf, 32, fmt, v);
        return WritePrivateProfileStringW(sec, key, buf, g_iniPath.c_str()) != 0;
    };
    auto writeInt = [&](const wchar_t* sec, const wchar_t* key, int v) {
        wchar_t buf[32]{};
        std::swprintf(buf, 32, L"%d", v);
        return WritePrivateProfileStringW(sec, key, buf, g_iniPath.c_str()) != 0;
    };

    bool ok = true;
    ok &= writeFloat(L"VR", L"WorldScale", GetWorldScale(), L"%.1f");
    ok &= writeFloat(L"VR", L"EyeHeightOffsetCm", GetEyeHeightOffsetCm(), L"%.1f");
    ok &= writeInt(L"Comfort", L"CameraLockSuppressor", IsCameraLockSuppressorEnabled() ? 1 : 0);
    ok &= writeInt(L"Stereo", L"StereoMode", GetStereoMode());
    ok &= writeInt(L"Stereo", L"MatchRenderPose", IsMatchRenderPoseEnabled() ? 1 : 0);
    ok &= writeFloat(L"Display", L"ViewScale", GetViewScale(), L"%.2f");
    ok &= writeFloat(L"Display", L"ImageSharpness", GetImageSharpness(), L"%.2f");
    ok &= writeFloat(L"Display", L"HudTextLift", GetHudTextLift(), L"%.2f");
    ok &= writeInt(L"MotionControls", L"AimFromHand", IsAimFromHandEnabled() ? 1 : 0);
    ok &= writeInt(L"MotionControls", L"HideGameCrosshair", IsHideGameCrosshairEnabled() ? 1 : 0);
    ok &= writeFloat(L"MotionControls", L"ReticleScale", GetReticleScale(), L"%.2f");
    ok &= writeInt(L"MotionControls", L"LocomotionDirection", GetLocomotionDirection());
    ok &= writeFloat(L"MotionControls", L"TurnSpeedScale", GetTurnSpeedScale(), L"%.2f");
    LOG_INFO("Config: saved live VR settings to '%ls' (%s)", g_iniPath.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

void SaveViewScale() {
    if (g_iniPath.empty()) return;
    wchar_t buf[32];
    std::swprintf(buf, 32, L"%.3f", GetViewScale());
    const BOOL ok = WritePrivateProfileStringW(L"Display", L"ViewScale", buf, g_iniPath.c_str());
    LOG_INFO("Config: saved [Display] ViewScale=%ls to '%ls' (%s)", buf, g_iniPath.c_str(), ok ? "ok" : "FAILED");
}

} // namespace samu::config
