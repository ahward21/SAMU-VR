# SAMU-VR — Free 6DOF VR Mod for *A Story About My Uncle*

An open-source, free **OpenXR 6DOF VR mod** for ***A Story About My Uncle*** (PC / Steam, Unreal Engine 3 / DirectX 9).

- **6DOF Head Tracking** with matched render-pose reprojection and suppressed flat-screen camera shakes
- **6DOF Motion-Tracked Grapple Glove** with hand-aimed grappling and a world-space aiming reticle that lights up when a surface is grappleable
- **Full VR Controller Support** (locomotion, jet boots / jump, power jump charge, sprint, smooth turn)
- **In-Headset VR Settings Menu** (`R3` / `F10`) with laser-pointer sliders and live INI saving
- **Automatic Menu Cinema Screen** with right-hand laser mouse control
- **Single-File Installer & Launcher** (`SAMU-VR-Launcher.exe`) — auto-detects Steam libraries, installs/updates cleanly, and uninstalls back to vanilla in one click

> Tested on **Meta Quest 3** and **Steam Frame** via SteamVR / OpenXR.

---

## Quick Start (Using the Launcher)

1. **Install and own the base game** (*A Story About My Uncle* on Steam).
2. **Download `SAMU-VR-Launcher.exe`** from the [Releases](https://github.com/ahward21/SAMU-VR/releases) page.
   - Note: `SAMU-VR-Launcher.exe` is a standalone single-file executable — it already embeds `d3d9.dll` and the default `samu_vr.ini`, so no manual DLL copying is needed.
3. **Start your VR / OpenXR runtime first** (e.g. **SteamVR**, **Meta Quest Link**, or **Virtual Desktop / VDXR**).
4. **Run `SAMU-VR-Launcher.exe`**:
   - The launcher scans your Windows registry and Steam `libraryfolders.vdf` to auto-detect `A Story About My Uncle\Binaries\Win32\ASAMU-Win32-Shipping.exe`.
   - If your game is in a custom location, click **Browse** and select either the game's root folder or its `Binaries\Win32` folder.
   - Check the OpenXR status line in the card to confirm your runtime is detected.
5. **Click `Install & Launch VR`** (or click **Install** first, then **Launch in VR**):
   - If the game is installed under `C:\Program Files (x86)\Steam\...`, Windows may prompt for Administrator (UAC) approval so the launcher can write into that folder.
   - Installing places `d3d9.dll`, `samu_vr.ini` (only created if missing, so your saved settings are kept on updates), and `steam_appid.txt` (`278360`) into `Binaries\Win32\`.
6. **To uninstall / return to vanilla flat-screen:** Open `SAMU-VR-Launcher.exe` and click **Uninstall** (removes `d3d9.dll`).

---

## Controls

Supports **Oculus / Meta Touch**, **Valve Index**, **Windows Mixed Reality**, and **HTC Vive** controllers via OpenXR action bindings.

### Gameplay Controls
| Input | Action |
| :--- | :--- |
| **Headset (6DOF)** | Look & lean freely (`1 m = 72 UU` default, adjustable in VR menu) |
| **Right Controller Aim** | Aim Grapple Glove & world-space reticle (green when grappleable) |
| **Right Trigger** | Fire / Hold Grapple Beam |
| **Left Trigger** or **A / X** | Jump / Activate Jet Boots |
| **Right Grip (Squeeze)** | Hold to Charge Power Jump, release to launch |
| **Left Grip (Squeeze)** or **L3 + Stick Forward** | Sprint |
| **Left Thumbstick** | Move / Strafe (head-relative by default; changeable to Body or Left Hand) |
| **Right Thumbstick** | Smooth Turn |
| **Menu (Left) / B** | Pause / Back (`Escape`) |
| **Right Stick Click (R3)** | Open / Close **In-Headset VR Settings Menu** |

### Menu & Flat-Screen Controls
During the main menu, loading screens, and pause menu, the game switches automatically to a floating virtual cinema screen:
- **Right Controller Laser** → Moves the mouse cursor
- **Right or Left Trigger** → Left click
- **A / X** → `Enter`
- **Left Thumbstick** → Arrow keys
- **Menu (Left) / B** → `Escape` (pause / back)

> **Important:** Windows only forwards simulated mouse/keyboard menu clicks when the game window is focused. If menu pointing/clicking isn't responding, click the game window on your desktop once.

### VR Settings Menu (`R3` or `F10`)
Press **Right Stick Click (`R3`)** or **`F10`** on the keyboard at any time to open the floating VR Settings overlay:
- **Control it** by pointing your right controller laser and pulling **Trigger** (drag sliders or click toggles), or navigate with **Thumbstick + A** (`B` / `Menu` closes it).
- **Available settings:**
  - `View Scale (1=Full)` — trim FOV/zoom scale (`0.70` – `1.15`)
  - `Image Sharpness` — D3D11 Contrast-Adaptive Sharpening (`0%` – `100%`, default `55%`)
  - `Text / HUD Lift` — raises subtitles & Flash tutorial popups into comfortable VR view (`0%` – `100%`)
  - `Crosshair Size` — hand reticle scale (`0.50x` – `2.00x`)
  - `World Scale (UU/m)` — Unreal Units per meter (`30` – `100`, default `72`)
  - `Eye Height Offset` — manual vertical offset (`-30 cm` to `+30 cm`)
  - `Turn Speed` — right-stick smooth turn speed (`0.00x` – `2.50x`)
  - `Move Relative To` — `Head` / `Body` / `Left Hand`
  - `Stereo Mode` — `MONO (Smooth)` / `AFR (3D)`
  - `Match Render Pose` — pose-matched compositor reprojection (`ON` / `OFF`)
  - `Cam-Lock Suppressor` — blocks scripted camera tilts/shakes (`ON` / `OFF`)
  - `Aim From Hand` — aim grapple from right controller vs head (`ON` / `OFF`)
  - `Hide Game Crosshair` — hides the center-screen HUD crosshair (`ON` / `OFF`)
  - **Recenter View**, **Save Settings to `samu_vr.ini`**, **Close**

### Keyboard & Gesture Shortcuts
| Shortcut | Action |
| :--- | :--- |
| **`F6`** | Toggle Comfort Camera-Lock Suppressor |
| **`F7`** | Recenter HMD position/yaw & virtual screen |
| **`F8`** | Toggle forced flat-screen mode |
| **`F9`** or **`Both Grips + A/X`** | Toggle `MONO (Smooth)` ↔ `AFR (3D)` stereo mode |
| **`F10`** or **`R3`** | Toggle in-headset VR Settings Menu |

---

## Known Issues & Limitations

- **Mono is default (`StereoMode=0`); AFR 3D (`F9`) is optional:**
  Unreal Engine 3 renders one camera view per frame. By default the mod runs in smooth full-rate Mono (with full 6DOF head tracking and 6DOF hand tracking) so framerate stays locked and crisp. Switching to **AFR (Alternate-Frame Rendering)** gives true stereoscopic 3D depth by alternating left and right eyes on consecutive frames, but cuts each eye's refresh rate in half and can shimmer on fast head turns.
- **Right-handed grapple only (Left arm hidden by default):**
  The game's first-person viewmodel combines both arms in one skeletal mesh. Left-side arm bones are hidden by default so the left arm doesn't float awkwardly off your right hand. An experimental tracked left-hand mesh toggle exists in `samu_vr.ini` (`EnableLeftHand=1`).
- **Desktop window must be in focus for menu input:**
  Virtual menu clicks and `Enter`/`Escape` presses are only sent while `ASAMU-Win32-Shipping.exe` is the foreground window. If another window steals focus, click the game window on your desktop once.
- **Cutscenes / scripted cameras:**
  When the game moves the camera far away from the player (`> 250 UU`) for scripted sequences or cinematics, the mod leaves that camera untouched so cinematics play normally without 6DOF tracking.
- **Antivirus / SmartScreen warnings:**
  `SAMU-VR-Launcher.exe` and the embedded `d3d9.dll` are unsigned binaries that hook DirectX 9 and engine functions (`ProcessEvent`), which can occasionally trigger heuristic warnings from Windows Defender or SmartScreen. You can inspect the entire codebase and build it yourself from source below.
- **Logs for troubleshooting:**
  If anything goes wrong, check `samu_vr.log` inside `A Story About My Uncle\Binaries\Win32\`.

---

## How It Was Made (Technical Overview)

*A Story About My Uncle* is a 32-bit (x86) **Unreal Engine 3 / UDK** game running on **DirectX 9**. Because there is no source code or native VR plugin, the mod works as a self-contained `d3d9.dll` proxy + runtime engine bridge:

1. **DX9 Proxy & DX9 → D3D11 OpenXR Bridge (`src/proxy/`, `src/vr_core/`):**
   - `d3d9.dll` forwards all standard DirectX 9 exports to Windows' real `SysWOW64\d3d9.dll` and intercepts `Direct3DCreate9`, `CreateDevice`, `BeginScene`, `EndScene`, `Present`, and `Reset` via MinHook.
   - Each captured DX9 backbuffer is bridged into D3D11 textures (with an optional 5-tap **Contrast-Adaptive Sharpening** shader pass) and submitted to OpenXR (`XR_KHR_D3D11_enable`) either as an `XrCompositionLayerQuad` (menus/loading) or an `XrCompositionLayerProjection` matched to the exact HMD pose sample ID the game frame was rendered with.
   - The game's resolution is automatically adjusted via `setres` to match the headset's per-eye aspect ratio, VSync is disabled, UE3's 62 FPS cap is raised (`TargetFrameRate=120`), and motion blur is switched off via the game's settings manager.

2. **UE3 Reflection & `ProcessEvent` Hooking (`src/ue3_bridge/`, `tools/`):**
   - The mod resolves `GNames`, `GObjects`, and `UObject::ProcessEvent` at runtime and walks UE3 reflection metadata (and extracted UnrealScript from the cooked `.upk`/`.u` packages via `tools/upk.py` + `tools/dumpsrc.py`) to locate exact property offsets and functions without patching game files on disk.
   - **6DOF Camera & Comfort:** Hooks `UpdateCamera` / `GetPlayerViewPoint` / `GetFOVAngle`, overrides `CameraCache.POV` with the 6DOF HMD pose, suppresses scripted `CameraAnimInst` / shake pitch & roll, and keeps `PlayerController.Rotation` body-relative for movement.
   - **6DOF Grapple Glove & Aiming:** Drives `GrappleGun.Mesh` (`UDKSkeletalMeshComponent`) in world space at the right controller's palm socket, clears the flat-screen "weapon FOV" squeeze (`SetFOV(0)`), disables the free-fall arm-flailing animation blend (`AnimIsFallingNode`) so jumps don't shake the hand, hides left-arm bones, hides the center-screen Scaleform crosshair (`CrosshairMC`), and traces along the right controller ray (`Actor.Trace`) so grapple shots go where your hand points.
   - **UI & Subtitles:** Scales down `ASAMUSubtitleFont` and shrinks/lifts Scaleform tutorial popups (`PopupsMC`) into comfortable VR view, while rendering the custom `VrSettingsMenu` onto an interactive OpenXR quad layer.

3. **Single-File Launcher (`tools/launcher/`):**
   - Pure Win32 C++ GUI app that embeds `d3d9.dll` and `samu_vr.ini` directly as `RCDATA` resources, parses Steam's `libraryfolders.vdf` to find the game automatically, handles UAC elevation only when needed, and verifies installed DLL bytes for one-click updates and clean uninstalls.

---

## Building from Source

### Requirements
- Windows 10 / 11
- **Visual Studio 2022** (or **VS 2022 Build Tools**) with C++ desktop workload
- **CMake 3.24+** (bundled with VS 2022)
- Internet access on first configure (CMake `FetchContent` pulls `OpenXR-SDK v1.1.65` and `MinHook v1.3.4` automatically)

### Build Commands (Win32 / x86)
You must build for **Win32 (`-A Win32`)** to match `ASAMU-Win32-Shipping.exe`:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A Win32
cmake --build build --config Release --parallel
```
Or run `build.bat` if using VS 2022 Build Tools.

Outputs:
- `dist/d3d9.dll` — the VR proxy DLL
- `dist/SAMU-VR-Launcher.exe` — standalone installer/launcher with `d3d9.dll` and `samu_vr.ini` embedded

---

## Credits & License
- Built with [Khronos OpenXR-SDK](https://github.com/KhronosGroup/OpenXR-SDK) and [MinHook](https://github.com/TsudaKageyu/minhook).
- *A Story About My Uncle* is developed by Gone North Games and published by Coffee Stain Studios. This is an unofficial, free fan-made mod and is not affiliated with the developers or publishers.
