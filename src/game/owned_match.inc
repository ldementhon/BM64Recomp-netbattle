// The connection owner survives this process. Native checkpoints and input
// history stay here; only controller records and lifecycle events cross IPC.
struct OwnedMatch {
    online::MatchChannel channel{3};
    uint64_t match = 0, origin = 0, logged = UINT64_MAX;
    unsigned slot = 0, local_port = 0;
    bool started = false, network = std::getenv("BM64_NETWORK_INPUT") != nullptr;
    std::optional<online::RollbackInputs> history;
    std::chrono::steady_clock::time_point epoch, sent{}, progress;
    uint64_t corrections = 0, max_depth = 0;

    explicit OwnedMatch(const char* text) {
        require(online::match_settings_locked, "owned match requires --online-test");
        require(!setting("BM64_CHECKPOINT_FRAME", 0), "owned match cannot use diagnostic restores");
        auto end = text + std::strlen(text);
        auto parsed = std::from_chars(text, end, match);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !match)
            throw std::runtime_error("invalid owner match ID");
        if (fcntl(channel.fd(), F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("missing owner channel");
        if (network) {
            const char* local = std::getenv("BM64_ONLINE_SLOT");
            if (!local || std::strlen(local) != 1 || *local < '0' || unsigned(*local - '0') >= online::human_players)
                throw std::runtime_error("invalid assigned local player");
            slot = unsigned(*local - '0'); local_port = setting("BM64_LOCAL_PORT", 0);
            require(local_port < 4, "invalid physical controller port");
            require(!loop_at && !std::getenv("BM64_LOOP_FRAMES") && !std::getenv("BM64_CATCHUP_FRAMES") &&
                    !std::getenv("BM64_BENCHMARK_FRAMES"), "conflicting network and replay diagnostics");
        }
    }
    void receive() {
        for (unsigned messages = 0; messages < 128; ++messages) {
            auto message = channel.receive();
            if (!message) break;
            if (!history || message->event != online::MatchEvent::InputRelay || message->match != match)
                throw std::runtime_error("unexpected owner command during match");
            auto before = history->confirmed();
            history->receive(message->input);
            if (history->confirmed() > before) progress = std::chrono::steady_clock::now();
        }
    }
    void upload(bool force = false) {
        auto now = std::chrono::steady_clock::now();
        if (force || now - sent >= std::chrono::milliseconds(17)) {
            channel.send(online::input_message(history->upload(), false)); sent = now;
        }
    }
    void finish() {
        channel.send({online::match_channel_version, online::MatchEvent::Finished, match, frame});
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!channel.flush()) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("owner finish delivery timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        fprintf(stderr, "MATCH_FINISHED: match=%llu frame=%llu corrections=%llu max_depth=%llu replay=%u\n", match, frame, corrections, max_depth, replay_pass);
        fflush(stderr); std::_Exit(0);
    }
    void network_boundary() {
        for (;;) {
            receive();
            auto relative = frame - origin;
            require(relative <= history->head(), "network simulation advanced without frame inputs");
            if (auto first = history->take_correction(); first && *first < relative) {
                auto index = 1 + *first % online::checkpoint_slots;
                require(snapshots[index].saved_frame == origin + *first, "network checkpoint was overwritten");
                finish_presentation();
                ++corrections; max_depth = std::max(max_depth, history->head() - *first);
                fprintf(stderr, "NETWORK_ROLLBACK: from=%llu head=%llu confirmed=%llu\n", *first, history->head(), history->confirmed());
                restore(index, true);
                relative = *first;
            }
            // A correction can discover the winner before the old predicted
            // head. All clients must report that same first terminal boundary.
            if (battle_state().complete()) {
                if (relative <= history->confirmed()) {
                    replay_pass = relative < history->head();
                    finish_presentation();
                    fprintf(stderr, "NETWORK_STATE: frame=%llu guest=%llu ram=%016llx registers=%016llx corrections=%llu max_depth=%llu\n",
                            relative, frame, digest().ram, comparable_registers(), corrections, max_depth);
                    finish();
                }
                upload();
                if (std::chrono::steady_clock::now() - progress >= std::chrono::seconds(5))
                    throw std::runtime_error("network completion confirmation timed out");
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            if (relative < history->head()) {
                replay_pass = 1; catchup_active = true;
                suppress_render = relative + 1 < history->head();
                save(1 + relative % online::checkpoint_slots, true);
                network_pads = history->inputs_for(relative);
                upload();
                return;
            }
            replay_pass = 0; catchup_active = false; suppress_render = false;
            auto now = std::chrono::steady_clock::now();
            auto due = epoch + std::chrono::nanoseconds(relative * 1000000000ULL / 60);
            if (now >= due && history->needs_sample()) {
                uint16_t buttons = 0; float x = 0, y = 0;
                if (std::getenv("BM64_INPUT_SCRIPT")) recorded_input(int(slot), frame, &buttons, &x, &y);
                else { recomp::poll_inputs(); recomp::get_n64_input(int(local_port), &buttons, &x, &y); }
                history->sample(online::quantize_pad(buttons, x, y)); upload(true);
            } else upload();
            const auto stop = setting("BM64_NETWORK_STOP", 0);
            const auto interval = setting("BM64_NETWORK_VERIFY_EVERY", 0);
            bool verify = (interval && relative % interval == 0) || (stop && relative == stop);
            bool confirmed = relative <= history->confirmed();
            if (now >= due && confirmed && verify && logged != relative) {
                finish_presentation();
                fprintf(stderr, "NETWORK_STATE: frame=%llu guest=%llu ram=%016llx registers=%016llx corrections=%llu max_depth=%llu\n",
                        relative, frame, digest().ram, comparable_registers(), corrections, max_depth);
                fflush(stderr); logged = relative;
            }
            if (now >= due && history->can_advance() && (!verify || confirmed) && !(stop && relative >= stop)) {
                save(1 + relative % online::checkpoint_slots, true);
                network_pads = history->inputs_for(relative);
                return;
            }
            if ((!stop || relative < stop) && now - progress >= std::chrono::seconds(5))
                throw std::runtime_error("network input progress timed out");
            // Service IPC/retransmits during pacing, prediction limits, verification,
            // and an explicit diagnostic stop. No game frame advances in this loop.
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
};

static bool owned_match_boundary() {
    const char* value = std::getenv("BM64_OWNER_MATCH");
    if (!value) return false;
    try {
        static OwnedMatch owner(value);
        if (!owner.started && player_select_ready && overlay == 13) {
            finish_presentation(); owner.origin = frame;
            if (owner.network) {
                owner.history.emplace(owner.match, online::human_players, owner.slot);
                network_input_active = true;
            }
            online::MatchMessage ready{online::match_channel_version, online::MatchEvent::Ready, owner.match, frame,
                                      digest().ram, comparable_registers()};
            owner.channel.send(ready);
            fprintf(stderr, "MATCH_READY: match=%llu frame=%llu ram=%016llx registers=%016llx\n", owner.match, frame, ready.ram, ready.registers);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!owner.started) {
                if (auto command = owner.channel.receive()) {
                    if (owner.network && command->event == online::MatchEvent::InputRelay && command->match == owner.match) {
                        owner.history->receive(command->input); continue;
                    }
                    if (command->event != online::MatchEvent::Start || command->match != ready.match ||
                        command->frame != ready.frame || command->ram != ready.ram || command->registers != ready.registers)
                        throw std::runtime_error("start does not acknowledge ready state");
                    owner.started = true;
                } else {
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("owner start timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
            }
            owner.epoch = owner.progress = std::chrono::steady_clock::now();
            fprintf(stderr, "MATCH_STARTED: match=%llu frame=%llu network=%d slot=%u\n", owner.match, frame, owner.network, owner.slot);
        }
        if (owner.started) {
            if (owner.network) { owner.network_boundary(); return true; }
            if (owner.channel.receive()) throw std::runtime_error("unexpected owner command during match");
            if (battle_state().complete()) owner.finish();
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "MATCH_ERROR: %s\n", error.what()); fflush(stderr); std::_Exit(2);
    }
    return false;
}
