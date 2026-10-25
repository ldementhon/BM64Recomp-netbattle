#pragma once
// shortcut: the playable owner currently targets macOS; add a tested Windows
// hashing/process backend before advertising cross-platform compatibility.
#include "lobby_protocol.hpp"
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace online::lobby {
inline std::filesystem::path current_executable() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size)) throw std::runtime_error("cannot locate game executable");
    return std::filesystem::canonical(path.data());
}

inline Fingerprint file_sha256(const std::filesystem::path& path) {
    const auto before_size = std::filesystem::file_size(path);
    const auto before_time = std::filesystem::last_write_time(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read compatibility file: " + path.string());
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    std::array<char, 64 * 1024> buffer;
    uint64_t read = 0;
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        read += uint64_t(input.gcount());
        CC_SHA256_Update(&context, buffer.data(), CC_LONG(input.gcount()));
    }
    if (!input.eof() || input.bad() || read != before_size ||
        std::filesystem::file_size(path) != before_size || std::filesystem::last_write_time(path) != before_time)
        throw std::runtime_error("compatibility file changed or failed while reading: " + path.string());
    Fingerprint digest; CC_SHA256_Final(digest.data(), &context); return digest;
}

inline Fingerprint client_fingerprint(const std::filesystem::path& executable, const std::filesystem::path& rom) {
    auto game_hash = file_sha256(executable), rom_hash = file_sha256(rom);
    // Domain separation includes the wire/profile version and native architecture.
#if defined(__aarch64__)
    constexpr std::string_view profile = "BM64 online v1; macOS arm64; fixed Battle profile 1; delay 2";
#elif defined(__x86_64__)
    constexpr std::string_view profile = "BM64 online v1; macOS x86_64; fixed Battle profile 1; delay 2";
#else
#error Unsupported online client architecture
#endif
    CC_SHA256_CTX context; CC_SHA256_Init(&context);
    CC_SHA256_Update(&context, profile.data(), CC_LONG(profile.size()));
    CC_SHA256_Update(&context, game_hash.data(), game_hash.size());
    CC_SHA256_Update(&context, rom_hash.data(), rom_hash.size());
    Fingerprint digest; CC_SHA256_Final(digest.data(), &context); return digest;
}

inline std::string fingerprint_hex(const Fingerprint& fingerprint) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : fingerprint) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
}
