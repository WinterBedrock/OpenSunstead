#pragma once
#include <cstddef>

// Compile-time obfuscated string storage for signature patterns.
//
// The plaintext pattern never exists in .rdata: every byte is XORed at compile
// time (constant-evaluated, so the encrypted array is what the linker embeds)
// and decoded lazily on first use into a process-lifetime buffer. Plaintext
// therefore only exists in memory, and only after the scan starts.
//
// Build the DLL once through VMProtect (see tools/VMPROTECT.md) and the decode
// sites themselves land inside virtualized regions via the SUN_VMP_* markers.
namespace sigobf {

// Position-dependent key: byte i is XORed with (kXorKey ^ i), so identical
// characters stop producing runs of identical ciphertext.
constexpr unsigned char kXorKey = 0x5A;

template <size_t N>
struct ObfBytes {
    char data[N];
    constexpr ObfBytes(const char (&s)[N]) : data{} {
        for (size_t i = 0; i < N; ++i)
            data[i] = static_cast<char>(
                static_cast<unsigned char>(s[i]) ^
                static_cast<unsigned char>(kXorKey + static_cast<unsigned char>(i)));
    }
    void decode(char* out) const {
        for (size_t i = 0; i < N; ++i)
            out[i] = static_cast<char>(
                static_cast<unsigned char>(data[i]) ^
                static_cast<unsigned char>(kXorKey + static_cast<unsigned char>(i)));
    }
};

template <size_t N>
struct Holder {
    ObfBytes<N> enc;
    char        plain[N];
    bool        ready;
    constexpr explicit Holder(const char (&s)[N]) : enc(s), plain{}, ready(false) {}
    const char* get() {
        // First decode happens inside SigManager::buildTable (single-threaded);
        // later re-entries (alt patterns) decode identical bytes into the same
        // buffer, so a torn read can only ever see the correct plaintext.
        if (!ready) { enc.decode(plain); ready = true; }
        return plain;
    }
};

} // namespace sigobf

// Materialises an obfuscated string literal. Each expansion site gets its own
// Holder thanks to being a function-local static inside its own lambda.
#define SUNSIG(s) ([]() -> const char* { \
    static sigobf::Holder<sizeof(s)> sun_sig_holder(s); \
    return sun_sig_holder.get(); }())
