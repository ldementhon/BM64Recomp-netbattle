#pragma once
#include "lobby_protocol.hpp"
#include "server_clock.hpp"
#include "enet_host.hpp"
#include "input_protocol.hpp"
#include "release.hpp"
#include <deque>
#include <memory>

namespace online::lobby {
inline std::string release_mismatch_text(uint16_t server, uint16_t client) {
    auto theirs = release_label(server), ours = release_label(client);
    auto text = "This server runs online " + theirs + " and you have " + ours + ". ";
    return text + (server > client ? "Get the " + theirs + " download to join."
                                   : "Ask the host to update the server to " + ours + ".");
}
inline std::string incompatible_text(uint16_t client) {
    auto ours = release_label(client);
    return "Your game files don't match online " + ours + " on this server. Reinstall from the " + ours +
           " download, and use the same US ROM as the host.";
}

// Connection owner, independent of the disposable game process and renderer.
// Call ENet initialization once before constructing any clients.
class LobbyClient {
public:
    LobbyClient(const std::string& address, uint16_t port, std::string name, Fingerprint fingerprint,
                uint16_t release = online::release)
        : host_(make_host(nullptr, 1)), release_(release) {
        if (!valid_name(name) || !port) throw std::invalid_argument("invalid server port or display name");
        ENetAddress endpoint{}; endpoint.port = port;
        if (enet_address_set_host(&endpoint, address.c_str()) < 0) throw std::runtime_error("cannot resolve server address");
        peer_ = enet_host_connect(host_.get(), &endpoint, channel_count, 0);
        if (!peer_) throw std::runtime_error("cannot connect to server");
        enet_peer_timeout(peer_, 8, 1000, 5000);
        hello_.name = std::move(name); hello_.fingerprint = fingerprint; hello_.release = release;
        connected_at_ = now();
    }
    ~LobbyClient() { if (peer_) enet_peer_disconnect_now(peer_, 0); }
    LobbyClient(const LobbyClient&) = delete;
    LobbyClient& operator=(const LobbyClient&) = delete;
    const Packet& roster() const { return roster_; }
    bool joined() const { return roster_.identity != 0 && !disconnected_; }
    bool disconnected() const { return disconnected_; }
    const std::string& error() const { return error_; }
    std::optional<ServerClock::Estimate> clock() const { return clock_.estimate(now()); }
    std::optional<uint64_t> start_at() const { return start_at_; }
    uint64_t checked_frame() const { return checked_frame_; }
    unsigned slot() const {
        if (joined()) for (unsigned i = 0; i < roster_.count; ++i)
            if (roster_.players[i].id == roster_.identity) return i;
        throw std::logic_error("client has no player slot");
    }
    void send_inputs(const InputBatch& batch) {
        require_phase(Phase::Playing);
        if (batch.match != roster_.match || batch.slot != slot()) throw std::invalid_argument("input does not match assigned player");
        if (!send_packet(peer_, uint8_t(batch.slot + 1), encode_input(batch, false), 0))
            throw std::runtime_error("server input queue is full");
    }
    std::vector<InputBatch> take_inputs() {
        auto result = std::move(input_events_); input_events_.clear(); return result;
    }
    static uint64_t now() { return monotonic_ms(); }

    void ready(bool value) {
        require_phase(Phase::Lobby);
        auto p = command(Op::Ready); p.ready = value; send(p);
    }
    void start() { require_phase(Phase::Lobby); send(command(Op::Start)); }
    void loaded(uint64_t frame, uint64_t ram, uint64_t registers) {
        require_phase(Phase::Loading);
        if (!clock()) throw std::logic_error("wait for server clock before acknowledging load");
        auto p = command(Op::Loaded); p.frame = frame; p.ram = ram; p.registers = registers; send(p);
    }
    void cancel(Reason reason = Reason::Cancelled) {
        if (!joined() || roster_.phase == Phase::Lobby) return;
        if (reason != Reason::Cancelled && reason != Reason::LoadFailed) throw std::invalid_argument("invalid cancellation reason");
        auto p = command(Op::Cancel); p.reason = reason; send(p);
    }
    void finished(uint64_t frame) {
        require_phase(Phase::Playing);
        auto p = command(Op::Finished); p.frame = frame; send(p);
    }
    void check_state(uint64_t frame, uint64_t ram, uint64_t registers) {
        require_phase(Phase::Playing);
        auto p = command(Op::State); p.frame = frame; p.ram = ram; p.registers = registers; send(p);
    }
    // Only P1 may restart; the relay ends this match and prepares another.
    void restart() { require_phase(Phase::Playing); send(command(Op::Restart)); }
    void rename(const std::string& name) {
        if (!joined()) throw std::logic_error("command invalid in current lobby state");
        if (!valid_name(name)) throw std::invalid_argument("invalid display name");
        auto p = command(Op::Rename); p.name = name; send(p);
    }

