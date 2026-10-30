#include "MemoryScanner.h"
#include "../Core/Logger.h"
#include "../Security/VMP.h"

#include <libhat.hpp>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

void MemoryScanner::Initialize() {
    const auto mod = hat::process::get_process_module();
    baseAddress = mod.address();
    moduleSize = mod.get_module_data().size();
}

bool MemoryScanner::EnsureInitialized() {
    if (baseAddress != 0 && moduleSize != 0) {
        return true;
    }

    Initialize();
    return baseAddress != 0 && moduleSize != 0;
}

uintptr_t MemoryScanner::FindSignature(const std::string& signature) {
    return FindSignature(signature, signature);
}

uintptr_t MemoryScanner::FindSignature(const std::string& name, const std::string& signature) {
    // Mutation (lighter than virtualization): the scanner itself is not secret,
    // but the name/pattern strings it compares against are the decoded
    // obfuscated literals from SigObfuscate.h, so the compare loop that touches
    // them is kept out of plain x86 analysis.
    SUN_VMP_BEGIN_MUTATION("MemoryScanner.FindSignature");
    // Every named scan counts toward the success/total tally that SigManager
    // prints at the end of Initialize(). Only the (name, pattern) overload is
    // counted - the bare-pattern overload forwards into it, so each signature
    // registered in SigManager is counted exactly once.
    mTotalSignatures.fetch_add(1, std::memory_order_relaxed);

    if (!EnsureInitialized()) {
        if (IsVerbose()) {
            LOG_ERROR("Failed to find signature '" + name + "' (scanner not initialized)", "MemoryScanner");
        }
        return 0;
    }

    auto parsed = hat::parse_signature(signature);
    if (!parsed.has_value() || parsed.value().empty()) {
        if (IsVerbose()) {
            LOG_ERROR("Failed to find signature '" + name + "' (failed to parse pattern)", "MemoryScanner");
        }
        return 0;
    }

    const hat::signature& sig = parsed.value();

    const auto mod = hat::process::get_process_module();
    hat::scan_result result = hat::find_pattern(
        sig,
        ".text",
        mod,
        hat::scan_alignment::X1,
        hat::scan_hint::x86_64
    );

    // Fallback: some images put code outside a named .text section (or the
    // section name is stripped). Scan the full module image as a last resort.
    if (!result.has_result()) {
        auto data = mod.get_module_data();
        if (!data.empty()) {
            result = hat::find_pattern(
                data.begin(),
                data.end(),
                sig,
                hat::scan_alignment::X1,
                hat::scan_hint::x86_64
            );
        }
    }

    if (result.has_result()) {
        const uintptr_t addr = reinterpret_cast<uintptr_t>(result.get());
        mFoundSignatures.fetch_add(1, std::memory_order_relaxed);
        if (IsVerbose()) {
            const uintptr_t offset = addr - baseAddress;
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "Found signature '%s' @ 0x%llX (offset: 0x%llX)",
                name.c_str(),
                static_cast<unsigned long long>(addr),
                static_cast<unsigned long long>(offset));
            LOG_INFO(buf, "MemoryScanner");
        }
        return addr;
    }

    if (IsVerbose()) {
        LOG_WARN("Failed to find signature '" + name + "' (no match found)", "MemoryScanner");
    }
    return 0;
    SUN_VMP_END();
}

int MemoryScanner::ScanAllChunked(const std::vector<std::string>& names,
                                  const std::vector<std::string>& patterns,
                                  std::vector<uintptr_t>& out) {
    if (!EnsureInitialized()) return -1;
    const size_t n = patterns.size();
    if (n == 0 || names.size() != n || out.size() < n) return -1;

    // Parse every pattern ONCE (the old path re-parsed per scan call).
    std::vector<hat::signature> sigs;
    sigs.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        auto parsed = hat::parse_signature(patterns[i]);
        if (parsed.has_value() && !parsed.value().empty()) {
            sigs.push_back(parsed.value());
        } else {
            sigs.push_back(hat::signature{});   // unusable row: never matches
        }
    }

    // Scan range: the .text section (identical to FindSignature's primary
    // path), whole module image as the fallback.
    const auto mod = hat::process::get_process_module();
    auto section = mod.get_section_data(".text");
    std::byte* begin = nullptr;
    std::byte* end = nullptr;
    if (!section.empty()) {
        begin = section.data();
        end = begin + section.size();
    } else {
        auto data = mod.get_module_data();
        begin = data.data();
        end = begin + data.size();
    }
    if (!begin || end <= begin) return -1;

    // Overlap scan windows, but count a match only in the chunk owning its start.
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned workers = hw == 0 ? 4u : (hw > 16u ? 16u : hw);
    const size_t chunkCount = (size_t)workers * 4;
    const size_t total = (size_t)(end - begin);
    const size_t chunkSize = (total + chunkCount - 1) / chunkCount;

    std::atomic<size_t> nextChunk{ 0 };
    std::vector<std::atomic<uintptr_t>> hits(n);
    std::vector<std::atomic<unsigned>> counts(n);
    for (size_t i = 0; i < n; ++i) {
        hits[i].store(0, std::memory_order_relaxed);
        counts[i].store(0, std::memory_order_relaxed);
    }

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= chunkCount) break;
                const size_t chunkOffset = chunk * chunkSize;
                if (chunkOffset >= total) break;
                std::byte* cb = begin + chunkOffset;
                const size_t ownedSize = chunkSize < total - chunkOffset ? chunkSize : total - chunkOffset;
                std::byte* ce = cb + ownedSize;
                for (size_t i = 0; i < n; ++i) {
                    if (out[i] != 0 || sigs[i].empty()) continue;
                    if (counts[i].load(std::memory_order_relaxed) > 1) continue;
                    const size_t available = static_cast<size_t>(end - ce);
                    const size_t overlap = sigs[i].size() - 1;
                    std::byte* scanEnd = ce + (overlap < available ? overlap : available);
                    std::byte* cursor = cb;
                    while (cursor < ce) {
                        auto result = hat::find_pattern(
                            cursor, scanEnd, sigs[i],
                            hat::scan_alignment::X1,
                            hat::scan_hint::x86_64
                        );
                        if (!result.has_result()) break;
                        auto* match = result.get();
                        if (match >= ce) break;
                        hits[i].store(reinterpret_cast<uintptr_t>(match), std::memory_order_relaxed);
                        if (counts[i].fetch_add(1, std::memory_order_relaxed) >= 1) break;
                        cursor = match + 1;
                    }
                }
            }
        });
    }
    for (auto& t : pool) t.join();

    int resolved = 0;
    for (size_t i = 0; i < n; ++i) {
        if (out[i] == 0) {
            const unsigned count = counts[i].load(std::memory_order_relaxed);
            if (count == 1) out[i] = hits[i].load(std::memory_order_relaxed);
            else if (count > 1)
                LOG_WARN("Rejected ambiguous signature '" + names[i] + "' (multiple matches)", "MemoryScanner");
        }
        if (out[i] != 0) ++resolved;
    }
    return resolved;
}

