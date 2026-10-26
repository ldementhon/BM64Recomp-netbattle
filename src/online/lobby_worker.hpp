#pragma once
#include "lobby_session.hpp"
#include "fingerprint.hpp"
#include <deque>
#include <mutex>
#include <thread>

namespace online::lobby {
class LobbyWorker {
public:
    enum class Action { Connect, Ready, Start, Cancel, Leave };
    struct Command {
        Action action;
        std::string address{}, name{};
        uint16_t port = 6464;
        bool ready = false;
        LocalInput input{};
    };
    struct View {
        bool connected = false, joined = false, ready = false, can_start = false, in_match = false;
        uint64_t match = 0;
        std::string status = "Enter a server address to join.", fingerprint, latency;
        std::array<std::string, 4> seats{"P1 — CPU", "P2 — CPU", "P3 — CPU", "P4 — CPU"};
        bool operator==(const View&) const = default;
    };
    explicit LobbyWorker(LobbySession::Launch launch)
        : thread_([this, launch = std::move(launch)](std::stop_token stop) mutable { run(stop, std::move(launch)); }) {}
    ~LobbyWorker() { thread_.request_stop(); thread_.join(); }
    void send(Command command) {
        std::lock_guard lock(mutex_);
        if (commands_.size() >= 16) throw std::runtime_error("Please wait for the current lobby action.");
        commands_.push_back(std::move(command));
    }
    View view() const { std::lock_guard lock(mutex_); return view_; }
private:
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    View view_;
    std::jthread thread_;
    void publish(const View& view) { std::lock_guard lock(mutex_); view_ = view; }
    void run(std::stop_token stop, LobbySession::Launch launch) {
        if (enet_initialize() != 0) { View view; view.status = "Network initialization failed."; publish(view); return; }
        // Balance initialization, including Windows resources, after all sockets close.
        {
            std::unique_ptr<LobbySession> session;
            View view;
            std::string command_error;
            while (!stop.stop_requested()) {
                std::deque<Command> commands;
                { std::lock_guard lock(mutex_); commands.swap(commands_); }
                for (auto& command : commands) {
                    try {
                        command_error.clear();
                        if (command.action == Action::Leave) {
                            session.reset(); view = {}; view.status = "Left the lobby.";
                        } else if (command.action == Action::Connect) {
                            if (session) throw std::runtime_error("Leave the current lobby before connecting again.");
                            if (command.address.empty() || command.address.size() > 253 ||
                                command.address.find('\0') != std::string::npos || !valid_name(command.name) || !command.port)
                                throw std::invalid_argument("Enter a server address, valid port, and a name of up to 32 bytes.");
                            view = {}; view.connected = true; view.status = "Checking this build…"; publish(view);
                            auto fingerprint = client_fingerprint(launch.executable, launch.rom);
                            launch.verify_compatibility = [executable = launch.executable, rom = launch.rom, fingerprint] {
                                if (client_fingerprint(executable, rom) != fingerprint)
                                    throw std::runtime_error("The game or ROM changed. Leave and reconnect before playing.");
                            };
                            view.fingerprint = fingerprint_hex(fingerprint);
                            view.status = "Connecting…"; publish(view);
                            session = std::make_unique<LobbySession>(command.address, command.port, command.name, fingerprint, launch);
                        } else if (session) {
                            if (command.action == Action::Ready) session->ready(command.ready, std::move(command.input));
                            else if (command.action == Action::Start) session->start();
                            else if (command.action == Action::Cancel) session->cancel();
                        }
                    } catch (const std::exception& error) {
                        command_error = error.what();
                        view.status = command_error;
                        if (!session) view.connected = false;
                    }
                }
                if (session) {
                    session->poll();
                    const auto& connection = session->connection();
                    if (connection.disconnected()) {
                        auto error = session->error(); session.reset(); view = {}; view.status = error;
                    } else if (connection.joined()) {
                        const auto& roster = connection.roster();
                        view.connected = view.joined = true;
                        view.in_match = roster.phase != Phase::Lobby;
                        view.match = roster.match;
                        view.ready = roster.players[connection.slot()].ready;
                        bool all_ready = roster.count >= 2;
                        for (unsigned i = 0; i < 4; ++i) {
                            view.seats[i] = "P" + std::to_string(i + 1) + " — ";
                            if (i >= roster.count) view.seats[i] += "CPU";
                            else {
                                view.seats[i] += roster.players[i].name;
                                if (i == connection.slot()) view.seats[i] += " (you)";
                                view.seats[i] += roster.players[i].ready ? " · Ready" : " · Not ready";
                                all_ready &= roster.players[i].ready;
                            }
                        }
                        view.can_start = connection.slot() == 0 && all_ready && !view.in_match;
                        auto clock = connection.clock();
                        view.latency = clock ? "Server round trip: " + std::to_string(clock->round_trip) + " ms" : "Measuring server round trip…";
                        if (!session->error().empty()) view.status = session->error();
                        else if (roster.phase == Phase::Loading) view.status = "Loading the battle…";
                        else if (roster.phase == Phase::Playing) view.status = "Battle running in the game window.";
                        else view.status = "Ready up. P1 starts when everyone is ready.";
                    }
                }
                if (!command_error.empty()) view.status = command_error;
                publish(view);
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        enet_deinitialize();
    }
};
}
