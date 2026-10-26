#include "../src/online/game_session.hpp"
#include <cassert>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef _WIN32
#include "../src/online/windows_game_process.hpp"
#endif
#ifndef _WIN32
#include <sys/wait.h>
#endif

using namespace std::chrono_literals;
namespace fs = std::filesystem;
static const std::vector<std::string> quoted_arguments{"", "two words", "a\\\"b\\\\", "\"quoted\"", "C:\\path with spaces\\", "\t"};
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
    if (argc >= 2 && std::string(argv[1]).starts_with("--child-")) {
        assert(!std::getenv("BM64_NEGATIVE_INPUT"));
        if (std::string(argv[1]) == "--child-arguments") {
            assert(argc == int(quoted_arguments.size()) + 2);
            for (size_t i = 0; i < quoted_arguments.size(); ++i) assert(argv[i + 2] == quoted_arguments[i]);
            assert(std::string(std::getenv("BM64_ONLINE_HUMANS")) == "2");
            assert(std::string(std::getenv("BM64_LOCAL_PORT")) == "0");
#ifdef _WIN32
            wchar_t value[32];
            assert(GetEnvironmentVariableW(L"BM64_TEST_UNICODE", value, 32) && std::wstring(value) == L"\u00e9\u96ea");
            wchar_t path[32768];
            auto handle = reinterpret_cast<HANDLE>(uintptr_t(std::stoull(std::getenv("BM64_TEST_HANDLE"))));
            auto length = GetFinalPathNameByHandleW(handle, path, 32768, FILE_NAME_NORMALIZED);
            assert(!length || fs::path(path).filename() != L"personal-marker");
#endif
        }
        if (std::string(argv[1]) == "--child-seats-2")
            assert(std::string(std::getenv("BM64_ONLINE_HUMANS")) == "2");
        fs::path settings = std::getenv("APP_FOLDER_PATH");
        assert(fs::exists(settings / "bm64_us.z64"));
        assert(!fs::exists(settings / "personal-marker"));
        if (std::string(argv[1]) == "--child-preferences") {
            assert(std::string(std::getenv("BM64_LOCAL_PORT")) == "3");
            assert(std::string(std::getenv("BM64_ONLINE_SLOT")) == "1");
            std::ifstream input(settings / "online-input.json");
            std::string text((std::istreambuf_iterator<char>(input)), {});
            assert(text == R"({"controls":{},"joystick_deadzone":17})");
            online::validate_match_folder(settings);
        }
        std::ofstream(settings / "child-save") << "match only";
        if (std::string(argv[1]) == "--child-crash") {
#ifdef _WIN32
            TerminateProcess(GetCurrentProcess(), 77);
#else
            raise(SIGKILL);
#endif
        }
        if (std::string(argv[1]) == "--child-hold") for (;;) std::this_thread::sleep_for(1s);
        auto channel = online::MatchChannel::inherited();
#ifdef _WIN32
        DWORD inherited_flags;
        assert(GetHandleInformation(channel.fd(), &inherited_flags) && !(inherited_flags & HANDLE_FLAG_INHERIT));
#endif
        auto match = std::stoull(std::getenv("BM64_OWNER_MATCH"));
        if (std::string(argv[1]) == "--child-wrong-match") {
            channel.send({online::match_channel_version, online::MatchEvent::Ready, match + 1, 1374});
            for (;;) std::this_thread::sleep_for(1s);
        }
        if (std::string(argv[1]) == "--child-early-finish") {
            channel.send({online::match_channel_version, online::MatchEvent::Finished, match, 2000});
            for (;;) std::this_thread::sleep_for(1s);
        }
        channel.send({online::match_channel_version, online::MatchEvent::Ready, match, 1374, 123, 456});
        if (std::string(argv[1]) == "--child-duplicate") {
            channel.send({online::match_channel_version, online::MatchEvent::Ready, match, 1374, 123, 456});
            for (;;) std::this_thread::sleep_for(1s);
        }
        for (;;) {
            if (auto command = channel.receive()) {
                assert(command->event == online::MatchEvent::Start && command->match == match && command->frame == 1374);
                break;
            }
            std::this_thread::sleep_for(1ms);
        }
        if (std::string(argv[1]) == "--child-play-hold") for (;;) std::this_thread::sleep_for(1s);
        if (std::string(argv[1]) == "--child-input" || std::string(argv[1]) == "--child-bad-input") {
            online::InputBatch input;
            input.match = match; input.count = 1;
            input.slot = uint8_t(std::stoul(std::getenv("BM64_ONLINE_SLOT")));
            if (std::string(argv[1]) == "--child-bad-input") input.slot = uint8_t((input.slot + 1) % 3);
            input.pads[0] = {0x8000, 64, -64};
            channel.send(online::input_message(input, false));
            for (;;) {
                if (auto relayed = channel.receive()) {
                    assert(relayed->event == online::MatchEvent::InputRelay && relayed->input.match == match);
                    assert(relayed->input.slot == 0 && relayed->input.pads[0].buttons == 0x4000);
                    break;
                }
                std::this_thread::sleep_for(1ms);
            }
        }
        channel.send({online::match_channel_version, online::MatchEvent::Finished, match, 2000});
        return 0;
    }
    auto scratch = fs::absolute("build/experiments/session-tests");
    fs::create_directories(scratch);
    auto personal = scratch / "personal";
    fs::create_directories(personal);
    std::ofstream(personal / "personal-marker") << "unchanged";
