#pragma once
#include <array>
#include <cstdint>

namespace online {
struct BattleState {
    int32_t overlay, scene, kind, target;
    std::array<int32_t, 4> wins;

    bool complete() const {
        // Only the tested single-battle, first-to-three preset is supported.
        // Scene 20 is the round result; scene 21 is the acknowledged match winner.
        if (overlay != 17 || scene != 21 || kind != 0 || target != 3) return false;
        unsigned winners = 0;
        for (auto score : wins) {
            if (score < 0 || score > target) return false;
            winners += score == target;
        }
        return winners == 1;
    }
};
}
