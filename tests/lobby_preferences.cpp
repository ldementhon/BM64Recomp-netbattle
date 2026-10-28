#include "../src/online/lobby_preferences.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace online::lobby;
namespace fs = std::filesystem;

int main() {
    auto scratch = fs::temp_directory_path() / ("bm64-lobby-preferences-" + std::to_string(std::random_device{}()));
    fs::create_directories(scratch);
    auto path = scratch / "online-lobby.json";
    assert(!load_lobby_preferences(path)); // First run keeps the screen's defaults.

    assert(save_lobby_preferences(path, {"203.0.113.7", "6464", "Louis"}));
    auto saved = load_lobby_preferences(path);
    assert(saved && saved->address == "203.0.113.7" && saved->port == "6464" && saved->name == "Louis");
    assert(save_lobby_preferences(path, {"lobby.example.net", "7000", "Renamed \xc3\xa9"}));
    saved = load_lobby_preferences(path);
    assert(saved && saved->address == "lobby.example.net" && saved->port == "7000" && saved->name == "Renamed \xc3\xa9");
    assert(!fs::exists(path.string() + ".tmp"));

    auto write = [&](const char* text) { std::ofstream(path, std::ios::binary | std::ios::trunc) << text; };
    for (const char* text : {"", "{", "[]", "\"text\""}) {
        write(text);
        assert(!load_lobby_preferences(path));
    }
    // Invalid fields are dropped individually; the valid ones are kept.
    write(R"({"address":"has space","port":"12a","name":""})");
    saved = load_lobby_preferences(path);
    assert(saved && saved->address.empty() && saved->port.empty() && saved->name.empty());
    write(R"({"address":7,"port":"123456","name":"a\u0001b"})");
    saved = load_lobby_preferences(path);
    assert(saved && saved->address.empty() && saved->port.empty() && saved->name.empty());
    write(R"({"address":"10.0.0.2","name":"123456789012345678901234567890123"})");
    saved = load_lobby_preferences(path);
    assert(saved && saved->address == "10.0.0.2" && saved->port.empty() && saved->name.empty());

    fs::remove_all(scratch);
    std::cout << "PASS: online lobby address, port and name are remembered; malformed files and fields are ignored\n";
}
