#pragma once
#include "lobby_protocol.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <CommonCrypto/CommonDigest.h>
#include <mach-o/dyld.h>
#endif
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>

namespace online::lobby {
inline std::filesystem::path current_executable() {
#ifdef _WIN32
    std::vector<wchar_t> path(256);
    for (;;) {
        auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
        if (!size) throw std::system_error(GetLastError(), std::system_category(), "locate game executable");
        if (size < path.size()) return std::filesystem::canonical(std::wstring(path.data(), size));
        if (path.size() >= 32768) throw std::runtime_error("game executable path is too long");
        path.resize(path.size() * 2);
    }
#else
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size)) throw std::runtime_error("cannot locate game executable");
    return std::filesystem::canonical(path.data());
#endif
}

namespace detail {
class Sha256 {
public:
    Sha256() {
#ifdef _WIN32
        check(BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, &context_, nullptr, 0, nullptr, 0, 0));
#else
        CC_SHA256_Init(&context_);
#endif
    }
    ~Sha256() {
#ifdef _WIN32
        BCryptDestroyHash(context_);
#endif
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;
    void update(const void* data, uint32_t size) {
#ifdef _WIN32
        check(BCryptHashData(context_, const_cast<PUCHAR>(static_cast<const UCHAR*>(data)), size, 0));
#else
        CC_SHA256_Update(&context_, data, size);
#endif
    }
    Fingerprint finish() {
        Fingerprint digest;
#ifdef _WIN32
        check(BCryptFinishHash(context_, digest.data(), ULONG(digest.size()), 0));
#else
        CC_SHA256_Final(digest.data(), &context_);
#endif
        return digest;
    }
private:
#ifdef _WIN32
    static void check(NTSTATUS status) {
        if (status < 0) throw std::runtime_error("SHA-256 failed: " + std::to_string(status));
    }
    BCRYPT_HASH_HANDLE context_ = nullptr;
#else
    CC_SHA256_CTX context_;
#endif
};
}

inline Fingerprint file_sha256(const std::filesystem::path& path) {
    const auto before_size = std::filesystem::file_size(path);
    const auto before_time = std::filesystem::last_write_time(path);
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("cannot read compatibility file: " + path.string());
    detail::Sha256 context;
    std::array<char, 64 * 1024> buffer;
    uint64_t read = 0;
    while (input.read(buffer.data(), buffer.size()) || input.gcount()) {
        read += uint64_t(input.gcount());
        context.update(buffer.data(), uint32_t(input.gcount()));
    }
    if (!input.eof() || input.bad() || read != before_size ||
        std::filesystem::file_size(path) != before_size || std::filesystem::last_write_time(path) != before_time)
        throw std::runtime_error("compatibility file changed or failed while reading: " + path.string());
    return context.finish();
}

inline Fingerprint client_fingerprint(const std::filesystem::path& executable, const std::filesystem::path& rom) {
    auto game_hash = file_sha256(executable), rom_hash = file_sha256(rom);
    // Domain separation includes the wire/profile version and native architecture.
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    constexpr std::string_view profile = "BM64 online v6; Windows x86_64; fixed Battle profile 1; delay 2";
#elif defined(__APPLE__) && defined(__aarch64__)
    constexpr std::string_view profile = "BM64 online v6; macOS arm64; fixed Battle profile 1; delay 2";
#elif defined(__APPLE__) && defined(__x86_64__)
    constexpr std::string_view profile = "BM64 online v6; macOS x86_64; fixed Battle profile 1; delay 2";
#else
#error Unsupported online client architecture
#endif
    detail::Sha256 context;
    context.update(profile.data(), uint32_t(profile.size()));
    context.update(game_hash.data(), uint32_t(game_hash.size()));
    context.update(rom_hash.data(), uint32_t(rom_hash.size()));
    return context.finish();
}

inline std::string fingerprint_hex(const Fingerprint& fingerprint) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (auto byte : fingerprint) { result += hex[byte >> 4]; result += hex[byte & 15]; }
    return result;
}
}
