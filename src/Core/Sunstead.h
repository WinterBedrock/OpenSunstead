#pragma once
#include <array>
#include <string_view>
#include <string>

class Sunstead {
public:
    static Sunstead& GetInstance() {
        static Sunstead instance;
        return instance;
    }

    void Initialize();
    void Shutdown();
    // raknet poll 26.4
    void UpdateServerPresence();

    bool IsRunning() const { return isRunning; }
    void Stop() { isRunning = false; }

private:
    Sunstead() = default;
    ~Sunstead() = default;
    Sunstead(const Sunstead&) = delete;
    Sunstead& operator=(const Sunstead&) = delete;

    bool isRunning = false;
public:
    enum class InjectionState { Scanning, Success, Finished };
    static inline InjectionState g_InjectionState = InjectionState::Scanning;
    static inline float g_InjectionSuccessTime = 0.0f;
    static inline unsigned long long g_InjectionStartMs = 0;
    static inline const char* ClientName = "Sunstead";
    static inline const char* ClientVersion = "v1.5";

    static inline const std::array<std::string_view, 1> supportedVersions = {
        "1.26.6x"
    };

    [[nodiscard]] static bool IsVersionSupported(const std::string& version) noexcept {
        if (version.empty()) return false;
        for (const auto& sv : supportedVersions) {
            std::string_view pat(sv);
            if (pat.empty()) continue;
            if (pat.back() != 'x') {
                if (std::string_view(version) == pat) return true;
                continue;
            }
            pat.remove_suffix(1);
            if (!pat.empty() && version.size() >= pat.size() &&
                std::string_view(version).substr(0, pat.size()) == pat) return true;
        }
        return false;
    }

    [[nodiscard]] static std::string GetSupportedVersionsString() noexcept {
        std::string result;
        for (size_t i = 0; i < supportedVersions.size(); ++i) {
            if (i > 0) result += ", ";
            result += supportedVersions[i];
        }
        return result;
    }
};