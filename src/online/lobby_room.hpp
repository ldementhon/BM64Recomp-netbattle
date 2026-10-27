#pragma once
#include "lobby_protocol.hpp"
#include <algorithm>
#include <optional>
#include <stdexcept>
#include <vector>

namespace online::lobby {
struct Delivery { uint64_t peer; Packet packet; bool disconnect = false; };

// The transport serializes calls. Peer IDs come from the server, never a packet.
class Room {
public:
    // Accepted client builds: one exact build, or a platform build set whose
    // histories were validated together (for example one Mac and one Windows build).
    explicit Room(std::vector<Fingerprint> accepted) : accepted_(std::move(accepted)) {
        if (accepted_.empty() || accepted_.size() > 4) throw std::invalid_argument("room accepts 1 to 4 client builds");
        for (size_t i = 0; i < accepted_.size(); ++i)
            for (size_t j = i + 1; j < accepted_.size(); ++j)
                if (accepted_[i] == accepted_[j]) throw std::invalid_argument("duplicate accepted client build");
    }
    explicit Room(Fingerprint fingerprint) : Room(std::vector<Fingerprint>{fingerprint}) {}
    Phase phase() const { return phase_; }
    uint64_t revision() const { return revision_; }
    uint64_t match() const { return match_; }
    uint64_t start_epoch() const { return start_epoch_; }
    unsigned count() const { return count_; }
    const std::array<Player, 4>& players() const { return players_; }
    uint64_t initial_frame() const { return loaded_[0] ? loaded_[0]->frame : 0; }
    std::vector<Delivery> abort(Reason reason) {
        std::vector<Delivery> out;
        if (phase_ != Phase::Lobby) end(out, reason);
        return out;
    }

    std::vector<Delivery> handle(uint64_t peer, const Packet& p, uint64_t now) {
        std::vector<Delivery> out;
        auto reject = [&](Reason reason, bool close = false) {
            Packet error{Op::Reject}; error.reason = reason;
            out.push_back({peer, error, close});
        };
        unsigned slot = find(peer);
        if (p.op == Op::Ping) {
            Packet pong{Op::Pong}; pong.identity = p.identity; pong.time = now;
            return {{peer, pong}};
        }
        if (p.op == Op::Hello) {
            if (!peer || slot != count_ || !valid_name(p.name)) reject(Reason::Protocol, true);
            else if (std::find(accepted_.begin(), accepted_.end(), p.fingerprint) == accepted_.end()) reject(Reason::Incompatible, true);
            else if (phase_ != Phase::Lobby) reject(Reason::InProgress, true);
            else if (count_ == 4) reject(Reason::Full, true);
            else {
                players_[count_++] = {peer, p.name, false};
                changed_roster(out);
            }
            return out;
        }
        if (slot == count_) { reject(Reason::Protocol, true); return out; }
        if (p.op == Op::Ready || p.op == Op::Start) {
            if (phase_ != Phase::Lobby) reject(Reason::InProgress);
            else if (p.revision != revision_) reject(Reason::StaleRoster);
            else if (p.op == Op::Ready) {
                if (players_[slot].ready != p.ready) {
                    players_[slot].ready = p.ready;
                    publish(out);
                }
            } else if (slot != 0) reject(Reason::NotP1);
            else if (count_ < 2 || !std::all_of(players_.begin(), players_.begin() + count_,
                                             [](const Player& player) { return player.ready; })) reject(Reason::NotReady);
            else {
                ++match_;
                phase_ = Phase::Loading;
                deadline_ = now + 30000;
                loaded_.fill(std::nullopt); finished_.fill(std::nullopt);
                publish(out);
                Packet prepare{Op::Prepare};
                prepare.revision = revision_; prepare.match = match_; prepare.count = uint8_t(count_);
                broadcast(out, prepare);
            }
            return out;
        }
        if (p.op != Op::Loaded && p.op != Op::Cancel && p.op != Op::Finished) {
            reject(Reason::Protocol, true); return out;
        }
        if (p.match != match_ || phase_ == Phase::Lobby) return out;
        if (p.op == Op::Cancel) {
            end(out, p.reason == Reason::LoadFailed ? Reason::LoadFailed : Reason::Cancelled);
        } else if (p.op == Op::Loaded && phase_ == Phase::Loading) {
            Digest digest{p.frame, p.ram, p.registers};
            if (!p.frame || (loaded_[slot] && *loaded_[slot] != digest)) end(out, Reason::Mismatch);
            else {
                loaded_[slot] = digest;
                for (unsigned i = 0; i < count_; ++i) {
                    if (loaded_[i] && *loaded_[i] != digest) { end(out, Reason::Mismatch); return out; }
                }
                if (std::all_of(loaded_.begin(), loaded_.begin() + count_, [](const auto& d) { return d.has_value(); })) {
                    phase_ = Phase::Playing;
                    deadline_ = 0;
                    start_epoch_ = now + 1000;
                    publish(out);
                    Packet commit{Op::Commit}; commit.match = match_; commit.time = start_epoch_;
                    broadcast(out, commit);
                }
            }
        } else if (p.op == Op::Finished && phase_ == Phase::Playing) {
            if (now < start_epoch_ || p.frame <= loaded_[0]->frame) { end(out, Reason::Mismatch); return out; }
            for (unsigned i = 0; i < count_; ++i) {
                if (finished_[i] && *finished_[i] != p.frame) { end(out, Reason::Mismatch); return out; }
            }
            finished_[slot] = p.frame;
            if (!deadline_) deadline_ = now + 5000;
            if (std::all_of(finished_.begin(), finished_.begin() + count_, [](const auto& f) { return f.has_value(); }))
                end(out, Reason::None, p.frame);
        } else reject(Reason::Protocol);
        return out;
    }

