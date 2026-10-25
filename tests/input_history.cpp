#include "../src/online/input_history.hpp"
#include "../src/online/input_protocol.hpp"
#include <cassert>
#include <functional>
#include <iostream>
#include <vector>

using namespace online;
template<class F> static void fails(F f) {
    try { f(); assert(false); } catch (const std::exception&) {}
}
static Pad sample(unsigned slot, uint64_t frame) {
    return {uint16_t(((frame / (slot + 2)) % 2 ? 0x8000 : 0x4000) | (1 << slot)),
            int8_t(int((frame * 7 + slot * 31) % 201) - 100), int8_t(int((frame * 13 + slot * 17) % 201) - 100)};
}
static uint64_t advance(uint64_t state, uint64_t frame, const Pads& pads) {
    state = (state ^ frame) * 0x9e3779b185ebca87ULL;
    for (auto p : pads) state = state * 1099511628211ULL ^ (uint64_t(p.buttons) << 16 | uint8_t(p.x) << 8 | uint8_t(p.y));
    return state;
}
static uint64_t reference(unsigned humans, uint64_t end) {
    uint64_t state = 0;
    for (uint64_t frame = 0; frame < end; ++frame) {
        Pads pads{};
        if (frame >= input_delay) for (unsigned slot = 0; slot < humans; ++slot) pads[slot] = sample(slot, frame - input_delay);
        state = advance(state, frame, pads);
    }
    return state;
}
struct Sim {
    RollbackInputs input;
    uint64_t state = 0, corrections = 0, max_depth = 0;
    struct Snapshot { uint64_t frame = UINT64_MAX, state = 0; };
    std::array<Snapshot, checkpoint_slots> snapshots{};
    std::vector<uint64_t> restores;
    Sim(unsigned humans, unsigned slot) : input(1, humans, slot) {}
    void run(uint64_t frame) {
        snapshots[frame % checkpoint_slots] = {frame, state};
        auto pads = input.inputs_for(frame);
        state = advance(state, frame, pads);
    }
    void correct(const std::function<void(uint64_t)>& during = {}) {
        while (auto first = input.take_correction()) {
            ++corrections;
            restores.push_back(*first);
            max_depth = std::max(max_depth, input.head() - *first);
            auto snapshot = snapshots[*first % checkpoint_slots];
            assert(snapshot.frame == *first && input.head() - *first <= prediction_limit);
            state = snapshot.state;
            auto end = input.head();
            for (auto frame = *first; frame < end; ++frame) {
                run(frame);
                if (during) during(frame);
            }
            assert(input.head() == end);
        }
    }
};

static void hostile_inputs() {
    InputRelay relay(1, 2);
    RollbackInputs local(1, 2, 0);
    local.sample(sample(0, 0));
    auto batch = local.upload();
    auto wire = lobby::encode_input(batch, false);
    assert(lobby::encode_input(lobby::decode_input(wire, false), false) == wire);
    assert(wire.size() == 61 && wire.back() == uint8_t(batch.pads[0].y));
    for (size_t size = 0; size < wire.size(); ++size)
        fails([&] { lobby::decode_input({wire.data(), size}, false); });
    fails([&] { lobby::decode_input(wire, true); });
    auto extra = wire; extra.push_back(0);
    fails([&] { lobby::decode_input(extra, false); });
    auto spoof = batch; spoof.slot = 1;
    fails([&] { relay.accept(0, spoof); });
    auto future = batch; future.first = 99;
    fails([&] { relay.accept(0, future); });
    auto invalid = batch; invalid.pads[0].x = -128;
    fails([&] { relay.accept(0, invalid); });
    invalid = batch; invalid.pads[0].buttons = 0x40;
    fails([&] { relay.accept(0, invalid); });
    invalid = batch; invalid.received[1] = 3;
    fails([&] { relay.accept(0, invalid); });
    assert(relay.received(0) == 2); // Failed uploads are atomic.
    auto accepted = relay.accept(0, batch);
    assert(accepted && relay.received(0) == 3 && relay.confirmed() == 2);
    assert(relay.accept(0, batch)); // A repeat still produces a relay.
    auto conflict = batch; conflict.pads[0].buttons ^= 0x8000;
    fails([&] { relay.accept(0, conflict); });
    auto old = batch; old.match = 99;
    assert(!relay.accept(0, old)); local.receive(old);
    accepted->confirmed = 99;
    fails([&] { local.receive(*accepted); });
    assert(local.confirmed() == 2);
    auto invented = *accepted; invented.confirmed = 2; invented.first = 3;
    fails([&] { local.receive(invented); });
    assert(!local.sample(sample(0, 1))); // New physical value while stalled is ignored.
    assert(local.upload().pads[0] == sample(0, 0));
    fails([&] { local.inputs_for(1); });
}

static void arrival_during_replay() {
    Sim client(4, 0);
    for (unsigned frame = 0; frame < 10; ++frame) {
        client.input.sample(sample(0, frame)); client.run(frame);
    }
    auto remote = [](unsigned slot) {
        InputBatch batch; batch.match = 1; batch.slot = uint8_t(slot); batch.count = 8;
        for (unsigned i = 0; i < batch.count; ++i) batch.pads[i] = sample(slot, i);
        return batch;
    };
    client.input.receive(remote(1)); // P2 first differs at input 2.
    bool arrived = false;
    client.correct([&](uint64_t frame) {
        if (frame == 3 && !arrived) { client.input.receive(remote(2)); arrived = true; }
    });
    assert(arrived && (client.restores == std::vector<uint64_t>{2, 2}));
    client.input.receive(remote(3)); client.correct();
    assert(client.state == reference(4, 10) && client.max_depth == 8);
    std::cout << "PASS: P3 arrival during P2 correction requests a new earliest restore\n";
}

