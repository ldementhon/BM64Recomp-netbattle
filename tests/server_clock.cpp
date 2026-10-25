#include "../src/online/server_clock.hpp"
#include <cassert>
#include <iostream>

template<class F> static void fails(F f) {
    try { f(); assert(false); } catch (const std::exception&) {}
}
int main() {
    online::ServerClock clock;
    assert(!clock.estimate(0));
    fails([&] { clock.local_start(1000, 0); });
    clock.sample(100, 200, 650); // offset +500, RTT 100
    clock.sample(300, 320, 810); // offset +500, RTT 20
    assert(!clock.estimate(320));
    clock.sample(400, 550, 1025); // asymmetric slower route: estimated +550
    auto estimate = clock.estimate(550);
    assert(estimate && estimate->offset == 500 && estimate->round_trip == 20);
    assert(clock.local_start(2000, 550) == 1500);
    fails([&] { clock.local_start(1050, 550); }); // already due
    fails([&] { clock.local_start(1060, 550); }); // uncertainty/service margin
    fails([&] { clock.local_start(5000, 550); }); // wrong time domain/epoch
    assert(!clock.estimate(549));
    assert(!clock.estimate(5321)); // fewer than three fresh measurements
    fails([&] { clock.sample(600, 599, 1); });
    fails([&] { clock.sample(600, 1601, 1); });
    fails([&] { clock.sample(1, 2, 1); });
    fails([&] { clock.sample(600, 601, UINT64_MAX); });

    // Replace the fixed ring, removing the old low-RTT winner after clock drift.
    for (uint64_t i = 0; i < 8; ++i) clock.sample(1000 + i * 100, 1020 + i * 100, 1610 + i * 100);
    assert(clock.estimate(1720)->offset == 600);
    assert(clock.local_start(3000, 1720) == 2400);

    online::ServerClock behind;
    for (uint64_t i = 0; i < 3; ++i) behind.sample(1000 + i * 100, 1020 + i * 100, 510 + i * 100);
    assert(behind.estimate(1220)->offset == -500);
    assert(behind.local_start(1500, 1220) == 2000);
    fails([&] { behind.local_start(UINT64_MAX, 1220); });
    std::cout << "PASS: bounded fresh clock samples, offset signs, jitter, stale measurements and explicit late-start rejection\n";
}
