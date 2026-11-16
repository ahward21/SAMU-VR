# SAMU-VR — OpenXR 6DOF VR Mod for *A Story About My Uncle*

> **Handoff Note for Future AI / Claude Code Sessions:**  
> This file summarizes the project architecture, what is currently implemented, the latest fixes compiled into `dist/d3d9.dll`, and the exact next steps to test and continue development. For deep technical details (UE3 RVAs, property offsets, and prior bugfix history), see [DOCUMENTATION.md](DOCUMENTATION.md) and `./sdk_dump/`.

---

## 1. Project Overview
- **Target Game:** *A Story About My Uncle* (`ASAMU-Win32-Shipping.exe`, 32-bit x86 Unreal Engine 3 / UDK, DirectX 9).
- **Steam Path:** `C:\Program Files (x86)\Steam\steamapps\common\A Story About My Uncle\Binaries\Win32\`
- **Injection Mechanism:** Proxy `d3d9.dll` placed alongside `ASAMU-Win32-Shipping.exe` (installed via `install_to_steam.bat`).
- **VR Runtime:** OpenXR (`XR_KHR_D3D11_enable`) via SteamVR, bridged from the game's DX9 backbuffer (`DX9DX11Bridge`) into OpenXR D3D11 swapchains (`OpenXRManager`).
- **Live Telemetry Log:** Every launch automatically mirrors logs to `samu_vr_live.log` in this repo root (and `samu_vr.log` in `Binaries\Win32\`).

---

## 2. Repository Structure
| Path | Purpose |
| :--- | :--- |
| `src/proxy/` | `d3d9.dll` proxy exports (`D3D9Proxy.cpp`, `d3d9.def`, `dllmain.cpp`) and background OpenXR/input loop (`ModThread.cpp`). |
| `src/vr_core/` | Strictly decoupled from `ue3_bridge`. `OpenXRManager.cpp/.h` (OpenXR session, swapchains, `XrCompositionLayerQuad` flat screen & `XrCompositionLayerProjection`, action sets), `DX9DX11Bridge.cpp/.h` (D3D9 `CreateDevice`/`EndScene`/`Present`/`Reset` hooks + DX9→D3D11 texture bridge), `XrMath.h`. |
| `src/ue3_bridge/` | MinHook detour on `UObject::ProcessEvent` (`UE3Bridge.cpp/.h`), UE3 structs (`UE3Types.h`), and runtime SDK dumper (`SdkDumper.cpp/.h`). |
| `src/common/` | `Config.cpp/.h` (`samu_vr.ini` loader/writer) and `Log.cpp/.h`. |
| `sdk_dump/` | Complete dumped UE3 headers from the live game (e.g. `GrappleGlove_SDK.h`, `ASAMU_SDK.h`). |
| `dist/d3d9.dll` | Latest compiled Win32 `Release` proxy DLL (`760,832` bytes). |
| `install_to_steam.bat` / `.ps1` | Copies `dist/d3d9.dll` into the Steam `Binaries\Win32\` folder. |
| `DOCUMENTATION.md` | Detailed RVA/offset reference, coordinate transforms, controls, and bugfix log. |

---

## 3. Current State & Latest Changes

### Verified Working in Latest Live SteamVR Test (`13:59` run, `samu_vr_live.log`)
- **Startup, Menu, and `AG-StarHaven` Level Load All Work Without Crashing:**
  - Intro & `ASAMUFrontEndMap` stayed on the flat virtual screen (`2.50 m` ahead, `3.20 m` wide).
  - Clicking **Continue** transitioned cleanly: `UE3Bridge: gameplay state -> IMMERSIVE GAMEPLAY (map='AG-StarHaven')` and `OpenXR: display mode -> IMMERSIVE (projection, game-matched FOV)`, running continuously at `~62.0 Hz` (`PlayerTick #3357`, `StereoCap=3799`, `StereoSubmit=6386`, `GloveXform=1708`, `InputInj=1742`).

### Session 2 (Nov 14, Claude Code) — Fixes for all 4 issues: BUILT, NOT YET TESTED IN THE HEADSET
`dist/d3d9.dll` = 760,832 bytes, 0 errors / 0 warnings. Outside the game, the smoke test (`build/smoke/smoke_test.exe`) loads it cleanly. Nothing below has run in-game yet.

