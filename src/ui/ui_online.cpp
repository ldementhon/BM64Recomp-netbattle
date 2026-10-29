#include "ui_online.hpp"
#include "RmlUi/Core.h"
#include "zelda_config.h"
#include "recomp_input.h"
#include <charconv>
#include <chrono>
#include <memory>
#if defined(__APPLE__) || defined(_WIN32)
#include "../online/lobby_worker.hpp"
#endif
#include "../online/lobby_preferences.hpp"
#include "../online/release.hpp"

namespace {
std::filesystem::path preferences_path() { return zelda64::get_app_folder_path() / "online-lobby.json"; }

struct OnlineMenu {
    Rml::DataModelHandle model;
#if defined(__APPLE__) || defined(_WIN32)
    bool available = true;
    std::unique_ptr<online::lobby::LobbyWorker> worker;
#else
    bool available = false;
#endif
    bool visible = false, connected = false, joined = false, ready = false, can_start = false, in_match = false;
    int port = 0;
    std::string address = "127.0.0.1", server_port = "6464", name = "Player", release = online::release_name;
    std::string status = "Enter a server address to join.", latency, local_error;
    std::array<std::string, 4> seats{"P1 — CPU", "P2 — CPU", "P3 — CPU", "P4 — CPU"};
    // 0 = CPU, 1 = not ready, 2 = ready.
    std::array<int, 4> seat_states{};
    void action(const std::string& action) {
        local_error.clear();
        if (action == "open") visible = true;
        else if (action == "back") visible = false;
#if defined(__APPLE__) || defined(_WIN32)
        using Worker = online::lobby::LobbyWorker;
        try {
            if (action == "connect") {
                unsigned number = 0;
                auto parsed = std::from_chars(server_port.data(), server_port.data() + server_port.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr != server_port.data() + server_port.size() || number == 0 || number > 65535)
                    throw std::invalid_argument("Server port must be a number from 1 to 65535.");
                if (!worker) {
                    online::lobby::LobbySession::Launch launch;
                    launch.executable = online::lobby::current_executable();
                    launch.rom = zelda64::get_app_folder_path() / "bm64_us.z64";
                    launch.log = zelda64::get_app_folder_path() / "online-match.log";
                    worker = std::make_unique<Worker>(std::move(launch));
                }
                // Remember what was entered, even if this attempt fails.
                online::lobby::save_lobby_preferences(preferences_path(), {address, server_port, name});
                Worker::Command command{Worker::Action::Connect};
                command.address = address; command.port = uint16_t(number); command.name = name;
                worker->send(std::move(command));
                sent_name = typed_name = name; name_hint = false;
            } else if (worker && action != "open") {
                Worker::Command command{Worker::Action::Leave};
                if (action == "ready") {
                    command.action = Worker::Action::Ready; command.ready = !ready;
                    if (command.ready) {
                        if (port < 0 || port >= recomp::max_ports || recomp::get_port_mode(port) == recomp::ControllerPortMode::Off)
                            throw std::runtime_error("Enable this local port in Setup controls before readying up.");
                        if (recomp::get_port_mode(port) == recomp::ControllerPortMode::Controller && !recomp::port_controller_connected(port))
                            throw std::runtime_error("Connect a controller for this local port before readying up.");
                        command.input = {unsigned(port), zelda64::serialize_online_input(port)};
                    }
                } else if (action == "start") command.action = Worker::Action::Start;
                else if (action == "cancel") command.action = Worker::Action::Cancel;
                worker->send(std::move(command));
            }
        } catch (const std::exception& error) { status = local_error = error.what(); }
#endif
        model.DirtyAllVariables();
    }
    void update() {
#if defined(__APPLE__) || defined(_WIN32)
        if (!worker) return;
        rename_after_typing();
        auto next = worker->view();
        // Keep local validation errors until the worker has a new state to show.
        if (next == last_view) return;
        last_view = next;
        connected = next.connected; joined = next.joined; ready = next.ready;
        can_start = next.can_start; in_match = next.in_match;
        // The match window has focus; the menu still sees background controller events.
        recomp::set_menu_controller_input_blocked(in_match);
        status = local_error.empty() ? next.status : local_error; latency = next.latency; seats = next.seats;
        for (unsigned i = 0; i < 4; ++i) seat_states[i] = int(next.seat_states[i]);
        model.DirtyAllVariables();
#endif
    }
#if defined(__APPLE__) || defined(_WIN32)
    online::lobby::LobbyWorker::View last_view;
    std::string typed_name, sent_name;
    bool name_hint = false;
    std::chrono::steady_clock::time_point typed_at;
    // Send a changed name once typing pauses; an empty or invalid name is never sent.
    void rename_after_typing() {
        auto now = std::chrono::steady_clock::now();
        if (name != typed_name) { typed_name = name; typed_at = now; return; }
        if (!joined || now - typed_at < std::chrono::milliseconds(500)) return;
        const bool valid = online::lobby::valid_name(name);
        if (valid == name_hint) {
            name_hint = !valid;
            status = local_error = name_hint ? "Enter a name of up to 32 bytes to rename yourself." : "";
            if (!name_hint) status = last_view.status;
            model.DirtyAllVariables();
        }
        if (!valid || name == sent_name) return;
        try {
            online::lobby::LobbyWorker::Command command{online::lobby::LobbyWorker::Action::Rename};
            command.name = name;
            worker->send(std::move(command));
            sent_name = name;
            online::lobby::save_lobby_preferences(preferences_path(), {address, server_port, name});
        } catch (const std::exception& error) { status = local_error = error.what(); }
        model.DirtyAllVariables();
    }
#endif
};
std::unique_ptr<OnlineMenu> menu;
}

void recompui::bind_online_menu(Rml::DataModelConstructor& constructor) {
    menu = std::make_unique<OnlineMenu>();
    if (auto saved = online::lobby::load_lobby_preferences(preferences_path())) {
        if (!saved->address.empty()) menu->address = saved->address;
        if (!saved->port.empty()) menu->server_port = saved->port;
        if (!saved->name.empty()) menu->name = saved->name;
    }
    menu->model = constructor.GetModelHandle();
    constructor.Bind("online_available", &menu->available);
    constructor.Bind("online_release", &menu->release);
    constructor.Bind("online_visible", &menu->visible);
    constructor.Bind("online_connected", &menu->connected);
    constructor.Bind("online_joined", &menu->joined);
    constructor.Bind("online_ready", &menu->ready);
    constructor.Bind("online_can_start", &menu->can_start);
    constructor.Bind("online_in_match", &menu->in_match);
    constructor.Bind("online_address", &menu->address);
    constructor.Bind("online_server_port", &menu->server_port);
    constructor.Bind("online_name", &menu->name);
    constructor.Bind("online_port", &menu->port);
    constructor.Bind("online_status", &menu->status);
    constructor.Bind("online_latency", &menu->latency);
    for (unsigned i = 0; i < 4; ++i) {
        constructor.Bind("online_seat" + std::to_string(i), &menu->seats[i]);
        constructor.Bind("online_seat" + std::to_string(i) + "_state", &menu->seat_states[i]);
    }
    constructor.BindEventCallback("online_action", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& inputs) {
        if (menu && inputs.size() == 1) menu->action(inputs[0].Get<std::string>());
    });
}
void recompui::update_online_menu() { if (menu) menu->update(); }
void recompui::shutdown_online_menu() {
    menu.reset();
    recomp::set_menu_controller_input_blocked(false);
}
