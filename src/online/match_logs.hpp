#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace online {
// Separate files preserve a failed attempt when the player retries or reconnects.
inline std::filesystem::path new_match_log(const std::filesystem::path& base, uint64_t match) {
    namespace fs = std::filesystem;
    static const auto session = std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) +
                                "-" + std::to_string(std::random_device{}());
    auto directory = base.parent_path() / (base.stem().string() + "-logs");
    fs::create_directories(directory);
    // Include a fresh suffix because reconnecting can restart server match IDs.
    auto path = directory / (session + "-" + std::to_string(match) + "-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".log");
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("Cannot create match log");
    output.close();
    std::vector<fs::directory_entry> previous;
    for (const auto& entry : fs::directory_iterator(directory))
        if (fs::is_regular_file(entry.symlink_status()) && entry.path().extension() == ".log" && entry.path() != path)
            previous.push_back(entry);
    std::sort(previous.begin(), previous.end(), [](const auto& a, const auto& b) {
        return a.last_write_time() < b.last_write_time();
    });
    // Keep this attempt and the nine most recent previous attempts.
    for (size_t i = 0; i + 9 < previous.size(); ++i) {
        std::error_code ignored;
        fs::remove(previous[i].path(), ignored);
    }
    return path;
}
inline std::string match_log_location(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return " Match log: " + std::string(text.begin(), text.end());
}
}
