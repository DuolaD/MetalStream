#include "logger.h"
#include "config.h"
#include <chrono>
#include <iomanip>
#include <iostream>
#include <cstdlib>
#include <strings.h>

namespace metalstream {

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::Logger() {
    // 1. Check environment variable toggle (e.g. METALSTREAM_DEBUG=1 or METALSTREAM_LOG=1)
    const char* envDebug = getenv("METALSTREAM_DEBUG");
    const char* envLog = getenv("METALSTREAM_LOG");
    if (envDebug && (strcmp(envDebug, "1") == 0 || strcasecmp(envDebug, "true") == 0)) {
        m_enabled = true;
        m_minLevel = LogLevel::Debug;
    } else if (envLog && (strcmp(envLog, "1") == 0 || strcasecmp(envLog, "true") == 0)) {
        m_enabled = true;
        m_minLevel = LogLevel::Info;
    }

    // 2. Check marker file in module directory, then metalstream.ini (see config.cpp)
    wchar_t modulePath[MAX_PATH];
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Logger::Instance, &hMod);
    if (GetModuleFileNameW(hMod, modulePath, MAX_PATH) > 0) {
        std::wstring dir = modulePath;
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            dir = dir.substr(0, pos + 1);
        }

        // Marker file: metalstream.debug or metalstream_debug.txt
        if (GetFileAttributesW((dir + L"metalstream.debug").c_str()) != INVALID_FILE_ATTRIBUTES ||
            GetFileAttributesW((dir + L"metalstream_debug.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        {
            m_enabled = true;
            m_minLevel = LogLevel::Debug;
        }
    }
    const Config& cfg = Config::Get();
    if (cfg.enableLogging == 1) {
        m_enabled = true;
        m_minLevel = (cfg.debug == 1) ? LogLevel::Debug : LogLevel::Info;
    }
}

Logger::~Logger() {
    Shutdown();
}

void Logger::Init(const std::wstring& logFileName) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    wchar_t modulePath[MAX_PATH];
    HMODULE hMod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Logger::Instance, &hMod);
    
    bool isAbsolute = false;
    if (logFileName.size() >= 2 && iswalpha(logFileName[0]) && logFileName[1] == L':') {
        isAbsolute = true;
    } else if (!logFileName.empty() && (logFileName[0] == L'\\' || logFileName[0] == L'/')) {
        isAbsolute = true;
    }

    std::wstring fullPath = logFileName;
    if (!isAbsolute && GetModuleFileNameW(hMod, modulePath, MAX_PATH) > 0) {
        std::wstring dir = modulePath;
        size_t pos = dir.find_last_of(L"\\/");
        if (pos != std::wstring::npos) {
            fullPath = dir.substr(0, pos + 1) + logFileName;
        }
    }

    if (m_initialized && m_logPath == fullPath) {
        return;
    }

    if (m_file.is_open()) {
        m_file.flush();
        m_file.close();
    }

    m_logPath = fullPath;
    m_initialized = true;

    // Only open the log file if logging is actually enabled
    if (m_enabled) {
        m_file.open(fullPath.c_str(), std::ios::out | std::ios::trunc);
        if (m_file.is_open()) {
            m_file << "===================================================\n";
            m_file << " MetalStream: Streamline to MetalFX Proxy Log\n";
            m_file << "===================================================\n";
            m_file.flush();
        }
    }
}

std::string Logger::GetLogPathUtf8() const {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_logPath.empty()) return {};
    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, m_logPath.c_str(), (int)m_logPath.size(), nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0) return {};
    std::string str(sizeNeeded, 0);
    WideCharToMultiByte(CP_UTF8, 0, m_logPath.c_str(), (int)m_logPath.size(), &str[0], sizeNeeded, nullptr, nullptr);
    return str;
}

void Logger::Shutdown() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_file.is_open()) {
        m_file << "[MetalStream] Logger shutting down.\n";
        m_file.flush();
        m_file.close();
    }
    m_initialized = false;
}

void Logger::Log(LogLevel level, const char* file, int line, const char* func, const std::string& message) {
    sl::PFun_LogMessageCallback* callback = nullptr;
    std::string out;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        if (!m_initialized) {
            Init();
        }

        // If file was not yet opened (e.g. for LOG_ERROR when m_enabled was false), open it now in append mode
        if (!m_file.is_open() && !m_logPath.empty()) {
            m_file.open(m_logPath.c_str(), std::ios::out | std::ios::app);
        }

        const char* levelStr = "INFO";
        switch (level) {
            case LogLevel::Debug:   levelStr = "DEBUG"; break;
            case LogLevel::Info:    levelStr = "INFO";  break;
            case LogLevel::Warning: levelStr = "WARN";  break;
            case LogLevel::Error:   levelStr = "ERROR"; break;
        }

        // Timestamp
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::tm bt{};
        localtime_s(&bt, &in_time_t);

        std::stringstream ss;
        ss << "[" << std::put_time(&bt, "%H:%M:%S") << "." << std::setfill('0') << std::setw(3) << ms.count() << "]"
           << " [" << levelStr << "] [" << func << "] "
           << message << "\n";

        out = ss.str();

        if (m_file.is_open()) {
            m_file << out;
            m_file.flush();
        }

        callback = m_logCallback;
    }

    if (callback) {
        sl::LogType slType = sl::LogType::eInfo;
        if (level == LogLevel::Warning) slType = sl::LogType::eWarn;
        else if (level == LogLevel::Error) slType = sl::LogType::eError;
        callback(slType, message.c_str());
    }
    OutputDebugStringA(out.c_str());
}

} // namespace metalstream
