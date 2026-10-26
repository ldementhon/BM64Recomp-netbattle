#include "../src/online/match_channel.hpp"
#include <cassert>
#include <iostream>

int main() {
    using namespace online;
    auto [owner, child] = MatchChannel::pair();
#ifdef _WIN32
    for (auto handle : {owner.fd(), child.fd()}) {
        DWORD flags = 0, mode = 0;
        assert(GetHandleInformation(handle, &flags) && !(flags & HANDLE_FLAG_INHERIT));
        assert(GetNamedPipeHandleStateW(handle, &mode, nullptr, nullptr, nullptr, nullptr, 0) && (mode & PIPE_NOWAIT));
    }
#endif
    MatchMessage ready{match_channel_version, MatchEvent::Ready, 7, 1374, 123, 456};
    assert(!owner.receive());
    // A stream can split any message even though the sender writes it once.
    auto* bytes = reinterpret_cast<const char*>(&ready);
    for (size_t i = 0; i < sizeof(ready); ++i) {
#ifdef _WIN32
        DWORD written = 0;
        assert(WriteFile(child.fd(), bytes + i, 1, &written, nullptr) && written == 1);
#else
        assert(write(child.fd(), bytes + i, 1) == 1);
#endif
        auto message = owner.receive();
        assert(bool(message) == (i + 1 == sizeof(ready)));
        if (message) assert(message->match == 7 && message->frame == 1374 && message->registers == 456);
        if (i == 17) { auto moved = std::move(owner); owner = std::move(moved); }
    }
    child.send(ready); child.send(ready);
    assert(owner.receive() && owner.receive() && !owner.receive());
    owner.send({match_channel_version, MatchEvent::Start, 7, 1374});
    assert(child.receive()->event == MatchEvent::Start);
    InputBatch input; input.match = 7; input.slot = 2; input.count = input_slots;
    for (unsigned i = 0; i < input.count; ++i) input.pads[i] = {0x8000, int8_t(i), int8_t(-int(i))};
    child.send(input_message(input, false));
    auto upload = owner.receive();
    assert(upload && upload->event == MatchEvent::InputUpload && upload->input.pads == input.pads);
    owner.send(input_message(input, true));
    auto relay = child.receive();
    assert(relay && relay->event == MatchEvent::InputRelay && relay->input.first == 2 && relay->input.count == 32);
    for (auto invalid : {input_message(input, false), input_message(input, true)}) {
        ++invalid.match; child.send(invalid);
        try { owner.receive(); assert(false); } catch (const std::runtime_error&) {}
    }
    for (auto invalid : {MatchMessage{1, MatchEvent::Ready, 7, 1374},
                         MatchMessage{match_channel_version, MatchEvent(99), 7, 1374},
                         MatchMessage{match_channel_version, MatchEvent::Ready, 0, 1374},
                         MatchMessage{match_channel_version, MatchEvent::Ready, 7, 0}}) {
        child.send(invalid);
        try { owner.receive(); assert(false); } catch (const std::runtime_error&) {}
    }
    child = MatchChannel{};
    try { owner.receive(); assert(false); } catch (const std::runtime_error&) {}
    // A dead child must not terminate its persistent owner with SIGPIPE.
    try { owner.send({match_channel_version, MatchEvent::Start, 7, 1374}); assert(false); }
    catch (const std::runtime_error&) {}
    auto [blocked, unread] = MatchChannel::pair();
    unsigned sent = 0;
    try { for (; sent < 10000; ++sent) {
        input.first = 2 + sent; input.pads[0].x = int8_t(sent % 100);
        blocked.send(input_message(input, false));
    } }
    catch (const std::runtime_error& error) { assert(std::string(error.what()) == "match event queue is full"); }
    assert(sent >= 2048 && sent < 10000); // Full IPC buffers fail instead of accumulating an unbounded queue.
    { auto moved = std::move(blocked); blocked = std::move(moved); }
    unsigned received = 0;
    for (unsigned attempts = 0; received < sent && attempts < 100000; ++attempts) {
        blocked.flush();
        if (auto message = unread.receive()) {
            assert(message->event == MatchEvent::InputUpload && message->input.first == 2 + received);
            assert(message->input.pads[0].x == int8_t(received % 100));
            ++received;
        }
    }
    assert(received == sent && blocked.flush());
    blocked = MatchChannel{};
    try { unread.receive(); assert(false); } catch (const std::runtime_error&) {}
    try { unread.send(ready); assert(false); } catch (const std::runtime_error&) {}
    std::cout << "PASS: fragmented/batched input events, bounded queue recovery, validation, disconnect and closed-peer send\n";
}
