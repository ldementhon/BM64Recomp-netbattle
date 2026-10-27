#include "../src/online/lobby_room.hpp"
#include <cassert>
#include <iostream>

using namespace online::lobby;
static Fingerprint fingerprint{42};
static Packet hello() { Packet p{Op::Hello}; p.name = "Player"; p.fingerprint = fingerprint; return p; }
static Packet command(Op op, const Room& room) {
    Packet p{op}; p.revision = room.revision(); p.match = room.match(); return p;
}
static void ready_all(Room& room) {
    for (unsigned i = 0; i < room.count(); ++i) {
        auto ready = command(Op::Ready, room); ready.ready = true;
        auto out = room.handle(room.players()[i].id, ready, 100);
        assert(out.size() == room.count());
    }
}
static void start(Room& room, uint64_t now = 100) {
    ready_all(room);
    auto out = room.handle(room.players()[0].id, command(Op::Start, room), now);
    assert(room.phase() == Phase::Loading && out.size() == 2 * room.count());
    for (auto& delivery : out) assert(encode(decode(encode(delivery.packet))) == encode(delivery.packet));
}
static Packet loaded(const Room& room) {
    auto p = command(Op::Loaded, room); p.frame = 1374; p.ram = 123; p.registers = 456; return p;
}
static void reject(const std::vector<Delivery>& out, Reason reason, bool closed = false) {
    assert(out.size() == 1 && out[0].packet.op == Op::Reject && out[0].packet.reason == reason && out[0].disconnect == closed);
}

