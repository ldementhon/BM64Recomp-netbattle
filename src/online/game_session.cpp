#include "game_session.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace online {
GameSession::~GameSession() { reap(); cleanup(); }

void GameSession::cleanup() {
    channel_ = MatchChannel{};
    ready_.reset();
    inputs_.clear();
    if (!settings_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(settings_, ignored);
        settings_.clear();
    }
    state_ = State::Lobby;
}

int GameSession::reap() {
    if (child_ < 0) return 0;
    // A disposable game has no personal save to flush; do not block the lobby
    // on the runtime's unsupported shutdown path.
    kill(child_, SIGKILL);
    int status = 0;
    while (waitpid(child_, &status, 0) < 0 && errno == EINTR) {}
    child_ = -1;
    return status;
}

void GameSession::start(uint64_t match, const std::filesystem::path& executable,
                        const std::filesystem::path& rom, const std::filesystem::path& log,
                        const std::vector<std::string>& arguments,
                        const std::map<std::string, std::string>& diagnostic_environment,
                        unsigned human_players, unsigned local_slot, const LocalInput& local_input) {
    if (state_ != State::Lobby || child_ >= 0) throw std::logic_error("match already running");
    if (!match || match <= match_) throw std::invalid_argument("match ID must increase");
    if (human_players < 2 || human_players > 4) throw std::invalid_argument("online match requires 2, 3, or 4 humans");
    if (local_slot >= human_players) throw std::invalid_argument("local player is not a human slot");
    if (local_input.port >= 4 || local_input.settings.size() > local_input_limit)
        throw std::invalid_argument("invalid local input settings");
    // Consume IDs even on failed launches: queued events must never become valid later.
    match_ = match;
    auto pattern = (std::filesystem::temp_directory_path() / "bm64-match-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "match settings");
    settings_ = pattern;
    try {
        auto [owner, child] = MatchChannel::pair();
        channel_ = std::move(owner);
        // Keep the source above fd 3, which is the child's fixed IPC descriptor.
        int inherited = fcntl(child.fd(), F_DUPFD_CLOEXEC, 4);
        if (inherited < 0) throw std::system_error(errno, std::generic_category(), "inherit match socket");
        MatchChannel inherited_channel(inherited);
        std::filesystem::copy_file(rom, settings_ / "bm64_us.z64");
        if (!local_input.settings.empty()) {
            std::ofstream output(settings_ / "online-input.json", std::ios::binary);
            output << local_input.settings;
            output.close();
            if (!output) throw std::runtime_error("cannot write local input settings");
        }
        std::map<std::string, std::string> environment;
        for (char** e = environ; *e; ++e) {
            std::string entry(*e);
            auto equal = entry.find('=');
            auto key = entry.substr(0, equal);
            if (equal != std::string::npos && key != "APP_FOLDER_PATH" && !key.starts_with("BM64_"))
                environment[key] = entry.substr(equal + 1);
        }
        for (auto& [key, value] : diagnostic_environment) {
            if (!key.starts_with("BM64_") || key.find('=') != std::string::npos ||
                key.find('\0') != std::string::npos || value.find('\0') != std::string::npos)
                throw std::invalid_argument("invalid diagnostic environment");
            environment[key] = value;
        }
        environment["APP_FOLDER_PATH"] = settings_.string();
        environment["BM64_OWNER_MATCH"] = std::to_string(match);
        environment["BM64_ONLINE_HUMANS"] = std::to_string(human_players);
        environment["BM64_ONLINE_SLOT"] = std::to_string(local_slot);
        environment["BM64_LOCAL_PORT"] = std::to_string(local_input.port);
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

std::optional<GameSession::Exit> GameSession::poll(std::chrono::steady_clock::time_point now) {
    if (child_ < 0) return std::nullopt;
    try {
        for (unsigned received = 0; received < 128; ++received) {
            auto message = channel_.receive();
            if (!message) break;
            if (message->match != match_) throw std::runtime_error("wrong-match child event");
            if (message->event == MatchEvent::Ready && state_ == State::Loading && !ready_) {
                ready_ = *message;
            } else if (message->event == MatchEvent::InputUpload && state_ == State::Playing) {
                if (message->input.slot != local_slot_ || inputs_.size() >= 128)
                    throw std::runtime_error("invalid or excessive game input uploads");
                inputs_.push_back(message->input);
            } else if (message->event == MatchEvent::Finished && state_ == State::Playing &&
                       ready_ && message->frame > ready_->frame) {
                auto status = reap();
                cleanup();
                return Exit{match_, status, false, message->frame, {}};
            } else {
                throw std::runtime_error("unexpected child event");
            }
        }
    } catch (const std::exception& error) {
        auto status = reap();
        cleanup();
        return Exit{match_, status, false, {}, error.what()};
    }
    int status;
    pid_t result;
    do { result = waitpid(child_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "poll game");
    if (result == child_) {
        child_ = -1; cleanup();
        return Exit{match_, status, false, {}, "game exited before match completion"};
    }
    if (state_ == State::Loading && now >= loading_deadline_) {
        reap(); cleanup();
        return Exit{match_, 0, true, {}, "game loading timed out"};
    }
    return std::nullopt;
}
}
