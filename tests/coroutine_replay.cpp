#define BM64_COROUTINE_UNIT_TEST
// Component experiment: exercise the production Hudson-process bridge with
// synthetic entry functions. This is NOT a complete game-state snapshot.
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <stdexcept>
#include <vector>

static void* probe_alloc(size_t size);
static void probe_free(void* ptr, size_t size);
#define MCO_ALLOC(size) probe_alloc(size)
#define MCO_DEALLOC(ptr, size) probe_free(ptr, size)
// Include the real bridge so the experiment can inspect its private state
// without adding an unproven save-state API to the shipped game.
#include "../src/game/process_coroutines.cpp"

struct Allocation { size_t size; bool resident; size_t generation; };
static std::map<void*, Allocation> allocations;
static bool retain_allocations = false;
static size_t allocation_generation = 0;

static void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void* probe_alloc(size_t size) {
    void* ptr = std::calloc(1, size);
    require(ptr != nullptr, "allocation failed");
    allocations[ptr] = {size, true, ++allocation_generation};
    return ptr;
}

static void probe_free(void* ptr, size_t) {
    if (!retain_allocations) {
        allocations.at(ptr).resident = false;
        std::free(ptr);
    }
}

static std::array<uint8_t, 4096> memory;
static uint32_t input;

static void nested_process(uint8_t* rdram, recomp_context* ctx, int depth) {
    // Locals and pointers survive a yield at several native call depths.
    uint32_t local = 123 + depth;
    uint32_t* local_ptr = &local;
    if (depth) {
        nested_process(rdram, ctx, depth - 1);
        return;
    }
    const size_t index = (ctx->r29 / 16) % memory.size();
    for (;;) {
        *local_ptr = *local_ptr * 1664525u + input + 1013904223u;
        ctx->r16 = uint32_t(ctx->r16) + *local_ptr;
        ctx->f4.fl += float(input & 7) * 0.125f;
        rdram[index] ^= uint8_t(*local_ptr >> 24);
        ctx->r4 = YIELD_NORMAL;
        recomp_process_yield(rdram, ctx);
        *local_ptr ^= uint32_t(ctx->r2);
    }
}

static void entry(uint8_t* rdram, recomp_context* ctx) {
    nested_process(rdram, ctx, 3);
}

static void terminating_entry(uint8_t* rdram, recomp_context* ctx) {
    ctx->r4 = YIELD_NORMAL;
    recomp_process_yield(rdram, ctx);
    ctx->r16 = 0x1234;
    rdram[0] = 77;
}

extern "C" recomp_func_t* get_function(int32_t address) {
    if (address == 0x1000) return entry;
    if (address == 0x2000) return terminating_entry;
    return nullptr;
}

static void create(uint32_t id, uint32_t address = 0x1000) {
    recomp_context args{};
    args.r4 = id;
    args.r5 = address;
    args.r6 = 0x1000;
    args.r7 = 0x100 + 16 * id;
    recomp_process_coro_create(memory.data(), &args);
}

static void advance(uint32_t tick, int count) {
    input = tick * 37 + (tick % 5 == 0 ? 91 : 0);
    for (int id = 0; id < count; ++id) {
        recomp_context args{};
        args.r4 = id;
        args.r5 = tick ^ 0x55;
        recomp_process_switch_to(memory.data(), &args);
        require(args.r2 == YIELD_NORMAL, "unexpected yield");
    }
}


#include "../src/game/checkpoint_stack.hpp"

struct Image {
    mco_coro* address;
    CheckpointStack stack;
    size_t generation;
};

struct Snapshot {
    decltype(g_slots) slots;
    decltype(memory) ram;
    int32_t yield_reason, resume_value;
    uint8_t* entry_rdram;
    bool initialized;
    std::vector<Image> images;

    Snapshot() : slots(g_slots), ram(memory), yield_reason(g_yield_reason),
        resume_value(g_resume_value), entry_rdram(g_entry_rdram), initialized(g_initialized) {
        require(g_current_slot == nullptr && mco_running() == nullptr, "snapshot while executing");
        for (auto& slot : slots) {
            if (slot.coro) {
                require(mco_status(slot.coro) != MCO_RUNNING, "running coroutine");
                Image image{slot.coro, {}, allocations.at(slot.coro).generation};
                image.stack.capture(slot.coro); images.push_back(std::move(image));
            }
        }
    }

    bool restorable() const {
        for (auto& image : images) {
            auto it = allocations.find(image.address);
            if (it == allocations.end() || !it->second.resident ||
                it->second.size != image.stack.allocation_size || it->second.generation != image.generation) return false;
        }
        return true;
    }

    void restore(bool native_stacks = true) const {
        require(g_current_slot == nullptr && mco_running() == nullptr, "restore while executing");
        require(restorable(), "snapshot allocation was freed");
        if (native_stacks) {
            for (auto& image : images) image.stack.restore();
        }
        g_slots = slots;
        memory = ram;
        g_yield_reason = yield_reason;
        g_resume_value = resume_value;
        g_entry_rdram = entry_rdram;
        g_initialized = initialized;
    }

