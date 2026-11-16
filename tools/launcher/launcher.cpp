// tools/launcher/launcher.cpp
// Minimal single-file Installer & Launcher for SAMU-VR (A Story About My Uncle VR Mod).

#include "resource.h"

#include <windows.h>
#include <bcrypt.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <winhttp.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kAppTitle[]         = L"SAMU-VR";
constexpr wchar_t kLauncherVersion[]  = L"1.0.1";
constexpr wchar_t kWindowClass[]      = L"SamuVrLauncherWnd";
constexpr wchar_t kGameExeName[]      = L"ASAMU-Win32-Shipping.exe";
constexpr wchar_t kProxyDllName[]     = L"d3d9.dll";
constexpr wchar_t kConfigIniName[]    = L"samu_vr.ini";
constexpr wchar_t kSteamAppId[]       = L"278360";
constexpr wchar_t kGithubApiHost[]    = L"api.github.com";
constexpr wchar_t kGithubLatestPath[] = L"/repos/ahward21/SAMU-VR/releases/latest";
constexpr UINT    WM_APP_REMOTE_CHECK = WM_APP + 1;

// Minimal matte dark palette (single calm monochrome hierarchy)
constexpr COLORREF kColBg         = RGB(20, 21, 25);
constexpr COLORREF kColSurface    = RGB(29, 31, 37);
constexpr COLORREF kColBorder     = RGB(48, 51, 60);
constexpr COLORREF kColTextHi     = RGB(244, 244, 246);
constexpr COLORREF kColTextMuted  = RGB(154, 160, 172);
constexpr COLORREF kColTextSubtle = RGB(114, 120, 132);
constexpr COLORREF kColDotOk      = RGB(82, 196, 134);
constexpr COLORREF kColDotWarn    = RGB(224, 174, 84);
constexpr COLORREF kColDotErr     = RGB(220, 96, 96);

enum ControlId : int {
    IDC_EDIT_PATH = 1001,
    IDC_BTN_BROWSE,
    IDC_BTN_LAUNCH,
    IDC_BTN_INSTALL,
    IDC_BTN_UNINSTALL,
    IDC_LBL_STATUS,
    IDC_LBL_OPENXR,
    IDC_LBL_FEEDBACK,
};

struct RemoteReleaseInfo {
    bool         valid = false;
    std::wstring tag;
    std::wstring dllUrl;
    std::wstring sha256;
};

struct AppState {
    HINSTANCE hInst = nullptr;
    HWND      hwnd  = nullptr;
    UINT      dpi   = 96;

    HBRUSH brBg      = nullptr;
    HBRUSH brSurface = nullptr;

    HFONT fontTitle  = nullptr;
    HFONT fontBold   = nullptr;
    HFONT fontBody   = nullptr;
    HFONT fontSmall  = nullptr;

    std::wstring win32Dir;
    std::wstring openXrText;
    bool         openXrReady = false;

    RemoteReleaseInfo remote;
    bool              remoteUpdateAvailable = false;

    enum class ModStatus { NoGameFound, NotInstalled, UpdateAvailable, UpToDate };
    ModStatus modStatus = ModStatus::NoGameFound;
    int       hoverId   = 0;
} g_app;

int S(int px) { return MulDiv(px, static_cast<int>(g_app.dpi), 96); }

void EnableDarkTitleBarAndDpi(HWND hwnd = nullptr) {
    if (!hwnd) {
        if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
            using PFN_SetDpiCtx = BOOL(WINAPI*)(DPI_AWARENESS_CONTEXT);
            if (auto fn = reinterpret_cast<PFN_SetDpiCtx>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"))) {
                fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
            }
        }
        return;
    }
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using PFN_DwmSet = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto fn = reinterpret_cast<PFN_DwmSet>(GetProcAddress(dwm, "DwmSetWindowAttribute"))) {
            BOOL dark = TRUE;
            fn(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
        }
        FreeLibrary(dwm);
    }
}

bool FileExists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring TrimSlashes(std::wstring s) {
    while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    return s;
}

std::wstring NormalizeWin32Dir(const std::wstring& rawInput) {
    std::wstring p = TrimSlashes(rawInput);
    if (p.empty()) return {};
    if (FileExists(p)) {
        size_t slash = p.find_last_of(L"\\/");
        if (slash != std::wstring::npos) p = p.substr(0, slash);
    }
    if (FileExists(p + L"\\" + kGameExeName)) return p;
    if (FileExists(p + L"\\Binaries\\Win32\\" + kGameExeName)) return p + L"\\Binaries\\Win32";
    if (FileExists(p + L"\\Win32\\" + kGameExeName)) return p + L"\\Win32";
    return {};
}

bool LoadResourceBytes(int resId, const void*& outPtr, DWORD& outSize) {
    HRSRC hr = FindResourceW(g_app.hInst, MAKEINTRESOURCEW(resId), RT_RCDATA);
    if (!hr) return false;
    HGLOBAL hg = LoadResource(g_app.hInst, hr);
    if (!hg) return false;
    outPtr  = LockResource(hg);
    outSize = SizeofResource(g_app.hInst, hr);
    return outPtr != nullptr && outSize > 0;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 64 * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }
    out.resize(static_cast<size_t>(sz.QuadPart));
    DWORD got = 0;
    BOOL ok = ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    return ok && got == out.size();
}

bool WriteBytesAtomic(const std::wstring& path, const void* data, DWORD size, DWORD& outErr) {
    outErr = 0;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outErr = GetLastError();
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, size, &written, nullptr);
    if (!ok) outErr = GetLastError();
    CloseHandle(h);
    return ok && written == size;
}