    std::vector<Packet> poll() {
        std::vector<Packet> events;
        if (disconnected_) return events;
        ENetEvent event{};
        try {
            for (unsigned processed = 0; processed < 128; ++processed) {
                auto result = enet_host_service(host_.get(), &event, 0);
                if (result < 0) throw std::runtime_error("server connection failed");
                if (!result) break;
                if (event.type == ENET_EVENT_TYPE_CONNECT) { connected_ = true; send(hello_); }
                else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                    disconnected_ = true; start_at_.reset(); input_events_.clear();
                    if (error_.empty()) error_ = "Server disconnected.";
                    break;
                } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                    std::unique_ptr<ENetPacket, decltype(&enet_packet_destroy)> wire(event.packet, enet_packet_destroy);
                    if (event.channelID != control_channel) {
                        if (wire->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED))
                            throw std::runtime_error("invalid server input channel flags");
                        auto input = decode_input({wire->data, wire->dataLength}, true);
                        if (event.channelID != input.slot + 1) throw std::runtime_error("wrong server input channel");
                        if (!joined() || roster_.phase == Phase::Lobby || input.match != roster_.match) continue;
                        if (input.slot >= roster_.count || input_events_.size() >= 128)
                            throw std::runtime_error("invalid or excessive relayed input");
                        input_events_.push_back(input);
                        continue;
                    }
                    if (!(wire->flags & ENET_PACKET_FLAG_RELIABLE))
                        throw std::runtime_error("invalid server control channel");
                    std::span<const uint8_t> bytes{wire->data, wire->dataLength};
                    if (auto server = release_mismatch(bytes)) throw std::runtime_error(release_mismatch_text(*server, release_));
                    if (auto protocol = wire_protocol(bytes); protocol && *protocol < protocol_version)
                        throw std::runtime_error("This server runs an online build from before v1. Ask the host to update it.");
                    auto p = decode(bytes);
                    if (p.op == Op::Pong) {
                        if (pings_.empty() || pings_.front().nonce != p.identity)
                            throw std::runtime_error("unexpected server clock response");
                        auto sent = pings_.front().sent; pings_.pop_front();
                        // A delayed probe is not useful, but must not poison newer samples.
                        auto received = now();
                        if (received - sent <= 1000) clock_.sample(sent, received, p.time);
                        continue;
                    }
                    if (p.op == Op::Roster) {
                        bool found = false;
                        for (unsigned i = 0; i < p.count; ++i) found |= p.players[i].id == p.identity;
                        if (!found || !p.identity || p.revision < roster_.revision ||
                            (roster_.identity && p.identity != roster_.identity))
                            throw std::runtime_error("invalid server roster identity or revision");
                        roster_ = p;
                    } else if (p.op == Op::Prepare) {
                        require_phase(Phase::Loading);
                        if (p.match != roster_.match || p.revision != roster_.revision || p.count != roster_.count)
                            throw std::runtime_error("match preparation differs from locked roster");
                        start_at_.reset(); error_.clear(); input_events_.clear(); checked_frame_ = 0;
                    } else if (p.op == Op::Commit) {
                        require_phase(Phase::Playing);
                        if (p.match != roster_.match || start_at_) throw std::runtime_error("unexpected match start");
                        try { start_at_ = clock_.local_start(p.time, now()); }
                        catch (const std::runtime_error& e) { error_ = e.what(); cancel(Reason::LoadFailed); continue; }
                    } else if (p.op == Op::StateChecked || p.op == Op::Desync) {
                        require_phase(Phase::Playing);
                        if (p.match != roster_.match || !p.frame || p.frame <= checked_frame_)
                            throw std::runtime_error("unexpected state-check acknowledgment");
                        checked_frame_ = p.frame;
                    } else if (p.op == Op::End) {
                        if (p.match != roster_.match) throw std::runtime_error("unexpected match termination");
                        start_at_.reset(); input_events_.clear();
                        if (p.reason != Reason::None && p.reason != Reason::Restarted && error_.empty()) error_ = reason_text(p.reason);
                        if (p.reason == Reason::Mismatch && p.frame) error_ += " Frame " + std::to_string(p.frame) + ".";
                    } else if (p.op == Op::Reject)
                        error_ = p.reason == Reason::Incompatible ? incompatible_text(release_) : reason_text(p.reason);
                    else throw std::runtime_error("unexpected server message");
                    events.push_back(p);
                }
            }
            auto time = now();
            if (!joined() && time - connected_at_ >= 10000) throw std::runtime_error("server join timed out");
            if (!pings_.empty() && time - pings_.front().sent >= 5000)
                throw std::runtime_error("server clock responses timed out");
            if (connected_ && !disconnected_ && time - last_ping_ >= 250 && pings_.size() < 8) {
                auto p = command(Op::Ping); p.identity = ++nonce_;
                send(p); pings_.push_back({p.identity, time}); last_ping_ = time;
            }
            enet_host_flush(host_.get());
        } catch (const std::exception& e) {
            error_ = e.what(); disconnected_ = true; start_at_.reset(); input_events_.clear();
            enet_peer_disconnect_now(peer_, 0);
        }
        return events;
    }
private:
    Host host_;
    uint16_t release_;
    ENetPeer* peer_ = nullptr;
    Packet hello_{Op::Hello}, roster_{Op::Roster};
    ServerClock clock_;
    struct Ping { uint64_t nonce, sent; };
    std::deque<Ping> pings_;
    uint64_t connected_at_ = 0, last_ping_ = 0, nonce_ = 0;
    bool connected_ = false, disconnected_ = false;
    std::string error_;
    std::optional<uint64_t> start_at_;
    uint64_t checked_frame_ = 0;
    std::vector<InputBatch> input_events_;
    Packet command(Op op) const {
        Packet p{op}; p.revision = roster_.revision; p.match = roster_.match; return p;
    }
    void require_phase(Phase phase) const {
        if (!joined() || roster_.phase != phase) throw std::logic_error("command invalid in current lobby state");
    }
    void send(const Packet& p) {
        if (disconnected_) throw std::runtime_error("server disconnected");
        if (!send_packet(peer_, control_channel, encode(p), ENET_PACKET_FLAG_RELIABLE)) {
            throw std::runtime_error("cannot send server message");
        }
    }
};
}
