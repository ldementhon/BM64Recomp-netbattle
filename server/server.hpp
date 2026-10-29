#pragma once
#include "../src/online/lobby_room.hpp"
#include "../src/online/enet_host.hpp"
#include "../src/online/input_protocol.hpp"
#include "../src/online/release.hpp"
#include <deque>
#include <memory>

namespace online::lobby {
// One serialized room. Pending handshakes have their own bounded transport slots.
class Server {
public:
    Server(const ENetAddress& address, std::vector<Fingerprint> accepted, uint16_t release = online::release)
        : host_(make_host(&address, 8)), room_(std::move(accepted)), release_(release) {}
    Server(const ENetAddress& address, Fingerprint fingerprint)
        : Server(address, std::vector<Fingerprint>{fingerprint}) {}
    unsigned port() const { return host_->address.port; }
    const Room& room() const { return room_; }
    uint64_t input_confirmed() const { return inputs_ ? inputs_->confirmed() : 0; }
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
                if (now - connection.rate_window >= 1000) {
                    connection.rate_window = now; connection.received = connection.inputs_received = 0;
                }
                // Too many control messages is a protocol fault. Input packets arrive in bursts
                // after a network or machine hitch, and each repeats the newest inputs, so excess
                // ones are dropped instead.
                if (event.channelID == control_channel ? ++connection.received > 120 : ++connection.inputs_received > 240) {
                    if (event.channelID == control_channel) reject(event.peer, Reason::Protocol);
                    continue;
                }
                try {
                    if (event.channelID == control_channel) {
                        if (!(packet->flags & ENET_PACKET_FLAG_RELIABLE)) throw std::invalid_argument("unreliable control message");
                        std::span<const uint8_t> bytes{packet->data, packet->dataLength};
                        if (auto theirs = hello_release(bytes); theirs && *theirs != release_) {
                            reject_release(event.peer); continue;
                        }
                        auto decoded = decode(bytes);
                        if ((decoded.op == Op::Finished || decoded.op == Op::State) && room_.phase() == Phase::Playing && decoded.match == room_.match() &&
                            decoded.frame > room_.initial_frame() && decoded.frame - room_.initial_frame() > input_confirmed())
                            deliver(room_.abort(Reason::Mismatch));
                        else deliver(room_.handle(connection.identity, decoded, now));
                    } else {
                        if (packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED))
                            throw std::invalid_argument("input must use an unreliable sequenced channel");
                        auto decoded = decode_input({packet->data, packet->dataLength}, false);
                        if (event.channelID != decoded.slot + 1) throw std::invalid_argument("wrong input stream channel");
                        receive_inputs(connection.identity, decoded, now);
                    }
                } catch (const std::invalid_argument&) { reject(event.peer, Reason::Protocol); }
            }
            sync_inputs();
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
        sync_inputs();
        if (inputs_ && now >= last_progress_ && now - last_progress_ >= 5000) {
            deliver(room_.abort(Reason::Timeout)); sync_inputs();
        }
        enet_host_flush(host_.get());
    }
private:
    struct Connection {
        uint64_t identity = 0, connected = 0, rate_window = 0;
        unsigned received = 0, inputs_received = 0;
        bool closing = false;
    };
    Host host_;
    Room room_;
    uint16_t release_;
    std::array<Connection, 8> connections_{};
    uint64_t next_identity_ = 0;
    std::optional<InputRelay> inputs_;
    uint64_t last_progress_ = 0;

    void sync_inputs() {
        if (room_.phase() != Phase::Playing) inputs_.reset();
        else if (!inputs_ || inputs_->match() != room_.match()) {
            inputs_.emplace(room_.match(), room_.count()); last_progress_ = room_.start_epoch();
        }
    }
    void receive_inputs(uint64_t identity, const InputBatch& batch, uint64_t now) {
        if (room_.phase() != Phase::Playing || batch.match != room_.match()) return;
        unsigned slot = 0;
        while (slot < room_.count() && room_.players()[slot].id != identity) ++slot;
        if (slot == room_.count()) throw std::invalid_argument("unassigned input sender");
        sync_inputs();
        auto before = inputs_->confirmed();
        auto relayed = inputs_->accept(slot, batch);
        if (inputs_->confirmed() > before) last_progress_ = std::max(now, room_.start_epoch());
        if (!relayed) return;
        auto bytes = encode_input(*relayed, true);
        for (size_t i = 0; i < connections_.size(); ++i) {
            auto& connection = connections_[i];
            bool joined = false;
            for (const auto& player : room_.players()) joined |= player.id && player.id == connection.identity;
            if (!joined || connection.closing) continue;
            // A peer whose queue is full misses this relay; later relays repeat the inputs,
            // and ENet's own timeout still removes a peer that stopped responding.
            send_packet(&host_->peers[i], uint8_t(batch.slot + 1), bytes, 0);
        }
    }

    // Another release may not parse a Reject, so it gets the fixed release reply instead.
    void reject_release(ENetPeer* peer) {
        auto& connection = connections_[peer - host_->peers];
        if (!send_packet(peer, control_channel, encode_release_mismatch(release_), ENET_PACKET_FLAG_RELIABLE)) {
            enet_peer_reset(peer); connection = {}; return;
        }
        connection.closing = true; connection.connected = monotonic_ms();
        enet_peer_disconnect_later(peer, 0);
    }
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
                bool failed = !send_packet(peer, control_channel, encode(message.packet), ENET_PACKET_FLAG_RELIABLE);
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