std::wstring ReadRegString(HKEY root, const wchar_t* subKey, const wchar_t* valueName, REGSAM extraFlags = 0) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(root, subKey, 0, KEY_READ | extraFlags, &hk) != ERROR_SUCCESS) return {};
    wchar_t buf[1024]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    std::wstring out;
    if (RegQueryValueExW(hk, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb) == ERROR_SUCCESS &&
        (type == REG_SZ || type == REG_EXPAND_SZ)) {
        out = buf;
    }
    RegCloseKey(hk);
    return out;
}

void AddUniquePath(std::vector<std::wstring>& list, const std::wstring& p) {
    std::wstring t = TrimSlashes(p);
    if (t.empty()) return;
    for (const auto& existing : list) {
        if (_wcsicmp(existing.c_str(), t.c_str()) == 0) return;
    }
    list.push_back(t);
}

void ParseSteamLibraryFolders(const std::wstring& steamRoot, std::vector<std::wstring>& outLibs) {
    AddUniquePath(outLibs, steamRoot);
    std::vector<uint8_t> raw;
    if (!ReadFileBytes(TrimSlashes(steamRoot) + L"\\steamapps\\libraryfolders.vdf", raw)) return;

    std::string text(reinterpret_cast<const char*>(raw.data()), raw.size());
    size_t pos = 0;
    while (pos < text.size()) {
        size_t q1 = text.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = text.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string key = text.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        bool isNum = !key.empty() && std::all_of(key.begin(), key.end(), [](unsigned char c) {
            return c >= '0' && c <= '9';
        });
        if (key != "path" && !isNum) continue;

        size_t v1 = text.find('"', pos);
        if (v1 == std::string::npos) break;
        size_t v2 = text.find('"', v1 + 1);
        if (v2 == std::string::npos) break;
        std::string val = text.substr(v1 + 1, v2 - v1 - 1);
        pos = v2 + 1;

        if (val.find(":\\") == std::string::npos && val.find(":/") == std::string::npos) continue;
        std::string unescaped;
        unescaped.reserve(val.size());
        for (size_t i = 0; i < val.size(); ++i) {
            if (val[i] == '\\' && i + 1 < val.size() && val[i + 1] == '\\') {
                unescaped.push_back('\\');
                ++i;
            } else {
                unescaped.push_back(val[i]);
            }
        }
        int wlen = MultiByteToWideChar(CP_UTF8, 0, unescaped.c_str(), static_cast<int>(unescaped.size()), nullptr, 0);
        if (wlen > 0) {
            std::wstring wp(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(CP_UTF8, 0, unescaped.c_str(), static_cast<int>(unescaped.size()), wp.data(), wlen);
            AddUniquePath(outLibs, wp);
        }
    }
}

std::wstring FindGameInstallPath() {
    std::vector<std::wstring> steamRoots;
    for (const std::wstring& r : {
             ReadRegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
             ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"),
             ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY),
             std::wstring(L"C:\\Program Files (x86)\\Steam"),
             std::wstring(L"C:\\Program Files\\Steam"),
             std::wstring(L"D:\\Steam"),
             std::wstring(L"D:\\SteamLibrary"),
             std::wstring(L"E:\\SteamLibrary"),
         }) {
        if (!r.empty()) {
            std::wstring fixed = r;
            std::replace(fixed.begin(), fixed.end(), L'/', L'\\');
            AddUniquePath(steamRoots, fixed);
        }
    }

    std::vector<std::wstring> libraries;
    for (const auto& root : steamRoots) ParseSteamLibraryFolders(root, libraries);

    for (const auto& lib : libraries) {
        std::wstring p1 = lib + L"\\steamapps\\common\\A Story About My Uncle\\Binaries\\Win32";
        if (FileExists(p1 + L"\\" + kGameExeName)) return p1;
        std::wstring p2 = lib + L"\\A Story About My Uncle\\Binaries\\Win32";
        if (FileExists(p2 + L"\\" + kGameExeName)) return p2;
    }
    return {};
}

void DetectOpenXrRuntime() {
    std::wstring rt = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", KEY_WOW64_64KEY);
    if (rt.empty()) rt = ReadRegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime");
    if (rt.empty()) {
        g_app.openXrReady = false;
        g_app.openXrText = L"OpenXR not detected (launch SteamVR or Meta Quest Link before playing)";
        return;
    }
    g_app.openXrReady = FileExists(rt);
    std::wstring lower = rt;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    std::wstring name = L"OpenXR Runtime";
    if (lower.find(L"steamvr") != std::wstring::npos) name = L"SteamVR OpenXR";
    else if (lower.find(L"oculus") != std::wstring::npos) name = L"Meta Quest / Oculus OpenXR";
    else if (lower.find(L"virtualdesktop") != std::wstring::npos || lower.find(L"vdxr") != std::wstring::npos) name = L"Virtual Desktop VDXR";
    else if (lower.find(L"mixedreality") != std::wstring::npos) name = L"Windows Mixed Reality OpenXR";
    g_app.openXrText = name + (g_app.openXrReady ? L" ready" : L" (runtime file missing)");
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    return out;
}

