#include "../src/online/match_channel.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace online;
    auto [owner, child] = MatchChannel::pair();
    MatchMessage ready{1, MatchEvent::Ready, 7, 1374, 123, 456};
    assert(!owner.receive());
    // A stream can split any message even though the sender writes it once.
    auto* bytes = reinterpret_cast<const char*>(&ready);
    for (size_t i = 0; i < sizeof(ready); ++i) {
        assert(write(child.fd(), bytes + i, 1) == 1);
        auto message = owner.receive();
        assert(bool(message) == (i + 1 == sizeof(ready)));
        if (message) assert(message->match == 7 && message->frame == 1374 && message->registers == 456);
    }
    child.send(ready); child.send(ready);
    assert(owner.receive() && owner.receive() && !owner.receive());
    owner.send({1, MatchEvent::Start, 7, 1374});
    assert(child.receive()->event == MatchEvent::Start);
    for (auto invalid : {MatchMessage{2, MatchEvent::Ready, 7, 1374},
                         MatchMessage{1, MatchEvent(99), 7, 1374},
                         MatchMessage{1, MatchEvent::Ready, 0, 1374},
                         MatchMessage{1, MatchEvent::Ready, 7, 0}}) {
        child.send(invalid);
        try { owner.receive(); assert(false); } catch (const std::runtime_error&) {}
    }
    child = MatchChannel{};
    try { owner.receive(); assert(false); } catch (const std::runtime_error&) {}
    // A dead child must not terminate its persistent owner with SIGPIPE.
    try { owner.send({1, MatchEvent::Start, 7, 1374}); assert(false); }
    catch (const std::runtime_error&) {}
    std::cout << "PASS: fragmented/batched match events, validation, disconnect and closed-peer send\n";
}
