// The connection owner survives this process. Native checkpoints and input
// history stay here; only controller records and lifecycle events cross IPC.
struct OwnedMatch {
    online::MatchChannel channel = online::MatchChannel::inherited();
    uint64_t match = 0, origin = 0;
    unsigned slot = 0, local_port = 0;
    bool started = false, network = std::getenv("BM64_NETWORK_INPUT") != nullptr;
    std::optional<online::RollbackInputs> history;
    // Full-RAM checks every check_interval updates. Each check frame gets its own
    // image, sealed once the inputs before it are confirmed and then hashed on
    // another thread. The relay compares one report at a time.
    struct Check {
        uint64_t relative, guest, registers = 0; int32_t overlay = 0;
        std::unique_ptr<CheckpointMemory> image; bool sealed = false;
        std::optional<StateHash> hash;
    };
    std::deque<Check> checks;
    // Boundary N depends only on inputs before N, so a later correction at input N
    // replays an unchanged sealed state; replays at or before this frame are skipped.
    std::optional<uint64_t> sealed_through;
    // Images awaiting a hash; the game thread waits only past this backlog.
    static constexpr size_t image_limit = 4;
    std::future<StateHash> hashing;
    uint64_t hashing_frame = 0;
    bool reported = false, stop_checked = false;
    unsigned check_interval = online::state_check_interval;
    std::chrono::steady_clock::time_point epoch, sent{}, progress;
    uint64_t corrections = 0, max_depth = 0;
    // Test hooks: replay onto check frame test_replay once its hash has started and
    // again once acknowledged; delay every background hash by test_hash_delay ms.
    uint64_t test_replay = 0; unsigned test_replays = 0, test_hash_delay = 0; bool test_sealed = false;

