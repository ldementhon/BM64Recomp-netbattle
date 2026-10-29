#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace online::lobby {
constexpr uint32_t magic = 0x424d3634;
constexpr uint16_t protocol_version = 6;
constexpr size_t packet_limit = 1199;
constexpr uint8_t control_channel = 0, channel_count = 5;
using Fingerprint = std::array<uint8_t, 32>;
enum class Phase : uint8_t { Lobby, Loading, Playing };
enum class Op : uint8_t { Hello = 1, Roster, Ready, Start, Prepare, Loaded, Commit, Cancel, Finished, End, Reject, Ping, Pong, State, StateChecked, Desync, Restart, Rename, Rules };
enum class Reason : uint8_t { None, Protocol, Incompatible, Full, InProgress, StaleRoster, NotReady, NotP1, LoadFailed, Mismatch, Timeout, Left, Cancelled, Restarted };
struct Player { uint64_t id = 0; std::string name; bool ready = false; };

// Only fields selected by op are serialized. No native padding or pointers cross the wire.
struct Packet {
    Op op;
    uint64_t revision = 0, match = 0, identity = 0, frame = 0, ram = 0, registers = 0, time = 0;
    uint16_t release = 0;
    Fingerprint fingerprint{};
    std::string name{};
    std::array<Player, 4> players{};
    uint8_t count = 0;
    bool ready = false;
    // Room rule set by P1 in the lobby; the server copies it into each match.
    bool pausing = true;
    Phase phase = Phase::Lobby;
    Reason reason = Reason::None;
};

