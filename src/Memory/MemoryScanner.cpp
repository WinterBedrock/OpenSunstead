#include "MemoryScanner.h"
#include "../Core/Logger.h"

#include <libhat/scanner.hpp>
#include <libhat/process.hpp>
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

void MemoryScanner::Initialize() {
    const auto mod = hat::process::get_process_module();
    baseAddress = mod.address();
    moduleSize = mod.get_module_data().size();
}

bool MemoryScanner::EnsureInitialized() {
    if (baseAddress != 0 && moduleSize != 0) return true;
    Initialize();
    return baseAddress != 0 && moduleSize != 0;
}

uintptr_t MemoryScanner::FindSignature(const std::string& signature) {
    return FindSignature(signature, signature);
}

uintptr_t MemoryScanner::FindSignature(const std::string& name, const std::string& signature) {
    mTotalSignatures.fetch_add(1, std::memory_order_relaxed);
    if (!EnsureInitialized()) return 0;

    auto parsed = hat::parse_signature(signature);
    if (!parsed.has_value() || parsed.value().empty()) return 0;

    const auto mod = hat::process::get_process_module();
    auto section = mod.get_section_data(".text");
    if (section.empty()) section = mod.get_module_data();

    const auto result = hat::find_pattern(parsed.value(), ".text", mod,
        hat::scan_alignment::X1, (hat::scan_hint::x86_64 | hat::scan_hint::pair0));
    if (!result.has_result()) return 0;

    const uintptr_t address = reinterpret_cast<uintptr_t>(result.get());
    const auto remainder = std::span<const std::byte>(result.get() + 1,
            section.data() + section.size());
    if (hat::find_pattern(remainder.begin(), remainder.end(), parsed.value(),
                          hat::scan_alignment::X1, (hat::scan_hint::x86_64 | hat::scan_hint::pair0)).has_result()) {
        LOG_WARN("Rejected ambiguous signature '" + name + "'", "MemoryScanner");
        return 0;
    }

    mFoundSignatures.fetch_add(1, std::memory_order_relaxed);
    return address;
}

int MemoryScanner::ScanAllChunked(const std::vector<std::string>& names,
                                  const std::vector<std::string>& patterns,
                                  std::vector<uintptr_t>& out,
                                  std::vector<std::vector<uintptr_t>>* matchesOut) {
    if (!EnsureInitialized()) return -1;
    const size_t n = patterns.size();
    if (n == 0 || names.size() != n || out.size() < n) return -1;

    std::vector<hat::signature> sigs;
    sigs.reserve(n);
    for (const auto& pattern : patterns) {
        auto parsed = hat::parse_signature(pattern);
        sigs.push_back(parsed.has_value() && !parsed.value().empty()
                    ? std::move(parsed.value()) : hat::signature{});
    }

    const auto mod = hat::process::get_process_module();
    auto section = mod.get_section_data(".text");
    if (section.empty()) section = mod.get_module_data();
    if (section.empty()) return -1;

    std::byte* const begin = section.data();
    std::byte* const end = begin + section.size();
    const size_t total = section.size();
    const unsigned hw = std::thread::hardware_concurrency();
    const unsigned workers = std::max(1u, std::min(hw == 0 ? 4u : hw, 8u));
    const size_t chunkCount = static_cast<size_t>(workers) * 4;
    const size_t chunkSize = (total + chunkCount - 1) / chunkCount;

    std::vector<std::vector<uintptr_t>> matches(n);
    std::vector<std::mutex> matchLocks(n);
    std::vector<std::atomic<size_t>> matchCounts(n);
    for (auto& count : matchCounts) count.store(0, std::memory_order_relaxed);
    std::atomic<size_t> nextChunk{ 0 };
    std::atomic<size_t> totalFound{ 0 };
    constexpr size_t kMatchLimit = 32;

    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (unsigned w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
            for (;;) {
                const size_t chunk = nextChunk.fetch_add(1, std::memory_order_relaxed);
                if (chunk >= chunkCount) break;
                const size_t chunkOffset = chunk * chunkSize;
                if (chunkOffset >= total) break;
                std::byte* const chunkBegin = begin + chunkOffset;
                std::byte* const chunkEnd = chunkBegin + std::min(chunkSize, total - chunkOffset);

                for (size_t i = 0; i < n; ++i) {
                    if (sigs[i].empty() || matchCounts[i].load(std::memory_order_relaxed) >= kMatchLimit) continue;
                    const size_t overlap = std::min(sigs[i].size() - 1,
                                                    static_cast<size_t>(end - chunkEnd));
                    std::byte* const scanEnd = chunkEnd + overlap;
                    std::byte* cursor = chunkBegin;
                    while (cursor < chunkEnd && matchCounts[i].load(std::memory_order_relaxed) < kMatchLimit) {
                        const auto result = hat::find_pattern(cursor, scanEnd, sigs[i],
                            hat::scan_alignment::X1, (hat::scan_hint::x86_64 | hat::scan_hint::pair0));
                        if (!result.has_result() || result.get() >= chunkEnd) break;

                        std::byte* const hit = result.get();
                        {
                            std::lock_guard lock(matchLocks[i]);
                            matches[i].push_back(reinterpret_cast<uintptr_t>(hit));
                            matchCounts[i].fetch_add(1, std::memory_order_relaxed);
                        }
                        totalFound.fetch_add(1, std::memory_order_relaxed);
                        cursor = hit + 1;
                    }
                }
            }
        });
    }
    for (auto& thread : pool) thread.join();

    if (matchesOut) *matchesOut = matches;
    mTotalSignatures.fetch_add(static_cast<int>(n), std::memory_order_relaxed);

    int resolved = 0;
    for (size_t i = 0; i < n; ++i) {
        auto& hits = matches[i];
        std::sort(hits.begin(), hits.end());
        hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
        out[i] = hits.size() == 1 ? hits.front() : 0;
        if (out[i] != 0) {
            ++resolved;
        } else if (hits.size() > 1) {
            LOG_WARN("Signature '" + names[i] + "' requires contextual disambiguation", "MemoryScanner");
        }
    }
    mFoundSignatures.fetch_add(resolved, std::memory_order_relaxed);
    return resolved;
}