1. **Controllers didn't move the player → fixed at the root cause.** `PlayerTick` calls `PlayerInput.PlayerInput()` and `PlayerMove()` as *script-to-script* calls, which never pass through `ProcessEvent`. So the old `PlayerInput` hook never fired, and the post-`PlayerTick` write came after movement had already been computed. Raw `[-1..1]` stick values are now added to `aBaseY`/`aStrafe`/`aTurn` in a **`PlayerTick` pre-hook** (`BeginGameplayFrame` → `InjectLocomotionIntoPlayerInput`), and the script scales them as it does for keyboard input. Movement is head-relative by default (`LocomotionDirection`). The 1 s log line shows `inj(...) -> script aForward=…`: non-zero `aForward` while pushing the stick means the game consumed the input.
2. **Camera hooks could apply head yaw/offset twice.** The `GetPlayerViewPoint`/`UpdateViewTarget` post-hooks *added* the HMD pose on top of a `CameraCache` that already contained it. All camera hooks now go through one **absolute** `ComputeVrView()` (pawn eye + body-yaw-rotated head offset, rotation = body yaw + HMD yaw), so applying it twice is harmless. If the game camera is >250 UU from the pawn's eyes, the view is treated as a cinematic and left alone.
3. **Judder.** (a) Each gameplay frame latches one HMD pose sample (`sampleId`). The Present hook pops the tag, and `OpenXRManager` submits the image with *that* pose from a 256-entry pose history (`MatchRenderPose`), so SteamVR reprojects correctly instead of the image swimming. (b) `GameEngine.MaxSmoothedFrameRate` 62 → `TargetFrameRate` (90) via runtime reflection. (c) VSync forced off in `CreateDevice`/`Reset` (`DisableVSync`).
4. **No depth → AFR stereo (`StereoMode=1`, F9 toggles).** Even frames render the left eye, odd frames the right, with the real per-eye OpenXR view positions. Each eye goes to its own capture slot and swapchain and is submitted with its own render pose. UE3 renders on a separate **render thread** (Present on T42604 vs PlayerTick on T36912 in the last log), which makes frame↔pose pairing ambiguous by one frame. The mod therefore runs `TOGGLERENDERINGTHREAD` via `PlayerController.ConsoleCommand` once. **AFR only turns on after Present is observed on the game thread** (`pairing=EXACT`). If the command is unavailable in this shipping build, it stays in mono rather than risk swapped eyes.
5. **"Close 2D screen" / FOV.** The game was rendering **1280×720** (16:9) with a 94° FOV, so it filled only a band of the headset view. The mod now runs `setres` to a square-ish resolution at the HMD eye aspect (`RenderHeight=-1` auto, capped at 90% of desktop height). The game FOV is now the smallest symmetric FOV that covers both eyes' full frustums.
6. **Eye height.** The mod auto-recenters position, yaw and height once per gameplay level (`AutoRecenterOnLevel`), and `EyeHeightOffsetCm` adds a manual height offset. The gameplay→flat-screen timeout went from 500 to 1000 ms. The mid-game controller dropout at 13:59:11 was the runtime reporting profile `(none)` (SteamVR dashboard), not a binding bug.

### Session 2b (17:13 live test feedback) — built and installed, not yet re-tested
Results from the 17:13 test: TOGGLERENDERINGTHREAD worked (`pairing -> EXACT`), AFR ran, `setres 1296x1296` applied, the 90 fps cap applied, and **stick locomotion worked** (`script aForward=+1619`). Problems found:
- **Head turning moved the world the wrong way.** `Camera.UpdateCamera` is an *event* (`0x902`) that the native camera manager calls every frame **after** `PlayerTick`. It rebuilt `CameraCache.POV` from the body rotation and dropped head yaw, so the image turned with the head. Fix: an absolute `ComputeVrView` post-hook on `UpdateCamera`. `UpdateCamera=` in the log should now climb at the frame rate. (`GetPlayerViewPoint` only fires through ProcessEvent a few times per second, so it is not the render path.)
- **Glove appeared once, then vanished.** Writing `Actor::Location` directly doesn't move the mesh components, and feeding hand offsets into `GrappleGun::OffsetX/Y/Z`/`FireOffset` broke the game's own `SetPosition`. Now uses native `Actor.SetLocation`/`SetRotation` through ProcessEvent, and leaves the offsets alone.
- **Still shaky.** The game ran at only ~53–60 fps (each eye ~28 Hz in AFR) because the synchronous `GetRenderTargetData` at Present serialized the CPU and GPU. The readback is now deferred to the next `BeginScene` (vtable 41), when the GPU has normally finished. That adds one frame of latency, which pose matching compensates for. If it's still below ~80 fps, try `StereoMode=0` (F9).
- **Menus had no controller support.** In flat-screen mode, the right-hand laser now drives the mouse cursor on the virtual screen, either trigger left-clicks, and A/X sends Enter. Menu/B sends Escape in any mode (pause/back). New OpenXR action `menu_button`. Input only goes to the game while its window is in the foreground.

