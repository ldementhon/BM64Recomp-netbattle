#include "../server/server.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <thread>

using namespace online::lobby;
static Fingerprint fingerprint{42};
struct Client {
    Host host{enet_host_create(nullptr, 1, channel_count, 0, 0), enet_host_destroy};
    ENetPeer* peer = nullptr;
    bool connected = false, disconnected = false;
    unsigned relays = 0;
    std::optional<uint16_t> server_release;
    Packet roster{Op::Roster};
    std::vector<Packet> events;
    explicit Client(unsigned port) {
        assert(host);
        ENetAddress address{}; address.port = uint16_t(port);
        assert(enet_address_set_host(&address, "127.0.0.1") == 0);
        peer = enet_host_connect(host.get(), &address, channel_count, 0);
        assert(peer);
    }
    void send(const Packet& p, uint8_t channel = control_channel) {
        auto bytes = encode(p);
        auto* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
        assert(packet && enet_peer_send(peer, channel, packet) == 0);
        enet_host_flush(host.get());
    }
    void upload(const online::InputBatch& batch) {
        auto bytes = encode_input(batch, false);
        auto* packet = enet_packet_create(bytes.data(), bytes.size(), 0);
        assert(packet && enet_peer_send(peer, uint8_t(batch.slot + 1), packet) == 0);
        enet_host_flush(host.get());
    }
    void service() {
        ENetEvent event{};
        while (enet_host_service(host.get(), &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) connected = true;
            if (event.type == ENET_EVENT_TYPE_DISCONNECT) disconnected = true;
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                if (event.channelID != control_channel) {
                    assert(!(event.packet->flags & ENET_PACKET_FLAG_RELIABLE));
                    auto input = decode_input({event.packet->data, event.packet->dataLength}, true);
                    assert(event.channelID == input.slot + 1); ++relays;
                    enet_packet_destroy(event.packet); continue;
                }
                assert(event.channelID == control_channel && (event.packet->flags & ENET_PACKET_FLAG_RELIABLE));
                if ((server_release = release_mismatch({event.packet->data, event.packet->dataLength}))) {
                    enet_packet_destroy(event.packet); continue;
                }
                auto packet = decode({event.packet->data, event.packet->dataLength});
                enet_packet_destroy(event.packet);
                if (packet.op == Op::Roster) roster = packet;
                events.push_back(packet);
            }
        }
    }
    bool saw(Op op, Reason reason = Reason::None) const {
        for (const auto& event : events) if (event.op == op && event.reason == reason) return true;
        return false;
    }
    Packet command(Op op) const {
        Packet packet{op}; packet.revision = roster.revision; packet.match = roster.match; return packet;
    }
};
struct Fixture {
    Server server;
    std::vector<std::unique_ptr<Client>> clients;
    Fixture() : server(address(), fingerprint) {}
    static ENetAddress address() {
        ENetAddress a{}; assert(enet_address_set_host(&a, "127.0.0.1") == 0); return a;
    }
    Client& add() { clients.push_back(std::make_unique<Client>(server.port())); return *clients.back(); }
    void until(const std::function<bool()>& done) {
        auto deadline = monotonic_ms() + 7000;
        do {
            server.service();
            for (auto& client : clients) client->service();
            if (done()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } while (monotonic_ms() < deadline);
        throw std::runtime_error("network test deadline exceeded");
    }
    void join(Client& client, bool compatible = true, uint16_t release = online::release) {
        until([&] { return client.connected; });
        Packet hello{Op::Hello}; hello.name = "Player"; hello.fingerprint = fingerprint; hello.release = release;
        if (!compatible) hello.fingerprint[0] ^= 1;
        client.send(hello);
    }
    Client& p1() {
        for (auto& client : clients)
            if (client->roster.identity == server.room().players()[0].id) return *client;
        throw std::runtime_error("missing P1");
    }
    void start(unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            clients[i]->events.clear();
            auto ready = clients[i]->command(Op::Ready); ready.ready = true; clients[i]->send(ready);
        }
        until([&] {
            for (unsigned i = 0; i < count; ++i)
                for (unsigned j = 0; j < count; ++j) if (!clients[i]->roster.players[j].ready) return false;
            return true;
        });
        p1().send(p1().command(Op::Start));
        until([&] {
            for (unsigned i = 0; i < count; ++i) if (!clients[i]->saw(Op::Prepare)) return false;
            return true;
        });
    }
};

