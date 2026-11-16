#include "proxy/D3D9Proxy.h"
#include "proxy/ModThread.h"
#include "common/Log.h"

#include <windows.h>

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(hModule);
        samu::log::Init();
        LOG_INFO("==== SAMU-VR d3d9 proxy attached (Phase 3) ====");

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        LOG_INFO("Host process: %ls (PID %lu)", exe, GetCurrentProcessId());

        if (!samu::proxy::LoadRealD3D9()) {
            LOG_ERROR("Failed to load system d3d9.dll (err %lu). Aborting attach.", GetLastError());
            return FALSE;
        }
        LOG_INFO("System d3d9.dll loaded and all exports resolved.");

        // Do not perform heavy work under the loader lock: the worker thread
        // only begins executing after DllMain returns.
        samu::proxy::StartModThread();
        break;
    }
    case DLL_PROCESS_DETACH:
        LOG_INFO("SAMU-VR d3d9 proxy detaching (process terminating=%s).", reserved ? "yes" : "no");
        samu::proxy::SignalModThreadStop();
        samu::proxy::UnloadRealD3D9();
        samu::log::Shutdown();
        break;
    }
    return TRUE;
}
