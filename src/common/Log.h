#pragma once
#include <cstdarg>
#include <string>

namespace samu::log {

enum class Level { Info, Warn, Error, Debug };

// Opens samu_vr.log next to the host executable. Safe to call multiple times.
void Init();
void Shutdown();
void Write(Level lvl, const char* fmt, ...);
std::wstring ModuleDirectory(); // directory of the host .exe

} // namespace samu::log

#define LOG_INFO(...)  ::samu::log::Write(::samu::log::Level::Info,  __VA_ARGS__)
#define LOG_WARN(...)  ::samu::log::Write(::samu::log::Level::Warn,  __VA_ARGS__)
#define LOG_ERROR(...) ::samu::log::Write(::samu::log::Level::Error, __VA_ARGS__)
#define LOG_DEBUG(...) ::samu::log::Write(::samu::log::Level::Debug, __VA_ARGS__)
