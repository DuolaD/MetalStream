#pragma once

#include <string>
#include <sstream>
#include <mutex>
#include <fstream>
#include <windows.h>
#include <streamline/sl_core_types.h>

namespace metalstream {

enum class LogLevel {
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3
};

class Logger {
public:
    static Logger& Instance();

    void Init(const std::wstring& logFileName = L"metalstream.log");
    void Shutdown();

    inline bool IsEnabled(LogLevel level) const {
        return m_enabled && (static_cast<int>(level) >= static_cast<int>(m_minLevel));
    }

    void SetEnabled(bool enabled) { m_enabled = enabled; }
    void SetMinLogLevel(LogLevel level) { m_minLevel = level; }
    void SetLogPath(const std::wstring& path) { Init(path); }
    void SetLogCallback(sl::PFun_LogMessageCallback* callback) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_logCallback = callback;
    }
    std::string GetLogPathUtf8() const;

    void Log(LogLevel level, const char* file, int line, const char* func, const std::string& message);

    template<typename... Args>
    void LogFormat(LogLevel level, const char* file, int line, const char* func, const char* format, Args... args) {
        if (!IsEnabled(level)) return;
        char buffer[2048];
        snprintf(buffer, sizeof(buffer), format, args...);
        Log(level, file, line, func, std::string(buffer));
    }

private:
    Logger();
    ~Logger();

    mutable std::recursive_mutex m_mutex;
    std::ofstream m_file;
    std::wstring m_logPath;
    bool m_initialized = false;
    bool m_enabled = false; // Default: OFF (can be toggled via env/ini/marker file)
    LogLevel m_minLevel = LogLevel::Info;
    sl::PFun_LogMessageCallback* m_logCallback = nullptr;
};

} // namespace metalstream

#define LOG_DEBUG(fmt, ...) \
    do { \
        if (metalstream::Logger::Instance().IsEnabled(metalstream::LogLevel::Debug)) { \
            metalstream::Logger::Instance().LogFormat(metalstream::LogLevel::Debug, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_INFO(fmt, ...) \
    do { \
        if (metalstream::Logger::Instance().IsEnabled(metalstream::LogLevel::Info)) { \
            metalstream::Logger::Instance().LogFormat(metalstream::LogLevel::Info, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_WARN(fmt, ...) \
    do { \
        if (metalstream::Logger::Instance().IsEnabled(metalstream::LogLevel::Warning)) { \
            metalstream::Logger::Instance().LogFormat(metalstream::LogLevel::Warning, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
        } \
    } while(0)

#define LOG_ERROR(fmt, ...) \
    do { \
        metalstream::Logger::Instance().LogFormat(metalstream::LogLevel::Error, __FILE__, __LINE__, __FUNCTION__, fmt, ##__VA_ARGS__); \
    } while(0)
