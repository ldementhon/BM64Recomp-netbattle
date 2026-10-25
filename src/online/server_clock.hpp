#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace online {
// All times are monotonic milliseconds. The lowest-RTT fresh sample minimizes
// the uncertainty of the midpoint estimate; wall clocks never enter simulation.
class ServerClock {
public:
    void sample(uint64_t sent, uint64_t received, uint64_t server) {
        constexpr auto limit = uint64_t(std::numeric_limits<int64_t>::max());
        if (received < sent || received > limit || server > limit || received - sent > 1000)
            throw std::invalid_argument("invalid or excessive clock round trip");
        if (count_ && received < latest_) throw std::invalid_argument("local clock moved backwards");
        auto midpoint = sent + (received - sent) / 2;
        samples_[next_] = {received, received - sent, int64_t(server) - int64_t(midpoint)};
        next_ = (next_ + 1) % samples_.size();
        if (count_ < samples_.size()) ++count_;
        latest_ = received;
    }

    struct Estimate { int64_t offset; uint64_t round_trip; };
    std::optional<Estimate> estimate(uint64_t now) const {
        if (now < latest_) return std::nullopt;
        const Sample* best = nullptr;
        unsigned fresh = 0;
        for (size_t i = 0; i < count_; ++i) {
            const auto& candidate = samples_[i];
            if (now - candidate.received > 5000) continue;
            ++fresh;
            if (!best || candidate.round_trip < best->round_trip ||
                (candidate.round_trip == best->round_trip && candidate.received > best->received)) best = &candidate;
        }
        if (fresh < 3) return std::nullopt;
        return Estimate{best->offset, best->round_trip};
    }

    uint64_t local_start(uint64_t server_epoch, uint64_t now) const {
        auto clock = estimate(now);
        if (!clock) throw std::runtime_error("waiting for server clock measurements");
        uint64_t local;
        if (clock->offset >= 0) {
            if (server_epoch < uint64_t(clock->offset)) throw std::runtime_error("invalid server start epoch");
            local = server_epoch - uint64_t(clock->offset);
        } else {
            auto delta = uint64_t(-clock->offset);
            if (server_epoch > std::numeric_limits<uint64_t>::max() - delta)
                throw std::runtime_error("invalid server start epoch");
            local = server_epoch + delta;
        }
        // Reserve the measured uncertainty plus a small service margin. A late
        // client must cancel/reload, never start behind or silently skip frames.
        if (local <= now || local - now <= (clock->round_trip + 1) / 2 + 10)
            throw std::runtime_error("start arrived too late");
        if (local - now > 3000) throw std::runtime_error("server start epoch is too far ahead");
        return local;
    }
private:
    struct Sample { uint64_t received = 0, round_trip = 0; int64_t offset = 0; };
    std::array<Sample, 8> samples_{};
    size_t count_ = 0, next_ = 0;
    uint64_t latest_ = 0;
};
}
