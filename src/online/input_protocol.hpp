#pragma once
#include "input_history.hpp"
#include "lobby_protocol.hpp"

namespace online::lobby {
inline std::vector<uint8_t> encode_input(const InputBatch& batch, bool relay) {
    validate_batch(batch);
    Writer w;
    w.number(0x424d3649, 4); w.number(protocol_version, 2); w.number(relay ? 2 : 1, 1);
    w.number(batch.match, 8); w.number(batch.slot, 1); w.number(batch.first, 8); w.number(batch.count, 1);
    if (relay) w.number(batch.confirmed, 8);
    else for (auto frontier : batch.received) w.number(frontier, 8);
    for (unsigned i = 0; i < batch.count; ++i) {
        w.number(batch.pads[i].buttons, 2);
        w.number(uint8_t(batch.pads[i].x), 1); w.number(uint8_t(batch.pads[i].y), 1);
    }
    if (w.bytes.size() > packet_limit) throw std::logic_error("input packet exceeds transport cap");
    return w.bytes;
}
inline InputBatch decode_input(std::span<const uint8_t> bytes, bool relay) {
    if (bytes.size() > packet_limit) throw std::invalid_argument("oversized input packet");
    Reader r(bytes);
    if (r.number(4) != 0x424d3649 || r.number(2) != protocol_version || r.number(1) != (relay ? 2 : 1))
        throw std::invalid_argument("invalid input protocol or direction");
    InputBatch batch;
    batch.match = r.number(8); batch.slot = uint8_t(r.number(1));
    batch.first = r.number(8); batch.count = uint8_t(r.number(1));
    if (batch.count > input_slots) throw std::invalid_argument("oversized input history");
    if (relay) batch.confirmed = r.number(8);
    else for (auto& frontier : batch.received) frontier = r.number(8);
    auto axis = [&] { int value = int(r.number(1)); return int8_t(value < 128 ? value : value - 256); };
    for (unsigned i = 0; i < batch.count; ++i) {
        batch.pads[i].buttons = uint16_t(r.number(2)); batch.pads[i].x = axis(); batch.pads[i].y = axis();
    }
    validate_batch(batch);
    if (!r.end()) throw std::invalid_argument("unexpected input packet tail");
    return batch;
}
}