std::wstring ComputeSha256Hex(const void* data, size_t size) {
    if (!data || size == 0) return {};
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) return {};

    DWORD hashLen = 0, cbData = 0;
    if (!BCRYPT_SUCCESS(BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLen),
                                          sizeof(hashLen), &cbData, 0)) ||
        hashLen != 32) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return {};
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::wstring hex;
    if (BCRYPT_SUCCESS(BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0))) {
        uint8_t digest[32]{};
        if (BCRYPT_SUCCESS(BCryptHashData(hHash, static_cast<PUCHAR>(const_cast<void*>(data)),
                                          static_cast<ULONG>(size), 0)) &&
            BCRYPT_SUCCESS(BCryptFinishHash(hHash, digest, sizeof(digest), 0))) {
            static constexpr wchar_t kHex[] = L"0123456789abcdef";
            hex.reserve(64);
            for (uint8_t b : digest) {
                hex.push_back(kHex[(b >> 4) & 0xF]);
                hex.push_back(kHex[b & 0xF]);
            }
        }
        BCryptDestroyHash(hHash);
    }
    BCryptCloseAlgorithmProvider(hAlg, 0);
    return hex;
}

bool ParseSemVer(const std::wstring& tag, int& maj, int& min, int& pat) {
    maj = min = pat = 0;
    size_t i = 0;
    if (i < tag.size() && (tag[i] == L'v' || tag[i] == L'V')) ++i;
    auto readNum = [&](int& out) -> bool {
        if (i >= tag.size() || tag[i] < L'0' || tag[i] > L'9') return false;
        int v = 0;
        while (i < tag.size() && tag[i] >= L'0' && tag[i] <= L'9') {
            v = v * 10 + (tag[i] - L'0');
            ++i;
        }
        out = v;
        return true;
    };
    if (!readNum(maj)) return false;
    if (i < tag.size() && tag[i] == L'.') { ++i; if (!readNum(min)) return false; }
    if (i < tag.size() && tag[i] == L'.') { ++i; if (!readNum(pat)) return false; }
    return true;
}

int CompareSemVer(const std::wstring& a, const std::wstring& b) {
    int amaj = 0, amin = 0, apat = 0;
    int bmaj = 0, bmin = 0, bpat = 0;
    if (!ParseSemVer(a, amaj, amin, apat) || !ParseSemVer(b, bmaj, bmin, bpat)) return 0;
    if (amaj != bmaj) return (amaj > bmaj) ? 1 : -1;
    if (amin != bmin) return (amin > bmin) ? 1 : -1;
    if (apat != bpat) return (apat > bpat) ? 1 : -1;
    return 0;
}

bool HttpGetBytes(const std::wstring& url, std::vector<uint8_t>& out, bool isApi) {
    out.clear();
    URL_COMPONENTSW uc{sizeof(uc)};
    wchar_t host[256]{};
    wchar_t path[2048]{};
    uc.lpszHostName      = host;
    uc.dwHostNameLength  = 256;
    uc.lpszUrlPath       = path;
    uc.dwUrlPathLength   = 2048;
    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &uc)) return false;

    HINTERNET hSession = WinHttpOpen(L"SAMU-VR-Launcher/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return false;
    WinHttpSetTimeouts(hSession, 3500, 3500, 4000, isApi ? 5000 : 25000);

    HINTERNET hConnect = WinHttpConnect(hSession, host, uc.nPort, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return false; }

    const DWORD flags = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                            WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    const wchar_t* hdrs = isApi ? L"Accept: application/vnd.github+json\r\n" : WINHTTP_NO_ADDITIONAL_HEADERS;
    bool ok = WinHttpSendRequest(hRequest, hdrs, hdrs ? static_cast<DWORD>(-1) : 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
              WinHttpReceiveResponse(hRequest, nullptr);
    if (ok) {
        DWORD status = 0, cbStatus = sizeof(status);
        if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 nullptr, &status, &cbStatus, nullptr) ||
            status != 200) {
            ok = false;
        }
    }

    if (ok) {
        constexpr size_t kMaxBytes = 16 * 1024 * 1024;
        for (;;) {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &avail)) { ok = false; break; }
            if (avail == 0) break;
            if (out.size() + avail > kMaxBytes) { ok = false; break; }
            const size_t oldSz = out.size();
            out.resize(oldSz + avail);
            DWORD read = 0;
            if (!WinHttpReadData(hRequest, out.data() + oldSz, avail, &read)) { ok = false; break; }
            out.resize(oldSz + read);
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return ok && !out.empty();
}

bool ExtractJsonStringField(const std::string& jsonSlice, const char* fieldName, std::string& outVal) {
    std::string needle = std::string("\"") + fieldName + "\"";
    size_t pos = jsonSlice.find(needle);
    while (pos != std::string::npos) {
        size_t p = pos + needle.size();
        while (p < jsonSlice.size() && (jsonSlice[p] == ' ' || jsonSlice[p] == '\t' || jsonSlice[p] == '\r' || jsonSlice[p] == '\n')) ++p;
        if (p < jsonSlice.size() && jsonSlice[p] == ':') {
            ++p;
            while (p < jsonSlice.size() && (jsonSlice[p] == ' ' || jsonSlice[p] == '\t' || jsonSlice[p] == '\r' || jsonSlice[p] == '\n')) ++p;
            if (p < jsonSlice.size() && jsonSlice[p] == '"') {
                ++p;
                outVal.clear();
                while (p < jsonSlice.size()) {
                    char c = jsonSlice[p++];
                    if (c == '"') return true;
                    if (c == '\\' && p < jsonSlice.size()) {
                        char esc = jsonSlice[p++];
                        if (esc == '"' || esc == '\\' || esc == '/') outVal.push_back(esc);
                    } else {
                        outVal.push_back(c);
                    }
                }
                return false;
            }
        }
        pos = jsonSlice.find(needle, pos + needle.size());
    }
    return false;
}

