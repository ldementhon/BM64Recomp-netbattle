#include "../src/online/fingerprint.hpp"
#include "../src/online/lobby_client.hpp"
#include "../src/online/game_session.hpp"
#include <fstream>
#include <iostream>
#include <thread>

// One scripted player that joins a real lobby server, so a match can span machines.
// Run one per machine; compare the NETWORK_STATE lines in each player log afterwards.
using namespace online;
using namespace online::lobby;
namespace fs = std::filesystem;
static void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }

static void run(int argc, char** argv) {
    check(argc == 10, "Usage: network_peer SERVER PORT NAME GAME ROM INPUT_RECORDING OUTPUT_DIR HUMANS STOP_FRAME");
    auto executable = fs::absolute(argv[4]), rom = fs::absolute(argv[5]), recording = fs::absolute(argv[6]), output = fs::absolute(argv[7]);
    unsigned humans = unsigned(std::stoul(argv[8])), stop = unsigned(std::stoul(argv[9]));
    check(humans >= 2 && humans <= 4, "invalid human count");
    fs::create_directories(output);
    auto fingerprint = client_fingerprint(executable, rom);
    std::cout << "PEER: fingerprint=" << fingerprint_hex(fingerprint) << std::endl;
    LobbyClient client(argv[1], uint16_t(std::stoul(argv[2])), argv[3], fingerprint);
    GameSession game;
    fs::path log, settings;
    bool ready_sent = false, start_sent = false, cancel_sent = false, acknowledged = false, started = false;
    std::optional<uint64_t> finished;
    uint64_t last_log_read = 0;
    const auto deadline = monotonic_ms() + 300000;
    while (monotonic_ms() < deadline) {
        auto now = monotonic_ms();
        for (const auto& event : client.poll()) {
            if (event.op == Op::Prepare) {
                log = output / ("player-" + std::to_string(client.slot()) + ".log");
                game.start(event.match, executable, rom, log,
                    {"--checkpoint-test", "--single-battle", "--online-test"},
                    {{"BM64_INPUT_SCRIPT", recording.string()}, {"BM64_NETWORK_INPUT", "1"},
                     {"BM64_NETWORK_VERIFY_EVERY", "200"}, {"BM64_NETWORK_STOP", std::to_string(stop)}},
                    humans, client.slot());
                settings = game.settings();
                std::cout << "PEER: slot=" << client.slot() << " match=" << event.match << " log=" << log.string() << std::endl;
            } else if (event.op == Op::End) {
                check(event.reason == Reason::None || (stop && event.reason == Reason::Cancelled), reason_text(event.reason));
                game.cancel(event.match);
            } else if (event.op == Op::Reject) throw std::runtime_error(reason_text(event.reason));
        }
        check(!client.disconnected(), client.error());
        if (auto exited = game.poll()) {
            check(exited->error.empty() && exited->completed_frame.has_value(), "game failed: " + exited->error + " (" + log.string() + ")");
            finished = *exited->completed_frame; client.finished(*finished);
        }
        if (game.ready() && client.clock() && !acknowledged) {
            auto ready = *game.ready(); client.loaded(ready.frame, ready.ram, ready.registers); acknowledged = true;
        }
        if (client.start_at() && now >= *client.start_at() && !started) {
            check(game.playing(client.roster().match), "cannot release ready game"); started = true;
        }
        if (client.roster().phase == Phase::Playing) {
            for (const auto& input : game.take_inputs()) client.send_inputs(input);
            for (const auto& input : client.take_inputs()) game.send_inputs(input);
        }
        if (!ready_sent && client.roster().count == humans && client.clock()) { client.ready(true); ready_sent = true; }
        if (ready_sent && !start_sent && client.slot() == 0) {
            bool all_ready = true;
            for (unsigned i = 0; i < humans; ++i) all_ready &= client.roster().players[i].ready;
            if (all_ready) { client.start(); start_sent = true; }
        }
        if (stop && started && !cancel_sent && client.slot() == 0 && now - last_log_read >= 100) {
            last_log_read = now;
            std::ifstream text(log);
            std::string contents{std::istreambuf_iterator<char>(text), std::istreambuf_iterator<char>()};
            if (contents.find("NETWORK_STATE: frame=" + std::to_string(stop) + " ") != std::string::npos) { client.cancel(); cancel_sent = true; }
        }
        if (started && client.roster().phase == Phase::Lobby && game.state() == GameSession::State::Lobby && (stop || finished)) {
            check(!fs::exists(settings), "settings were not cleaned");
            std::cout << "PASS: peer slot=" << client.slot() << " finish=" << finished.value_or(0) << " log=" << log.string() << std::endl;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    throw std::runtime_error("network peer deadline exceeded");
}

int main(int argc, char** argv) {
    if (enet_initialize() != 0) return 1;
    try { run(argc, argv); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; enet_deinitialize(); return 1; }
    enet_deinitialize();
}
