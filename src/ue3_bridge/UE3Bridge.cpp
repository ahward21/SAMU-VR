#include "ue3_bridge/UE3Bridge.h"
#include "ue3_bridge/SdkDumper.h"
#include "common/Config.h"
#include "common/Log.h"

#include <windows.h>
#include <MinHook.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <vector>

namespace samu::ue3 {
namespace {

// Verified UE3 property offsets from ./sdk_dump/
namespace offsets {
    // Engine.Actor (Size = 0x01CC)
    constexpr size_t kActor_Location                  = 0x0054; // FVector (0x0C)
    constexpr size_t kActor_Rotation                  = 0x0060; // FRotator (0x0C)
    constexpr size_t kActor_MinSize                   = 0x01CC;

    // Engine.Controller (Size = 0x0350)
    constexpr size_t kController_Pawn                 = 0x01D0; // Pawn* (0x04)

    // Engine.Pawn (Size = 0x04EC)
    constexpr size_t kPawn_EyeHeight                  = 0x02C8; // float (0x04)  (0x028C is GroundSpeed!)
    constexpr size_t kPawn_Weapon                     = 0x03B4; // Weapon* (0x04)
    constexpr size_t kPawn_MinSize                    = 0x03BC;

    // Engine.Weapon (Size = 0x02BC) & UDKBase.UDKWeapon (Size = 0x02DC)
    constexpr size_t kWeapon_FireOffset               = 0x0284; // FVector (0x0C)
    constexpr size_t kWeapon_WeaponRange              = 0x0294; // float (0x04)
    constexpr size_t kWeapon_Mesh                     = 0x0298; // MeshComponent* (0x04)
    constexpr size_t kWeapon_MinSize                  = 0x02BC;
    constexpr size_t kUDKWeapon_AimTraceRange         = 0x02CC; // float (0x04)

    // asamu.GrappleGun (Size = 0x0518)
    constexpr size_t kGrappleGun_BeamSocketName       = 0x0370; // FName (0x08): socket the grapple beam leaves the hand from
    constexpr size_t kGrappleGun_vGrappleLocation     = 0x02F0; // FVector (0x0C)
    constexpr size_t kGrappleGun_fMaxDistance         = 0x0328; // float (0x04)
    constexpr size_t kGrappleGun_PC                   = 0x03C0; // ASAMUPlayerController* (0x04)
    constexpr size_t kGrappleGun_OffsetX              = 0x041C; // float (0x04)
    constexpr size_t kGrappleGun_OffsetY              = 0x0420; // float (0x04)
    constexpr size_t kGrappleGun_OffsetZ              = 0x0424; // float (0x04)
    constexpr size_t kGrappleGun_MinSize              = 0x0518;

    // Engine.PlayerController (Size = 0x0588)
    constexpr size_t kPC_PlayerCamera                 = 0x0354; // Camera* (0x04)
    constexpr size_t kPC_FOVAngle                     = 0x0384; // float (0x04)
    constexpr size_t kPC_DesiredFOV                   = 0x0388; // float (0x04)
    constexpr size_t kPC_TargetViewRotation           = 0x0394; // FRotator (0x0C)
    constexpr size_t kPC_BlendedTargetViewRotation    = 0x03A4; // FRotator (0x0C)
    constexpr size_t kPC_PlayerInput                  = 0x0440; // PlayerInput* (0x04)
    constexpr size_t kPC_MinSize                      = 0x0588;

    // Engine.PlayerInput (Size = 0x0260) & asamu.ASAMUPlayerInput (Size = 0x0268)
    constexpr size_t kPlayerInput_Flags               = 0x0118; // uint32_t (0x01 = bUsingGamepad)
    constexpr size_t kPlayerInput_aBaseX              = 0x0130; // float (0x04)
    constexpr size_t kPlayerInput_aBaseY              = 0x0134; // float (0x04)
    constexpr size_t kPlayerInput_aBaseZ              = 0x0138; // float (0x04)
    constexpr size_t kPlayerInput_aMouseX             = 0x013C; // float (0x04)
    constexpr size_t kPlayerInput_aMouseY             = 0x0140; // float (0x04)
    constexpr size_t kPlayerInput_aForward            = 0x0144; // float (0x04)
    constexpr size_t kPlayerInput_aTurn               = 0x0148; // float (0x04)
    constexpr size_t kPlayerInput_aStrafe             = 0x014C; // float (0x04)
    constexpr size_t kPlayerInput_aUp                 = 0x0150; // float (0x04)
    constexpr size_t kPlayerInput_aLookUp             = 0x0154; // float (0x04)
    constexpr size_t kPlayerInput_aRightAnalogTrigger = 0x0158; // float (0x04)
    constexpr size_t kPlayerInput_aLeftAnalogTrigger  = 0x015C; // float (0x04)
    constexpr size_t kPlayerInput_RawJoyUp            = 0x0220; // float (0x04)
    constexpr size_t kPlayerInput_RawJoyRight         = 0x0224; // float (0x04)
    constexpr size_t kPlayerInput_RawJoyLookRight     = 0x0228; // float (0x04)
    constexpr size_t kPlayerInput_RawJoyLookUp        = 0x022C; // float (0x04)
    constexpr size_t kPlayerInput_MoveForwardSpeed    = 0x0230; // float (0x04)
    constexpr size_t kPlayerInput_MoveStrafeSpeed     = 0x0234; // float (0x04)
    constexpr size_t kPlayerInput_MinSize             = 0x0260;

    // UDKBase.UDKPlayerController (Size = 0x0870)
    constexpr size_t kUDKPC_ShakeOffset               = 0x05A4; // FVector (0x0C)
    constexpr size_t kUDKPC_ShakeRot                  = 0x05B0; // FRotator (0x0C)
    constexpr size_t kUDKPC_MinSize                   = 0x05BC;

    // asamu.ASAMUPlayerController (Size = 0x08DC)
    constexpr size_t kASAMUPC_CamAnimSlotsStart       = 0x0874; // 6x CameraAnimInst* (Sprint, Charge, Fall, HardFall, PowerJumpCharge, Shake)
    constexpr size_t kASAMUPC_CamAnimSlotCount        = 6;
    constexpr size_t kASAMUPC_MinSize                 = 0x08DC;

    // Engine.Camera (Size = 0x04A8)
    constexpr size_t kCamera_PCOwner                  = 0x01CC; // PlayerController* (0x04)
    constexpr size_t kCamera_Flags                    = 0x01DC; // bitfield: 0x02 = bConstrainAspectRatio
    constexpr size_t kCamera_ConstrainedAspectRatio   = 0x01E4; // float
    constexpr size_t kCamera_CameraCache_POV_Loc      = 0x0384; // FVector (0x0C)
    constexpr size_t kCamera_CameraCache_POV_Rot      = 0x0390; // FRotator (0x0C)
    constexpr size_t kCamera_CameraCache_POV_FOV      = 0x039C; // float (0x04)
    constexpr size_t kCamera_ViewTarget_POV_Loc       = 0x03C8; // FVector (0x0C)
    constexpr size_t kCamera_ViewTarget_POV_Rot       = 0x03D4; // FRotator (0x0C)
    constexpr size_t kCamera_ViewTarget_POV_FOV       = 0x03E0; // float (0x04)
    constexpr size_t kCamera_MinSize                  = 0x04A8;

    // Engine.CameraAnimInst (Size = 0x0220)
    constexpr size_t kCamAnimInst_BasePlayScale       = 0x0060; // float
    constexpr size_t kCamAnimInst_TransientScale      = 0x0064; // float
    constexpr size_t kCamAnimInst_CurrentBlendWeight  = 0x0068; // float
    constexpr size_t kCamAnimInst_MinSize             = 0x0070;
} // namespace offsets

uintptr_t            g_moduleBase         = 0;
TArray<FNameEntry*>* g_gnames             = nullptr;
TArray<UObject*>*    g_gobjects           = nullptr;
void*                g_processEventTarget = nullptr;
PFN_ProcessEvent     g_origProcessEvent   = nullptr;

std::atomic<bool>     g_hooked{false};
std::atomic<bool>     g_sdkDumped{false};
std::atomic<bool>     g_inSyntheticSelfTest{false};
std::atomic<int32_t>  g_playerTickFnameIdx{-1};
std::atomic<int32_t>  g_pcFnameIdx{-1};
std::atomic<int32_t>  g_controllerFnameIdx{-1};
std::atomic<int32_t>  g_udkPcFnameIdx{-1};
std::atomic<int32_t>  g_asamuPcFnameIdx{-1};
std::atomic<int32_t>  g_pawnFnameIdx{-1};
std::atomic<int32_t>  g_cameraFnameIdx{-1};
std::atomic<int32_t>  g_camAnimInstFnameIdx{-1};
std::atomic<int32_t>  g_weaponFnameIdx{-1};
std::atomic<int32_t>  g_grappleGunFnameIdx{-1};
std::atomic<int32_t>  g_playerInputFnameIdx{-1};
std::atomic<int32_t>  g_skelMeshCompFnameIdx{-1};
std::atomic<uint64_t> g_totalEvents{0};
std::atomic<uint64_t> g_playerTicks{0};
std::atomic<uint64_t> g_cameraDecoupleUpdates{0};
std::atomic<uint64_t> g_suppressedCameraEvents{0};
std::atomic<uint64_t> g_grappleAimOverrides{0};
std::atomic<uint64_t> g_gloveTransformOverrides{0};
std::atomic<uint64_t> g_playerInputInjections{0};
std::atomic<uint64_t> g_viewPointOverrides{0};
std::atomic<uint64_t> g_updateCameraOverrides{0};
std::atomic<uint64_t> g_fovOverrides{0};
std::atomic<float>    g_measTanH{0.0f};
std::atomic<float>    g_measTanV{0.0f};
std::atomic<bool>     g_measValid{false};
std::atomic<bool>     g_aimFromHandActive{false};

// Head view of the current frame (what must be RENDERED), computed in the Camera.UpdateCamera post-hook.
FVector  g_headViewLoc{0.0f, 0.0f, 0.0f};
FRotator g_headViewRot{0, 0, 0};
uint64_t g_headViewFrame   = 0;      // g_gameFrameIdx when computed (0 = never)
bool     g_headViewIsPawn  = true;
// Render-path validation (LocalPlayer.Project): does the renderer take its view from the GetPlayerViewPoint
// event (head view) rather than reading CameraCache directly? Required before CameraCache may hold the hand aim.
// Frame phase on the game thread: true from Present (end of the previous frame) until the camera update at the
// end of the world tick. GetPlayerViewPoint events in this phase come from game logic (weapon aim through
// Pawn.GetBaseAimRotation, traces, interactions); after the camera update they come from the renderer.
bool     g_inTickPhase        = false;
bool     g_probingProjection  = false;   // forces the head view while MeasureViewProjection runs
int      g_renderHeadViewOk   = 0;   // consecutive measurements where the head-view centre projected to screen centre
bool     g_aimFromHandBlocked = false;
std::atomic<float>    g_appliedFpsCap{0.0f};

std::mutex            g_dumpMutex;
std::mutex            g_poseMutex;
HmdUe3Pose            g_rawHmdPose{};
HandUe3Pose           g_rawRightAimPose{};
HandUe3Pose           g_rawRightGripPose{};
HandUe3Pose           g_rawLeftAimPose{};
HandUe3Pose           g_rawLeftGripPose{};
MotionControllerInput g_controllerInput{};
FVector               g_recenterOffsetUU{0.0f, 0.0f, 0.0f};
int32_t               g_recenterYawURot    = 0;
float                 g_recenterWorldScale = 72.0f;
DecoupledCameraDiag   g_lastCameraDiag{};
GrappleGloveDiag      g_lastGrappleDiag{};

// Cached gameplay action UFunctions on PlayerController / ASAMUPlayerController
UFunction* g_fnStartFire           = nullptr;
UFunction* g_fnStopFire            = nullptr;
UFunction* g_fnRocketBoostKeyDown  = nullptr;
UFunction* g_fnPowerJumpKeyDown    = nullptr;
UFunction* g_fnPowerJumpKeyUp      = nullptr;
UFunction* g_fnStartSprinting      = nullptr;
UFunction* g_fnStopSprinting       = nullptr;
bool       g_prevRightTriggerPress = false;
bool       g_prevLeftTriggerPress  = false;
bool       g_prevRightGripPress    = false;
bool       g_prevLeftGripPress     = false;
bool       g_prevLeftStickClick    = false;
bool       g_stickSprintLatched    = false;
UFunction* g_fnConsoleCommand      = nullptr;
UFunction* g_fnActorSetLocation    = nullptr;
UFunction* g_fnActorSetRotation    = nullptr;
bool       g_actorMoveFnsResolved  = false;

// ---- Game-thread-only per-frame VR state (written in the PlayerTick pre-hook) ----
HmdUe3Pose  g_framePose{};                 // HMD pose latched once per gameplay frame (recentered)
HandUe3Pose g_frameRightAimPose{};         // Right-hand aim pose latched with g_framePose
bool        g_framePoseValid = false;
int         g_frameEye       = -1;         // -1 = mono, 0 = left, 1 = right (AFR stereo)
uint64_t    g_gameFrameIdx   = 0;
std::string g_lastRecenteredMap;
std::string g_lastEngineTweakMap;
uint64_t    g_approxPairingFrames = 0;
uint64_t    g_lastRenderToggleMs  = 0;
int         g_renderToggleCount   = 0;
int         g_appliedResW = 0;
int         g_appliedResH = 0;
uint32_t    g_appliedResEpoch = 0;
std::atomic<int>      g_requestedResW{0};
std::atomic<int>      g_requestedResH{0};
std::atomic<uint32_t> g_requestedResEpoch{0};
std::atomic<DWORD> g_gameThreadId{0};
std::atomic<bool>  g_renderPairingExact{false};
std::atomic<bool>  g_presentObserved{false};

// Body yaw bookkeeping. The game derives its view from PlayerController.Rotation, so outside PlayerTick we store
// (body yaw + HMD yaw) there; inside PlayerTick it is restored to the pure body yaw so stick turning, movement
// direction and the pawn's facing keep working off the body.
UObject* g_bodyYawPc      = nullptr;
int32_t  g_bodyYaw        = 0;
int32_t  g_lastWrittenYaw = 0;
bool     g_bodyYawValid   = false;

// AFR safety: alternate-eye rendering needs the per-eye camera POSITION to reach the renderer. Count frames in
// which a view hook actually applied it; AFR stays off until that is proven.
uint32_t g_viewOverridesThisFrame = 0;
uint32_t g_framesWithViewOverride = 0;
std::atomic<bool> g_afrPositionProven{false};

// Diagnostics: which camera/aim related script events does the game really dispatch through ProcessEvent?
std::unordered_map<UFunction*, uint32_t> g_camEventCounts;   // game thread only

// Render-pose matching: one packed tag (sampleId << 2 | eyeCode) per latched gameplay frame.
std::mutex           g_tagMutex;
std::deque<uint64_t> g_tagQueue;

// Locomotion injection bookkeeping (detects whether UE3 reset the axis since our last write).
struct AxisInjState { float lastTotal = 0.0f; float lastInj = 0.0f; bool valid = false; };
AxisInjState       g_injFwd, g_injStrafe, g_injTurn;
InputInjectionDiag g_inputDiag{};          // protected by g_poseMutex

FVector RotateVectorByYawURot(const FVector& v, int32_t yawURot) {
    constexpr float kURotToRad = 3.14159265358979323846f / 32768.0f;
    const float rad = static_cast<float>(yawURot) * kURotToRad;
    const float c = std::cos(rad);
    const float s = std::sin(rad);
    return FVector{
        v.X * c - v.Y * s,
        v.X * s + v.Y * c,
        v.Z
    };
}

FVector RotatorToForwardVector(const FRotator& r) {
    constexpr float kURotToRad = 3.14159265358979323846f / 32768.0f;
    const float pitchRad = static_cast<float>(r.Pitch) * kURotToRad;
    const float yawRad   = static_cast<float>(r.Yaw)   * kURotToRad;
    const float cp = std::cos(pitchRad);
    const float sp = std::sin(pitchRad);
    const float cy = std::cos(yawRad);
    const float sy = std::sin(yawRad);
    return FVector{cp * cy, cp * sy, sp};
}

// Converts a raw roomscale offset (OpenXR LOCAL space in UU) into the recentered player frame and applies
// the configured eye-height offset. Caller must hold g_poseMutex.
FVector ApplyRecenterLocked(const FVector& v) {
    const auto& cfg = samu::config::Get();
    const float curScale = (cfg.worldScale > 1.0f) ? cfg.worldScale : 72.0f;
    const float recScale = (g_recenterWorldScale > 1.0f) ? g_recenterWorldScale : curScale;
    const float ratio    = curScale / recScale;
    FVector rel{v.X - g_recenterOffsetUU.X * ratio,
                v.Y - g_recenterOffsetUU.Y * ratio,
                v.Z - g_recenterOffsetUU.Z * ratio};
    FVector out = RotateVectorByYawURot(rel, -g_recenterYawURot);
    out.Z += cfg.eyeHeightOffsetCm * curScale / 100.0f;
    return out;
}

// The HMD pose used for everything rendered in the current game frame (falls back to the live pose).
HmdUe3Pose GetFramePose() {
    return g_framePoseValid ? g_framePose : UE3Bridge::GetHmdPose();
}

void PushFrameTag(uint64_t sampleId, int eye) {
    const uint64_t eyeCode = (eye < 0) ? 0u : (eye == 0 ? 1u : 2u);
    std::lock_guard lock(g_tagMutex);
    g_tagQueue.push_back((sampleId << 2) | eyeCode);
    while (g_tagQueue.size() > 4) g_tagQueue.pop_front();
}

void ClearFrameTags() {
    std::lock_guard lock(g_tagMutex);
    g_tagQueue.clear();
}

float ApplyDeadzone(float val, float dz) {
    float absV = std::fabs(val);
    if (absV <= dz) return 0.0f;
    float sign = (val >= 0.0f) ? 1.0f : -1.0f;
    float norm = (absV - dz) / (1.0f - dz);
    return sign * (norm > 1.0f ? 1.0f : norm);
}

// Simple byte-pattern scanner over a memory range (-1 in pattern = wildcard).
const uint8_t* FindPattern(const uint8_t* start, size_t size, const int* pat, size_t patLen) {
    if (!start || size < patLen) return nullptr;
    size_t limit = size - patLen;
    for (size_t i = 0; i <= limit; ++i) {
        bool ok = true;
        for (size_t j = 0; j < patLen; ++j) {
            if (pat[j] >= 0 && start[i + j] != static_cast<uint8_t>(pat[j])) {
                ok = false;
                break;
            }
        }
        if (ok) return start + i;
    }
    return nullptr;
}

bool GetTextSectionBounds(uintptr_t base, const uint8_t*& outStart, size_t& outSize) {
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            outStart = reinterpret_cast<const uint8_t*>(base + sec[i].VirtualAddress);
            outSize  = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

bool ResolveAddressesFromBinary() {
    g_moduleBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const uint8_t* textStart = nullptr;
    size_t textSize = 0;
    if (!GetTextSectionBounds(g_moduleBase, textStart, textSize)) {
        LOG_ERROR("UE3Bridge: failed to read PE .text section from base 0x%08X",
                  static_cast<unsigned>(g_moduleBase));
        return false;
    }

    // 1. Verify direct RVA for UObject::ProcessEvent (0x0019EBC0) first:
    //    55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 54
    const uint8_t* candidatePe = reinterpret_cast<const uint8_t*>(g_moduleBase + rva::kProcessEvent);
    if (textSize > rva::kProcessEvent &&
        candidatePe[0] == 0x55 && candidatePe[1] == 0x8B && candidatePe[2] == 0xEC &&
        candidatePe[3] == 0x6A && candidatePe[4] == 0xFF && candidatePe[5] == 0x68 &&
        candidatePe[17] == 0x83 && candidatePe[18] == 0xEC && candidatePe[19] == 0x54) {
        g_processEventTarget = const_cast<uint8_t*>(candidatePe);
        g_gnames   = reinterpret_cast<TArray<FNameEntry*>*>(g_moduleBase + rva::kGNames);
        g_gobjects = reinterpret_cast<TArray<UObject*>*>(g_moduleBase + rva::kGObjects);
        LOG_INFO("UE3Bridge: verified exact binary RVAs on ASAMU-Win32-Shipping.exe (base=0x%08X):",
                 static_cast<unsigned>(g_moduleBase));
        LOG_INFO("  GNames                = VA 0x%08X (RVA 0x%08X)",
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_gnames)),
                 static_cast<unsigned>(rva::kGNames));
        LOG_INFO("  GObjects              = VA 0x%08X (RVA 0x%08X)",
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_gobjects)),
                 static_cast<unsigned>(rva::kGObjects));
        LOG_INFO("  UObject::ProcessEvent = VA 0x%08X (RVA 0x%08X, vtable[%zu])",
                 static_cast<unsigned>(reinterpret_cast<uintptr_t>(g_processEventTarget)),
                 static_cast<unsigned>(rva::kProcessEvent), rva::kProcessEventVIndex);
        return true;
    }

    // 2. Pattern scan fallback across .text
    LOG_INFO("UE3Bridge: primary RVA prologue not matched; scanning .text (%zu bytes)...", textSize);

    static constexpr int kPatProcessEvent[] = {
        0x55, 0x8B, 0xEC, 0x6A, 0xFF, 0x68, -1, -1, -1, -1,
        0x64, 0xA1, 0x00, 0x00, 0x00, 0x00, 0x50, 0x83, 0xEC, 0x54,
        0xA1, -1, -1, -1, -1, 0x33, 0xC5, 0x89, 0x45, 0xF0,
        0x53, 0x56, 0x57, 0x50, 0x8D, 0x45, 0xF4, 0x64, 0xA3, 0x00, 0x00, 0x00, 0x00,
        0x8B, 0xF1, 0x89, 0x75, 0xEC, 0x8B, 0x5D, 0x08,
        0xF7, 0x83, 0x84, 0x00, 0x00, 0x00, 0x02, 0x04, 0x00, 0x00
    };
    const uint8_t* peHit = FindPattern(textStart, textSize, kPatProcessEvent, std::size(kPatProcessEvent));

    static constexpr int kPatGNames[] = {
        0xA1, -1, -1, -1, -1, 0x8B, 0x34, 0xB0, 0xF6, 0x46, 0x08, 0x01, 0x8D, 0x46, 0x10
    };
    const uint8_t* gnHit = FindPattern(textStart, textSize, kPatGNames, std::size(kPatGNames));

    static constexpr int kPatGObjects[] = {
        0xA1, -1, -1, -1, -1, 0x83, 0xC4, 0x10, 0x8B, 0x0D, -1, -1, -1, -1, 0x39, 0x1C, 0xB1
    };
    const uint8_t* goHit = FindPattern(textStart, textSize, kPatGObjects, std::size(kPatGObjects));

    if (!peHit || !gnHit || !goHit) {
        LOG_ERROR("UE3Bridge: signature scan failed (PE=%p, GNames=%p, GObjects=%p)",
                  static_cast<const void*>(peHit), static_cast<const void*>(gnHit), static_cast<const void*>(goHit));
        return false;
    }

    g_processEventTarget = const_cast<uint8_t*>(peHit);
    uintptr_t gnAddr = *reinterpret_cast<const uintptr_t*>(gnHit + 1);
    uintptr_t goAddr = *reinterpret_cast<const uintptr_t*>(goHit + 10);
    g_gnames   = reinterpret_cast<TArray<FNameEntry*>*>(gnAddr);
    g_gobjects = reinterpret_cast<TArray<UObject*>*>(goAddr);

    LOG_INFO("UE3Bridge: resolved via signature scan: GNames=0x%08X, GObjects=0x%08X, ProcessEvent=0x%08X",
             static_cast<unsigned>(gnAddr), static_cast<unsigned>(goAddr),
             static_cast<unsigned>(reinterpret_cast<uintptr_t>(peHit)));
    return true;
}

// Checks whether a function name is a scripted camera pitch/roll animation or shake
// that should be blocked when the Camera-Lock Suppressor is enabled.
bool IsScriptedCameraLockEvent(std::string_view fnName, uint16_t parmsSize, void* parms) {
    if (fnName == "FallAnimation" ||
        fnName == "HardFallAnimation" ||
        fnName == "PlayHardFallAnimation" ||
        fnName == "StartCameraShake" ||
        fnName == "ViewShake" ||
        fnName == "DamageShake" ||
        fnName == "ClientPlayCameraAnim" ||
        fnName == "ClientPlayCameraShake" ||
        fnName == "PlayCameraShake" ||
        fnName == "PlayWorldCameraShake" ||
        fnName == "StartFastFallAnim" ||
        fnName == "FastFallingAnimRampUp") {
        return true;
    }

    // Functions with return values in parms: zero out return slot before suppressing
    if (fnName == "PlayChargingAnimation") {
        if (parms && parmsSize >= sizeof(void*)) {
            *reinterpret_cast<void**>(parms) = nullptr;
        }
        return true;
    }
    if (fnName == "LandingShake") {
        if (parms && parmsSize >= sizeof(uint32_t)) {
            *reinterpret_cast<uint32_t*>(parms) = 0;
        }
        return true;
    }
    if (fnName == "PlayCameraAnim") {
        // Camera::PlayCameraAnim has ParmsSize=0x28 with CameraAnimInst* ReturnValue at +0x24
        if (parms && parmsSize >= 0x28) {
            *reinterpret_cast<void**>( static_cast<uint8_t*>(parms) + 0x24 ) = nullptr;
        }
        return true;
    }
    return false;
}

