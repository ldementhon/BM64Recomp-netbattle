#include "../lib/rt64/src/common/rt64_rollback_probe.h"
#include <cassert>
#include <cstdio>
int main() {
    namespace P=RT64::RollbackProbe;
    P::bounded_present=true;
    auto period=P::refresh_period_ns.load();
    P::observeDisplay(period,2*period);
    assert(!P::recovering_display);
    P::observeDisplay(2*period,4*period);
    assert(P::recovering_display);
    P::observeDisplay(4*period,5*period);
    assert(P::recovering_display);
    P::observeDisplay(5*period,6*period);
    assert(!P::recovering_display);
    P::observeDisplay(period,3*period); // A delayed callback must not reopen recovery.
    assert(!P::recovering_display);
    P::submittedDisplay(); P::submittedDisplay();
    assert(P::maximum_in_flight==2);
    P::completedDisplay(); P::completedDisplay();
    assert(P::in_flight==0);
    puts("PASS: late display opens recovery; two fresh displays close it; old callbacks ignored; bound accounts for both completions");
}
