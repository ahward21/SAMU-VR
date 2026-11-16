#pragma once

namespace samu::proxy {

// Starts the mod worker thread (called from DLL_PROCESS_ATTACH).
void StartModThread();
// Signals the worker to stop (called from DLL_PROCESS_DETACH; never blocks).
void SignalModThreadStop();

} // namespace samu::proxy
