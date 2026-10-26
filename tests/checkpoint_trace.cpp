#include "../src/game/checkpoint_trace.hpp"
#include <cassert>
#include <iostream>

int main() {
    CheckpointTrace disabled(false, "test", 1, 0);
    disabled.mark("ignored"); disabled.report(0, 0, 0);
    assert(disabled.count == 0);
    CheckpointTrace trace(true, "test", 1, 0);
    volatile uint64_t work = 1;
    auto end = CheckpointTrace::Clock::now() + std::chrono::milliseconds(100);
    while (CheckpointTrace::Clock::now() < end) work = work * 1664525 + 1013904223;
    trace.mark("work");
    assert(trace.count == 1 && trace.stages[0].wall > 0 && trace.stages[0].cpu > 0);
    assert(trace.cpu_previous > trace.cpu_start);
    trace.report(0, 0, 0);
    std::cout << "PASS: disabled tracing, native thread CPU timing and stage report\n";
}
