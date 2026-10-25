#include "../server/server.hpp"
#include "../src/online/lobby_client.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <thread>

using namespace online;
using namespace online::lobby;
static Pad physical(unsigned slot, uint64_t frame) {
    return {uint16_t(frame % (slot + 2) ? 0x8000 : 0x4000), int8_t((frame + slot * 3) % 100), int8_t(-int((frame * 3 + slot) % 100))};
}
static uint64_t update(uint64_t state, const Pads& pads) {
    for (auto pad : pads) state = state * 1099511628211ULL ^ (uint64_t(pad.buttons) << 16 | uint8_t(pad.x) << 8 | uint8_t(pad.y));
    return state;
}
struct Simulation {
    RollbackInputs input;
    struct Checkpoint { uint64_t frame = UINT64_MAX, state = 0; };
    std::array<Checkpoint, checkpoint_slots> checkpoints{};
    uint64_t state = 0, corrections = 0;
    Simulation(uint64_t match, unsigned slot) : input(match, 4, slot) {}
    void frame(uint64_t at) {
        checkpoints[at % checkpoint_slots] = {at, state};
        state = update(state, input.inputs_for(at));
    }
    void correct() {
        while (auto first = input.take_correction()) {
            auto saved = checkpoints[*first % checkpoint_slots];
            assert(saved.frame == *first); state = saved.state; ++corrections;
            for (auto at = *first; at < input.head(); ++at) frame(at);
        }
    }
};

int main() {
    assert(enet_initialize() == 0);
    {
        ENetAddress address{}; assert(enet_address_set_host(&address, "127.0.0.1") == 0);
        Fingerprint fingerprint{42}; Server server(address, fingerprint);
        std::vector<std::unique_ptr<LobbyClient>> clients;
        for (unsigned i = 0; i < 4; ++i)
            clients.push_back(std::make_unique<LobbyClient>("127.0.0.1", uint16_t(server.port()), "Player", fingerprint));
        auto pump = [&] {
            server.service();
            for (auto& client : clients) { client->poll(); assert(!client->disconnected()); }
        };
        auto until = [&](const std::function<bool()>& done) {
            auto deadline = monotonic_ms() + 8000;
            do {
                pump();
                if (done()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            } while (monotonic_ms() < deadline);
            throw std::runtime_error("input network test deadline exceeded");
        };
        until([&] { for (auto& client : clients) if (client->roster().count != 4 || !client->clock()) return false; return true; });
        auto& p1 = **std::find_if(clients.begin(), clients.end(), [](const auto& client) { return client->slot() == 0; });
        auto prepare = [&] {
            for (auto& client : clients) client->ready(true);
            until([&] { for (const auto& player : p1.roster().players) if (!player.ready) return false; return true; });
            p1.start();
            until([&] { for (auto& client : clients) if (client->roster().phase != Phase::Loading) return false; return true; });
            for (auto& client : clients) client->loaded(1374, 123, 456);
            until([&] { for (auto& client : clients) if (!client->start_at() || monotonic_ms() < *client->start_at()) return false; return true; });
        };
        auto lobby = [&] { for (auto& client : clients) if (client->roster().phase != Phase::Lobby) return false; return true; };
        prepare();
        std::vector<Simulation> games;
        for (auto& client : clients) games.emplace_back(client->roster().match, client->slot());
        struct Delivery { uint64_t due; unsigned client; InputBatch batch; };
        std::vector<Delivery> pending;
        unsigned dropped = 0, stalled = 0, duplicate = 0;
        bool done = false;
        for (uint64_t tick = 0; tick < 600 && !done; ++tick) {
            auto begin = monotonic_ms();
            pump();
            for (unsigned i = 0; i < clients.size(); ++i) {
                for (const auto& batch : clients[i]->take_inputs()) {
                    if (clients[i]->slot() == 2 && tick >= 50 && tick < 95) { ++dropped; continue; }
                    auto delay = (batch.slot * 7 + clients[i]->slot() * 3 + tick) % 7;
                    pending.push_back({tick + delay, i, batch});
                    if (batch.slot == 3 && tick % 11 == 0) { pending.push_back({tick + 1, i, batch}); ++duplicate; }
                }
            }
            assert(pending.size() < 256);
            for (auto it = pending.begin(); it != pending.end();) {
                if (it->due <= tick) { games[it->client].input.receive(it->batch); it = pending.erase(it); }
                else ++it;
            }
            done = true;
            for (unsigned i = 0; i < games.size(); ++i) {
                auto& game = games[i]; game.correct();
                if (game.input.head() < 180) {
                    game.input.sample(physical(clients[i]->slot(), game.input.head()));
                    if (game.input.can_advance()) game.frame(game.input.head());
                    else ++stalled;
                }
                // Send on a wall-time cadence even when all simulation is stalled.
                clients[i]->send_inputs(game.input.upload());
                done &= game.input.head() == 180 && game.input.confirmed() >= 180;
                assert(game.input.head() <= game.input.confirmed() + prediction_limit);
            }
            pump();
            auto elapsed = monotonic_ms() - begin;
            if (elapsed < 17) std::this_thread::sleep_for(std::chrono::milliseconds(17 - elapsed));
        }
        assert(done && dropped > 0 && stalled > 0 && duplicate > 0);
        uint64_t reference = 0;
        for (unsigned frame = 0; frame < 180; ++frame) {
            Pads pads{};
            if (frame >= input_delay) for (unsigned slot = 0; slot < 4; ++slot) pads[slot] = physical(slot, frame - input_delay);
            reference = update(reference, pads);
        }
        assert(server.input_confirmed() >= 180);
        for (unsigned i = 0; i < games.size(); ++i) {
            assert(games[i].state == reference && games[i].corrections > 0);
            clients[i]->finished(1374 + 180);
        }
        until(lobby);
        for (auto& client : clients) assert(client->error().empty());
        prepare();
        p1.finished(1377); // The server has confirmed only the two neutral inputs.
        until(lobby);
        for (auto& client : clients) assert(client->error() == reason_text(Reason::Mismatch));
        prepare();
        // Keep servicing the connection but make no input progress for five seconds.
        until(lobby);
        for (auto& client : clients) assert(client->error() == reason_text(Reason::Timeout) && client->joined());
    }
    enet_deinitialize();
    std::cout << "PASS: four ENet input streams, independent delayed/dropped/duplicate delivery, stalled resend, confirmed finish and timeout\n";
}
