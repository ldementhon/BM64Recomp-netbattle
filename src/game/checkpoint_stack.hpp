#pragma once
#include <cstdint>
#include <algorithm>
#include <vector>
#include <memory>
#include <cstring>

// Include after minicoro's implementation. Preserve the header/storage and live
// stack tail, including a conservative 256-byte ABI red zone below saved SP.
struct CheckpointStack {
    std::shared_ptr<void> owner; // Production snapshots keep their allocation alive.
    mco_coro* address = nullptr;
    size_t allocation_size = 0, tail_offset = 0;
    std::vector<uint8_t> prefix, tail;
    void capture(mco_coro* co, bool full_copy = false) {
        address = co; allocation_size = co->coro_size;
        auto start = reinterpret_cast<uintptr_t>(co);
        auto low = reinterpret_cast<uintptr_t>(co->stack_base);
        auto high = low + co->stack_size;
        uintptr_t sp = 0;
#if defined(__aarch64__) && defined(MCO_USE_ASM)
        sp = reinterpret_cast<uintptr_t>(static_cast<_mco_context*>(co->context)->ctx.sp);
#elif (defined(__x86_64__) || defined(_M_X64)) && defined(MCO_USE_ASM)
        sp = reinterpret_cast<uintptr_t>(static_cast<_mco_context*>(co->context)->ctx.rsp);
#endif
        // An OS continuation can be suspended on a nested Hudson stack. In that
        // case retain its whole outer stack; its saved SP does not bound it.
        if (full_copy || sp < low || sp > high || low < start || high > start + allocation_size) {
            prefix.assign(reinterpret_cast<uint8_t*>(co), reinterpret_cast<uint8_t*>(co) + allocation_size);
            tail.clear(); tail_offset = allocation_size;
        } else {
            auto tail_start = std::max(low, sp - std::min<uintptr_t>(sp, 256));
            tail_offset = tail_start - start;
            prefix.assign(reinterpret_cast<uint8_t*>(co), reinterpret_cast<uint8_t*>(low));
            tail.assign(reinterpret_cast<uint8_t*>(tail_start), reinterpret_cast<uint8_t*>(start + allocation_size));
        }
    }
    void restore() const {
        std::memcpy(address, prefix.data(), prefix.size());
        if (!tail.empty()) std::memcpy(reinterpret_cast<uint8_t*>(address) + tail_offset, tail.data(), tail.size());
    }
    size_t bytes() const { return prefix.size() + tail.size(); }
};
