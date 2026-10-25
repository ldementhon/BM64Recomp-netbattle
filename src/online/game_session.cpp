#include "game_session.hpp"
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <stdexcept>
#include <system_error>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace online {
GameSession::~GameSession() { reap(); cleanup(); }

void GameSession::cleanup() {
    if (!settings_.empty()) {
        std::error_code ignored;
        std::filesystem::remove_all(settings_, ignored);
        settings_.clear();
    }
    state_ = State::Lobby;
}

void GameSession::reap() {
    if (child_ < 0) return;
    // A disposable game has no personal save to flush; do not block the lobby
    // on the runtime's unsupported shutdown path.
    kill(child_, SIGKILL);
    while (waitpid(child_, nullptr, 0) < 0 && errno == EINTR) {}
    child_ = -1;
}

void GameSession::start(uint64_t match, const std::filesystem::path& executable,
                        const std::filesystem::path& rom, const std::filesystem::path& log,
                        const std::vector<std::string>& arguments,
                        const std::map<std::string, std::string>& diagnostic_environment) {
    if (state_ != State::Lobby || child_ >= 0) throw std::logic_error("match already running");
    if (!match || match <= match_) throw std::invalid_argument("match ID must increase");
    // Consume IDs even on failed launches: queued events must never become valid later.
    match_ = match;
    auto pattern = (std::filesystem::temp_directory_path() / "bm64-match-XXXXXX").string();
    if (!mkdtemp(pattern.data())) throw std::system_error(errno, std::generic_category(), "match settings");
    settings_ = pattern;
    try {
        std::filesystem::copy_file(rom, settings_ / "bm64_us.z64");
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
        error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(),
                                                O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
        if (!error) error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
        if (!error) error = posix_spawn(&child_, argv[0], &actions, nullptr, argv.data(), env.data());
        posix_spawn_file_actions_destroy(&actions);
        if (error) { child_ = -1; throw std::system_error(error, std::generic_category(), "launch game"); }
        state_ = State::Loading;
        loading_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    } catch (...) { reap(); cleanup(); throw; }
}

bool GameSession::playing(uint64_t match) {
    if (match != match_ || state_ != State::Loading) return false;
    // The caller must validate the scene/configuration acknowledgment first.
    state_ = State::Playing;
    return true;
}

bool GameSession::cancel(uint64_t match) {
    if (match != match_ || state_ == State::Lobby) return false;
    reap(); cleanup();
    return true;
}

std::optional<GameSession::Exit> GameSession::poll(std::chrono::steady_clock::time_point now) {
    if (child_ < 0) return std::nullopt;
    int status;
    pid_t result;
    do { result = waitpid(child_, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result < 0) throw std::system_error(errno, std::generic_category(), "poll game");
    if (result == child_) {
        child_ = -1; cleanup();
        return Exit{match_, status, false};
    }
    if (state_ == State::Loading && now >= loading_deadline_) {
        reap(); cleanup();
        return Exit{match_, 0, true};
    }
    return std::nullopt;
}
}