RemoteReleaseInfo ParseLatestReleaseJson(const std::string& json) {
    RemoteReleaseInfo info;
    std::string tag;
    if (!ExtractJsonStringField(json, "tag_name", tag) || tag.empty()) return info;
    info.tag = Utf8ToWide(tag);

    size_t assetsPos = json.find("\"assets\"");
    if (assetsPos == std::string::npos) return info;
    size_t arrStart = json.find('[', assetsPos);
    if (arrStart == std::string::npos) return info;

    size_t p = arrStart + 1;
    bool inStr = false;
    int  depth = 0;
    size_t objStart = std::string::npos;
    for (; p < json.size(); ++p) {
        char c = json[p];
        if (inStr) {
            if (c == '\\' && p + 1 < json.size()) { ++p; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == ']' && depth == 0) break;
        if (c == '{') {
            if (depth == 0) objStart = p;
            ++depth;
        } else if (c == '}') {
            if (depth > 0) --depth;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = json.substr(objStart, p - objStart + 1);
                std::string name, url, digest;
                if (ExtractJsonStringField(obj, "name", name) && name == "d3d9.dll" &&
                    ExtractJsonStringField(obj, "browser_download_url", url) &&
                    url.rfind("https://", 0) == 0) {
                    info.dllUrl = Utf8ToWide(url);
                    if (ExtractJsonStringField(obj, "digest", digest) && digest.rfind("sha256:", 0) == 0) {
                        std::string hex = digest.substr(7);
                        std::transform(hex.begin(), hex.end(), hex.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                        info.sha256 = Utf8ToWide(hex);
                    }
                    info.valid = true;
                    return info;
                }
                objStart = std::string::npos;
            }
        }
    }
    return info;
}

DWORD WINAPI RemoteCheckThread(LPVOID param) {
    HWND hwnd = static_cast<HWND>(param);
    std::wstring apiUrl = std::wstring(L"https://") + kGithubApiHost + kGithubLatestPath;
    std::vector<uint8_t> body;
    auto* info = new RemoteReleaseInfo();
    if (HttpGetBytes(apiUrl, body, true)) {
        std::string json(reinterpret_cast<const char*>(body.data()), body.size());
        *info = ParseLatestReleaseJson(json);
    }
    if (!hwnd || !PostMessageW(hwnd, WM_APP_REMOTE_CHECK, 0, reinterpret_cast<LPARAM>(info))) {
        delete info;
    }
    return 0;
}

bool PerformInstallFromBytes(const std::wstring& win32Dir, const void* dllBytes, DWORD dllSize, bool overwriteIni, DWORD& outErr) {
    outErr = 0;
    if (!dllBytes || dllSize == 0) {
        outErr = ERROR_RESOURCE_DATA_NOT_FOUND;
        return false;
    }
    if (!WriteBytesAtomic(win32Dir + L"\\" + kProxyDllName, dllBytes, dllSize, outErr)) return false;

    const std::wstring iniPath = win32Dir + L"\\" + kConfigIniName;
    if (overwriteIni || !FileExists(iniPath)) {
        const void* iniBytes = nullptr;
        DWORD iniSize = 0;
        if (LoadResourceBytes(IDR_DEFAULT_INI, iniBytes, iniSize)) {
            if (!WriteBytesAtomic(iniPath, iniBytes, iniSize, outErr)) return false;
        }
    }

    const char appIdBytes[] = "278360";
    DWORD tmpErr = 0;
    WriteBytesAtomic(win32Dir + L"\\steam_appid.txt", appIdBytes, sizeof(appIdBytes) - 1, tmpErr);
    return true;
}

bool PerformInstallCore(const std::wstring& win32Dir, bool overwriteIni, DWORD& outErr) {
    const void* dllBytes = nullptr;
    DWORD dllSize = 0;
    if (!LoadResourceBytes(IDR_PROXY_DLL, dllBytes, dllSize)) {
        outErr = ERROR_RESOURCE_DATA_NOT_FOUND;
        return false;
    }
    return PerformInstallFromBytes(win32Dir, dllBytes, dllSize, overwriteIni, outErr);
}

bool PerformInstallFromFile(const std::wstring& win32Dir, const std::wstring& srcDllPath, bool overwriteIni, DWORD& outErr) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(srcDllPath, bytes) || bytes.size() < 65536 || bytes[0] != 'M' || bytes[1] != 'Z') {
        outErr = ERROR_INVALID_DATA;
        return false;
    }
    return PerformInstallFromBytes(win32Dir, bytes.data(), static_cast<DWORD>(bytes.size()), overwriteIni, outErr);
}

bool PerformUninstallCore(const std::wstring& win32Dir, DWORD& outErr) {
    outErr = 0;
    std::wstring dllPath = win32Dir + L"\\" + kProxyDllName;
    if (FileExists(dllPath) && !DeleteFileW(dllPath.c_str())) {
        outErr = GetLastError();
        return false;
    }
    return true;
}

bool RunElevatedCommand(const std::wstring& args) {
    wchar_t selfPath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd   = g_app.hwnd;
    sei.lpVerb = L"runas";
    sei.lpFile = selfPath;
    sei.lpParameters = args.c_str();
    sei.nShow  = SW_HIDE;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 15000);
        DWORD code = 1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return code == 0;
    }
    return false;
}