bool IsRunningNullRhi() {
    const wchar_t* cmdLine = GetCommandLineW();
    return cmdLine && (wcsstr(cmdLine, L"-nullrhi") || wcsstr(cmdLine, L"-NULLRHI"));
}

// SEH-guarded validation that a raw pointer is a live UObject still registered in GObjects.
// Protects against stale Pawn->Weapon / PC->PlayerInput / PC->PlayerCamera pointers during level loads.
bool IsLiveUObject(const UObject* obj) {
    const uintptr_t addr = reinterpret_cast<uintptr_t>(obj);
    if (addr < 0x10000 || (addr & 0x3) != 0 || !g_gobjects || !g_gobjects->Data) return false;
    __try {
        const int32_t idx = obj->ObjectInternalInteger;
        if (idx < 0 || idx >= g_gobjects->Num) return false;
        if (g_gobjects->Data[idx] != obj) return false;
        return obj->Class != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int64_t SteadyNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Returns the FName of the outermost package (e.g. "ASAMUFrontEndMap", "AG-StarHaven").
std::string_view OutermostAnsiName(const UObject* obj) {
    const UObject* cur = obj;
    for (int i = 0; cur && cur->Outer && i < 64; ++i) cur = cur->Outer;
    if (!cur || !g_gnames || !g_gnames->Data || !g_gnames->IsValidIndex(cur->Name.Index)) return {};
    const FNameEntry* e = g_gnames->Data[cur->Name.Index];
    if (!e || e->IsWide()) return {};
    return std::string_view(e->AnsiName);
}

std::atomic<bool>    g_gameplayActive{false};
std::atomic<bool>    g_gamePaused{false};
std::atomic<int64_t> g_lastGameplayTickMs{0};
std::atomic<int64_t> g_pauseStartedMs{0};

bool IsGameplayActiveNow() {
    return g_gameplayActive.load(std::memory_order_relaxed) &&
           !g_gamePaused.load(std::memory_order_relaxed) &&
           (SteadyNowMs() - g_lastGameplayTickMs.load(std::memory_order_relaxed)) < 1000;
}

bool HasSuperClassNamed(const UStruct* cls, std::atomic<int32_t>& cachedFnameIdx, std::string_view targetName) {
    if (!cls || !g_gnames || !g_gnames->Data) return false;
    int32_t idx = cachedFnameIdx.load(std::memory_order_relaxed);
    if (idx < 0) {
        for (const UStruct* cur = cls, *prev = nullptr; cur && cur != prev; ) {
            if (cur->Name.Number == 0 && g_gnames->IsValidIndex(cur->Name.Index)) {
                const FNameEntry* e = g_gnames->Data[cur->Name.Index];
                if (e && !e->IsWide() && targetName == e->AnsiName) {
                    cachedFnameIdx.store(cur->Name.Index, std::memory_order_relaxed);
                    return true;
                }
            }
            prev = cur;
            cur  = cur->SuperField;
        }
        return false;
    }
    for (int step = 0; cls && step < 32; ++step) {
        if (cls->Name.Index == idx && cls->Name.Number == 0) {
            return true;
        }
        const UStruct* next = cls->SuperField;
        if (next == cls) break;
        cls = next;
    }
    return false;
}

bool IsPlayerControllerInstance(const UObject* obj) {
    return IsLiveUObject(obj) &&
           obj->Class->PropertiesSize >= static_cast<int32_t>(offsets::kPC_MinSize) &&
           HasSuperClassNamed(obj->Class, g_pcFnameIdx, "PlayerController");
}

// Gameplay = an ASAMUPlayerController possessing a live Pawn on a non-front-end map.
// Menus, legal screens, and loading (no PlayerTicks) stay in flat virtual-screen mode.
bool EvaluateGameplayState(UObject* pcObj, std::string_view& mapName) {
    bool gameplay = false;
    mapName = {};
    if (HasSuperClassNamed(pcObj->Class, g_asamuPcFnameIdx, "ASAMUPlayerController")) {
        mapName = OutermostAnsiName(pcObj);
        if (!mapName.empty() && mapName.find("FrontEnd") == std::string_view::npos) {
            UObject* pawn = *reinterpret_cast<UObject* const*>(reinterpret_cast<uint8_t*>(pcObj) + offsets::kController_Pawn);
            gameplay = IsLiveUObject(pawn);
        }
    }
    if (gameplay) {
        const int64_t nowMs = SteadyNowMs();
        g_lastGameplayTickMs.store(nowMs, std::memory_order_relaxed);
        // Safety fallback: if PlayerTick resumed after pause (>250ms) and CompletePauseMenuClose was missed, clear pause.
        if (g_gamePaused.load(std::memory_order_relaxed) &&
            nowMs - g_pauseStartedMs.load(std::memory_order_relaxed) > 250) {
            g_gamePaused.store(false, std::memory_order_relaxed);
    g_stickSprintLatched     = false;
    g_prevLeftStickClick     = false;
            LOG_INFO("UE3Bridge: PlayerTick resumed -> clearing pause state.");
        }
    } else {
        g_gamePaused.store(false, std::memory_order_relaxed);
    g_stickSprintLatched     = false;
    g_prevLeftStickClick     = false;
    }
    const bool prev = g_gameplayActive.exchange(gameplay, std::memory_order_relaxed);
    if (prev != gameplay) {
        LOG_INFO("UE3Bridge: gameplay state -> %s (map='%.*s')",
                 gameplay ? "IMMERSIVE GAMEPLAY" : "MENU/FLAT",
                 static_cast<int>(mapName.size()), mapName.data());
    }
    return gameplay;
}

struct SyntheticSelfTestGuard {
    SyntheticSelfTestGuard() { g_inSyntheticSelfTest.store(true, std::memory_order_release); }
    ~SyntheticSelfTestGuard() { g_inSyntheticSelfTest.store(false, std::memory_order_release); }
    SyntheticSelfTestGuard(const SyntheticSelfTestGuard&) = delete;
    SyntheticSelfTestGuard& operator=(const SyntheticSelfTestGuard&) = delete;
};

// Applies the Camera-Lock Suppressor directly to active camera anim slots & shake offsets on a PlayerController.
void SuppressActiveCameraShakesOnPC(UObject* pcObj) {
    if (!IsPlayerControllerInstance(pcObj)) return;
    const int32_t propSize = pcObj->Class->PropertiesSize;
    auto* raw = reinterpret_cast<uint8_t*>(pcObj);

    // Zero UDKPlayerController::ShakeOffset (+0x05A4) and ShakeRot (+0x05B0)
    if (propSize >= static_cast<int32_t>(offsets::kUDKPC_MinSize) &&
        HasSuperClassNamed(pcObj->Class, g_udkPcFnameIdx, "UDKPlayerController")) {
        auto* shakeOffset = reinterpret_cast<FVector*>(raw + offsets::kUDKPC_ShakeOffset);
        auto* shakeRot    = reinterpret_cast<FRotator*>(raw + offsets::kUDKPC_ShakeRot);
        *shakeOffset = FVector{0.0f, 0.0f, 0.0f};
        *shakeRot    = FRotator{0, 0, 0};
    }

    // Zero blend weights ONLY on ASAMUPlayerController's 6 CameraAnimInst* slots (+0x0874..+0x0888).
    // Checking ASAMUPlayerController inheritance prevents misreading UTEntryPlayerController (0x097C).
    if (propSize >= static_cast<int32_t>(offsets::kASAMUPC_MinSize) &&
        HasSuperClassNamed(pcObj->Class, g_asamuPcFnameIdx, "ASAMUPlayerController")) {
        auto** animSlots = reinterpret_cast<UObject**>(raw + offsets::kASAMUPC_CamAnimSlotsStart);
        for (size_t i = 0; i < offsets::kASAMUPC_CamAnimSlotCount; ++i) {
            UObject* animInst = animSlots[i];
            if (IsLiveUObject(animInst) &&
                animInst->Class->PropertiesSize >= static_cast<int32_t>(offsets::kCamAnimInst_MinSize) &&
                HasSuperClassNamed(animInst->Class, g_camAnimInstFnameIdx, "CameraAnimInst")) {
                auto* instRaw = reinterpret_cast<uint8_t*>(animInst);
                *reinterpret_cast<float*>(instRaw + offsets::kCamAnimInst_BasePlayScale)      = 0.0f;
                *reinterpret_cast<float*>(instRaw + offsets::kCamAnimInst_TransientScale)     = 0.0f;
                *reinterpret_cast<float*>(instRaw + offsets::kCamAnimInst_CurrentBlendWeight) = 0.0f;
            }
        }
    }
}

// ---- VR view construction (game thread) -------------------------------------------------------------------
// Live logs showed: Camera.UpdateCamera fires once per frame and ASAMUPlayerController.GetPlayerViewPoint ~4x per
// frame through ProcessEvent. The game's own camera location is the ground truth for "where the eyes are", so the
// VR view is: game camera location + roomscale head (or per-eye) offset, with the rotation replaced by
// body yaw + HMD orientation.

UObject* GetLivePawn(UObject* pcObj) {
    UObject* pawnObj = *reinterpret_cast<UObject* const*>(reinterpret_cast<uint8_t*>(pcObj) + offsets::kController_Pawn);
    if (!IsLiveUObject(pawnObj) ||
        pawnObj->Class->PropertiesSize < static_cast<int32_t>(offsets::kPawn_MinSize) ||
        !HasSuperClassNamed(pawnObj->Class, g_pawnFnameIdx, "Pawn")) {
        return nullptr;
    }
    return pawnObj;
}

// Offset from Pawn.Location to the game's first-person camera, learned from the game's own camera every frame.
FVector g_eyeFromPawn{0.0f, 0.0f, 0.0f};
bool    g_eyeFromPawnValid = false;
// The last view location we handed back (to recognise an already-adjusted location coming round again).
FVector g_lastAdjustedViewLoc{0.0f, 0.0f, 0.0f};
bool    g_lastAdjustedValid = false;
bool    g_lastViewWasPawn   = true;

// Where the player's eyes are before any head offset. Returns false if the PC has no live Pawn.
bool GetPawnEyeLocation(UObject* pcObj, FVector& out) {
    UObject* pawnObj = GetLivePawn(pcObj);
    if (!pawnObj) return false;
    const auto* pawnRaw = reinterpret_cast<const uint8_t*>(pawnObj);
    out = *reinterpret_cast<const FVector*>(pawnRaw + offsets::kActor_Location);
    if (g_eyeFromPawnValid) {
        out.X += g_eyeFromPawn.X;
        out.Y += g_eyeFromPawn.Y;
        out.Z += g_eyeFromPawn.Z;
    } else {
        out.Z += *reinterpret_cast<const float*>(pawnRaw + offsets::kPawn_EyeHeight);
    }
    return true;
}

// A game camera further than this from the pawn is a scripted / cinematic camera.
constexpr float kCinematicDistUU = 250.0f;

int32_t BodyYawFor(UObject* pcObj) {
    if (g_bodyYawValid && g_bodyYawPc == pcObj) return g_bodyYaw;
    return reinterpret_cast<const FRotator*>(reinterpret_cast<uint8_t*>(pcObj) + offsets::kActor_Rotation)->Yaw;
}

// Roomscale head (or, in AFR, per-eye) offset in world space for this frame.
FVector HeadOffsetWorld(int32_t bodyYaw) {
    const auto& cfg = samu::config::Get();
    const HmdUe3Pose hmd = GetFramePose();
    if (!hmd.positionValid) return FVector{};
    FVector local = hmd.offsetUU;
    if (g_frameEye >= 0 && hmd.eyesValid) {
        const FVector& e = hmd.eyeOffsetUU[g_frameEye];
        local = FVector{hmd.offsetUU.X + (e.X - hmd.offsetUU.X) * cfg.ipdScale,
                        hmd.offsetUU.Y + (e.Y - hmd.offsetUU.Y) * cfg.ipdScale,
                        hmd.offsetUU.Z + (e.Z - hmd.offsetUU.Z) * cfg.ipdScale};
    }
    return RotateVectorByYawURot(local, bodyYaw);
}

// Absolute VR view rotation: body yaw + HMD yaw, HMD pitch & roll.
FRotator VrViewRotation(int32_t bodyYaw, const FRotator& fallback) {
    const HmdUe3Pose hmd = GetFramePose();
    FRotator out = fallback;
    if (hmd.orientationValid) {
        out.Pitch = hmd.rotationURot.Pitch;
        out.Yaw   = (bodyYaw + hmd.rotationURot.Yaw) & 0xFFFF;
        out.Roll  = hmd.rotationURot.Roll;
    } else if (samu::config::IsCameraLockSuppressorEnabled() && samu::config::Get().suppressPitchRoll) {
        out.Roll = 0;
    }
    return out;
}

// Turns a view the GAME just computed (loc/rot) into the VR view, in place. Safe to call on a location that was
// already adjusted this frame (e.g. GetPlayerViewPoint returning the CameraCache we adjusted in UpdateCamera).
void AdjustGameView(UObject* pcObj, FVector& loc, FRotator& rot) {
    const int32_t bodyYaw = BodyYawFor(pcObj);

    if (g_lastAdjustedValid) {
        const float ax = loc.X - g_lastAdjustedViewLoc.X;
        const float ay = loc.Y - g_lastAdjustedViewLoc.Y;
        const float az = loc.Z - g_lastAdjustedViewLoc.Z;
        if (ax * ax + ay * ay + az * az < 0.01f) {
            if (g_lastViewWasPawn) rot = VrViewRotation(bodyYaw, rot);   // idempotent
            return;
        }
    }

    bool pawnView = true;
    if (UObject* pawnObj = GetLivePawn(pcObj)) {
        const FVector pawnLoc = *reinterpret_cast<const FVector*>(reinterpret_cast<const uint8_t*>(pawnObj) + offsets::kActor_Location);
        const FVector d{loc.X - pawnLoc.X, loc.Y - pawnLoc.Y, loc.Z - pawnLoc.Z};
        pawnView = (d.X * d.X + d.Y * d.Y + d.Z * d.Z) < kCinematicDistUU * kCinematicDistUU;
        if (pawnView) {
            if (!g_eyeFromPawnValid) {
                LOG_INFO("UE3Bridge: learned eye offset from pawn = (%.1f, %.1f, %.1f) UU from the game's camera", d.X, d.Y, d.Z);
            }
            g_eyeFromPawn      = d;
            g_eyeFromPawnValid = true;
        }
    }

    const FVector w = HeadOffsetWorld(bodyYaw);
    loc = FVector{loc.X + w.X, loc.Y + w.Y, loc.Z + w.Z};
    if (pawnView) rot = VrViewRotation(bodyYaw, rot);   // scripted cameras keep their own rotation
    g_lastAdjustedViewLoc = loc;
    g_lastAdjustedValid   = true;
    g_lastViewWasPawn     = pawnView;
    ++g_viewOverridesThisFrame;
}

float VrFovOr(float fallback) {
    const HmdUe3Pose hmd = GetFramePose();
    if (!(hmd.fovDeg > 30.0f && hmd.fovDeg < 170.0f)) return fallback;
    // Apply ViewScale symmetrically to the game camera FOV so GameFOV always matches OpenXR DisplayFOV (no skew).
    const float vs = std::clamp(samu::config::GetViewScale(), 0.50f, 1.50f);
    if (std::fabs(vs - 1.0f) < 1e-4f) return hmd.fovDeg;
    const float halfRad = hmd.fovDeg * 0.5f * 3.14159265358979323846f / 180.0f;
    return std::clamp(2.0f * std::atan(std::tan(halfRad) * vs) * 57.2957795f, 40.0f, 160.0f);
}

// Post-PlayerTick: record the body yaw the game just produced, then write the full head orientation into the
// PlayerController (the game derives view + aim from it) and publish the view for hands / aiming.
void Apply6DofCameraToPlayerController(UObject* pcObj) {
    if (!IsPlayerControllerInstance(pcObj)) return;

    const auto& cfg = samu::config::Get();
    if (!cfg.decoupleCamera) return;

    auto* raw = reinterpret_cast<uint8_t*>(pcObj);
    auto* pcRot = reinterpret_cast<FRotator*>(raw + offsets::kActor_Rotation);

    // PlayerTick just ran with the pure body yaw (restored in the pre-hook); pick up any stick/mouse turning.
    g_bodyYawPc    = pcObj;
    g_bodyYaw      = pcRot->Yaw;
    g_bodyYawValid = true;

    const HmdUe3Pose hmd = GetFramePose();
    const FRotator finalRot = VrViewRotation(g_bodyYaw, *pcRot);

    FVector eyeBase{};
    if (!GetPawnEyeLocation(pcObj, eyeBase)) {
        eyeBase = *reinterpret_cast<const FVector*>(raw + offsets::kActor_Location);
    }
    const FVector w = HeadOffsetWorld(g_bodyYaw);
    const FVector finalLoc{eyeBase.X + w.X, eyeBase.Y + w.Y, eyeBase.Z + w.Z};
    const float finalFov = VrFovOr(*reinterpret_cast<const float*>(raw + offsets::kPC_FOVAngle));

    *reinterpret_cast<FRotator*>(raw + offsets::kPC_TargetViewRotation)        = finalRot;
    *reinterpret_cast<FRotator*>(raw + offsets::kPC_BlendedTargetViewRotation) = finalRot;
    if (hmd.orientationValid) {
        int32_t hmdYaw = hmd.rotationURot.Yaw & 0xFFFF;
        if (hmdYaw > 32767) hmdYaw -= 65536;
        pcRot->Pitch = finalRot.Pitch;
        pcRot->Roll  = finalRot.Roll;
        pcRot->Yaw   = g_bodyYaw + hmdYaw;   // the pre-hook restores the body yaw before the next PlayerTick
    }
    g_lastWrittenYaw = pcRot->Yaw;

    {
        std::lock_guard lock(g_poseMutex);
        g_lastCameraDiag.baseLocation  = eyeBase;
        g_lastCameraDiag.hmdOffsetUU   = w;
        g_lastCameraDiag.finalLocation = finalLoc;
        g_lastCameraDiag.finalRotation = finalRot;
        g_lastCameraDiag.bodyYaw       = g_bodyYaw;
        g_lastCameraDiag.finalFovDeg   = finalFov;
        g_lastCameraDiag.renderEye     = g_frameEye;
        g_lastCameraDiag.active        = true;
    }
    g_cameraDecoupleUpdates.fetch_add(1, std::memory_order_relaxed);
}

// Computes a world-space 6DOF Hand Aim Pose (position, rotation, and unit aim direction)
// relative to the player's current eye/body origin and base yaw.
bool ComputeHandAimWorldPose(const HandUe3Pose& handPose,
                             const FVector& fallbackLocalOffset,
                             FVector& outWorldLoc,
                             FRotator& outWorldRot,
                             FVector& outWorldAimDir,
                             FVector& outRelOffsetUU,
                             const FVector* baseLocOverride = nullptr) {
    if (!samu::config::Get().enable6DofGrapple) return false;
    if (!handPose.positionValid && !handPose.orientationValid) return false;

    DecoupledCameraDiag camDiag = UE3Bridge::GetLastCameraDiag();
    FVector baseLoc  = baseLocOverride ? *baseLocOverride : camDiag.baseLocation;
    int32_t baseYaw  = camDiag.bodyYaw;

    outRelOffsetUU = handPose.offsetUU;
    FVector worldHandOffset = handPose.positionValid
                                  ? RotateVectorByYawURot(handPose.offsetUU, baseYaw)
                                  : RotateVectorByYawURot(fallbackLocalOffset, baseYaw);
    outWorldLoc = FVector{
        baseLoc.X + worldHandOffset.X,
        baseLoc.Y + worldHandOffset.Y,
        baseLoc.Z + worldHandOffset.Z
    };

    outWorldRot = camDiag.finalRotation;
    if (handPose.orientationValid) {
        outWorldRot.Pitch = handPose.rotationURot.Pitch;
        outWorldRot.Yaw   = (baseYaw + handPose.rotationURot.Yaw) & 0xFFFF;
        outWorldRot.Roll  = handPose.rotationURot.Roll;
        if (outWorldRot.Pitch > 32767) outWorldRot.Pitch -= 65536;
        if (outWorldRot.Yaw   > 32767) outWorldRot.Yaw   -= 65536;
        if (outWorldRot.Roll  > 32767) outWorldRot.Roll  -= 65536;
    }

    outWorldAimDir = RotatorToForwardVector(outWorldRot);
    return true;
}

bool ComputeRightAimWorldPose(FVector& outWorldLoc,
                              FRotator& outWorldRot,
                              FVector& outWorldAimDir,
                              FVector& outRelOffsetUU,
                              const FVector* baseLocOverride = nullptr) {
    const HandUe3Pose& handPose = (g_framePoseValid && g_frameRightAimPose.positionValid)
                                      ? g_frameRightAimPose
                                      : UE3Bridge::GetRightAimPose();
    return ComputeHandAimWorldPose(handPose, FVector{20.0f, 12.0f, -10.0f},
                                   outWorldLoc, outWorldRot, outWorldAimDir, outRelOffsetUU, baseLocOverride);
}

bool ComputeLeftAimWorldPose(FVector& outWorldLoc,
                             FRotator& outWorldRot,
                             FVector& outWorldAimDir,
                             FVector& outRelOffsetUU) {
    return ComputeHandAimWorldPose(UE3Bridge::GetLeftAimPose(), FVector{20.0f, -12.0f, -10.0f},
                                   outWorldLoc, outWorldRot, outWorldAimDir, outRelOffsetUU);
}

UFunction* FindFunction(const char* fnName, const char* outerName);
bool ComputeRightAimWorldPose(FVector& outWorldLoc, FRotator& outWorldRot, FVector& outWorldAimDir, FVector& outRelOffsetUU,
                              const FVector* baseLocOverride);

// ---- FRotator basis helpers (UE3 FRotationMatrix convention) ----
struct RotBasis { FVector x, y, z; };

RotBasis BasisFromRotator(const FRotator& r) {
    constexpr float k = 3.14159265358979323846f / 32768.0f;
    const float sp = std::sin(r.Pitch * k), cp = std::cos(r.Pitch * k);
    const float sy = std::sin(r.Yaw * k),   cy = std::cos(r.Yaw * k);
    const float sr = std::sin(r.Roll * k),  cr = std::cos(r.Roll * k);
    RotBasis b;
    b.x = FVector{cp * cy, cp * sy, sp};
    b.y = FVector{sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp};
    b.z = FVector{-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp};
    return b;
}

FVector RotateByBasis(const RotBasis& b, const FVector& v) {   // local -> world
    return FVector{v.X * b.x.X + v.Y * b.y.X + v.Z * b.z.X,
                   v.X * b.x.Y + v.Y * b.y.Y + v.Z * b.z.Y,
                   v.X * b.x.Z + v.Y * b.y.Z + v.Z * b.z.Z};
}

FVector UnrotateByBasis(const RotBasis& b, const FVector& d) { // world -> local
    return FVector{d.X * b.x.X + d.Y * b.x.Y + d.Z * b.x.Z,
                   d.X * b.y.X + d.Y * b.y.Y + d.Z * b.y.Z,
                   d.X * b.z.X + d.Y * b.z.Y + d.Z * b.z.Z};
}

RotBasis TransposeBasis(const RotBasis& b) {
    return RotBasis{
        FVector{b.x.X, b.y.X, b.z.X},
        FVector{b.x.Y, b.y.Y, b.z.Y},
        FVector{b.x.Z, b.y.Z, b.z.Z}
    };
}

RotBasis MultiplyBasis(const RotBasis& a, const RotBasis& b) {
    return RotBasis{
        RotateByBasis(a, b.x),
        RotateByBasis(a, b.y),
        RotateByBasis(a, b.z)
    };
}

FRotator BasisToRotator(const RotBasis& b) {
    constexpr float kRadToURot = 32768.0f / 3.14159265358979323846f;
    const float cp = std::sqrt(b.x.X * b.x.X + b.x.Y * b.x.Y);
    FRotator r{};
    r.Pitch = static_cast<int32_t>(std::lround(std::atan2(b.x.Z, cp) * kRadToURot));
    if (cp > 1e-4f) {
        r.Yaw  = static_cast<int32_t>(std::lround(std::atan2(b.x.Y, b.x.X) * kRadToURot));
        r.Roll = static_cast<int32_t>(std::lround(std::atan2(-b.y.Z, b.z.Z) * kRadToURot));
    } else {
        r.Yaw  = static_cast<int32_t>(std::lround(std::atan2(-b.y.X, b.y.Y) * kRadToURot));
        r.Roll = 0;
    }
    return r;
}

FVector CrossVec(const FVector& a, const FVector& b) {
    return FVector{a.Y * b.Z - a.Z * b.Y, a.Z * b.X - a.X * b.Z, a.X * b.Y - a.Y * b.X};
}

bool NormalizeVec(FVector& v) {
    const float len = std::sqrt(v.X * v.X + v.Y * v.Y + v.Z * v.Z);
    if (len < 1e-4f) return false;
    v.X /= len; v.Y /= len; v.Z /= len;
    return true;
}

// ---- VR hand: drive the glove's visible mesh component ----
// Scales first-person arm meshes in sync with WorldScale so changing world scale preserves physical hand size/reach.
float GetDesiredHandMeshScale() {
    const float ws = samu::config::GetWorldScale();
    return std::clamp((ws > 1.0f ? ws : 72.0f) / 72.0f, 0.45f, 2.0f);
}

// Shifts the glove palm slightly down and back from the OpenXR aim emitter tip so the in-game glove sits
// where your real palm/knuckles grip the controller, while keeping the aim ray itself on the pointer tip.
FVector ApplyVisualHandErgonomicOffset(const FVector& aimLoc, const FRotator& handRot, float worldScale) {
    const float ws = (worldScale > 1.0f) ? worldScale : 72.0f;
    const RotBasis b = BasisFromRotator(handRot);
    const float backUU      = -0.025f * ws; // ~2.5 cm back toward wrist
    const float localDownUU = -0.038f * ws; // ~3.8 cm down controller-local vertical
    const float worldDownUU = -0.022f * ws; // ~2.2 cm world-down (aim pose sits above Touch grip)
    return FVector{
        aimLoc.X + b.x.X * backUU + b.z.X * localDownUU,
        aimLoc.Y + b.x.Y * backUU + b.z.Y * localDownUU,
        aimLoc.Z + b.x.Z * backUU + b.z.Z * localDownUU + worldDownUU
    };
}

// The visible hand is GrappleGun.Mesh. The game re-attaches it to the view every frame, so moving the weapon
// ACTOR has no visible effect. Instead the mesh component is switched to absolute translation/rotation and
// placed so that the glove's beam socket (the palm) sits on the right controller.
struct HandRig {
    bool       fnsResolved      = false;
    bool       usable           = false;
    UFunction* fnSetTranslation = nullptr;
    UFunction* fnSetRotation    = nullptr;
    UFunction* fnSetScale       = nullptr;   // PrimitiveComponent.SetScale(float NewScale)
    UFunction* fnSetAbsolute    = nullptr;
    UFunction* fnGetSocket      = nullptr;
    UProperty* propFallingBlend = nullptr;   // GrappleGun.AnimIsFallingNode (AnimNodeBlend)
    UProperty* propChild2Target = nullptr;   // AnimNodeBlend.Child2WeightTarget (FloatProperty)
    UFunction* fnSetBlendTarget = nullptr;   // AnimNodeBlend.SetBlendTarget(BlendTarget, BlendTime)
    UObject*   mesh             = nullptr;   // mesh the state below belongs to
    int        learnFrames      = 0;
    bool       learned          = false;
    FVector    socketLocal{};                // palm socket position in the mesh's own frame (at appliedScale)
    float      appliedScale     = 1.0f;
    bool       havePrev         = false;
    uint64_t   lastLearnFrame   = 0;
    FVector    prevHand{};                   // previous visual palm target (used for Phase 2 closed-loop)
    FRotator   prevRot{};
    int64_t    lastLogMs        = 0;
} g_handRig;

// ---- Script-side aim ----
// The game's own code (GrappleGun.CheckForRange / InstantFire) traces from the pawn's eye along
// PlayerController.GetPlayerViewPoint(), which in script simply returns PlayerCamera.CameraCache.POV. The
// renderer asks through ProcessEvent instead and is answered with the head view, so CameraCache.POV.Rotation is
// free to hold the HAND's aim: we trace the hand ray ourselves and point the game's eye ray at what it hits.
struct AimRig {
    bool       resolved    = false;
    UFunction* fnTrace     = nullptr;     // Actor.Trace
    UProperty* propMaxDist = nullptr;     // GrappleGun.fMaxDistance
    UProperty* propGrappleLoc = nullptr;  // GrappleGun.vGrappleLocation
    UObject*   gun         = nullptr;     // live GrappleGun (from its SetPosition event)
    UObject*   camera      = nullptr;     // live PlayerCamera (from its UpdateCamera event)
    FVector    hand{};
    FVector    target{};
    FRotator   aimRot{};
    bool       hit         = false;
    float      hitDistUU   = 0.0f;
    int64_t    lastLogMs   = 0;
} g_aimRig;
std::atomic<float> g_reticleDistUU{1500.0f};
std::atomic<bool>  g_reticleLit{false};

// The game's HUD: a Scaleform movie whose crosshair clip sits in the middle of the (head-locked) picture.
struct HudRig {
    bool       resolved        = false;
    UProperty* propMyHud       = nullptr;
    UProperty* propHudMovie    = nullptr;
    UProperty* propCrosshairMC = nullptr;
    UProperty* propPopupsMC    = nullptr;      // ASAMUHUDMovie.PopupsMC (GFxObject)
    UProperty* propPopupTF     = nullptr;      // ASAMUHUDMovie.PopupTF (GFxClikWidget)
    UBoolProperty* propMovieHidden = nullptr;  // ASAMUHUDMovie.bHidden
    UBoolProperty* propPauseFade   = nullptr;  // ASAMUHUD.bPauseMenuFade
    UProperty*     propPauseMovie  = nullptr;  // ASAMUHUD.PauseMenuASAMU
    UBoolProperty* propMovieIsOpen = nullptr;  // GFxMoviePlayer.bMovieIsOpen
    UProperty* propCrosshairState = nullptr;   // ASAMUHUD.CurrentCrosshair (enum CrosshairTypes)
    UFunction* fnSetVisible    = nullptr;      // GFxObject.SetVisible
    UFunction* fnGetPosition   = nullptr;      // GFxObject.GetPosition(out float x, out float y) -> bool
    UFunction* fnSetPosition   = nullptr;      // GFxObject.SetPosition(float x, float y)
    UFunction* fnGetFloat      = nullptr;      // GFxObject.GetFloat(string Member) -> float
    UFunction* fnSetFloat      = nullptr;      // GFxObject.SetFloat(string Member, float f)
    UFunction* fnGetString     = nullptr;      // GFxObject.GetString(string Member) -> string
    UFunction* fnSetString     = nullptr;      // GFxObject.SetString(string Member, string s, optional Context)
    UObject*   lastHud         = nullptr;
    UObject*   lastClip        = nullptr;
    UObject*   lastPopup       = nullptr;
    UObject*   scaledSubtitleFont = nullptr;
    float      origSubtitleScale  = 1.0f;
    float      basePopupX      = 0.0f;
    float      basePopupY      = 0.0f;
    float      basePopupW      = 0.0f;
    bool       haveBasePopupPos = false;
    bool       popupScaled      = false;
    float      lastAppliedLift = -1.0f;
    int        lastWanted      = -1;
    int64_t    lastAssertMs    = 0;
    int64_t    lastResolveMs   = 0;
    int64_t    lastPopupPollMs = 0;
    std::wstring lastVrPopupText;
} g_hudRig;

UProperty* FindPropertyInClassChain(UStruct* cls, const char* name, const char* propClass);
void FixHandMeshFov(UObject* gun, UObject* mesh);

// ---- Tracked 6DOF Left Hand ----
// Spawns an independent collision-free SkeletalMeshActorSpawnable using GrappleGun.Mesh's skeletal mesh
// and materials, hides the right-arm & belt bones so only LowerArm_L..MiddleFinger_L_03 remain visible,
// and drives its world position & orientation from LeftAimPose each frame.
struct LeftHandRig {
    bool       fnsResolved     = false;
    bool       usable          = false;
    bool       failedOnce      = false;
    UClass*    clsSpawnable    = nullptr;
    UFunction* fnSpawn         = nullptr;
    UFunction* fnDestroy       = nullptr;
    UFunction* fnSetCollision  = nullptr;
    UFunction* fnSetLocation   = nullptr;
    UFunction* fnSetSkelMesh   = nullptr;
    UFunction* fnGetBoneLoc    = nullptr;
    UFunction* fnGetBoneName   = nullptr;
    UFunction* fnHideByName    = nullptr;
    UFunction* fnGetNumElems   = nullptr;
    UFunction* fnGetMaterial   = nullptr;
    UFunction* fnSetMaterial   = nullptr;
    UFunction* fnSetHidden     = nullptr;
    UObject*   actor           = nullptr;
    UObject*   mesh            = nullptr;
    UObject*   sourceRightMesh = nullptr;
    bool       hidden          = false;
    float      appliedScale    = 1.0f;
    FVector    palmLocal{32.0f, -13.6f, -9.1f};
    RotBasis   invLocalArm{FVector{1, 0, 0}, FVector{0, 1, 0}, FVector{0, 0, 1}};
    int64_t    lastLogMs       = 0;
} g_leftHandRig;

UClass* FindClassByName(const char* name) {
    if (!g_gobjects || !g_gobjects->Data) return nullptr;
    for (int32_t i = 0; i < g_gobjects->Num; ++i) {
        UObject* obj = g_gobjects->Data[i];
        if (!obj || !obj->Class) continue;
        if (SdkDumper::NameToString(g_gnames, obj->Name) != name) continue;
        if (SdkDumper::GetObjClassName(g_gnames, obj) == "Class") return static_cast<UClass*>(obj);
    }
    return nullptr;
}

bool HasSideTag(const std::string& raw, const char* longWord, const char* prefixDash,
                const char* midTag, const char* spaceTag, const char* dotTag, const char* suffixUnder,
                const char* suffixDot, const char* suffixSpace) {
    std::string n(raw);
    std::transform(n.begin(), n.end(), n.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (n.find(longWord) != std::string::npos) return true;
    if (n.rfind(prefixDash, 0) == 0 || n.rfind(std::string(1, prefixDash[0]) + "_", 0) == 0) return true;
    if (n.find(midTag) != std::string::npos || n.find(spaceTag) != std::string::npos || n.find(dotTag) != std::string::npos) return true;
    const size_t len = n.size();
    return len >= 2 && (n.compare(len - 2, 2, suffixUnder) == 0 ||
                        n.compare(len - 2, 2, suffixDot) == 0 ||
                        n.compare(len - 2, 2, suffixSpace) == 0);
}

// True if a bone name looks like it belongs to the LEFT side of a character rig.
bool IsLeftSideBoneName(const std::string& raw) {
    return HasSideTag(raw, "left", "l-", "_l_", " l ", ".l.", "_l", ".l", " l") ||
           raw.find("lft") != std::string::npos || raw.find("Lft") != std::string::npos;
}

// True for right-side bones or central belt bone that must stay hidden on the dedicated left-hand mesh.
bool IsRightOrBeltBoneName(const std::string& raw) {
    std::string lower(raw);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "belt") return true;
    return HasSideTag(raw, "right", "r-", "_r_", " r ", ".r.", "_r", ".r", " r") ||
           lower.find("rgt") != std::string::npos;
}

void DestroyLeftHandActor() {
    LeftHandRig& lr = g_leftHandRig;
    if (lr.actor && IsLiveUObject(lr.actor) && lr.fnDestroy && lr.fnDestroy->ParmsSize == 0x04 && g_origProcessEvent) {
        struct { uint32_t ReturnValue; } p{0};
        g_origProcessEvent(lr.actor, lr.fnDestroy, &p, nullptr);
        LOG_INFO("UE3Bridge: destroyed spawned left-hand actor %s", SdkDumper::GetFullObjectPath(g_gnames, lr.actor).c_str());
    }
    lr.actor           = nullptr;
    lr.mesh            = nullptr;
    lr.sourceRightMesh = nullptr;
    lr.hidden          = false;
    lr.failedOnce      = false;
    lr.appliedScale    = 1.0f;
}

bool QueryBoneLocalLocation(UObject* mesh, UFunction* fnGetBoneLoc, const FName& boneName, FVector& outLoc) {
    if (!fnGetBoneLoc || fnGetBoneLoc->ParmsSize != 0x18 || boneName.Index == 0) return false;
    struct {
        FName   BoneName;     // +0x00
        int32_t Space;        // +0x08 (1 = Local Component)
        FVector ReturnValue;  // +0x0C
    } p{boneName, 1, FVector{}};
    static_assert(sizeof(p) == 0x18, "GetBoneLocation parms");
    g_origProcessEvent(mesh, fnGetBoneLoc, &p, nullptr);
    outLoc = p.ReturnValue;
    return (outLoc.X * outLoc.X + outLoc.Y * outLoc.Y + outLoc.Z * outLoc.Z) > 1e-4f;
}

void SetLeftHandHidden(bool hide) {
    LeftHandRig& lr = g_leftHandRig;
    if (!lr.fnSetHidden || !IsLiveUObject(lr.mesh) || lr.hidden == hide) return;
    if (lr.fnSetHidden->ParmsSize != 0x01 && lr.fnSetHidden->ParmsSize != 0x04) return;
    uint32_t val = hide ? 1u : 0u;
    g_origProcessEvent(lr.mesh, lr.fnSetHidden, &val, nullptr);
    lr.hidden = hide;
}

void UpdateLeftHandVisual(UObject* weaponObj, UObject* rightMesh, const HandRig& rightRig) {
    LeftHandRig& lr = g_leftHandRig;
    if (!lr.fnsResolved) {
        lr.fnsResolved     = true;
        lr.clsSpawnable    = FindClassByName("SkeletalMeshActorSpawnable");
        lr.fnSpawn         = FindFunction("Spawn", "Actor");
        lr.fnDestroy       = FindFunction("Destroy", "Actor");
        lr.fnSetCollision  = FindFunction("SetCollision", "Actor");
        lr.fnSetLocation   = FindFunction("SetLocation", "Actor");
        lr.fnSetSkelMesh   = FindFunction("SetSkeletalMesh", "SkeletalMeshComponent");
        lr.fnGetBoneLoc    = FindFunction("GetBoneLocation", "SkeletalMeshComponent");
        lr.fnGetBoneName   = FindFunction("GetBoneName", "SkeletalMeshComponent");
        lr.fnHideByName    = FindFunction("HideBoneByName", "SkeletalMeshComponent");
        lr.fnGetNumElems   = FindFunction("GetNumElements", "MeshComponent");
        lr.fnGetMaterial   = FindFunction("GetMaterial", "MeshComponent");
        lr.fnSetMaterial   = FindFunction("SetMaterial", "MeshComponent");
        lr.fnSetHidden     = FindFunction("SetHidden", "PrimitiveComponent");
        lr.usable = lr.clsSpawnable && lr.fnSpawn && lr.fnSpawn->ParmsSize == 0x34 &&
                    lr.fnSetSkelMesh && lr.fnSetSkelMesh->ParmsSize >= 0x05 && lr.fnSetSkelMesh->ParmsSize <= 0x08 &&
                    lr.fnGetBoneName && lr.fnGetBoneName->ParmsSize == 0x0C &&
                    lr.fnHideByName && (lr.fnHideByName->ParmsSize == 0x09 || lr.fnHideByName->ParmsSize == 0x0C);
        LOG_INFO("UE3Bridge: left-hand rig functions: Spawnable=%p Spawn=%p(0x%X) SetSkelMesh=%p(0x%X) GetBoneLoc=%p(0x%X) -> %s",
                 static_cast<void*>(lr.clsSpawnable),
                 static_cast<void*>(lr.fnSpawn), lr.fnSpawn ? lr.fnSpawn->ParmsSize : 0,
                 static_cast<void*>(lr.fnSetSkelMesh), lr.fnSetSkelMesh ? lr.fnSetSkelMesh->ParmsSize : 0,
                 static_cast<void*>(lr.fnGetBoneLoc), lr.fnGetBoneLoc ? lr.fnGetBoneLoc->ParmsSize : 0,
                 lr.usable ? "usable" : "NOT usable");
    }
    if (!lr.usable) return;

    FVector  leftLoc{};
    FRotator leftRot{};
    FVector  leftDir{};
    FVector  leftRelOffset{};
    const bool leftTracked = ComputeLeftAimWorldPose(leftLoc, leftRot, leftDir, leftRelOffset);

    // Respawn cleanly if the weapon mesh changed or actor was garbage collected.
    if (lr.actor && (!IsLiveUObject(lr.actor) || !IsLiveUObject(lr.mesh) || lr.sourceRightMesh != rightMesh)) {
        DestroyLeftHandActor();
    }

    if (!lr.actor) {
        if (lr.failedOnce || !leftTracked) return;
        UProperty* propSrcSkel = FindPropertyInClassChain(rightMesh->Class, "SkeletalMesh", "ObjectProperty");
        UObject* srcSkel = propSrcSkel ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(rightMesh) + propSrcSkel->Offset) : nullptr;
        if (!IsLiveUObject(srcSkel)) {
            lr.failedOnce = true;
            LOG_WARN("UE3Bridge: left-hand setup aborted (source SkeletalMesh missing on %s)",
                     SdkDumper::GetFullObjectPath(g_gnames, rightMesh).c_str());
            return;
        }

        struct {
            UClass*  SpawnClass;       // +0x00
            UObject* SpawnOwner;       // +0x04
            FName    SpawnTag;         // +0x08
            FVector  SpawnLocation;    // +0x10
            FRotator SpawnRotation;    // +0x1C
            UObject* ActorTemplate;    // +0x28
            uint32_t bNoCollisionFail; // +0x2C
            UObject* ReturnValue;      // +0x30
        } sp{};
        static_assert(sizeof(sp) == 0x34, "Actor.Spawn parms");
        sp.SpawnClass       = lr.clsSpawnable;
        sp.SpawnLocation    = leftLoc;
        sp.SpawnRotation    = leftRot;
        sp.bNoCollisionFail = 1;
        g_origProcessEvent(weaponObj, lr.fnSpawn, &sp, nullptr);
        if (!IsLiveUObject(sp.ReturnValue)) {
            lr.failedOnce = true;
            LOG_WARN("UE3Bridge: Actor.Spawn(SkeletalMeshActorSpawnable) returned null; left hand disabled for this map.");
            return;
        }
        lr.actor = sp.ReturnValue;
        lr.sourceRightMesh = rightMesh;

        if (lr.fnSetCollision && lr.fnSetCollision->ParmsSize == 0x0C) {
            struct { uint32_t col, blk, ign; } cp{0, 0, 1};
            g_origProcessEvent(lr.actor, lr.fnSetCollision, &cp, nullptr);
        }

        UProperty* propComp = FindPropertyInClassChain(lr.actor->Class, "SkeletalMeshComponent", "ComponentProperty");
        lr.mesh = propComp ? *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(lr.actor) + propComp->Offset) : nullptr;
        if (!IsLiveUObject(lr.mesh)) {
            LOG_WARN("UE3Bridge: spawned left-hand actor lacks SkeletalMeshComponent; destroying.");
            DestroyLeftHandActor();
            lr.failedOnce = true;
            return;
        }

        struct { UObject* NewMesh; uint32_t bKeepSpaceBases; } sm{srcSkel, 0};
        g_origProcessEvent(lr.mesh, lr.fnSetSkelMesh, &sm, nullptr);

        // Copy materials from right-hand mesh so glove / plates match player progression.
        int copiedMats = 0;
        if (lr.fnGetMaterial && lr.fnGetMaterial->ParmsSize == 0x08 &&
            lr.fnSetMaterial && lr.fnSetMaterial->ParmsSize == 0x08) {
            int numElems = 4;
            if (lr.fnGetNumElems && lr.fnGetNumElems->ParmsSize == 0x04) {
                struct { int32_t ReturnValue; } np{0};
                g_origProcessEvent(rightMesh, lr.fnGetNumElems, &np, nullptr);
                if (np.ReturnValue > 0 && np.ReturnValue <= 16) numElems = np.ReturnValue;
            }
            for (int i = 0; i < numElems; ++i) {
                struct { int32_t ElementIndex; UObject* ReturnValue; } gp{i, nullptr};
                g_origProcessEvent(rightMesh, lr.fnGetMaterial, &gp, nullptr);
                if (IsLiveUObject(gp.ReturnValue)) {
                    struct { int32_t ElementIndex; UObject* Material; } mp{i, gp.ReturnValue};
                    g_origProcessEvent(lr.mesh, lr.fnSetMaterial, &mp, nullptr);
                    ++copiedMats;
                }
            }
        }

        // Hide right-arm and belt bones on the left mesh, and capture bone names to measure ref-pose palm offset.
        FName fnHandL{}, fnArmL{}, fnThumbL{}, fnPinkyL{};
        int hiddenCount = 0;
        for (int i = 0; i < 256; ++i) {
            struct { int32_t BoneIndex; FName ReturnValue; } bp{i, FName{}};
            g_origProcessEvent(lr.mesh, lr.fnGetBoneName, &bp, nullptr);
            if (bp.ReturnValue.Index == 0) break;
            const std::string bname = SdkDumper::NameToString(g_gnames, bp.ReturnValue);
            if (bname == "Hand_L")            fnHandL  = bp.ReturnValue;
            else if (bname == "LowerArm_L")   fnArmL   = bp.ReturnValue;
            else if (bname == "Thumb_L_01")   fnThumbL = bp.ReturnValue;
            else if (bname == "PinkyFinger_L_01") fnPinkyL = bp.ReturnValue;
            if (!IsRightOrBeltBoneName(bname)) continue;
            struct { FName BoneName; uint8_t PhysBodyOption; uint8_t Pad[3]; } hp{bp.ReturnValue, 0, {0, 0, 0}};
            g_origProcessEvent(lr.mesh, lr.fnHideByName, &hp, nullptr);
            ++hiddenCount;
        }

        // Mirror right palm offset as safe baseline, then refine from the actual Hand_L bind-pose bone location.
        const float invRightS = (rightRig.appliedScale > 0.1f) ? (1.0f / rightRig.appliedScale) : 1.0f;
        lr.palmLocal   = FVector{rightRig.socketLocal.X * invRightS, -rightRig.socketLocal.Y * invRightS, rightRig.socketLocal.Z * invRightS};
        lr.appliedScale = 1.0f;
        lr.invLocalArm = RotBasis{FVector{1, 0, 0}, FVector{0, 1, 0}, FVector{0, 0, 1}};
        FVector pHand{}, pArm{}, pThumb{}, pPinky{};
        const bool gotHand = QueryBoneLocalLocation(lr.mesh, lr.fnGetBoneLoc, fnHandL, pHand);
        const bool gotArm  = QueryBoneLocalLocation(lr.mesh, lr.fnGetBoneLoc, fnArmL, pArm);
        if (gotHand) lr.palmLocal = pHand;
        if (gotHand && gotArm) {
            FVector fwd{pHand.X - pArm.X, pHand.Y - pArm.Y, pHand.Z - pArm.Z};
            if (NormalizeVec(fwd)) {
                FVector upHint{0.0f, 0.0f, 1.0f};
                if (fwd.X < 0.70f &&
                    QueryBoneLocalLocation(lr.mesh, lr.fnGetBoneLoc, fnThumbL, pThumb) &&
                    QueryBoneLocalLocation(lr.mesh, lr.fnGetBoneLoc, fnPinkyL, pPinky)) {
                    FVector span{pThumb.X - pPinky.X, pThumb.Y - pPinky.Y, pThumb.Z - pPinky.Z};
                    if (NormalizeVec(span)) upHint = span;
                } else if (std::fabs(fwd.Z) >= 0.90f) {
                    upHint = FVector{1.0f, 0.0f, 0.0f};
                }
                FVector right = CrossVec(upHint, fwd);
                if (!NormalizeVec(right)) {
                    right = CrossVec(FVector{0.0f, 1.0f, 0.0f}, fwd);
                    NormalizeVec(right);
                }
                const FVector up = CrossVec(fwd, right);
                lr.invLocalArm = TransposeBasis(RotBasis{fwd, right, up});
            }
        }
        lr.hidden = false;
        LOG_INFO("UE3Bridge: spawned left-hand %s | hiddenBones=%d mats=%d | handL=(%.1f,%.1f,%.1f) armL=(%.1f,%.1f,%.1f) (got=%d/%d)",
                 SdkDumper::GetFullObjectPath(g_gnames, lr.actor).c_str(), hiddenCount, copiedMats,
                 pHand.X, pHand.Y, pHand.Z, pArm.X, pArm.Y, pArm.Z, gotHand ? 1 : 0, gotArm ? 1 : 0);
    }

    if (!leftTracked) {
        SetLeftHandHidden(true);
        return;
    }
    SetLeftHandHidden(false);

    const float desiredScale = GetDesiredHandMeshScale();
    if (rightRig.fnSetScale && std::fabs(desiredScale - lr.appliedScale) > 0.002f) {
        float sParm = desiredScale;
        g_origProcessEvent(lr.mesh, rightRig.fnSetScale, &sParm, nullptr);
        lr.appliedScale = desiredScale;
    }

    const FVector  visualLeftLoc = ApplyVisualHandErgonomicOffset(leftLoc, leftRot, samu::config::GetWorldScale());
    const RotBasis meshBasis = MultiplyBasis(BasisFromRotator(leftRot), lr.invLocalArm);
    const FRotator meshRot   = BasisToRotator(meshBasis);
    const FVector  rawOff    = RotateByBasis(meshBasis, lr.palmLocal);
    const FVector  off{rawOff.X * lr.appliedScale, rawOff.Y * lr.appliedScale, rawOff.Z * lr.appliedScale};
    const FVector  meshLoc{visualLeftLoc.X - off.X, visualLeftLoc.Y - off.Y, visualLeftLoc.Z - off.Z};

    if (lr.fnSetLocation && lr.fnSetLocation->ParmsSize == 0x10) {
        struct { FVector Loc; uint32_t Ret; } lp{visualLeftLoc, 0};
        g_origProcessEvent(lr.actor, lr.fnSetLocation, &lp, nullptr);
    }
    struct { uint32_t bTranslation, bRotation, bScale; } absParms{1, 1, 0};
    g_origProcessEvent(lr.mesh, rightRig.fnSetAbsolute, &absParms, nullptr);
    FVector  tParm = meshLoc;
    FRotator rParm = meshRot;
    g_origProcessEvent(lr.mesh, rightRig.fnSetTranslation, &tParm, nullptr);
    g_origProcessEvent(lr.mesh, rightRig.fnSetRotation, &rParm, nullptr);

    const int64_t nowMs = SteadyNowMs();
    if (nowMs - lr.lastLogMs > 5000) {
        lr.lastLogMs = nowMs;
        LOG_INFO("UE3Bridge: left hand: controller=(%.0f, %.0f, %.0f) meshOrigin=(%.0f, %.0f, %.0f) rot=(P=%d Y=%d R=%d)",
                 leftLoc.X, leftLoc.Y, leftLoc.Z, meshLoc.X, meshLoc.Y, meshLoc.Z,
                 meshRot.Pitch, meshRot.Yaw, meshRot.Roll);
    }
}

