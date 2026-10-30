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

    // ONE-PASS multi-pattern scan: partitions the module's code section into
    // chunks and scans every not-yet-resolved pattern inside each chunk, so
    // each byte of the section is read exactly ONCE per scan instead of once
    // PER PATTERN (the per-pattern API re-walks the whole ~230MB .text for
    // each of the ~95 table rows — ~22GB of memory traffic at init; this
    // path cuts that by the pattern count and also drops the redundant
    // libhat setup per call).
    //
    // `names`/`patterns` are parallel arrays (patterns[i] may be nullptr /
    // empty to skip). `out` receives the found address (0 = not found) for
    // each row; already non-zero entries are treated as pre-resolved and
    // skipped. Returns the index of a found row, or -1 when the whole
    // section has been walked.
    int ScanAllChunked(const std::vector<std::string>& names,
                       const std::vector<std::string>& patterns,
                       std::vector<uintptr_t>& out);

    // Signature scan statistics — SigManager prints a success/total summary
    // at the end of Initialize() so dead/shifted patterns are immediately
    // visible in the log.
    int GetTotalSignatures() const { return mTotalSignatures.load(std::memory_order_relaxed); }
    int GetFoundSignatures() const { return mFoundSignatures.load(std::memory_order_relaxed); }

    // When true, FindSignature emits per-sig Info/Warn lines. Disable during
    // parallel bulk scans (SigManager) so Logger is not hammered from many
    // threads; the final summary still reports totals.
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
