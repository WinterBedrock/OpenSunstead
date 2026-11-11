#pragma once
#include <Windows.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

class MemoryScanner {
public:
    static MemoryScanner& GetInstance() {
        static MemoryScanner instance;
        return instance;
    }

    void Initialize();
    uintptr_t FindSignature(const std::string& signature);
    uintptr_t FindSignature(const std::string& name, const std::string& signature);

    int ScanAllChunked(const std::vector<std::string>& names,
                       const std::vector<std::string>& patterns,
                       std::vector<uintptr_t>& out,
                       std::vector<std::vector<uintptr_t>>* matchesOut = nullptr);

    int GetTotalSignatures() const { return mTotalSignatures.load(std::memory_order_relaxed); }
    int GetFoundSignatures() const { return mFoundSignatures.load(std::memory_order_relaxed); }

    void SetVerbose(bool v) { mVerbose.store(v, std::memory_order_relaxed); }
    bool IsVerbose() const { return mVerbose.load(std::memory_order_relaxed); }

    uintptr_t GetBaseAddress() const { return baseAddress; }
    size_t GetModuleSize() const { return moduleSize; }

private:
    MemoryScanner() = default;

    std::atomic<int> mTotalSignatures{0};
    std::atomic<int> mFoundSignatures{0};
    std::atomic<bool> mVerbose{true};

    bool EnsureInitialized();

    uintptr_t baseAddress = 0;
    size_t moduleSize = 0;
};
