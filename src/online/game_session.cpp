#include "game_session.hpp"
#include "json/json.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#include "windows_game_process.hpp"
#else
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace online {
GameSession::~GameSession() { reap(); cleanup(); }

void GameSession::cleanup() {
    channel_ = MatchChannel{};
    ready_.reset();
    inputs_.clear();
    states_.clear(); pending_state_.reset(); restart_ = false;
    if (!settings_.empty()) {
        // The game's menu writes sound.json; keep its main volume for the next match.
        std::ifstream sound(settings_ / "sound.json", std::ios::binary);
        auto json = nlohmann::json::parse(sound, nullptr, false);
        if (json.is_object() && json.contains("main_volume") && json["main_volume"].is_number_integer()) {
            auto volume = json["main_volume"].get<int64_t>();
            if (volume >= 0 && volume <= 100) volume_ = int(volume);
        }
        sound.close();
        std::error_code ignored;
        std::filesystem::remove_all(settings_, ignored);
        settings_.clear();
    }
    state_ = State::Lobby;
}

int GameSession::reap() {
    if (child_ == no_child) return 0;
    // A disposable game has no personal save to flush; do not block the lobby
    // on the runtime's unsupported shutdown path.
#ifdef _WIN32
    TerminateProcess(child_, 1);
    WaitForSingleObject(child_, INFINITE);
    DWORD status = 0;
    GetExitCodeProcess(child_, &status);
    CloseHandle(child_);
    CloseHandle(job_); job_ = nullptr;
#else
    kill(child_, SIGKILL);
    int status = 0;
    while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
#endif
    child_ = no_child;
    return int(status);
}

std::optional<int> GameSession::poll_child() {
#ifdef _WIN32
    auto result = WaitForSingleObject(child_, 0);
    if (result == WAIT_TIMEOUT) return {};
    if (result != WAIT_OBJECT_0) detail::windows_fail("poll game");
    DWORD status;
    if (!GetExitCodeProcess(child_, &status)) detail::windows_fail("read game exit status");
    CloseHandle(child_);
    CloseHandle(job_); job_ = nullptr;
#else
    int status;
    pid_t result;
    do { result = waitpid(child_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "poll game");
    if (!result) return {};
#endif
    child_ = no_child;
    return int(status);
}

void GameSession::start(uint64_t match, const std::filesystem::path& executable,
                        const std::filesystem::path& rom, const std::filesystem::path& log,
                        const std::vector<std::string>& arguments,
                        const std::map<std::string, std::string>& diagnostic_environment,
                        unsigned human_players, unsigned local_slot, const LocalInput& local_input,
                        const MatchOptions& options) {
    if (state_ != State::Lobby || child_ != no_child) throw std::logic_error("match already running");
    if (!match || match <= match_) throw std::invalid_argument("match ID must increase");
    if (human_players < 2 || human_players > 4) throw std::invalid_argument("online match requires 2, 3, or 4 humans");
    if (local_slot >= human_players) throw std::invalid_argument("local player is not a human slot");
    if (local_input.port >= 4 || local_input.settings.size() > local_input_limit)
        throw std::invalid_argument("invalid local input settings");
    if (options.volume < 0 || options.volume > 100) throw std::invalid_argument("invalid match volume");
    // Consume IDs even on failed launches: queued events must never become valid later.
    match_ = match;
#ifdef _WIN32
    settings_ = detail::make_match_directory();
#else
    auto pattern = (std::filesystem::temp_directory_path() / "bm64-match-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "match settings");
    settings_ = pattern;
#endif
    try {
        auto [owner, child] = MatchChannel::pair();
        channel_ = std::move(owner);
#ifndef _WIN32
        // Keep the source above fd 3, which is the child's fixed IPC descriptor.
        int inherited = fcntl(child.fd(), F_DUPFD_CLOEXEC, 4);
        if (inherited < 0) throw std::system_error(errno, std::generic_category(), "inherit match socket");
        MatchChannel inherited_channel(inherited);
#endif
        std::filesystem::copy_file(rom, settings_ / "bm64_us.z64");
        if (!local_input.settings.empty()) {
            std::ofstream output(settings_ / "online-input.json", std::ios::binary);
            output << local_input.settings;
            output.close();
            if (!output) throw std::runtime_error("cannot write local input settings");
        }
        std::map<std::string, std::string> environment;
#ifndef _WIN32
        for (char** e = environ; *e; ++e) {
            std::string entry(*e);
            auto equal = entry.find('=');
            auto key = entry.substr(0, equal);
            if (equal != std::string::npos && key != "APP_FOLDER_PATH" && !key.starts_with("BM64_"))
                environment[key] = entry.substr(equal + 1);
        }
#endif
        for (auto& [key, value] : diagnostic_environment) {
            if (!key.starts_with("BM64_") || key.find('=') != std::string::npos ||
                key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
                throw std::invalid_argument("invalid diagnostic environment");
            auto name = key;
#ifdef _WIN32
            for (auto& ch : name) if (ch >= 'a' && ch <= 'z') ch += 'A' - 'a';
#endif
            environment[name] = value;
        }
#ifndef _WIN32
        environment["APP_FOLDER_PATH"] = settings_.string();
#endif
        environment["BM64_OWNER_MATCH"] = std::to_string(match);
        environment["BM64_ONLINE_HUMANS"] = std::to_string(human_players);
        environment["BM64_ONLINE_SLOT"] = std::to_string(local_slot);
        environment["BM64_LOCAL_PORT"] = std::to_string(local_input.port);
        environment["BM64_ONLINE_PAUSE"] = options.pausing ? "1" : "0";
        environment["BM64_MAIN_VOLUME"] = std::to_string(options.volume);
#ifdef _WIN32
        auto process = detail::launch_windows_game(executable, log, settings_, child.fd(), arguments, environment);
        child_ = process.process; job_ = process.job;
#else
        std::vector<std::string> env_strings;
        for (auto& [key, value] : environment) env_strings.push_back(key + "=" + value);
        std::vector<char*> env;
        for (auto& value : env_strings) env.push_back(value.data());
        env.push_back(nullptr);
        std::vector<std::string> args{std::filesystem::absolute(executable).string()};
        args.insert(args.end(), arguments.begin(), arguments.end());
        std::vector<char*> argv;
        for (auto& arg : args) {
            if (arg.find('\0') != std::string::npos) throw std::invalid_argument("invalid argument");
            argv.push_back(arg.data());
        }
        argv.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        int error = posix_spawn_file_actions_init(&actions);
        if (error) throw std::system_error(error, std::generic_category(), "spawn actions");
        error = posix_spawn_file_actions_adddup2(&actions, inherited_channel.fd(), 3);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                                O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!error) error = posix_spawn(&child_, argv[0], &actions, nullptr, argv.data(), env.data());
        posix_spawn_file_actions_destroy(&actions);
        if (error) { child_ = -1; throw std::system_error(error, std::generic_category(), "launch game"); }
#endif
        state_ = State::Loading;
        humans_ = human_players; local_slot_ = local_slot;
        loading_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    } catch (...) { reap(); cleanup(); throw; }
}

bool GameSession::playing(uint64_t match) {
    if (match != match_ || state_ != State::Loading || !ready_) return false;
    // A network lobby must compare all clients' Ready digests before calling this.
    auto message = *ready_;
    message.event = MatchEvent::Start;
    channel_.send(message);
    state_ = State::Playing;
    return true;
}

bool GameSession::cancel(uint64_t match) {
    if (match != match_ || state_ == State::Lobby) return false;
    reap(); cleanup();
    return true;
}

bool GameSession::send_inputs(const InputBatch& batch) {
    if (state_ == State::Lobby || batch.match != match_) return false;
    if (!ready_ || batch.slot >= humans_) throw std::logic_error("input cannot reach this game session");
    validate_batch(batch);
    channel_.send(input_message(batch, true));
    return true;
}

bool GameSession::state_checked(uint64_t frame, bool desync) {
    if (state_ != State::Playing) return false;
    if (pending_state_ != frame) throw std::runtime_error("unexpected state-check acknowledgment");
    channel_.send({match_channel_version, desync ? MatchEvent::Desync : MatchEvent::StateChecked, match_, frame});
    pending_state_.reset();
    return true;
}

std::optional<GameSession::Exit> GameSession::poll(std::chrono::steady_clock::time_point now) {
    if (child_ == no_child) return std::nullopt;
    try {
        for (unsigned received = 0; received < 128; ++received) {
            auto message = channel_.receive();
            if (!message) break;
            if (message->match != match_) throw std::runtime_error("wrong-match child event");
            if (message->event == MatchEvent::Ready && state_ == State::Loading && !ready_) {
                ready_ = *message;
            } else if (message->event == MatchEvent::InputUpload && state_ == State::Playing) {
                if (message->input.slot != local_slot_) throw std::runtime_error("invalid game input upload");
                // Each upload carries the newest inputs and covers the whole lead bound, so only the
                // latest is worth sending. After a stall this avoids a burst to the relay.
                inputs_.assign(1, message->input);
            } else if (message->event == MatchEvent::State && state_ == State::Playing && ready_ &&
                       message->frame > ready_->frame && !pending_state_ && states_.empty()) {
                states_.push_back(*message); pending_state_ = message->frame;
            } else if (message->event == MatchEvent::Finished && state_ == State::Playing &&
                       ready_ && message->frame > ready_->frame) {
                auto status = reap();
                cleanup();
                return Exit{match_, status, false, message->frame, {}};
            } else if (message->event == MatchEvent::Restart && state_ == State::Playing && local_slot_ == 0) {
                restart_ = true;
            } else if (message->event == MatchEvent::Left && state_ != State::Lobby) {
                auto status = reap();
                cleanup();
                return Exit{match_, status, false, {}, {}, true};
            } else {
                throw std::runtime_error("unexpected child event");
            }
        }
    } catch (const std::exception& error) {
        auto status = reap();
        cleanup();
        return Exit{match_, status, false, {}, error.what()};
    }
    if (auto status = poll_child()) {
        cleanup();
        return Exit{match_, *status, false, {}, "game exited before match completion"};
    }
    if (state_ == State::Loading && now >= loading_deadline_) {
        reap(); cleanup();
        return Exit{match_, 0, true, {}, "game loading timed out"};
    }
    return std::nullopt;
}
}
