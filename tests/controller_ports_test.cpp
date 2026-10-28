#include "recomp_input.h"
#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_sound.h"
#include "zelda_cheats.h"
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
static int queued_controller_events = 0;
namespace recompui {
void queue_event(const SDL_Event& event) {
    if (event.type == SDL_CONTROLLERBUTTONDOWN || event.type == SDL_CONTROLLERAXISMOTION) ++queued_controller_events;
}
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
#ifdef _WIN32
// COFF linking also resolves unused shell functions. Fail if this focused test
// accidentally calls one, rather than supplying fake sound/graphics behavior.
#define UNUSED_SETTING(type, name) type get_##name() { std::abort(); } void set_##name(type) { std::abort(); }
namespace recomp {
UNUSED_SETTING(int, mouse_sensitivity)
UNUSED_SETTING(int, gyro_sensitivity)
UNUSED_SETTING(BackgroundInputMode, background_input_mode)
void set_rumble_strength(int) { std::abort(); }
}
namespace recompui { void process_game_started() { std::abort(); } }
namespace ultramodern::renderer {
const GraphicsConfig& get_graphics_config() { std::abort(); }
void set_graphics_config(const GraphicsConfig&) { std::abort(); }
}
namespace zelda64 {
UNUSED_SETTING(TargetingMode, targeting_mode)
UNUSED_SETTING(FilmGrainMode, film_grain_mode)
UNUSED_SETTING(RadioBoxMode, radio_comm_box_mode)
UNUSED_SETTING(AimInvertMode, invert_y_axis_mode)
UNUSED_SETTING(AimInvertMode, analog_camera_invert_mode)
UNUSED_SETTING(bool, debug_mode_enabled)
UNUSED_SETTING(int, main_volume)
UNUSED_SETTING(int, bgm_volume)
UNUSED_SETTING(int, sfx_volume)
UNUSED_SETTING(int, voice_volume)
UNUSED_SETTING(bool, low_health_beeps_enabled)
void reset_sound_settings() { std::abort(); }
UNUSED_SETTING(GenericOnOffOption, moonjump)
UNUSED_SETTING(GenericOnOffOption, always_max_bomb)
UNUSED_SETTING(GenericOnOffOption, always_max_fire)
UNUSED_SETTING(GenericOnOffOption, always_have_remote_bombs)
UNUSED_SETTING(GenericOnOffOption, always_have_red_bombs)
UNUSED_SETTING(GenericOnOffOption, infinite_credits)
UNUSED_SETTING(GenericOnOffOption, infinite_lives)
void reset_cheats_settings() { std::abort(); }
}
#undef UNUSED_SETTING
#endif
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

static int attach_pad(int model) {
    static const char* names[] = {"Fixture Pad A", "Fixture Pad B", "Fixture Pad C", "Fixture Pad D"};
    SDL_VirtualJoystickDesc descriptor{};
    descriptor.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    descriptor.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    descriptor.naxes = SDL_CONTROLLER_AXIS_MAX;
    descriptor.nbuttons = SDL_CONTROLLER_BUTTON_MAX;
    descriptor.vendor_id = 0x1234;
    descriptor.product_id = Uint16(model + 1);
    descriptor.name = names[model];
    return SDL_JoystickAttachVirtualEx(&descriptor);
}

static int device_index(SDL_JoystickID id) {
    for (int index = 0; index < SDL_NumJoysticks(); ++index)
        if (SDL_JoystickGetDeviceInstanceID(index) == id && SDL_JoystickIsVirtual(index)) return index;
    return -1;
}

int main(int argc, char** argv) {
    check(argc == 2 || argc == 3, "temporary config directory supplied");
    const bool child = argc == 3;
    auto handoff = std::filesystem::path(argv[1]) / "online-handoff.json";
    if (child) check(zelda64::load_online_input(handoff), "restore selection before SDL enumerates any devices");
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // Count only the fixture pads, even with a physical controller connected.
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, "0x1234/0x0001,0x1234/0x0002,0x1234/0x0003,0x1234/0x0004");
    check(SDL_Init(SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) == 0, "SDL init");
    if (!child) zelda64::reset_input_bindings();
    std::array<SDL_Joystick*, 4> sticks{};
    for (int order = 0; order < 4; ++order) {
        int p = child ? 3 - order : order;
        int index = attach_pad(p);
        check(index >= 0, "attach virtual gamepad");
        sticks[p] = SDL_JoystickOpen(index);
        check(sticks[p] != nullptr, "open virtual joystick");
    }
    pump();
    check(recomp::get_connected_controller_names().size() == 4, "four controllers detected");
    if (child) {
        check(recomp::online_input_ready(1) && recomp::get_port_controller_name(1) == "Fixture Pad A" &&
              recomp::get_port_controller_index(1) == 3, "fresh process retains manually selected device despite reversed discovery");
        SDL_JoystickSetVirtualButton(sticks[0], SDL_CONTROLLER_BUTTON_Y, 1);
        pump();
        uint16_t buttons; float x, y;
        check(recomp::get_n64_input(1, &buttons, &x, &y) && buttons == 0x8000,
              "fresh process uses selected device and transferred binding");
        for (auto joystick : sticks) SDL_JoystickClose(joystick);
        SDL_Quit();
        std::cout << "PASS: fresh-process controller identity, reversed enumeration and assigned input\n";
        return 0;
    }
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
    check(SDL_JoystickDetachVirtual(device_index(disconnected_id)) == 0, "detach second controller");
    pump();
    uint16_t buttons = 1; float x = 1, y = 1;
    check(recomp::get_n64_input(1, &buttons, &x, &y) && buttons == 0 && x == 0 && y == 0,
        "disconnected calibrated controller produces neutral input, not center-offset drift");
    float raw_x, raw_y; int32_t id;
    check(recomp::get_raw_controller_stick(2, &raw_x, &raw_y, &id)
        && id == SDL_JoystickInstanceID(sticks[2]), "disconnect does not move remaining players");
    int replacement = attach_pad(1);
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
    recomp::assign_controller_to_port(1, 0);
    auto selected_id = SDL_JoystickInstanceID(sticks[0]);
    auto snapshot = zelda64::serialize_online_input(1);
    { std::ofstream out(online_path); out << snapshot; }
    { std::ofstream out(handoff); out << snapshot; }
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
    check(zelda64::serialize_online_input(1) == snapshot, "all four ports round trip without personal file writes");
    check(recomp::online_input_ready(1) && !recomp::online_input_ready(0), "only selected local port is ready for live input");
    recomp::assign_controller_to_port(1, 2);
    check(recomp::get_raw_controller_stick(1, &raw_x, &raw_y, &id) && id == selected_id,
          "online device selection is immutable");
    recomp::assign_controller_to_port(0, 0);
    check(recomp::get_port_controller_index(0) == -1 && recomp::get_port_controller_index(1) == 0,
          "another port cannot steal the online controller");
    auto selected_device = recomp::get_port_controller_identity(1);
#ifdef __APPLE__
    check(selected_device.path.empty(), "macOS identities omit per-process device paths");
#endif
    recomp::set_online_controller(std::nullopt);
    int duplicate = attach_pad(0);
    auto duplicate_id = SDL_JoystickGetDeviceInstanceID(duplicate);
    pump();
    bool rejected = false;
    try { zelda64::serialize_online_input(1); } catch (const std::runtime_error&) { rejected = true; }
    check(rejected, "ambiguous online selection cannot be readied");
    recomp::set_online_controller(recomp::OnlineController{1, selected_device});
    check(!recomp::online_input_ready(1) && !recomp::port_controller_connected(1),
          "ambiguous identical controllers cannot silently select the first enumerated device");
    check(SDL_JoystickDetachVirtual(device_index(duplicate_id)) == 0, "detach ambiguous controller");
    pump();
    check(recomp::online_input_ready(1), "unique selected controller becomes available again");
    SDL_JoystickClose(sticks[0]);
    check(SDL_JoystickDetachVirtual(device_index(selected_id)) == 0, "detach selected controller");
    pump();
    check(!recomp::online_input_ready(1), "disconnect never adopts another connected controller");
    buttons = 1; x = y = 1;
    check(recomp::get_n64_input(1, &buttons, &x, &y) && buttons == 0 && x == 0 && y == 0,
          "unavailable online device produces neutral input");
    sticks[0] = SDL_JoystickOpen(attach_pad(0));
    pump();
    check(recomp::online_input_ready(1) && recomp::get_raw_controller_stick(1, &raw_x, &raw_y, &id) && id != selected_id,
          "identity survives a changed SDL instance ID");
    auto identity = recomp::get_port_controller_identity(1);
    auto candidate = identity;
    identity.serial = "serial-a"; candidate.serial = "serial-b";
    check(!identity.matches(candidate), "serial mismatch never falls back to model");
    candidate.serial = identity.serial;
    identity.path = "path-a"; candidate.path = "path-b";
    check(!identity.matches(candidate), "path mismatch never falls back to serial or model");
    candidate.path = identity.path;
    check(identity.matches(candidate), "matching stable device keys are accepted");
    candidate.guid[0] = candidate.guid[0] == '0' ? '1' : '0';
    check(!identity.matches(candidate), "device model mismatch cannot reuse a serial or path");
    recomp::set_online_controller(std::nullopt);
    check(!recomp::online_input_ready(1), "controller mode without captured identity cannot start live online input");
    recomp::set_port_mode(0, recomp::ControllerPortMode::Keyboard);
    check(recomp::online_input_ready(0), "keyboard input does not require controller identity");
    recomp::set_port_mode(0, recomp::ControllerPortMode::Off);
    check(!recomp::online_input_ready(0), "disabled local port cannot start live online input");
    for (int variant = 0; variant < 7; ++variant) {
        auto invalid = nlohmann::json::parse(snapshot);
        auto& device = invalid["controller"];
        switch (variant) {
            case 0: device["port"] = 4294967297ULL; break;
            case 1: device["port"] = 1.5; break;
            case 2: device["guid"] = "not-a-device-guid"; break;
            case 3: device["serial"] = std::string("a\0b", 3); break;
            case 4: device["path"] = std::string(4097, 'x'); break;
            case 5: device["instance_id"] = 0; break;
            case 6: invalid["controls"]["ports"][1]["mode"] = "Keyboard"; break;
        }
        { std::ofstream out(online_path); out << invalid; }
        check(!zelda64::load_online_input(online_path), "malformed or contradictory device selection rejected");
    }
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
    // A running online match leaves the launcher menu receiving background
    // controller events; they must not press its Cancel/Leave buttons.
    recomp::set_port_mode(0, recomp::ControllerPortMode::Controller);
    pump();
    check(recomp::port_controller_connected(0), "port 0 has a controller for the menu checks");
    // Press on every pad, so the check does not depend on which one port 0 holds.
    auto press = [&](bool down) {
        for (auto joystick : sticks) {
            SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_A, down);
            // The legacy config loaded above binds N64 A to SDL Y.
            SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_Y, down);
            SDL_JoystickSetVirtualAxis(joystick, SDL_CONTROLLER_AXIS_LEFTX, down ? 32767 : 0);
        }
        pump();
    };
    queued_controller_events = 0;
    press(true); press(false);
    check(queued_controller_events > 0, "menus receive controller presses outside a match");
    recomp::set_menu_controller_input_blocked(true);
    queued_controller_events = 0;
    press(true);
    uint16_t held_buttons; float held_x, held_y;
    check(recomp::get_n64_input(0, &held_buttons, &held_x, &held_y) && (held_buttons & 0x8000),
        "blocking menu input leaves the controller readable");
    press(false);
    check(queued_controller_events == 0, "menus ignore controller presses during an online match");
    recomp::set_menu_controller_input_blocked(false);
    queued_controller_events = 0;
    press(true); press(false);
    check(queued_controller_events > 0, "menus receive controller presses again after the match");
    for (auto joystick : sticks) SDL_JoystickClose(joystick);
    SDL_Quit();
    std::cout << checks << " controller routing and config checks passed.\n";
}
