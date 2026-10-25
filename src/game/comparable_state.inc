// Guest-state diagnostic, deliberately separate from full same-process replay.
// Native stacks/GPU/runtime queues are not portable state and are not certified
// by this digest. Never use a matching digest alone to claim network determinism.
static uint64_t comparable_registers() {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    auto word = [&](uint64_t value) { XXH3_64bits_update(&state, &value, sizeof(value)); };
    auto context = [&](const recomp_context& ctx) {
        try {
            auto words = comparable_context(ctx);
            XXH3_64bits_update(&state, words.data(), sizeof(words));
        } catch (const std::invalid_argument&) {
            require(false, "unsupported comparable register reference");
        }
    };
    word(1); word(frame); word(ticks); word(boot_frame); word(fast_boot);
    word(uint32_t(next_thread)); word(uint32_t(overlay));
    for (const auto& slot : g_slots) {
        word(uint64_t(slot.state)); word(slot.process_id); word(slot.entry_func); word(slot.mips_sp);
        context(slot.ctx);
    }
    word(fiber_count);
    for (size_t i = 0; i < fiber_count; ++i) {
        const auto& fiber = fibers[i];
        word(i); word(uint32_t(fiber.thread)); word(uint32_t(fiber.entry));
        word(uint32_t(fiber.arg)); word(fiber.dead);
        if (!fiber.dead) {
            word(fiber.ctx != nullptr);
            if (fiber.ctx) context(*fiber.ctx);
        }
    }
    return XXH3_64bits_digest(&state);
}