// True if a bone name looks like it belongs to the LEFT side of a character rig.

// The first-person arms are one skeletal mesh holding BOTH arms. It is now positioned by the right palm, so
// the left arm would hang at a fixed offset beside the right hand. Until the left arm gets its own tracked
// copy, hide its bones. Also logs the whole skeleton so the rig can be refined.
void HideLeftArmBones(UObject* mesh) {
    UFunction* fnGetBoneName = FindFunction("GetBoneName", "SkeletalMeshComponent");
    UFunction* fnHideByName  = FindFunction("HideBoneByName", "SkeletalMeshComponent");
    LOG_INFO("UE3Bridge: skeleton functions: GetBoneName=%p(0x%X) HideBoneByName=%p(0x%X)",
             static_cast<void*>(fnGetBoneName), fnGetBoneName ? fnGetBoneName->ParmsSize : 0,
             static_cast<void*>(fnHideByName), fnHideByName ? fnHideByName->ParmsSize : 0);
    if (!fnGetBoneName || fnGetBoneName->ParmsSize != 0x0C) return;

    struct BoneInfo { int index; FName name; std::string text; };
    std::vector<BoneInfo> bones;
    for (int i = 0; i < 256; ++i) {
        struct { int32_t BoneIndex; FName ReturnValue; } p{i, FName{}};
        static_assert(sizeof(p) == 0x0C, "GetBoneName parms");
        g_origProcessEvent(mesh, fnGetBoneName, &p, nullptr);
        if (p.ReturnValue.Index == 0) break;   // NAME_None: past the last bone
        bones.push_back(BoneInfo{i, p.ReturnValue, SdkDumper::NameToString(g_gnames, p.ReturnValue)});
    }

    std::string line;
    for (const BoneInfo& b : bones) {
        line += std::to_string(b.index) + ":" + b.text + " ";
        if (line.size() > 700) {
            LOG_INFO("UE3Bridge: glove skeleton: %s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOG_INFO("UE3Bridge: glove skeleton: %s", line.c_str());
    LOG_INFO("UE3Bridge: glove mesh %s has %zu bones", SdkDumper::GetFullObjectPath(g_gnames, mesh).c_str(), bones.size());

    if (!fnHideByName || (fnHideByName->ParmsSize != 0x09 && fnHideByName->ParmsSize != 0x0C)) return;
    int hidden = 0;
    for (const BoneInfo& b : bones) {
        if (!IsLeftSideBoneName(b.text)) continue;
        struct { FName BoneName; uint8_t PhysBodyOption; uint8_t Pad[3]; } p{b.name, 0 /* PBO_None */, {0, 0, 0}};
        static_assert(sizeof(p) == 0x0C, "HideBoneByName parms");
        g_origProcessEvent(mesh, fnHideByName, &p, nullptr);
        ++hidden;
        if (hidden <= 6) LOG_INFO("UE3Bridge: hiding left-arm bone %d '%s'", b.index, b.text.c_str());
    }
    LOG_INFO("UE3Bridge: left arm: %d bone(s) hidden%s", hidden,
             hidden ? "" : " (no left-side bone names recognised; see the skeleton list above)");
}

bool QuerySocketWorld(UObject* mesh, const FName& socket, FVector& outLoc) {
    struct {
        FName    InSocketName;   // +0x00
        FVector  OutLocation;    // +0x08
        FRotator OutRotation;    // +0x14
        int32_t  Space;          // +0x20
        uint32_t ReturnValue;    // +0x24
    } p{};
    static_assert(sizeof(p) == 0x28, "GetSocketWorldLocationAndRotation parms");
    p.InSocketName = socket;
    g_origProcessEvent(mesh, g_handRig.fnGetSocket, &p, nullptr);
    outLoc = p.OutLocation;
    return p.ReturnValue != 0;
}

// Phase 4: places the glove's visible hand mesh on the right controller. Called from the GrappleGun::SetPosition
// post-hook (and re-synced in Camera::UpdateCamera so high-speed falls/landings have zero 1-frame eye/hand lag).
void Apply6DofGloveTransformToWeapon(UObject* weaponObj, UObject* holderOverride = nullptr) {
    if (!IsLiveUObject(weaponObj) || !g_origProcessEvent) return;
    const int32_t propSize = weaponObj->Class->PropertiesSize;
    if (propSize < static_cast<int32_t>(offsets::kGrappleGun_MinSize) ||
        !HasSuperClassNamed(weaponObj->Class, g_grappleGunFnameIdx, "GrappleGun")) return;

    HandRig& rig = g_handRig;
    if (!rig.fnsResolved) {
        rig.fnsResolved      = true;
        rig.fnSetTranslation = FindFunction("SetTranslation", "PrimitiveComponent");
        rig.fnSetRotation    = FindFunction("SetRotation", "PrimitiveComponent");
        rig.fnSetScale       = FindFunction("SetScale", "PrimitiveComponent");
        if (rig.fnSetScale && rig.fnSetScale->ParmsSize != 0x04) rig.fnSetScale = nullptr;
        rig.fnSetAbsolute    = FindFunction("SetAbsolute", "PrimitiveComponent");
        rig.fnGetSocket      = FindFunction("GetSocketWorldLocationAndRotation", "SkeletalMeshComponent");
        rig.propFallingBlend = FindPropertyInClassChain(weaponObj->Class, "AnimIsFallingNode", "ObjectProperty");
        rig.fnSetBlendTarget = FindFunction("SetBlendTarget", "AnimNodeBlend");
        if (rig.fnSetBlendTarget && rig.fnSetBlendTarget->ParmsSize != 0x08) rig.fnSetBlendTarget = nullptr;
        if (UClass* blendCls = FindClassByName("AnimNodeBlend")) {
            rig.propChild2Target = FindPropertyInClassChain(blendCls, "Child2WeightTarget", "FloatProperty");
        }
        rig.usable = rig.fnSetTranslation && rig.fnSetRotation && rig.fnSetAbsolute && rig.fnGetSocket &&
                     rig.fnSetTranslation->ParmsSize == 0x0C && rig.fnSetRotation->ParmsSize == 0x0C &&
                     rig.fnSetAbsolute->ParmsSize == 0x0C && rig.fnGetSocket->ParmsSize == 0x28;
        LOG_INFO("UE3Bridge: hand rig functions: SetTranslation=%p(0x%X) SetRotation=%p(0x%X) SetAbsolute=%p(0x%X) GetSocket=%p(0x%X) -> %s",
                 static_cast<void*>(rig.fnSetTranslation), rig.fnSetTranslation ? rig.fnSetTranslation->ParmsSize : 0,
                 static_cast<void*>(rig.fnSetRotation), rig.fnSetRotation ? rig.fnSetRotation->ParmsSize : 0,
                 static_cast<void*>(rig.fnSetAbsolute), rig.fnSetAbsolute ? rig.fnSetAbsolute->ParmsSize : 0,
                 static_cast<void*>(rig.fnGetSocket), rig.fnGetSocket ? rig.fnGetSocket->ParmsSize : 0,
                 rig.usable ? "usable" : "NOT usable (VR hand disabled)");
    }
    if (!rig.usable) return;

    auto* raw = reinterpret_cast<uint8_t*>(weaponObj);
    UObject* mesh = *reinterpret_cast<UObject* const*>(raw + offsets::kWeapon_Mesh);
    if (!IsLiveUObject(mesh) || !HasSuperClassNamed(mesh->Class, g_skelMeshCompFnameIdx, "SkeletalMeshComponent")) return;
    g_aimRig.gun = weaponObj;
    if (mesh != rig.mesh) {            // new level / new glove: start over
        DestroyLeftHandActor();
        rig.mesh           = mesh;
        rig.learnFrames    = 0;
        rig.learned        = false;
        rig.appliedScale   = 1.0f;
        rig.havePrev       = false;
        rig.lastLearnFrame = 0;
        FixHandMeshFov(weaponObj, mesh);
    } else if ((g_gameFrameIdx % 120u) == 0) {
        FixHandMeshFov(weaponObj, mesh);   // re-assert in case the game restores its weapon FOV
    }
    const float desiredScale = GetDesiredHandMeshScale();
    if (rig.fnSetScale && std::fabs(desiredScale - rig.appliedScale) > 0.002f) {
        float sParm = desiredScale;
        g_origProcessEvent(mesh, rig.fnSetScale, &sParm, nullptr);
        if (rig.learned && rig.appliedScale > 0.1f) {
            const float factor = desiredScale / rig.appliedScale;
            rig.socketLocal.X *= factor;
            rig.socketLocal.Y *= factor;
            rig.socketLocal.Z *= factor;
            rig.havePrev = false;
        }
        rig.appliedScale = desiredScale;
        LOG_INFO("UE3Bridge: hand mesh scale -> %.3f (WorldScale=%.1f)", desiredScale, samu::config::GetWorldScale());
    }
    const FName socket = *reinterpret_cast<const FName*>(raw + offsets::kGrappleGun_BeamSocketName);

    const DecoupledCameraDiag cam = UE3Bridge::GetLastCameraDiag();
    FVector socketWorld{};
    const bool socketOk = QuerySocketWorld(mesh, socket, socketWorld);

    if (!rig.learned) {
        // Phase 1: leave the game's placement alone for a few frames and measure where the palm socket sits
        // relative to the view. First-person arm meshes are authored around the camera, so this is (close to)
        // the socket's position in the mesh's own frame; phase 2 refines it.
        if (socketOk && cam.active && ++rig.learnFrames >= 20) {
            const FVector d{socketWorld.X - cam.finalLocation.X, socketWorld.Y - cam.finalLocation.Y, socketWorld.Z - cam.finalLocation.Z};
            rig.socketLocal = UnrotateByBasis(BasisFromRotator(cam.finalRotation), d);
            rig.learned = true;
            HideLeftArmBones(mesh);
            LOG_INFO("UE3Bridge: hand rig learned: palm socket '%s' sits at (%.1f, %.1f, %.1f) UU in view space",
                     SdkDumper::NameToString(g_gnames, socket).c_str(), rig.socketLocal.X, rig.socketLocal.Y, rig.socketLocal.Z);
        }
        return;
    }

    // Keep the flat-screen free-fall arm-flailing blend disabled in 6DOF VR so big jumps don't twist/shake the hand
    // away from your tracked controller (grappling, power-jump & idle animations stay intact).
    if (rig.propFallingBlend && rig.propChild2Target && rig.fnSetBlendTarget) {
        UObject* blendNode = *reinterpret_cast<UObject* const*>(raw + rig.propFallingBlend->Offset);
        if (IsLiveUObject(blendNode)) {
            const float targetWeight = *reinterpret_cast<const float*>(
                reinterpret_cast<const uint8_t*>(blendNode) + rig.propChild2Target->Offset);
            if (targetWeight > 0.01f) {
                struct { float BlendTarget; float BlendTime; } bp{0.0f, 0.0f};
                g_origProcessEvent(blendNode, rig.fnSetBlendTarget, &bp, nullptr);
            }
        }
    }

    if (samu::config::Get().enableLeftHand) {
        UpdateLeftHandVisual(weaponObj, mesh, rig);
    } else {
        DestroyLeftHandActor();
    }

    FVector holderBaseLoc{};
    bool    useHolderBase = false;
    if (holderOverride && g_eyeFromPawnValid && IsLiveUObject(holderOverride) &&
        holderOverride->Class->PropertiesSize >= static_cast<int32_t>(offsets::kPawn_MinSize) &&
        HasSuperClassNamed(holderOverride->Class, g_pawnFnameIdx, "Pawn")) {
        const FVector pLoc = *reinterpret_cast<const FVector*>(
            reinterpret_cast<const uint8_t*>(holderOverride) + offsets::kActor_Location);
        holderBaseLoc = FVector{pLoc.X + g_eyeFromPawn.X, pLoc.Y + g_eyeFromPawn.Y, pLoc.Z + g_eyeFromPawn.Z};
        useHolderBase = true;
    }

    FVector  handLoc{};
    FRotator handRot{};
    FVector  handDir{};
    FVector  relOffset{};
    if (!ComputeRightAimWorldPose(handLoc, handRot, handDir, relOffset,
                                  useHolderBase ? &holderBaseLoc : nullptr)) return;   // controller not tracked: keep last pose

    const FVector visualHandLoc = ApplyVisualHandErgonomicOffset(handLoc, handRot, samu::config::GetWorldScale());

    // Phase 2 (closed loop, SetPosition pass only): refine socketLocal gently while practically stationary so
    // transient high-speed falls or landing animations can never pull the hand rig off-center.
    float errLen = 0.0f;
    if (rig.havePrev && socketOk && holderOverride != nullptr && rig.lastLearnFrame != g_gameFrameIdx) {
        rig.lastLearnFrame = g_gameFrameIdx;
        const FVector e{socketWorld.X - rig.prevHand.X, socketWorld.Y - rig.prevHand.Y, socketWorld.Z - rig.prevHand.Z};
        errLen = std::sqrt(e.X * e.X + e.Y * e.Y + e.Z * e.Z);
        const float mx = visualHandLoc.X - rig.prevHand.X, my = visualHandLoc.Y - rig.prevHand.Y, mz = visualHandLoc.Z - rig.prevHand.Z;
        if (errLen < 12.0f && (mx * mx + my * my + mz * mz) < 0.25f) {
            const FVector eLocal = UnrotateByBasis(BasisFromRotator(rig.prevRot), e);
            rig.socketLocal.X += eLocal.X * 0.35f;
            rig.socketLocal.Y += eLocal.Y * 0.35f;
            rig.socketLocal.Z += eLocal.Z * 0.35f;
        }
    }

    const FVector off = RotateByBasis(BasisFromRotator(handRot), rig.socketLocal);
    const FVector meshLoc{visualHandLoc.X - off.X, visualHandLoc.Y - off.Y, visualHandLoc.Z - off.Z};

    struct { uint32_t bTranslation, bRotation, bScale; } absParms{1, 1, 0};
    g_origProcessEvent(mesh, rig.fnSetAbsolute, &absParms, nullptr);
    FVector  tParm = meshLoc;
    FRotator rParm = handRot;
    g_origProcessEvent(mesh, rig.fnSetTranslation, &tParm, nullptr);
    g_origProcessEvent(mesh, rig.fnSetRotation, &rParm, nullptr);
    rig.prevHand = visualHandLoc;
    rig.prevRot  = handRot;
    rig.havePrev = true;

    const int64_t nowMs = SteadyNowMs();
    if (nowMs - rig.lastLogMs > 3000) {
        rig.lastLogMs = nowMs;
        LOG_INFO("UE3Bridge: hand rig: controller=(%.0f, %.0f, %.0f) palmSocket=(%.0f, %.0f, %.0f) err=%.1f UU socketOk=%d | "
                 "socketLocal=(%.1f, %.1f, %.1f) handRot=(P=%d Y=%d R=%d) | eye=(%.0f, %.0f, %.0f)",
                 handLoc.X, handLoc.Y, handLoc.Z, socketWorld.X, socketWorld.Y, socketWorld.Z, errLen, socketOk ? 1 : 0,
                 rig.socketLocal.X, rig.socketLocal.Y, rig.socketLocal.Z, handRot.Pitch, handRot.Yaw, handRot.Roll,
                 cam.finalLocation.X, cam.finalLocation.Y, cam.finalLocation.Z);
    }

    {
        std::lock_guard lock(g_poseMutex);
        g_lastGrappleDiag.gloveWorldLocation = handLoc;
        g_lastGrappleDiag.gloveWorldRotation = handRot;
        g_lastGrappleDiag.gloveRelOffsetUU   = relOffset;
        g_lastGrappleDiag.aimRayOrigin       = handLoc;
        g_lastGrappleDiag.aimRayDirection    = handDir;
        g_lastGrappleDiag.active             = true;
    }
    g_gloveTransformOverrides.fetch_add(1, std::memory_order_relaxed);
}

// Adds an injected value to a UE3 PlayerInput axis. UE3's native input zeroes the axes every frame before
// re-accumulating bound keys; if it did NOT (value unchanged since our last write) we replace our previous
// contribution instead of stacking it, so the axis can never run away.
void AddInjectedAxis(float* axis, float inj, AxisInjState& st) {
    float base = *axis;
    if (st.valid && base == st.lastTotal) base -= st.lastInj;
    *axis        = base + inj;
    st.lastTotal = *axis;
    st.lastInj   = inj;
    st.valid     = true;
}

// Phase 4 (fixed): Injects OpenXR thumbsticks into UE3 PlayerInput BEFORE PlayerController.PlayerTick runs.
// PlayerTick calls PlayerInput.PlayerInput() (which scales aBaseY/aStrafe/aTurn by DeltaTime and the move/look
// speeds) and then PlayerMove() as script-to-script calls that never pass through ProcessEvent, so the raw,
// unscaled [-1..1] axes must be in place before PlayerTick starts. relYawURot rotates the stick vector from
// the head/controller frame into the body frame (head- or controller-relative locomotion).
void InjectLocomotionIntoPlayerInput(UObject* piObj, int32_t relYawURot) {
    if (!IsLiveUObject(piObj)) return;
    if (piObj->Class->PropertiesSize < static_cast<int32_t>(offsets::kPlayerInput_MinSize) ||
        !HasSuperClassNamed(piObj->Class, g_playerInputFnameIdx, "PlayerInput")) return;

    const auto& cfg = samu::config::Get();
    if (!cfg.enableControllerInput) return;

    MotionControllerInput inp = UE3Bridge::GetControllerInput();
    float lx = 0.0f, ly = 0.0f, rx = 0.0f;
    if (inp.active) {
        lx = ApplyDeadzone(inp.leftStickX,  cfg.thumbstickDeadzone);
        ly = ApplyDeadzone(inp.leftStickY,  cfg.thumbstickDeadzone);
        rx = ApplyDeadzone(inp.rightStickX, cfg.thumbstickDeadzone);
    }

    // Stick vector in the reference frame: X = forward, Y = right. Rotate into the body (PlayerController) frame.
    const FVector mv = RotateVectorByYawURot(FVector{ly, lx, 0.0f}, relYawURot);
    const float turn = rx * cfg.turnSpeedScale;

    auto* raw = reinterpret_cast<uint8_t*>(piObj);
    AddInjectedAxis(reinterpret_cast<float*>(raw + offsets::kPlayerInput_aBaseY),  mv.X, g_injFwd);
    AddInjectedAxis(reinterpret_cast<float*>(raw + offsets::kPlayerInput_aStrafe), mv.Y, g_injStrafe);
    AddInjectedAxis(reinterpret_cast<float*>(raw + offsets::kPlayerInput_aTurn),   turn, g_injTurn);

    {
        std::lock_guard lock(g_poseMutex);
        g_inputDiag.injForward = mv.X;
        g_inputDiag.injStrafe  = mv.Y;
        g_inputDiag.injTurn    = turn;
    }
    if (mv.X != 0.0f || mv.Y != 0.0f || turn != 0.0f) {
        g_playerInputInjections.fetch_add(1, std::memory_order_relaxed);
    }
}

// Yaw (relative to the body) that the left stick's "forward" points along.
int32_t LocomotionRelativeYaw() {
    switch (samu::config::Get().locomotionDirection) {
    case 1: {
        const HmdUe3Pose hmd = GetFramePose();
        return hmd.orientationValid ? hmd.rotationURot.Yaw : 0;
    }
    case 2: {
        const HandUe3Pose left = UE3Bridge::GetLeftAimPose();
        return left.orientationValid ? left.rotationURot.Yaw : 0;
    }
    default:
        return 0;
    }
}

// ---- Console commands & engine tweaks (game thread only) ----

UFunction* FindFunction(const char* fnName, const char* outerName) {
    if (!g_gobjects || !g_gobjects->Data) return nullptr;
    for (int32_t i = 0; i < g_gobjects->Num; ++i) {
        UObject* obj = g_gobjects->Data[i];
        if (!obj || !obj->Class || !obj->Outer) continue;
        if (SdkDumper::NameToString(g_gnames, obj->Name) != fnName) continue;
        if (SdkDumper::GetObjClassName(g_gnames, obj) != "Function") continue;
        if (SdkDumper::NameToString(g_gnames, obj->Outer->Name) == outerName) return static_cast<UFunction*>(obj);
    }
    return nullptr;
}

// Executes a UE3 console command through PlayerController.ConsoleCommand (native, ParmsSize 0x1C).
bool RunConsoleCommand(UObject* pcObj, const wchar_t* cmd) {
    if (!g_fnConsoleCommand) g_fnConsoleCommand = FindFunction("ConsoleCommand", "PlayerController");
    if (!g_fnConsoleCommand || !g_origProcessEvent || !IsPlayerControllerInstance(pcObj)) {
        LOG_WARN("UE3Bridge: cannot run console command '%ls' (ConsoleCommand=%p)", cmd, static_cast<void*>(g_fnConsoleCommand));
        return false;
    }
    static wchar_t s_cmdBuf[256];
    wcsncpy_s(s_cmdBuf, cmd, _TRUNCATE);
    struct ConsoleCommandParms {
        FString  Command;      // +0x00
        uint32_t bWriteToLog;  // +0x0C
        FString  ReturnValue;  // +0x10
    } parms{};
    static_assert(sizeof(ConsoleCommandParms) == 0x1C, "ConsoleCommand parms layout");
    const int32_t len = static_cast<int32_t>(std::wcslen(s_cmdBuf)) + 1;
    parms.Command.Data = s_cmdBuf;
    parms.Command.Num  = len;
    parms.Command.Max  = len;
    parms.bWriteToLog  = 1;
    g_origProcessEvent(pcObj, g_fnConsoleCommand, &parms, nullptr);
    // ReturnValue was allocated by the engine; it is intentionally leaked (tiny, rare).
    LOG_INFO("UE3Bridge: console command '%ls' executed (result='%ls')", s_cmdBuf,
             (parms.ReturnValue.Data && parms.ReturnValue.Num > 0) ? parms.ReturnValue.Data : L"");
    return true;
}

UProperty* FindPropertyInClassChain(UStruct* cls, const char* name, const char* propClass) {
    for (int depth = 0; cls && depth < 32; ++depth, cls = cls->SuperField) {
        for (UField* f = cls->Children; f; f = f->Next) {
            if (SdkDumper::NameToString(g_gnames, f->Name) == name &&
                SdkDumper::GetObjClassName(g_gnames, f) == propClass) {
                return static_cast<UProperty*>(f);
            }
        }
    }
    return nullptr;
}

// ---- VR fixes that go through the game's own objects (game thread only) ----

// First-person arms in UDK are drawn with their own, narrower "weapon FOV": the engine squeezes the mesh towards
// the view axis every frame. On a monitor that hides wide-angle stretching; in VR it makes the hand look enlarged
// and slide against the world whenever the head turns. FOV 0 = draw it with the world's perspective.
void FixHandMeshFov(UObject* gun, UObject* mesh) {
    if (!samu::config::Get().trueScaleHand || !g_origProcessEvent) return;
    static bool       s_resolved = false;
    static UFunction* s_fnMeshSetFov = nullptr;
    static UFunction* s_fnPscSetFov  = nullptr;
    static std::atomic<int32_t> s_udkSkelIdx{-1};
    static std::atomic<int32_t> s_udkPscIdx{-1};
    if (!s_resolved) {
        s_resolved     = true;
        s_fnMeshSetFov = FindFunction("SetFOV", "UDKSkeletalMeshComponent");
        s_fnPscSetFov  = FindFunction("SetFOV", "UDKParticleSystemComponent");
        LOG_INFO("UE3Bridge: weapon-FOV functions: mesh SetFOV=%p(0x%X) particle SetFOV=%p(0x%X)",
                 static_cast<void*>(s_fnMeshSetFov), s_fnMeshSetFov ? s_fnMeshSetFov->ParmsSize : 0,
                 static_cast<void*>(s_fnPscSetFov), s_fnPscSetFov ? s_fnPscSetFov->ParmsSize : 0);
    }
    auto fix = [](UObject* comp, UFunction* fn, std::atomic<int32_t>& idx, const char* cls, const char* what) {
        if (!fn || fn->ParmsSize != 0x04 || !IsLiveUObject(comp) || !HasSuperClassNamed(comp->Class, idx, cls)) return;
        UProperty* p = FindPropertyInClassChain(comp->Class, "FOV", "FloatProperty");
        if (!p) return;
        const float cur = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(comp) + p->Offset);
        if (cur == 0.0f) return;
        float parm = 0.0f;
        g_origProcessEvent(comp, fn, &parm, nullptr);
        LOG_INFO("UE3Bridge: %s was drawn with its own FOV of %.1f deg (flat-screen weapon trick) -> now uses the world's perspective",
                 what, cur);
    };
    fix(mesh, s_fnMeshSetFov, s_udkSkelIdx, "UDKSkeletalMeshComponent", "hand mesh");
    if (!IsLiveUObject(gun)) return;
    for (const char* name : {"MyBeamComponent", "grappleBeginEffect", "PowerJumpLightEffect"}) {
        UProperty* p = FindPropertyInClassChain(gun->Class, name, "ComponentProperty");
        if (!p) continue;
        UObject* comp = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(gun) + p->Offset);
        fix(comp, s_fnPscSetFov, s_udkPscIdx, "UDKParticleSystemComponent", name);
    }
}

// Parameters of Actor.Trace (native 277).
struct TraceParms {
    FVector  HitLocation;      // +0x00 (out)
    FVector  HitNormal;        // +0x0C (out)
    FVector  TraceEnd;         // +0x18
    FVector  TraceStart;       // +0x24
    uint32_t bTraceActors;     // +0x30
    FVector  Extent;           // +0x34
    UObject* HitMaterial;      // +0x40 TraceHitInfo
    UObject* HitPhysMaterial;
    int32_t  HitItem;
    int32_t  HitLevelIndex;
    FName    HitBoneName;
    UObject* HitComponent;
    int32_t  ExtraTraceFlags;  // +0x5C
    UObject* ReturnValue;      // +0x60
};
static_assert(sizeof(TraceParms) == 0x64, "Actor.Trace parms");

// Points the game's script-side aim (CameraCache.POV.Rotation) along the right hand. Called right after the
// camera update of every frame and again immediately before firing.
void UpdateScriptAim(UObject* pcObj) {
    AimRig& a = g_aimRig;
    const auto& cfg = samu::config::Get();
    if (!a.resolved) {
        a.resolved = true;
        a.fnTrace  = FindFunction("Trace", "Actor");
        if (a.fnTrace && a.fnTrace->ParmsSize != sizeof(TraceParms)) {
            LOG_WARN("UE3Bridge: Actor.Trace has ParmsSize 0x%X (expected 0x%X); aiming without a hand-ray trace.",
                     a.fnTrace->ParmsSize, static_cast<unsigned>(sizeof(TraceParms)));
            a.fnTrace = nullptr;
        }
        LOG_INFO("UE3Bridge: aim rig: Actor.Trace=%p", static_cast<void*>(a.fnTrace));
    }

    bool active = false;
    do {
        if (!cfg.aimFromHand || !cfg.enable6DofGrapple || !g_origProcessEvent) break;
        if (g_headViewFrame == 0 || !g_headViewIsPawn) break;              // cutscene cameras keep their own aim
        if (!IsLiveUObject(a.camera) || !IsPlayerControllerInstance(pcObj)) break;
        const HandUe3Pose ra = UE3Bridge::GetRightAimPose();
        if (!ra.positionValid || !ra.orientationValid) break;              // controller asleep: aim with the head
        FVector  handLoc{};
        FRotator handRot{};
        FVector  handDir{};
        FVector  relOffset{};
        if (!ComputeRightAimWorldPose(handLoc, handRot, handDir, relOffset)) break;
        FVector eye{};
        if (!GetPawnEyeLocation(pcObj, eye)) break;                        // where the game starts its own trace

        UObject* gun = (IsLiveUObject(a.gun) && HasSuperClassNamed(a.gun->Class, g_grappleGunFnameIdx, "GrappleGun")) ? a.gun : nullptr;
        float range = cfg.grappleRangeUU + 1000.0f;
        if (gun) {
            if (!a.propMaxDist)    a.propMaxDist    = FindPropertyInClassChain(gun->Class, "fMaxDistance", "FloatProperty");
            if (!a.propGrappleLoc) a.propGrappleLoc = FindPropertyInClassChain(gun->Class, "vGrappleLocation", "StructProperty");
            if (a.propMaxDist) {
                const float md = *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(gun) + a.propMaxDist->Offset);
                if (md > 100.0f && md < 200000.0f) range = md + 1000.0f;   // same reach as the game's own trace
            }
        }

        FVector target{handLoc.X + handDir.X * range, handLoc.Y + handDir.Y * range, handLoc.Z + handDir.Z * range};
        bool  hit     = false;
        float hitDist = range;
        if (gun && a.fnTrace) {
            TraceParms tp{};
            tp.TraceEnd        = target;
            tp.TraceStart      = handLoc;
            tp.bTraceActors    = 1;
            tp.ExtraTraceFlags = 1;   // TRACEFLAG_Bullet, exactly like GrappleGun.CheckForRange
            g_origProcessEvent(gun, a.fnTrace, &tp, nullptr);
            if (tp.ReturnValue) {
                const float dx = tp.HitLocation.X - handLoc.X, dy = tp.HitLocation.Y - handLoc.Y, dz = tp.HitLocation.Z - handLoc.Z;
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (d == d && d <= range + 10.0f) {
                    hit     = true;
                    hitDist = d;
                    target  = tp.HitLocation;
                }
            }
        }

        const float tx = target.X - eye.X, ty = target.Y - eye.Y, tz = target.Z - eye.Z;
        const float horiz = std::sqrt(tx * tx + ty * ty);
        if (horiz + std::fabs(tz) < 1.0f) break;                           // target on top of the eye: keep head aim
        constexpr float kRadToURot = 32768.0f / 3.14159265358979323846f;
        FRotator aim{};
        aim.Yaw   = static_cast<int32_t>(std::lround(std::atan2(ty, tx) * kRadToURot)) & 0xFFFF;
        aim.Pitch = static_cast<int32_t>(std::lround(std::atan2(tz, horiz) * kRadToURot));
        aim.Roll  = 0;
        *reinterpret_cast<FRotator*>(reinterpret_cast<uint8_t*>(a.camera) + offsets::kCamera_CameraCache_POV_Rot) = aim;

        a.hand      = handLoc;
        a.target    = target;
        a.aimRot    = aim;
        a.hit       = hit;
        a.hitDistUU = hitDist;
        g_reticleDistUU.store(hit ? hitDist : 3000.0f, std::memory_order_relaxed);
        active = true;

        const int64_t nowMs = SteadyNowMs();
        if (nowMs - a.lastLogMs > 5000) {
            a.lastLogMs = nowMs;
            LOG_INFO("UE3Bridge: hand aim: hand=(%.0f, %.0f, %.0f) dir=(%+.2f, %+.2f, %+.2f) -> %s at %.0f UU, target=(%.0f, %.0f, %.0f) | "
                     "script aim P=%d Y=%d (head P=%d Y=%d) | range=%.0f gun=%d crosshair-lit=%d",
                     handLoc.X, handLoc.Y, handLoc.Z, handDir.X, handDir.Y, handDir.Z, hit ? "HIT" : "nothing", hitDist,
                     target.X, target.Y, target.Z, aim.Pitch, aim.Yaw, g_headViewRot.Pitch, g_headViewRot.Yaw & 0xFFFF,
                     range, gun ? 1 : 0, g_reticleLit.load(std::memory_order_relaxed) ? 1 : 0);
        }
    } while (false);

    if (g_aimFromHandActive.exchange(active, std::memory_order_relaxed) != active) {
        LOG_INFO("UE3Bridge: aim-from-hand %s", active ? "ACTIVE (the game's aim is pointed at what the right hand's ray hits)"
                                                        : "inactive (aim follows the head)");
    }
}