void SetFeedback(const std::wstring& msg) {
    SetWindowTextW(GetDlgItem(g_app.hwnd, IDC_LBL_FEEDBACK), msg.c_str());
    InvalidateRect(GetDlgItem(g_app.hwnd, IDC_LBL_FEEDBACK), nullptr, TRUE);
}

void RefreshStatus() {
    wchar_t buf[MAX_PATH]{};
    GetWindowTextW(GetDlgItem(g_app.hwnd, IDC_EDIT_PATH), buf, MAX_PATH);
    g_app.win32Dir = NormalizeWin32Dir(buf);

    const void* embPtr = nullptr;
    DWORD embSize = 0;
    const bool haveEmb = LoadResourceBytes(IDR_PROXY_DLL, embPtr, embSize);
    const bool remoteNewer = g_app.remote.valid && (CompareSemVer(g_app.remote.tag, kLauncherVersion) > 0);
    g_app.remoteUpdateAvailable = false;

    HWND hStatus    = GetDlgItem(g_app.hwnd, IDC_LBL_STATUS);
    HWND hLaunch    = GetDlgItem(g_app.hwnd, IDC_BTN_LAUNCH);
    HWND hInstall   = GetDlgItem(g_app.hwnd, IDC_BTN_INSTALL);
    HWND hUninstall = GetDlgItem(g_app.hwnd, IDC_BTN_UNINSTALL);

    if (g_app.win32Dir.empty()) {
        g_app.modStatus = AppState::ModStatus::NoGameFound;
        SetWindowTextW(hStatus, L"Game not found  —  choose your install folder");
        SetWindowTextW(hLaunch, L"Launch in VR");
        SetWindowTextW(hInstall, L"Install");
        EnableWindow(hLaunch, FALSE);
        EnableWindow(hInstall, FALSE);
        EnableWindow(hUninstall, FALSE);
    } else {
        EnableWindow(hLaunch, TRUE);
        EnableWindow(hInstall, TRUE);

        std::wstring dllPath = g_app.win32Dir + L"\\" + kProxyDllName;
        if (!FileExists(dllPath)) {
            g_app.modStatus = AppState::ModStatus::NotInstalled;
            g_app.remoteUpdateAvailable = remoteNewer;
            std::wstring st = remoteNewer
                ? (L"Ready to install (" + g_app.remote.tag + L" from GitHub)")
                : L"Ready to install";
            SetWindowTextW(hStatus, st.c_str());
            SetWindowTextW(hLaunch, L"Install && Launch VR");
            SetWindowTextW(hInstall, L"Install");
            EnableWindow(hUninstall, FALSE);
        } else {
            std::vector<uint8_t> cur;
            const bool readOk = ReadFileBytes(dllPath, cur);
            bool match = false;
            if (remoteNewer) {
                const std::wstring curSha = readOk ? ComputeSha256Hex(cur.data(), cur.size()) : std::wstring{};
                match = !g_app.remote.sha256.empty() && curSha == g_app.remote.sha256;
                g_app.remoteUpdateAvailable = !match;
            } else {
                match = haveEmb && readOk &&
                        cur.size() == embSize && std::memcmp(cur.data(), embPtr, embSize) == 0;
            }
            g_app.modStatus = match ? AppState::ModStatus::UpToDate : AppState::ModStatus::UpdateAvailable;
            if (match) {
                std::wstring st = remoteNewer
                    ? (L"VR Mod installed (" + g_app.remote.tag + L", up to date)")
                    : L"VR Mod installed (up to date)";
                SetWindowTextW(hStatus, st.c_str());
                SetWindowTextW(hInstall, L"Reinstall");
            } else {
                std::wstring st = remoteNewer
                    ? (L"Update available (" + g_app.remote.tag + L" on GitHub)")
                    : L"Update available";
                SetWindowTextW(hStatus, st.c_str());
                SetWindowTextW(hInstall, L"Update");
            }
            SetWindowTextW(hLaunch, L"Launch in VR");
            EnableWindow(hUninstall, TRUE);
        }
    }

    DetectOpenXrRuntime();
    SetWindowTextW(GetDlgItem(g_app.hwnd, IDC_LBL_OPENXR), g_app.openXrText.c_str());

    InvalidateRect(g_app.hwnd, nullptr, TRUE);
    InvalidateRect(hLaunch, nullptr, TRUE);
    InvalidateRect(hInstall, nullptr, TRUE);
    InvalidateRect(hUninstall, nullptr, TRUE);
}

