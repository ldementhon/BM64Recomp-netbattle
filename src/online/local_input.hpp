#pragma once
#include <filesystem>
#include <string>
#include <stdexcept>

namespace online {
constexpr size_t local_input_limit = 64 * 1024;
struct LocalInput {
    unsigned port = 0;
    std::string settings;
};

inline void validate_match_folder(const std::filesystem::path& directory) {
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        const auto name = entry.path().filename();
        if (!std::filesystem::is_regular_file(entry.symlink_status()) ||
            (name != "bm64_us.z64" && name != "online-input.json") ||
            (name == "online-input.json" && (entry.file_size() == 0 || entry.file_size() > local_input_limit)))
            throw std::runtime_error("requires fresh settings containing only bm64_us.z64 and optional online-input.json");
    }
    if (!std::filesystem::is_regular_file(directory / "bm64_us.z64"))
        throw std::runtime_error("missing bm64_us.z64 in match settings");
}
}
