#pragma once
#include "../src/online/lobby_protocol.hpp"
#include <charconv>
#include <istream>
#include <sstream>
#include <utility>

namespace online::lobby {
// Written by make_release.py:
//   release 1
//   macOS-arm64 <64 hex digits>
//   Windows-x86_64 <64 hex digits>
// Blank lines and lines starting with # are ignored.
struct ReleaseFile {
    uint16_t release = 0;
    std::vector<std::pair<std::string, Fingerprint>> builds;
};

inline Fingerprint parse_fingerprint(const std::string& text) {
    Fingerprint fingerprint{};
    if (text.size() != fingerprint.size() * 2) throw std::invalid_argument("fingerprint must be 64 hex digits");
    for (size_t i = 0; i < fingerprint.size(); ++i) {
        unsigned byte;
        auto* begin = text.data() + 2 * i;
        auto result = std::from_chars(begin, begin + 2, byte, 16);
        if (result.ec != std::errc{} || result.ptr != begin + 2) throw std::invalid_argument("invalid fingerprint hex");
        fingerprint[i] = uint8_t(byte);
    }
    return fingerprint;
}

inline ReleaseFile parse_release_file(std::istream& input) {
    ReleaseFile file;
    std::string line;
    for (unsigned number = 1; std::getline(input, line); ++number) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        auto fail = [&](const std::string& why) {
            throw std::invalid_argument("release file line " + std::to_string(number) + ": " + why);
        };
        std::istringstream fields(line);
        std::string key, value, extra;
        if (!(fields >> key >> value) || (fields >> extra)) fail("expected two fields");
        if (key == "release") {
            unsigned release = 0;
            auto parsed = std::from_chars(value.data(), value.data() + value.size(), release);
            if (file.release || !file.builds.empty()) fail("release must be the first entry, once");
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !release || release > 65535)
                fail("release must be a number from 1 to 65535");
            file.release = uint16_t(release);
            continue;
        }
        if (!file.release) fail("release must be the first entry");
        Fingerprint fingerprint;
        try { fingerprint = parse_fingerprint(value); } catch (const std::invalid_argument& error) { fail(error.what()); }
        for (const auto& build : file.builds)
            if (build.first == key || build.second == fingerprint) fail("duplicate build");
        if (file.builds.size() == 4) fail("at most 4 builds");
        file.builds.emplace_back(key, fingerprint);
    }
    if (!file.release || file.builds.empty()) throw std::invalid_argument("release file needs a release and at least one build");
    return file;
}
}