// After StartFire: compare where we aimed with where the game says the grapple attached.
void LogGrappleShot(float trigger) {
    const AimRig& a = g_aimRig;
    const bool handAim = g_aimFromHandActive.load(std::memory_order_relaxed);
    if (!handAim || !IsLiveUObject(a.gun) || !a.propGrappleLoc) {
        LOG_INFO("[MotionControls] Right Trigger PRESSED (%.2f) -> StartFire(0) [Grapple Beam] aim=%s", trigger, handAim ? "HAND" : "head");
        return;
    }
    const FVector g = *reinterpret_cast<const FVector*>(reinterpret_cast<const uint8_t*>(a.gun) + a.propGrappleLoc->Offset);
    const float dx = g.X - a.target.X, dy = g.Y - a.target.Y, dz = g.Z - a.target.Z;
    LOG_INFO("[MotionControls] Right Trigger PRESSED (%.2f) -> StartFire(0) [Grapple Beam] aim=HAND | hand ray %s at %.0f UU, "
             "target=(%.0f, %.0f, %.0f) | game grapple point=(%.0f, %.0f, %.0f) miss=%.0f UU",
             trigger, a.hit ? "HIT" : "hit nothing", a.hitDistUU, a.target.X, a.target.Y, a.target.Z, g.X, g.Y, g.Z,
             std::sqrt(dx * dx + dy * dy + dz * dz));
}

