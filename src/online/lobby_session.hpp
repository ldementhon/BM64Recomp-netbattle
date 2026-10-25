#pragma once
#include "game_session.hpp"
#include "lobby_client.hpp"

namespace online::lobby {
// Service this owner regularly on the network thread, independently of rendering.
class LobbySession {
public:
    struct Launch {
        std::filesystem::path executable, rom, log;
        std::vector<std::string> arguments{"--checkpoint-test", "--single-battle", "--online-test"};
        std::map<std::string, std::string> diagnostics{{"BM64_NETWORK_INPUT", "1"}};
    };
    LobbySession(const std::string& address, uint16_t port, std::string name,
                 Fingerprint fingerprint, Launch launch)
        : connection_(address, port, std::move(name), fingerprint), launch_(std::move(launch)) {}

    const LobbyClient& connection() const { return connection_; }
    const GameSession& game() const { return game_; }
    const std::string& error() const { return error_.empty() ? connection_.error() : error_; }
    void ready(bool value, LocalInput input = {}) {
        if (input.port >= 4 || input.settings.size() > local_input_limit)
            throw std::invalid_argument("invalid local input settings");
        connection_.ready(value);
        input_ = std::move(input);
        error_.clear();
    }
    void start() { connection_.start(); }
    void cancel() {
        game_.cancel(match_);
        connection_.cancel();
        stopping_ = true;
    }

    void poll() {
        try {
            for (const auto& event : connection_.poll()) {
                if (event.op == Op::Prepare) {
                    match_ = event.match;
                    loaded_ = stopping_ = false;
                    error_.clear();
                    game_.start(match_, launch_.executable, launch_.rom, launch_.log,
                                launch_.arguments, launch_.diagnostics, event.count, connection_.slot(), input_);
                } else if (event.op == Op::End) {
                    game_.cancel(match_);
                    stopping_ = true;
                }
            }
            if (connection_.disconnected()) {
                game_.cancel(match_);
                return;
            }
            // End/late-start cancellation can arrive alongside obsolete input packets.
            auto relays = connection_.take_inputs();
            if (stopping_) return;
            if (auto exit = game_.poll()) {
                stopping_ = true;
                if (exit->completed_frame) connection_.finished(*exit->completed_frame);
                else {
                    error_ = exit->error;
                    connection_.cancel(Reason::LoadFailed);
                }
                return;
            }
            for (const auto& input : game_.take_inputs()) connection_.send_inputs(input);
            for (const auto& input : relays) game_.send_inputs(input);
            if (game_.ready() && !loaded_ && connection_.clock()) {
                const auto& ready = *game_.ready();
                connection_.loaded(ready.frame, ready.ram, ready.registers);
                loaded_ = true;
            }
            auto epoch = connection_.start_at();
            if (epoch && game_.state() == GameSession::State::Loading && monotonic_ms() >= *epoch) {
                if (monotonic_ms() - *epoch > 10) throw std::runtime_error("local match start missed its deadline");
                if (!game_.playing(match_)) throw std::runtime_error("game was not ready at match start");
            }
        } catch (const std::exception& e) {
            error_ = e.what();
            game_.cancel(match_);
            stopping_ = true;
            // A failed disposable game leaves the room connection available for retry.
            try { connection_.cancel(Reason::LoadFailed); } catch (...) {}
        }
    }
private:
    LobbyClient connection_;
    GameSession game_;
    Launch launch_;
    LocalInput input_;
    uint64_t match_ = 0;
    bool loaded_ = false, stopping_ = false;
    std::string error_;
};
}