void BrowseForGameFolder() {
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))) && dlg) {
        DWORD opts = 0;
        dlg->GetOptions(&opts);
        dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dlg->SetTitle(L"Select \"A Story About My Uncle\" folder");
        if (SUCCEEDED(dlg->Show(g_app.hwnd))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    std::wstring norm = NormalizeWin32Dir(path);
                    if (norm.empty()) {
                        MessageBoxW(g_app.hwnd,
                                    L"Could not find ASAMU-Win32-Shipping.exe in that folder or Binaries\\Win32.",
                                    kAppTitle, MB_ICONWARNING | MB_OK);
                    } else {
                        SetWindowTextW(GetDlgItem(g_app.hwnd, IDC_EDIT_PATH), norm.c_str());
                        RefreshStatus();
                        SetFeedback(L"");
                    }
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
}

bool DoInstall(bool quietSuccess = false) {
    if (g_app.win32Dir.empty()) return false;

    std::vector<uint8_t> downloaded;
    bool useRemote = false;
    if (g_app.remoteUpdateAvailable && g_app.remote.valid) {
        SetFeedback(L"Downloading " + g_app.remote.tag + L" from GitHub...");
        UpdateWindow(g_app.hwnd);
        if (HttpGetBytes(g_app.remote.dllUrl, downloaded, false) &&
            downloaded.size() >= 65536 && downloaded[0] == 'M' && downloaded[1] == 'Z') {
            if (g_app.remote.sha256.empty() ||
                ComputeSha256Hex(downloaded.data(), downloaded.size()) == g_app.remote.sha256) {
                useRemote = true;
            }
        }
        if (!useRemote) {
            int ans = MessageBoxW(
                g_app.hwnd,
                L"Could not download or verify the latest update from GitHub.\n\nInstall the bundled offline version instead?",
                kAppTitle, MB_ICONWARNING | MB_YESNO);
            if (ans != IDYES) {
                SetFeedback(L"Update cancelled.");
                return false;
            }
        }
    }

    DWORD err = 0;
    const bool ok = useRemote
        ? PerformInstallFromBytes(g_app.win32Dir, downloaded.data(), static_cast<DWORD>(downloaded.size()), false, err)
        : PerformInstallCore(g_app.win32Dir, false, err);
    if (!ok) {
        if (err == ERROR_ACCESS_DENIED) {
            bool elevOk = false;
            if (useRemote) {
                wchar_t tmpDir[MAX_PATH]{};
                wchar_t tmpFile[MAX_PATH]{};
                if (GetTempPathW(MAX_PATH, tmpDir) && GetTempFileNameW(tmpDir, L"smu", 0, tmpFile)) {
                    DWORD wErr = 0;
                    if (WriteBytesAtomic(tmpFile, downloaded.data(), static_cast<DWORD>(downloaded.size()), wErr)) {
                        elevOk = RunElevatedCommand(L"--install-file \"" + g_app.win32Dir + L"\" \"" + tmpFile + L"\"");
                    }
                    DeleteFileW(tmpFile);
                }
            } else {
                elevOk = RunElevatedCommand(L"--install-keep \"" + g_app.win32Dir + L"\"");
            }
            if (!elevOk) {
                MessageBoxW(g_app.hwnd,
                            L"Write access denied. Please approve Administrator access when prompted.",
                            kAppTitle, MB_ICONERROR | MB_OK);
                return false;
            }
        } else {
            wchar_t msg[256]{};
            std::swprintf(msg, 256, L"Install failed (error %lu). Make sure the game is closed.", err);
            MessageBoxW(g_app.hwnd, msg, kAppTitle, MB_ICONERROR | MB_OK);
            return false;
        }
    }
    RefreshStatus();
    if (!quietSuccess) SetFeedback(useRemote ? (L"Updated to " + g_app.remote.tag + L".") : L"Mod installed.");
    return true;
}

void DoLaunchGame() {
    if (g_app.modStatus != AppState::ModStatus::UpToDate) {
        if (!DoInstall(true)) return;
    }
    if (!g_app.openXrReady) {
        int res = MessageBoxW(
            g_app.hwnd,
            L"No OpenXR headset runtime was detected.\n\nStart SteamVR or Meta Quest Link first, or click OK to launch anyway.",
            kAppTitle, MB_ICONWARNING | MB_OKCANCEL);
        if (res != IDOK) return;
    }

    SetFeedback(L"Launching game...");
    std::wstring lower = g_app.win32Dir;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    if (lower.find(L"\\steamapps\\") != std::wstring::npos) {
        std::wstring uri = std::wstring(L"steam://rungameid/") + kSteamAppId;
        HINSTANCE r = ShellExecuteW(g_app.hwnd, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) > 32) return;
    }
    std::wstring exePath = g_app.win32Dir + L"\\" + kGameExeName;
    ShellExecuteW(g_app.hwnd, L"open", exePath.c_str(), nullptr, g_app.win32Dir.c_str(), SW_SHOWNORMAL);
}

void DoUninstall() {
    if (g_app.win32Dir.empty()) return;
    int ans = MessageBoxW(g_app.hwnd,
                          L"Remove SAMU-VR (d3d9.dll) and restore the vanilla flat-screen game?",
                          kAppTitle, MB_ICONQUESTION | MB_YESNO);
    if (ans != IDYES) return;

    DWORD err = 0;
    if (!PerformUninstallCore(g_app.win32Dir, err)) {
        if (err == ERROR_ACCESS_DENIED) {
            if (!RunElevatedCommand(L"--uninstall \"" + g_app.win32Dir + L"\"")) {
                MessageBoxW(g_app.hwnd, L"Could not remove d3d9.dll (close the game or approve Admin access).", kAppTitle, MB_ICONERROR | MB_OK);
                return;
            }
        } else {
            wchar_t msg[256]{};
            std::swprintf(msg, 256, L"Could not remove d3d9.dll (error %lu). Make sure the game is closed.", err);
            MessageBoxW(g_app.hwnd, msg, kAppTitle, MB_ICONERROR | MB_OK);
            return;
        }
    }
    RefreshStatus();
    SetFeedback(L"Mod uninstalled.");
}