// Hides the game's head-locked crosshair while the hand reticle replaces it, and reads its state (the game lights
// the crosshair up when the aimed spot can be grappled) so the hand reticle can show the same thing.
void UpdateHudForVr(UObject* pcObj) {
    HudRig& h = g_hudRig;
    if (!g_origProcessEvent || !IsPlayerControllerInstance(pcObj)) return;
    const int64_t nowMs = SteadyNowMs();
    if (!h.propMyHud) h.propMyHud = FindPropertyInClassChain(pcObj->Class, "myHUD", "ObjectProperty");
    if (!h.propMyHud) return;
    UObject* hud = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(pcObj) + h.propMyHud->Offset);
    if (!IsLiveUObject(hud)) { g_reticleLit.store(false, std::memory_order_relaxed); return; }
    h.lastHud = hud;

    if ((!h.resolved || !h.propPopupsMC || !h.propPopupTF || !h.propPauseFade || !h.propPauseMovie ||
         !h.fnGetPosition || !h.fnSetPosition || !h.fnGetFloat || !h.fnSetFloat || !h.fnGetString || !h.fnSetString) &&
        nowMs - h.lastResolveMs > 1000) {
        h.lastResolveMs = nowMs;
        if (!h.propHudMovie)       h.propHudMovie       = FindPropertyInClassChain(hud->Class, "HudMovie", "ObjectProperty");
        if (!h.propPauseFade)      h.propPauseFade      = static_cast<UBoolProperty*>(FindPropertyInClassChain(hud->Class, "bPauseMenuFade", "BoolProperty"));
        if (!h.propPauseMovie)     h.propPauseMovie     = FindPropertyInClassChain(hud->Class, "PauseMenuASAMU", "ObjectProperty");
        if (!h.propCrosshairState) h.propCrosshairState = FindPropertyInClassChain(hud->Class, "CurrentCrosshair", "ByteProperty");
        if (!h.fnSetVisible) {
            h.fnSetVisible = FindFunction("SetVisible", "GFxObject");
            if (h.fnSetVisible && h.fnSetVisible->ParmsSize != 0x04) h.fnSetVisible = nullptr;
        }
        if (!h.fnGetPosition) {
            h.fnGetPosition = FindFunction("GetPosition", "GFxObject");
            if (h.fnGetPosition && h.fnGetPosition->ParmsSize != 0x0C) h.fnGetPosition = nullptr;
        }
        if (!h.fnSetPosition) {
            h.fnSetPosition = FindFunction("SetPosition", "GFxObject");
            if (h.fnSetPosition && h.fnSetPosition->ParmsSize != 0x08) h.fnSetPosition = nullptr;
        }
        if (!h.fnGetFloat) {
            h.fnGetFloat = FindFunction("GetFloat", "GFxObject");
            if (h.fnGetFloat && h.fnGetFloat->ParmsSize != 0x10) h.fnGetFloat = nullptr;
        }
        if (!h.fnSetFloat) {
            h.fnSetFloat = FindFunction("SetFloat", "GFxObject");
            if (h.fnSetFloat && h.fnSetFloat->ParmsSize != 0x10) h.fnSetFloat = nullptr;
        }
        if (!h.fnGetString) {
            h.fnGetString = FindFunction("GetString", "GFxObject");
            if (h.fnGetString && h.fnGetString->ParmsSize != 0x18) h.fnGetString = nullptr;
        }
        if (!h.fnSetString) {
            h.fnSetString = FindFunction("SetString", "GFxObject");
            if (h.fnSetString && h.fnSetString->ParmsSize != 0x1C) h.fnSetString = nullptr;
        }
        if (h.propHudMovie) {
            UObject* movie = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(hud) + h.propHudMovie->Offset);
            if (IsLiveUObject(movie)) {
                if (!h.propCrosshairMC) h.propCrosshairMC = FindPropertyInClassChain(movie->Class, "CrosshairMC", "ObjectProperty");
                if (!h.propPopupsMC)    h.propPopupsMC    = FindPropertyInClassChain(movie->Class, "PopupsMC",    "ObjectProperty");
                if (!h.propPopupTF)     h.propPopupTF     = FindPropertyInClassChain(movie->Class, "PopupTF",     "ObjectProperty");
                if (!h.propMovieHidden) h.propMovieHidden = static_cast<UBoolProperty*>(FindPropertyInClassChain(movie->Class, "bHidden", "BoolProperty"));
            }
        }
        if (!h.resolved && h.propHudMovie && h.propCrosshairMC && h.propCrosshairState && h.fnSetVisible) {
            h.resolved = true;
            LOG_INFO("UE3Bridge: HUD rig ready (%s): HudMovie +0x%X, CrosshairMC +0x%X, PopupsMC +0x%X, CurrentCrosshair +0x%X",
                     SdkDumper::GetFullObjectPath(g_gnames, hud).c_str(), h.propHudMovie->Offset, h.propCrosshairMC->Offset,
                     h.propPopupsMC ? h.propPopupsMC->Offset : 0u, h.propCrosshairState->Offset);
        }
    }
    if (!h.resolved) return;

    // enum CrosshairTypes { HIDDEN, DOT, DISABLED, ENABLED, STORY_DISABLED, STORY_ENABLED }
    const uint8_t state = *(reinterpret_cast<const uint8_t*>(hud) + h.propCrosshairState->Offset);
    g_reticleLit.store(state == 3 || state == 5, std::memory_order_relaxed);

    UObject* movie = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(hud) + h.propHudMovie->Offset);
    if (!IsLiveUObject(movie)) return;
    UObject* clip = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(movie) + h.propCrosshairMC->Offset);
    if (IsLiveUObject(clip)) {
        const int wanted = (samu::config::Get().hideGameCrosshair && g_aimFromHandActive.load(std::memory_order_relaxed)) ? 0 : 1;
        if (clip != h.lastClip || wanted != h.lastWanted || nowMs - h.lastAssertMs > 1000) {
            if (clip != h.lastClip || wanted != h.lastWanted) {
                LOG_INFO("UE3Bridge: game crosshair (screen centre) -> %s", wanted ? "shown" : "hidden (the hand reticle replaces it)");
            }
            h.lastClip     = clip;
            h.lastWanted   = wanted;
            h.lastAssertMs = nowMs;
            uint32_t visible = static_cast<uint32_t>(wanted);
            g_origProcessEvent(clip, h.fnSetVisible, &visible, nullptr);
        }
    }

    // Shrink Flash tutorial/prompt popups (~66%) and lift them into comfortable center VR view.
    if (h.propPopupsMC && h.fnGetPosition && h.fnSetPosition) {
        UObject* popup = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(movie) + h.propPopupsMC->Offset);
        if (IsLiveUObject(popup)) {
            if (popup != h.lastPopup) {
                struct { float X; float Y; uint32_t Ret; } gp{};
                g_origProcessEvent(popup, h.fnGetPosition, &gp, nullptr);
                if (gp.Ret) {
                    h.lastPopup        = popup;
                    h.basePopupX       = gp.X;
                    h.basePopupY       = gp.Y;
                    h.basePopupW       = 0.0f;
                    h.haveBasePopupPos = true;
                    h.popupScaled      = false;
                    h.lastAppliedLift  = -1.0f;
                    if (h.fnGetFloat) {
                        static wchar_t s_mW[] = L"_width";
                        struct { FString Member; float Ret; } gf{};
                        gf.Member.Data = s_mW; gf.Member.Num = 7; gf.Member.Max = 7;
                        g_origProcessEvent(popup, h.fnGetFloat, &gf, nullptr);
                        h.basePopupW = gf.Ret;
                    }
                    LOG_INFO("UE3Bridge: HUD PopupsMC base pos=(%.1f, %.1f) w=%.1f", gp.X, gp.Y, h.basePopupW);
                }
            }
            constexpr float kPopupPct = 66.0f;
            if (h.haveBasePopupPos && h.fnSetFloat && !h.popupScaled) {
                static wchar_t s_mXS[] = L"_xscale";
                static wchar_t s_mYS[] = L"_yscale";
                struct { FString Member; float Val; } sx{}, sy{};
                sx.Member.Data = s_mXS; sx.Member.Num = 8; sx.Member.Max = 8; sx.Val = kPopupPct;
                sy.Member.Data = s_mYS; sy.Member.Num = 8; sy.Member.Max = 8; sy.Val = kPopupPct;
                g_origProcessEvent(popup, h.fnSetFloat, &sx, nullptr);
                g_origProcessEvent(popup, h.fnSetFloat, &sy, nullptr);
                h.popupScaled     = true;
                h.lastAppliedLift = -1.0f;
                LOG_INFO("UE3Bridge: HUD PopupsMC scaled -> %.0f%%", kPopupPct);
            }
            const float lift = std::clamp(samu::config::GetHudTextLift(), 0.0f, 1.0f);
            if (h.haveBasePopupPos && std::fabs(lift - h.lastAppliedLift) > 0.005f) {
                float targetX = h.basePopupX;
                if (h.popupScaled && h.basePopupX < 450.0f && h.basePopupW > 10.0f) {
                    targetX = h.basePopupX + h.basePopupW * (1.0f - kPopupPct * 0.01f) * 0.5f;
                }
                float targetY = h.basePopupY - lift * 185.0f;
                if (h.basePopupY > 250.0f) targetY = std::max(80.0f, targetY);
                struct { float X; float Y; } sp{targetX, targetY};
                g_origProcessEvent(popup, h.fnSetPosition, &sp, nullptr);
                h.lastAppliedLift = lift;
                LOG_INFO("UE3Bridge: HUD PopupsMC moved -> (%.1f, %.1f) (HudTextLift=%.2f)", sp.X, sp.Y, lift);
            }
        }
    }

    // Scale down UE3's voice-over subtitle font once so long narrator sentences fit cleanly in center VR view.
    if (!IsLiveUObject(h.scaledSubtitleFont) && g_gobjects && g_gobjects->Data) {
        for (int32_t i = 0; i < g_gobjects->Num; ++i) {
            UObject* obj = g_gobjects->Data[i];
            if (!obj || !obj->Class) continue;
            if (SdkDumper::NameToString(g_gnames, obj->Name) != "ASAMUSubtitleFont") continue;
            UProperty* propScale = FindPropertyInClassChain(obj->Class, "ScalingFactor", "FloatProperty");
            if (propScale) {
                auto* sf = reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(obj) + propScale->Offset);
                if (*sf > 0.05f && *sf < 10.0f) {
                    h.origSubtitleScale  = *sf;
                    *sf                  = std::max(0.35f, *sf * 0.72f);
                    h.scaledSubtitleFont = obj;
                    LOG_INFO("UE3Bridge: subtitle font '%s' ScalingFactor %.2f -> %.2f",
                             SdkDumper::GetFullObjectPath(g_gnames, obj).c_str(), h.origSubtitleScale, *sf);
                    break;
                }
            }
        }
    }

    // Remap keyboard/mouse tutorial hints in Scaleform's PopupTF to VR Touch controls (e.g. [SPACE] -> [LEFT TRIGGER / A]).
    if (h.propPopupTF && h.fnGetString && h.fnSetString && nowMs - h.lastPopupPollMs >= 150) {
        bool popupHidden = false;
        if (h.propMovieHidden) {
            const uint32_t bits = *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(movie) + h.propMovieHidden->Offset);
            popupHidden = (bits & h.propMovieHidden->BitMask) != 0;
        }
        if (!popupHidden) {
            UObject* tf = *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(movie) + h.propPopupTF->Offset);
            if (IsLiveUObject(tf)) {
                h.lastPopupPollMs = nowMs;
                static wchar_t s_memberText[] = L"text";
                struct { FString Member; FString ReturnValue; } gs{};
                static_assert(sizeof(gs) == 0x18, "GetString parms");
                gs.Member.Data = s_memberText;
                gs.Member.Num  = 5;
                gs.Member.Max  = 5;
                g_origProcessEvent(tf, h.fnGetString, &gs, nullptr);
                if (gs.ReturnValue.Data && gs.ReturnValue.Num > 1 && gs.ReturnValue.Num < 512) {
                    std::wstring txt(gs.ReturnValue.Data, static_cast<size_t>(gs.ReturnValue.Num - 1));
                    if (!txt.empty() && txt != h.lastVrPopupText) {
                        std::wstring out = txt;
                        auto replaceCI = [&](const std::wstring& needle, const std::wstring& repl) {
                            std::wstring upperOut = out, upperN = needle;
                            std::transform(upperOut.begin(), upperOut.end(), upperOut.begin(), ::towupper);
                            std::transform(upperN.begin(), upperN.end(), upperN.begin(), ::towupper);
                            size_t pos = 0;
                            bool hit = false;
                            while ((pos = upperOut.find(upperN, pos)) != std::wstring::npos) {
                                out.replace(pos, needle.size(), repl);
                                upperOut.replace(pos, needle.size(), repl);
                                pos += repl.size();
                                hit = true;
                            }
                            return hit;
                        };
                        bool changed = false;
                        changed |= replaceCI(L"USE THE MOUSE TO LOOK AROUND", L"LOOK AROUND OR R-STICK");
                        changed |= replaceCI(L"[SPACEBAR]",         L"[L-TRIGGER / A]");
                        changed |= replaceCI(L"[SPACE]",            L"[L-TRIGGER / A]");
                        changed |= replaceCI(L"[LEFTSHIFT]",        L"[L-GRIP / L3]");
                        changed |= replaceCI(L"[RIGHTSHIFT]",       L"[L-GRIP / L3]");
                        changed |= replaceCI(L"[SHIFT]",            L"[L-GRIP / L3]");
                        changed |= replaceCI(L"[RIGHTMOUSEBUTTON]", L"[R-GRIP]");
                        changed |= replaceCI(L"[RIGHTMOUSE]",       L"[R-GRIP]");
                        changed |= replaceCI(L"[RMB]",              L"[R-GRIP]");
                        changed |= replaceCI(L"[LEFTMOUSEBUTTON]",  L"[R-TRIGGER]");
                        changed |= replaceCI(L"[LEFTMOUSE]",        L"[R-TRIGGER]");
                        changed |= replaceCI(L"[LMB]",              L"[R-TRIGGER]");
                        changed |= replaceCI(L"[XBOXTYPE_A]",       L"[L-TRIGGER / A]");
                        changed |= replaceCI(L"[W]",                L"[L-STICK]");
                        changed |= replaceCI(L"[MOUSE]",            L"[R-STICK]");
                        changed |= replaceCI(L"AND THE MOUSE",      L"AND R-STICK");
                        if (changed) {
                            static wchar_t s_vrTextBuf[512]{};
                            const size_t n = std::min<size_t>(out.size(), 510);
                            std::wmemcpy(s_vrTextBuf, out.c_str(), n);
                            s_vrTextBuf[n] = L'\0';
                            struct { FString Member; FString s; UObject* InContext; } ss{};
                            static_assert(sizeof(ss) == 0x1C, "SetString parms");
                            ss.Member.Data = s_memberText;
                            ss.Member.Num  = 5;
                            ss.Member.Max  = 5;
                            ss.s.Data      = s_vrTextBuf;
                            ss.s.Num       = static_cast<int32_t>(n + 1);
                            ss.s.Max       = static_cast<int32_t>(n + 1);
                            g_origProcessEvent(tf, h.fnSetString, &ss, nullptr);
                            h.lastVrPopupText = out;
                            LOG_INFO("UE3Bridge: tutorial popup remapped to VR -> '%ls'", out.c_str());
                        }
                    }
                }
            }
        }
    }
}

// Motion blur is computed from how far the camera turned since the previous frame, i.e. it smears the whole
// picture on every head turn (and, with alternate-frame stereo, between the two eye positions). Switch it off
// through the game's own settings object, exactly like unticking "Motion blur" in the options menu.
void DisableGameMotionBlur() {
    if (!samu::config::Get().disableMotionBlur || !g_origProcessEvent || !g_gobjects || !g_gobjects->Data) return;
    UFunction* fnGet = FindFunction("GetBoolSetting", "ASAMUSystemSettingsManager");
    UFunction* fnSet = FindFunction("SetBoolSetting", "ASAMUSystemSettingsManager");
    UObject* mgr = nullptr;
    for (int32_t i = 0; i < g_gobjects->Num; ++i) {
        UObject* obj = g_gobjects->Data[i];
        if (!obj || !obj->Class) continue;
        if (SdkDumper::GetObjClassName(g_gnames, obj) != "ASAMUSystemSettingsManager") continue;
        if (SdkDumper::NameToString(g_gnames, obj->Name).rfind("Default__", 0) == 0) continue;
        mgr = obj;   // keep the newest instance
    }
    if (!mgr || !fnGet || !fnSet || fnGet->ParmsSize != 0x14 || fnSet->ParmsSize != 0x14) {
        LOG_WARN("UE3Bridge: motion blur left as is (settings manager=%p, Get=%p(0x%X), Set=%p(0x%X)). "
                 "Untick 'Motion blur' in the game's options instead.",
                 static_cast<void*>(mgr), static_cast<void*>(fnGet), fnGet ? fnGet->ParmsSize : 0,
                 static_cast<void*>(fnSet), fnSet ? fnSet->ParmsSize : 0);
        return;
    }
    static wchar_t s_name[] = L"MotionBlur";
    const int32_t nameLen = static_cast<int32_t>(std::wcslen(s_name)) + 1;
    auto get = [&](int& value) {
        struct { FString Name; uint8_t Out; uint8_t Pad[3]; uint32_t Ret; } p{};
        static_assert(sizeof(p) == 0x14, "GetBoolSetting parms");
        p.Name.Data = s_name; p.Name.Num = nameLen; p.Name.Max = nameLen;
        p.Out = 0xFF;
        g_origProcessEvent(mgr, fnGet, &p, nullptr);
        value = p.Out;
        return p.Ret != 0;
    };
    int before = -1, after = -1;
    const bool gotBefore = get(before);
    if (gotBefore && before == 0) {
        LOG_INFO("UE3Bridge: motion blur is already off.");
        return;
    }
    struct { FString Name; uint32_t Value; uint32_t Ret; } sp{};
    static_assert(sizeof(sp) == 0x14, "SetBoolSetting parms");
    sp.Name.Data = s_name; sp.Name.Num = nameLen; sp.Name.Max = nameLen;
    sp.Value = 0;
    g_origProcessEvent(mgr, fnSet, &sp, nullptr);
    const bool gotAfter = get(after);
    LOG_INFO("UE3Bridge: motion blur: was %d (read ok=%d) -> set off (accepted=%d) -> now %d (read ok=%d)",
             before, gotBefore ? 1 : 0, sp.Ret != 0 ? 1 : 0, after, gotAfter ? 1 : 0);
}

