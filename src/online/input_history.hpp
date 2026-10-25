#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace online {
constexpr unsigned input_delay = 2, prediction_limit = 12, checkpoint_slots = 16, input_slots = 32;
constexpr uint64_t last_input_frame = std::numeric_limits<uint64_t>::max() - 64;
static_assert(prediction_limit + input_delay + 1 < checkpoint_slots);
static_assert(2 * prediction_limit + 2 * input_delay + 3 <= input_slots);

struct Pad {
    uint16_t buttons = 0;
    int8_t x = 0, y = 0;
    bool operator==(const Pad&) const = default;
    bool valid() const { return !(buttons & 0x00c0) && x != -128 && y != -128; }
};
using Pads = std::array<Pad, 4>;
inline Pad quantize_pad(uint16_t buttons, float x, float y) {
    if (!std::isfinite(x) || !std::isfinite(y)) throw std::invalid_argument("nonfinite controller axis");
    return {uint16_t(buttons & 0xff3f), int8_t(127 * std::clamp(x, -1.0f, 1.0f)), int8_t(127 * std::clamp(y, -1.0f, 1.0f))};
}
inline float pad_axis(int8_t axis) {
    // The runtime truncates 127*x. Stay inside the integer's bin after float
    // rounding instead of occasionally turning (axis/127)*127 into axis-1.
    if (!axis) return 0;
    return std::clamp((float(axis) + (axis > 0 ? 0.25f : -0.25f)) / 127.0f, -1.0f, 1.0f);
}
struct InputBatch {
    uint64_t match = 0, first = input_delay;
    uint8_t slot = 0, count = 0;
    std::array<Pad, input_slots> pads{};
    std::array<uint64_t, 4> received{}; // Exclusive contiguous frontiers, uploads only.
    uint64_t confirmed = input_delay; // Minimum of all human receiver/stream frontiers, relays only.
};
inline void validate_batch(const InputBatch& batch) {
    if (!batch.match || batch.slot >= 4 || batch.count > input_slots || batch.first < input_delay ||
        batch.first > last_input_frame || batch.count > last_input_frame - batch.first)
        throw std::invalid_argument("invalid input batch range");
    for (unsigned i = 0; i < batch.count; ++i)
        if (!batch.pads[i].valid()) throw std::invalid_argument("invalid controller input");
}

class InputStream {
public:
    InputStream() { insert(0, {}, 0); insert(1, {}, 0); }
    std::optional<Pad> get(uint64_t frame) const {
        const auto& entry = entries_[frame % input_slots];
        if (entry.frame == frame) return entry.pad;
        return {};
    }
    void insert(uint64_t frame, Pad pad, uint64_t protect) {
        auto& entry = entries_[frame % input_slots];
        if (entry.frame == frame) {
            if (entry.pad != pad) throw std::invalid_argument("conflicting input replacement");
            return;
        }
        if (frame < protect) return;
        if (entry.frame != empty && entry.frame >= protect)
            throw std::runtime_error("input history would overwrite a protected frame");
        entry = {frame, pad};
        while (get(contiguous_)) ++contiguous_;
    }
    uint64_t contiguous() const { return contiguous_; }
    Pad predict(uint64_t frame) const {
        for (unsigned age = 0; age < input_slots && age <= frame; ++age)
            if (auto pad = get(frame - age)) return *pad;
        throw std::runtime_error("prediction lost its last known input");
    }
private:
    static constexpr uint64_t empty = std::numeric_limits<uint64_t>::max();
    struct Entry { uint64_t frame = empty; Pad pad{}; };
    std::array<Entry, input_slots> entries_{};
    uint64_t contiguous_ = 0;
};

// No simulation on the server. A receipt only becomes globally confirmed after
// every human client reports receiving a contiguous prefix of every human stream.
class InputRelay {
public:
    InputRelay(uint64_t match, unsigned humans) : match_(match), humans_(humans) {
        if (!match || humans < 2 || humans > 4) throw std::invalid_argument("invalid relay match");
        for (unsigned peer = 0; peer < humans; ++peer)
            for (unsigned stream = 0; stream < humans; ++stream) received_[peer][stream] = input_delay;
    }
    uint64_t match() const { return match_; }
    uint64_t confirmed() const { return confirmed_; }
    uint64_t received(unsigned slot) const { return streams_.at(slot).contiguous(); }
    std::optional<InputBatch> accept(unsigned sender, const InputBatch& batch) {
        if (batch.match != match_) return {};
        validate_batch(batch);
        if (sender >= humans_ || batch.slot != sender) throw std::invalid_argument("input stream does not belong to sender");
        if (batch.first + batch.count > confirmed_ + prediction_limit + input_delay + 1)
            throw std::invalid_argument("input exceeds confirmed lead bound");
        auto stream = streams_[sender];
        for (unsigned i = 0; i < batch.count; ++i) stream.insert(batch.first + i, batch.pads[i], confirmed_ - 1);
        auto acknowledgments = received_[sender];
        for (unsigned i = 0; i < 4; ++i) {
            auto limit = i == sender ? stream.contiguous() : streams_[i].contiguous();
            if (i >= humans_ ? batch.received[i] != 0 : (batch.received[i] < input_delay || batch.received[i] > limit))
                throw std::invalid_argument("input acknowledgment exceeds received history");
            acknowledgments[i] = std::max(acknowledgments[i], batch.received[i]);
        }
        // Validate the whole upload before changing either inputs or receipts.
        streams_[sender] = stream; received_[sender] = acknowledgments;
        auto confirmed = last_input_frame;
        for (unsigned peer = 0; peer < humans_; ++peer)
            for (unsigned i = 0; i < humans_; ++i) confirmed = std::min(confirmed, received_[peer][i]);
        confirmed_ = confirmed;
        auto result = batch; result.received = {}; result.confirmed = confirmed_;
        // Identical retransmissions are deliberately forwarded again: server
        // receipt does not prove the other clients received the first relay.
        return result;
    }
private:
    uint64_t match_, confirmed_ = input_delay;
    unsigned humans_;
    std::array<InputStream, 4> streams_{};
    std::array<std::array<uint64_t, 4>, 4> received_{};
};