int main() {
    assert(enet_initialize() == 0);
    for (unsigned count : {2, 3, 4}) {
        Fixture f;
        for (unsigned i = 0; i < count; ++i) f.add();
        f.until([&] { for (auto& c : f.clients) if (!c->connected) return false; return true; });
        // Queue all hellos before the server runs: serialized allocation must stay unique.
        for (auto& c : f.clients) {
            Packet hello{Op::Hello}; hello.name = "Player"; hello.fingerprint = fingerprint; hello.release = online::release;
            c->send(hello);
        }
        f.until([&] { for (auto& c : f.clients) if (c->roster.count != count) return false; return true; });
        for (unsigned i = 0; i < count; ++i) {
            assert(f.clients[i]->roster.identity != 0);
            for (unsigned j = 0; j < i; ++j) assert(f.clients[i]->roster.identity != f.clients[j]->roster.identity);
        }
        auto& incompatible = f.add(); f.join(incompatible, false);
        f.until([&] { return incompatible.saw(Op::Reject, Reason::Incompatible) && incompatible.disconnected; });
        // Another release gets the server's release number instead of a Reject it might not parse,
        // even when its fingerprint would otherwise be accepted.
        for (uint16_t release : {uint16_t(online::release - 1), uint16_t(online::release + 1)}) {
            auto& other = f.add(); f.join(other, true, release);
            f.until([&] { return other.server_release == online::release && other.disconnected; });
            assert(other.events.empty() && f.server.room().count() == count);
        }
        if (count == 4) {
            auto& fifth = f.add(); f.join(fifth);
            f.until([&] { return fifth.saw(Op::Reject, Reason::Full) && fifth.disconnected; });
        }
        auto stale = f.p1().command(Op::Ready); --stale.revision; stale.ready = true;
        f.p1().send(stale);
        f.until([&] { return f.p1().saw(Op::Reject, Reason::StaleRoster); });
        const auto identity = f.p1().roster.identity;
        for (unsigned match = 1; match <= 2; ++match) {
            f.start(count);
            assert(f.server.room().match() == match);
            for (unsigned i = 0; i < count; ++i) {
                auto loaded = f.clients[i]->command(Op::Loaded);
                loaded.frame = 1374; loaded.ram = 123; loaded.registers = 456;
                f.clients[i]->send(loaded);
            }
            f.until([&] { for (unsigned i = 0; i < count; ++i) if (!f.clients[i]->saw(Op::Commit)) return false; return true; });
            f.until([&] { return monotonic_ms() >= f.server.room().start_epoch(); });
            for (unsigned i = 0; i < count; ++i) {
                auto finish = f.clients[i]->command(Op::Finished); finish.frame = 1376; f.clients[i]->send(finish);
            }
            f.until([&] { for (unsigned i = 0; i < count; ++i)
                if (!f.clients[i]->saw(Op::End) || f.clients[i]->roster.phase != Phase::Lobby) return false;
                return true;
            });
            assert(f.p1().roster.identity == identity);
            for (unsigned i = 0; i < count; ++i) assert(!f.clients[i]->roster.players[i].ready);
        }
        f.start(count);
        auto cancel = f.p1().command(Op::Cancel); cancel.reason = Reason::LoadFailed; f.p1().send(cancel);
        f.until([&] { for (unsigned i = 0; i < count; ++i) if (!f.clients[i]->saw(Op::End, Reason::LoadFailed)) return false; return true; });
        f.start(count);
        auto& late = f.add(); f.join(late);
        f.until([&] { return late.saw(Op::Reject, Reason::InProgress) && late.disconnected; });
        auto old_p1 = f.p1().roster.identity;
        enet_peer_disconnect(f.p1().peer, 0);
        f.until([&] { return f.server.room().phase() == Phase::Lobby && f.server.room().count() == count - 1; });
        assert(f.server.room().players()[0].id != old_p1);
    }
    {
        Fixture f; auto& bad = f.add(); f.join(bad);
        f.until([&] { return bad.roster.count == 1; });
        bad.send(bad.command(Op::Ready), 1);
        f.until([&] { return bad.saw(Op::Reject, Reason::Protocol) && bad.disconnected; });
        assert(f.server.room().count() == 0);
    }
    for (bool conflict : {false, true}) {
        Fixture f;
        for (unsigned i = 0; i < 2; ++i) { auto& client = f.add(); f.join(client); }
        f.until([&] { return f.clients[0]->roster.count == 2 && f.clients[1]->roster.count == 2; });
        f.start(2);
        for (auto& client : f.clients) {
            auto loaded = client->command(Op::Loaded); loaded.frame = 1374; loaded.ram = 123; loaded.registers = 456;
            client->send(loaded);
        }
        f.until([&] { return f.server.room().phase() == Phase::Playing; });
        auto& sender = f.p1();
        online::InputBatch batch; batch.match = sender.roster.match; batch.count = 1;
        batch.received = {2, 2, 0, 0}; batch.pads[0].buttons = 0x8000;
        if (conflict) {
            sender.upload(batch); f.until([&] { return sender.relays > 0; });
            batch.pads[0].buttons = 0x4000;
        }
        else batch.slot = 1; // Correct channel for P2, wrong connection identity.
        sender.upload(batch);
        f.until([&] { return sender.saw(Op::Reject, Reason::Protocol) && sender.disconnected; });
        assert(f.server.room().phase() == Phase::Lobby && f.server.room().count() == 1);
    }
    enet_deinitialize();
    std::cout << "PASS: real ENet 2/3/4-client joins, rejection, readiness, repeated matches, loading failure and disconnects\n";
}
