#include "config.h"
#include "logger.h"

#include <climits>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

namespace metalstream {
namespace {

struct KeyDef {
    const char* name;
    int Config::* field;
    int defaultValue;
    int minValue;
    int maxValue;
    bool clampToRange; // out of range: clamp (numeric settings) instead of falling back to the default (switches)
};

// The single list of metalstream.ini keys. Keep README.md and metalstream.ini in sync with it.
const KeyDef kKeys[] = {
    {"EnableLogging",   &Config::enableLogging,   0,  0, 1,    false},
    {"Debug",           &Config::debug,           0,  0, 1,    false},
    {"FrameGeneration", &Config::frameGeneration, 1,  0, 1,    false},
    {"FGDebugView",     &Config::debugView,       0,  0, 1,    false},
    {"FGPacing",        &Config::pacing,          1,  0, 1,    false},
    {"FGMaxGameFps",    &Config::maxGameFps,      0, -1, 1000, true},
    {"FGCameraData",    &Config::cameraData,      1,  0, 1,    false},
    {"FGCapture",       &Config::capture,         0,  0, 2,    false},
};
constexpr size_t kKeyCount = sizeof(kKeys) / sizeof(kKeys[0]);

struct LoadedConfig {
    Config values{};
    bool fromIni[kKeyCount] = {};
    bool corrected[kKeyCount] = {};
    bool iniFound = false;
    std::string iniPath;
    std::vector<std::string> corrections;
};

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring IniPath() {
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&IniPath), &self);
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return {};
    std::wstring dir(path);
    return dir.substr(0, dir.find_last_of(L"\\/") + 1) + L"metalstream.ini";
}

// Accepts an optional sign, digits, then only whitespace or a ;/# comment
bool ParseInt(const wchar_t* text, int* out) {
    wchar_t* end = nullptr;
    long v = wcstol(text, &end, 10);
    if (end == text) return false;
    while (*end == L' ' || *end == L'\t') ++end;
    if (*end != L'\0' && *end != L';' && *end != L'#') return false;
    if (v < INT_MIN || v > INT_MAX) return false;
    *out = (int)v;
    return true;
}

LoadedConfig Load() {
    LoadedConfig cfg;
    for (const KeyDef& k : kKeys) cfg.values.*k.field = k.defaultValue;

    const std::wstring ini = IniPath();
    cfg.iniPath = ToUtf8(ini);
    cfg.iniFound = !ini.empty() && GetFileAttributesW(ini.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!cfg.iniFound) return cfg;

    static const wchar_t kMissing[] = L"\x01missing";
    char line[256];
    for (size_t i = 0; i < kKeyCount; ++i) {
        const KeyDef& k = kKeys[i];
        std::wstring name(k.name, k.name + strlen(k.name));
        wchar_t text[64] = {};
        GetPrivateProfileStringW(L"MetalStream", name.c_str(), kMissing, text, 64, ini.c_str());
        if (wcscmp(text, kMissing) == 0) continue;

        int v = 0;
        if (!ParseInt(text, &v)) {
            snprintf(line, sizeof line, "%s=%s is not a number; using %d (default)", k.name, ToUtf8(text).c_str(), k.defaultValue);
            cfg.corrections.push_back(line);
            cfg.corrected[i] = true;
            continue;
        }
        if (v < k.minValue || v > k.maxValue) {
            int corrected = k.clampToRange ? (v < k.minValue ? k.minValue : k.maxValue) : k.defaultValue;
            snprintf(line, sizeof line, "%s=%d is outside [%d, %d]; using %d%s", k.name, v, k.minValue, k.maxValue,
                     corrected, k.clampToRange ? "" : " (default)");
            cfg.corrections.push_back(line);
            cfg.corrected[i] = true;
            v = corrected;
        }
        cfg.values.*k.field = v;
        cfg.fromIni[i] = true;
    }
    return cfg;
}

const LoadedConfig& Loaded() {
    static const LoadedConfig s = Load();
    return s;
}

} // namespace

const Config& Config::Get() {
    return Loaded().values;
}

static void LogConfigSummary() {
    const LoadedConfig& cfg = Loaded();
    std::string summary;
    for (size_t i = 0; i < kKeyCount; ++i) {
        char item[96];
        snprintf(item, sizeof item, "%s%s=%d (%s)", i ? ", " : "", kKeys[i].name, cfg.values.*kKeys[i].field,
                 cfg.corrected[i] ? "ini, corrected" : cfg.fromIni[i] ? "ini" : "default");
        summary += item;
    }
    LOG_INFO("config: %s %s: %s", cfg.iniFound ? "read" : "no", cfg.iniPath.empty() ? "metalstream.ini" : cfg.iniPath.c_str(),
             summary.c_str());
    for (const std::string& c : cfg.corrections) LOG_WARN("config: %s", c.c_str());
}

void Config::LogSummary() {
    static std::once_flag once;
    std::call_once(once, &LogConfigSummary);
}

} // namespace metalstream
