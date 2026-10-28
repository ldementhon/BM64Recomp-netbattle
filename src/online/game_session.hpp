#pragma once

// The lobby owns this object; a game process owns all runtime/checkpoint state.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#ifndef _WIN32
#include <sys/types.h>
#endif
#include "match_channel.hpp"
#include "local_input.hpp"

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
        bool left = false; // The player quit from the match window.
    };
    GameSession() = default;
    ~GameSession();
    GameSession(const GameSession&) = delete;
    GameSession& operator=(const GameSession&) = delete;

    void start(uint64_t match, const std::filesystem::path& executable,
               const std::filesystem::path& rom, const std::filesystem::path& log,
               const std::vector<std::string>& arguments,
               const std::map<std::string, std::string>& diagnostic_environment = {},
               unsigned human_players = 4, unsigned local_slot = 0,
               const LocalInput& local_input = {});
    bool playing(uint64_t match);
    bool cancel(uint64_t match);
    std::optional<Exit> poll(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    State state() const { return state_; }
    const std::optional<MatchMessage>& ready() const { return ready_; }
    const std::filesystem::path& settings() const { return settings_; }
    bool send_inputs(const InputBatch& batch);
    std::vector<InputBatch> take_inputs() { auto result = std::move(inputs_); inputs_.clear(); return result; }
    std::vector<MatchMessage> take_states() { auto result = std::move(states_); states_.clear(); return result; }
    bool state_checked(uint64_t frame, bool desync = false);
    // True once after P1 confirms a restart in the match window.
    bool take_restart() { return std::exchange(restart_, false); }

private:
    int reap();
    std::optional<int> poll_child();
    void cleanup();
#ifdef _WIN32
    static constexpr HANDLE no_child = nullptr;
    HANDLE child_ = no_child;
    HANDLE job_ = nullptr;
#else
    static constexpr pid_t no_child = -1;
    pid_t child_ = -1;
#endif
    uint64_t match_ = 0;
    State state_ = State::Lobby;
    std::filesystem::path settings_;
    std::chrono::steady_clock::time_point loading_deadline_;
    MatchChannel channel_;
    std::optional<MatchMessage> ready_;
    std::vector<InputBatch> inputs_;
    std::vector<MatchMessage> states_;
    std::optional<uint64_t> pending_state_;
    bool restart_ = false;
    unsigned humans_ = 4, local_slot_ = 0;
};
}