    explicit OwnedMatch(const char* text) {
        require(online::match_settings_locked, "owned match requires --online-test");
        require(!setting("BM64_CHECKPOINT_FRAME", 0), "owned match cannot use diagnostic restores");
        require(!std::getenv("BM64_RENDER_TO_RAM"), "owned match requires display-only rendering");
        auto end = text + std::strlen(text);
        auto parsed = std::from_chars(text, end, match);
        if (parsed.ec != std::errc{} || parsed.ptr != end || !match)
            throw std::runtime_error("invalid owner match ID");
        if (network) {
            check_interval = setting("BM64_NETWORK_VERIFY_EVERY", online::state_check_interval);
            test_replay = setting("BM64_NETWORK_TEST_REPLAY", 0); test_hash_delay = setting("BM64_NETWORK_TEST_HASH_DELAY_MS", 0);
            require(!test_replay || test_replay % check_interval == 0, "test replay must target a check frame");
            require(check_interval && check_interval <= online::state_check_interval, "invalid network state-check interval");
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
            if ((message->event == online::MatchEvent::StateChecked || message->event == online::MatchEvent::Desync) &&
                message->match == match && reported && message->frame == checks.front().guest) {
                if (message->event == online::MatchEvent::Desync) desync(checks.front());
                checks.pop_front(); reported = false;
                continue;
            }
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
    void report(online::MatchEvent event) {
        channel.send({online::match_channel_version, event, match, frame});
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!channel.flush()) {
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("owner event delivery timed out");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    void leave() {
        report(online::MatchEvent::Left);
        fprintf(stderr, "MATCH_LEFT: match=%llu frame=%llu\n", match, frame);
        fflush(stderr); std::_Exit(0);
    }
    void finish() {
        report(online::MatchEvent::Finished);
        fprintf(stderr, "MATCH_FINISHED: match=%llu frame=%llu corrections=%llu max_depth=%llu replay=%u\n", match, frame, corrections, max_depth, replay_pass);
        fflush(stderr); std::_Exit(0);
    }
    void desync(const Check& check) {
        fprintf(stderr, "NETWORK_DESYNC: frame=%llu guest=%llu ram=%016llx registers=%016llx\n",
                check.relative, check.guest, check.hash->ram, check.registers);
        // Nonzero sections only; compare them across player logs to find what differs.
        fprintf(stderr, "NETWORK_SECTIONS: frame=%llu size=%zu", check.relative, check_section);
        for (size_t i = 0; i < check.hash->sections.size(); ++i)
            if (check.hash->sections[i] != zero_section) fprintf(stderr, " %zu:%016llx", i, check.hash->sections[i]);
        fprintf(stderr, "\n"); fflush(stderr);
        if (!online::desync_detected.exchange(true)) zelda64::show_online_desync(slot == 0);
    }
    // Capture a check frame when it is first simulated; replays before sealing capture it again.
    void record(uint64_t relative) {
        if (relative % check_interval || (sealed_through && relative <= *sealed_through)) return;
        Check* check = nullptr;
        for (auto& existing : checks) if (existing.relative == relative) check = &existing;
        if (!check) {
            auto images = [&] { return std::count_if(checks.begin(), checks.end(), [](const Check& c) { return c.image != nullptr; }); };
            if (size_t(images()) >= image_limit && hashing.valid()) {
                auto start = std::chrono::steady_clock::now();
                collect(true);
                fprintf(stderr, "NETWORK_CHECK_WAIT: ms=%.3f\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
            }
            require(size_t(images()) < image_limit, "state-check images were not confirmed");
            require(checks.size() < 8, "state checks stopped receiving acknowledgments");
            check = &checks.emplace_back(Check{relative, frame});
            check->image = std::make_unique<CheckpointMemory>();
        }
        require(!check->sealed, "a sealed state check was recaptured");
        check->registers = comparable_registers(); check->overlay = overlay;
        check->image->capture(ram, recomp::mem_size, std::getenv("BM64_FULL_COPY") != nullptr);
    }
    void collect(bool wait) {
        if (!hashing.valid() || (!wait && hashing.wait_for(std::chrono::seconds(0)) != std::future_status::ready)) return;
        auto hash = hashing.get();
        for (auto& check : checks) if (check.relative == hashing_frame) {
            check.image.reset(); finish(check, std::move(hash)); return;
        }
        require(false, "hashed state check is missing");
    }
    void finish(Check& check, StateHash hash) {
        check.hash = std::move(hash);
        fprintf(stderr, "NETWORK_STATE: frame=%llu guest=%llu ram=%016llx registers=%016llx corrections=%llu max_depth=%llu overlay=%d\n",
                check.relative, check.guest, check.hash->ram, check.registers, corrections, max_depth, check.overlay);
        fprintf(stderr, "NETWORK_CHECK_COST: frame=%llu ms=%.3f\n", check.relative, check.hash->ms);
        fflush(stderr);
    }
    // Seal checks whose earlier inputs are confirmed, hash the oldest sealed one,
    // then report it once the previous report is acknowledged.
    void check_states(uint64_t relative) {
        collect(false);
        for (auto& check : checks) {
            if (!check.sealed && check.image && check.relative < relative && check.relative <= history->confirmed()) {
                check.sealed = true; sealed_through = check.relative;
            }
        }
        for (auto& check : checks) {
            if (hashing.valid() || !check.sealed || !check.image) continue;
            if (check.relative == test_replay) test_sealed = true;
            hashing_frame = check.relative;
            hashing = std::async(std::launch::async, [this, image = check.image.get()] {
                if (test_hash_delay) std::this_thread::sleep_for(std::chrono::milliseconds(test_hash_delay));
                return state_hash(*image);
            });
        }
        // Startup was already compared through Ready.
        if (!checks.empty() && checks.front().hash && !checks.front().relative) checks.pop_front();
        if (!reported && !checks.empty() && checks.front().hash) {
            const auto& check = checks.front();
            channel.send({online::match_channel_version, online::MatchEvent::State, match, check.guest, check.hash->ram, check.registers});
            reported = true;
        }
    }
    // A late input exactly at check frame N replays boundary N after its check was
    // sealed. Force that replay deterministically, with the same inputs.
    std::optional<uint64_t> forced_replay(uint64_t relative) {
        if (!test_replay || test_replays >= 2 || relative <= test_replay ||
            history->head() - test_replay > online::prediction_limit ||
            snapshots[1 + test_replay % online::checkpoint_slots].saved_frame != origin + test_replay) return std::nullopt;
        bool pending = std::any_of(checks.begin(), checks.end(), [&](const Check& c) { return c.relative == test_replay; });
        if (test_replays == 0 ? !test_sealed : pending) return std::nullopt;
        fprintf(stderr, "NETWORK_TEST_REPLAY: frame=%llu after=%s head=%llu\n", test_replay,
                test_replays++ ? "acknowledged" : "sealed", history->head());
        return test_replay;
    }
    void network_boundary() {
        for (;;) {
            if (online::leave_requested) leave();
            if (online::restart_requested.exchange(false)) report(online::MatchEvent::Restart);
            receive();
            auto relative = frame - origin;
            require(relative <= history->head(), "network simulation advanced without frame inputs");
            auto first = history->take_correction();
            bool forced = !first && (first = forced_replay(relative));
            if (first && *first < relative) {
                auto index = 1 + *first % online::checkpoint_slots;
                require(snapshots[index].saved_frame == origin + *first, "network checkpoint was overwritten");
                finish_presentation();
                if (!forced) ++corrections;
                max_depth = std::max(max_depth, history->head() - *first);
                fprintf(stderr, "NETWORK_ROLLBACK: from=%llu head=%llu confirmed=%llu\n", *first, history->head(), history->confirmed());
                restore(index, true);
                relative = *first;
            }
            check_states(relative);
            // A win does not end an online match: P1 continues through the award
            // screens to Player Select and the next battle until a player leaves.
            if (relative < history->head()) {
                replay_pass = 1; catchup_active = true; audio_output = false;
                suppress_render = relative + 1 < history->head();
                save(1 + relative % online::checkpoint_slots, true); record(relative);
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
                else {
                    require(recomp::online_input_ready(int(local_port)), "selected controller is unavailable or ambiguous; return to the lobby and reconnect");
                    recomp::poll_inputs(); recomp::get_n64_input(int(local_port), &buttons, &x, &y);
                }
                history->sample(online::quantize_pad(buttons, x, y)); upload(true);
            } else upload();
            const auto stop = setting("BM64_NETWORK_STOP", 0);
            // A diagnostic stop never saves its frame, so check the live state there.
            if (stop && relative == stop && !stop_checked && relative <= history->confirmed() &&
                std::all_of(checks.begin(), checks.end(), [](const Check& c) { return c.hash.has_value(); })) {
                stop_checked = true; finish_presentation();
                auto& check = checks.emplace_back(Check{relative, frame, comparable_registers(), overlay});
                finish(check, live_state_hash());
            }
            if (now >= due && history->can_advance() && !(stop && relative >= stop)) {
                save(1 + relative % online::checkpoint_slots, true); record(relative);
                // Play the first simulation of each boundary; corrected replays stay silent.
                audio_output = true;
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
        if (online::leave_requested) owner.leave();
        if (!owner.started && player_select_ready && overlay == 13) {
            if (owner.network && !std::getenv("BM64_INPUT_SCRIPT"))
                require(recomp::online_input_ready(int(owner.local_port)), "selected controller is unavailable or ambiguous; return to the lobby and reconnect");
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
                    if (online::leave_requested) owner.leave();
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
