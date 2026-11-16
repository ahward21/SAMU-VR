#pragma once
// src/common/Config.h
// Reads and writes samu_vr.ini settings (WorldScale, CameraLockSuppressor, Stereo, etc.).
// Shared across vr_core and ue3_bridge without coupling either module to the other.

#include <cstdint>
#include <string>

namespace samu::config {

struct Settings {
    // [VR]
    float worldScale           = 72.0f;      // 1m in OpenXR = 72.0 Unreal Units
    float ipdScale             = 1.0f;       // Multiplier for stereo eye separation
    float nearClipUU           = 2.0f;       // Near clip plane in Unreal Units (~4 cm)
    float farClipUU            = 100000.0f;  // Far clip plane in Unreal Units (~2000 m)
    bool  decoupleCamera       = true;       // Feed 6DOF HMD pose into PlayerController/Camera
    bool  autoRecenterOnLevel  = true;       // Recenter HMD position/yaw/height once each time a gameplay level starts
    float eyeHeightOffsetCm    = 0.0f;       // Extra vertical camera offset (cm) on top of the recentered head height

    // [Comfort]
    bool  cameraLockSuppressor = true;       // Block scripted camera pitch/roll animations & shakes
    bool  suppressPitchRoll    = true;       // Lock out non-HMD pitch/roll on PlayerController/Camera
    int   toggleSuppressorVKey = 0x75;       // VK_F6 (117) toggles Camera-Lock Suppressor live
    int   recenterVKey         = 0x76;       // VK_F7 (118) recenters HMD yaw & roomscale origin
    int   openVrMenuVKey       = 0x79;       // VK_F10 (121) toggles the in-headset VR Settings Menu

    // [Stereo]
    bool  enableStereo         = true;       // Enable DX9-to-DX11 shared texture stereo submission
    bool  sbsCompanionWindow   = false;      // Draw side-by-side stereo preview in desktop window
    int   stereoMode           = 0;          // 0 = Mono (full-rate smooth both eyes), 1 = AFR (alternate-frame stereo: L/R eye on alternating game frames)
    int   toggleStereoVKey     = 0x78;       // VK_F9 (120) toggles Mono <-> AFR stereo live
    bool  matchRenderPose      = true;       // Submit each image with the exact HMD pose the game rendered it with (removes head-turn judder)

    // [Performance]
    float targetFrameRate      = 120.0f;     // Raises UE3's smoothed frame-rate cap (default 62 Hz) above the HMD rate; 0 = leave the game's setting
    bool  disableVSync         = true;       // Force D3DPRESENT_INTERVAL_IMMEDIATE so the desktop monitor's refresh rate can't cap the game
    int   renderHeight         = -1;         // Game backbuffer height via "setres" at the HMD eye aspect: -1 = auto, 0 = leave game's, >0 = explicit

    // [Display]
    int   displayMode          = 0;          // 0 = Auto (flat screen in menus/loading, immersive in gameplay), 1 = Always flat, 2 = Always immersive
    float flatScreenDistanceM  = 2.5f;       // Distance of the flat virtual screen (meters)
    float flatScreenWidthM     = 3.2f;       // Width of the flat virtual screen (meters)
    float gameFovDeg           = 0.0f;       // Horizontal FOV forced on the game camera in gameplay (0 = auto from HMD)
    float viewScale            = 1.0f;       // Trim: symmetrical game + VR display FOV scale (1.0 = full HMD FOV)
    float imageSharpness       = 0.55f;      // 0..1: D3D11 contrast-adaptive sharpening intensity
    float hudTextLift          = 0.82f;      // 0..1: lifts subtitles and Flash tutorial popups up into the comfortable VR view area
    int   toggleDisplayVKey    = 0x77;       // VK_F8 (119) toggles forced flat-screen mode

    // [MotionControls] (Phase 4)
    bool  enable6DofGrapple    = true;       // Override grapple aim vector & glove viewmodel with Right OpenXR Aim Pose
    bool  enableControllerInput= true;       // Inject OpenXR thumbsticks, triggers, and grips into UE3 PlayerInput
    float triggerThreshold     = 0.55f;      // Analog trigger press threshold [0..1]
    float gripThreshold        = 0.55f;      // Analog grip press threshold [0..1]
    float thumbstickDeadzone   = 0.15f;      // Radial deadzone for Left/Right thumbsticks
    float grappleRangeUU       = 4500.0f;    // Default grapple trace distance in Unreal Units
    bool  aimFromHand          = true;       // Game aim (grapple trace, interactions) follows the right hand instead of the head
    bool  hideGameCrosshair    = true;       // Hide the game's screen-centre (head-locked) crosshair while the hand reticle is shown
    bool  trueScaleHand        = true;       // Draw the hand with the world's perspective instead of the flat-screen "weapon FOV" trick
    bool  disableMotionBlur    = true;       // Turn off the game's motion blur (smears the picture on every head turn in VR)
    bool  enableLeftHand       = false;      // Spawn a tracked left-hand grapple mesh from LeftAimPose (disabled by default)
    int   locomotionDirection  = 1;          // Left stick moves relative to: 0 = body (game yaw), 1 = head (HMD yaw), 2 = left controller
    float turnSpeedScale       = 1.0f;       // Right-stick smooth-turn speed multiplier (0 disables stick turning)
    float reticleScale         = 1.0f;       // VR hand reticle size multiplier (0.5 .. 2.0)
};

// Loads samu_vr.ini from the DLL directory (creating a default file if missing).
const Settings& Load();

// Thread-safe access to current runtime settings.
const Settings& Get();

// Runtime setters & getters (updated live by the in-headset VR Settings Menu).
void  SetCameraLockSuppressor(bool enabled);
bool  IsCameraLockSuppressorEnabled();

void  SetStereoMode(int mode);
int   GetStereoMode();

void  SetMatchRenderPose(bool enabled);
bool  IsMatchRenderPoseEnabled();

void  SetViewScale(float scale);
float GetViewScale();

void  SetImageSharpness(float sharpness);
float GetImageSharpness();

void  SetHudTextLift(float lift);
float GetHudTextLift();

void  SetWorldScale(float uuPerMeter);
float GetWorldScale();

void  SetEyeHeightOffsetCm(float cm);
float GetEyeHeightOffsetCm();

void  SetTurnSpeedScale(float scale);
float GetTurnSpeedScale();

void  SetLocomotionDirection(int mode);
int   GetLocomotionDirection();

void  SetAimFromHand(bool enabled);
bool  IsAimFromHandEnabled();

void  SetHideGameCrosshair(bool enabled);
bool  IsHideGameCrosshairEnabled();

void  SetReticleScale(float scale);
float GetReticleScale();

// Persists the current runtime-adjustable settings to samu_vr.ini.
bool  SaveSettings();
void  SaveViewScale();

} // namespace samu::config
