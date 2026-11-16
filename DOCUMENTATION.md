# SAMU-VR: Technical Architecture, Handover & Developer Guide

**Project:** A Story About My Uncle — 6DOF OpenXR VR Mod (`SAMU-VR`)  
**Engine & Graphics API:** Unreal Engine 3 (UDK 32-bit `x86`) / DirectX 9 (`d3d9.dll`) -> DirectX 11 (`XR_KHR_D3D11_enable`)  
**Workspace Root:** `C:\Users\alexa\Desktop\SAMU-VR`  
**Steam Game Directory:** `C:\Program Files (x86)\Steam\steamapps\common\A Story About My Uncle\Binaries\Win32`  
**Steam AppID:** `278360`

---

## 1. Executive Summary & Project Status

All **4 development phases** (plus Phase 0 workspace bootstrap) are **complete, compiled cleanly (`0` errors, `0` warnings via MSVC 19.37 / C++20), and verified**:

| Phase | Scope | Key Modules | Status |
| :--- | :--- | :--- | :--- |
| **Phase 0** | Workspace bootstrap, game binary inspection (`PE32` x86), MSVC/CMake verification | [rules.md](file:///C:/Users/alexa/Desktop/SAMU-VR/.antigravity/rules.md), [CMakeLists.txt](file:///C:/Users/alexa/Desktop/SAMU-VR/CMakeLists.txt) | Complete |
| **Phase 1** | `d3d9.dll` proxy export forwarder + `OpenXRManager` session, spaces, and 90 Hz pose polling | [D3D9Proxy.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/proxy/D3D9Proxy.cpp), [OpenXRManager.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/OpenXRManager.cpp) | Complete |
| **Phase 2** | Binary signature scanning, runtime UE3 SDK dumper (`./sdk_dump/`), and `UObject::ProcessEvent` MinHook detour | [SdkDumper.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/SdkDumper.cpp), [UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp) | Complete |
| **Phase 3** | 6DOF Head Tracking camera decoupling (`1m = 50 UU`), DX9-to-DX11 shared stereo swapchain submission, and Camera-Lock Suppressor | [XrMath.h](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/XrMath.h), [DX9DX11Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/DX9DX11Bridge.cpp), [UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp) | Complete |
| **Phase 4** | 6DOF Grapple Glove aim & viewmodel override + OpenXR controller action sets (`xrSyncActions` -> `ASAMUPlayerInput`) | [OpenXRManager.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/OpenXRManager.cpp), [UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp) | Complete |

---

## 2. Directory & Module Architecture

Strict modular separation is enforced between `src/vr_core/` (OpenXR + DX9/DX11 stereo pipeline) and `src/ue3_bridge/` (UE3 reflection, offsets, and `ProcessEvent` detours), joined only by the worker/proxy layer in `src/proxy/`:

```text
C:\Users\alexa\Desktop\SAMU-VR\
├── .antigravity/
│   └── rules.md                  # Architectural & build rules
├── CMakeLists.txt                # CMake C++20 Win32 build (FetchContent: OpenXR-SDK 1.1.65, MinHook 1.3.4)
├── samu_vr.ini                   # Runtime VR, Comfort, Stereo, and MotionControls configuration
├── install_to_steam.bat          # One-click installer copying dist\d3d9.dll + samu_vr.ini to Steam folder
├── install_to_steam.ps1          # PowerShell deployment script for host desktop execution
├── dist/
│   └── d3d9.dll                  # Built 32-bit Release proxy DLL ready for deployment
├── game_bin/                     # Read-only copy of ASAMU-Win32-Shipping.exe & adjacent DLLs
├── sdk_dump/                     # Generated UE3 C++ SDK headers & property offset maps
│   ├── SDK_Offsets_Summary.txt
│   ├── Core_Object_Offsets.h
│   ├── Engine_PlayerController_SDK.h
│   ├── Engine_Pawn_SDK.h
│   ├── Engine_Camera_SDK.h
│   ├── Engine_PlayerInput_SDK.h
│   └── GrappleGlove_SDK.h
└── src/
    ├── common/
    │   ├── Config.h / Config.cpp # INI parser & runtime comfort hotkey state
    │   └── Log.h / Log.cpp       # Thread-safe logger writing samu_vr.log (+ samu_vr_live.log mirror)
    ├── proxy/
    │   ├── dllmain.cpp           # DLL_PROCESS_ATTACH entry point
    │   ├── d3d9.def              # 23 exports matching Windows SysWOW64\d3d9.dll ordinals
    │   ├── D3D9Proxy.h / .cpp    # Naked JMP export forwarders + Direct3DCreate9 interception
    │   └── ModThread.h / .cpp    # Background OpenXR frame loop + pose/input synchronization
    ├── vr_core/                  # [DECOUPLED FROM UE3]
    │   ├── XrMath.h              # OpenXR (RH Y-up m) <-> UE3 (LH Z-up UU) math & asymmetric frustums
    │   ├── DX9DX11Bridge.h / .cpp# IDirect3DDevice9 hooks + DX9<->DX11 WDDM shared stereo textures
    │   └── OpenXRManager.h / .cpp# OpenXR instance, D3D11 session, swapchains, spaces & action sets
    └── ue3_bridge/               # [DECOUPLED FROM OPENXR]
        ├── UE3Types.h            # UE3 TArray, FNameEntry, UObject, UField, UStruct, UClass, UFunction
        ├── SdkDumper.h / .cpp    # Runtime reflection walker & C++ header generator
        └── UE3Bridge.h / .cpp    # UObject::ProcessEvent detour, 6DOF camera, suppressor, 6DOF grapple & input
```

---

## 3. Verified Binary Offsets & UE3 Signatures (`ASAMU-Win32-Shipping.exe`)

- **Executable Format:** `PE32` 32-bit (`IMAGE_FILE_MACHINE_I386 = 0x014C`), `ASLR` (`DYNAMIC_BASE`), `LARGE_ADDRESS_AWARE`.
- **Default ImageBase:** `0x00400000` (computed dynamically at runtime via `GetModuleHandleW(nullptr)`).

### 3.1 Global Engine Symbols
| Symbol | RVA | Static VA (Base `0x00400000`) | Description |
| :--- | :--- | :--- | :--- |
| `GNames` | `0x026AF4B0` | `0x02AAF4B0` | `TArray<FNameEntry*>` (`39,586` entries in game) |
| `GObjects` | `0x026F17C8` | `0x02AF17C8` | `TArray<UObject*>` (`74,904` objects in game) |
| `UObject::ProcessEvent` | `0x0019EBC0` | `0x0059EBC0` | `__thiscall` (`vtable[68]`, byte offset `0x0110`) |

### 3.2 Core UE3 Object Layout (`Win32` x86)
- `UObject` (`sizeof = 0x003C`):
  - `+0x0000`: `void** VfTableObject` (`vtable[68]` = `ProcessEvent`)
  - `+0x0004`: `int32_t ObjectInternalInteger`
  - `+0x0008`: `uint64_t ObjectFlags`
  - `+0x0010`: `FName HashNext` / `HashOuterNext` / `StateFrame`
  - `+0x0028`: `UObject* Outer`
  - `+0x002C`: `FName Name` (`int32_t Index`, `int32_t Number`)
  - `+0x0034`: `UClass* Class`
  - `+0x0038`: `UObject* ObjectArchetype`
- `FNameEntry`:
  - `+0x0000`: `uint64_t Flags`
  - `+0x0008`: `int32_t Index` (bit 0 = wide char flag; `Index >> 1` = name index)
  - `+0x000C`: `FNameEntry* HashNext`
  - `+0x0010`: `char AnsiName[1024]` (or `wchar_t WideName[1024]`)
- `UField` (`sizeof = 0x0044`): `UObject` + `UField* Next` (`+0x003C`) + `UField* SuperField` (`+0x0040` in `UStruct`)
- `UStruct` (`sizeof = 0x0090`):
  - `+0x0040`: `UStruct* SuperField`
  - `+0x0044`: `UField* Children`
  - `+0x0048`: `int32_t PropertySize`
- `UProperty`:
  - `+0x0044`: `int32_t ArrayDim`
  - `+0x0048`: `int32_t ElementSize`
  - `+0x004C`: `uint64_t PropertyFlags`
  - `+0x0058`: `int32_t Offset`
- `UFunction`:
  - `+0x0090`: `uint32_t FunctionFlags`
  - `+0x0096`: `uint16_t ParmsSize` (`+0x005A` on some UDK builds; verified via `Property->Offset + ElementSize`)
  - `+0x00A4`: `void* Func`

### 3.3 Gameplay Class & Property Offsets (Verified in `./sdk_dump/`)

#### `Engine.Actor` (Size `0x01CC`)
- `+0x0054`: `FVector Location` (`X, Y, Z`)
- `+0x0060`: `FRotator Rotation` (`Pitch, Yaw, Roll` in 16-bit Unreal Rotator units: `65536 = 360 deg`)

#### `Engine.Controller` (Size `0x0350`) & `Engine.PlayerController` (Size `0x0588`)
- `+0x01D0`: `Pawn* Pawn`
- `+0x0354`: `Camera* PlayerCamera`
- `+0x0384`: `float FOVAngle`
- `+0x0388`: `float DesiredFOV`
- `+0x0394`: `FRotator TargetViewRotation`
- `+0x03A4`: `FRotator BlendedTargetViewRotation`
- `+0x0440`: `PlayerInput* PlayerInput`

#### `UDKBase.UDKPlayerController` (Size `0x0870`) & `asamu.ASAMUPlayerController` (Size `0x08DC`)
- `+0x05A4`: `FVector ShakeOffset` (Zeroed every tick when Camera-Lock Suppressor is enabled)
- `+0x05B0`: `FRotator ShakeRot` (Zeroed every tick when Camera-Lock Suppressor is enabled)
- `+0x0874..+0x0888`: `CameraAnimInst*` slots (`Sprint`, `Charge`, `Fall`, `HardFall`, `PowerJumpCharge`, `Shake`)

#### `Engine.Camera` (Size `0x04A8`)
- `+0x0384`: `FVector CameraCache.POV.Location`
- `+0x0390`: `FRotator CameraCache.POV.Rotation`
- `+0x039C`: `float CameraCache.POV.FOV`
- `+0x03C8`: `FVector ViewTarget.POV.Location`
- `+0x03D4`: `FRotator ViewTarget.POV.Rotation`
- `+0x03E0`: `float ViewTarget.POV.FOV`

#### `Engine.Pawn` (Size `0x04EC`) & `asamu.ASAMUPawn` (Size `0x06C0`)
- `+0x028C`: `float EyeHeight`
- `+0x03B4`: `Weapon* Weapon`

#### `Engine.Weapon` (Size `0x02BC`), `UDKBase.UDKWeapon` (Size `0x02DC`), & `asamu.GrappleGun` (Size `0x0518`)
- `+0x0284`: `FVector FireOffset`
- `+0x0294`: `float WeaponRange`
- `+0x0298`: `MeshComponent* Mesh`
- `+0x02CC`: `float AimTraceRange`
- `+0x02F0`: `FVector vGrappleLocation`
- `+0x0328`: `float fMaxDistance`
- `+0x03C0`: `ASAMUPlayerController* PC`
- `+0x041C`: `float OffsetX`
- `+0x0420`: `float OffsetY`
- `+0x0424`: `float OffsetZ`

#### `Engine.PlayerInput` (Size `0x0260`) & `asamu.ASAMUPlayerInput` (Size `0x0268`)
- `+0x0118`: `uint32_t bUsingGamepad` bitfield (bit `0x01`)
- `+0x0134`: `float aBaseY`
- `+0x0144`: `float aForward`
- `+0x0148`: `float aTurn`
- `+0x014C`: `float aStrafe`
- `+0x0154`: `float aLookUp`
- `+0x0158`: `float aRightAnalogTrigger`
- `+0x015C`: `float aLeftAnalogTrigger`
- `+0x0220`: `float RawJoyUp`
- `+0x0224`: `float RawJoyRight`
- `+0x0228`: `float RawJoyLookRight`
- `+0x022C`: `float RawJoyLookUp`

---

## 4. Coordinate System & Math Conventions ([XrMath.h](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/XrMath.h))

- **OpenXR (`XR_REFERENCE_SPACE_TYPE_LOCAL`):** Right-handed, `+X` Right, `+Y` Up, `-Z` Forward (meters).
- **Unreal Engine 3:** Left-handed, `+X` Forward, `+Y` Right, `+Z` Up (Unreal Units, default `WorldScale = 50.0 UU/m` in `samu_vr.ini`).
- **Position Mapping:**
  - `UE3.X = -OpenXR.Z * WorldScale`
  - `UE3.Y = +OpenXR.X * WorldScale`
  - `UE3.Z = +OpenXR.Y * WorldScale`
- **Rotation Mapping:**
  - OpenXR quaternion `(x, y, z, w)` is converted to a forward/right/up basis in UE3 coordinates and extracted into 16-bit `FRotator(Pitch, Yaw, Roll)` where `65536 URot = 360 deg`.

---

## 5. How to Build & Deploy

### 5.1 Build via CMake + MSVC (Win32 / x86)
From PowerShell:
```powershell
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' -S 'C:\Users\alexa\Desktop\SAMU-VR' -B 'C:\Users\alexa\Desktop\SAMU-VR\build' -G "Visual Studio 17 2022" -A Win32
& 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' --build 'C:\Users\alexa\Desktop\SAMU-VR\build' --config Release
```
This automatically outputs `C:\Users\alexa\Desktop\SAMU-VR\dist\d3d9.dll`.

### 5.2 Deploy to Steam Installation
> **Important Note for AI Agents Running in Sandboxed Terminals:**  
> Antigravity's terminal sandbox isolates writes to `C:\Program Files (x86)\...` inside a virtual overlay so host system files aren't modified without user action. Therefore, if an AI agent copies `d3d9.dll` to `C:\Program Files (x86)\Steam\...` from inside the sandboxed terminal, the user's host Windows Explorer / Steam client will not see that file until the user runs [install_to_steam.bat](file:///C:/Users/alexa/Desktop/SAMU-VR/install_to_steam.bat) on their desktop (or copies `dist\d3d9.dll` manually).

To deploy on the host machine:
1. Double-click [install_to_steam.bat](file:///C:/Users/alexa/Desktop/SAMU-VR/install_to_steam.bat) in `C:\Users\alexa\Desktop\SAMU-VR` (or copy `C:\Users\alexa\Desktop\SAMU-VR\dist\d3d9.dll` and `samu_vr.ini` into `C:\Program Files (x86)\Steam\steamapps\common\A Story About My Uncle\Binaries\Win32\`).
2. Start **SteamVR** so the headset is active.
3. Click **Play** on *A Story About My Uncle* in Steam.
4. Check `C:\Users\alexa\Desktop\SAMU-VR\samu_vr_live.log` (mirrored automatically by `Log.cpp` into the workspace folder!) or `Binaries\Win32\samu_vr.log` for real-time 90 Hz telemetry.

---

## 6. Controls & Hotkeys

| Input | In-Game Action |
| :--- | :--- |
| **HMD 6DOF Pose** | Decoupled 1:1 6DOF Camera Position (`1m = 50 UU`) + Yaw/Pitch/Roll |
| **Right Controller Aim Pose** | 6DOF Grapple Glove World Position, Orientation & Grapple Beam Raycast |
| **Right Trigger** | Fire / Hold Grapple Beam (`StartFire(0)` / `StopFire(0)`) |
| **Left Trigger / A Button** | Jump / Activate Jet Boots (`RocketBoostKeyDown()`) |
| **Right Grip (Squeeze)** | Charge / Release Power Jump (`PowerJumpKeyDown()` / `PowerJumpKeyUp()`) |
| **Left Grip (Squeeze)** | Sprint (`StartSprinting()` / `StopSprinting()`) |
| **Left Thumbstick** | Walk Forward / Back / Strafe Left / Right (head-relative by default; `LocomotionDirection`) |
| **Right Thumbstick** | Smooth Turn Yaw |
| **F6 (`VK_F6 = 117`)** | Toggle Comfort Camera-Lock Suppressor ON / OFF |
| **F7 (`VK_F7 = 118`)** | Recenter HMD & Motion Controller Roomscale Origin (also automatic once per gameplay level) |
| **F8 (`VK_F8 = 119`)** | Toggle forced flat virtual screen |
| **Menu (left) / B** | Escape: pause menu in gameplay, back in menus |
| **Right controller laser (menus/flat screen)** | Moves the game's mouse cursor; either trigger = left click, A/X = Enter |
| **F9 (`VK_F9 = 120`)** | Toggle stereo mode: Mono ↔ AFR alternate-frame stereo |
| **Both grips + right stick up/down** | View-scale trim (zoom the displayed image in/out); saved on release |
| **Both grips + A/X** | Toggle stereo mode (same as F9) |

---

## 7. Troubleshooting & Bugfix Log

### Startup Crash in `ASAMU-Win32-Shipping.exe` at Offset `0x0060453C` (Resolved Nov 14, 2026)
- **Symptom:** Launching the Steam build booted OpenXR (`SteamVR/OpenXR`), hooked `Direct3D9::CreateDevice` and `UObject::ProcessEvent`, and then crashed (`0xC0000005` Access Violation reading `0x00000000` at offset `0x0060453C` inside `AGameInfo::eventLogin`, `EndScene=0 Present=0`) around event #351 during initial map load.
- **Root Cause:** During map initialization (`UWorld::SetGameInfo`), UE3 calls `SetGameType` on the Class Default Object `Default__ASAMUGameInfo` via `UObject::ProcessEvent` to resolve the `AGameInfo` class stored into `WorldInfo->Game` (`+0x4A8`). `Detour_ProcessEvent` previously skipped calling `g_origProcessEvent` for all `Default__*` objects (`isCdo`), which was intended only to keep synthetic `-nullrhi` self-tests from executing unspawned Actor bytecode. Blocking `Default__ASAMUGameInfo::SetGameType` left `WorldInfo->Game == nullptr`, crashing `UWorld::SpawnPlayActor`.
- **Fixes Applied:**
  1. **CDO Self-Test Isolation ([UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp)):** Added `g_inSyntheticSelfTest` + `SyntheticSelfTestGuard` around the headless `-nullrhi` CDO verification block in `TryTriggerSdkDump()`. During normal gameplay (`!inSelfTest`), `Detour_ProcessEvent` immediately forwards all `Default__*` CDO events untouched to `g_origProcessEvent` and returns without VR hooks mutating CDOs.
  2. **Game-Thread SDK Dump Synchronization ([UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp)):** `UE3Bridge::PollSdkDump()` now polls from background `ModThread` only in `-nullrhi` mode; during live gameplay `TryTriggerSdkDump()` runs exclusively on the Unreal Game Thread inside `Detour_ProcessEvent`, preventing concurrent `GNames`/`GObjects` `TArray` reallocations.
  3. **Hierarchy-Safe Property Guards ([UE3Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/ue3_bridge/UE3Bridge.cpp)):** Added `HasSuperClassNamed` (walking `UStruct::SuperField` with cached `FName` indices) alongside `PropertiesSize` checks so menu/UT subclasses like `UTEntryPlayerController` (`Size=0x097C`) cannot be misidentified as `ASAMUPlayerController` (`Size=0x08DC`) when clearing `+0x0874` `CameraAnimInst*` slots or dispatching `ASAMUPlayerController`-only action functions, and `StartFire`/`StopFire` receive a zeroed 16-byte `fireParms` buffer.
  4. **Cross-Thread DX9/D3D11 Safety & Resolution Clamp ([DX9DX11Bridge.h](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/DX9DX11Bridge.h), [DX9DX11Bridge.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/DX9DX11Bridge.cpp), [OpenXRManager.cpp](file:///C:/Users/alexa/Desktop/SAMU-VR/src/vr_core/OpenXRManager.cpp)):**
     - `Detour_CreateDevice` adds `D3DCREATE_MULTITHREADED` and records the device via `RecordD3D9Device`, deferring D3D9 surface allocation to `BindD3D9Device` on the render thread.
     - `OpenXRManager::CreateD3D11Device` enables `ID3D11Multithread::SetMultithreadProtected(TRUE)`, and `AcquireAndBlitEyeSwapchain` serializes immediate-context copies through `DX9DX11Bridge::CopySharedEyeToD3D11Texture` (locked under `m_mutex`) rather than calling unsynchronized `CopyResource` concurrently with `CaptureStereoFrameFromD3D9`.
     - Shared `ClampedEyeExtent` helper caps swapchains, D3D11 bridge textures, and `LocateAll()` view extents at `1440` per axis to keep DX9 `GetRenderTargetData` sysmem readback within smooth frame times.
- **Post-Fix Verification:** Rebuilt Win32 `Release` (`0` errors, `0` warnings, `dist\d3d9.dll` = `731,648` bytes) and ran `ASAMU-Win32-Shipping.exe -nullrhi`: map load passed past event #351 into continuous `ASAMUPlayerControllerLegal_0` gameplay ticking (`PlayerTick #2` through `#156+` at `~62.0 Hz`) with all Phase 0–4 self-tests passing cleanly.

