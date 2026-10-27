#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include "../src/online/match_settings.hpp"
#include "../src/online/input_history.hpp"

namespace checkpoint {
bool enabled = true;
std::atomic<bool> fast_boot = false;
std::atomic<uint64_t> boot_frame = 0;
static uint64_t frame = 0;
static int overlay = 86;
static bool player_select_passed = false;
static unsigned replay_pass = 0;
static void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
#include "../src/game/checkpoint_input.inc"
}

static unsigned checks;
static void check(bool ok, const char* message) {
    ++checks;
    if (!ok) throw std::runtime_error(message);
}
struct Pad { uint16_t buttons; float x, y; };
static Pad scripted(uint64_t frame, int port) {
    if (frame < 20 || frame >= 500) return {};
    return {uint16_t(frame * 4 + port), float(int(frame % 5) - 2) / 4, float(port - 1) / 4};
}
static void read(int port, Pad expected) {
    uint16_t buttons = 0xffff; float x = 9, y = 9;
    check(checkpoint::input(port, &buttons, &x, &y), "connected port did not respond");
    check(buttons == expected.buttons && x == expected.x && y == expected.y,
          "wrong input for simulation frame/port");
}
static void read_frame(uint64_t f, bool predicted) {
    using namespace checkpoint;
    frame = f;
    auto boot = boot_frame.load();
    // Gameplay and cheats can read every port repeatedly, in different orders.
    for (int repeat = 0; repeat < 3; ++repeat) for (int port : {3, 0, 2, 1}) {
        auto sample = f;
        if (loop_at && f >= loop_at) {
            sample -= loop_buffer;
            auto begin = pending_begin(f);
            if (predicted && port != 0 && begin) sample = begin - loop_buffer - 1;
        }
        read(port, scripted(sample, port));
        check(frame == f && boot_frame == boot, "controller read advanced a clock");
    }
}
int main(int argc, char** argv) {
    try {
        check(argc == 2, "expected temporary fixture path");
        {
            std::ofstream file(argv[1]);
            for (uint64_t f = 20; f < 500; ++f) for (int port = 0; port < 4; ++port) {
                auto pad = scripted(f, port);
                file << std::dec << f << ' ' << f+1 << ' ' << std::hex << pad.buttons
                     << std::dec << ' ' << pad.x << ' ' << pad.y << ' ' << port << '\n';
            }
            check(file.good(), "write fixture");
        }
        setenv("BM64_INPUT_SCRIPT", argv[1], 1);
        unsetenv("BM64_NEGATIVE_INPUT");
        using namespace checkpoint;
        for (uint64_t f : {0, 19, 20, 21, 499, 500}) read_frame(f, false);
        loop_at = 100; loop_end = 600;
        for (unsigned delay : {0, 2, 4}) for (unsigned length : {4, 8, 12}) {
            loop_buffer = delay; loop_length = length;
            auto first = loop_at + loop_warmup + loop_lead;
            for (auto f : {loop_at-1, loop_at, first-1, first, first+length-1,
                           first+length, first+loop_spacing}) read_frame(f, true);
            // Rewind to the exact same frame with corrected inputs on all three
            // remote ports; then replay several historical frames and resume.
            read_frame(first + length, false);
            replay_pass = 1;
            for (uint64_t f = first; f <= first+length; ++f) read_frame(f, false);
            replay_pass = 0;
            read_frame(first+length, false);
            loop_oracle = true;
            read_frame(first, false);
            loop_oracle = false;
        }
        auto first = loop_at + loop_warmup + loop_lead;
        for (unsigned length : {4, 8, 12}) {
            loop_length = length;
            for (auto end : {first+length-1, first+length}) {
                loop_end = end;
                check(pending_begin(first) == 0, "run ends with an uncorrected prediction");
                read_frame(first, false);
            }
            loop_end = first+length+1;
            check(pending_begin(first) == first, "complete correction window was skipped");
            read_frame(first, true);
            check(pending_begin(loop_end-1) == 0, "last update still predicts inputs");
            read_frame(loop_end-1, false);
        }
        loop_length = 0;
        read_frame(300, false);
        loop_at = 0;
        fast_boot = true; boot_frame = 500;
        for (int repeat = 0; repeat < 3; ++repeat) {
            read(0, {0x1000, 0, 0});
            for (int port = 1; port < 4; ++port) read(port, {});
            check(boot_frame == 500, "boot input advanced on repeated reads");
        }
        for (bool boot : {false, true}) {
            fast_boot = boot;
            for (int port : {-1, 4}) {
                uint16_t b; float x, y;
                check(!input(port, &b, &x, &y), "invalid port responded");
            }
        }
        fast_boot = false; network_input_active = true; frame = 7777; boot_frame = 999;
        online::match_settings_locked = true; online::human_players = 4;
        for (int port = 0; port < 4; ++port) network_pads[port] = {uint16_t(0x8000 | (1 << port)), int8_t(port * 31 - 60), int8_t(50 - port * 29)};
        for (int scene : {86, 13, 17}) for (int pass : {0, 1}) for (int repeat = 0; repeat < 3; ++repeat) {
            overlay = scene; replay_pass = unsigned(pass);
            for (int port = 0; port < 4; ++port) {
                uint16_t buttons; float x, y;
                check(input(port, &buttons, &x, &y), "latched network port disconnected");
                auto expected = scene == 86 ? network_pads[port] : online::Pad{uint16_t(port ? 0 : 0x8000), 0, 0};
                check(buttons == expected.buttons && int8_t(127 * x) == expected.x && int8_t(127 * y) == expected.y,
                      "network read mixed slots, changed quantization, or bypassed menu policy");
            }
            check(frame == 7777 && boot_frame == 999, "network controller read advanced a clock");
        }
        std::printf("PASS: %u replay input timing checks (four ports, repeated reads, delays, rewinds, boot, network latches)\n", checks);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
