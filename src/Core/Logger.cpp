#include "Logger.h"
// SUNSTEAD_LOGGING comes from Logger.h (Debug config or _DEBUG)

#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {
    const char* kLogFileName = "logs.txt";
    std::ofstream g_LogFile;
    bool g_ConsoleInitialized = false;
    bool g_AnsiSupported = false;
    std::mutex g_LogMutex;

    bool TryEnableVirtualTerminal(HANDLE handle) {
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD mode = 0;
        if (!GetConsoleMode(handle, &mode)) {
            return false;
        }

        mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        return SetConsoleMode(handle, mode) != 0;
    }
}

void Logger::Initialize() {
    if (g_ConsoleInitialized) {
        return;
    }

#ifdef SUNSTEAD_LOGGING
    // Debug console is DEBUG-only: a console window in a release build is an
    // instant client-detection tell, and everything printed to it is already
    // stripped by the LOG_* macros.
    if (AllocConsole()) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        setvbuf(stdout, nullptr, _IONBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);

        SetConsoleTitleA("Sunstead Console");
        SetConsoleOutputCP(CP_UTF8);
        SetConsoleCP(CP_UTF8);

        g_AnsiSupported = TryEnableVirtualTerminal(GetStdHandle(STD_OUTPUT_HANDLE));
        TryEnableVirtualTerminal(GetStdHandle(STD_ERROR_HANDLE));

        g_ConsoleInitialized = true;
    }
#endif

    // Rotate an oversized log away before opening: the debug client logs a
    // lot (per-frame diagnostics), and an unbounded append-only file grew to
    // ~100MB over a few sessions. Anything past the cap moves to logs.old.txt
    // (one generation deep) so the live file always starts small and the
    // disk footprint stays bounded at ~2x the cap.
    const std::string logPath = GetLogFilePath();
    {
        WIN32_FILE_ATTRIBUTE_DATA fad = {};
        constexpr ULONGLONG kMaxLogBytes = 4ull * 1024ull * 1024ull;
        if (GetFileAttributesExA(logPath.c_str(), GetFileExInfoStandard, &fad) &&
            ((ULONGLONG(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow) > kMaxLogBytes) {
            const std::string oldPath = logPath + ".old";
            DeleteFileA(oldPath.c_str());
            MoveFileA(logPath.c_str(), oldPath.c_str());
        }
    }

    g_LogFile.open(logPath.c_str(), std::ios::app);
    if (g_LogFile.is_open()) {
        g_LogFile << "\n[Logger] Session started at " << GetTimestamp() << "\n";
        g_LogFile << "[Logger] Log file: " << logPath << "\n";
        g_LogFile.flush();
    }
#ifdef SUNSTEAD_LOGGING
    // Always print absolute path so "where are the logs" is never a mystery.
    fprintf(stdout, "[Logger] Writing to: %s\n", logPath.c_str());
    fflush(stdout);
#endif

    // Marks Initialize done for BOTH configurations (release never allocates a
    // console but must not re-run the file-open on every Write either).
    g_ConsoleInitialized = true;
}

void Logger::Shutdown() {
    if (g_LogFile.is_open()) {
        g_LogFile << "[Logger] Session ended\n";
        g_LogFile.close();
    }
}

void Logger::Info(const std::string& message, const std::string& source) {
    Write(Level::Info, source, message);
}

void Logger::Warn(const std::string& message, const std::string& source) {
    Write(Level::Warning, source, message);
}

void Logger::Error(const std::string& message, const std::string& source) {
    Write(Level::Error, source, message);
}

void Logger::Write(Level level, const std::string& source, const std::string& message) {
#ifndef SUNSTEAD_LOGGING
    // Release: crash (Error) logs only — Info/Warning call sites are compiled
    // out by the LOG_* macros; this guards any leftover direct calls too.
    if (level != Level::Error) return;
#endif

    // Mutex so parallel signature scans (and anything else concurrent) don't
    // interleave console/file writes mid-line.
    std::lock_guard<std::mutex> lock(g_LogMutex);

    EnsureConsole();

    const std::string timestamp = GetTimestamp();
    const std::string levelLabel = GetLevelLabel(level);

    std::ostringstream line;
    line << "[" << timestamp << "] [" << source << "] [" << levelLabel << "] " << message;

    if (g_LogFile.is_open()) {
        g_LogFile << line.str() << "\n";
    }

#ifdef SUNSTEAD_LOGGING
    if (g_AnsiSupported) {
        const std::string levelColor = GetLevelColor(level);
        const std::string reset = GetResetColor();
        const std::string timeColor = "\033[38;2;255;175;55m";
        const std::string srcColor  = "\033[38;2;80;200;120m";
        const std::string msgColor  = "\033[38;2;230;230;230m";
        const std::string bracket   = "\033[38;2;128;128;128m";

        // Use printf for more reliable ANSI output
        fprintf(stdout, "%s[%s%s%s] %s[%s%s%s] %s[%s%s%s] %s%s%s\n",
            bracket.c_str(), timeColor.c_str(), timestamp.c_str(), bracket.c_str(),
            bracket.c_str(), srcColor.c_str(), source.c_str(), bracket.c_str(),
            bracket.c_str(), levelColor.c_str(), levelLabel.c_str(), bracket.c_str(),
            msgColor.c_str(), message.c_str(), reset.c_str());
        fflush(stdout);
    } else {
        fprintf(stdout, "%s\n", line.str().c_str());
        fflush(stdout);
    }
#endif
}

std::string Logger::GetTimestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &time);

    std::ostringstream oss;
    oss << std::put_time(&localTime, "%H:%M:%S");
    return oss.str();
}

std::string Logger::GetLevelLabel(Level level) {
    switch (level) {
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    default: return "INFO";
    }
}

std::string Logger::GetLevelColor(Level level) {
    switch (level) {
    case Level::Info: return "\033[38;2;80;220;100m";
    case Level::Warning: return "\033[38;2;255;200;50m";
    case Level::Error: return "\033[38;2;255;80;80m";
    default: return "\033[38;2;80;220;100m";
    }
}

std::string Logger::GetResetColor() {
    return "\033[0m";
}

std::string Logger::GetLogFilePath() {
    // Prefer host (Minecraft.exe) dir so logs live next to the process the user
    // is looking at. Also printed once at Initialize so the path is obvious.
    char buffer[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    std::string path(buffer);
    const size_t pos = path.find_last_of("\\/");
    if (pos != std::string::npos) {
        path = path.substr(0, pos + 1);
    } else {
        path.clear();
    }
    return path + kLogFileName;
}

void Logger::EnsureConsole() {
    if (!g_ConsoleInitialized) {
        Initialize();
    }
}