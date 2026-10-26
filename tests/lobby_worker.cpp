#include "../server/server.hpp"
#include "../src/online/lobby_worker.hpp"
#include <cassert>
#include <iostream>

using namespace online::lobby;
int main() {
    namespace fs = std::filesystem;
    auto pattern = (fs::temp_directory_path() / "bm64-worker-XXXXXX").string();
    assert(mkdtemp(pattern.data())); fs::path scratch(pattern);
    auto fixture = current_executable().parent_path() / "lobby_client", rom = scratch / "fixture.rom";
    std::ofstream(rom) << "worker test fixture";
    assert(enet_initialize() == 0);
    {
        ENetAddress address{}; assert(enet_address_set_host(&address, "127.0.0.1") == 0);
        Server server(address, client_fingerprint(fixture, rom));
        LobbySession::Launch launch{fixture, rom, scratch / "a.log", {"--child"}, {}};
        LobbyWorker a(launch); launch.log = scratch / "b.log"; LobbyWorker b(launch);
        auto connect = [&](LobbyWorker& worker, std::string name) {
            LobbyWorker::Command command{LobbyWorker::Action::Connect};
            command.address = "127.0.0.1"; command.port = uint16_t(server.port()); command.name = std::move(name);
            worker.send(std::move(command));
        };
        auto until = [&](auto done) {
            const auto deadline = monotonic_ms() + 7000;
            do {
                server.service();
                if (done()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (monotonic_ms() < deadline);
            throw std::runtime_error("worker deadline: " + a.view().status + "; " + b.view().status);
        };
        connect(a, "Alice"); connect(b, "Bob");
        until([&] { return a.view().joined && b.view().joined && server.room().count() == 2; });
        assert(!a.view().fingerprint.empty());
        assert(a.view().seats[2] == "P3 — CPU" && b.view().seats[3] == "P4 — CPU");
        auto ready = [](LobbyWorker& worker) {
            LobbyWorker::Command command{LobbyWorker::Action::Ready}; command.ready = true;
            command.input = {2, R"({"controls":{},"joystick_deadzone":17})"};
            worker.send(std::move(command));
        };
        for (uint64_t match = 1; match <= 2; ++match) {
            ready(a); ready(b);
            until([&] { return a.view().ready && b.view().ready && (a.view().can_start || b.view().can_start); });
            (a.view().can_start ? a : b).send({LobbyWorker::Action::Start});
            // Only the server is serviced here: no render/UI polling is needed
            // for either client to load, start, exchange inputs, and finish.
            auto deadline = monotonic_ms() + 7000;
            do {
                server.service();
                if (server.room().match() == match && server.room().phase() == Phase::Lobby) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (monotonic_ms() < deadline);
            until([&] { return a.view().match == match && b.view().match == match && !a.view().in_match && !b.view().in_match; });
            assert(a.view().status == "Ready up. P1 starts when everyone is ready.");
            assert(b.view().status == a.view().status && !a.view().ready && !b.view().ready);
        }
        std::ofstream(rom) << "changed after connecting";
        ready(a); ready(b);
        until([&] { return a.view().can_start || b.view().can_start; });
        (a.view().can_start ? a : b).send({LobbyWorker::Action::Start});
        until([&] { return a.view().match == 3 && b.view().match == 3 && !a.view().in_match && !b.view().in_match; });
        assert(a.view().status.find("changed") != std::string::npos && b.view().status.find("changed") != std::string::npos);
        a.send({LobbyWorker::Action::Leave}); b.send({LobbyWorker::Action::Leave});
        until([&] { return !a.view().connected && !b.view().connected && server.room().count() == 0; });
        connect(a, "Alice");
        until([&] { return a.view().status == reason_text(Reason::Incompatible); });
    }
    enet_deinitialize(); fs::remove_all(scratch);
    std::cout << "PASS: background owners finish repeat matches without UI polls, publish seats/Ready, reject changed ROM on Prepare/reconnect, and leave cleanly\n";
}
