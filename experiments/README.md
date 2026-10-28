# State replay feasibility experiment

Date: 2026-10-24. Platform: Apple Silicon macOS. Branch: `state-replay-feasibility`.

## Decision

**Full-game CPU rollback is technically viable in the tested configuration.**
The recomp now saves, restores, and replays an actual Rock Garden battle with four
scripted controllers. This is an opt-in, same-process feasibility experiment on
Apple Silicon. The checkpoint performance follow-up now supports a 16-slot rolling
history with sub-millisecond capture. See [measurements and reproduction](checkpoint-performance.md).
The [catch-up follow-up](catchup-performance.md) now preserves full state while
presenting only the final replayed update. The [running-loop follow-up](running-loop-performance.md)
adds actual Metal display timestamps, delayed-input corrections with a two-update
buffer, framebuffer-consumer tracing, and safe writeback optimizations. Normal
updates typically fit the CPU budget. The [pacing and recovery follow-up](pacing-and-recovery.md)
established display-aligned releases and a bounded queue. The latest
[optimization decisions and delivery investigation](optimization-decisions-and-delivery.md)
removes shared native uploads and the dither variant after an independent four-way
comparison. Snapshot-address reuse and a new queue-age heuristic were also rejected.
The [native command preparation follow-up](native-command-preparation.md) adds
CPU/wall-time attribution. Palette-hash batching was removed: 636 corrections across
six A/B pairs did not show repeatable tail improvement. An isolated original-gameplay
baseline preserves all 1,077 saved RAM hashes; the shared controller work remains intact.
Final four-update p99 is 17.54 ms, so reliable 60 Hz rollback remains an unmet target.
Executable hashes are verified on every comparison and replay run.


The original `../BM64Recomp-1.0.0` source archive is untouched. This checkout starts
at upstream tag `v1.0.0`, commit `79fda1f089f4c3def1e617f5368af3a3143069ca`.
Changes are local and uncommitted. Normal play retains the original threaded runtime.

## Coordinated battle checkpoint

`--checkpoint-test` enables the experiment. Its runner supplies isolated settings,
the existing local ROM, fresh saves, a recorded input fixture, and a 180-second
deadline. It exits deliberately after the test rather than exercising shutdown.

The checkpoint hook runs at the **start of the complete main-loop iteration**,
after the previous iteration's controller, process, animation, graphics, and audio
submission work. It suspends the current continuation back to an outer scheduler
before copying anything. All other N64 game continuations are already suspended.
Frame N denotes this boundary; inputs for N run before boundary N+1 is checked.

The test mode replaces N64 `std::thread` continuations with retained minicoro
allocations on one host thread, preserving the runtime's priority/message queues.
The coroutine switch also handles an OS scheduling call nested inside a Hudson
process. Each continuation has its own saved floating-point environment. This is
why copying RAM alone, or enabling the old disabled quicksave file, is insufficient.

| State | What the experiment does |
| --- | --- |
| Game, patches, and heap RAM | Preserves all 512 MiB through macOS copy-on-write mappings; verification hashes the whole region, with no ignored ranges. |
| Hudson processes | Copies slot/register metadata and the live ARM64 native stack ranges back to their original addresses. |
| N64 game threads | Copies live continuations, register contexts, scheduler metadata, and floating-point environments; unsupported stack layouts use full copies. |
| Scheduling and device messages | Restores the runnable-queue head and ordered pending messages. All game queues and links in RAM are covered too. |
| Time, VI, timers, RNG | Advances a fixed virtual clock from the idle thread, seeds through that clock, and restores VI/event registrations, active timers, and clock state. |
| RSP | Runs audio tasks synchronously; restores the 4 KiB DMEM buffer. Constants are immutable. |
| Graphics completions | Fences the graphics worker after display-list processing and screen updates; restores pending display-list events. RT64 GPU/history state is **not** snapshotted. |
| Overlays | Shares immutable overlay images until mappings change; restores loaded sections, functions, and section addresses. |
| Audio, rumble, saves | Restores sample rate and in-memory save data; reports fixed audio occupancy and suppresses audio/rumble output and disk save writes. |
| Inputs | Supplies four connected test controllers and frame-indexed, validated button/stick values. Live input polling is disabled in this mode. |

Native allocations are owned by the live game and snapshots that reference them.
Retired allocations are released when the last owner expires. The diagnostic has
16 rolling slots plus an optional baseline anchor, a 512 MiB retained-native-allocation
limit, and 64 recorded OS-thread generations. The generation cap still limits long
sessions. Verification hashes and copy-on-write pages require additional memory.
Snapshots only work in the same process at the same addresses; they are not save files.

