// Included beside minicoro so the experimental scheduler can suspend a nested
// Hudson stack without unwinding it. Not a portable or persistent save format.
#include "ultramodern/checkpoint.hpp"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/addresses.hpp"
#define XXH_INLINE_ALL
#include "../../lib/N64ModernRuntime/thirdparty/xxHash/xxhash.h"
#include <vector>
#include <fstream>
#include <sstream>
#include <cfenv>
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <map>
#include <tuple>
#include <dlfcn.h>
#include <pthread.h>
#include <sys/resource.h>
#ifdef __APPLE__
#include <mach/mach_time.h>
#endif
#include "../../lib/rt64/src/common/rt64_rollback_probe.h"
#include "checkpoint_memory.hpp"
#include "checkpoint_stack.hpp"
#include "checkpoint_trace.hpp"
#include "comparable_context.hpp"
#include "../online/match_settings.hpp"
#include "../online/battle_state.hpp"
#include "../online/match_channel.hpp"
#include <charconv>
#include "zelda_cheats.h"
#include "zelda_sound.h"

extern uint8_t dmem[0x1000];
extern thread_local PTR(OSThread) thread_self;
void run_thread_function(uint8_t*, uint64_t, uint64_t, uint64_t);
void run_next_thread(uint8_t*);

namespace checkpoint {
bool enabled = false;
std::atomic<bool> profiling = std::getenv("BM64_PROFILE") || std::getenv("BM64_LOOP_FRAMES");
std::atomic<uint64_t> graphics_wait_ns = 0, presentation_frame = 0, presentation_deadline = 0;
std::atomic<bool> fast_boot = false;
std::atomic<uint64_t> boot_frame = 0;
std::atomic<bool> suppress_render = false;
std::atomic<bool> catchup_active = false;
std::atomic<uint64_t> presentation_wait_ns = 0;

size_t state_slot = 0;
uint64_t ticks = 0;
static uint8_t* ram;
static uint64_t frame = 0;
static int32_t players, player_stride, overlay;
static unsigned replay_pass = 0;
static bool player_select_ready = false;
struct Fiber { mco_coro* co; int32_t thread, entry, arg; bool dead; recomp_context* ctx; };
static std::array<Fiber, 64> fibers{};
static size_t fiber_count = 0;
static Fiber* current;
void context(void* ctx) { current->ctx = static_cast<recomp_context*>(ctx); }
static int32_t next_thread;
static bool boundary;
static fenv_t host_fenv;
static std::array<fenv_t, 64> fiber_fenv;
static size_t retained_bytes;
static void require(bool ok, const char* message) {
    if (!ok) { fprintf(stderr, "CHECKPOINT FAIL: %s\n", message); fflush(stderr); std::_Exit(2); }
}
#include "checkpoint_input.inc"

static std::unordered_map<void*, std::shared_ptr<void>> live_allocations;
static void* allocate_native(size_t size, void*) {
    require(retained_bytes + size < 512 * 1024 * 1024, "native history allocation limit reached");
    void* memory = std::malloc(size);
    require(memory != nullptr, "native allocation failed");
    retained_bytes += size;
    live_allocations.emplace(memory, std::shared_ptr<void>(memory, [size](void* p) {
        std::free(p); retained_bytes -= size;
    }));
    return memory;
}
static void release_native(void* p, size_t, void*) { live_allocations.erase(p); }
void retain_process(mco_desc* desc) {
    desc->alloc_cb = allocate_native;
    desc->dealloc_cb = release_native;
}
static Fiber* find(int32_t t) {
    for (size_t i = fiber_count; i-- > 0;) if (fibers[i].thread == t && !fibers[i].dead) return &fibers[i];
    require(false, "unknown thread"); return nullptr;
}
// The OS continuation can currently be executing inside a nested Hudson stack.
// Preserve minicoro's TLS chain while switching the outer continuation directly.
static void suspend() {
    auto* running = mco_running();
    auto* outer = current->co;
    auto outer_state = outer->state;
    auto* process = g_current_slot;
    fegetenv(&fiber_fenv[current - fibers.data()]);
    outer->state = MCO_SUSPENDED;
    _mco_jumpout(outer);
    outer->state = outer_state;
    mco_current_co = running;
    g_current_slot = process;
}
static void fiber_entry(mco_coro*) {
    uint8_t* rdram = ram;
    auto* f = current;
    try {
        run_thread_function(ram, f->entry, TO_PTR(OSThread, f->thread)->sp, f->arg);
    } catch (ultramodern::thread_terminated&) {}
    f->dead = true;
    run_next_thread(ram);
    require(false, "terminated thread resumed");
}
void create_thread(uint8_t* rdram, int32_t thread, int32_t entry, int32_t arg) {
    ram = rdram;
    // Snapshot owners keep retired generations alive until their slots expire.
    for (size_t i=0; i<fiber_count; ++i) if (fibers[i].thread == thread && !fibers[i].dead) {
        fibers[i].dead = true;
        if (&fibers[i] != current) require(mco_destroy(fibers[i].co) == MCO_SUCCESS, "replace native thread");
    }
    require(fiber_count < fibers.size(), "thread generation limit reached");
    auto& f = fibers[fiber_count++];
    f = {nullptr, thread, entry, arg, false};
    auto desc = mco_desc_init(fiber_entry, 2 * 1024 * 1024);
    retain_process(&desc);
    require(mco_create(&f.co, &desc) == MCO_SUCCESS, "thread allocation");
    fegetenv(&fiber_fenv[fiber_count-1]);
}
void destroy_thread(int32_t thread) {
    for (size_t i=0; i<fiber_count; ++i) if (fibers[i].thread == thread && !fibers[i].dead) {
        fibers[i].dead = true;
        require(&fibers[i] != current && mco_destroy(fibers[i].co) == MCO_SUCCESS, "destroy native thread");
    }
}

using Allocation = CheckpointStack;
struct Snapshot {
std::vector<Allocation> saved_allocations;
decltype(g_slots) saved_slots;
decltype(fibers) saved_fibers;
decltype(fiber_fenv) saved_fenv;
size_t saved_fiber_count;
int32_t saved_next, saved_yield, saved_resume;
int32_t saved_players, saved_player_stride, saved_overlay;
uint64_t saved_frame, saved_ticks;
uint64_t saved_boot_frame;
bool saved_fast_boot, saved_player_select_ready;
bool saved_initialized;
uint8_t* saved_entry_rdram;
CheckpointMemory saved_ram;
};
static std::array<Snapshot, snapshot_slots> snapshots;
static void runtime_state(bool restore) {
    static std::array<uint8_t, 0x1000> dmem_images[snapshot_slots];
    auto& saved_dmem = dmem_images[state_slot];
    if (restore) std::memcpy(dmem, saved_dmem.data(), saved_dmem.size());
    else std::memcpy(saved_dmem.data(), dmem, saved_dmem.size());
    events_state(restore); timers_state(restore); queues_state(restore); runqueue_state(restore);
    overlays_state(restore); extensions_state(restore); audio_state(restore); input_state(restore); saves_state(restore);
}
static double save(size_t index = 0, bool quiet = false) {
    state_slot = index;
    auto& s = snapshots[index];
    require(mco_running() == nullptr && g_current_slot == nullptr, "checkpoint outside scheduler");
    auto start = std::chrono::steady_clock::now();
    CheckpointTrace trace(profiling,"capture",frame,index);
    static bool injected=false;
    if(!injected && frame==2100 && std::getenv("BM64_CHECKPOINT_STALL")) {
        injected=true; std::this_thread::sleep_for(std::chrono::milliseconds(25)); trace.mark("injected");
    }
    s.saved_ram.capture(ram, recomp::mem_size, std::getenv("BM64_FULL_COPY") != nullptr);
    trace.mark("RAM");
    size_t allocation_index = 0;
    auto copy = [&](mco_coro* co) { if (co) { if (allocation_index == s.saved_allocations.size()) s.saved_allocations.emplace_back(); auto& image = s.saved_allocations[allocation_index++]; image.capture(co, std::getenv("BM64_FULL_COPY") != nullptr); image.owner = live_allocations.at(co); } };
    for (auto& s : g_slots) copy(s.coro);
    for (size_t i=0; i<fiber_count; ++i) if (!fibers[i].dead) copy(fibers[i].co);
    s.saved_allocations.resize(allocation_index);
    trace.mark("stacks");
    s.saved_slots = g_slots; s.saved_fibers = fibers; s.saved_fiber_count = fiber_count; s.saved_fenv = fiber_fenv;
    s.saved_next = next_thread; s.saved_frame = frame; s.saved_ticks = ticks;
    s.saved_players = players; s.saved_player_stride = player_stride; s.saved_overlay = overlay;
    s.saved_boot_frame = boot_frame.load(); s.saved_fast_boot = fast_boot.load();
    s.saved_player_select_ready = player_select_ready;
    s.saved_yield = g_yield_reason; s.saved_resume = g_resume_value;
    s.saved_initialized = g_initialized; s.saved_entry_rdram = g_entry_rdram;
    trace.mark("metadata");
    runtime_state(false); trace.mark("runtime");
    size_t bytes = s.saved_ram.copied_bytes(); for (auto& a : s.saved_allocations) bytes += a.bytes();
    auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    trace.report(s.saved_ram.clear_ms,s.saved_ram.remap_ms,bytes);
    if (!quiet) fprintf(stderr,"CHECKPOINT: saved frame=%llu vi=%llu allocations=%zu copied_bytes=%zu capture_ms=%.3f\n", frame,ticks,s.saved_allocations.size(),bytes,elapsed);
    return elapsed;
}
static double restore(size_t index = 0, bool quiet = false) {
    state_slot = index;
    auto& s = snapshots[index];
    auto start = std::chrono::steady_clock::now();
    CheckpointTrace trace(profiling,"restore",s.saved_frame,index);
    live_allocations.clear();
    for (auto& image : s.saved_allocations) live_allocations.emplace(image.address, image.owner);
    trace.mark("ownership");
    s.saved_ram.restore(ram); trace.mark("RAM");
    if (!std::getenv("BM64_NEGATIVE_STACKS"))
        for (auto& a : s.saved_allocations) a.restore();
    trace.mark("stacks");
    g_entry_rdram = s.saved_entry_rdram; g_initialized = s.saved_initialized;
    g_slots = s.saved_slots; fibers = s.saved_fibers; fiber_count = s.saved_fiber_count; fiber_fenv = s.saved_fenv;
    next_thread = s.saved_next; frame = s.saved_frame; ticks = s.saved_ticks;
    players = s.saved_players; player_stride = s.saved_player_stride; overlay = s.saved_overlay;
    boot_frame = s.saved_boot_frame; fast_boot = s.saved_fast_boot;
    player_select_ready = s.saved_player_select_ready;
    g_yield_reason = s.saved_yield; g_resume_value = s.saved_resume;
    trace.mark("metadata");
    runtime_state(true); trace.mark("runtime");
    size_t bytes=0; for(auto& a:s.saved_allocations) bytes+=a.bytes();
    trace.report(s.saved_ram.clear_ms,s.saved_ram.remap_ms,bytes);
    auto elapsed = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    if (!quiet) fprintf(stderr,"CHECKPOINT: restored frame=%llu restore_ms=%.3f\n",frame,elapsed);
    return elapsed;
}
static unsigned setting(const char* name, unsigned fallback) {
    const char* v = std::getenv(name); return v ? std::stoul(v) : fallback;
}
static void distribution(const char* name, std::vector<double> samples) {
    require(!samples.empty(), "empty benchmark");
    std::sort(samples.begin(), samples.end());
    fprintf(stderr,"PERF: %s samples=%zu p50_ms=%.3f p95_ms=%.3f p99_ms=%.3f max_ms=%.3f\n",name,samples.size(),samples[samples.size()/2],samples[(samples.size()-1)*95/100],samples[(samples.size()-1)*99/100],samples.back());
}
static uint64_t footprint() {
#ifdef __APPLE__
    task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS, "task memory accounting");
    return info.phys_footprint;
#else
    return 0;
#endif
}
static uint64_t cow_faults() {
#ifdef __APPLE__
    task_events_info_data_t info{}; mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    require(task_info(mach_task_self(), TASK_EVENTS_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS, "task fault accounting");
    return info.cow_faults;
#else
    return 0;
#endif
}
static size_t eager_bytes(const Snapshot& s) {
    size_t size = sizeof(s) + s.saved_ram.copied_bytes() + 4096;
    for (auto& a:s.saved_allocations) size += a.bytes();
    return size;
}
struct Digest { uint64_t ram, registers; };
static Digest digest() {
    XXH3_state_t state;
    XXH3_64bits_reset(&state);
    for (auto& slot : g_slots) {
        XXH3_64bits_update(&state, &slot.state, sizeof(slot.state));
        XXH3_64bits_update(&state, &slot.process_id, sizeof(slot.process_id));
        XXH3_64bits_update(&state, &slot.ctx, sizeof(slot.ctx));
    }
    for (size_t i=0; i<fiber_count; ++i) if (!fibers[i].dead && fibers[i].ctx) XXH3_64bits_update(&state, fibers[i].ctx, sizeof(recomp_context));
    XXH3_64bits_update(&state, &player_select_ready, sizeof(player_select_ready));
    auto context_hash = XXH3_64bits_digest(&state);
    return {XXH3_64bits(ram, recomp::mem_size), context_hash};
}
#include "comparable_state.inc"
static bool benchmark(unsigned at) {
    const unsigned count = setting("BM64_BENCHMARK_FRAMES", 0);
    if (!count) return false;
    require(at > 0 && count >= 16, "benchmark needs a checkpoint frame and at least 16 updates");
    static std::vector<double> captures, game_times, total_times;
    static uint64_t initial_memory, peak_memory, initial_faults;
    static auto dispatched = std::chrono::steady_clock::now();
    static double previous_capture = 0;
    if (frame < at) return true;
    if (frame == at) {
        initial_memory = peak_memory = footprint(); initial_faults = cow_faults();
        captures.reserve(count); game_times.reserve(count); total_times.reserve(count);
    } else {
        double game_ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-dispatched).count();
        game_times.push_back(game_ms); total_times.push_back(game_ms + previous_capture);
    }
    if (frame == at+count) {
        peak_memory = std::max(peak_memory, footprint());
        distribution("capture",captures); distribution("game_including_cow_faults",game_times); distribution("game_plus_capture",total_times);
        size_t bytes=0; for (size_t i=1;i<snapshot_slots;++i) bytes += eager_bytes(snapshots[i]);
        fprintf(stderr,"PERF: ring_slots=16 eager_ring_bytes=%zu task_initial_bytes=%llu task_peak_bytes=%llu cow_faults=%llu retained_native_allocation_bytes=%zu\n",bytes,initial_memory,peak_memory,cow_faults()-initial_faults,retained_bytes);
        if (!std::getenv("BM64_BENCHMARK_NO_SNAPSHOT")) {
            std::vector<double> restores;
            // Measure fixed-address restores separately from hashing and drawing.
            // Correct replay from these same ring ages is checked by BM64_RING_REPLAY.
            for (unsigned repeat=0;repeat<3;++repeat)
                for (size_t i=1;i<snapshot_slots;++i) restores.push_back(restore(i, true));
            distribution("restore",restores);
        }
        fprintf(stderr,"PASS: checkpoint performance run completed (%u updates, verification hashes disabled)\n",count); fflush(stderr); std::_Exit(0);
    }
    previous_capture = std::getenv("BM64_BENCHMARK_NO_SNAPSHOT") ? 0 : save(1 + frame % 16, true);
    captures.push_back(previous_capture);
    peak_memory = std::max(peak_memory, footprint());
    dispatched = std::chrono::steady_clock::now();
    return true;
}
#include "rollback_profile.inc"
#include "framebuffer_trace.inc"
#include "running_loop.inc"
#include "catchup.inc"

