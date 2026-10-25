#include "../src/online/battle_state.hpp"
#include <cassert>
#include <iostream>

int main() {
    online::BattleState winner{17, 21, 0, 3, {0, 1, 2, 3}};
    assert(winner.complete());
    auto state = winner;
    state.scene = 20; assert(!state.complete()); // Even the third star is still a round result.
    state = winner; state.overlay = 86; assert(!state.complete()); // Scene load pending.
    state = winner; state.kind = 1; assert(!state.complete()); // Team battle is unsupported.
    state = winner; state.target = 1; assert(!state.complete());
    state = winner; state.wins[3] = 2; assert(!state.complete());
    state = winner; state.wins[0] = 3; assert(!state.complete());
    state = winner; state.wins[0] = -1; assert(!state.complete());
    state = winner; state.wins[0] = 4; assert(!state.complete());
    std::cout << "PASS: only the acknowledged winner of the supported preset completes a match\n";
}