void DrawButton(const DRAWITEMSTRUCT* dis) {
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const int id = static_cast<int>(dis->CtlID);
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed  = (dis->itemState & ODS_SELECTED) != 0;
    const bool hovered  = (id == g_app.hoverId) && !disabled;
    const bool primary  = (id == IDC_BTN_LAUNCH);

    COLORREF bg   = primary ? RGB(238, 240, 244) : RGB(36, 39, 46);
    COLORREF edge = primary ? RGB(238, 240, 244) : RGB(56, 60, 70);
    COLORREF fg   = primary ? RGB(18, 20, 24)    : kColTextHi;

    if (hovered) {
        bg   = primary ? RGB(255, 255, 255) : RGB(46, 50, 59);
        edge = primary ? RGB(255, 255, 255) : RGB(78, 84, 98);
    }
    if (pressed) {
        bg = primary ? RGB(212, 215, 222) : RGB(28, 30, 36);
    }
    if (disabled) {
        bg   = RGB(28, 30, 35);
        edge = RGB(40, 43, 50);
        fg   = RGB(92, 97, 108);
    }

    // Clear parent background behind rounded corners cleanly
    FillRect(hdc, &rc, g_app.brBg);

    HBRUSH fill = CreateSolidBrush(bg);
    HPEN   pen  = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ oldB = SelectObject(hdc, fill);
    HGDIOBJ oldP = SelectObject(hdc, pen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, S(7), S(7));
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(fill);
    DeleteObject(pen);

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg);
    HGDIOBJ oldF = SelectObject(hdc, primary ? g_app.fontBold : g_app.fontBody);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldF);
}

