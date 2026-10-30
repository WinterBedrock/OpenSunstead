#pragma once

#include <string>

class Logger {
public:
    enum class Level {
        Info,
        Warning,
        Error
    };

    static void Initialize();
    static void Shutdown();

    static void Info(const std::string& message, const std::string& source = "main");
    static void Warn(const std::string& message, const std::string& source = "main");
    static void Error(const std::string& message, const std::string& source = "main");

private:
    static void Write(Level level, const std::string& source, const std::string& message);
    static std::string GetTimestamp();
    static std::string GetLevelLabel(Level level);
    static std::string GetLevelColor(Level level);
    static std::string GetResetColor();
    static std::string GetLogFilePath();
    static void EnsureConsole();
};

// ---------------------------------------------------------------------------
// Call-site logging macros.
//
// Release builds must not carry debug strings for reverse engineers to grep,
// so Info/Warning calls (message literals included) are compiled OUT
// entirely. Error logs (crash diagnostics) stay in every configuration —
// always call those through LOG_ERROR. SUNSTEAD_DEBUG comes from CMake's
// Debug config (independent of the CRT flavour), _DEBUG is honoured too.
// ---------------------------------------------------------------------------
#if defined(_DEBUG) || defined(SUNSTEAD_DEBUG)
#define SUNSTEAD_LOGGING 1
#endif
#ifdef SUNSTEAD_LOGGING
#define LOG_INFO(...) Logger::Info(__VA_ARGS__)
#define LOG_WARN(...) Logger::Warn(__VA_ARGS__)
#else
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#endif
#define LOG_ERROR(...) Logger::Error(__VA_ARGS__)
