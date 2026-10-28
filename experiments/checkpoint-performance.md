# Checkpoint performance follow-up

2026-10-24 · Apple M5 Pro, 64 GiB · macOS 26.4 · ARM64 release build.

**The checkpoint cost now fits a real-time frame budget on this Mac.** Two serial
600-update battle runs captured a checkpoint every update and retained the latest
16. Capture p99 was 0.121–0.131 ms; restore p99 was 0.016–0.017 ms without verification
hashes. Game work including capture and deferred copying was below 16.7 ms at p99
in both measured runs, with occasional 18–19 ms spikes. This establishes a useful checkpoint primitive, not a complete
rollback netcode implementation or a worst-case timing guarantee.

## What changed

- RAM uses macOS copy-on-write mappings of the **entire 512 MiB region**. Unchanged
  pages are shared, and writes copy affected pages. Restore remaps the saved image
  at the original address. Untouched pages are covered too; no RAM range is omitted.
- ARM64 minicoro images copy their header/storage and live stack tail, including a
  conservative 256-byte red zone. A saved pointer outside its own stack, unsupported
  architecture, or explicit reference mode falls back to a whole-stack copy.
- The 16 history slots own references to native allocations. Retired processes can
  be restored while referenced; allocations are freed when no live game or snapshot
  owns them. Dead OS continuations are not recopied. The diagnostic still caps OS
  thread generations at 64 and retained native allocation capacity at 512 MiB.
- Unchanged overlay/function maps share one immutable image across snapshots.
  Runtime queues, timers, VI state, input/audio settings, saves, and RSP DMEM have
  independent images per history slot.
- `BM64_FULL_COPY=1` retains the eager RAM/native-stack implementation as a reference.
  The non-macOS RAM fallback also copies the full region; it has not been benchmarked.

The ring has 16 slots; verification additionally holds one baseline anchor. The
performance run uses only the ring. Snapshot handles are same-process objects,
not serializable saves. No new dependencies were added.

## Serial measurements

Each run used fresh settings, the existing US 1.0 ROM, the four-controller battle
fixture, and 600 updates beginning at boundary 1945. Runs were sequential with no
other game test running. Full-RAM/register verification was disabled only for these
measurements; the separate replay tests retain it. Intermediate frames still render.

| Measurement | No snapshots | COW ring | COW ring repeat |
| --- | ---: | ---: | ---: |
| Capture p50 / p95 / p99 (ms) | 0 / 0 / 0 | .067 / .098 / .121 | .060 / .093 / .131 |
| Capture maximum (ms) | 0 | .434 | .978 |
| Game + capture p95 / p99 (ms) | 15.190 / 15.348 | 15.220 / 16.072 | 15.105 / 15.367 |
| Game + capture maximum (ms) | 15.672 | 19.141 | 18.277 |
| Restore p50 / p95 / p99 (ms) | — | .010 / .014 / .016 | .009 / .017 / .017 |
| Restore maximum (ms) | — | .086 | .074 |
| Process COW faults during the run | 0 | 39,328 | 39,328 |
| Process footprint growth (MiB) | 11.78 | 17.34 | 17.92 |

Evidence: [control](results/perf-no-snapshot.txt), [ring](results/perf-cow.txt),
[repeat](results/perf-cow-repeat.txt). Restore samples are 48 remaps: three passes
through all 16 images after the timed gameplay interval. Their subsequent replay
correctness is tested separately by the ring replay case.

The game interval includes the writes that trigger COW faults, graphics synchronization,
and normal scheduling. Capture time is added to that interval; allocator work during
capture is included. Per-boundary footprint sampling is outside the timed interval.
Renderer/vsync scheduling accounts for much of the frame distribution and its run-to-run
variation. A lower percentile than the control is not evidence that snapshots speed up play.
The occasional over-budget updates mean this is not yet a guaranteed 60 Hz rollback loop.

The measured eagerly stored ring payload is **2,511,456 bytes (2.40 MiB)**, counting
native stack image sizes, snapshot structs, and DMEM. It excludes runtime container
storage, spare vector capacity, shared overlay images, kernel bookkeeping, and RAM
pages created by COW. Each snapshot still maps 512 MiB of *virtual* RAM; calling it
merely a 50 KiB snapshot would be misleading. Initial battle anchors eagerly copy
about 50–52 KiB of native stack data; stack depth changes during the match.

Whole-process footprint growth was about **5.6–6.1 MiB above the control's growth**
for this window. That is an observation including the renderer and allocator, not
an exact measurement of checkpoint-owned RAM or an upper bound for other scenes.
Retained native allocation capacity at the end was 35,197,952 bytes with the ring
versus 32,043,008 without it; unused reserved stack capacity is not all resident.

