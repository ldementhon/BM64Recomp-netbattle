#pragma once
#include <atomic>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace online {
// Set once by main before runtime/UI threads start. A fresh process releases the
// override; personal settings are never loaded into this disposable match.
inline bool match_settings_locked = false;
inline unsigned human_players = 4;
// The room's pause rule, the same for every player. In battle, the ROM pause
// ignores everything but Start, and any player's Start resumes.
inline bool pausing_allowed = false;
// Set by a match window's quit prompt; its game thread leaves at the next boundary.
inline std::atomic<bool> leave_requested = false;
// Set by the game thread when the relay reports differing states; the match keeps
// running. P1's restart prompt sets restart_requested for the game thread to send.
inline std::atomic<bool> desync_detected = false, restart_requested = false;

inline unsigned parse_human_players(std::string_view value) {
    if (value.size() != 1 || value[0] < '2' || value[0] > '4')
        throw std::invalid_argument("online match requires 2, 3, or 4 humans");
    return unsigned(value[0] - '0');
}

// Stage Select is the only overlay-13 screen after Player Select in the single-battle route.
inline void filter_match_input(int port, int overlay, bool stage_select, uint16_t& buttons, float& x, float& y) {
    if (port < 0 || unsigned(port) >= human_players) {
        buttons = 0; x = y = 0;
    } else if (overlay == 13 && stage_select && port == 0) {
        // P1 chooses the stage: A plus left/right on the stick or D-pad.
        buttons &= 0x8300;
        y = 0;
    } else if (overlay == 13 || overlay == 17) {
        // P1 confirms the fixed preset; no client edits seats, costumes or rules.
        buttons = port == 0 ? buttons & 0x8000 : 0;
        x = y = 0;
    } else if (!pausing_allowed) {
        // Online cancellation belongs to the owner; do not enter the ROM pause menu.
        buttons &= ~uint16_t(0x1000);
    }
}

inline bool parse_switch(std::string_view value) {
    if (value != "0" && value != "1") throw std::invalid_argument("online switch must be 0 or 1");
    return value == "1";
}
inline int parse_volume(std::string_view value) {
    int volume = -1;
    auto end = value.data() + value.size();
    auto parsed = std::from_chars(value.data(), end, volume);
    if (parsed.ec != std::errc{} || parsed.ptr != end || volume < 0 || volume > 100)
        throw std::invalid_argument("online volume must be 0 to 100");
    return volume;
}
}