### Evidence

The original positive cases compare full-RAM XXH3 hashes **and** Hudson/N64 CPU
register hashes after every replayed update, three restores per case. The additional
rolling-history case captures every update and restores 12, 8, 4, and 1 updates back. These are hash
comparisons, not a byte-by-byte equality proof. Divergence reports the first
failing frame and differing 4 KiB RAM pages. No game-state exclusions are used.

| Test | Result / evidence |
| --- | --- |
| Four-controller battle, 1/4/8/12 updates | Passed all three restores: [1](results/four-player-1.txt), [4](results/four-player-4.txt), [8](results/four-player-8.txt), [12](results/four-player-12.txt). |
| Four-controller battle, 400 updates | Passed all three restores, including movement, bomb actions, explosions, and two player deaths: [log](results/four-player-lifecycle.txt). |
| Stage selection → battle, 120 updates | Passed all three restores across overlay/process changes: [log](results/four-player-entry-transition.txt). |
| Omit native allocation restoration across battle entry | Divergence detected on the first replayed update: [negative control](results/four-player-transition-negative-stacks.txt). |
| Change player-one replay input | Divergence detected on the first replayed update: [negative control](results/four-player-negative-input.txt). |
| Independent launches on this Mac | The 1/4/8/12-update baseline RAM hashes match the overlapping frames of the independently launched 400-update baseline. This is not cross-machine evidence. |
| Normal runtime regression | The unchanged threaded path still passes the [300-read startup probe](results/startup-probe.txt); both coroutine component builds and bundle signature verification pass. |

The final positive cases total **1,635 replayed updates** checked against their
baselines. The long battle uses four scripted humans, not AI opponents. The fixture
includes A/B input sequences, but successful grabs, pumping, and throws are not
separately asserted. Player control-state transitions confirm two deaths; a captured
window also shows moved players and active bombs. The screenshot is saved locally
at `build/experiments/four-player-lifecycle.png`. Rendering has been inspected for
scene identification, not pixel-perfect fidelity or equivalence after rollback.

The earlier one-controller experiments remain in `results/` as exploratory evidence.
In particular, omitting native stacks in steady-state 12- and 240-update windows
**did not diverge** (`battle-negative-stacks*.txt`, expected-control failures).
The battle-entry transition is the meaningful stack negative control: continuations
actually change there. Do not cite the steady-state failures as passing tests.

The **previous implementation** copied about 531–532 MiB per battle snapshot,
with observed captures of 155–280 ms and restores of 10–83 ms. Those historical
runs overlapped. The current optimized results and serial performance measurements
are in [checkpoint-performance.md](checkpoint-performance.md). The original harness renders intermediate frames by default; the
[catch-up follow-up](catchup-performance.md) adds verified presentation suppression
and measures complete multi-update windows.

## Component test

`tests/run_coroutine_replay.sh` compiles the **actual**
`src/game/process_coroutines.cpp` bridge into a standalone executable, substituting
synthetic entry functions for the ROM functions. It runs at both `-O0` and `-O2`.
Its checks remain active with assertions disabled.

Passing cases, recorded in [the current component results](results/coroutine-replay-current.txt):

- Save before first execution, restore, and reproduce the first update.
- Save suspended processes with native locals and pointers into their stacks.
  Restore and replay twelve inputs 100 times, comparing RAM and selected register
  and process-state outputs after **every** update.
- Negative control: restoring RAM/registers without native stacks diverges.
- Change an input: obtain a different outcome, then reproduce that outcome after
  restoring and applying the same changed input.
- Rewind across natural process termination.
- Reject a snapshot whose coroutine allocation has actually been freed.
- Rewind across destruction/recreation and remove a process created after the
  checkpoint, using an experiment-only allocator that retains old allocations.
- Restore/replay with all 80 process slots occupied.

The compact ARM64 80-process snapshot occupies 213,376 bytes (about 208 KiB),
including the test's 4 KiB RAM buffer and process metadata, versus 42,115,456 bytes
with whole native stacks. This synthetic maximum-pool case is not battle memory
usage. Printed component timings are individual samples, not latency budgets.

These snapshots work only in the same process at stable addresses, with all Hudson
coroutines suspended. The component fixture deliberately retains allocations until
cleanup; the real-game history uses reference ownership. This component alone does
not snapshot OS threads or device state and does not establish machine-to-machine
determinism. The additional RAM component test checks ring eviction, repeated
immutable restore, writes to previously untouched pages, and adjacent mappings.

