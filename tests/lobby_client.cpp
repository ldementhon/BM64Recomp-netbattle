#include "../server/server.hpp"
#include "../src/online/lobby_client.hpp"
#include "../src/online/game_session.hpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>

using namespace online::lobby;
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--child") {
        // ENet itself does not set CLOEXEC. Verify the owner helper does.
        for (int fd = 4; fd < 1024; ++fd) {
            int type; socklen_t size = sizeof(type);
            assert(getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &size) != 0 || type != SOCK_DGRAM);
        }
        online::MatchChannel channel(3);
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
                until([&] {
                    for (unsigned i = 0; i < games.size(); ++i) {
                        auto& connection = i == 0 ? a : b;
                        if (!exits[i]) exits[i] = games[i].poll();
                        for (auto& input : games[i].take_inputs()) connection.send_inputs(input);
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
    enet_deinitialize();
    std::cout << "PASS: production client clocks, owned " << (real ? "real game startup/cancel" : "fixture child completion and IPC/ENet input transfer")
              << ", repeated match identity retention and late-start cancellation\n";
}
