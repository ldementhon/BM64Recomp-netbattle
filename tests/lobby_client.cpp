#include "../server/server.hpp"
#include "../src/online/lobby_client.hpp"
#include "../src/online/game_session.hpp"
#include "../src/online/lobby_session.hpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

using namespace online::lobby;
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--crash-child") return 1;
    if (argc == 2 && std::string(argv[1]) == "--leave-child") {
        // The player confirms the match window's leave prompt once play starts.
        auto channel = online::MatchChannel::inherited();
        auto match = std::stoull(std::getenv("BM64_OWNER_MATCH"));
        channel.send({online::match_channel_version, online::MatchEvent::Ready, match, 1374, 123, 456});
        for (;;) {
            if (auto command = channel.receive(); command && command->event == online::MatchEvent::Start) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        channel.send({online::match_channel_version, online::MatchEvent::Left, match, 1380});
        while (!channel.flush()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return 0;
    }
    if (argc == 2 && (std::string(argv[1]) == "--child" || std::string(argv[1]) == "--desync-child")) {
#ifndef _WIN32
        // ENet itself does not set CLOEXEC. Verify the owner helper does.
        for (int fd = 4; fd < 1024; ++fd) {
            int type; socklen_t size = sizeof(type);
            assert(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_DGRAM);
        }
#endif
        auto channel = online::MatchChannel::inherited();
        auto match = std::stoull(std::getenv("BM64_OWNER_MATCH"));
        channel.send({online::match_channel_version, online::MatchEvent::Ready, match, 1374, 123, 456});
        for (;;) {
            if (auto command = channel.receive()) {
                assert(command->event == online::MatchEvent::Start && command->match == match);
                online::InputBatch input; input.match = match; input.count = 1;
                input.slot = uint8_t(std::stoul(std::getenv("BM64_ONLINE_SLOT")));
                input.pads[0].buttons = input.slot ? 0x4000 : 0x8000;
                input.received = {2, 2, 0, 0}; input.received[input.slot] = 3;
                channel.send(online::input_message(input, false));
                for (;;) {
                    if (auto received = channel.receive()) {
                        assert(received->event == online::MatchEvent::InputRelay);
                        if (received->input.slot != input.slot) {
                            assert(received->input.pads[0].buttons == (input.slot ? 0x8000 : 0x4000));
                            break;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                channel.send({online::match_channel_version, online::MatchEvent::State, match, 1376,
                              std::string(argv[1]) == "--desync-child" ? 999u : 123u, 456});
                for (;;) {
                    auto checked = channel.receive();
                    if (checked && checked->event == online::MatchEvent::StateChecked) {
                        assert(checked->frame == 1376);
                        break;
                    }
                    // A desync keeps the game running; P1's window asks for a restart.
                    if (checked && checked->event == online::MatchEvent::Desync && input.slot == 0)
                        channel.send({online::match_channel_version, online::MatchEvent::Restart, match, 1377});
                    channel.flush();
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                channel.send({online::match_channel_version, online::MatchEvent::Finished, match, 1376});
                return 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    namespace fs = std::filesystem;
    const bool real = argc == 4;
    assert(argc == 1 || real);
    auto scratch = fs::absolute("build/experiments/lobby-client"); fs::create_directories(scratch);
    auto executable = fs::absolute(real ? argv[1] : argv[0]);
    auto rom = real ? fs::absolute(argv[2]) : scratch / "fixture.rom";
    if (!real) std::ofstream(rom) << "fixture";
    assert(enet_initialize() == 0);
    {
        ENetAddress address{}; assert(enet_address_set_host(&address, "127.0.0.1") == 0);
        Fingerprint fingerprint{42};
        Server server(address, fingerprint);
        LobbyClient a("127.0.0.1", uint16_t(server.port()), "Alice", fingerprint);
        LobbyClient b("127.0.0.1", uint16_t(server.port()), "Bob", fingerprint);
        std::array<online::GameSession, 2> games;
        auto until = [&](const std::function<bool()>& done) {
            auto deadline = monotonic_ms() + (real ? 35000 : 7000);
            do {
                server.service(); a.poll(); b.poll();
                assert(!a.disconnected() && !b.disconnected());
                if (done()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (monotonic_ms() < deadline);
            throw std::runtime_error("client test deadline exceeded");
        };
        until([&] { return a.roster().count == 2 && b.roster().count == 2 && a.clock() && b.clock(); });
        auto a_id = a.roster().identity, b_id = b.roster().identity;
        auto& p1 = a.roster().players[0].id == a_id ? a : b;
        std::optional<online::MatchMessage> initial;
        auto game_log = [&](unsigned i) {
            return scratch / ("match-" + std::to_string(a.roster().match) + "-player-" + std::to_string(i) + ".log");
        };
        auto prepare = [&] {
            a.ready(true); b.ready(true);
            until([&] { return p1.roster().players[0].ready && p1.roster().players[1].ready; });
            p1.start();
            until([&] { return a.roster().phase == Phase::Loading && b.roster().phase == Phase::Loading; });
            assert(a.error().empty() && b.error().empty());
            for (unsigned i = 0; i < games.size(); ++i) {
                games[i].start(a.roster().match, executable, rom, game_log(i),
                    real ? std::vector<std::string>{"--checkpoint-test", "--single-battle", "--online-test"} :
                           std::vector<std::string>{"--child"},
                    real ? std::map<std::string, std::string>{{"BM64_INPUT_SCRIPT", fs::absolute(argv[3]).string()},
                                                             {"BM64_STOP_FRAME", "5600"}} :
                           std::map<std::string, std::string>{}, a.roster().count, i == 0 ? a.slot() : b.slot());
            }
            until([&] {
                for (auto& game : games) {
                    auto exited = game.poll();
                    if (exited) throw std::runtime_error("game exited before readiness: " + exited->error);
                }
                return games[0].ready() && games[1].ready() && a.clock() && b.clock();
            });
            auto first = *games[0].ready(), second = *games[1].ready();
            assert(first.frame == second.frame && first.ram == second.ram && first.registers == second.registers);
            if (initial) assert(first.frame == initial->frame && first.ram == initial->ram && first.registers == initial->registers);
            initial = first;
            a.loaded(first.frame, first.ram, first.registers); b.loaded(second.frame, second.ram, second.registers);
            // Flush acknowledgments without servicing the server yet.
            a.poll(); b.poll();
        };
        for (unsigned match = 1; match <= 2; ++match) {
            prepare();
            until([&] { return a.start_at() && b.start_at(); });
            assert(a.roster().match == match && b.roster().match == match);
            auto uncertainty = (a.clock()->round_trip + b.clock()->round_trip + 1) / 2 + 2;
            assert(*a.start_at() <= *b.start_at() + uncertainty && *b.start_at() <= *a.start_at() + uncertainty);
            until([&] { return monotonic_ms() >= *a.start_at() && monotonic_ms() >= *b.start_at(); });
            for (auto& game : games) assert(game.playing(a.roster().match));
            if (real) {
                // This fixture certifies startup/cancel with two CPU seats. The
                // existing four-human fixture separately proves normal completion.
                auto started = monotonic_ms();
                until([&] { return monotonic_ms() - started >= 100; });
                a.cancel();
            } else {
                std::array<std::optional<online::GameSession::Exit>, 2> exits;
                std::array<bool, 2> state_acknowledged{};
                until([&] {
                    for (unsigned i = 0; i < games.size(); ++i) {
                        auto& connection = i == 0 ? a : b;
                        if (connection.checked_frame() && games[i].state() == online::GameSession::State::Playing) {
                            // The fixture has one state check per match.
                            if (connection.checked_frame() == 1376 && !state_acknowledged[i]) {
                                games[i].state_checked(1376); state_acknowledged[i] = true;
                            }
                        }
                        if (!exits[i]) exits[i] = games[i].poll();
                        for (auto& input : games[i].take_inputs()) connection.send_inputs(input);
                        for (auto& state : games[i].take_states()) connection.check_state(state.frame, state.ram, state.registers);
                        for (auto& input : connection.take_inputs()) games[i].send_inputs(input);
                    }
                    return exits[0] && exits[1];
                });
                for (const auto& exit : exits) assert(exit->error.empty() && exit->completed_frame == 1376);
                a.finished(*exits[0]->completed_frame); b.finished(*exits[1]->completed_frame);
            }
            until([&] { return a.roster().phase == Phase::Lobby && b.roster().phase == Phase::Lobby; });
            for (auto& game : games) {
                auto settings = game.settings(); game.cancel(a.roster().match);
                assert(settings.empty() || !fs::exists(settings));
            }
            if (real) for (unsigned i = 0; i < games.size(); ++i) {
                std::ifstream log(game_log(i));
                std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
                assert(text.find("MATCH_STARTED: match=" + std::to_string(match)) != std::string::npos);
            }
            assert(!a.start_at() && !b.start_at());
            assert(a.roster().identity == a_id && b.roster().identity == b_id);
        }
        prepare();
        // Deliver the commit to the OS socket, but do not let clients process it
        // until after its epoch. They must cancel instead of granting a lead.
        auto deadline = monotonic_ms() + 1000;
        while (server.room().phase() != Phase::Playing && monotonic_ms() < deadline) {
            server.service(); std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        assert(server.room().phase() == Phase::Playing);
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        until([&] { return a.roster().phase == Phase::Lobby && b.roster().phase == Phase::Lobby; });
        assert(a.error() == "start arrived too late" && b.error() == "start arrived too late");
        assert(!a.start_at() && !b.start_at());
        assert(a.joined() && b.joined());
        for (auto& game : games) {
            auto settings = game.settings(); assert(game.cancel(a.roster().match));
            assert(!fs::exists(settings));
        }
    }
    if (!real) {
        ENetAddress address{}; assert(enet_address_set_host(&address, "127.0.0.1") == 0);
        Fingerprint fingerprint{42};
        Server server(address, fingerprint);
        LobbySession::Launch launch{executable, rom, scratch / "session-a.log", {"--child"}, {}};
        unsigned compatibility_checks = 0;
        bool compatible = true;
        launch.verify_compatibility = [&] {
            ++compatibility_checks;
            if (!compatible) throw std::runtime_error("local compatibility changed");
        };
        LobbySession a("127.0.0.1", uint16_t(server.port()), "Alice", fingerprint, launch);
        launch.log = scratch / "session-b.log";
        auto b = std::make_unique<LobbySession>("127.0.0.1", uint16_t(server.port()), "Bob", fingerprint, launch);
        auto until = [&](const std::function<bool()>& done) {
            auto deadline = monotonic_ms() + 7000;
            do {
                server.service(); a.poll(); if (b) b->poll();
                if (done()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (monotonic_ms() < deadline);
            throw std::runtime_error("session test deadline exceeded: " + a.error() + "; " + (b ? b->error() : ""));
        };
        until([&] { return a.connection().roster().count == 2 && b->connection().roster().count == 2; });
        auto a_id = a.connection().roster().identity, b_id = b->connection().roster().identity;
        auto prepare = [&](bool wait_for_loading = true) {
            a.ready(true, {3, R"({"controls":{},"joystick_deadzone":17})"}); b->ready(true);
            until([&] { return a.connection().roster().players[0].ready && a.connection().roster().players[1].ready &&
                               b->connection().roster().players[0].ready && b->connection().roster().players[1].ready; });
            (a.connection().slot() == 0 ? a : *b).start();
            if (wait_for_loading)
                until([&] { return a.game().state() == online::GameSession::State::Loading &&
                                   b->game().state() == online::GameSession::State::Loading; });
        };
        for (unsigned match = 1; match <= 3; ++match) {
            prepare();
            auto settings_a = a.game().settings(), settings_b = b->game().settings();
            assert(fs::exists(settings_a / "online-input.json"));
            if (match == 2) a.cancel();
            until([&] { return a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby; });
            assert(a.game().state() == online::GameSession::State::Lobby && b->game().state() == online::GameSession::State::Lobby);
            assert(!fs::exists(settings_a) && !fs::exists(settings_b));
            assert(a.connection().roster().identity == a_id && b->connection().roster().identity == b_id);
            if (match != 2) assert(a.error().empty() && b->error().empty());
            assert(compatibility_checks == match * 2);
        }
        prepare();
        until([&] { return a.connection().start_at() && b->connection().start_at(); });
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        until([&] { return a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby; });
        assert(a.error().starts_with("local match start missed its deadline") &&
               b->error().starts_with("local match start missed its deadline"));
        assert(a.error().find("Match log:") != std::string::npos && b->error().find("Match log:") != std::string::npos);
        assert(a.game().state() == online::GameSession::State::Lobby && b->game().state() == online::GameSession::State::Lobby);
        compatible = false;
        prepare(false);
        until([&] { return a.connection().roster().match == 5 &&
            a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby; });
        assert(a.error() == "local compatibility changed" && b->error() == a.error());
        assert(a.game().settings().empty() && b->game().settings().empty());
        compatible = true;
        prepare();
        auto settings_a = a.game().settings(), settings_b = b->game().settings();
        b.reset();
        until([&] { return a.connection().roster().phase == Phase::Lobby && a.connection().roster().count == 1; });
        assert(a.game().state() == online::GameSession::State::Lobby);
        assert(!fs::exists(settings_a) && !fs::exists(settings_b));
        for (bool crash : {false, true}) {
            launch.executable = crash ? executable : scratch / "missing-game";
            launch.arguments = {"--crash-child"};
            b = std::make_unique<LobbySession>("127.0.0.1", uint16_t(server.port()), "Bob", fingerprint, launch);
            until([&] { return a.connection().roster().count == 2 && b->connection().roster().count == 2; });
            auto previous_match = a.connection().roster().match;
            prepare(false);
            until([&] { return a.connection().roster().match > previous_match &&
                a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby; });
            assert(!b->error().empty() && a.connection().joined() && b->connection().joined());
            assert(b->error().find("Match log:") != std::string::npos);
            assert(a.game().settings().empty() && b->game().settings().empty());
            b.reset();
            until([&] { return a.connection().roster().count == 1; });
        }
        // Leaving from a match window returns everyone to the lobby with the leave as the reason.
        launch.executable = executable; launch.arguments = {"--leave-child"};
        b = std::make_unique<LobbySession>("127.0.0.1", uint16_t(server.port()), "Bob", fingerprint, launch);
        until([&] { return a.connection().roster().count == 2 && b->connection().roster().count == 2; });
        auto left_match = a.connection().roster().match;
        prepare();
        until([&] { return a.connection().roster().match > left_match &&
            a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby; });
        assert(a.error() == reason_text(Reason::Cancelled) && b->error() == a.error());
        assert(a.game().state() == online::GameSession::State::Lobby && b->game().state() == online::GameSession::State::Lobby);
        b.reset();
        until([&] { return a.connection().roster().count == 1; });
        launch.arguments = {"--desync-child"};
        b = std::make_unique<LobbySession>("127.0.0.1", uint16_t(server.port()), "Bob", fingerprint, launch);
        until([&] { return a.connection().roster().count == 2 && b->connection().roster().count == 2; });
        auto desync_match = a.connection().roster().match + 1;
        prepare();
        // Differing states keep both games running until P1 restarts with the same roster.
        until([&] { return a.connection().roster().match == desync_match + 1 && b->connection().roster().match == desync_match + 1 &&
            a.game().state() != online::GameSession::State::Lobby && b->game().state() != online::GameSession::State::Lobby; });
        assert(a.error().empty() && b->error().empty());
        a.cancel();
        until([&] { return a.connection().roster().phase == Phase::Lobby && b->connection().roster().phase == Phase::Lobby &&
            a.game().state() == online::GameSession::State::Lobby && b->game().state() == online::GameSession::State::Lobby; });
        assert(a.connection().joined() && b->connection().joined() && a.game().settings().empty() && b->game().settings().empty());
        // A reconnect may reuse a server match ID; it must not erase the last attempt.
        auto log_base = scratch / "retention.log";
        auto first_log = online::new_match_log(log_base, 1);
        std::ofstream(first_log) << "failure evidence";
        auto second_log = online::new_match_log(log_base, 1);
        assert(first_log != second_log && fs::file_size(first_log) == 16);
        for (unsigned i = 0; i < 12; ++i) online::new_match_log(log_base, i + 2);
        size_t kept = 0;
        for (const auto& entry : fs::directory_iterator(first_log.parent_path()))
            kept += entry.path().extension() == ".log";
        assert(kept == 10);
        std::cout << "PASS: application session owns prepare/start/finish, input transfer, preference handoff, cancel/restart/leave, match-window leave, missed epoch and failed launch/crash cleanup\n";
    }
    enet_deinitialize();
    std::cout << "PASS: production client clocks, owned " << (real ? "real game startup/cancel" : "fixture child completion and IPC/ENet input transfer")
              << ", repeated match identity retention and late-start cancellation\n";
}