## Build and startup

The app builds at `build/BM64Recompiled.app`, with an ad-hoc code signature.
`codesign --verify --deep --strict` passed on the final build. The real-game
startup probe passed with exit code 0 and 300 controller reads. At reads
60/120/180/240/300 it reported seven allocated Hudson coroutines occupying
3,680,768 bytes, with no Hudson process executing at the sampling point.
Native thread-start callbacks reported IDs 1, 3, 4, 19, 2, 4, and 10 (ID 4 was
reported twice). This is startup evidence, not a battle-state measurement.
See [the startup output](results/startup-probe.txt) and
[the initial build manifest (historical)](results/manifest.json).

The log includes a nonfatal executable-preload warning. Graphics initialization
and the probe complete. The initial desktop-automation attempt stalled; the
coordinated experiment now captures only its own game window using macOS tools.

The initial bundled app aborted before `main`: Homebrew's `sdl2` was actually
`sdl2-compat` 2.32.72, whose dynamically loaded SDL3 library was not included by
the bundle step. The recipe uses native SDL **2.30.3**, the version specified by
the upstream release workflow, built in `../tools/sdl2-install`. It does not
replace the user's installed SDL package.

The macOS bundle step also now refreshes SDL from the configured CMake target
before resolving dependencies. Without this, an incremental rebuild could retain
the old same-named dylib already inside `Contents/Frameworks`.

Another build fix is the one-line `<stdlib.h>` include in the pinned hlsl++ scalar
backend, required for its `labs` calls with this compiler. The exact change is
preserved in [patches/hlslpp-stdlib.patch](patches/hlslpp-stdlib.patch), and the build
recipe applies it idempotently. Nested dependency revisions remain pinned.

The `--smoke-test` switch starts the real game using the stored ROM, supplies
neutral player-one input, logs native game-thread IDs and coroutine allocations,
and exits after 300 controller reads. It requires `APP_FOLDER_PATH`; the Python
runner supplies a fresh temporary directory and imposes a 45-second timeout.
Auto-start waits until the first VI callback has initialized the launcher video
mode. Starting before runtime initialization was an invalid probe setup and
caused a null video-mode access; that ordering is corrected in the probe.
The deliberate process exit bypasses runtime shutdown. Controller reads are
**not assumed to equal gameplay updates**. This check does not prove rendered
fidelity, successful battle play, deterministic replay, or graceful shutdown.

## Reproduce

Prerequisites used on this Mac:

- Xcode 26.6 / Apple Clang 21.0.0, plus the Metal Toolchain component.
- CMake 4.4.3, Ninja 1.13.2, LLVM 18.1.8 (MIPS patch compiler/linker), FreeType 2.14.3.
- Python with `macholib` available to `/usr/bin/python3`, for the upstream Mach-O
  linker wrapper. The installed version here was 1.15.2.
- The existing local US 1.0 big-endian ROM. No ROM was downloaded or committed.

System build prerequisites were installed with Homebrew (`cmake ninja llvm@18
freetype`) and `xcodebuild -downloadComponent MetalToolchain`.

The main checkout's dependencies were restored with:

```sh
git submodule update --init --recursive --jobs 8
```

The sibling `../tools` directory contains these source checkouts:

| Directory | Upstream | Pinned revision |
| --- | --- | --- |
| `N64Recomp` | `https://github.com/N64Recomp/N64Recomp.git` | `98bf104b1b5ed83126af8bcab0cc964782617dbf` |
| `bm64` | `https://github.com/Bomberhackers/bm64.git` | `c7c5fd5a73a9d5969673535030fc4f1df956fa5f` |
| `SDL2` | `https://github.com/libsdl-org/SDL.git` | `fb1497566c5a05e2babdcf45ef0ab5c7cca2c4ae` (`release-2.30.3`) |

Clone those repositories when recreating the workspace, check out the revisions
above, and initialize N64Recomp's recursive submodules. The separate CLI
recompiler revision comes from the release workflow; it is intentionally different
from the runtime's nested N64Recomp revision.

From this checkout:

```sh
# This toolchain identifier is specific to the component installed on this Mac.
# Find another installation's identifier with xcodebuild -showComponent MetalToolchain.
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  sh experiments/build-macos.sh '../Bomberman 64 (USA).z64'

sh tests/run_coroutine_replay.sh
python3 tests/run_startup_probe.py
sh tests/run_battle_replay.sh
```

