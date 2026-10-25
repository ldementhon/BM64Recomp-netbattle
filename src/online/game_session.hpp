#pragma once

// The lobby owns this object; a game process owns all runtime/checkpoint state.
// shortcut: POSIX clients only; add a Windows process backend when that build is tested.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>
#include <sys/types.h>

namespace online {
class GameSession {
public:
    enum class State { Lobby, Loading, Playing };
    struct Exit { uint64_t match; int status; bool loading_timeout; };
    GameSession() = default;
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    void start(uint64_t match, const std::filesystem::path& executable,
               const std::filesystem::path& rom, const std::filesystem::path& log,
               const std::vector<std::string>& arguments,
               const std::map<std::string, std::string>& diagnostic_environment = {});
    bool playing(uint64_t match);
    bool cancel(uint64_t match);
    std::optional<Exit> poll();
    State state() const { return state_; }
    const std::filesystem::path& settings() const { return settings_; }

private:
    void reap();
    void cleanup();
    pid_t child_ = -1;
    uint64_t match_ = 0;
    State state_ = State::Lobby;
    std::filesystem::path settings_;
    std::chrono::steady_clock::time_point loading_deadline_;
};
}