// Raises UE3's smoothed frame-rate cap (GameEngine.bSmoothFrameRate / MaxSmoothedFrameRate, default 62 Hz)
// so the game can render at the headset refresh rate.
void ApplyFrameRateCap() {
    const float target = samu::config::Get().targetFrameRate;
    if (target <= 0.0f || !g_gobjects || !g_gobjects->Data) return;

    UObject* engine = nullptr;
    for (int32_t i = 0; i < g_gobjects->Num && !engine; ++i) {
        UObject* obj = g_gobjects->Data[i];
        if (!obj || !obj->Class) continue;
        if (SdkDumper::GetObjClassName(g_gnames, obj) != "GameEngine") continue;
        if (SdkDumper::NameToString(g_gnames, obj->Name).rfind("Default__", 0) == 0) continue;
        engine = obj;
    }
    if (!engine) {
        LOG_WARN("UE3Bridge: GameEngine instance not found; frame-rate cap unchanged.");
        return;
    }
    auto* smoothProp = static_cast<UBoolProperty*>(FindPropertyInClassChain(engine->Class, "bSmoothFrameRate", "BoolProperty"));
    UProperty* maxProp = FindPropertyInClassChain(engine->Class, "MaxSmoothedFrameRate", "FloatProperty");
    if (!smoothProp || !maxProp) {
        LOG_WARN("UE3Bridge: GameEngine frame-rate properties not found (bSmoothFrameRate=%p, MaxSmoothedFrameRate=%p).",
                 static_cast<void*>(smoothProp), static_cast<void*>(maxProp));
        return;
    }
    auto* raw = reinterpret_cast<uint8_t*>(engine);
    auto* bits = reinterpret_cast<uint32_t*>(raw + smoothProp->Offset);
    auto* maxFps = reinterpret_cast<float*>(raw + maxProp->Offset);
    const bool  wasSmooth = (*bits & smoothProp->BitMask) != 0;
    const float oldMax = *maxFps;
    *bits  |= smoothProp->BitMask;
    *maxFps = target;
    g_appliedFpsCap.store(target, std::memory_order_relaxed);
    LOG_INFO("UE3Bridge: GameEngine frame cap: bSmoothFrameRate %d -> 1, MaxSmoothedFrameRate %.1f -> %.1f (offsets +0x%X/+0x%X)",
             wasSmooth ? 1 : 0, oldMax, target, smoothProp->Offset, maxProp->Offset);
}

// Once per gameplay level / on demand: frame-rate cap, disabling motion blur / DOF / SSAO / radial blur /
// 1-frame render lag (sharp low-latency head turns), disabling the UE3 rendering thread (exact frame<->pose
// pairing for AFR + pose-matched submission), and the requested backbuffer resolution.
void ApplyEngineTweaks(UObject* pcObj, std::string_view mapName) {
    const auto& cfg = samu::config::Get();
    if (g_lastEngineTweakMap != mapName) {
        g_lastEngineTweakMap.assign(mapName.data(), mapName.size());
        ApplyFrameRateCap();
        DisableGameMotionBlur();
        if (cfg.disableMotionBlur) {
            for (const wchar_t* cmd : {
                     L"Scale set MotionBlur False",
                     L"Scale set MotionBlurPause False",
                     L"Scale set DepthOfField False",
                     L"Scale set AmbientOcclusion False",
                     L"Scale set AllowRadialBlur False",
                     L"Scale set OneFrameThreadLag False",
                     L"Scale set MaxAnisotropy 16",
                 }) {
                RunConsoleCommand(pcObj, cmd);
            }
        }
    }

    if (cfg.matchRenderPose || samu::config::GetStereoMode() == 1) {
        // Re-assert single-threaded rendering if Present shifts back onto the UE3 render thread (e.g. Device::Reset).
        if (!g_renderPairingExact.load(std::memory_order_relaxed)) {
            if (++g_approxPairingFrames > 30 && g_presentObserved.load(std::memory_order_relaxed) &&
                g_renderToggleCount < 6) {
                const uint64_t nowMs = GetTickCount64();
                if (g_lastRenderToggleMs == 0 || nowMs - g_lastRenderToggleMs >= 3000) {
                    g_lastRenderToggleMs = nowMs;
                    ++g_renderToggleCount;
                    g_approxPairingFrames = 0;
                    LOG_INFO("UE3Bridge: Present runs on the UE3 rendering thread -> disabling it for exact frame/pose pairing (attempt %d).",
                             g_renderToggleCount);
                    RunConsoleCommand(pcObj, L"TOGGLERENDERINGTHREAD");
                }
            }
        } else {
            g_approxPairingFrames = 0;
        }
    }

    const int w = g_requestedResW.load(std::memory_order_relaxed);
    const int h = g_requestedResH.load(std::memory_order_relaxed);
    const uint32_t epoch = g_requestedResEpoch.load(std::memory_order_relaxed);
    if (w > 0 && h > 0 && (w != g_appliedResW || h != g_appliedResH || epoch != g_appliedResEpoch)) {
        g_appliedResW = w;
        g_appliedResH = h;
        g_appliedResEpoch = epoch;
        // Invalidate stale projection measurements so OpenXRManager re-measures cleanly at the new size.
        g_measValid.store(false, std::memory_order_relaxed);
        g_measTanH.store(0.0f, std::memory_order_relaxed);
        g_measTanV.store(0.0f, std::memory_order_relaxed);
        wchar_t cmd[64];
        std::swprintf(cmd, 64, L"setres %dx%d", w, h);
        RunConsoleCommand(pcObj, cmd);
    }
}

// PlayerTick pre-hook for the gameplay PlayerController: auto-recenter, latch this frame's pose/eye,
// queue the render tag, engine tweaks, and locomotion injection.
void BeginGameplayFrame(UObject* pcObj, std::string_view mapName, bool inSelfTest) {
    const auto& cfg = samu::config::Get();
    g_gameThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);

    if (!inSelfTest && cfg.autoRecenterOnLevel && !mapName.empty() && g_lastRecenteredMap != mapName &&
        UE3Bridge::GetHmdPose().positionValid) {
        g_lastRecenteredMap.assign(mapName.data(), mapName.size());
        LOG_INFO("UE3Bridge: auto-recentering HMD for level '%.*s'", static_cast<int>(mapName.size()), mapName.data());
        UE3Bridge::RecenterHmdOrigin();
    }

    // Restore the pure body yaw for the game's own PlayerTick (turning, movement direction, pawn facing).
    if (!inSelfTest) {
        auto* pcRot = reinterpret_cast<FRotator*>(reinterpret_cast<uint8_t*>(pcObj) + offsets::kActor_Rotation);
        if (g_bodyYawValid && g_bodyYawPc == pcObj) {
            // Something else (teleport, respawn, level script) rotated the controller since we wrote it: keep that turn.
            if (pcRot->Yaw != g_lastWrittenYaw) g_bodyYaw += (pcRot->Yaw - g_lastWrittenYaw);
            pcRot->Yaw = g_bodyYaw;
        } else {
            g_bodyYawPc    = pcObj;
            g_bodyYaw      = pcRot->Yaw;
            g_bodyYawValid = true;
        }
    }

    // Did a view hook deliver the camera POSITION to the renderer last frame? (required for AFR eye separation)
    g_framesWithViewOverride = (g_viewOverridesThisFrame > 0) ? g_framesWithViewOverride + 1 : 0;
    g_viewOverridesThisFrame = 0;
    const bool positionProven = g_framesWithViewOverride >= 10;
    if (g_afrPositionProven.exchange(positionProven, std::memory_order_relaxed) != positionProven) {
        LOG_INFO("UE3Bridge: per-eye camera position %s", positionProven
                     ? "is reaching the renderer -> AFR stereo allowed"
                     : "is NOT reaching the renderer (no view hook fired) -> AFR stereo held off (mono)");
    }

    g_framePose         = UE3Bridge::GetHmdPose();
    g_frameRightAimPose = UE3Bridge::GetRightAimPose();
    g_framePoseValid    = true;
    const bool afr = samu::config::GetStereoMode() == 1 && g_framePose.eyesValid &&
                     (inSelfTest || (positionProven && g_renderPairingExact.load(std::memory_order_relaxed)));
    g_frameEye = afr ? static_cast<int>(g_gameFrameIdx & 1u) : -1;
    ++g_gameFrameIdx;

    if (inSelfTest) return;
    PushFrameTag(g_framePose.sampleId, g_frameEye);
    ApplyEngineTweaks(pcObj, mapName);

    UObject* piObj = *reinterpret_cast<UObject* const*>(reinterpret_cast<uint8_t*>(pcObj) + offsets::kPC_PlayerInput);
    if (piObj) InjectLocomotionIntoPlayerInput(piObj, LocomotionRelativeYaw());

    // Re-assert the hand aim with the newest controller pose before the game's own scripts run this tick.
    UpdateScriptAim(pcObj);
}

void EndGameplayFrames() {
    if (g_framePoseValid) ClearFrameTags();
    DestroyLeftHandActor();
    g_gamePaused.store(false, std::memory_order_relaxed);
    g_stickSprintLatched     = false;
    g_prevLeftStickClick     = false;
    g_bodyYawValid           = false;
    g_bodyYawPc              = nullptr;
    g_eyeFromPawnValid       = false;
    g_lastAdjustedValid      = false;
    g_headViewFrame          = 0;
    g_inTickPhase            = false;
    g_renderHeadViewOk       = 0;
    g_aimFromHandActive.store(false, std::memory_order_relaxed);
    g_reticleLit.store(false, std::memory_order_relaxed);
    g_aimRig.camera          = nullptr;
    g_aimRig.gun             = nullptr;
    g_hudRig.lastClip        = nullptr;
    g_hudRig.lastHud         = nullptr;
    g_hudRig.lastWanted      = -1;
    g_framesWithViewOverride = 0;
    g_appliedResW            = 0;
    g_appliedResH            = 0;
    g_approxPairingFrames    = 0;
    g_renderToggleCount      = 0;
    g_measValid.store(false, std::memory_order_relaxed);
    g_measTanH.store(0.0f, std::memory_order_relaxed);
    g_measTanV.store(0.0f, std::memory_order_relaxed);
    g_frameRightAimPose      = HandUe3Pose{};
    g_framePoseValid = false;
    g_frameEye       = -1;
}

// ---- Projection measurement -------------------------------------------------------------------------------
// LocalPlayer.Project(WorldLoc) runs the engine's own CalcSceneView + WorldToScreen, i.e. the exact view and
// projection used for rendering. Projecting three points placed relative to the head view tells us
//   (a) whether the rendered view really is the head view (centre point lands on screen centre), and
//   (b) the true tan(half-FOV) horizontally and vertically (no assumptions about aspect handling).
struct ProjProbe {
    bool       resolved   = false;
    UFunction* fnProject  = nullptr;
    UObject*   localPlayer = nullptr;
    int        logged     = 0;
    float      lastTanH   = 0.0f, lastTanV = 0.0f;
    int        stable     = 0;
    uint64_t   lastFrame  = 0;
} g_projProbe;

bool ProjectWorldPoint(const FVector& world, float& sx, float& sy) {
    struct { FVector WorldLoc; float X; float Y; } p{world, -999.0f, -999.0f};
    static_assert(sizeof(p) == 0x14, "LocalPlayer.Project parms");
    g_origProcessEvent(g_projProbe.localPlayer, g_projProbe.fnProject, &p, nullptr);
    sx = p.X;
    sy = p.Y;
    return p.X > -900.0f;
}

void MeasureViewProjection() {
    ProjProbe& pr = g_projProbe;
    if (!pr.resolved) {
        pr.resolved  = true;
        pr.fnProject = FindFunction("Project", "LocalPlayer");
        if (pr.fnProject && pr.fnProject->ParmsSize != 0x14) {
            LOG_WARN("UE3Bridge: LocalPlayer.Project has ParmsSize 0x%X (expected 0x14); projection measurement disabled.", pr.fnProject->ParmsSize);
            pr.fnProject = nullptr;
        }
        if (!pr.fnProject) LOG_WARN("UE3Bridge: LocalPlayer.Project not found; projection measurement disabled.");
    }
    if (!pr.fnProject || !g_origProcessEvent || g_headViewFrame == 0) return;

    // Measure every frame for the first second after a change, then ~2x per second.
    const bool settling = pr.stable < 5 || (g_aimFromHandActive.load(std::memory_order_relaxed) && g_renderHeadViewOk < 60);
    if (!settling && g_gameFrameIdx - pr.lastFrame < 45) return;
    pr.lastFrame = g_gameFrameIdx;

    if (!IsLiveUObject(pr.localPlayer)) {
        pr.localPlayer = nullptr;
        static std::atomic<int32_t> s_lpIdx{-1};
        for (int32_t i = 0; i < g_gobjects->Num && !pr.localPlayer; ++i) {
            UObject* obj = g_gobjects->Data[i];
            if (!obj || !obj->Class || !HasSuperClassNamed(obj->Class, s_lpIdx, "LocalPlayer")) continue;
            if (SdkDumper::NameToString(g_gnames, obj->Name).rfind("Default__", 0) == 0) continue;
            pr.localPlayer = obj;
        }
        if (!pr.localPlayer) return;
        LOG_INFO("UE3Bridge: projection probe using %s", SdkDumper::GetFullObjectPath(g_gnames, pr.localPlayer).c_str());
    }

    const RotBasis b = BasisFromRotator(g_headViewRot);
    constexpr float kD = 2000.0f, kS = 1000.0f;
    const FVector c{g_headViewLoc.X + b.x.X * kD, g_headViewLoc.Y + b.x.Y * kD, g_headViewLoc.Z + b.x.Z * kD};
    const FVector r{c.X + b.y.X * kS, c.Y + b.y.Y * kS, c.Z + b.y.Z * kS};
    const FVector u{c.X + b.z.X * kS, c.Y + b.z.Y * kS, c.Z + b.z.Z * kS};
    float cx = 0.0f, cy = 0.0f, rx = 0.0f, ry = 0.0f, ux = 0.0f, uy = 0.0f;
    g_probingProjection = true;
    const bool projOk = ProjectWorldPoint(c, cx, cy) && ProjectWorldPoint(r, rx, ry) && ProjectWorldPoint(u, ux, uy);
    g_probingProjection = false;
    if (!projOk) return;

    // Project returns either 0..1 or -1..1 screen coordinates depending on engine version.
    float half = 0.0f, centre = 0.0f;
    if (std::fabs(cx - 0.5f) < 0.06f && std::fabs(cy - 0.5f) < 0.06f) { half = 0.5f; centre = 0.5f; }
    else if (std::fabs(cx) < 0.12f && std::fabs(cy) < 0.12f)           { half = 1.0f; centre = 0.0f; }

    const bool centred = half > 0.0f;
    if (pr.logged < 12 || (!centred && (pr.logged % 30) == 0)) {
        LOG_INFO("UE3Bridge: projection probe: centre=(%.3f, %.3f) right=(%.3f, %.3f) up=(%.3f, %.3f) -> %s",
                 cx, cy, rx, ry, ux, uy, centred ? "rendered view == head view" : "rendered view is NOT the head view");
    }
    ++pr.logged;

    if (!centred) {
        g_renderHeadViewOk = 0;
        return;
    }
    ++g_renderHeadViewOk;

    const float dx = std::fabs(rx - cx) / half;
    const float dy = std::fabs(uy - cy) / half;
    if (dx < 0.02f || dy < 0.02f) return;
    const float tanH = (kS / kD) / dx;
    const float tanV = (kS / kD) / dy;
    if (tanH < 0.2f || tanH > 6.0f || tanV < 0.2f || tanV > 6.0f) return;
    pr.stable = (std::fabs(tanH - pr.lastTanH) < 0.02f && std::fabs(tanV - pr.lastTanV) < 0.02f) ? pr.stable + 1 : 0;
    pr.lastTanH = tanH;
    pr.lastTanV = tanV;
    if (pr.stable >= 3) {
        const bool changed = !g_measValid.load(std::memory_order_relaxed) ||
                             std::fabs(g_measTanH.load() - tanH) > 0.01f || std::fabs(g_measTanV.load() - tanV) > 0.01f;
        g_measTanH.store(tanH, std::memory_order_relaxed);
        g_measTanV.store(tanV, std::memory_order_relaxed);
        g_measValid.store(true, std::memory_order_relaxed);
        if (changed) {
            constexpr float kDeg = 57.2957795f;
            LOG_INFO("UE3Bridge: MEASURED game projection: horizontal FOV %.1f deg, vertical FOV %.1f deg (tanH=%.3f tanV=%.3f, V/H=%.3f)",
                     2.0f * std::atan(tanH) * kDeg, 2.0f * std::atan(tanV) * kDeg, tanH, tanV, tanV / tanH);
        }
    }
}

// Resolves and caches gameplay action UFunctions on PlayerController / ASAMUPlayerController.
void EnsureActionFunctionsResolved() {
    if (g_fnStartFire || !g_gobjects || !g_gobjects->Data) return;
    for (int32_t i = 0; i < g_gobjects->Num; ++i) {
        UObject* obj = g_gobjects->Data[i];
        if (!obj || !obj->Class || !obj->Outer) continue;
        if (SdkDumper::GetObjClassName(g_gnames, obj) != "Function") continue;
        std::string fnName    = SdkDumper::NameToString(g_gnames, obj->Name);
        std::string outerName = SdkDumper::NameToString(g_gnames, obj->Outer->Name);
        if (outerName == "ASAMUPlayerController" || outerName == "PlayerController") {
            if (fnName == "StartFire" && outerName == "ASAMUPlayerController") {
                g_fnStartFire = static_cast<UFunction*>(obj);
            } else if (fnName == "StopFire" && outerName == "PlayerController") {
                g_fnStopFire = static_cast<UFunction*>(obj);
            } else if (fnName == "RocketBoostKeyDown" && outerName == "ASAMUPlayerController") {
                g_fnRocketBoostKeyDown = static_cast<UFunction*>(obj);
            } else if (fnName == "PowerJumpKeyDown" && outerName == "ASAMUPlayerController") {
                g_fnPowerJumpKeyDown = static_cast<UFunction*>(obj);
            } else if (fnName == "PowerJumpKeyUp" && outerName == "ASAMUPlayerController") {
                g_fnPowerJumpKeyUp = static_cast<UFunction*>(obj);
            } else if (fnName == "StartSprinting" && outerName == "ASAMUPlayerController") {
                g_fnStartSprinting = static_cast<UFunction*>(obj);
            } else if (fnName == "StopSprinting" && outerName == "ASAMUPlayerController") {
                g_fnStopSprinting = static_cast<UFunction*>(obj);
            }
        }
    }
}

// Dispatches edge-triggered OpenXR button actions (Right Trigger = Fire Grapple, Left Trigger = Jet Boots/Jump,
// Right Grip = Power Jump Charge, Left Grip = Sprint) on PlayerController.
void ProcessControllerActionsOnPC(UObject* pcObj, bool inSelfTest) {
    if (!IsPlayerControllerInstance(pcObj)) return;
    auto* raw = reinterpret_cast<uint8_t*>(pcObj);
    const bool isAsamuPc = HasSuperClassNamed(pcObj->Class, g_asamuPcFnameIdx, "ASAMUPlayerController");

    // 1. Update equipped GrappleGun viewmodel transform on Pawn->Weapon if present
    UObject* pawnObj = *reinterpret_cast<UObject* const*>(raw + offsets::kController_Pawn);
    if (IsLiveUObject(pawnObj) &&
        pawnObj->Class->PropertiesSize >= static_cast<int32_t>(offsets::kPawn_MinSize) &&
        HasSuperClassNamed(pawnObj->Class, g_pawnFnameIdx, "Pawn")) {
        const auto* pawnRaw = reinterpret_cast<const uint8_t*>(pawnObj);
        UObject* weapObj = *reinterpret_cast<UObject* const*>(pawnRaw + offsets::kPawn_Weapon);
        (void)weapObj;   // the glove mesh is driven from the GrappleGun::SetPosition post-hook
    }

    // 2. Locomotion axes are injected in the PlayerTick PRE-hook (BeginGameplayFrame). Record what the
    //    script made of them (aForward/aStrafe are scaled by PlayerInput(); non-zero => consumed).
    UObject* piObj = *reinterpret_cast<UObject* const*>(raw + offsets::kPC_PlayerInput);
    if (IsLiveUObject(piObj) && piObj->Class->PropertiesSize >= static_cast<int32_t>(offsets::kPlayerInput_MinSize)) {
        const auto* piRaw = reinterpret_cast<const uint8_t*>(piObj);
        std::lock_guard lock(g_poseMutex);
        g_inputDiag.postForward = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aForward);
        g_inputDiag.postStrafe  = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aStrafe);
    }

    // 3. Edge-triggered OpenXR gameplay actions (Right Trigger, Left Trigger, Grips)
    if (!samu::config::Get().enableControllerInput) return;
    MotionControllerInput inp = UE3Bridge::GetControllerInput();
    if (!inp.active) return;

    EnsureActionFunctionsResolved();

    // Right Trigger -> Fire Grapple (StartFire / StopFire)
    if (inp.rightTriggerPressed != g_prevRightTriggerPress) {
        g_prevRightTriggerPress = inp.rightTriggerPressed;
        uint8_t fireParms[16]{};
        if (inp.rightTriggerPressed && g_fnStartFire) {
            // The grapple fires along the script-side aim (CameraCache); refresh it with the newest hand pose.
            if (!inSelfTest) UpdateScriptAim(pcObj);
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnStartFire, fireParms, nullptr);
            LogGrappleShot(inp.rightTrigger);
        } else if (!inp.rightTriggerPressed && g_fnStopFire) {
            LOG_INFO("[MotionControls] Right Trigger RELEASED (%.2f) -> StopFire(0) [Release Grapple]", inp.rightTrigger);
            if (!inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnStopFire, fireParms, nullptr);
        }
    }

    // Left Trigger / Jump Button -> Jet Boots / Jump (RocketBoostKeyDown)
    bool leftTrigOrJump = inp.leftTriggerPressed || inp.jumpPressed;
    if (leftTrigOrJump != g_prevLeftTriggerPress) {
        g_prevLeftTriggerPress = leftTrigOrJump;
        if (leftTrigOrJump && g_fnRocketBoostKeyDown) {
            LOG_INFO("[MotionControls] Left Trigger/Jump PRESSED (%.2f) -> RocketBoostKeyDown() [Jet Boots / Jump]", inp.leftTrigger);
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnRocketBoostKeyDown, nullptr, nullptr);
        }
    }

    // Right Grip -> Power Jump Charge (PowerJumpKeyDown / PowerJumpKeyUp)
    if (inp.rightGripPressed != g_prevRightGripPress) {
        g_prevRightGripPress = inp.rightGripPressed;
        if (inp.rightGripPressed && g_fnPowerJumpKeyDown) {
            LOG_INFO("[MotionControls] Right Grip PRESSED (%.2f) -> PowerJumpKeyDown()", inp.rightGrip);
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnPowerJumpKeyDown, nullptr, nullptr);
        } else if (!inp.rightGripPressed && g_fnPowerJumpKeyUp) {
            LOG_INFO("[MotionControls] Right Grip RELEASED (%.2f) -> PowerJumpKeyUp()", inp.rightGrip);
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnPowerJumpKeyUp, nullptr, nullptr);
        }
    }

    // Left Grip OR Left Stick Click (L3) while pushing forward -> Sprint (StartSprinting / StopSprinting)
    const bool stickForward = (inp.leftStickY > std::max(0.25f, samu::config::Get().thumbstickDeadzone));
    if (inp.leftThumbstickClickPressed && !g_prevLeftStickClick && stickForward) {
        g_stickSprintLatched = true;
    }
    g_prevLeftStickClick = inp.leftThumbstickClickPressed;
    if (!stickForward) {
        g_stickSprintLatched = false;
    }
    const bool sprintWanted = inp.leftGripPressed || g_stickSprintLatched ||
                              (inp.leftThumbstickClickPressed && stickForward);
    if (sprintWanted != g_prevLeftGripPress) {
        g_prevLeftGripPress = sprintWanted;
        if (sprintWanted && g_fnStartSprinting) {
            LOG_INFO("[MotionControls] Sprint START (%s) -> StartSprinting()",
                     inp.leftGripPressed ? "Left Grip" : "Left Stick Click");
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnStartSprinting, nullptr, nullptr);
        } else if (!sprintWanted && g_fnStopSprinting) {
            LOG_INFO("[MotionControls] Sprint STOP -> StopSprinting()");
            if (isAsamuPc && !inSelfTest && g_origProcessEvent) g_origProcessEvent(pcObj, g_fnStopSprinting, nullptr, nullptr);
        }
    }
}