int main() {
    assert(valid_name("Louis") && valid_name("\xc3\xa9"));
    for (const auto& name : {std::string{}, std::string(" "), std::string(33, 'a'), std::string("a\nb"),
                            std::string("\xc0\x80"), std::string("\xed\xa0\x80"), std::string("\xf4\x90\x80\x80")})
        assert(!valid_name(name));
    auto h = hello();
    assert(decode(encode(h)).name == h.name && decode(encode(h)).fingerprint == fingerprint);
    Packet ready{Op::Ready}; ready.revision = 0x0102030405060708; ready.ready = true;
    auto wire = encode(ready);
    assert((wire == std::vector<uint8_t>{'B','M','6','4',0,1,3,1,2,3,4,5,6,7,8,1}));
    auto malformed = [&](const std::vector<uint8_t>& bytes) {
        try { decode(bytes); assert(false); } catch (const std::invalid_argument&) {}
    };
    for (size_t i = 0; i < wire.size(); ++i) malformed({wire.begin(), wire.begin() + i});
    auto bad = wire; bad.push_back(0); malformed(bad);
    bad = wire; bad.back() = 2; malformed(bad);
    bad = wire; bad[5] = 2; malformed(bad);
    bad = wire; bad[6] = 255; malformed(bad);
    malformed(std::vector<uint8_t>(packet_limit + 1));
    Packet cancel_wire{Op::Cancel}; cancel_wire.match = 7; cancel_wire.reason = Reason::LoadFailed;
    assert(decode(encode(cancel_wire)).reason == Reason::LoadFailed);
    cancel_wire.reason = Reason::Cancelled;
    assert(decode(encode(cancel_wire)).reason == Reason::Cancelled);
    cancel_wire.reason = Reason::None;
    malformed(encode(cancel_wire));

    {
        Fingerprint windows{43}, other{44};
        Room room(std::vector<Fingerprint>{fingerprint, windows});
        auto mac = hello(); auto pc = hello(); pc.fingerprint = windows; auto stranger = hello(); stranger.fingerprint = other;
        reject(room.handle(9, stranger, 0), Reason::Incompatible, true);
        assert(room.handle(1, mac, 0).size() == 1 && room.handle(2, pc, 0).size() == 2 && room.count() == 2);
        for (auto bad : {std::vector<Fingerprint>{}, std::vector<Fingerprint>{windows, windows},
                         std::vector<Fingerprint>(5, Fingerprint{})}) {
            try { Room invalid(bad); assert(false); } catch (const std::invalid_argument&) {}
        }
    }

    for (unsigned count : {2, 3, 4}) {
        Room room(fingerprint);
        auto wrong = hello(); wrong.fingerprint[0] ^= 1;
        reject(room.handle(99, wrong, 0), Reason::Incompatible, true);
        assert(room.count() == 0);
        for (unsigned i = 1; i <= count; ++i) {
            auto out = room.handle(i, hello(), 0);
            assert(out.size() == i && room.count() == i);
            for (auto& item : out) {
                auto roster = decode(encode(item.packet));
                assert(roster.count == i && roster.identity == item.peer);
                for (unsigned j = 0; j < i; ++j) assert(roster.players[j].id == j + 1);
            }
        }
        if (count == 4) reject(room.handle(5, hello(), 0), Reason::Full, true);
        reject(room.handle(1, hello(), 0), Reason::Protocol, true);
        reject(room.handle(1, command(Op::Start, room), 0), Reason::NotReady);
        reject(room.handle(2, command(Op::Start, room), 0), Reason::NotP1);
        auto stale = command(Op::Ready, room); --stale.revision; stale.ready = true;
        reject(room.handle(1, stale, 0), Reason::StaleRoster);
        start(room);
        reject(room.handle(5, hello(), 0), Reason::InProgress, true);
        auto ack = loaded(room);
        for (unsigned i = 1; i <= count; ++i) {
            auto out = room.handle(i, ack, 200);
            assert(out.size() <= 8);
            assert(room.phase() == (i == count ? Phase::Playing : Phase::Loading));
            if (i == count) assert(out.back().packet.op == Op::Commit && out.back().packet.time == 1200);
        }
        auto ended = command(Op::Finished, room); ended.frame = 5401;
        for (unsigned i = 1; i <= count; ++i) room.handle(i, ended, 1300);
        assert(room.phase() == Phase::Lobby && room.count() == count);
        for (unsigned i = 0; i < count; ++i) assert(!room.players()[i].ready);
        auto old_match = room.match();
        start(room, 2000);
        assert(room.match() == old_match + 1);
        assert(room.handle(1, ack, 2001).empty());
        assert(room.phase() == Phase::Loading);
        room.disconnect(1);
        assert(room.phase() == Phase::Lobby && room.players()[0].id == 2 && room.count() == count - 1);
    }
    Room room(fingerprint);
    room.handle(1, hello(), 0); room.handle(2, hello(), 0);
    ready_all(room);
    auto stale_start = command(Op::Start, room);
    room.handle(3, hello(), 0);
    assert(!room.players()[0].ready && !room.players()[1].ready);
    reject(room.handle(1, stale_start, 0), Reason::StaleRoster);
    start(room);
    auto ack = loaded(room);
    room.handle(1, ack, 101);
    room.handle(1, ack, 102); // Idempotent loading acknowledgment.
    ++ack.registers;
    auto mismatch = room.handle(2, ack, 103);
    assert(room.phase() == Phase::Lobby && mismatch[0].packet.reason == Reason::Mismatch);
    start(room);
    assert(room.tick(30099).empty());
    assert(room.tick(30100)[0].packet.reason == Reason::Timeout && room.phase() == Phase::Lobby);
    start(room);
    auto cancel = command(Op::Cancel, room);
    cancel.reason = Reason::Cancelled;
    assert(room.handle(2, cancel, 101)[0].packet.reason == Reason::Cancelled);
    start(room);
    ack = loaded(room);
    for (unsigned i = 1; i <= 3; ++i) room.handle(i, ack, 200);
    auto finish = command(Op::Finished, room); finish.frame = 5401;
    room.handle(1, finish, 1300);
    assert(room.tick(6299).empty());
    assert(room.tick(6300)[0].packet.reason == Reason::Timeout);
    start(room);
    for (unsigned i = 1; i <= 3; ++i) room.handle(i, loaded(room), 200);
    auto dropped = room.disconnect(2);
    assert(dropped[0].packet.reason == Reason::Left && room.players()[1].id == 3);
    auto ping = Packet{Op::Ping}; ping.identity = 42;
    auto pong = room.handle(99, ping, 1234);
    assert(pong[0].packet.op == Op::Pong && pong[0].packet.time == 1234 && pong[0].packet.identity == 42);
    std::cout << "PASS: bounded wire format, compatibility, 2/3/4-seat roster, revisioned readiness, load agreement, repeat matches and disconnects\n";
}
