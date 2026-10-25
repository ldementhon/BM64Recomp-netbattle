#include "../server/server.hpp"
#include "../src/online/lobby_client.hpp"
#include "../src/online/game_session.hpp"
#include <fstream>
#include <iostream>
#include <thread>

using namespace online;
using namespace online::lobby;
namespace fs = std::filesystem;
static void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
struct Peer {
    LobbyClient connection;
    GameSession game;
    fs::path log, settings;
    bool acknowledged = false, started = false, reached = false;
    std::optional<uint64_t> finished;
    uint64_t sampled = 0;
    Peer(unsigned port, const Fingerprint& fingerprint)
        : connection("127.0.0.1", uint16_t(port), "Player", fingerprint) {}
};

static void run(int argc, char** argv) {
    check(argc == 8, "Usage: network_game GAME ROM INPUT_RECORDING OUTPUT_DIR HUMANS DELAY_MS STOP_FRAME");
    auto executable = fs::absolute(argv[1]), rom = fs::absolute(argv[2]), recording = fs::absolute(argv[3]), output = fs::absolute(argv[4]);
    unsigned humans = unsigned(std::stoul(argv[5])), delay = unsigned(std::stoul(argv[6])), stop = unsigned(std::stoul(argv[7]));
    check(humans >= 2 && humans <= 4 && delay <= 200, "invalid test parameters");
    fs::create_directories(output);
    ENetAddress address{}; check(enet_address_set_host(&address, "127.0.0.1") == 0, "loopback address");
    Fingerprint fingerprint{42}; Server server(address, fingerprint);
    std::vector<std::unique_ptr<Peer>> peers;
    for (unsigned i = 0; i < humans; ++i) peers.push_back(std::make_unique<Peer>(server.port(), fingerprint));
    struct Pending { uint64_t due; unsigned peer; bool upload; InputBatch input; };
    std::vector<Pending> pending;
    bool ready_sent = false, start_sent = false, cancel_sent = false, loss_triggered = false;
    uint64_t loss_end = 0, last_log_read = 0, dropped = 0;
    const auto deadline = monotonic_ms() + 150000;
    while (monotonic_ms() < deadline) {
        server.service();
        auto now = monotonic_ms();
        bool all_joined = true, all_reached = true, all_done = true;
        for (unsigned i = 0; i < peers.size(); ++i) {
            auto& peer = *peers[i]; auto& client = peer.connection;
            for (const auto& event : client.poll()) {
                if (event.op == Op::Prepare) {
                    peer.log = output / ("player-" + std::to_string(client.slot()) + ".log");
                    peer.game.start(event.match, executable, rom, peer.log,
                        {"--checkpoint-test", "--single-battle", "--online-test"},
                        {{"BM64_INPUT_SCRIPT", recording.string()}, {"BM64_NETWORK_INPUT", "1"},
                         {"BM64_NETWORK_VERIFY_EVERY", "200"}, {"BM64_NETWORK_STOP", std::to_string(stop)}},
                        humans, client.slot());
                    peer.settings = peer.game.settings();
                } else if (event.op == Op::End) {
                    check(event.reason == Reason::None || (cancel_sent && event.reason == Reason::Cancelled), reason_text(event.reason));
                    peer.game.cancel(event.match);
                } else if (event.op == Op::Reject) throw std::runtime_error(reason_text(event.reason));
            }
            check(!client.disconnected(), client.error());
            all_joined &= client.roster().count == humans && client.clock().has_value();
            if (auto exited = peer.game.poll()) {
                check(exited->error.empty() && exited->completed_frame.has_value(), "game failed: " + exited->error + " (" + peer.log.string() + ")");
                peer.finished = *exited->completed_frame; client.finished(*exited->completed_frame);
            }
            if (peer.game.ready() && client.clock() && !peer.acknowledged) {
                auto ready = *peer.game.ready(); client.loaded(ready.frame, ready.ram, ready.registers); peer.acknowledged = true;
            }
            if (client.start_at() && now >= *client.start_at() && !peer.started) {
                check(peer.game.playing(client.roster().match), "cannot release ready game"); peer.started = true;
            }
            for (const auto& input : peer.game.take_inputs()) {
                peer.sampled = input.first + input.count;
                auto extra = delay ? delay / 3 : 0;
                if (delay && input.slot == 1 && peer.sampled % 180 < 60) extra = delay;
                if (delay && input.slot == 3 && peer.sampled % 2 == 0) extra = delay * 2 / 3;
                pending.push_back({now + extra, i, true, input});
            }
            if (delay && !loss_triggered && peer.sampled >= 650) {
                loss_triggered = true; loss_end = now + 500;
            }
            for (const auto& input : client.take_inputs()) {
                if (delay && client.slot() == 2 && now < loss_end) { ++dropped; continue; }
                auto extra = delay ? (input.slot * 7 + client.slot() * 11 + input.first + input.count) % 31 : 0;
                pending.push_back({now + extra, i, false, input});
            }
            if (stop && now - last_log_read >= 100 && peer.started) {
                std::ifstream log(peer.log);
                std::string text{std::istreambuf_iterator<char>(log), std::istreambuf_iterator<char>()};
                peer.reached = text.find("NETWORK_STATE: frame=" + std::to_string(stop) + " ") != std::string::npos;
            }
            all_reached &= peer.reached;
            all_done &= peer.finished.has_value();
        }
        if (now - last_log_read >= 100) last_log_read = now;
        check(pending.size() < 512, "unbounded test delivery queue");
        for (auto item = pending.begin(); item != pending.end();) {
            if (item->due > now) { ++item; continue; }
            auto& peer = *peers[item->peer];
            if (peer.connection.roster().phase == Phase::Playing) {
                if (item->upload) peer.connection.send_inputs(item->input);
                else peer.game.send_inputs(item->input);
            }
            item = pending.erase(item);
        }
        if (all_joined && !ready_sent) {
            for (auto& peer : peers) peer->connection.ready(true);
            ready_sent = true;
        }
        if (ready_sent && !start_sent) {
            for (auto& peer : peers) if (peer->connection.slot() == 0) {
                bool all_ready = true;
                for (unsigned i = 0; i < humans; ++i) all_ready &= peer->connection.roster().players[i].ready;
                if (all_ready) { peer->connection.start(); start_sent = true; }
            }
        }
        if (stop && all_reached && !cancel_sent) {
            peers[0]->connection.cancel(); cancel_sent = true;
        }
        bool lobby = start_sent && server.room().phase() == Phase::Lobby;
        for (auto& peer : peers) lobby &= peer->connection.roster().phase == Phase::Lobby && peer->game.state() == GameSession::State::Lobby;
        if (lobby && ((stop && cancel_sent) || (!stop && all_done))) {
            for (auto& peer : peers) check(!fs::exists(peer->settings), "settings were not cleaned");
            if (!stop) for (auto& peer : peers) check(peer->finished == peers[0]->finished, "different finish frames");
            if (delay && humans >= 3 && stop >= 1000) check(dropped > 0, "P3 loss injection did not run");
            std::cout << "PASS: " << humans << " real ROM clients; delay=" << delay << " dropped=" << dropped
                      << " stop=" << stop << " finish=" << peers[0]->finished.value_or(0) << '\n';
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("real network game test deadline exceeded");
}
int main(int argc, char** argv) {
    if (enet_initialize() != 0) return 1;
    try { run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; enet_deinitialize(); return 1; }
    enet_deinitialize();
}
