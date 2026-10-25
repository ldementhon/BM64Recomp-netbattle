// Scripted lifecycle proof. Live networking must gate completion on its confirmed
// frontier; these diagnostic inputs are all known before the match starts.
static void owned_match_boundary() {
    const char* value = std::getenv("BM64_OWNER_MATCH");
    if (!value) return;
    try {
        struct Owner {
            online::MatchChannel channel{3};
            uint64_t match = 0;
            bool started = false;
            explicit Owner(const char* text) {
                require(online::match_settings_locked, "owned match requires --online-test");
                require(!setting("BM64_CHECKPOINT_FRAME", 0), "owned match cannot use diagnostic restores");
                auto end = text + std::strlen(text);
                auto parsed = std::from_chars(text, end, match);
                if (parsed.ec != std::errc{} || parsed.ptr != end || !match)
                    throw std::runtime_error("invalid owner match ID");
                if (fcntl(channel.fd(), F_SETFD, FD_CLOEXEC) < 0)
                    throw std::runtime_error("missing owner channel");
            }
        };
        static Owner owner(value);
        if (!owner.started && player_select_ready && overlay == 13) {
            finish_presentation();
            online::MatchMessage ready{online::match_channel_version, online::MatchEvent::Ready, owner.match, frame,
                                      digest().ram, comparable_registers()};
            owner.channel.send(ready);
            fprintf(stderr, "MATCH_READY: match=%llu frame=%llu ram=%016llx registers=%016llx\n",
                    owner.match, frame, ready.ram, ready.registers);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            while (!owner.started) {
                if (auto command = owner.channel.receive()) {
                    if (command->event != online::MatchEvent::Start || command->match != ready.match ||
                        command->frame != ready.frame || command->ram != ready.ram || command->registers != ready.registers)
                        throw std::runtime_error("start does not acknowledge ready state");
                    owner.started = true;
                } else {
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("owner start timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            fprintf(stderr, "MATCH_STARTED: match=%llu frame=%llu\n", owner.match, frame);
        }
        if (owner.started) {
            if (owner.channel.receive()) throw std::runtime_error("unexpected owner command during match");
            if (battle_state().complete()) {
                owner.channel.send({online::match_channel_version, online::MatchEvent::Finished, owner.match, frame});
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while (!owner.channel.flush()) {
                    if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("owner finish delivery timed out");
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                fprintf(stderr, "MATCH_FINISHED: match=%llu frame=%llu\n", owner.match, frame);
                fflush(stderr);
                std::_Exit(0);
            }
        }
    } catch (const std::exception& error) {
        fprintf(stderr, "MATCH_ERROR: %s\n", error.what());
        fflush(stderr);
        std::_Exit(2);
    }
}
