//todo remove this shit
#pragma once
#include <cstddef>

namespace sigobf {

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
        if (!ready) { enc.decode(plain); ready = true; }
        return plain;
    }
};

} // namespace sigobf

#define SUNSIG(s) ([]() -> const char* { \
    static sigobf::Holder<sizeof(s)> sun_sig_holder(s); \
    return sun_sig_holder.get(); }())