### Session 2c (17:43 live test feedback) — built and installed, not yet re-tested
Findings from the 17:43 log: the deferred readback lifted the game to **80–90 fps**. But `UpdateCamera=0`, `ViewPoint=1` and `CamDecouple` rose exactly once per tick: **no camera event reaches the renderer through ProcessEvent each frame**, and `GloveAimOverrides=0` (the weapon aim hooks are script-to-script too). The only thing that demonstrably reaches the screen is `PlayerController.Rotation` (pitch written there worked; yaw was never written).
- **Head yaw now goes through `PlayerController.Rotation`.** Post-`PlayerTick`: `Rotation.Yaw = bodyYaw + hmdYaw`. Pre-`PlayerTick`: the pure body yaw is restored so stick turning, movement direction and pawn facing stay body-relative (`g_bodyYaw`, `g_lastWrittenYaw`; external rotations such as teleports are folded in).
- **Grapple aims from the hand at fire time.** For the `StartFire` call only, `Rotation` is pointed along the right-hand aim ray (parallax-corrected from the eye), then restored. Log line: `StartFire(0) ... aim=HAND`. The game's own reticle is still drawn at the view centre.
- **AFR safety gate.** AFR needs the per-eye camera *position* to reach the renderer, and no position hook is proven yet. AFR is held off (mono) until a view hook has applied the position for 10 consecutive frames (`per-eye camera position ...` log line).
- **Diagnostics.** `[CamEvent] first call: ...` and periodic `[CamEvent] totals` list every View/Camera/Aim/Fire/Trace event the game really dispatches through ProcessEvent. Use them to pick the right hook for head *position* and per-eye stereo.
- **Menus.** Enter/Escape did arrive (Escape toggled pause) but the pointer didn't take. Now: keys carry hardware scan codes, the laser sends relative mouse motion (DirectInput) plus absolute cursor position, the left stick sends arrow keys, and `[MenuPointer]` lines log hit/cursor/clip state every 2 s.

### Session 2d (18:01 live test feedback) — built and installed, not yet re-tested
The `[CamEvent]` diagnostics settled the camera question: per frame the game dispatches `Camera.UpdateCamera` x1, `ASAMUPlayerController.GetPlayerViewPoint` x4 and `GrappleGun.SetPosition` x1 through ProcessEvent. The hooks had been bailing out because of a **wrong offset inherited from the original code: `kPawn_EyeHeight = 0x28C` is actually `GroundSpeed` (~400)**; the real `EyeHeight` is `0x2C8`. The computed "pawn eye" was ~402 UU too high, so every frame looked like a cinematic (`game view is 402 UU from the pawn eye`), and the glove was being moved 8 m above the player.
- The VR view is now **the game's own camera location + head/eye offset** (`AdjustGameView`), with absolute rotation. It is applied in `UpdateCamera` (CameraCache) and `GetPlayerViewPoint`, and recognises an already-adjusted location so it never doubles. The eye offset from the pawn is learned from the game's camera each frame (`g_eyeFromPawn`) and used as the base for hands and fire-time aim.
- **FOV** is forced in `UpdateCamera` (CameraCache FOV) and via a `GetFOVAngle` hook. Before this the game rendered its own FOV while the layer was submitted at 110 deg, which caused the "zoomed in" look and made the world swim on head turns. Check `UpdateCamera: game FOV X -> VR FOV Y` in the log.
- Menu pointer: the log shows hits, cursor tracking and clicks, and gameplay started after the clicks, so menus appear to work now.

