#pragma once
#include <windows.h>

namespace samu::proxy {

// Loads the real system d3d9.dll (SysWOW64 under WOW64) and resolves all exports.
// Called from DllMain; uses only kernel32 APIs.
bool LoadRealD3D9();
void UnloadRealD3D9();

// Hooks Direct3DCreate9 on the loaded system d3d9.dll if not yet called via proxy export
// (e.g. if loaded as XInput1_3.dll / dinput8.dll or alongside GameOverlayRenderer.dll).
void EnsureSystemDirect3DCreate9Hooked();

// Number of times the game called Direct3DCreate9 / Ex (diagnostics).
long Direct3DCreateCount();

} // namespace samu::proxy
