#include "recomp_input.h"
#include "recomp_ui.h"
#include "zelda_config.h"
#include "ultramodern/ultramodern.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <limits>

// Only the window/game shell is stubbed. SDL, routing, calibration and config are real.
namespace recomp {
static int test_deadzone = 0;
int get_joystick_deadzone() { return test_deadzone; }
void set_joystick_deadzone(int value) { test_deadzone = value; }
int get_rumble_strength() { return 50; }
int get_scanned_input_index() { return -1; }
void cancel_scanning_input() { stop_scanning_input(); }
}
namespace recompui {
void queue_event(const SDL_Event&) {}
void activate_mouse() {}
void toggle_fullscreen() {}
bool is_context_capturing_input() { return false; }
void drop_files(const std::list<std::filesystem::path>&) {}
}
namespace zelda64 { void open_quit_game_prompt() {} }
namespace ultramodern {
bool is_game_started() { return false; }
void quit() {}
}
bool sdl_event_filter(void*, SDL_Event*);
bool load_controls_config(const std::filesystem::path&);
bool save_controls_config(const std::filesystem::path&);

static int checks = 0;
static void check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << ": " << SDL_GetError() << '\n'; std::exit(1); }
    ++checks;
}
static void pump() {
    SDL_Event event;
    while (SDL_PollEvent(&event)) sdl_event_filter(nullptr, &event);
    recomp::poll_inputs();
}