### Session 2e (18:41 live test feedback) — built and installed, not yet re-tested
The 18:41 log shows every hook firing each frame (`UpdateCamera` x1, `GetPlayerViewPoint` x4, `GetFOVAngle` x1, `SetPosition` x1), AFR on with exact pairing, the eye offset learned as (0, 0, 38) UU, and `game FOV 90 -> VR FOV 110`. The user still reported zoom, head-turn swimming and a static hand.
- **Zoom / swimming: unresolved, now measurable.** All values the mod controls are consistent (110 deg on a 1:1 backbuffer), so the rendered FOV must differ from what is submitted for a reason not yet identified. Added `[Display] ViewScale` (scales the submitted FOV in tan space) with an in-headset gesture: **hold both grips + right stick up/down**; saved to the ini on release, logged as `[Calibration] ViewScale = x`. The value the user settles on reveals the real rendered FOV (`atan(tan(55 deg) * ViewScale)`). `bConstrainAspectRatio` on the camera is now cleared and logged.
- **Stereo toggle in the headset:** both grips + A/X (same as F9), to compare AFR against mono for the motion-sickness report.
- **Hand:** the visible hand is `GrappleGun.Mesh` (a SkeletalMeshComponent re-attached to the view each frame), so moving the weapon actor did nothing. `Apply6DofGloveTransformToWeapon` now drives the mesh component: `SetAbsolute(1,1,0)` + `SetTranslation`/`SetRotation` via ProcessEvent, placed so the `BeamSocketName` socket (palm) sits on the right controller. The socket offset is learned from the game's own placement for 20 frames, then refined closed-loop (`hand rig:` log line shows controller vs socket error). Mesh orientation relative to the controller is a first guess.