static void stalled_retransmission() {
    InputRelay relay(1, 4);
    std::vector<Sim> clients;
    for (unsigned slot = 0; slot < 4; ++slot) clients.emplace_back(4, slot);
    auto deliver = [&](bool drop_p3) {
        for (unsigned source = 0; source < 4; ++source) {
            auto relayed = relay.accept(source, clients[source].input.upload()); assert(relayed);
            for (unsigned destination = 0; destination < 4; ++destination)
                if (!drop_p3 || destination != 2) clients[destination].input.receive(*relayed);
        }
        for (auto& client : clients) client.correct();
    };
    for (unsigned frame = 0; frame < input_delay + prediction_limit; ++frame) {
        for (unsigned slot = 0; slot < 4; ++slot) clients[slot].input.sample(sample(slot, frame));
        deliver(true);
        for (auto& client : clients) client.run(frame);
    }
    for (unsigned slot = 0; slot < 4; ++slot) clients[slot].input.sample(sample(slot, clients[slot].input.head()));
    deliver(true);
    for (auto& client : clients) assert(!client.input.can_advance() && client.input.head() == 14);
    assert(relay.received(0) == 17 && clients[2].input.received(0) == 2 && relay.confirmed() == 2);
    // No new sample or simulation update is allowed to make this recover.
    for (unsigned retry = 0; retry < 3; ++retry) deliver(false);
    assert(relay.confirmed() == 17);
    for (auto& client : clients) {
        assert(client.input.can_advance() && client.input.head() == 14);
        assert(client.state == reference(4, 14));
    }
    assert(clients[2].max_depth == prediction_limit);
    std::cout << "PASS: already-accepted input repairs server-to-P3 loss while all four simulations stay stalled\n";
}

// Each delivery has its own destination and time. The server never waits for a
// complete frame; the simulated game is merely an order-sensitive test oracle.
static void independent_delivery(unsigned humans) {
    InputRelay relay(1, humans);
    std::vector<Sim> clients;
    for (unsigned i = 0; i < humans; ++i) clients.emplace_back(humans, i);
    struct Delivery { uint64_t due; unsigned destination; InputBatch batch; };
    std::vector<Delivery> pending;
    unsigned stalled = 0, repaired_p3 = 0, verified = 0;
    uint64_t accepted_during_loss = 0;
    for (uint64_t tick = 0; tick < 1300; ++tick) {
        // A deterministic network clock continues even when every game is stalled.
        for (unsigned source = 0; source < humans; ++source) {
            auto& client = clients[source];
            if (client.input.head() < 400) client.input.sample(sample(source, client.input.head()));
            auto upload = client.input.upload();
            if (source == 1 && tick % 61 < 20) continue; // Independently late P2 uploads.
            auto relayed = relay.accept(source, upload); assert(relayed);
            if (tick >= 100 && tick < 150) accepted_during_loss = std::max(accepted_during_loss, relay.received(0));
            for (unsigned destination = 0; destination < humans; ++destination) {
                if (destination == 2 && tick >= 100 && tick < 150) continue; // Only server-to-P3 is lost.
                auto delay = (source * 7 + destination * 11 + tick) % 6;
                if (source == 3) delay = tick % 2 ? 0 : 9; // Reorder P4 separately.
                pending.push_back({tick + delay, destination, *relayed});
                if (source == 3 && tick % 13 == 0) pending.push_back({tick + delay + 1, destination, *relayed});
            }
        }
        assert(pending.size() < 256);
        std::vector<Delivery> due;
        for (auto i = pending.begin(); i != pending.end();) {
            if (i->due <= tick) { due.push_back(*i); i = pending.erase(i); }
            else ++i;
        }
        // Reverse delivery order periodically; no stream depends on another's order.
        if (tick % 3 == 0) std::reverse(due.begin(), due.end());
        for (auto& delivery : due) clients[delivery.destination].input.receive(delivery.batch);
        for (unsigned slot = 0; slot < humans; ++slot) {
            auto& client = clients[slot];
            client.correct();
            if (client.input.head() <= client.input.confirmed()) {
                assert(client.state == reference(humans, client.input.head())); ++verified;
            }
            if (client.input.head() < 400) {
                if (client.input.can_advance()) client.run(client.input.head());
                else ++stalled;
            }
            assert(client.input.head() <= client.input.confirmed() + prediction_limit);
            if (slot == 2 && tick > 150 && client.input.received(0) >= accepted_during_loss) ++repaired_p3;
        }
    }
    for (auto& client : clients) {
        client.correct();
        assert(client.input.head() == 400 && client.input.confirmed() >= 400);
        assert(client.state == reference(humans, 400));
        assert(client.corrections > 10 && client.max_depth <= prediction_limit);
    }
    assert(stalled > 0);
    assert(verified > 100);
    if (humans >= 3) assert(repaired_p3 > 0 && accepted_during_loss > 2);
    std::cout << "PASS: " << humans << " independent clients, 400 corrected updates, bounded stalls and relay-loss recovery\n";
}

int main() {
    for (int axis = -127; axis <= 127; ++axis) assert(int8_t(127 * pad_axis(int8_t(axis))) == axis);
    assert((quantize_pad(0xffff, 2, -2) == Pad{0xff3f, 127, -127}));
    fails([] { quantize_pad(0, std::numeric_limits<float>::quiet_NaN(), 0); });
    hostile_inputs();
    arrival_during_replay();
    stalled_retransmission();
    for (unsigned humans : {2, 3, 4}) independent_delivery(humans);
}
