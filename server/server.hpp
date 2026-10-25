#pragma once
#include "../src/online/lobby_room.hpp"
#include "../src/online/enet_host.hpp"
#include <deque>
#include <memory>

namespace online::lobby {
// One serialized room. Pending handshakes have their own bounded transport slots.
class Server {
public:
    Server(const ENetAddress& address, Fingerprint fingerprint)
        : host_(make_host(&address, 8)), room_(fingerprint) {}
    unsigned port() const { return host_->address.port; }
    const Room& room() const { return room_; }
    void service(unsigned wait_ms = 0) {
        ENetEvent event{};
        // Bound one service pass so a busy peer cannot starve deadlines.
        for (unsigned processed = 0; processed < 128; ++processed) {
            auto result = enet_host_service(host_.get(), &event, processed ? 0 : wait_ms);
            if (result < 0) throw std::runtime_error("lobby UDP service failed");
            if (!result) break;
            auto now = monotonic_ms();
            auto& connection = connections_[event.peer - host_->peers];
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                connection = {++next_identity_, now, now, 0, false};
                enet_peer_timeout(event.peer, 8, 1000, 5000);
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                deliver(room_.disconnect(connection.identity));
                connection = {};
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> packet(event.packet, enet_packet_destroy);
                if (connection.closing || !connection.identity) continue;
                if (now - connection.rate_window >= 1000) { connection.rate_window = now; connection.received = 0; }
                if (++connection.received > 120 || event.channelID != control_channel ||
                    !(packet->flags & ENET_PACKET_FLAG_RELIABLE)) {
                    reject(event.peer, Reason::Protocol); continue;
                }
                try {
                    auto decoded = decode({packet->data, packet->dataLength});
                    deliver(room_.handle(connection.identity, decoded, now));
                } catch (const std::invalid_argument&) { reject(event.peer, Reason::Protocol); }
            }
        }
        auto now = monotonic_ms();
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& connection = connections_[i];
            if (!connection.identity) continue;
            bool joined = false;
            for (const auto& player : room_.players()) joined |= player.id == connection.identity;
            if (connection.closing && now - connection.connected >= 5000) {
                enet_peer_reset(&host_->peers[i]); connection = {};
            } else if (!connection.closing && !joined && now - connection.connected >= 5000)
                reject(&host_->peers[i], Reason::Timeout);
        }
        deliver(room_.tick(now));
        enet_host_flush(host_.get());
    }
private:
    struct Connection {
        uint64_t identity = 0, connected = 0, rate_window = 0;
        unsigned received = 0;
        bool closing = false;
    };
    Host host_;
    Room room_;
    std::array<Connection, 8> connections_{};
    uint64_t next_identity_ = 0;

    void reject(ENetPeer* peer, Reason reason) {
        Packet packet{Op::Reject}; packet.reason = reason;
        deliver({{connections_[peer - host_->peers].identity, packet, true}});
    }
    void deliver(const std::vector<Delivery>& messages) {
        std::deque<Delivery> pending(messages.begin(), messages.end());
        while (!pending.empty()) {
            auto message = std::move(pending.front()); pending.pop_front();
            for (size_t i = 0; i < connections_.size(); ++i) {
                auto& connection = connections_[i];
                if (!connection.identity || connection.identity != message.peer || connection.closing) continue;
                auto* peer = &host_->peers[i];
                auto queued = enet_list_size(&peer->outgoingCommands) + enet_list_size(&peer->outgoingSendReliableCommands) +
                              enet_list_size(&peer->sentReliableCommands);
                bool failed = queued >= 128;
                if (!failed) {
                    auto bytes = encode(message.packet);
                    auto* packet = enet_packet_create(bytes.data(), bytes.size(), ENET_PACKET_FLAG_RELIABLE);
                    failed = !packet || enet_peer_send(peer, control_channel, packet) < 0;
                    if (failed && packet) enet_packet_destroy(packet);
                }
                if (failed || message.disconnect) {
                    connection.closing = true; connection.connected = monotonic_ms();
                    auto departures = room_.disconnect(connection.identity);
                    pending.insert(pending.end(), departures.begin(), departures.end());
                    if (failed) { enet_peer_reset(peer); connection = {}; }
                    else enet_peer_disconnect_later(peer, uint32_t(message.packet.reason));
                }
                break;
            }
        }
    }
};
}
