#include "common/Log.h"

#include <windows.h>
#include <cstdio>
#include <mutex>

namespace samu::log {
namespace {
std::mutex g_mutex;
FILE* g_file = nullptr;
FILE* g_mirrorFile = nullptr;

const char* LevelName(Level l) {
    switch (l) {
    case Level::Info:  return "INFO ";
    case Level::Warn:  return "WARN ";
    case Level::Error: return "ERROR";
    case Level::Debug: return "DEBUG";
    }
    return "?????";
}
} // namespace

std::wstring ModuleDirectory() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    auto pos = s.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"." : s.substr(0, pos);
}

void Init() {
    std::lock_guard lock(g_mutex);
    if (g_file) return;
    std::wstring dir = ModuleDirectory();
    std::wstring p   = dir + L"\\samu_vr.log";
    g_file = _wfsopen(p.c_str(), L"a", _SH_DENYNO);

    // Also mirror live logs into the SAMU-VR workspace directory when running from the Steam folder
    const std::wstring repoDir = L"C:\\Users\\alexa\\Desktop\\SAMU-VR";
    DWORD attr = GetFileAttributesW(repoDir.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) &&
        _wcsicmp(dir.c_str(), repoDir.c_str()) != 0) {
        std::wstring mirrorPath = repoDir + L"\\samu_vr_live.log";
        g_mirrorFile = _wfsopen(mirrorPath.c_str(), L"a", _SH_DENYNO);
    }
}

void Shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_file) { fclose(g_file); g_file = nullptr; }
    if (g_mirrorFile) { fclose(g_mirrorFile); g_mirrorFile = nullptr; }
}

void Write(Level lvl, const char* fmt, ...) {
    char msg[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u][%s][T%05lu] %s\n",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, LevelName(lvl),
             GetCurrentThreadId(), msg);

    std::lock_guard lock(g_mutex);
    if (g_file) {
        fputs(line, g_file);
        fflush(g_file);
    }
    if (g_mirrorFile) {
        fputs(line, g_mirrorFile);
        fflush(g_mirrorFile);
    }
    OutputDebugStringA(line);
}

} // namespace samu::log