void TryTriggerSdkDump() {
    static int s_lastLoggedGObj = -1;
    int curGNames = (g_gnames && g_gnames->Data) ? g_gnames->Num : -1;
    int curGObj   = (g_gobjects && g_gobjects->Data) ? g_gobjects->Num : -1;
    if (curGObj != s_lastLoggedGObj && (curGObj > 0 || s_lastLoggedGObj == -1)) {
        LOG_INFO("UE3Bridge poll: GNames.Num=%d (Data=%p), GObjects.Num=%d (Data=%p)",
                 curGNames, g_gnames ? static_cast<void*>(g_gnames->Data) : nullptr,
                 curGObj, g_gobjects ? static_cast<void*>(g_gobjects->Data) : nullptr);
        s_lastLoggedGObj = curGObj;
    }
    if (g_sdkDumped.load(std::memory_order_relaxed)) return;
    if (!SdkDumper::IsReadyToDump(g_gnames, g_gobjects)) return;

    std::lock_guard lock(g_dumpMutex);
    if (g_sdkDumped.load(std::memory_order_relaxed)) return;

    std::wstring repoDir      = L"C:\\Users\\alexa\\Desktop\\SAMU-VR";
    std::wstring repoDumpDir  = repoDir + L"\\sdk_dump";
    std::wstring checkFile    = repoDumpDir + L"\\GrappleGlove_SDK.h";
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    const bool hasDevWorkspace = (GetFileAttributesW(repoDir.c_str()) != INVALID_FILE_ATTRIBUTES);
    bool alreadyHasFullDump = GetFileAttributesExW(checkFile.c_str(), GetFileExInfoStandard, &fad) &&
                              fad.nFileSizeLow > 20000;

    if (!alreadyHasFullDump && (hasDevWorkspace || IsRunningNullRhi())) {
        std::wstring localDumpDir = samu::log::ModuleDirectory() + L"\\sdk_dump";
        SdkDumper::DumpSdk(g_gnames, g_gobjects, g_moduleBase,
                           reinterpret_cast<uintptr_t>(g_gnames),
                           reinterpret_cast<uintptr_t>(g_gobjects),
                           reinterpret_cast<uintptr_t>(g_processEventTarget),
                           hasDevWorkspace ? repoDumpDir : localDumpDir);
    } else if (alreadyHasFullDump) {
        LOG_INFO("UE3Bridge: existing complete SDK dump verified in '%ls' (%lu bytes in GrappleGlove_SDK.h; live GNames=%d, GObjects=%d).",
                 repoDumpDir.c_str(), static_cast<unsigned long>(fad.nFileSizeLow),
                 g_gnames ? g_gnames->Num : -1, g_gobjects ? g_gobjects->Num : -1);
    } else {
        LOG_INFO("UE3Bridge: live UE3 reflection ready (GNames=%d, GObjects=%d).",
                 g_gnames ? g_gnames->Num : -1, g_gobjects ? g_gobjects->Num : -1);
    }

    g_sdkDumped.store(true, std::memory_order_release);

    const bool isNullRhi = IsRunningNullRhi();

    // Self-test verification: ONLY when running headless (-nullrhi), dispatch synthetic CDO events
    // to verify end-to-end MinHook interception without mutating CDOs during live interactive gameplay.
    if (isNullRhi && g_playerTicks.load(std::memory_order_relaxed) == 0 && g_gobjects && g_gobjects->Data) {
        SyntheticSelfTestGuard selfTestGuard;
        UObject* defaultPC      = nullptr;
        UObject* defaultGrapple = nullptr;
        UObject* defaultPI      = nullptr;
        UFunction* playerTickFn = nullptr;
        UFunction* fallAnimFn   = nullptr;
        UFunction* camShakeFn   = nullptr;
        UFunction* getPovFn     = nullptr;
        UFunction* setPosFn     = nullptr;
        UFunction* getAimFn     = nullptr;
        UFunction* getStartFn   = nullptr;
        UFunction* calcFireFn   = nullptr;
        UFunction* playerInpFn  = nullptr;

        for (int32_t i = 0; i < g_gobjects->Num; ++i) {
            UObject* obj = g_gobjects->Data[i];
            if (!obj || !obj->Class) continue;
            std::string n = SdkDumper::NameToString(g_gnames, obj->Name);
            if (!defaultPC && n == "Default__ASAMUPlayerController") {
                defaultPC = obj;
            } else if (!defaultGrapple && n == "Default__GrappleGun") {
                defaultGrapple = obj;
            } else if (!defaultPI && n == "Default__ASAMUPlayerInput") {
                defaultPI = obj;
            } else if (SdkDumper::GetObjClassName(g_gnames, obj) == "Function" && obj->Outer) {
                std::string outerName = SdkDumper::NameToString(g_gnames, obj->Outer->Name);
                if (!playerTickFn && n == "PlayerTick" && outerName == "PlayerController") {
                    playerTickFn = static_cast<UFunction*>(obj);
                } else if (!fallAnimFn && n == "FallAnimation" && outerName == "ASAMUPlayerController") {
                    fallAnimFn = static_cast<UFunction*>(obj);
                } else if (!camShakeFn && n == "StartCameraShake" && outerName == "ASAMUPlayerController") {
                    camShakeFn = static_cast<UFunction*>(obj);
                } else if (!getPovFn && n == "GetPlayerViewPoint" && outerName == "ASAMUPlayerController") {
                    getPovFn = static_cast<UFunction*>(obj);
                } else if (!setPosFn && n == "SetPosition" && outerName == "GrappleGun") {
                    setPosFn = static_cast<UFunction*>(obj);
                } else if (!getAimFn && n == "GetAdjustedAim" && outerName == "Weapon") {
                    getAimFn = static_cast<UFunction*>(obj);
                } else if (!getStartFn && n == "GetPhysicalFireStartLoc" && outerName == "Weapon") {
                    getStartFn = static_cast<UFunction*>(obj);
                } else if (!calcFireFn && n == "CalcWeaponFire" && outerName == "Weapon") {
                    calcFireFn = static_cast<UFunction*>(obj);
                } else if (!playerInpFn && n == "PlayerInput" && outerName == "ASAMUPlayerInput") {
                    playerInpFn = static_cast<UFunction*>(obj);
                }
            }
        }
        if (defaultPC && playerTickFn) {
            auto vtblPe = reinterpret_cast<PFN_ProcessEvent>(defaultPC->VfTableObject[rva::kProcessEventVIndex]);
            PlayerController_eventPlayerTick_Parms parms{0.016667f};
            LOG_INFO("UE3Bridge: verifying UObject::ProcessEvent(PlayerTick) + 6DOF Camera Decoupling on %s...",
                     SdkDumper::GetFullObjectPath(g_gnames, defaultPC).c_str());
            vtblPe(defaultPC, playerTickFn, &parms, nullptr);

            if (fallAnimFn) {
                vtblPe(defaultPC, fallAnimFn, nullptr, nullptr);
            }
            if (camShakeFn) {
                vtblPe(defaultPC, camShakeFn, nullptr, nullptr);
            }
            if (getPovFn) {
                struct {
                    FVector  POVLocation{100.0f, 200.0f, 300.0f};
                    FRotator POVRotation{0, 0, 0};
                } povParms;
                vtblPe(defaultPC, getPovFn, &povParms, nullptr);
                LOG_INFO("UE3Bridge: verified GetPlayerViewPoint 6DOF override -> POVLoc=(%.1f, %.1f, %.1f) UU, POVRot=(P=%d Y=%d R=%d)",
                         povParms.POVLocation.X, povParms.POVLocation.Y, povParms.POVLocation.Z,
                         povParms.POVRotation.Pitch, povParms.POVRotation.Yaw, povParms.POVRotation.Roll);
            }

            // Phase 4 Self-Test: Verify 6DOF Grapple Glove viewmodel transform & grapple beam spawn trace overrides
            if (defaultGrapple) {
                if (setPosFn) {
                    struct { UObject* Holder = nullptr; } setPosParms;
                    vtblPe(defaultGrapple, setPosFn, &setPosParms, nullptr);
                }
                if (getAimFn) {
                    struct {
                        FVector  StartFireLoc{0.0f, 0.0f, 0.0f};
                        FRotator ReturnValue{0, 0, 0};
                    } aimParms;
                    vtblPe(defaultGrapple, getAimFn, &aimParms, nullptr);
                    LOG_INFO("UE3Bridge: verified Phase 4 GrappleGun::GetAdjustedAim 6DOF override -> AimRot=(P=%d Y=%d R=%d URot)",
                             aimParms.ReturnValue.Pitch, aimParms.ReturnValue.Yaw, aimParms.ReturnValue.Roll);
                }
                if (getStartFn) {
                    struct {
                        FVector AimDir{1.0f, 0.0f, 0.0f};
                        FVector ReturnValue{0.0f, 0.0f, 0.0f};
                    } startParms;
                    vtblPe(defaultGrapple, getStartFn, &startParms, nullptr);
                    LOG_INFO("UE3Bridge: verified Phase 4 GrappleGun::GetPhysicalFireStartLoc 6DOF override -> MuzzleLoc=(%.1f, %.1f, %.1f) UU",
                             startParms.ReturnValue.X, startParms.ReturnValue.Y, startParms.ReturnValue.Z);
                }
                if (calcFireFn) {
                    struct {
                        FVector StartTrace{0.0f, 0.0f, 0.0f};
                        FVector EndTrace{4500.0f, 0.0f, 0.0f};
                        uint8_t Rest[0x80 - 0x18]{};
                    } fireParms;
                    vtblPe(defaultGrapple, calcFireFn, &fireParms, nullptr);
                    GrappleGloveDiag gdiag = UE3Bridge::GetLastGrappleDiag();
                    LOG_INFO("UE3Bridge: verified Phase 4 GrappleGun::CalcWeaponFire 6DOF trace -> Start=(%.1f, %.1f, %.1f) UU, End=(%.1f, %.1f, %.1f) UU | "
                             "GloveWorldLoc=(%.1f, %.1f, %.1f) UU, GloveWorldRot=(P=%d Y=%d R=%d)",
                             fireParms.StartTrace.X, fireParms.StartTrace.Y, fireParms.StartTrace.Z,
                             fireParms.EndTrace.X, fireParms.EndTrace.Y, fireParms.EndTrace.Z,
                             gdiag.gloveWorldLocation.X, gdiag.gloveWorldLocation.Y, gdiag.gloveWorldLocation.Z,
                             gdiag.gloveWorldRotation.Pitch, gdiag.gloveWorldRotation.Yaw, gdiag.gloveWorldRotation.Roll);
                }
            }

            // Phase 4 Self-Test: Verify OpenXR thumbstick injection into ASAMUPlayerInput raw axes
            if (defaultPI) {
                (void)playerInpFn;
                InjectLocomotionIntoPlayerInput(defaultPI, 0);
                auto* piRaw = reinterpret_cast<uint8_t*>(defaultPI);
                float aFwd   = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aBaseY);
                float aStr   = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aStrafe);
                float aTurn  = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aTurn);
                float rTrig  = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aRightAnalogTrigger);
                float lTrig  = *reinterpret_cast<const float*>(piRaw + offsets::kPlayerInput_aLeftAnalogTrigger);
                LOG_INFO("UE3Bridge: verified Phase 4 OpenXR -> ASAMUPlayerInput raw-axis injection: aBaseY=%.2f, aStrafe=%.2f, aTurn=%.2f, "
                         "RightTrigger=%.2f (FireGrapple), LeftTrigger=%.2f (JetBoots/Jump)",
                         aFwd, aStr, aTurn, rTrig, lTrig);

                // Restore Default__ASAMUPlayerInput CDO fields to clean defaults after self-test
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_RawJoyUp)            = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_RawJoyRight)         = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_RawJoyLookRight)     = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_RawJoyLookUp)        = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aBaseY)              = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aForward)            = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aStrafe)             = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aTurn)               = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aLookUp)             = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aRightAnalogTrigger) = 0.0f;
                *reinterpret_cast<float*>(piRaw + offsets::kPlayerInput_aLeftAnalogTrigger)  = 0.0f;
                g_injFwd = g_injStrafe = g_injTurn = AxisInjState{};
            }

            // Clear synthetic startup test poses/inputs so live in-game play only uses real OpenXR tracking
            {
                std::lock_guard poseLock(g_poseMutex);
                g_rawHmdPose.positionValid      = false;
                g_rawHmdPose.orientationValid   = false;
                g_rawRightAimPose.positionValid = false;
                g_rawRightAimPose.orientationValid = false;
                g_rawLeftAimPose.positionValid  = false;
                g_rawLeftAimPose.orientationValid  = false;
                g_controllerInput               = MotionControllerInput{};
                g_prevRightTriggerPress         = false;
                g_prevLeftTriggerPress          = false;
                g_prevRightGripPress            = false;
                g_prevLeftGripPress             = false;
            }
        }
    }
}

