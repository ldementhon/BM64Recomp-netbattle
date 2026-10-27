#include "../src/online/match_settings.hpp"
#include <cassert>
#include <iostream>

int main() {
    for (auto bad : {"", "1", "5", "02", "+2", " 2", "2x", "4294967296"}) {
        try { online::parse_human_players(bad); assert(false); }
        catch (const std::invalid_argument&) {}
    }
    for (auto text : {"2", "3", "4"}) {
        online::human_players = online::parse_human_players(text);
        for (bool stage : {false, true}) for (int scene : {13, 17, 86}) for (int port = 0; port < 4; ++port) {
            uint16_t buttons = 0xffff; float x = .7f, y = -.4f;
            online::filter_match_input(port, scene, stage, buttons, x, y);
            const bool human = unsigned(port) < online::human_players;
            if (human && port == 0 && scene == 13 && stage) {
                assert(buttons == 0x8300 && x == .7f && y == 0);
            } else if (!human || scene != 86) {
                assert(x == 0 && y == 0);
                assert(buttons == (human && port == 0 && scene != 86 ? 0x8000 : 0));
            } else {
                assert(buttons == 0xefff && x == .7f && y == -.4f);
            }
            auto old_buttons = buttons; auto old_x = x; auto old_y = y;
            online::filter_match_input(port, scene, stage, buttons, x, y);
            assert(buttons == old_buttons && x == old_x && y == old_y);
        }
    }
    std::cout << "PASS: 2/3/4-human layout validation, CPU neutral inputs, P1 menus and stage choice, and stable gameplay reads\n";
}