The build script verifies input ROM SHA-256
`e6da7c26127788cd894b88b71cc055ff9dec0d0f4f8e10d9b15b40153af2b52a`, uses the
upstream decompressor, and checks output SHA-256
`db712de814fb56a958d21c7bbf19563b7f9f05d5a3ad96d5c23f89ab69e887d8` before code
generation. The normal app uses the original compressed ROM; the generated-code
build uses the decompressed file. ROMs and generated code remain ignored by Git.

## Reproduce a single battle checkpoint

The build recipe applies the runtime modifications from
[patches/runtime-checkpoint.patch](patches/runtime-checkpoint.patch) idempotently,
including the new runtime header. This preserves edits inside the pinned runtime
submodule without changing its recorded upstream revision. It also builds the MIPS
patches explicitly before rebuilding the app.

For an incremental build and the long four-controller test:

```sh
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 cmake --build build --target PatchesBin -j 8
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 cmake --build build --target BM64Recompiled -j 8
BM64_INPUT_SCRIPT=tests/fixtures/battle-active-inputs.txt \
BM64_CHECKPOINT_FRAME=1945 BM64_REPLAY_LENGTH=400 BM64_STOP_FRAME=2500 \
BM64_REQUIRE_BATTLE=1 BM64_RESULT_NAME=four-player-lifecycle \
python3 tests/run_checkpoint_probe.py
```

Add `BM64_HOLD_FRAME=2100` to capture the game window at that boundary. This adds a
three-second inspection pause on each traversal and needs macOS screen-capture
access. Screenshots stay in the ignored build directory. They are optional for the
replay assertions. The runner locates its own child process's window by PID.

The input format is `start end hex_buttons stick_x stick_y [controller]`, with an
inclusive start, exclusive end, and zero-based controller (default zero). The
recording navigates the actual menus; no battle state is injected into game RAM.
The main entry points are `src/game/checkpoint.inc`, the frame hook in
`patches/required_patches.c`, and the runtime patch. Current source/binary/result hashes are recorded in
[decision-manifest.json](results/decision-manifest.json). The earlier manifests
describe historical builds. Final validation uses `simplified-` result names so the
preceding native comparison's replay results remain available.

## Remaining gates before netplay

The [minimal dedicated-server lobby plan](online-lobby-plan.md) changes the next
milestone to a private two-to-four-player network playtest. Performance targets
remain acceptance criteria for reliable 60 Hz support, but no longer block building
the prototype and collecting feel feedback. Independent-client determinism and
correct live input/audio/round transitions remain correctness gates. The server
will coordinate one room and relay inputs; clients retain full-game rollback.

1. Meet the end-to-end catch-up budget. The 4/8/12-update path is implemented and
   verified; see [its timings and remaining costs](catchup-performance.md). Capture
   alone is fast, but native framebuffer writebacks and presentation must be included.
   [Running-loop results](running-loop-performance.md) now measure those costs with
   CPU/GPU overlap, absolute deadlines, display timestamps, and checkpoint tail spikes.
   [Pacing and recovery](pacing-and-recovery.md) adds display-aligned releases, bounded
   drawables, hitch recovery, and several hundred fresh-process rollback events.
   [Native framebuffer attribution](native-framebuffer-performance.md) separates
   CPU upload waits, GPU execution, and scheduling. Its two candidate optimizations
   were subsequently [removed after independent comparison](optimization-decisions-and-delivery.md).
   The latest simplified path still misses the headroom and four-refresh recovery
   targets. Phase changes can improve cadence while retaining older images, so
   absolute display age and held frames remain part of acceptance.
2. Separate simulation from presentation. Validate GPU/framebuffer readback,
   interpolation history, duplicate effects, and audio commitment when intermediate
   frames are suppressed. The tested scene's CPU state matches; RT64 history is not
   rewindable by this experiment.
3. Broaden beyond the now-tested three stages and one CPU-player fixture. Each new
   fixture passes 1,200 updates with 53 wrong predictions corrected. Longer sessions,
   successful grabs/throws/pumping, round completion/restart, pause/settings, and
   unusual thread/timer lifetimes still need dedicated coverage. Entry into battle
   is covered; end-of-round transitions are not yet established by these tests.
4. Verify independent peers on other machines/builds and compare the fixed-clock
   cooperative mode against the original runtime's gameplay timing. Same-Mac
   reproducibility alone is not a cross-platform determinism guarantee.
5. Implement the proposed network/session layer and prediction policy in the
   linked lobby plan. There are currently no packets, lobbies, remote input
   transport, or production rollback UI; this remains a plan, not implemented support.