    std::vector<Delivery> disconnect(uint64_t peer) {
        std::vector<Delivery> out;
        auto slot = find(peer);
        if (slot == count_) return out;
        for (unsigned i = slot + 1; i < count_; ++i) players_[i - 1] = std::move(players_[i]);
        players_[--count_] = {};
        if (phase_ != Phase::Lobby) end(out, Reason::Left);
        else changed_roster(out);
        return out;
    }

    std::vector<Delivery> tick(uint64_t now) {
        std::vector<Delivery> out;
        if (phase_ != Phase::Lobby && deadline_ && now >= deadline_) end(out, Reason::Timeout);
        return out;
    }

private:
    struct Digest {
        uint64_t frame, ram, registers;
        bool operator==(const Digest&) const = default;
    };
    std::vector<Fingerprint> accepted_;
    std::array<Player, 4> players_{};
    std::array<std::optional<Digest>, 4> loaded_{};
    std::array<std::optional<uint64_t>, 4> finished_{};
    unsigned count_ = 0;
    Phase phase_ = Phase::Lobby;
    uint64_t revision_ = 0, match_ = 0, deadline_ = 0, start_epoch_ = 0;

    unsigned find(uint64_t peer) const {
        unsigned slot = 0;
        while (slot < count_ && players_[slot].id != peer) ++slot;
        return slot;
    }
    void broadcast(std::vector<Delivery>& out, const Packet& packet) const {
        for (unsigned i = 0; i < count_; ++i) out.push_back({players_[i].id, packet});
    }
    void publish(std::vector<Delivery>& out) const {
        Packet roster{Op::Roster};
        roster.revision = revision_; roster.match = match_; roster.phase = phase_;
        roster.players = players_; roster.count = uint8_t(count_);
        for (unsigned i = 0; i < count_; ++i) {
            roster.identity = players_[i].id;
            out.push_back({roster.identity, roster});
        }
    }
    void changed_roster(std::vector<Delivery>& out) {
        ++revision_;
        for (auto& player : players_) player.ready = false;
        publish(out);
    }
    void end(std::vector<Delivery>& out, Reason reason, uint64_t frame = 0) {
        Packet end{Op::End}; end.match = match_; end.reason = reason; end.frame = frame;
        broadcast(out, end);
        phase_ = Phase::Lobby; deadline_ = 0;
        loaded_.fill(std::nullopt); finished_.fill(std::nullopt);
        changed_roster(out);
    }
};
}
