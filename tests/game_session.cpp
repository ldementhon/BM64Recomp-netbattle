#include "../src/online/game_session.hpp"
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/socket.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

using namespace std::chrono_literals;
namespace fs = std::filesystem;
static void wait_ready(online::GameSession& session) {
    auto deadline = std::chrono::steady_clock::now() + 35s;
    while (!session.ready() && std::chrono::steady_clock::now() < deadline) {
        if (auto exit = session.poll()) throw std::runtime_error("child failed loading: " + exit->error);
        std::this_thread::sleep_for(10ms);
    }
    assert(session.ready());
}
static online::GameSession::Exit wait_exit(online::GameSession& session) {
    auto deadline = std::chrono::steady_clock::now() + 180s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto exit = session.poll()) return *exit;
        std::this_thread::sleep_for(10ms);
    }
    throw std::runtime_error("child exceeded test deadline");
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]).starts_with("--child-")) {
        assert(!std::getenv("BM64_NEGATIVE_INPUT"));
        if (std::string(argv[1]) == "--child-seats-2")
            assert(std::string(std::getenv("BM64_ONLINE_HUMANS")) == "2");
        fs::path settings = std::getenv("APP_FOLDER_PATH");
        assert(fs::exists(settings / "bm64_us.z64"));
        assert(!fs::exists(settings / "personal-marker"));
        std::ofstream(settings / "child-save") << "match only";
        if (std::string(argv[1]) == "--child-crash") raise(SIGKILL);
        if (std::string(argv[1]) == "--child-hold") for (;;) pause();
        online::MatchChannel channel(3);
        auto match = std::stoull(std::getenv("BM64_OWNER_MATCH"));
        if (std::string(argv[1]) == "--child-wrong-match") {
            channel.send({1, online::MatchEvent::Ready, match + 1, 1374});
            for (;;) pause();
        }
        if (std::string(argv[1]) == "--child-early-finish") {
            channel.send({1, online::MatchEvent::Finished, match, 2000});
            for (;;) pause();
        }
        channel.send({1, online::MatchEvent::Ready, match, 1374, 123, 456});
        if (std::string(argv[1]) == "--child-duplicate") {
            channel.send({1, online::MatchEvent::Ready, match, 1374, 123, 456});
            for (;;) pause();
        }
        for (;;) {
            if (auto command = channel.receive()) {
                assert(command->event == online::MatchEvent::Start && command->match == match && command->frame == 1374);
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (std::string(argv[1]) == "--child-play-hold") for (;;) pause();
        channel.send({1, online::MatchEvent::Finished, match, 2000});
        return 0;
    }
    auto scratch = fs::absolute("build/experiments/session-tests");
    fs::create_directories(scratch);
    auto personal = scratch / "personal";
    fs::create_directories(personal);
    std::ofstream(personal / "personal-marker") << "unchanged";
    setenv("APP_FOLDER_PATH", personal.c_str(), 1);
    setenv("BM64_NEGATIVE_INPUT", "1", 1);
    auto rom = scratch / "fixture.rom";
    std::ofstream(rom) << "local process ownership fixture, not a ROM";
    bool real_game = argc == 3;
    if (real_game) rom = fs::absolute(argv[2]);
    auto executable = fs::absolute(real_game ? argv[1] : argv[0]);
    auto recording = scratch / "complete-match-inputs.txt";
    if (real_game) {
        std::ofstream out(recording);
        for (auto file : {"tests/fixtures/battle-active-inputs.txt", "tests/fixtures/battle-finish.txt"}) {
            std::ifstream input(file);
            out << input.rdbuf() << '\n';
        }
    }
    // This endpoint belongs to the surviving owner, never to disposable children.
    int connection[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, connection) == 0);
    for (int fd : connection) assert(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    online::GameSession session;
    auto start = [&](uint64_t match, const char* child_mode) {
        session.start(match, executable, rom, scratch / ("match-" + std::to_string(match) + ".log"),
            real_game ? std::vector<std::string>{"--checkpoint-test", "--single-battle", "--online-test"} :
                        std::vector<std::string>{child_mode},
            real_game ? std::map<std::string, std::string>{{"BM64_STOP_FRAME", "5600"},
                            {"BM64_INPUT_SCRIPT", recording.string()}} :
                        std::map<std::string, std::string>{});
    };
    start(1, "--child-hold");
    auto first_settings = session.settings();
    assert(session.state() == online::GameSession::State::Loading);
    assert(!session.playing(99) && !session.cancel(99));
    assert(session.cancel(1));
    assert(!fs::exists(first_settings));
    assert(session.state() == online::GameSession::State::Lobby);
    std::optional<online::MatchMessage> initial;
    for (uint64_t match : {2, 3}) {
        start(match, "--child-finish");
        assert(!session.cancel(match - 1) && !session.playing(match - 1));
        auto settings = session.settings();
        assert(!session.playing(match));
        wait_ready(session);
        if (initial) {
            assert(initial->frame == session.ready()->frame && initial->ram == session.ready()->ram &&
                   initial->registers == session.ready()->registers);
        }
        initial = session.ready();
        // Waiting here must not advance or finish the paused game.
        std::this_thread::sleep_for(100ms);
        assert(!session.poll());
        assert(session.playing(match) && !session.playing(match));
        auto result = wait_exit(session);
        assert(result.match == match && !result.loading_timeout);
        assert(result.error.empty() && result.completed_frame == (real_game ? 5401 : 2000));
        assert(!fs::exists(settings));
        assert(session.state() == online::GameSession::State::Lobby);
        assert(write(connection[0], "x", 1) == 1);
        char byte; assert(read(connection[1], &byte, 1) == 1 && byte == 'x');
    }
    if (!real_game) {
        start(4, "--child-crash");
        auto result = wait_exit(session);
        assert(WIFSIGNALED(result.status));
        start(5, "--child-play-hold");
        wait_ready(session);
        assert(session.playing(5) && !session.playing(5));
        assert(session.cancel(5));
        bool failed = false;
        try { session.start(6, scratch / "missing-game", rom, scratch / "failed.log", {}); }
        catch (const std::system_error&) { failed = true; }
        assert(failed && session.state() == online::GameSession::State::Lobby && session.settings().empty());
        try { start(6, "--child-finish"); assert(false); }
        catch (const std::invalid_argument&) {}
        start(7, "--child-finish");
        wait_ready(session);
        assert(session.playing(7));
        result = wait_exit(session);
        assert(result.completed_frame);
        start(8, "--child-hold");
        auto timed_out_settings = session.settings();
        auto timeout = session.poll(std::chrono::steady_clock::now() + 31s);
        assert(timeout && timeout->match == 8 && timeout->loading_timeout);
        assert(session.state() == online::GameSession::State::Lobby && !fs::exists(timed_out_settings));
        start(9, "--child-play-hold");
        wait_ready(session);
        assert(session.playing(9));
        assert(!session.poll(std::chrono::steady_clock::now() + 31s));
        assert(session.cancel(9));
        fs::path abandoned_settings;
        {
            online::GameSession abandoned;
            abandoned.start(1, executable, rom, scratch / "abandoned.log", {"--child-hold"});
            abandoned_settings = abandoned.settings();
        }
        assert(!fs::exists(abandoned_settings));
        start(10, "--child-finish");
        wait_ready(session);
        timeout = session.poll(std::chrono::steady_clock::now() + 31s);
        assert(timeout && timeout->loading_timeout && !timeout->completed_frame);
        start(11, "--child-finish");
        wait_ready(session);
        assert(session.cancel(11));
        uint64_t invalid_match = 12;
        for (auto mode : {"--child-wrong-match", "--child-early-finish", "--child-duplicate"}) {
            start(invalid_match++, mode);
            auto exit = wait_exit(session);
            assert(!exit.error.empty() && !exit.completed_frame && session.state() == online::GameSession::State::Lobby);
        }
        for (unsigned count : {0, 1, 5}) {
            try {
                session.start(15, executable, rom, scratch / "invalid-seats.log", {}, {}, count);
                assert(false);
            } catch (const std::invalid_argument&) {}
        }
        session.start(15, executable, rom, scratch / "two-seats.log", {"--child-seats-2"},
                      {{"BM64_ONLINE_HUMANS", "999"}}, 2);
        wait_ready(session);
        assert(session.playing(15));
        assert(wait_exit(session).completed_frame);
    }
    close(connection[0]); close(connection[1]);
    std::string marker;
    std::ifstream(personal / "personal-marker") >> marker;
    assert(marker == "unchanged" && !fs::exists(personal / "child-save"));
    std::cout << "PASS: " << (real_game ? "real game cancellation and two normal matches with matching ready digests" :
        "loading cancellation, acknowledged start/finish, crash, failed launch, restart and invalid events")
        << "; isolated settings cleaned; local owner connection retained\n";
}