static online::BattleState battle_state() {
    auto* rdram = ram;
    // func_8023A834 returns this settings block; func_80243800 returns the scene.
    constexpr int32_t settings = int32_t(0x802ac660);
    return {overlay, MEM_W(0, int32_t(0x802ac5d8)), MEM_W(0, settings), MEM_W(0x24, settings),
            {MEM_W(0xa4, settings), MEM_W(0xa8, settings), MEM_W(0xac, settings), MEM_W(0xb0, settings)}};
}

#include "owned_match.inc"

static void on_boundary() {
    // Boundary N precedes input N; its resulting state is checked at boundary N+1.
    ++frame;
    owned_match_boundary();
    if (!replay_pass && std::getenv("BM64_TRACE_MENU")) {
        static bool last_ready = false;
        if (last_ready != player_select_ready) {
            fprintf(stderr, "PLAYER_SELECT: frame=%llu ready=%d\n", frame, player_select_ready);
            last_ready = player_select_ready;
        }
    }
    if (!replay_pass && std::getenv("BM64_TRACE_BATTLE_STATE")) {
        auto state = battle_state();
        static bool last_complete = false;
        if (state.complete() != last_complete) {
            fprintf(stderr, "MATCH_COMPLETE: frame=%llu complete=%d\n", frame, state.complete());
            last_complete = state.complete();
        }
        if (frame % 60 == 0) {
            fprintf(stderr, "BATTLE_STATE: frame=%llu overlay=%d scene=%d kind=%d target=%d scores=%d,%d,%d,%d complete=%d\n",
                    frame, state.overlay, state.scene, state.kind, state.target,
                    state.wins[0], state.wins[1], state.wins[2], state.wins[3], state.complete());
        }
    }
    if (!replay_pass && frame == 1940 && std::getenv("BM64_TRY_CHEATS")) {
        require(online::match_settings_locked, "cheat-change probe requires --online-test");
        const auto setters = std::array{zelda64::set_moonjump, zelda64::set_always_max_bomb,
            zelda64::set_always_max_fire, zelda64::set_always_have_remote_bombs,
            zelda64::set_always_have_red_bombs, zelda64::set_infinite_credits, zelda64::set_infinite_lives};
        const auto getters = std::array{zelda64::get_moonjump, zelda64::get_always_max_bomb,
            zelda64::get_always_max_fire, zelda64::get_always_have_remote_bombs,
            zelda64::get_always_have_red_bombs, zelda64::get_infinite_credits, zelda64::get_infinite_lives};
        for (size_t i = 0; i < setters.size(); ++i) {
            setters[i](zelda64::GenericOnOffOption::On);
            require(getters[i]() == zelda64::GenericOnOffOption::Off, "match cheat override changed");
        }
        fprintf(stderr, "PASS: all seven attempted mid-match cheat changes refused\n");
    }
    if (!replay_pass && frame == 1940 && std::getenv("BM64_TRY_SOUND")) {
        zelda64::set_bgm_volume(17);
        zelda64::set_sfx_volume(23);
        zelda64::set_voice_volume(29);
        zelda64::set_main_volume(37);
        require(zelda64::get_bgm_volume() == (online::match_settings_locked ? 100 : 17) &&
                zelda64::get_sfx_volume() == (online::match_settings_locked ? 100 : 23) &&
                zelda64::get_voice_volume() == (online::match_settings_locked ? 100 : 29), "sound balance override failed");
        require(zelda64::get_main_volume() == 37, "host main volume should remain adjustable");
        fprintf(stderr, "PASS: sound balance %s; host main volume remains adjustable\n",
                online::match_settings_locked ? "locked" : "changed");
    }
    if (!replay_pass && frame % 60 == 0) { fprintf(stderr,"CHECKPOINT: frame=%llu vi=%llu threads=%zu\n",frame,ticks,fiber_count); report_process_probe(); }
    if (frame == setting("BM64_HOLD_FRAME", 0)) {
        finish_presentation();
        fprintf(stderr,"CHECKPOINT: holding frame %llu for inspection\n", frame);
        std::this_thread::sleep_for(std::chrono::seconds(3));
    }
    const unsigned at = setting("BM64_CHECKPOINT_FRAME", 0);
    const unsigned length = setting("BM64_REPLAY_LENGTH", 12);
    if (std::getenv("BM64_SUPPRESS_REPLAY")) suppress_render = replay_pass && frame < at + length - 1;
    static std::vector<uint64_t> hashes, context_hashes;
    static std::vector<std::vector<uint64_t>> page_hashes;
    auto& pass = replay_pass;
    if ((!replay_pass && frame % 60 == 0) || frame == at) {
        auto* rdram = ram;
        fprintf(stderr,"BATTLE: frame=%llu overlay=%d stride=%d controls=",frame,overlay,player_stride);
        int active = 0;
        for (int i=0;i<4;++i) {
            int control = MEM_W(0x24, players + i*player_stride);
            int object = MEM_W(0x28, players + i*player_stride);
            fprintf(stderr,"%d:%08x,", control,object);
            active += (control==1 || control==2) && object!=0;
        }
        fprintf(stderr," active=%d\n",active);
        if (frame == at && std::getenv("BM64_REQUIRE_BATTLE")) require(active >= 2, "checkpoint did not reach a multiplayer battle");
    }
    if (trace_frame(at)) return;
    if (running_loop(at)) return;
    if (catchup(at)) return;
    if (benchmark(at)) return;
    const bool rolling = std::getenv("BM64_RING_REPLAY") != nullptr;
    static std::vector<double> rolling_captures;
    if (rolling && !pass && frame >= at && frame <= at + length) rolling_captures.push_back(save(1 + frame % 16, true));
    if (at && frame == at) save();
    if (at && frame > at && frame <= at + length) {
        if (catchup_active && frame == at + length) finish_presentation();
        auto state_hash = digest();
        if (!pass && std::getenv("BM64_COMPARE_STATE")) {
            fprintf(stderr, "COMPARABLE_V1: frame=%llu ram=%016llx registers=%016llx\n",
                    frame, state_hash.ram, comparable_registers());
        }
        uint64_t hash = state_hash.ram;
        size_t index = frame-at-1;
        auto context_hash = state_hash.registers;
        if (!pass) {
            hashes.push_back(hash); context_hashes.push_back(context_hash);
            std::vector<uint64_t> pages;
            for (size_t p=0;p<recomp::mem_size;p+=4096) pages.push_back(XXH3_64bits(ram+p,4096));
            page_hashes.push_back(std::move(pages));
        }
        else if (hashes[index] != hash || context_hashes[index] != context_hash) {
            fprintf(stderr,"CHECKPOINT FAIL: replay=%u frame=%llu expected=%016llx actual=%016llx\n",pass,frame,hashes[index],hash);
            fprintf(stderr,"CHECKPOINT: context expected=%016llx actual=%016llx\n",context_hashes[index],context_hash);
            int shown = 0;
            for (size_t p=0;p<recomp::mem_size;p+=4096) if (page_hashes[index][p/4096] != XXH3_64bits(ram+p,4096)) {
                if (shown++ < 16) fprintf(stderr,"DIFF: RAM page 0x%08zx\n",p);
            }
            fprintf(stderr,"DIFF: %d pages differ\n",shown);
            if (std::getenv("BM64_NEGATIVE_STACKS") || std::getenv("BM64_NEGATIVE_INPUT")) {
                fprintf(stderr,"PASS: negative control detected divergence\n"); fflush(stderr); std::_Exit(0);
            }
            std::_Exit(2);
        }
        fprintf(stderr,"CHECKPOINT: replay=%u frame=%llu ram_hash=%016llx\n",pass,frame,hash);
        if (frame == at+length) {
            if (++pass == (rolling ? 5 : 4)) { require(!std::getenv("BM64_NEGATIVE_STACKS") && !std::getenv("BM64_NEGATIVE_INPUT"), "negative control failed to diverge"); fprintf(stderr,"PASS: coordinated replay matched full 512 MiB RAM and register hashes for %u baseline frames on %u restores\n",length,rolling ? 4 : 3); fflush(stderr); std::_Exit(0); }
            if (rolling) {
                const unsigned windows[] = {12,8,4,1};
                auto wanted = at + length - windows[pass-1];
                auto index = 1 + wanted % 16;
                require(snapshots[index].saved_frame == wanted, "rolling slot mismatch");
                if (pass==1) distribution("rolling_capture_with_verification",rolling_captures);
                restore(index);
            } else restore();
            if (std::getenv("BM64_SUPPRESS_REPLAY")) {
                catchup_active = true;
                suppress_render = frame < at + length - 1;
            }
        }
    }
    if (frame >= setting("BM64_STOP_FRAME", 1800)) { fprintf(stderr,"CHECKPOINT: navigation run complete (no replay claim)\n"); fflush(stderr); std::_Exit(0); }
}
void switch_thread(int32_t thread) {
    next_thread = thread;
    if (current) { suspend(); return; }
    fegetenv(&host_fenv);
    while (true) {
        current = find(next_thread);
        thread_self = current->thread;
        fesetenv(&fiber_fenv[current-fibers.data()]);
        require(mco_resume(current->co) == MCO_SUCCESS, "resume OS continuation");
        if (current->dead) require(mco_destroy(current->co) == MCO_SUCCESS, "finish native thread");
        current = nullptr; g_current_slot = nullptr;
        fesetenv(&host_fenv);
        if (boundary) { boundary = false; on_boundary(); }
    }
}
}
extern "C" void bm64_player_select_ready(int ready) {
    checkpoint::player_select_ready = ready != 0;
}

