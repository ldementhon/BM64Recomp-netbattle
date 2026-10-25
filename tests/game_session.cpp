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
        fs::path settings = std::getenv("APP_FOLDER_PATH");
        assert(fs::exists(settings / "bm64_us.z64"));
        assert(!fs::exists(settings / "personal-marker"));
        std::ofstream(settings / "child-save") << "match only";
        if (std::string(argv[1]) == "--child-crash") raise(SIGKILL);
        if (std::string(argv[1]) == "--child-hold") for (;;) pause();
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
    // This endpoint belongs to the surviving owner, never to disposable children.
    int connection[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, connection) == 0);
    for (int fd : connection) assert(fcntl(fd, F_SETFD, FD_CLOEXEC) == 0);
    online::GameSession session;
    auto start = [&](uint64_t match, const char* child_mode) {
        session.start(match, executable, rom, scratch / ("match-" + std::to_string(match) + ".log"),
            real_game ? std::vector<std::string>{"--checkpoint-test", "--single-battle"} :
                        std::vector<std::string>{child_mode},
            real_game ? std::map<std::string, std::string>{{"BM64_STOP_FRAME", "1650"}} :
                        std::map<std::string, std::string>{});
    };
    start(1, "--child-hold");
    auto first_settings = session.settings();
    assert(session.state() == online::GameSession::State::Loading);
    assert(!session.playing(99) && !session.cancel(99));
    assert(session.cancel(1));
    assert(!fs::exists(first_settings));
    assert(session.state() == online::GameSession::State::Lobby);
    for (uint64_t match : {2, 3}) {
        start(match, "--child-finish");
        assert(!session.cancel(match - 1) && !session.playing(match - 1));
        auto settings = session.settings();
        // This tests process completion, not the unimplemented game-end acknowledgment.
        auto result = wait_exit(session);
        assert(result.match == match && !result.loading_timeout);
        assert(WIFEXITED(result.status) && WEXITSTATUS(result.status) == 0);
        assert(!fs::exists(settings));
        assert(session.state() == online::GameSession::State::Lobby);
        assert(write(connection[0], "x", 1) == 1);
        char byte; assert(read(connection[1], &byte, 1) == 1 && byte == 'x');
    }
    if (!real_game) {
        start(4, "--child-crash");
        auto result = wait_exit(session);
        assert(WIFSIGNALED(result.status));
        start(5, "--child-hold");
        assert(session.playing(5) && !session.playing(5));
        assert(session.cancel(5));
        bool failed = false;
        try { session.start(6, scratch / "missing-game", rom, scratch / "failed.log", {}); }
        catch (const std::system_error&) { failed = true; }
        assert(failed && session.state() == online::GameSession::State::Lobby && session.settings().empty());
        try { start(6, "--child-finish"); assert(false); }
        catch (const std::invalid_argument&) {}
        start(7, "--child-finish");
        result = wait_exit(session);
        assert(WIFEXITED(result.status));
        start(8, "--child-hold");
        auto timed_out_settings = session.settings();
        auto timeout = session.poll(std::chrono::steady_clock::now() + 31s);
        assert(timeout && timeout->match == 8 && timeout->loading_timeout);
        assert(session.state() == online::GameSession::State::Lobby && !fs::exists(timed_out_settings));
        start(9, "--child-hold");
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
    }
    close(connection[0]); close(connection[1]);
    std::string marker;
    std::ifstream(personal / "personal-marker") >> marker;
    assert(marker == "unchanged" && !fs::exists(personal / "child-save"));
    std::cout << "PASS: " << (real_game ? "real game cancellation and repeated diagnostic exits" :
        "loading cancellation, completion, crash, failed launch, restart and stale events")
        << "; isolated settings cleaned; local owner connection retained\n";
}
