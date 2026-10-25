#pragma once
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace online {
// Set once by main before runtime/UI threads start. A fresh process releases the
// override; personal settings are never loaded into this disposable match.
inline bool match_settings_locked = false;
inline unsigned human_players = 4;

inline unsigned parse_human_players(std::string_view value) {
    if (value.size() != 1 || value[0] < '2' || value[0] > '4')
        throw std::invalid_argument("online match requires 2, 3, or 4 humans");
    return unsigned(value[0] - '0');
}

inline void filter_match_input(int port, int overlay, uint16_t& buttons, float& x, float& y) {
    if (port < 0 || unsigned(port) >= human_players) {
        buttons = 0; x = y = 0;
    } else if (overlay == 13 || overlay == 17) {
        // P1 confirms the fixed preset; no client edits seats, costumes or rules.
        buttons = port == 0 ? buttons & 0x8000 : 0;
        x = y = 0;
    } else {
        // Online cancellation belongs to the owner; do not enter the ROM pause menu.
        buttons &= ~uint16_t(0x1000);
    }
}
}