    size_t bytes() const {
        size_t result = sizeof(slots) + sizeof(ram);
        for (auto& image : images) result += image.stack.bytes();
        return result;
    }
};

static std::vector<uint64_t> outcome(int count) {
    std::vector<uint64_t> result(memory.begin(), memory.end());
    for (int id = 0; id < count; ++id) {
        auto* slot = find_slot_by_id(id);
        require(slot && slot->ctx.f_odd == &slot->ctx.f0.u32h, "register pointer invalid");
        result.push_back(slot->ctx.r16);
        result.push_back(slot->ctx.f4.u64);
        result.push_back(static_cast<uint64_t>(slot->state));
    }
    return result;
}

static void reset() {
    recomp_context args{};
    recomp_process_init(memory.data(), &args);
    memory.fill(0);
}

int main() {
    try {
        reset();
        create(0); create(1);
        Snapshot unstarted;
        advance(0, 2);
        auto first = outcome(2);
        unstarted.restore();
        advance(0, 2);
        require(first == outcome(2), "created-state replay diverged");

        Snapshot saved;
        std::vector<std::vector<uint64_t>> expected;
        for (unsigned t = 1; t <= 12; ++t) {
            advance(t, 2);
            expected.push_back(outcome(2));
        }
        saved.restore(false);
        advance(1, 2);
        require(expected[0] != outcome(2), "negative control failed to detect missing native stacks");
        for (int repeat = 0; repeat < 100; ++repeat) {
            saved.restore();
            for (unsigned t = 1; t <= 12; ++t) {
                advance(t, 2);
                require(expected[t - 1] == outcome(2), "suspended-state replay diverged");
            }
        }
        std::puts("PASS: RAM/register-only restore diverges (negative control)");
        std::puts("PASS: created and suspended states, 100 x 12 updates, all outcomes identical");

        saved.restore();
        advance(999, 2);
        require(expected[0] != outcome(2), "changed input did not change outcome");
        auto changed = outcome(2);
        saved.restore();
        advance(999, 2);
        require(changed == outcome(2), "changed-input replay diverged");
        std::puts("PASS: changed input produces a different, repeatable outcome");

        reset(); create(0, 0x2000); advance(0, 1);
        Snapshot before_return;
        recomp_context resume{};
        recomp_process_switch_to(memory.data(), &resume);
        require(resume.r2 == YIELD_TERMINATE, "process did not terminate");
        auto returned = outcome(1);
        before_return.restore();
        recomp_process_switch_to(memory.data(), &resume);
        require(resume.r2 == YIELD_TERMINATE && outcome(1) == returned, "return replay diverged");
        std::puts("PASS: rewind across natural process termination");

        reset();
        create(0); advance(0, 1);
        Snapshot freed;
        recomp_context args{}; args.r4 = 0;
        recomp_process_coro_destroy(memory.data(), &args);
        require(!freed.restorable(), "freed allocation was accepted");
        std::puts("PASS: ordinary destruction invalidates the snapshot; restore refused");

        retain_allocations = true;
        reset(); create(0); advance(0, 1);
        Snapshot retained;
        advance(1, 1);
        auto retained_expected = outcome(1);
        args.r4 = 0;
        recomp_process_coro_destroy(memory.data(), &args);
        create(0); advance(555, 1); create(1); advance(556, 2);
        retained.restore();
        advance(1, 1);
        require(retained_expected == outcome(1), "lifetime replay diverged");
        require(find_slot_by_id(1) == nullptr, "future-created process survived restore");
        std::puts("PASS: destroy/recreate and future-created process rewind with retained allocations");

        reset();
        for (unsigned id = 0; id < MAX_PROCESSES; ++id) create(id);
        advance(0, MAX_PROCESSES);
        auto start = std::chrono::steady_clock::now();
        Snapshot full;
        auto captured = std::chrono::steady_clock::now();
        advance(1, MAX_PROCESSES);
        auto full_expected = outcome(MAX_PROCESSES);
        auto restoring = std::chrono::steady_clock::now();
        full.restore();
        auto restored = std::chrono::steady_clock::now();
        advance(1, MAX_PROCESSES);
        require(full_expected == outcome(MAX_PROCESSES), "full process pool replay diverged");
        std::printf("PASS: 80-process pool; snapshot_bytes=%zu capture_ms=%.3f restore_ms=%.3f (single sample)\n",
            full.bytes(), std::chrono::duration<double, std::milli>(captured-start).count(),
            std::chrono::duration<double, std::milli>(restored-restoring).count());
        reset();
        for (auto& [ptr, allocation] : allocations) if (allocation.resident) std::free(ptr);
        std::puts("SCOPE: synthetic entries through the real process bridge; no game/OS threads/render/audio snapshot");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
