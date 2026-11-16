#pragma once
// src/ue3_bridge/UE3Bridge.h
// Manages signature/RVA resolution for GNames, GObjects, and UObject::ProcessEvent,
// installs the MinHook detour on UObject::ProcessEvent, triggers the runtime SDK
// dump into ./sdk_dump/, and implements Phase 3 6DOF Camera Decoupling and the
// toggleable Camera-Lock Suppressor.
// Strictly decoupled from src/vr_core/.

#include "ue3_bridge/UE3Types.h"
#include <cstdint>

namespace samu::ue3 {

using PFN_ProcessEvent = void(__thiscall*)(UObject* self, UFunction* func, void* parms, void* result);

struct HmdUe3Pose {
    FVector  offsetUU{0.0f, 0.0f, 0.0f};      // Roomscale HMD (head-center) position in Unreal Units (1m = 50 UU)
    FVector  eyeOffsetUU[2]{};                // Roomscale Left/Right eye positions in Unreal Units (for AFR stereo)
    FRotator rotationURot{0, 0, 0};           // HMD 6DOF rotation in UE3 URot units (65536 = 360 deg)
    float    fovDeg           = 95.0f;        // Horizontal FOV derived from OpenXR asymmetric frustum
    uint64_t sampleId         = 0;            // Opaque id of the OpenXR pose sample (used to match render pose on submit)
    bool     positionValid    = false;
    bool     orientationValid = false;
    bool     eyesValid        = false;
};

// Live diagnostics of the OpenXR -> PlayerInput locomotion injection.
struct InputInjectionDiag {
    float injForward  = 0.0f;   // Value added to PlayerInput.aBaseY before PlayerTick
    float injStrafe   = 0.0f;   // Value added to PlayerInput.aStrafe before PlayerTick
    float injTurn     = 0.0f;   // Value added to PlayerInput.aTurn before PlayerTick
    float postForward = 0.0f;   // PlayerInput.aForward after PlayerTick (script-scaled; non-zero => consumed)
    float postStrafe  = 0.0f;   // PlayerInput.aStrafe after PlayerTick
};

struct DecoupledCameraDiag {
    FVector  baseLocation{0.0f, 0.0f, 0.0f};
    FVector  hmdOffsetUU{0.0f, 0.0f, 0.0f};
    FVector  finalLocation{0.0f, 0.0f, 0.0f};
    FRotator finalRotation{0, 0, 0};
    int32_t  bodyYaw     = 0;     // PlayerController (body) yaw the HMD/hand offsets are rotated by
    float    finalFovDeg = 90.0f;
    int      renderEye   = -1;    // -1 = mono, 0 = left, 1 = right (AFR)
    bool     active      = false;
};

// Phase 4: 6DOF Motion Controller pose in UE3 coordinates (1m = 50 UU)
struct HandUe3Pose {
    FVector  offsetUU{0.0f, 0.0f, 0.0f};      // Roomscale hand position in Unreal Units (1m = 50 UU)
    FRotator rotationURot{0, 0, 0};           // Hand 6DOF rotation in UE3 URot units (65536 = 360 deg)
    FVector  aimDir{1.0f, 0.0f, 0.0f};        // Unit forward aim vector in UE3 roomscale space
    bool     positionValid    = false;
    bool     orientationValid = false;
};

// Phase 4: OpenXR controller action states for UE3 PlayerInput injection
struct MotionControllerInput {
    float leftStickX          = 0.0f;         // Left Thumbstick X (Strafe)
    float leftStickY          = 0.0f;         // Left Thumbstick Y (Forward/Back locomotion)
    float rightStickX         = 0.0f;         // Right Thumbstick X (Turn)
    float rightStickY         = 0.0f;         // Right Thumbstick Y (Look)
    float rightTrigger        = 0.0f;         // Right Trigger [0..1] (Fire Grapple)
    float leftTrigger         = 0.0f;         // Left Trigger [0..1] (Jet Boots / Jump)
    float rightGrip           = 0.0f;         // Right Grip [0..1] (Power Jump Charge)
    float leftGrip            = 0.0f;         // Left Grip [0..1] (Sprint / Interact)
    bool  rightTriggerPressed = false;
    bool  leftTriggerPressed  = false;
    bool  rightGripPressed    = false;
    bool  leftGripPressed     = false;
    bool  jumpPressed                = false;
    bool  leftThumbstickClickPressed = false;
    bool  active                     = false;
};

// Phase 4: Diagnostic snapshot for 6DOF Grapple Glove aim & viewmodel overrides
struct GrappleGloveDiag {
    FVector  gloveWorldLocation{0.0f, 0.0f, 0.0f};
    FRotator gloveWorldRotation{0, 0, 0};
    FVector  gloveRelOffsetUU{0.0f, 0.0f, 0.0f};
    FVector  aimRayOrigin{0.0f, 0.0f, 0.0f};
    FVector  aimRayDirection{1.0f, 0.0f, 0.0f};
    FVector  calcTraceStart{0.0f, 0.0f, 0.0f};
    FVector  calcTraceEnd{0.0f, 0.0f, 0.0f};
    bool     active = false;
};

class UE3Bridge {
public:
    // Resolves GNames, GObjects, and UObject::ProcessEvent in the current process
    // and installs the MinHook detour.
    static bool Initialize();

