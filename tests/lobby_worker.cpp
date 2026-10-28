#include "../server/server.hpp"
#include "../src/online/lobby_worker.hpp"
#include <cassert>
#include <iostream>
#include <random>

using namespace online::lobby;
int main() {
    namespace fs = std::filesystem;
    auto scratch = fs::temp_directory_path() / ("bm64-worker-" + std::to_string(std::random_device{}()));
    assert(fs::create_directory(scratch));
#ifdef _WIN32
    auto fixture = current_executable().parent_path() / "lobby_client.exe";
#else
    auto fixture = current_executable().parent_path() / "lobby_client";
#endif
    auto rom = scratch / "fixture.rom";
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
        assert(a.view().seat_states[0] == LobbyWorker::SeatState::NotReady && a.view().seat_states[1] == LobbyWorker::SeatState::NotReady &&
               a.view().seat_states[2] == LobbyWorker::SeatState::Cpu);
        // A rename reaches every player; the room can rename without anyone readying again.
        LobbyWorker::Command rename{LobbyWorker::Action::Rename}; rename.name = "Bobby";
        b.send(std::move(rename));
        auto shows = [](const LobbyWorker::View& view, const std::string& suffix) {
            return std::any_of(view.seats.begin(), view.seats.begin() + 2, [&](const std::string& seat) {
                return seat.size() >= suffix.size() && seat.compare(seat.size() - suffix.size(), suffix.size(), suffix) == 0;
            });
        };
        until([&] { return shows(a.view(), " — Bobby") && shows(b.view(), " — Bobby (you)") && shows(a.view(), " — Alice (you)"); });
        auto ready = [](LobbyWorker& worker) {
            LobbyWorker::Command command{LobbyWorker::Action::Ready}; command.ready = true;
            command.input = {2, R"({"controls":{},"joystick_deadzone":17})"};
            worker.send(std::move(command));
        };
        for (uint64_t match = 1; match <= 2; ++match) {
            ready(a); ready(b);
            until([&] { return a.view().ready && b.view().ready && (a.view().can_start || b.view().can_start); });
            assert(a.view().seat_states[0] == LobbyWorker::SeatState::Ready && a.view().seat_states[1] == LobbyWorker::SeatState::Ready);
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
    std::cout << "PASS: background owners finish repeat matches without UI polls, publish seats/Ready, rename, reject changed ROM on Prepare/reconnect, and leave cleanly\n";
}