// __fastcall detour matching __thiscall UObject::ProcessEvent(UFunction*, void*, void*)
void __fastcall Detour_ProcessEvent(UObject* self, void* /*edx*/, UFunction* func, void* parms, void* result) {
    uint64_t callNum = ++g_totalEvents;

    if (callNum <= 5) {
        std::string objPath = SdkDumper::GetFullObjectPath(g_gnames, self);
        std::string fnPath  = SdkDumper::GetFullObjectPath(g_gnames, func);
        LOG_INFO("[ProcessEvent] call #%llu: self=%p (%s) | func=%p (%s) | GNames=%d GObjects=%d",
                 static_cast<unsigned long long>(callNum),
                 static_cast<void*>(self), objPath.c_str(),
                 static_cast<void*>(func), fnPath.c_str(),
                 g_gnames ? g_gnames->Num : -1,
                 g_gobjects ? g_gobjects->Num : -1);
    }

    const bool inSelfTest = g_inSyntheticSelfTest.load(std::memory_order_relaxed);
    bool isPlayerTick = false;
    bool isCdo        = false;
    bool vrActive     = false;
    std::string_view fnNameView;

    if (self && func && g_gnames && g_gnames->Data) {
        if (!inSelfTest && !g_sdkDumped.load(std::memory_order_relaxed) && (callNum == 1 || (callNum & 0xFF) == 0)) {
            TryTriggerSdkDump();
        }

        int32_t selfIdx = self->Name.Index;
        if (g_gnames->IsValidIndex(selfIdx) && g_gnames->Data[selfIdx] && !g_gnames->Data[selfIdx]->IsWide()) {
            std::string_view sv = g_gnames->Data[selfIdx]->AnsiName;
            isCdo = (sv.rfind("Default__", 0) == 0);
        }

        // In live gameplay (not our -nullrhi CDO self-test), ALWAYS forward Default__* CDO calls
        // untouched to UE3 (e.g. Default__ASAMUGameInfo::SetGameType sets WorldInfo->Game on map load).
        if (isCdo && !inSelfTest) {
            g_origProcessEvent(self, func, parms, result);
            return;
        }
        vrActive = inSelfTest || IsGameplayActiveNow();

        int32_t fnIdx = func->Name.Index;
        if (func->Name.Number == 0 && g_gnames->IsValidIndex(fnIdx)) {
            const FNameEntry* entry = g_gnames->Data[fnIdx];
            if (entry && !entry->IsWide()) {
                fnNameView = entry->AnsiName;
            }
        }

        int32_t ptIdx = g_playerTickFnameIdx.load(std::memory_order_relaxed);
        if (ptIdx >= 0) {
            isPlayerTick = (fnIdx == ptIdx && func->Name.Number == 0);
        } else if (fnNameView == "PlayerTick") {
            g_playerTickFnameIdx.store(fnIdx, std::memory_order_relaxed);
            isPlayerTick = true;
            LOG_INFO("UE3Bridge: resolved FName 'PlayerTick' -> Index=%d", fnIdx);
        }

        // Phase 3: Toggleable Camera-Lock Suppressor
        if (vrActive && !isPlayerTick && !fnNameView.empty() && samu::config::IsCameraLockSuppressorEnabled()) {
            if (IsScriptedCameraLockEvent(fnNameView, func->ParmsSize, parms)) {
                uint64_t supCount = ++g_suppressedCameraEvents;
                if (supCount <= 10 || (supCount & 0x3F) == 0) {
                    std::string objPath = SdkDumper::GetFullObjectPath(g_gnames, self);
                    std::string fnPath  = SdkDumper::GetFullObjectPath(g_gnames, func);
                    LOG_INFO("[CameraLockSuppressor] Blocked scripted camera event #%llu: %s on %s",
                             static_cast<unsigned long long>(supCount), fnPath.c_str(), objPath.c_str());
                }
                return;
            }
        }

        // Phase 4 Pre-Hook: Override Weapon::CalcWeaponFire (ParmsSize == 0x80: StartTrace +0x00, EndTrace +0x0C)
        // so grapple beam raycasts start at the 6DOF Right Hand and trace along the Right Aim Pose ray!
        if (vrActive && fnNameView == "CalcWeaponFire" && parms && func->ParmsSize >= 0x18 &&
            samu::config::Get().enable6DofGrapple &&
            self->Class && HasSuperClassNamed(self->Class, g_weaponFnameIdx, "Weapon")) {
            FVector  worldLoc{};
            FRotator worldRot{};
            FVector  worldDir{};
            FVector  relOffset{};
            if (ComputeRightAimWorldPose(worldLoc, worldRot, worldDir, relOffset)) {
                auto* startTrace = reinterpret_cast<FVector*>(static_cast<uint8_t*>(parms) + 0x00);
                auto* endTrace   = reinterpret_cast<FVector*>(static_cast<uint8_t*>(parms) + 0x0C);
                const float dx = endTrace->X - startTrace->X;
                const float dy = endTrace->Y - startTrace->Y;
                const float dz = endTrace->Z - startTrace->Z;
                float traceLen = std::sqrt(dx * dx + dy * dy + dz * dz);
                if (traceLen < 10.0f) {
                    traceLen = samu::config::Get().grappleRangeUU;
                }
                *startTrace = worldLoc;
                *endTrace   = FVector{
                    worldLoc.X + worldDir.X * traceLen,
                    worldLoc.Y + worldDir.Y * traceLen,
                    worldLoc.Z + worldDir.Z * traceLen
                };
                {
                    std::lock_guard lock(g_poseMutex);
                    g_lastGrappleDiag.aimRayOrigin     = worldLoc;
                    g_lastGrappleDiag.aimRayDirection  = worldDir;
                    g_lastGrappleDiag.calcTraceStart   = *startTrace;
                    g_lastGrappleDiag.calcTraceEnd     = *endTrace;
                    g_lastGrappleDiag.active           = true;
                }
                g_grappleAimOverrides.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    // Diagnostics: discover which camera / view / aim events the game dispatches through ProcessEvent.
    if (vrActive && !inSelfTest && !fnNameView.empty() && GetCurrentThreadId() == g_gameThreadId.load(std::memory_order_relaxed) &&
        (fnNameView.find("View") != std::string_view::npos || fnNameView.find("Camera") != std::string_view::npos ||
         fnNameView.find("Aim") != std::string_view::npos || fnNameView.find("Fire") != std::string_view::npos ||
         fnNameView.find("Trace") != std::string_view::npos || fnNameView.find("FOV") != std::string_view::npos ||
         fnNameView == "SetPosition")) {
        uint32_t& n = g_camEventCounts[func];
        if (++n == 1 && g_camEventCounts.size() <= 80) {
            LOG_INFO("[CamEvent] first call: %s on %s (ParmsSize=0x%X)",
                     SdkDumper::GetFullObjectPath(g_gnames, func).c_str(),
                     SdkDumper::GetFullObjectPath(g_gnames, self).c_str(), func->ParmsSize);
        }
    }

    // Pre-suppress landing/fall CameraAnim & ShakeOffset BEFORE Camera::UpdateCamera evaluates CameraCache.POV.
    if (vrActive && !inSelfTest && fnNameView == "UpdateCamera" && samu::config::IsCameraLockSuppressorEnabled() &&
        self->Class && self->Class->PropertiesSize >= static_cast<int32_t>(offsets::kCamera_MinSize) &&
        HasSuperClassNamed(self->Class, g_cameraFnameIdx, "Camera")) {
        UObject* owner = *reinterpret_cast<UObject* const*>(
            reinterpret_cast<const uint8_t*>(self) + offsets::kCamera_PCOwner);
        SuppressActiveCameraShakesOnPC(owner);
    }

    // PlayerTick PRE-hook: decide gameplay state, then latch this frame's pose + inject locomotion axes
    // before the script's PlayerInput()/PlayerMove() consume them.
    bool tickGameplay = false;
    if (isPlayerTick && IsPlayerControllerInstance(self)) {
        std::string_view mapName;
        tickGameplay = inSelfTest || EvaluateGameplayState(self, mapName);
        if (tickGameplay) {
            BeginGameplayFrame(self, mapName, inSelfTest);
        } else {
            EndGameplayFrames();
        }
    }

    // Execute original UE3 ProcessEvent (skip UnrealScript bytecode only during -nullrhi CDO self-tests)
    if (!inSelfTest) {
        g_origProcessEvent(self, func, parms, result);
    }

    if (!self || !func) return;

    // Post-ProcessEvent Phase 3 & Phase 4 hooks:
    // 1. PlayerTick: suppress active camera shake/anim blend weights, apply 6DOF HMD pose to PlayerController/Camera,
    //    and process 6DOF Grapple Glove + OpenXR PlayerInput actions.
    if (isPlayerTick) {
        if (!IsPlayerControllerInstance(self)) return;
        if (tickGameplay) {
            if (samu::config::IsCameraLockSuppressorEnabled()) {
                SuppressActiveCameraShakesOnPC(self);
            }
            Apply6DofCameraToPlayerController(self);
            ProcessControllerActionsOnPC(self, inSelfTest);
            if (!inSelfTest) UpdateHudForVr(self);
            if (!inSelfTest) MeasureViewProjection();
        }

        uint64_t tickNum = ++g_playerTicks;
        float dt = parms ? static_cast<const PlayerController_eventPlayerTick_Parms*>(parms)->DeltaTime : 0.0f;

        static auto s_lastLogTime = std::chrono::steady_clock::now();
        static uint64_t s_ticksSinceLog = 0;
        ++s_ticksSinceLog;

        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - s_lastLogTime).count();

        if (tickNum <= 5 || elapsed >= 1.0) {
            std::string objPath = SdkDumper::GetFullObjectPath(g_gnames, self);
            DecoupledCameraDiag diag = UE3Bridge::GetLastCameraDiag();
            GrappleGloveDiag gdiag   = UE3Bridge::GetLastGrappleDiag();
            double hz = (elapsed > 1e-4 && tickNum > 5) ? (s_ticksSinceLog / elapsed) : (dt > 1e-6f ? 1.0 / dt : 0.0);
            LOG_INFO("[ProcessEvent] PlayerTick #%llu | dt=%.4f s (~%.1f Hz) | self=%s | "
                     "CamLoc=(%.1f, %.1f, %.1f) UU [HMDd=(%+.1f, %+.1f, %+.1f) UU] | "
                     "CamRot=(P=%d Y=%d R=%d) | FOV=%.1f | GloveAimOverrides=%llu GloveXform=%llu InputInj=%llu",
                     static_cast<unsigned long long>(tickNum), dt, hz, objPath.c_str(),
                     diag.finalLocation.X, diag.finalLocation.Y, diag.finalLocation.Z,
                     diag.hmdOffsetUU.X, diag.hmdOffsetUU.Y, diag.hmdOffsetUU.Z,
                     diag.finalRotation.Pitch, diag.finalRotation.Yaw, diag.finalRotation.Roll,
                     diag.finalFovDeg,
                     static_cast<unsigned long long>(g_grappleAimOverrides.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_gloveTransformOverrides.load(std::memory_order_relaxed)),
                     static_cast<unsigned long long>(g_playerInputInjections.load(std::memory_order_relaxed)));
            (void)gdiag;
            static int s_camDumpCountdown = 5;
            if (elapsed >= 1.0 && --s_camDumpCountdown <= 0 && !g_camEventCounts.empty()) {
                s_camDumpCountdown = 10;
                std::string line;
                for (const auto& kv : g_camEventCounts) {
                    if (line.size() > 900) break;
                    if (!IsLiveUObject(kv.first)) continue;   // function unloaded with its level/package
                    line += SdkDumper::NameToString(g_gnames, kv.first->Outer ? kv.first->Outer->Name : kv.first->Name);
                    line += '.';
                    line += SdkDumper::NameToString(g_gnames, kv.first->Name);
                    line += '=';
                    line += std::to_string(kv.second);
                    line += ' ';
                }
                LOG_INFO("[CamEvent] totals (tick %llu): %s", static_cast<unsigned long long>(tickNum), line.c_str());
            }
            if (elapsed >= 1.0) {
                s_lastLogTime = now;
                s_ticksSinceLog = 0;
            }
        }
        return;
    }

    // Track in-game pause menu open/close with zero delay via ASAMUHUD.PostRender (bPauseMenuFade / PauseMenuASAMU.bMovieIsOpen),
    // because TogglePauseMenu runs inside UnrealScript bytecode and would otherwise wait 1000ms for PlayerTick timeout.
    if (!inSelfTest && g_gameplayActive.load(std::memory_order_relaxed)) {
        if (fnNameView == "PostRender" && self == g_hudRig.lastHud && IsLiveUObject(self)) {
            bool pausedNow = false;
            const auto* hudRaw = reinterpret_cast<const uint8_t*>(self);
            if (g_hudRig.propPauseFade) {
                const uint32_t bits = *reinterpret_cast<const uint32_t*>(hudRaw + g_hudRig.propPauseFade->Offset);
                pausedNow = (bits & g_hudRig.propPauseFade->BitMask) != 0;
            }
            if (!pausedNow && g_hudRig.propPauseMovie) {
                UObject* pm = *reinterpret_cast<UObject* const*>(hudRaw + g_hudRig.propPauseMovie->Offset);
                if (IsLiveUObject(pm)) {
                    if (!g_hudRig.propMovieIsOpen) {
                        g_hudRig.propMovieIsOpen = static_cast<UBoolProperty*>(
                            FindPropertyInClassChain(pm->Class, "bMovieIsOpen", "BoolProperty"));
                    }
                    if (g_hudRig.propMovieIsOpen) {
                        const uint32_t bits = *reinterpret_cast<const uint32_t*>(
                            reinterpret_cast<const uint8_t*>(pm) + g_hudRig.propMovieIsOpen->Offset);
                        pausedNow = (bits & g_hudRig.propMovieIsOpen->BitMask) != 0;
                    }
                }
            }
            const bool prevPaused = g_gamePaused.exchange(pausedNow, std::memory_order_relaxed);
            if (pausedNow && !prevPaused) {
                g_pauseStartedMs.store(SteadyNowMs(), std::memory_order_relaxed);
                LOG_INFO("UE3Bridge: pause menu OPENED -> immediate transition to pause screen");
            } else if (!pausedNow && prevPaused) {
                LOG_INFO("UE3Bridge: pause menu CLOSED -> immediate transition back to VR");
            }
            return;
        }
        if (fnNameView == "TogglePauseMenu") {
            if (!g_gamePaused.exchange(true, std::memory_order_relaxed)) {
                g_pauseStartedMs.store(SteadyNowMs(), std::memory_order_relaxed);
                LOG_INFO("UE3Bridge: pause menu OPENED -> immediate transition to pause screen");
            }
            return;
        }
        if (fnNameView == "CompletePauseMenuClose") {
            if (g_gamePaused.exchange(false, std::memory_order_relaxed)) {
                LOG_INFO("UE3Bridge: pause menu CLOSED -> immediate transition back to VR");
            }
            return;
        }
    }

    // All remaining VR overrides only apply during immersive gameplay (menus/loading stay flat & untouched).
    if (!vrActive) return;

    // 2. GetPlayerViewPoint (PlayerController) / GetCameraViewPoint (Camera): FVector& out_Loc +0x00, FRotator& out_Rot +0x0C.
    //    Calls arriving through ProcessEvent come from NATIVE code (the renderer's CalcSceneView, audio listener,
    //    streaming...). They always get the head view. Script code reads CameraCache directly instead, which is
    //    what lets the game's aim be pointed along the hand while the picture stays on the head.
    if ((fnNameView == "GetPlayerViewPoint" || fnNameView == "GetCameraViewPoint") &&
        parms && func->ParmsSize >= 0x18 && samu::config::Get().decoupleCamera) {
        UObject* pcObj = nullptr;
        if (fnNameView == "GetPlayerViewPoint") {
            if (IsPlayerControllerInstance(self)) pcObj = self;
        } else if (self->Class && HasSuperClassNamed(self->Class, g_cameraFnameIdx, "Camera")) {
            UObject* owner = *reinterpret_cast<UObject* const*>(reinterpret_cast<uint8_t*>(self) + offsets::kCamera_PCOwner);
            if (IsPlayerControllerInstance(owner)) pcObj = owner;
        }
        if (pcObj) {
            auto* outLoc = reinterpret_cast<FVector*>(static_cast<uint8_t*>(parms) + 0x00);
            auto* outRot = reinterpret_cast<FRotator*>(static_cast<uint8_t*>(parms) + 0x0C);
            if (g_headViewFrame != 0 && g_gameFrameIdx - g_headViewFrame <= 2) {
                *outLoc = g_headViewLoc;                 // computed by the UpdateCamera hook this frame
                *outRot = g_headViewRot;
                ++g_viewOverridesThisFrame;

            } else {
                AdjustGameView(pcObj, *outLoc, *outRot); // no camera actor updating: adjust the game's own answer
            }
            g_viewPointOverrides.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // 2b. Camera::UpdateCamera(float DeltaTime): native per-frame camera update. Compute the head view from the
    //     game's fresh camera, then store in CameraCache.POV either the head view or (aim-from-hand) the hand aim.
    if (fnNameView == "UpdateCamera" && samu::config::Get().decoupleCamera &&
        self->Class && self->Class->PropertiesSize >= static_cast<int32_t>(offsets::kCamera_MinSize) &&
        HasSuperClassNamed(self->Class, g_cameraFnameIdx, "Camera")) {
        auto* camRaw = reinterpret_cast<uint8_t*>(self);
        UObject* owner = *reinterpret_cast<UObject* const*>(camRaw + offsets::kCamera_PCOwner);
        if (IsPlayerControllerInstance(owner)) {
            auto* loc = reinterpret_cast<FVector*>(camRaw + offsets::kCamera_CameraCache_POV_Loc);
            auto* rot = reinterpret_cast<FRotator*>(camRaw + offsets::kCamera_CameraCache_POV_Rot);
            auto* fov = reinterpret_cast<float*>(camRaw + offsets::kCamera_CameraCache_POV_FOV);
            static int64_t s_lastFovLogMs = 0;
            const float gameFov = *fov;
            // A constrained aspect ratio letterboxes the scene inside the (square) backbuffer; never wanted in VR.
            auto* camFlags = reinterpret_cast<uint32_t*>(camRaw + offsets::kCamera_Flags);
            *camFlags &= ~0x02u;

            g_lastAdjustedValid = false;                 // the cache was just recomputed by the game
            AdjustGameView(owner, *loc, *rot);
            g_headViewLoc    = *loc;
            g_headViewRot    = *rot;
            g_headViewIsPawn = g_lastViewWasPawn;
            g_headViewFrame  = g_gameFrameIdx;
            *fov = VrFovOr(*fov);

            // Lock g_lastCameraDiag to this frame's exact rendered eye base so fast falls / landings keep the
            // VR hand mesh and aim ray aligned with the head view on the very same frame (zero 1-frame fall lag).
            if (g_lastViewWasPawn) {
                const int32_t bodyYawNow = BodyYawFor(owner);
                const FVector hmdW       = HeadOffsetWorld(bodyYawNow);
                const FVector eyeBaseNow{loc->X - hmdW.X, loc->Y - hmdW.Y, loc->Z - hmdW.Z};
                std::lock_guard lock(g_poseMutex);
                g_lastCameraDiag.baseLocation  = eyeBaseNow;
                g_lastCameraDiag.hmdOffsetUU   = hmdW;
                g_lastCameraDiag.finalLocation = *loc;
                g_lastCameraDiag.finalRotation = *rot;
                g_lastCameraDiag.bodyYaw       = bodyYawNow;
                g_lastCameraDiag.finalFovDeg   = *fov;
                g_lastCameraDiag.active        = true;
            }

            // Native view queries (renderer, audio) are answered with the head view saved above. Script code reads
            // CameraCache.POV directly, so its rotation is pointed along the right hand: the grapple, the
            // "can I grapple this" check and interactions all follow the hand instead of the head.
            g_inTickPhase = false;
            g_aimRig.camera = self;
            UpdateScriptAim(owner);
            if (g_handRig.learned && samu::config::Get().enable6DofGrapple && IsLiveUObject(g_aimRig.gun)) {
                Apply6DofGloveTransformToWeapon(g_aimRig.gun, nullptr);
            }
            const bool handAim = g_aimFromHandActive.load(std::memory_order_relaxed);

            const int64_t nowMs = SteadyNowMs();
            if (nowMs - s_lastFovLogMs > 5000) {
                s_lastFovLogMs = nowMs;
                LOG_INFO("UE3Bridge: UpdateCamera: game FOV %.1f -> VR FOV %.1f | eyeFromPawn=(%.1f, %.1f, %.1f) | pawnView=%d | aimFromHand=%d",
                         gameFov, *fov, g_eyeFromPawn.X, g_eyeFromPawn.Y, g_eyeFromPawn.Z, g_headViewIsPawn ? 1 : 0, handAim ? 1 : 0);
            }
            g_updateCameraOverrides.fetch_add(1, std::memory_order_relaxed);
            g_cameraDecoupleUpdates.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }

    // 2c. GetFOVAngle() (float ReturnValue +0x00) on the PlayerController / Camera: the renderer's FOV query.
    if (fnNameView == "GetFOVAngle" && parms && func->ParmsSize >= 0x04 && samu::config::Get().decoupleCamera &&
        self->Class && (IsPlayerControllerInstance(self) || HasSuperClassNamed(self->Class, g_cameraFnameIdx, "Camera"))) {
        auto* ret = reinterpret_cast<float*>(parms);
        *ret = VrFovOr(*ret);
        g_fovOverrides.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // 3. Raise dialogue / voice-over subtitles cleanly into the lower-third VR optical sweet spot.
    if (fnNameView == "GetSubtitleRegion" && parms && func->ParmsSize >= 0x10) {
        const float lift = std::clamp(samu::config::GetHudTextLift(), 0.0f, 1.0f);
        if (lift > 0.001f) {
            auto* minPos = reinterpret_cast<float*>(static_cast<uint8_t*>(parms) + 0x00); // FVector2D MinPos {X, Y}
            auto* maxPos = reinterpret_cast<float*>(static_cast<uint8_t*>(parms) + 0x08); // FVector2D MaxPos {X, Y}
            const float bandH   = std::max(0.05f, maxPos[1] - minPos[1]);
            const float bottomY = std::clamp(1.0f - lift * 0.43f, 0.55f, 1.0f);
            const float tallH   = std::clamp(bandH + lift * 0.11f, 0.08f, 0.22f);
            maxPos[1] = bottomY;
            minPos[1] = std::clamp(bottomY - tallH, 0.38f, bottomY - 0.06f);
            const float marginX = std::clamp(0.18f + lift * 0.11f, 0.18f, 0.29f);
            minPos[0] = std::max(minPos[0], marginX);
            maxPos[0] = std::min(maxPos[0], 1.0f - marginX);
            static float s_lastLoggedLift = -1.0f;
            if (std::fabs(lift - s_lastLoggedLift) > 0.01f) {
                s_lastLoggedLift = lift;
                LOG_INFO("UE3Bridge: GetSubtitleRegion -> X=[%.2f..%.2f] Y=[%.2f..%.2f] (HudTextLift=%.2f)",
                         minPos[0], maxPos[0], minPos[1], maxPos[1], lift);
            }
        }
        return;
    }

    // 4. Phase 4: GrappleGun::SetPosition(UDKPawn* Holder +0x00) -> Override first-person glove viewmodel world transform
    if (fnNameView == "SetPosition" && samu::config::Get().enable6DofGrapple) {
        UObject* holder = (parms && func->ParmsSize >= 0x04)
                              ? *reinterpret_cast<UObject* const*>(static_cast<const uint8_t*>(parms) + 0x00)
                              : nullptr;
        Apply6DofGloveTransformToWeapon(self, holder);
        return;
    }

    // 5. Phase 4: Weapon::GetAdjustedAim (ParmsSize == 0x18: StartFireLoc +0x00, FRotator ReturnValue +0x0C)
    //             and Controller::GetAdjustedAimFor (ParmsSize == 0x1C: W +0x00, StartFireLoc +0x04, FRotator ReturnValue +0x10)
    if (parms && samu::config::Get().enable6DofGrapple && self->Class &&
        ((fnNameView == "GetAdjustedAim" && HasSuperClassNamed(self->Class, g_weaponFnameIdx, "Weapon")) ||
         (fnNameView == "GetAdjustedAimFor" && HasSuperClassNamed(self->Class, g_controllerFnameIdx, "Controller")))) {
        FVector  worldLoc{};
        FRotator worldRot{};
        FVector  worldDir{};
        FVector  relOffset{};
        if (ComputeRightAimWorldPose(worldLoc, worldRot, worldDir, relOffset)) {
            if (fnNameView == "GetAdjustedAim" && func->ParmsSize >= 0x18) {
                *reinterpret_cast<FRotator*>(static_cast<uint8_t*>(parms) + 0x0C) = worldRot;
                g_grappleAimOverrides.fetch_add(1, std::memory_order_relaxed);
            } else if (fnNameView == "GetAdjustedAimFor" && func->ParmsSize >= 0x1C) {
                *reinterpret_cast<FRotator*>(static_cast<uint8_t*>(parms) + 0x10) = worldRot;
                g_grappleAimOverrides.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    // 6. Phase 4: Weapon::GetPhysicalFireStartLoc (ParmsSize == 0x18: AimDir +0x00, FVector ReturnValue +0x0C)
    //             and Weapon::GetMuzzleLoc (ParmsSize == 0x0C: FVector ReturnValue +0x00)
    if ((fnNameView == "GetPhysicalFireStartLoc" || fnNameView == "GetMuzzleLoc") &&
        parms && samu::config::Get().enable6DofGrapple &&
        self->Class && HasSuperClassNamed(self->Class, g_weaponFnameIdx, "Weapon")) {
        FVector  worldLoc{};
        FRotator worldRot{};
        FVector  worldDir{};
        FVector  relOffset{};
        if (ComputeRightAimWorldPose(worldLoc, worldRot, worldDir, relOffset)) {
            if (fnNameView == "GetPhysicalFireStartLoc" && func->ParmsSize >= 0x18) {
                *reinterpret_cast<FVector*>(static_cast<uint8_t*>(parms) + 0x0C) = worldLoc;
                g_grappleAimOverrides.fetch_add(1, std::memory_order_relaxed);
            } else if (fnNameView == "GetMuzzleLoc" && func->ParmsSize >= 0x0C) {
                *reinterpret_cast<FVector*>(static_cast<uint8_t*>(parms) + 0x00) = worldLoc;
                g_grappleAimOverrides.fetch_add(1, std::memory_order_relaxed);
            }
        }
        return;
    }

    // (Locomotion injection now happens in the PlayerTick pre-hook; PlayerInput() is a script-to-script call
    //  that never reaches ProcessEvent in live gameplay.)
}

bool InstallHookInternal() {
    MH_STATUS st = MH_Initialize();
    if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("UE3Bridge: MH_Initialize failed (%d)", static_cast<int>(st));
        return false;
    }

    st = MH_CreateHook(g_processEventTarget,
                       reinterpret_cast<LPVOID>(&Detour_ProcessEvent),
                       reinterpret_cast<LPVOID*>(&g_origProcessEvent));
    if (st != MH_OK) {
        LOG_ERROR("UE3Bridge: MH_CreateHook(ProcessEvent @ %p) failed (%d)",
                  g_processEventTarget, static_cast<int>(st));
        return false;
    }

    st = MH_EnableHook(g_processEventTarget);
    if (st != MH_OK) {
        LOG_ERROR("UE3Bridge: MH_EnableHook(ProcessEvent @ %p) failed (%d)",
                  g_processEventTarget, static_cast<int>(st));
        return false;
    }

    g_hooked.store(true, std::memory_order_release);
    LOG_INFO("UE3Bridge: MinHook detour active on UObject::ProcessEvent (target=%p, trampoline=%p).",
             g_processEventTarget, reinterpret_cast<void*>(g_origProcessEvent));
    return true;
}

} // namespace

bool UE3Bridge::Initialize() {
    if (g_hooked.load()) return true;
    if (!ResolveAddressesFromBinary()) return false;
    return InstallHookInternal();
}

bool UE3Bridge::InitializeWithTargets(TArray<FNameEntry*>* gnames,
                                      TArray<UObject*>* gobjects,
                                      void* processEventTarget) {
    if (g_hooked.load()) return true;
    g_moduleBase         = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    g_gnames             = gnames;
    g_gobjects           = gobjects;
    g_processEventTarget = processEventTarget;
    LOG_INFO("UE3Bridge: initialized with explicit targets (GNames=%p, GObjects=%p, ProcessEvent=%p)",
             static_cast<void*>(gnames), static_cast<void*>(gobjects), processEventTarget);
    return InstallHookInternal();
}

void UE3Bridge::Shutdown() {
    if (!g_hooked.exchange(false)) return;
    if (g_processEventTarget) {
        MH_DisableHook(g_processEventTarget);
        MH_RemoveHook(g_processEventTarget);
    }
    LOG_INFO("UE3Bridge: ProcessEvent hook removed (total events=%llu, PlayerTicks=%llu, CameraDecouple=%llu, "
             "SuppressedCamLocks=%llu, GrappleAimOverrides=%llu, GloveTransformOverrides=%llu, PlayerInputInjections=%llu).",
             static_cast<unsigned long long>(g_totalEvents.load()),
             static_cast<unsigned long long>(g_playerTicks.load()),
             static_cast<unsigned long long>(g_cameraDecoupleUpdates.load()),
             static_cast<unsigned long long>(g_suppressedCameraEvents.load()),
             static_cast<unsigned long long>(g_grappleAimOverrides.load()),
             static_cast<unsigned long long>(g_gloveTransformOverrides.load()),
             static_cast<unsigned long long>(g_playerInputInjections.load()));
}

bool UE3Bridge::IsGameplayActive() {
    return IsGameplayActiveNow();
}

void UE3Bridge::PollSdkDump() {
    // Only poll from background ModThread in headless -nullrhi mode; in live gameplay
    // TryTriggerSdkDump() runs on the Game Thread inside Detour_ProcessEvent.
    if (!g_sdkDumped.load(std::memory_order_relaxed) && IsRunningNullRhi()) {
        TryTriggerSdkDump();
    }
}

void UE3Bridge::UpdateHmdPose(const HmdUe3Pose& rawPose) {
    std::lock_guard lock(g_poseMutex);
    g_rawHmdPose = rawPose;
}

bool UE3Bridge::PopRenderedFrameTag(uint64_t& sampleId, int& eye) {
    const DWORD tid  = GetCurrentThreadId();
    const DWORD game = g_gameThreadId.load(std::memory_order_relaxed);
    const bool exact = (game != 0 && tid == game);
    g_presentObserved.store(true, std::memory_order_relaxed);
    if (exact) g_inTickPhase = true;   // frame presented: everything until the next camera update is game logic
    if (g_renderPairingExact.exchange(exact, std::memory_order_relaxed) != exact) {
        LOG_INFO("UE3Bridge: frame/pose pairing -> %s (Present thread %lu, game thread %lu)%s",
                 exact ? "EXACT" : "APPROXIMATE", static_cast<unsigned long>(tid), static_cast<unsigned long>(game),
                 exact ? "" : " -- AFR stereo disabled until the UE3 rendering thread is off");
    }

    std::lock_guard lock(g_tagMutex);
    if (g_tagQueue.empty()) return false;
    uint64_t packed = 0;
    if (exact) {
        // Present runs on the game thread at the end of the frame whose PlayerTick latched the NEWEST tag;
        // anything older belongs to frames that were never presented (taking the newest can't drift).
        packed = g_tagQueue.back();
        g_tagQueue.clear();
    } else {
        // Threaded rendering: the presented frame is usually the one latched before the newest.
        packed = (g_tagQueue.size() >= 2) ? g_tagQueue[g_tagQueue.size() - 2] : g_tagQueue.back();
        const uint64_t newest = g_tagQueue.back();
        g_tagQueue.clear();
        g_tagQueue.push_back(newest);
    }
    sampleId = packed >> 2;
    const uint32_t eyeCode = static_cast<uint32_t>(packed & 3u);
    eye = (eyeCode == 0) ? -1 : static_cast<int>(eyeCode - 1);
    if (!exact) eye = -1;
    return true;
}

bool UE3Bridge::GetMeasuredProjection(float& tanHalfH, float& tanHalfV) {
    if (!g_measValid.load(std::memory_order_relaxed)) return false;
    tanHalfH = g_measTanH.load(std::memory_order_relaxed);
    tanHalfV = g_measTanV.load(std::memory_order_relaxed);
    return true;
}

bool UE3Bridge::IsAimFromHandActive() {
    return g_aimFromHandActive.load(std::memory_order_relaxed);
}

bool UE3Bridge::GetHandAimReticle(float& distanceM, bool& lit) {
    const float scale = samu::config::Get().worldScale;
    distanceM = g_reticleDistUU.load(std::memory_order_relaxed) / (scale > 1.0f ? scale : 50.0f);
    lit       = g_reticleLit.load(std::memory_order_relaxed);
    return g_aimFromHandActive.load(std::memory_order_relaxed);
}

bool UE3Bridge::IsRenderPairingExact() {
    return g_renderPairingExact.load(std::memory_order_relaxed);
}

void UE3Bridge::RequestRenderResolution(int width, int height, bool forceReapply) {
    g_requestedResW.store(width, std::memory_order_relaxed);
    g_requestedResH.store(height, std::memory_order_relaxed);
    if (forceReapply) {
        g_requestedResEpoch.fetch_add(1, std::memory_order_relaxed);
    }
}

void UE3Bridge::UpdateMotionControllers(const HandUe3Pose& rightAim,
                                        const HandUe3Pose& rightGrip,
                                        const HandUe3Pose& leftAim,
                                        const HandUe3Pose& leftGrip,
                                        const MotionControllerInput& input) {
    std::lock_guard lock(g_poseMutex);
    g_rawRightAimPose  = rightAim;
    g_rawRightGripPose = rightGrip;
    g_rawLeftAimPose   = leftAim;
    g_rawLeftGripPose  = leftGrip;
    g_controllerInput  = input;
}

void UE3Bridge::RecenterHmdOrigin() {
    std::lock_guard lock(g_poseMutex);
    if (!g_rawHmdPose.positionValid && !g_rawHmdPose.orientationValid) {
        LOG_WARN("UE3Bridge: recenter ignored (no valid HMD pose yet).");
        return;
    }
    g_recenterOffsetUU   = g_rawHmdPose.offsetUU;
    g_recenterYawURot    = g_rawHmdPose.rotationURot.Yaw;
    g_recenterWorldScale = std::max(1.0f, samu::config::Get().worldScale);
    LOG_INFO("UE3Bridge: recentered HMD roomscale origin to Offset=(%.1f, %.1f, %.1f) UU, Yaw=%d URot, Scale=%.1f",
             g_recenterOffsetUU.X, g_recenterOffsetUU.Y, g_recenterOffsetUU.Z, g_recenterYawURot, g_recenterWorldScale);
}

HmdUe3Pose UE3Bridge::GetHmdPose() {
    std::lock_guard lock(g_poseMutex);
    HmdUe3Pose out = g_rawHmdPose;
    if (out.positionValid) {
        out.offsetUU = ApplyRecenterLocked(out.offsetUU);
    }
    if (out.eyesValid) {
        out.eyeOffsetUU[0] = ApplyRecenterLocked(out.eyeOffsetUU[0]);
        out.eyeOffsetUU[1] = ApplyRecenterLocked(out.eyeOffsetUU[1]);
    }
    if (out.orientationValid) {
        out.rotationURot.Yaw = (out.rotationURot.Yaw - g_recenterYawURot) & 0xFFFF;
    }
    return out;
}

HandUe3Pose UE3Bridge::GetRightAimPose() {
    std::lock_guard lock(g_poseMutex);
    HandUe3Pose out = g_rawRightAimPose;
    if (out.positionValid) {
        out.offsetUU = ApplyRecenterLocked(out.offsetUU);
    }
    if (out.orientationValid) {
        out.rotationURot.Yaw = (out.rotationURot.Yaw - g_recenterYawURot) & 0xFFFF;
        out.aimDir           = RotatorToForwardVector(out.rotationURot);
    }
    return out;
}

HandUe3Pose UE3Bridge::GetLeftAimPose() {
    std::lock_guard lock(g_poseMutex);
    HandUe3Pose out = g_rawLeftAimPose;
    if (out.positionValid) {
        out.offsetUU = ApplyRecenterLocked(out.offsetUU);
    }
    if (out.orientationValid) {
        out.rotationURot.Yaw = (out.rotationURot.Yaw - g_recenterYawURot) & 0xFFFF;
        out.aimDir           = RotatorToForwardVector(out.rotationURot);
    }
    return out;
}

MotionControllerInput UE3Bridge::GetControllerInput() {
    std::lock_guard lock(g_poseMutex);
    return g_controllerInput;
}

DecoupledCameraDiag UE3Bridge::GetLastCameraDiag() {
    std::lock_guard lock(g_poseMutex);
    return g_lastCameraDiag;
}

GrappleGloveDiag UE3Bridge::GetLastGrappleDiag() {
    std::lock_guard lock(g_poseMutex);
    return g_lastGrappleDiag;
}

bool UE3Bridge::IsHooked() { return g_hooked.load(); }
uint64_t UE3Bridge::TotalProcessEventCalls() { return g_totalEvents.load(); }
uint64_t UE3Bridge::TotalPlayerTickCalls() { return g_playerTicks.load(); }
uint64_t UE3Bridge::TotalCameraDecoupleUpdates() { return g_cameraDecoupleUpdates.load(); }
uint64_t UE3Bridge::TotalSuppressedCameraEvents() { return g_suppressedCameraEvents.load(); }
uint64_t UE3Bridge::TotalGrappleAimOverrides() { return g_grappleAimOverrides.load(); }
uint64_t UE3Bridge::TotalGloveTransformOverrides() { return g_gloveTransformOverrides.load(); }
uint64_t UE3Bridge::TotalPlayerInputInjections() { return g_playerInputInjections.load(); }
uint64_t UE3Bridge::TotalViewPointOverrides() { return g_viewPointOverrides.load(); }
uint64_t UE3Bridge::TotalUpdateCameraOverrides() { return g_updateCameraOverrides.load(); }
float UE3Bridge::AppliedFrameRateCap() { return g_appliedFpsCap.load(); }
InputInjectionDiag UE3Bridge::GetLastInputDiag() {
    std::lock_guard lock(g_poseMutex);
    return g_inputDiag;
}
const TArray<FNameEntry*>* UE3Bridge::GetGNames() { return g_gnames; }
const TArray<UObject*>* UE3Bridge::GetGObjects() { return g_gobjects; }

} // namespace samu::ue3
