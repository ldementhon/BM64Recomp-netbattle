#pragma once
#include "recomp.h"
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>

// Version 1, same tested little-endian client build only. Every architectural
// register is retained. f_odd is the sole native pointer in recomp_context:
// cop0_status_write selects one of two members of this same context.
inline std::array<uint64_t, 69> comparable_context(const recomp_context& context) {
    static_assert(offsetof(recomp_context, f_odd) == 66 * sizeof(uint64_t));
    static_assert(sizeof(fpr) == sizeof(uint64_t));
    std::array<uint64_t, 69> words{};
    std::memcpy(words.data(), &context, offsetof(recomp_context, f_odd));
    if (!context.f_odd) words[66] = 0; // Unstarted/reset context, distinct from either mode.
    else if (context.f_odd == &context.f0.u32h) words[66] = 1;
    else if (context.f_odd == &context.f1.u32l) words[66] = 2;
    else throw std::invalid_argument("unsupported external floating-register reference");
    words[67] = context.status_reg;
    words[68] = context.mips3_float_mode;
    return words;
}
