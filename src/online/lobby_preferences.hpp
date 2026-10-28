#pragma once
#include "json/json.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace online::lobby {
// The server and name last used to connect from the Online Battle screen.
struct LobbyPreferences {
    std::string address, port, name;
};

namespace detail {
inline bool printable(const std::string& text, size_t limit, bool spaces) {
    if (text.empty() || text.size() > limit) return false;
    for (unsigned char c : text)
        if (c < 32 || c == 127 || (!spaces && c == ' ')) return false;
    return true;
}
inline std::optional<std::string> field(const nlohmann::json& json, const char* key) {
    auto found = json.find(key);
    if (found == json.end() || !found->is_string()) return std::nullopt;
    return found->get<std::string>();
}
}

// Fields that are missing or invalid stay empty, so the screen keeps its defaults.
inline std::optional<LobbyPreferences> load_lobby_preferences(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    auto json = nlohmann::json::parse(input, nullptr, false);
    if (json.is_discarded() || !json.is_object()) return std::nullopt;
    LobbyPreferences preferences;
    if (auto address = detail::field(json, "address"); address && detail::printable(*address, 253, false))
        preferences.address = *address;
    if (auto port = detail::field(json, "port"); port && port->size() <= 5 &&
        !port->empty() && port->find_first_not_of("0123456789") == std::string::npos)
        preferences.port = *port;
    if (auto name = detail::field(json, "name"); name && detail::printable(*name, 32, true))
        preferences.name = *name;
    return preferences;
}

inline bool save_lobby_preferences(const std::filesystem::path& path, const LobbyPreferences& preferences) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << nlohmann::json{{"address", preferences.address}, {"port", preferences.port},
                                 {"name", preferences.name}}.dump(4);
        if (!output.flush()) return false;
    }
    std::error_code error;
    std::filesystem::rename(temporary, path, error);
    return !error;
}
}