int main(int argc, char** argv) {
    check(argc == 2, "temporary config directory supplied");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    check(SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0, "SDL init");
    zelda64::reset_input_bindings();
    std::array<SDL_Joystick*, 4> sticks{};
    for (int p = 0; p < 4; ++p) {
        int index = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
            SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
        check(index >= 0, "attach virtual gamepad");
        sticks[p] = SDL_JoystickOpen(index);
        check(sticks[p] != nullptr, "open virtual joystick");
    }
    pump();
    check(recomp::get_connected_controller_names().size() == 4, "four controllers detected");
    for (int p = 0; p < 4; ++p) {
        recomp::set_port_mode(p, recomp::ControllerPortMode::Controller);
        check(recomp::get_port_controller_index(p) == p, "enabling player uses next free controller");
    }
    for (int p = 0; p < 4; ++p) {
        SDL_JoystickSetVirtualButton(sticks[p], SDL_CONTROLLER_BUTTON_A, 1);
        SDL_JoystickSetVirtualAxis(sticks[p], SDL_CONTROLLER_AXIS_LEFTX, 24000);
        pump();
        for (int port = 0; port < 4; ++port) {
            uint16_t buttons; float x, y;
            check(recomp::get_n64_input(port, &buttons, &x, &y), "enabled port responds");
            check(port == p ? buttons == 0x8000 && x > .47f && x < .48f : buttons == 0 && x == 0,
                "button and stick input stay on the assigned player");
        }
        SDL_JoystickSetVirtualButton(sticks[p], SDL_CONTROLLER_BUTTON_A, 0);
        SDL_JoystickSetVirtualAxis(sticks[p], SDL_CONTROLLER_AXIS_LEFTX, 0);
        pump();
    }
    recomp::set_port_mode(1, recomp::ControllerPortMode::Off);
    check(recomp::get_port_controller_index(1) == -1, "disabled player releases controller");
    recomp::set_port_mode(1, recomp::ControllerPortMode::Controller);
    check(recomp::get_port_controller_index(1) == 1 && recomp::get_port_controller_index(0) == 0,
        "reenabling player preserves other assignments");

    recomp::StickCalibration calibration{true, .2f, 0, 1, 1, 1, 1, .1f};
    recomp::set_stick_calibration(1, calibration);
    auto disconnected_id = SDL_JoystickInstanceID(sticks[1]);
    SDL_JoystickClose(sticks[1]);
    check(SDL_JoystickDetachVirtual(1) == 0, "detach second controller");
    pump();
    uint16_t buttons = 1; float x = 1, y = 1;
    check(recomp::get_n64_input(1, &buttons, &x, &y) && buttons == 0 && x == 0 && y == 0,
        "disconnected calibrated controller produces neutral input, not center-offset drift");
    float raw_x, raw_y; int32_t id;
    check(recomp::get_raw_controller_stick(2, &raw_x, &raw_y, &id)
        && id == SDL_JoystickInstanceID(sticks[2]), "disconnect does not move remaining players");
    int replacement = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    sticks[1] = SDL_JoystickOpen(replacement);
    pump();
    check(recomp::get_raw_controller_stick(1, &raw_x, &raw_y, &id) && id != disconnected_id,
        "new controller fills vacant player slot");
    for (int invalid_axis : {0, std::numeric_limits<int>::min(), 1000}) {
        check(recomp::get_input_analog_port({4, invalid_axis}, 0) == 0, "invalid axis binding is neutral");
    }
    check(recomp::get_input_analog_port({999, 0}, 0) == 0, "unknown binding type is neutral");
    buttons = 1; x = y = 1;
    check(!recomp::get_n64_input(4, &buttons, &x, &y) && buttons == 0 && x == 0 && y == 0,
        "invalid port has no input");

    auto path = std::filesystem::path(argv[1]) / "controls.json";
    recomp::set_stick_sensitivity(1, 150);
    recomp::set_input_binding(recomp::GameInput::A, 0, recomp::InputDevice::Controller, {3, SDL_CONTROLLER_BUTTON_Y}, 1);
    check(save_controls_config(path), "save four-player config");
    recomp::set_stick_calibration(1, {});
    recomp::set_stick_sensitivity(1, 100);
    check(load_controls_config(path), "reload four-player config");
    check(recomp::get_stick_sensitivity(1) == 150 && recomp::get_stick_calibration(1).center_x == .2f,
        "sensitivity and calibration survive reload");
    check(recomp::get_input_binding(recomp::GameInput::A, 0, recomp::InputDevice::Controller, 1).input_id == SDL_CONTROLLER_BUTTON_Y,
        "custom per-player binding survives reload");
    auto online_path = std::filesystem::path(argv[1]) / "online-input.json";
    recomp::set_joystick_deadzone(17);
    auto snapshot = zelda64::serialize_online_input();
    { std::ofstream out(online_path); out << snapshot; }
    recomp::set_joystick_deadzone(0);
    recomp::set_stick_sensitivity(1, 100);
    recomp::set_stick_calibration(1, {});
    recomp::set_port_mode(1, recomp::ControllerPortMode::Off);
    zelda64::reset_input_bindings();
    check(zelda64::load_online_input(online_path), "load isolated online input snapshot");
    check(recomp::get_joystick_deadzone() == 17 && recomp::get_stick_sensitivity(1) == 150 &&
          recomp::get_stick_calibration(1).center_x == .2f &&
          recomp::get_port_mode(1) == recomp::ControllerPortMode::Controller,
          "online snapshot restores port mode, calibration, sensitivity and deadzone");
    check(recomp::get_input_binding(recomp::GameInput::A, 0, recomp::InputDevice::Controller, 1).input_id == SDL_CONTROLLER_BUTTON_Y,
          "online snapshot restores custom binding");
    check(zelda64::serialize_online_input() == snapshot, "all four ports round trip without personal file writes");
    for (const char* invalid : {"{", "[]", R"({"controls":{},"joystick_deadzone":-1})",
            R"({"controls":{},"joystick_deadzone":101})", R"({"controls":{},"joystick_deadzone":0.5})",
            R"({"controls":{},"joystick_deadzone":4294967313})",
            R"({"controls":{},"joystick_deadzone":0,"cheats":{}})",
            R"({"controls":{"ports":[{"mode":"Keyboard","stick_sensitivity":[]}]},"joystick_deadzone":0})"}) {
        { std::ofstream out(online_path); out << invalid; }
        check(!zelda64::load_online_input(online_path), "malformed online preferences rejected");
    }
    { std::ofstream out(path); out << R"({"ports":[{"mode":"Keyboard"}],"controller_defaults_version":1})"; }
    check(load_controls_config(path), "load partial config");
    check(recomp::get_port_mode(1) == recomp::ControllerPortMode::Off && !recomp::get_stick_calibration(1).enabled,
        "omitted ports reset stale modes and calibration");
    check(recomp::get_input_binding(recomp::GameInput::A, 0, recomp::InputDevice::Controller, 3).input_type != 0,
        "omitted ports still have usable default bindings");
    { std::ofstream out(path); out << R"({"keyboard":{},"controller":{"A":[{"input_type":3,"input_id":3}]}})"; }
    check(load_controls_config(path), "load old single-player config");
    check(recomp::get_input_binding(recomp::GameInput::A, 0, recomp::InputDevice::Controller, 0).input_id == 3,
        "legacy custom bindings are preserved");
    for (auto joystick : sticks) SDL_JoystickClose(joystick);
    SDL_Quit();
    std::cout << checks << " controller routing and config checks passed.\n";
}
