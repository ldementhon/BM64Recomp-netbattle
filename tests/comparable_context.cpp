#include "../src/game/comparable_context.hpp"
#include <cassert>
#include <iostream>

int main() {
    recomp_context first{}, second{};
    first.f_odd = &first.f0.u32h;
    second.f_odd = &second.f0.u32h;
    auto reference = comparable_context(first);
    assert(reference == comparable_context(second));
    // Each architectural word must affect the diagnostic, including NaN bit patterns.
    for (size_t word = 0; word < 66; ++word) {
        uint64_t mutation = 0x7ff8000000000042;
        std::memcpy(reinterpret_cast<char*>(&second) + word * 8, &mutation, 8);
        assert(reference != comparable_context(second));
        mutation = 0;
        std::memcpy(reinterpret_cast<char*>(&second) + word * 8, &mutation, 8);
    }
    second.status_reg = 1;
    assert(reference != comparable_context(second));
    second.status_reg = 0;
    second.mips3_float_mode = 1;
    assert(reference != comparable_context(second));
    second.mips3_float_mode = 0;
    second.f_odd = &second.f1.u32l;
    assert(reference != comparable_context(second));
    second.f_odd = nullptr;
    assert(reference != comparable_context(second));
    second.f_odd = first.f_odd;
    bool rejected = false;
    try { comparable_context(second); } catch (const std::invalid_argument&) { rejected = true; }
    assert(rejected);
    std::cout << "PASS: comparable contexts preserve all registers, normalize self references, reject unknown pointers\n";
}