The eager-copy reference in this build copies 557,345,792 bytes (531.5 MiB) and
took 263.586 ms to capture a battle anchor; its three restores took 9.499–57.023 ms
([reference log](results/full-copy-reference.txt)). The previous implementation took
155–280 ms per capture in historical diagnostic runs. Those were not controlled performance
runs, but the removal of that full copy is substantial. The new copy-on-write and
stack component tests cover data outside the small region touched by this battle.

## Correctness and startup checks

- Existing 1/4/8/12-update battle windows, 400-update battle lifecycle, and 120-update
  stage-to-battle transition: three restores each, checking all 512 MiB and the
  Hudson/N64 register hashes after every replayed update (1,635 checked updates).
- [Rolling battle history](results/compact-ring.txt): 401 consecutive captures,
  eviction of old slots, then replay 12, 8, 4, and 1 updates back (25 checked updates).
- Changed-input and omitted-native-stack negative controls still detect divergence.
- Independent baseline hashes agree across overlapping short/long battle runs.
- The full-menu boot and eager-copy reference runs retain the same replay checks;
  their overlapping RAM hashes also [match](results/reference-comparison.txt) the
  optimized fast-boot run.
- [RAM component](results/checkpoint-memory.txt): 16-slot history, repeated restore,
  immutable saved images after writes, far-end and future sparse writes across the
  full 512 MiB, adjacent mappings preserved, and the eager-copy fallback.
- [Actual process bridge at O0/O2](results/coroutine-replay-current.txt): stack locals,
  native pointers, repeated replay, termination/recreation, stale generations,
  negative controls, and all 80 slots. Compact pool image: 213,376 bytes versus the
  former 42,115,456 bytes.
- Normal threaded startup and optional Single Battle boot pass. The latter was
  visually checked at character select after the scripted input handoff. Bundle
  signature verification passes.

## Optional Single Battle startup

```sh
./build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled --single-battle
```

This starts the configured local ROM and traverses the game's existing menus with
scripted input. Intro rendering/audio are skipped, and input returns to the player
at update 1600 on Single Battle's character-select screen. Normal threaded mode
reached the handoff in about three seconds on this Mac. Without the flag, ordinary
startup is unchanged. It is optional rather than a new permanent default.

The script is calibrated to the supported US 1.0 game/menu flow and tested with fresh
settings. Modified menus or save-dependent dialogs need separate validation before
making this the default launch experience. It does not write a fabricated battle
state into RAM. Tests use `BM64_SINGLE_BATTLE=1` to request the same shortcut.

## Reproduce

Use the build recipe in [README.md](README.md). From the checkout root:

```sh
sh tests/run_coroutine_replay.sh
clang++ -std=c++20 -O2 tests/checkpoint_memory.cpp -o build/experiments/checkpoint_memory
./build/experiments/checkpoint_memory
sh tests/run_battle_replay.sh
sh tests/run_checkpoint_performance.sh
python3 tests/run_startup_probe.py
BM64_CAPTURE=1 python3 tests/run_single_battle_probe.py
```

Omit `BM64_CAPTURE=1` if a screenshot is unnecessary. The normal boot probe checks
arrival at the handoff boundary and continued execution; the screenshot is the
manual scene check. It and the other runners use isolated saves and bounded deadlines,
then end their own child processes. Screenshot: `build/experiments/single-battle-normal.png`.
The battle suite defaults to fast boot; `BM64_SINGLE_BATTLE=0` runs the original menus.

To exercise the eager reference, run a **single-anchor** short replay with
`BM64_FULL_COPY=1`, `BM64_CHECKPOINT_FRAME=1980`, `BM64_REPLAY_LENGTH=12`,
`BM64_STOP_FRAME=2500`, `BM64_REQUIRE_BATTLE=1`, the battle input fixture, and
`tests/run_checkpoint_probe.py`. A 16-slot full-copy history would intentionally
consume several GiB and is unnecessary for this reference check.

[performance-manifest.json](results/performance-manifest.json) records the exact
source, executable, and result hashes. Historical manifests predate these changes.

## Remaining limits

COW is currently optimized for macOS; compact stack images are optimized for ARM64.
Other platforms need their own validated implementation. RT64 GPU history is not
restored, intermediate replay frames still render, audio commitment is absent, and
the original measurements here did not include multi-update catch-up. The
[subsequent experiment](catchup-performance.md) now measures 4–12 updates with only
the final update presented, while retaining native framebuffer writebacks. Longer sessions,
other stages, AI, round transitions, and cross-machine determinism remain open.