// Boundary F is the state before consuming input F. F=0/1 are neutral; the
// physical sample at F is immutable input F+2. This object is never checkpointed.
class RollbackInputs {
public:
    RollbackInputs(uint64_t match, unsigned humans, unsigned local)
        : match_(match), humans_(humans), local_(local) {
        if (!match || humans < 2 || humans > 4 || local >= humans) throw std::invalid_argument("invalid local input assignment");
    }
    uint64_t head() const { return head_; }
    uint64_t confirmed() const { return confirmed_; }
    uint64_t received(unsigned slot) const { return streams_.at(slot).contiguous(); }
    bool can_advance() const { return !correction_ && head_ < confirmed_ + prediction_limit && head_ < last_input_frame - input_delay; }
    bool needs_sample() const { return !streams_[local_].get(head_ + input_delay); }
    bool sample(Pad pad) {
        if (!pad.valid()) throw std::invalid_argument("invalid sampled input");
        if (head_ >= last_input_frame - input_delay) throw std::runtime_error("input frame limit reached");
        auto target = head_ + input_delay;
        if (streams_[local_].get(target)) return false; // Stalled/repeated reads cannot resample.
        streams_[local_].insert(target, pad, protect());
        return true;
    }
    InputBatch upload() const {
        InputBatch batch; batch.match = match_; batch.slot = uint8_t(local_);
        auto end = streams_[local_].contiguous();
        batch.first = end > input_slots + input_delay ? end - input_slots : input_delay;
        batch.count = uint8_t(end - batch.first);
        for (unsigned i = 0; i < batch.count; ++i) {
            auto pad = streams_[local_].get(batch.first + i);
            if (!pad) throw std::runtime_error("local resend history has a hole");
            batch.pads[i] = *pad;
        }
        for (unsigned i = 0; i < humans_; ++i) batch.received[i] = streams_[i].contiguous();
        return batch;
    }
    void receive(const InputBatch& batch) {
        if (batch.match != match_) return;
        validate_batch(batch);
        if (batch.slot >= humans_ || batch.confirmed < input_delay || batch.confirmed > last_input_frame ||
            batch.first + batch.count > std::max(confirmed_, batch.confirmed) + prediction_limit + input_delay + 1)
            throw std::invalid_argument("invalid relayed input bounds");
        auto stream = streams_[batch.slot];
        auto correction = correction_;
        for (unsigned i = 0; i < batch.count; ++i) {
            auto frame = batch.first + i;
            if (batch.slot == local_ && frame >= protect() && !streams_[local_].get(frame))
                throw std::invalid_argument("server cannot invent local controller input");
            stream.insert(frame, batch.pads[i], protect());
            const auto& used = used_[frame % checkpoint_slots];
            if (frame < head_ && used.frame == frame && used.pads[batch.slot] != batch.pads[i]) {
                if (head_ - frame > prediction_limit) throw std::runtime_error("correction exceeds checkpoint lifetime");
                correction = correction ? std::min(*correction, frame) : frame;
            }
        }
        for (unsigned i = 0; i < humans_; ++i)
            if (batch.confirmed > (i == batch.slot ? stream.contiguous() : streams_[i].contiguous()))
                throw std::invalid_argument("server confirmation exceeds local received inputs");
        streams_[batch.slot] = stream; correction_ = correction;
        confirmed_ = std::max(confirmed_, batch.confirmed);
    }
    std::optional<uint64_t> take_correction() {
        auto frame = correction_; correction_.reset(); return frame;
    }
    // The caller restores checkpoint F before replaying F..head-1. New arrivals
    // during replay may request another earlier restore; head never moves back.
    Pads inputs_for(uint64_t frame) {
        if (frame > head_ || (frame < head_ && head_ - frame > prediction_limit))
            throw std::logic_error("input requested outside rollback window");
        if (frame == head_ && (!can_advance() || !streams_[local_].get(head_ + input_delay)))
            throw std::logic_error("must sample or resolve correction before advancing");
        Pads pads{};
        for (unsigned i = 0; i < humans_; ++i) pads[i] = streams_[i].predict(frame);
        used_[frame % checkpoint_slots] = {frame, pads};
        if (frame == head_) ++head_;
        return pads;
    }
private:
    uint64_t match_, head_ = 0, confirmed_ = input_delay;
    unsigned humans_, local_;
    std::array<InputStream, 4> streams_{};
    struct Used { uint64_t frame = std::numeric_limits<uint64_t>::max(); Pads pads{}; };
    std::array<Used, checkpoint_slots> used_{};
    std::optional<uint64_t> correction_;
    uint64_t protect() const { return head_ > prediction_limit + 1 ? head_ - prediction_limit - 1 : 0; }
};
}