    // Test/harness entry point allowing explicit pointers when running inside smoke_test.exe.
    static bool InitializeWithTargets(TArray<FNameEntry*>* gnames,
                                      TArray<UObject*>* gobjects,
                                      void* processEventTarget);

    static void Shutdown();

    // Called periodically from ModThread (or lazily inside ProcessEvent) to trigger
    // the SDK dump if it hasn't run yet.
    static void PollSdkDump();

    // Phase 3: Called by ModThread each frame with the converted UE3 HMD pose
    // (OpenXR right-handed Y-up meters -> UE3 left-handed Z-up Unreal Units).
    static void UpdateHmdPose(const HmdUe3Pose& rawPose);

    // Render-pose matching: every gameplay frame the game thread latches one HMD pose sample (and, in AFR
    // stereo mode, which eye it renders). Each latched frame is queued; the D3D9 Present hook pops one entry
    // per presented frame so the compositor receives the exact pose (and eye) each image was rendered with.
    // eye = -1 for mono frames, 0 = left, 1 = right. Returns false if no gameplay frame is queued.
    static bool PopRenderedFrameTag(uint64_t& sampleId, int& eye);

    // True once Present has been observed on the game thread (UE3 rendering thread disabled), which makes the
    // frame <-> pose pairing exact. AFR stereo is only enabled while this holds (otherwise eyes could swap).
    static bool IsRenderPairingExact();

    // The game's REAL projection, measured through LocalPlayer.Project: tan(half horizontal FOV) and
    // tan(half vertical FOV) of the rendered image. Returns false until a stable measurement exists.
    static bool GetMeasuredProjection(float& tanHalfH, float& tanHalfV);
    // True while the game's aim is being driven from the right hand (camera decoupled from aim).
    static bool IsAimFromHandActive();
    // Hand aim reticle: returns true while the game's aim follows the right hand. distanceM = distance from the
    // hand to what its ray hits in the game world; lit = the game reports that spot as grapple-able.
    static bool GetHandAimReticle(float& distanceM, bool& lit);

    // Requests a game backbuffer resolution (applied via "setres" in gameplay; forceReapply resets cached target).
    static void RequestRenderResolution(int width, int height, bool forceReapply = false);

    // Recenters the roomscale HMD position & yaw reference to the current HMD pose.
    static void RecenterHmdOrigin();

    // Phase 4: Called by ModThread each frame with the converted 6DOF controller poses & action states.
    static void UpdateMotionControllers(const HandUe3Pose& rightAim,
                                        const HandUe3Pose& rightGrip,
                                        const HandUe3Pose& leftAim,
                                        const HandUe3Pose& leftGrip,
                                        const MotionControllerInput& input);

    static HmdUe3Pose GetHmdPose();
    static HandUe3Pose GetRightAimPose();
    static HandUe3Pose GetLeftAimPose();
    static MotionControllerInput GetControllerInput();
    static DecoupledCameraDiag GetLastCameraDiag();
    static GrappleGloveDiag GetLastGrappleDiag();

    static bool IsHooked();
    // True while an ASAMUPlayerController with a live Pawn is ticking on a gameplay (non-front-end) map.
    // False in menus, legal/intro screens, and during level loads -> VR display should use a flat virtual screen.
    static bool IsGameplayActive();
    static uint64_t TotalProcessEventCalls();
    static uint64_t TotalPlayerTickCalls();
    static uint64_t TotalCameraDecoupleUpdates();
    static uint64_t TotalSuppressedCameraEvents();
    static uint64_t TotalGrappleAimOverrides();
    static uint64_t TotalGloveTransformOverrides();
    static uint64_t TotalPlayerInputInjections();
    static uint64_t TotalViewPointOverrides();
    static uint64_t TotalUpdateCameraOverrides();
    static InputInjectionDiag GetLastInputDiag();
    // Smoothed frame-rate cap applied to the live GameEngine (0 if not applied yet).
    static float AppliedFrameRateCap();
    static const TArray<FNameEntry*>* GetGNames();
    static const TArray<UObject*>* GetGObjects();
};

} // namespace samu::ue3