LRESULT CALLBACK BtnHoverProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    const int id = GetDlgCtrlID(hwnd);
    if (msg == WM_MOUSEMOVE) {
        if (g_app.hoverId != id) {
            g_app.hoverId = id;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    } else if (msg == WM_MOUSELEAVE) {
        if (g_app.hoverId == id) {
            g_app.hoverId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

void InitResources() {
    g_app.brBg      = CreateSolidBrush(kColBg);
    g_app.brSurface = CreateSolidBrush(kColSurface);

    auto mkFont = [](int pt, int weight) -> HFONT {
        return CreateFontW(-MulDiv(pt, static_cast<int>(g_app.dpi), 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    };
    g_app.fontTitle = mkFont(17, FW_SEMIBOLD);
    g_app.fontBold  = mkFont(10, FW_SEMIBOLD);
    g_app.fontBody  = mkFont(9,  FW_NORMAL);
    g_app.fontSmall = mkFont(8,  FW_NORMAL);
}

void CreateControls(HWND hwnd) {
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id, HFONT f) -> HWND {
        HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style,
                                 S(x), S(y), S(w), S(h), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_app.hInst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(f), TRUE);
        return c;
    };
    auto mkBtn = [&](const wchar_t* txt, int x, int y, int w, int h, int id) {
        HWND b = mk(L"BUTTON", txt, BS_OWNERDRAW | WS_TABSTOP, x, y, w, h, id, g_app.fontBody);
        SetWindowSubclass(b, BtnHoverProc, 0, 0);
    };

    // Title + subtitle
    mk(L"STATIC", L"SAMU-VR", SS_LEFT, 24, 22, 160, 28, 0, g_app.fontTitle);
    mk(L"STATIC", L"A Story About My Uncle  —  OpenXR VR Mod", SS_LEFT, 25, 50, 320, 18, 0, g_app.fontBody);

    // Status lines (inside surface card y=84..194)
    mk(L"STATIC", L"Checking...", SS_LEFT, 52, 99,  376, 18, IDC_LBL_STATUS, g_app.fontBold);
    mk(L"STATIC", L"OpenXR...",   SS_LEFT, 36, 121, 392, 16, IDC_LBL_OPENXR, g_app.fontSmall);

    // Path input + Browse button
    mk(L"EDIT", L"", ES_AUTOHSCROLL | ES_READONLY | WS_TABSTOP, 44, 152, 290, 22, IDC_EDIT_PATH, g_app.fontBody);
    mkBtn(L"Browse", 346, 146, 82, 32, IDC_BTN_BROWSE);

    // Action row (y=212..252)
    mkBtn(L"Launch in VR", 24,  212, 212, 40, IDC_BTN_LAUNCH);
    mkBtn(L"Install",      244, 212, 104, 40, IDC_BTN_INSTALL);
    mkBtn(L"Uninstall",    356, 212, 92,  40, IDC_BTN_UNINSTALL);

    // Quiet footer feedback + controls reminder
    mk(L"STATIC", L"In-game VR menu: Click Right Stick (R3) or press F10", SS_LEFT, 24, 268, 424, 16, IDC_LBL_FEEDBACK, g_app.fontSmall);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        g_app.hwnd = hwnd;
        EnableDarkTitleBarAndDpi(hwnd);
        InitResources();
        CreateControls(hwnd);
        g_app.win32Dir = FindGameInstallPath();
        if (!g_app.win32Dir.empty()) {
            SetWindowTextW(GetDlgItem(hwnd, IDC_EDIT_PATH), g_app.win32Dir.c_str());
        }
        RefreshStatus();
        if (HANDLE ht = CreateThread(nullptr, 0, &RemoteCheckThread, hwnd, 0, nullptr)) {
            CloseHandle(ht);
        }
        return 0;

    case WM_APP_REMOTE_CHECK: {
        auto* info = reinterpret_cast<RemoteReleaseInfo*>(lp);
        if (info) {
            if (info->valid) {
                g_app.remote = std::move(*info);
                RefreshStatus();
            }
            delete info;
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT cli{};
        GetClientRect(hwnd, &cli);
        FillRect(hdc, &cli, g_app.brBg);

        // Single clean surface card
        HGDIOBJ oldB = SelectObject(hdc, g_app.brSurface);
        HPEN pen     = CreatePen(PS_SOLID, 1, kColBorder);
        HGDIOBJ oldP = SelectObject(hdc, pen);
        RoundRect(hdc, S(24), S(84), S(448), S(194), S(10), S(10));

        // Inner path field frame
        HBRUSH brIn = CreateSolidBrush(kColBg);
        SelectObject(hdc, brIn);
        RoundRect(hdc, S(36), S(146), S(340), S(178), S(6), S(6));

        // Status indicator dot
        COLORREF dotCol = kColDotWarn;
        if (g_app.modStatus == AppState::ModStatus::UpToDate) dotCol = kColDotOk;
        else if (g_app.modStatus == AppState::ModStatus::NoGameFound) dotCol = kColDotErr;
        HBRUSH brDot = CreateSolidBrush(dotCol);
        HPEN   penDot = CreatePen(PS_SOLID, 1, dotCol);
        SelectObject(hdc, brDot);
        SelectObject(hdc, penDot);
        Ellipse(hdc, S(37), S(104), S(45), S(112));

        SelectObject(hdc, oldB);
        SelectObject(hdc, oldP);
        DeleteObject(pen);
        DeleteObject(brIn);
        DeleteObject(brDot);
        DeleteObject(penDot);

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wp);
        HWND ctrl = reinterpret_cast<HWND>(lp);
        const int id = GetDlgCtrlID(ctrl);
        SetBkMode(hdc, TRANSPARENT);
        if (id == IDC_EDIT_PATH) {
            SetTextColor(hdc, kColTextHi);
            SetBkColor(hdc, kColBg);
            return reinterpret_cast<LRESULT>(g_app.brBg);
        }
        if (id == IDC_LBL_STATUS) {
            SetTextColor(hdc, kColTextHi);
            return reinterpret_cast<LRESULT>(g_app.brSurface);
        }
        if (id == IDC_LBL_OPENXR) {
            SetTextColor(hdc, kColTextMuted);
            return reinterpret_cast<LRESULT>(g_app.brSurface);
        }
        if (id == IDC_LBL_FEEDBACK) {
            SetTextColor(hdc, kColTextSubtle);
            return reinterpret_cast<LRESULT>(g_app.brBg);
        }
        RECT rc{};
        GetWindowRect(ctrl, &rc);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&rc), 2);
        SetTextColor(hdc, (rc.top < S(45)) ? kColTextHi : kColTextMuted);
        return reinterpret_cast<LRESULT>(g_app.brBg);
    }

    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(g_app.brBg);

    case WM_DRAWITEM:
        DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lp));
        return TRUE;

    case WM_COMMAND: {
        if (HIWORD(wp) == BN_CLICKED) {
            const int id = LOWORD(wp);
            if (id == IDC_BTN_BROWSE)         BrowseForGameFolder();
            else if (id == IDC_BTN_LAUNCH)    DoLaunchGame();
            else if (id == IDC_BTN_INSTALL)   DoInstall(false);
            else if (id == IDC_BTN_UNINSTALL) DoUninstall();
        }
        return 0;
    }

    case WM_DESTROY:
        DeleteObject(g_app.brBg);
        DeleteObject(g_app.brSurface);
        DeleteObject(g_app.fontTitle);
        DeleteObject(g_app.fontBold);
        DeleteObject(g_app.fontBody);
        DeleteObject(g_app.fontSmall);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int HandleElevatedCli(int argc, LPWSTR* argv) {
    if (argc < 3) return -1;
    std::wstring cmd = argv[1];
    DWORD err = 0;
    if (cmd == L"--install-fresh" || cmd == L"--install-keep") {
        std::wstring dir = NormalizeWin32Dir(argv[2]);
        if (dir.empty()) return 1;
        return PerformInstallCore(dir, cmd == L"--install-fresh", err) ? 0 : 1;
    }
    if (cmd == L"--install-file" && argc >= 4) {
        std::wstring dir = NormalizeWin32Dir(argv[2]);
        if (dir.empty()) return 1;
        return PerformInstallFromFile(dir, argv[3], false, err) ? 0 : 1;
    }
    if (cmd == L"--uninstall") {
        std::wstring dir = NormalizeWin32Dir(argv[2]);
        if (dir.empty()) return 1;
        return PerformUninstallCore(dir, err) ? 0 : 1;
    }
    return -1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int nCmdShow) {
    EnableDarkTitleBarAndDpi();
    g_app.hInst = hInst;

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv) {
        int cliRes = HandleElevatedCli(argc, argv);
        LocalFree(argv);
        if (cliRes >= 0) return cliRes;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
    InitCommonControlsEx(&icc);

    HDC screenDc = GetDC(nullptr);
    g_app.dpi = static_cast<UINT>(GetDeviceCaps(screenDc, LOGPIXELSX));
    ReleaseDC(nullptr, screenDc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kWindowClass;
    RegisterClassExW(&wc);

    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT wr{0, 0, S(472), S(302)};
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    const int winW = wr.right - wr.left;
    const int winH = wr.bottom - wr.top;
    const int sx = (GetSystemMetrics(SM_CXSCREEN) - winW) / 2;
    const int sy = (GetSystemMetrics(SM_CYSCREEN) - winH) / 2;

    HWND hwnd = CreateWindowExW(0, kWindowClass, kAppTitle, style,
                                sx, sy, winW, winH, nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