extern "C" void bm64_online_player_layout(uint8_t* rdram) {
    if (!online::match_settings_locked) return;
    // The menu reads these settings before creating the MAN/COM and costume rows.
    constexpr int32_t settings = int32_t(0x802ac660);
    for (int port = 0; port < 4; ++port) {
        MEM_W(4 + 4 * port, settings) = unsigned(port) < online::human_players ? 1 : 2;
        MEM_W(0x40 + 4 * port, settings) = 0;
    }
}

extern "C" void recomp_checkpoint_frame(uint8_t*, recomp_context* ctx) {
    static const unsigned handoff = checkpoint::setting("BM64_BOOT_HANDOFF_FRAME", 1600);
    if (checkpoint::fast_boot) {
        ++checkpoint::boot_frame;
        if (online::match_settings_locked || std::getenv("BM64_BOOT_TO_PLAYER_SELECT")) {
            if (checkpoint::player_select_ready && int32_t(ctx->r6) == 13) {
                checkpoint::fast_boot = false;
                fprintf(stderr, "SINGLE_BATTLE: Player Select input loop ready at update %llu\n", checkpoint::boot_frame.load());
            }
        } else if (checkpoint::boot_frame >= handoff) {
            checkpoint::fast_boot = false;
            fprintf(stderr, "SINGLE_BATTLE: scripted boot handoff at update %u (scene not verified)\n", handoff);
        }
    }
    checkpoint::players = int32_t(ctx->r4);
    checkpoint::player_stride = int32_t(ctx->r5);
    checkpoint::overlay = int32_t(ctx->r6);
    if (!checkpoint::enabled) return;
    checkpoint::require(g_current_slot == nullptr, "frame hook inside Hudson process");
    checkpoint::boundary = true;
    checkpoint::next_thread = thread_self;
    checkpoint::suspend();
}

#ifdef BM64_TRACE_MEMORY
extern "C" void __sanitizer_cov_trace_pc_guard(uint32_t*) {}
extern "C" void __sanitizer_cov_trace_pc_guard_init(uint32_t* b,uint32_t* e) { while(b!=e) *b++=1; }
#define TRACE_CALLBACK(N) \
extern "C" void __sanitizer_cov_load##N(void* p) { checkpoint::trace_access(p,N,false,__builtin_return_address(0)); } \
extern "C" void __sanitizer_cov_store##N(void* p) { checkpoint::trace_access(p,N,true,__builtin_return_address(0)); }
TRACE_CALLBACK(1) TRACE_CALLBACK(2) TRACE_CALLBACK(4) TRACE_CALLBACK(8) TRACE_CALLBACK(16)
#undef TRACE_CALLBACK
#endif