inline bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 32) return false;
    bool visible = false;
    for (size_t i = 0; i < name.size();) {
        uint32_t c = uint8_t(name[i++]);
        if (c < 32 || c == 127) return false;
        visible |= c != 32;
        if (c < 128) continue;
        unsigned continuation;
        uint32_t minimum;
        if (c >= 0xc2 && c <= 0xdf) { continuation = 1; minimum = 0x80; c &= 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { continuation = 2; minimum = 0x800; c &= 0xf; }
        else if (c >= 0xf0 && c <= 0xf4) { continuation = 3; minimum = 0x10000; c &= 7; }
        else return false;
        for (unsigned j = 0; j < continuation; ++j) {
            if (i == name.size() || (uint8_t(name[i]) & 0xc0) != 0x80) return false;
            c = (c << 6) | (uint8_t(name[i++]) & 0x3f);
        }
        if (c < minimum || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return visible;
}

class Writer {
public:
    std::vector<uint8_t> bytes;
    void number(uint64_t value, unsigned size) {
        for (unsigned i = size; i; --i) bytes.push_back(uint8_t(value >> ((i - 1) * 8)));
    }
    void name(const std::string& value) {
        if (!valid_name(value)) throw std::invalid_argument("invalid display name");
        number(value.size(), 1);
        bytes.insert(bytes.end(), value.begin(), value.end());
    }
};
class Reader {
    std::span<const uint8_t> bytes_;
    size_t at_ = 0;
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint64_t number(unsigned size) {
        if (size > bytes_.size() - at_) throw std::invalid_argument("truncated lobby packet");
        uint64_t value = 0;
        while (size--) value = (value << 8) | bytes_[at_++];
        return value;
    }
    bool boolean() {
        auto value = number(1);
        if (value > 1) throw std::invalid_argument("invalid boolean");
        return value != 0;
    }
    std::string name() {
        auto size = number(1);
        if (size > 32 || size > bytes_.size() - at_) throw std::invalid_argument("invalid name length");
        std::string result(reinterpret_cast<const char*>(bytes_.data() + at_), size);
        at_ += size;
        if (!valid_name(result)) throw std::invalid_argument("invalid display name");
        return result;
    }
    bool end() const { return at_ == bytes_.size(); }
};

// Every release keeps these layouts, so builds from different releases can name each other's
// release even when the rest of the protocol changed. A Hello starts with magic, protocol,
// Op::Hello and the release number. A server of another release answers with magic,
// protocol 0 and its own release number, then disconnects.
inline std::optional<uint16_t> wire_protocol(std::span<const uint8_t> bytes) {
    if (bytes.size() < 6) return std::nullopt;
    Reader r(bytes);
    if (r.number(4) != magic) return std::nullopt;
    return uint16_t(r.number(2));
}
inline std::optional<uint16_t> hello_release(std::span<const uint8_t> bytes) {
    // Protocol 5 is the first with a release number.
    auto protocol = wire_protocol(bytes);
    if (!protocol || *protocol < 5 || bytes.size() < 9 || bytes[6] != uint8_t(Op::Hello)) return std::nullopt;
    return uint16_t(bytes[7] << 8 | bytes[8]);
}
inline std::vector<uint8_t> encode_release_mismatch(uint16_t server_release) {
    Writer w;
    w.number(magic, 4); w.number(0, 2); w.number(server_release, 2);
    return w.bytes;
}
inline std::optional<uint16_t> release_mismatch(std::span<const uint8_t> bytes) {
    if (bytes.size() != 8 || wire_protocol(bytes) != 0) return std::nullopt;
    return uint16_t(bytes[6] << 8 | bytes[7]);
}

inline std::vector<uint8_t> encode(const Packet& p) {
    Writer w;
    w.number(magic, 4); w.number(protocol_version, 2); w.number(uint8_t(p.op), 1);
    switch (p.op) {
    case Op::Hello:
        w.number(p.release, 2);
        for (auto byte : p.fingerprint) w.number(byte, 1);
        w.name(p.name); break;
    case Op::Rename: w.name(p.name); break;
    case Op::Rules: w.number(p.pausing, 1); break;
    case Op::Roster:
        if (p.count > 4) throw std::invalid_argument("invalid roster size");
        w.number(p.identity, 8); w.number(p.revision, 8); w.number(p.match, 8);
        w.number(uint8_t(p.phase), 1); w.number(p.pausing, 1); w.number(p.count, 1);
        for (unsigned i = 0; i < p.count; ++i) {
            w.number(p.players[i].id, 8); w.name(p.players[i].name); w.number(p.players[i].ready, 1);
        }
        break;
    case Op::Ready: w.number(p.revision, 8); w.number(p.ready, 1); break;
    case Op::Start: w.number(p.revision, 8); break;
    case Op::Prepare:
        w.number(p.revision, 8); w.number(p.match, 8); w.number(p.count, 1);
        // Profile 1: fixed colors, P1 stage choice, first to three, deterministic boot,
        // boot-seed profile/time origin zero, two-update scheduling offset.
        w.number(1, 2); w.number(0, 8); w.number(0, 8); w.number(2, 1); w.number(p.pausing, 1); break;
    case Op::Loaded:
    case Op::State:
        w.number(p.match, 8); w.number(p.frame, 8); w.number(p.ram, 8); w.number(p.registers, 8); break;
    case Op::Commit: w.number(p.match, 8); w.number(p.time, 8); break;
    case Op::Cancel: w.number(p.match, 8); w.number(uint8_t(p.reason), 1); break;
    case Op::Finished:
    case Op::StateChecked:
    case Op::Desync: w.number(p.match, 8); w.number(p.frame, 8); break;
    case Op::Restart: w.number(p.match, 8); break;
    case Op::End: w.number(p.match, 8); w.number(p.frame, 8); w.number(uint8_t(p.reason), 1); break;
    case Op::Reject: w.number(uint8_t(p.reason), 1); break;
    case Op::Ping: w.number(p.identity, 8); break;
    case Op::Pong: w.number(p.identity, 8); w.number(p.time, 8); break;
    default: throw std::invalid_argument("unknown lobby message");
    }
    if (w.bytes.size() > packet_limit) throw std::invalid_argument("oversized lobby packet");
    return w.bytes;
}

inline Packet decode(std::span<const uint8_t> bytes) {
    if (bytes.size() > packet_limit) throw std::invalid_argument("oversized lobby packet");
    Reader r(bytes);
    if (r.number(4) != magic || r.number(2) != protocol_version)
        throw std::invalid_argument("incompatible lobby protocol");
    Packet p{Op(r.number(1))};
    switch (p.op) {
    case Op::Hello:
        p.release = uint16_t(r.number(2));
        for (auto& byte : p.fingerprint) byte = uint8_t(r.number(1));
        p.name = r.name(); break;
    case Op::Rename: p.name = r.name(); break;
    case Op::Rules: p.pausing = r.boolean(); break;
    case Op::Roster:
        p.identity = r.number(8); p.revision = r.number(8); p.match = r.number(8);
        p.phase = Phase(r.number(1)); p.pausing = r.boolean(); p.count = uint8_t(r.number(1));
        if (p.phase > Phase::Playing || p.count > 4) throw std::invalid_argument("invalid roster");
        for (unsigned i = 0; i < p.count; ++i) {
            auto& player = p.players[i];
            player.id = r.number(8); player.name = r.name(); player.ready = r.boolean();
            if (!player.id) throw std::invalid_argument("invalid player identity");
            for (unsigned j = 0; j < i; ++j)
                if (p.players[j].id == player.id) throw std::invalid_argument("duplicate player identity");
        }
        break;
    case Op::Ready: p.revision = r.number(8); p.ready = r.boolean(); break;
    case Op::Start: p.revision = r.number(8); break;
    case Op::Prepare:
        p.revision = r.number(8); p.match = r.number(8); p.count = uint8_t(r.number(1));
        if (p.count < 2 || p.count > 4 || r.number(2) != 1 || r.number(8) != 0 || r.number(8) != 0 || r.number(1) != 2)
            throw std::invalid_argument("unsupported match profile");
        p.pausing = r.boolean();
        break;
    case Op::Loaded:
    case Op::State:
        p.match = r.number(8); p.frame = r.number(8); p.ram = r.number(8); p.registers = r.number(8); break;
    case Op::Commit: p.match = r.number(8); p.time = r.number(8); break;
    case Op::Cancel:
        p.match = r.number(8); p.reason = Reason(r.number(1));
        if (p.reason != Reason::Cancelled && p.reason != Reason::LoadFailed)
            throw std::invalid_argument("invalid cancellation reason");
        break;
    case Op::Finished:
    case Op::StateChecked:
    case Op::Desync: p.match = r.number(8); p.frame = r.number(8); break;
    case Op::Restart: p.match = r.number(8); break;
    case Op::End: p.match = r.number(8); p.frame = r.number(8); p.reason = Reason(r.number(1)); break;
    case Op::Reject: p.reason = Reason(r.number(1)); break;
    case Op::Ping: p.identity = r.number(8); break;
    case Op::Pong: p.identity = r.number(8); p.time = r.number(8); break;
    default: throw std::invalid_argument("unknown lobby message");
    }
    if (p.reason > Reason::Restarted || !r.end()) throw std::invalid_argument("invalid lobby packet tail");
    return p;
}

inline const char* reason_text(Reason reason) {
    switch (reason) {
    case Reason::None: return "Match complete.";
    case Reason::Protocol: return "Invalid or incompatible protocol.";
    case Reason::Incompatible: return "Client build or game settings do not match this server.";
    case Reason::Full: return "Room full.";
    case Reason::InProgress: return "Match in progress.";
    case Reason::StaleRoster: return "The player list changed. Ready again.";
    case Reason::NotReady: return "Two or more players must be ready.";
    case Reason::NotP1: return "Only Player 1 can start.";
    case Reason::LoadFailed: return "A player could not load the match.";
    case Reason::Mismatch: return "Clients disagree on the match state.";
    case Reason::Timeout: return "The match timed out.";
    case Reason::Left: return "A player disconnected.";
    case Reason::Cancelled: return "A player left or cancelled the match.";
    case Reason::Restarted: return "Player 1 restarted the match.";
    }
    return "Invalid protocol.";
}
}