#ifdef _WIN32
    assert(_putenv_s("app_folder_path", personal.string().c_str()) == 0);
    assert(_putenv_s("bm64_negative_input", "1") == 0);
#else
    setenv("APP_FOLDER_PATH", personal.c_str(), 1);
    setenv("BM64_NEGATIVE_INPUT", "1", 1);
#endif
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
    auto [connection, peer] = online::MatchChannel::pair();
    online::GameSession session;
    auto start = [&](uint64_t match, const char* child_mode) {
        session.start(match, executable, rom, scratch / ("match-" + std::to_string(match) + ".log"),
            real_game ? std::vector<std::string>{"--checkpoint-test", "--single-battle", "--online-test"} :
                        std::vector<std::string>{child_mode},
            real_game ? std::map<std::string, std::string>{{"BM64_STOP_FRAME", "5600"},
                            {"BM64_INPUT_SCRIPT", recording.string()}} :
                        std::map<std::string, std::string>{}, 4, 0,
            real_game && match == 3 ? online::LocalInput{2,
                R"({"controls":{"controller_defaults_version":1,"ports":[{"mode":"Keyboard","keyboard":{"A":[{"input_type":1,"input_id":27}]}},{"mode":"Off"},{"mode":"Keyboard","stick_sensitivity":150,"stick_calibration":{"enabled":true,"center_x":0.2,"center_y":0,"left":1,"right":1,"up":1,"down":1}}]},"joystick_deadzone":17})"}
                : online::LocalInput{});
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
        connection.send({online::match_channel_version, online::MatchEvent::Ready, match, 1});
        assert(peer.receive()->match == match);
    }
    if (!real_game) {
        start(4, "--child-crash");
        auto result = wait_exit(session);
#ifdef _WIN32
        assert(result.status == 77);
#else
        assert(WIFSIGNALED(result.status));
#endif
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
        try {
            session.start(16, executable, rom, scratch / "invalid-slot.log", {}, {}, 2, 2); assert(false);
        } catch (const std::invalid_argument&) {}
        session.start(16, executable, rom, scratch / "inputs.log", {"--child-input"},
                      {{"BM64_ONLINE_SLOT", "999"}}, 3, 2);
        wait_ready(session); assert(session.playing(16));
        std::vector<online::InputBatch> uploads;
        auto upload_deadline = std::chrono::steady_clock::now() + 2s;
        while (uploads.empty() && std::chrono::steady_clock::now() < upload_deadline) {
            assert(!session.poll()); uploads = session.take_inputs(); std::this_thread::sleep_for(1ms);
        }
        assert(uploads.size() == 1 && uploads[0].slot == 2 && uploads[0].pads[0].buttons == 0x8000);
        auto relayed = uploads[0]; relayed.match = 15;
        assert(!session.send_inputs(relayed));
        relayed.match = 16; relayed.slot = 3;
        try { session.send_inputs(relayed); assert(false); } catch (const std::logic_error&) {}
        relayed.slot = 0; relayed.pads[0].buttons = 0x4000;
        assert(session.send_inputs(relayed));
        assert(wait_exit(session).completed_frame && session.take_inputs().empty());
        start(17, "--child-bad-input"); wait_ready(session); assert(session.playing(17));
        assert(!wait_exit(session).error.empty());
        for (const auto& input : {online::LocalInput{4, {}}, online::LocalInput{0, std::string(online::local_input_limit + 1, 'x')}}) {
            try {
                session.start(18, executable, rom, scratch / "invalid-input.log", {}, {}, 2, 1, input);
                assert(false);
            } catch (const std::invalid_argument&) {}
        }
        session.start(18, executable, rom, scratch / "preferences.log", {"--child-preferences"},
                      {{"BM64_LOCAL_PORT", "999"}}, 2, 1, {3, R"({"controls":{},"joystick_deadzone":17})"});
        auto preferences = session.settings();
        wait_ready(session); assert(session.playing(18));
        assert(wait_exit(session).completed_frame && !fs::exists(preferences));

        auto spaced_executable = scratch / "child with spaces.exe";
        fs::copy_file(executable, spaced_executable, fs::copy_options::overwrite_existing);
        auto arguments = quoted_arguments;
        arguments.insert(arguments.begin(), "--child-arguments");
        std::map<std::string, std::string> environment;
#ifdef _WIN32
        SECURITY_ATTRIBUTES access{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE sentinel = CreateFileW((personal / "personal-marker").c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      &access, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        assert(sentinel != INVALID_HANDLE_VALUE);
        environment = {{"BM64_TEST_HANDLE", std::to_string(reinterpret_cast<uintptr_t>(sentinel))},
                       {"BM64_TEST_UNICODE", "\xc3\xa9\xe9\x9b\xaa"}, {"BM64_online_humans", "999"},
                       {"BM64_local_port", "999"}, {"BM64_owner_channel", "0"}};
#endif
        session.start(19, spaced_executable, rom, scratch / "quoted arguments.log", arguments, environment, 2);
        wait_ready(session); assert(session.playing(19));
        assert(wait_exit(session).completed_frame);
#ifdef _WIN32
        CloseHandle(sentinel);
#endif
        fs::remove(spaced_executable);
#ifdef _WIN32
        auto job_settings = online::detail::make_match_directory();
        fs::copy_file(rom, job_settings / "bm64_us.z64");
        auto [job_owner, job_child] = online::MatchChannel::pair();
        auto owned = online::detail::launch_windows_game(executable, scratch / "owner-exit.log", job_settings,
            job_child.fd(), {"--child-hold"}, {});
        BOOL assigned = FALSE;
        assert(IsProcessInJob(owned.process, owned.job, &assigned) && assigned);
        assert(WaitForSingleObject(owned.process, 20) == WAIT_TIMEOUT);
        assert(CloseHandle(owned.job));
        assert(WaitForSingleObject(owned.process, 5000) == WAIT_OBJECT_0);
        CloseHandle(owned.process);
        fs::remove_all(job_settings);
#endif

        auto isolated = scratch / "folder-validation";
        fs::create_directories(isolated);
        fs::copy_file(rom, isolated / "bm64_us.z64", fs::copy_options::overwrite_existing);
        online::validate_match_folder(isolated);
        auto rejects = [&] {
            try { online::validate_match_folder(isolated); assert(false); }
            catch (const std::runtime_error&) {}
        };
        std::ofstream(isolated / "online-input.json") << "{}";
        online::validate_match_folder(isolated);
        std::ofstream(isolated / "cheats.json") << "{}";
        rejects(); fs::remove(isolated / "cheats.json");
        std::ofstream(isolated / "online-input.json", std::ios::trunc);
        rejects();
        std::ofstream(isolated / "online-input.json") << std::string(online::local_input_limit + 1, 'x');
        rejects(); fs::remove(isolated / "online-input.json");
        std::error_code symlink_error;
        fs::create_symlink(rom, isolated / "online-input.json", symlink_error);
#ifdef _WIN32
        // Windows may require Developer Mode or administrator rights for symlinks.
        if (symlink_error) {
            assert(symlink_error.value() == ERROR_PRIVILEGE_NOT_HELD);
            std::cout << "SKIP: Windows symlink fixture requires Developer Mode or administrator rights\n";
        }
#else
        assert(!symlink_error);
#endif
        if (!symlink_error) { rejects(); fs::remove(isolated / "online-input.json"); }
        fs::create_directory(isolated / "online-input.json");
        rejects(); fs::remove(isolated / "online-input.json");
        fs::remove(isolated / "bm64_us.z64"); rejects();
        fs::remove(isolated);
    }
    std::string marker;
    std::ifstream(personal / "personal-marker") >> marker;
    assert(marker == "unchanged" && !fs::exists(personal / "child-save"));
    std::cout << "PASS: " << (real_game ? "real game cancellation and two normal matches with matching ready digests" :
        "loading cancellation, acknowledged start/finish, assigned input transfer, crash, failed launch, restart and invalid events")
        << "; isolated settings cleaned; local owner connection retained\n";
}
