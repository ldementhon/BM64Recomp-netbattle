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
#include "match_channel.hpp"

namespace online {
class GameSession {
public:
    enum class State { Lobby, Loading, Playing };
    struct Exit {
        uint64_t match;
        int status;
        bool loading_timeout;
        std::optional<uint64_t> completed_frame = {};
        std::string error;
    };
    GameSession() = default;
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    void start(uint64_t match, const std::filesystem::path& executable,
               const std::filesystem::path& rom, const std::filesystem::path& log,
               const std::vector<std::string>& arguments,
               const std::map<std::string, std::string>& diagnostic_environment = {},
               unsigned human_players = 4, unsigned local_slot = 0);
    bool playing(uint64_t match);
    bool cancel(uint64_t match);
    std::optional<Exit> poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    State state() const { return state_; }
    const std::optional<MatchMessage>& ready() const { return ready_; }
    const std::filesystem::path& settings() const { return settings_; }
    bool send_inputs(const InputBatch& batch);
    std::vector<InputBatch> take_inputs() { auto result = std::move(inputs_); inputs_.clear(); return result; }

private:
    int reap();
    void cleanup();
    pid_t child_ = -1;
    uint64_t match_ = 0;
    State state_ = State::Lobby;
    std::filesystem::path settings_;
    std::chrono::steady_clock::time_point loading_deadline_;
    MatchChannel channel_;
    std::optional<MatchMessage> ready_;
    std::vector<InputBatch> inputs_;
    unsigned humans_ = 4, local_slot_ = 0;
};
}