### Session 2f (18:54 live test feedback) — built and installed, not yet re-tested
The hand rig works (`hand rig: ... err=0.0 UU`; palm socket `GrappleSocket` at (32.3, 13.6, -9.1) UU in view space). The user swept `ViewScale` 0.75..1.19 and no value removed the warping on head turns, so the mismatch is not a uniform FOV scale (suspect: horizontal and vertical FOV differ on the 1:1 backbuffer). User also confirmed AFR gives depth (hand looks smaller/closer than in mono).
- **Projection is now measured, not assumed.** `MeasureViewProjection` calls `LocalPlayer.Project` (the engine's own CalcSceneView + WorldToScreen) on three points placed relative to the head view. It logs `MEASURED game projection: horizontal FOV X, vertical FOV Y` and feeds `tanH`/`tanV` to `OpenXRManager::SetMeasuredProjection`, which replaces the FOV/aspect estimate for the submitted layer. `ViewScale` still multiplies on top.
- **Aim decoupled from the head (`AimFromHand=1`).** The `UpdateCamera` hook computes the head view and stores it (`g_headViewLoc/Rot`). Every `GetPlayerViewPoint`/`GetCameraViewPoint` arriving through ProcessEvent (native callers: renderer, audio) returns that head view. `CameraCache.POV`, which script code reads for the weapon trace start/aim, is set to the right-hand aim pose. The probe validates this: if the head-view centre stops projecting to screen centre after activation, the renderer is reading the cache directly and aim-from-hand is disabled for the session (`the renderer followed the hand-aim camera`). Requires the hand rig to be learned and 5 good probe results first.
- Known gap: the game's HUD crosshair is still drawn at the centre of the view. Next step is a hand-anchored reticle (or hiding the HUD one).

### Session 2g (19:10 live test feedback) — built and installed, not yet re-tested
The projection probe settled two questions: `MEASURED game projection: horizontal FOV 110.0 deg, vertical FOV 110.0 deg` and `rendered view == head view`. So projection/FOV is **not** the cause of the warping, and `ViewScale` is not the fix. Aim-from-hand (hand pose in CameraCache) went ACTIVE without the render following it, but the grapple still followed the head: the game's aim does not read CameraCache from script.
- **Roll conversion bug fixed (`XrToUe3Rotator`).** Verified offline against UE3's FRotationMatrix: the old roll was `atan(tan(roll)*cos(pitch))`, wrong whenever pitch != 0 (basis error up to 0.19 for pitch 55 deg / roll 28 deg). Now `atan2(-right.z, up.z)`; forward was always correct. This skews the rendered world relative to the submitted pose whenever the head is pitched and rolled, and also affected hand rotation. It is a confirmed error but **not proven to be the whole warp**.
- **Aim is now split by frame phase.** Aim reaches the engine through the same `GetPlayerViewPoint` event the renderer uses (native `Pawn.GetBaseAimRotation`). `g_inTickPhase` is true from Present until the `UpdateCamera` post-hook; GPV events in that phase return the eye location with a rotation along the right hand's aim ray (parallax-corrected to converge at 1500 UU). After `UpdateCamera` they return the head view. The projection probe forces the head view while it runs.
- **VR hand reticle.** `OpenXRManager` submits a second layer: a 128x128 ring quad 30 m along the right aim ray (same convergence distance), visible while aim-from-hand is active. The game's own HUD crosshair is still drawn at view centre; hiding it needs the HUD class (not in the SDK dump yet).

### Session 2h — left arm (built and installed with 2g, not yet tested)
User report: the left hand "floats in space to the right". Working assumption (unverified; cooked packages are compressed so the asset can't be inspected offline): `GrappleGun.Mesh` is one skeletal mesh with both arms, and since it is now placed by the right palm socket the left arm hangs beside the right hand. `HideLeftArmBones` runs once when the hand rig is learned: it enumerates bones via `SkeletalMeshComponent.GetBoneName`, logs the whole skeleton (`glove skeleton: ...`), and hides bones whose names look left-sided via `HideBoneByName`. If the log says `0 bone(s) hidden`, use the logged skeleton to fix `IsLeftSideBoneName`, or the left hand is a different component. A tracked left hand needs a second mesh instance; the bone list is the prerequisite.

### Session 2i (21:55 live test feedback) — built and installed, not yet re-tested
User report: left hand gone (2h worked: 18 `*_L` bones hidden), still "zoomed in", warping/fuzzy turning "if anything worse", yellow ring visible but the grapple still goes to the big head-locked crosshair.

**The game's UnrealScript source is embedded in the cooked packages** (`ScriptText` TextBuffer exports, LZO-compressed). `tools/upk.py` + `tools/dumpsrc.py` extract it; the dump is in `sdk_dump/script_src/*.uc` (2063 files: asamu, UT*, Engine, UDKBase, GFxUI). Regenerate with `python -I tools/dumpsrc.py "<game>\ASAMU\CookedPC\Startup.upk" sdk_dump/script_src` (same for `Engine.u`, `UDKBase.u`, `GFxUI.u`). Read it before guessing how the game does something. Facts taken from it:
- **Aim chain (all script-to-script, never through ProcessEvent):** `GrappleGun.Tick -> CheckForRange` and `UTWeapon.InstantFire` trace from `Instigator.GetWeaponStartTraceLocation()` (= `Location + EyeHeight + WalkBob`, the pawn eye) along `GetAdjustedAim` -> `PlayerController.GetAdjustedAimFor` -> `Pawn.GetBaseAimRotation` -> `Controller.GetPlayerViewPoint` -> `Camera.GetCameraViewPoint` = **`CameraCache.POV.Rotation`**. Nothing else in script writes CameraCache between camera updates. 2g left the head view in CameraCache, which is exactly why the grapple followed the head.
- **Crosshair** = Scaleform clip `ASAMUHUDMovie.CrosshairMC` (PC.myHUD -> `ASAMUHUD.HudMovie`). `ASAMUHUD.CurrentCrosshair` (enum HIDDEN, DOT, DISABLED, ENABLED, STORY_DISABLED, STORY_ENABLED) is set by `CheckForRange`: ENABLED = the aimed spot can be grappled.
- **Hand mesh** is a `UDKSkeletalMeshComponent` with its own `FOV` (the flat-screen "weapon FOV" trick; the beam particle component copies it). The engine squeezes the mesh toward the view axis each frame, so in VR the hand is enlarged and slides against the world on every head turn. Probable cause of the "zoomed in" / "warps when I turn" reports that the projection probe could not explain.
- **Motion blur** is on by default (`BaseSystemSettings.ini MotionBlur=True`); the options menu toggles it with `ASAMUSystemSettingsManager.SetBoolSetting("MotionBlur", b)`.

Changes:
- `UpdateScriptAim` (UE3Bridge.cpp): traces the right hand's ray with `Actor.Trace` called on the GrappleGun (same flags as the game), then writes into `CameraCache.POV.Rotation` the rotation from the **pawn eye** to that hit point (far point if nothing is hit). Runs after every `UpdateCamera`, at the start of every PlayerTick, and right before `StartFire`. Native `GetPlayerViewPoint` events always get the head view now (the 2g phase split is gone). `[MotionControls] ... StartFire` logs `target=` vs `game grapple point=` and `miss=NN UU`: that number is the proof.
- Hand reticle now sits at the traced hit distance (`GetHandAimReticle`), constant angular size, and switches to a green ring while `CurrentCrosshair` is ENABLED/STORY_ENABLED. `UpdateHudForVr` hides `CrosshairMC` via `GFxObject.SetVisible(false)` while hand aim is active (`HideGameCrosshair=1`).
- `FixHandMeshFov`: `SetFOV(0)` on the hand mesh and the gun's UDK particle components (`TrueScaleHand=1`); logs the FOV it found.
- `DisableGameMotionBlur`: per level, through the game's settings manager (`DisableMotionBlur=1`); logs before/after.

If the grapple still misses: check `hand aim:` lines (is it ACTIVE, does the trace HIT), then the `miss=` value. If `miss` is small but the user sees a mismatch, the problem is visual (reticle/hand placement), not the aim.
Still open: game runs 62-80 fps with the render thread off, so AFR gives ~35 Hz per eye. Next step if shaking remains with motion blur off: restore the render thread and pair frames by a marker drawn into the frame, or default to mono. Tracked left hand: bones are `LowerArm_L`, `Hand_L`, `*Finger_L_0n` (38 bones total, `Root`, `Belt`); needs a second mesh instance.

### What to check in the next live test (`samu_vr_live.log`)
- `hand mesh was drawn with its own FOV of NN deg`, `motion blur: was 1 ... now 0`, `HUD rig ready`, `game crosshair (screen centre) -> hidden`, `aim rig: Actor.Trace=<non-null>`, `hand aim: ... HIT at NNN UU`, and `miss=NN UU` on each trigger pull.
- `GameEngine frame cap: ... MaxSmoothedFrameRate 62.0 -> 90.0`, then `PlayerTick` at ~90 Hz.
- `console command 'TOGGLERENDERINGTHREAD'`, then `frame/pose pairing -> EXACT`, then non-zero `AFRframes` in the `Locomotion …` line. If pairing stays `APPROXIMATE`, the command is compiled out. Next option: find `GFrameNumberRenderThread` to pair frames on the render thread.
- `console command 'setres WxH'` and a matching `IDirect3DDevice9::Reset ... WxH`. If the window misbehaves, set `RenderHeight=0`.
- Frame rate now that the `GetRenderTargetData` readback runs on the game thread. If it drops below 90, the next fix is a double-buffered async readback or D3D9Ex zero-copy.
- AFR eye check: close one eye at a time; nearby objects should shift the right way. If they look inverted, flip the eye parity in `BeginGameplayFrame`.
- Cinematics: any scripted camera should play un-tracked (the 250 UU threshold in `ComputeVrView`).

---

## 4. Build & Deploy Commands

### Build (Win32 / x86 Release)
```powershell
& "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" --build "C:\Users\alexa\Desktop\SAMU-VR\build" --config Release --parallel
```
Outputs `C:\Users\alexa\Desktop\SAMU-VR\dist\d3d9.dll`.

### Deploy to Steam
Run `install_to_steam.bat` (or copy `dist\d3d9.dll` to `C:\Program Files (x86)\Steam\steamapps\common\A Story About My Uncle\Binaries\Win32\d3d9.dll`), start SteamVR, and launch the game from Steam.

---

## 5. Recommended Next Steps for the Next AI
1. Run the live test above and read `samu_vr_live.log` against the checklist in section 3.
2. If AFR works but each eye's 45 Hz update shimmers, consider true dual-view rendering (UE3 splitscreen with a second view, or a native `ULocalPlayer::CalcSceneView` hook).
3. Remove the sysmem readback stall (async double-buffered `GetRenderTargetData`, or D3D9Ex shared handles).
4. Verify `StartFire(0)` / `CalcWeaponFire` 6DOF grapple aiming now that the camera hooks are absolute.
