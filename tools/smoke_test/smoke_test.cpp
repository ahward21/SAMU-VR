// Smoke test: loads ./d3d9.dll (the proxy) from the exe directory, calls
// Direct3DCreate9, and lets the mod thread run for a few seconds.
#include <windows.h>
#include <d3d9.h>
#include <cstdio>

int main() {
    wchar_t dir[MAX_PATH];
    GetModuleFileNameW(nullptr, dir, MAX_PATH);
    *wcsrchr(dir, L'\\') = 0;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\d3d9.dll", dir);
    HMODULE h = LoadLibraryW(path);
    if (!h) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(h, "Direct3DCreate9"));
    if (!create) { printf("Direct3DCreate9 export missing\n"); return 2; }
    IDirect3D9* d3d = create(D3D_SDK_VERSION);
    printf("Direct3DCreate9 -> %p, adapters=%u\n", (void*)d3d, d3d ? d3d->GetAdapterCount() : 0);
    auto perf = reinterpret_cast<DWORD(WINAPI*)()>(GetProcAddress(h, "D3DPERF_GetStatus"));
    printf("D3DPERF_GetStatus (naked thunk) -> %lu\n", perf ? perf() : 999);
    if (d3d) d3d->Release();
    Sleep(12000);
    return 0;
}
