# Framebuffer and running-loop rollback experiment

Date: 2026-10-24. Apple M5 Pro, 64 GiB, macOS 26.4, Metal.

## Result

**Normal play with checkpoint capture fits the CPU budget; routine four-update
rollback is not yet reliably within a 60 Hz end-to-end deadline.** Eight and
twelve updates remain stress cases, not acceptable routine windows. Full-state
correctness passes. The safe optimizations reduce RAM stores by about 21%, but
do not establish smooth, low-latency rollback. Presentation backpressure and
native framebuffer synchronization remain significant costs.

## What is measured

The existing fully drained catch-up benchmark remains available through
`tests/run_catchup.sh perf`. `tests/run_drained_profile.sh` runs that same path
with stage counters and paired original/optimized cases. Its total includes
restore, every replay update, corrected-history captures, and a final graphics
and presentation queue drain. That is a useful serialization stress test, but
it prevents the overlap that ordinary play uses.

The new `tests/run_running_loop.sh perf` uses an absolute 60 Hz schedule, a
two-update input buffer, and a 16-checkpoint rolling history. After 120 warmup
updates, it withholds one controller's input once every 60 updates. It predicts
by holding the last received sample. When the missing inputs arrive, it restores
4, 8, or 12 updates, replays with the corrected inputs, and runs the next normal
update. All replay updates retain native rendering and RAM writebacks; only the
new normal update is submitted for presentation. Interpolation is disabled for
that corrected presentation. Four updates are the provisional routine case;
eight and twelve are stress cases.

Each 720-update run has 600 measured cycles and ten rollbacks. Work timing
includes restore, replay, the normal update, and all checkpoint captures. The
schedule does not slide after an overrun: absolute deadline lateness includes
backlog and waking late. There are **no per-cycle GPU drains**. A one-time drain
and 100 ms pause clear unpaced menu-navigation backlog before warmup; a final
drain and callback collection happen after all measured cycles. Otherwise
normal presentation and workload queues overlap the next game update.

Metal drawable `presentedTime()` callbacks measure actual drawable presentation,
not merely `present()` returning or a queue ID becoming complete. The report
records each cycle's work, absolute deadline, and displayed timestamp. It reports:

- CPU work over 16.667 ms and missed absolute CPU deadlines.
- Strict display deadlines of release + 16.667 ms, including missing callbacks.
- Display intervals, especially gaps over 25 ms (a missed 60 Hz slot).
- Display latency from scheduled update release.
- A separate cadence deadline calibrated once from warmup's median display
  latency, with 1 ms tolerance. This detects extra queuing or missed slots without
  treating a constant pipeline delay as stutter. It does **not** establish a
  one-frame latency budget. Calibration never changes during the measured run.

A control without rollback retains normal updates and checkpoint captures.
Timing and hash validation are separate runs: hashing all 512 MiB every update
would invalidate performance results. Verification is never weakened to obtain
fast timings.

## Stage attribution

Counters are wall times on their owning threads, not GPU timestamp queries:

| Counter | Meaning |
| --- | --- |
| Simulation/runtime residual | Cycle work minus checkpoint/restore and game-thread graphics fences; includes game logic, coroutine scheduling, audio-task computation, and deferred COW faults. |
| Checkpoint / restore | Complete state capture/restore calls, including metadata and native continuation stacks. |
| Graphics fence | Game-thread wait for display-list/native framebuffer processing and VI action completion. Includes worker scheduling delay. |
| Graphics worker | Display-list interpretation and RT64 full-sync processing. |
| Native stage | Native framebuffer preparation, rendering, upload/readback synchronization, conversion, and RAM copy. |
| Native GPU/upload wait | Uploader completion plus native command submission and GPU fence wait. Does not separate GPU rendering from conversion/readback. |
| RAM copy | Readback buffer mapping, CPU copy/byte swap, optional comparisons, and RAM stores. |
| Drawable acquire | Waiting for an available Metal drawable, including display backpressure. |
| Presentation GPU wait | Submission/completion of the final presentation draw. |
| Pacing / swapchain wait / present call | Explicit high-rate sleep, swapchain fence wait, and present API call respectively. |
| Presentation dependency | Present thread waiting for a workload to become ready. |

These counters overlap and nest: native wait/copy are inside native stage, which
is inside graphics work and mostly inside the game's graphics fence. Presentation
work runs concurrently. **Do not add all counters to get frame time.** The measured
cycle and actual displayed timestamps are the end-to-end measurements. Per-rollback
stage records are also retained in the raw logs.

## Framebuffer consumers and safe changes

A separate `BM64_TRACE_MEMORY=ON` build instruments generated game, patch, and
runtime loads/stores with compiler callbacks. A byte ownership map marks native
color/depth writes; CPU stores clear ownership and subsequent CPU loads report
reads of framebuffer-produced bytes. RDP tile/block/TLUT load observers also check
for framebuffer-owned texture inputs. Tracking begins before battle entry, so a
result written before the measurement window is still tracked. A canary verifies
that an instrumented load and texture observer see a marked byte, and that a CPU
overwrite clears both observations. The tracer does not change game RAM values.

Across frames 1810–2609 (800 updates of the existing battle fixture), the trace
observed **zero game CPU read sites and zero framebuffer-owned texture loads**.
It saw 30 write regions, including cropped buffers during transition. Stable
battle output alternates between color buffers at `0x00100000` and `0x00125800`
and writes depth at `0x00200000`. Each full buffer is 153,600 bytes; typical
updates copy one color result and two depth results, totaling 460,800 bytes.
See `results/framebuffer-consumers.txt`; the earlier 400-update steady-battle
trace is in `results/framebuffer-read-trace.txt`.

This is bounded negative evidence. Uninstrumented native helpers/bulk operations,
RSP accesses, implicit GPU attachment reads, other stages/effects, and longer
sessions are not a complete consumer audit. RT64 itself reads RAM to detect
framebuffer changes, reload render targets, and service VI presentation. Absence
of game CPU/texture reads does not authorize deleting those bytes or their work.

Two independent, opt-in optimizations preserve the full-state contract:

1. `BM64_INCREMENTAL_WRITEBACK=1`: read back and byte-swap the full native output
   into reusable scratch storage, compare 4 KiB blocks, and store only differing
   blocks into RDRAM. Identical blocks remain part of every snapshot and every
   hash. This avoids unnecessary stores/COW faults, including repeated identical
   depth output; it does not skip GPU work, readbacks, or native-buffer history.
   Short/partial-word copies retain the original implementation. A randomized
   test compares the complete output against the original byte-swap operation,
   checks surrounding guards, and checks that repeating identical data stores
   zero bytes.
2. `BM64_SKIP_HIDDEN_FENCE=1`: for an already suppressed replay screen update,
   update the host VI register copy directly instead of sending a no-op screen
   action and semaphore fence to the graphics worker. Graphics tasks retain their
   fences; VI/timers/messages still advance, and the next visible update uses the
   normal path. This is limited to the cooperative checkpoint experiment.

Both flags default off; the benchmark's `optimized` cases enable both. Normal
threaded play retains its original behavior. No RAM ranges are excluded, no
hashes are masked, and no native framebuffer output is discarded.

## Measured running-loop results

These first paired runs each contain 600 measured cycles and ten rollback events.
Times below are milliseconds; rollback work includes the additional normal update.
“CPU misses” use the absolute deadline, including late wakes. “Cadence misses”
use the fixed warmup calibration described above, and therefore include persistent
extra queue delay after a hitch, not just the hitch itself.

| Path | Replay updates | Work median / max | CPU misses / 600 | Cadence misses / 600 | Gaps >25 ms | Display latency median |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Original | 0 | 2.985 / 4.995 | 0 | 0 | 0 | 40.899 |
| Optimized | 0 | 2.851 / 13.822 | 0 | 0 | 0 | 44.429 |
| Original | 4 | 14.928 / 15.780 | 5 | 459 | 1 | 55.187 |
| Optimized | 4 | 13.903 / 17.210 | 4 | 600 | 0 | 47.945 |
| Original | 8 | 23.501 / 25.609 | 11 | 575 | 1 | 65.378 |
| Optimized | 8 | 23.878 / 25.004 | 10 | 575 | 1 | 57.891 |
| Original | 12 | 32.595 / 35.228 | 20 | 571 | 2 | 73.332 |
| Optimized | 12 | 31.931 / 34.313 | 18 | 571 | 2 | 73.333 |

No displayed callbacks were missing. **Every measured frame in these runs missed
the strict release +16.667 ms display deadline**, even in controls. Typical
presentation intervals were 16.667–16.668 ms, but a missed slot increased queue
latency that did not subsequently recover. The original eight-update run's
largest gap was 50.002 ms; twelve-update runs had two 33.335 ms gaps.

The optimized four-update run already entered measurement with about one extra
display interval beyond its warmup median. Its 600 calibrated misses do not mean
600 stutters or 600 rollback-caused failures: it had no measured >25 ms gaps.
This illustrates why all three views—absolute latency, calibrated delay, and
actual gaps—must be retained. Uncontrolled wake/display phase makes these short
runs unsuitable for a precise input-latency claim.

The original four-update run had no work duration over 16.667 ms but five absolute
CPU misses. From each missed cycle's work and deadline lateness, its start was
2.5–5.6 ms late. Four updates therefore need more headroom and better wake/present
coordination, even before considering other machines.

### Four-update routine-target repeats

Two additional fresh-process runs per path give **30 rollback events across
1,800 measured cycles per path**. Original work median/max was 14.592/16.517 ms;
optimized was 13.886/17.210 ms. Both paths had 14 absolute CPU deadline misses.
Original had three measured >25 ms presentation gaps; optimized had one. All
callbacks arrived, but all 1,800 strict one-frame display deadlines were missed
in each path. The optimized path had one work duration over 16.667 ms; the
original had none. Cadence misses were 984 original / 624 optimized, counting
persistent added queue delay, with the warmup caveat above.

The mean work reduction was 14.159 to 13.682 ms (about 0.48 ms). That is modest
and does not improve the observed absolute CPU deadline-miss count. Display
phase and wake variability prevent claiming a reliable stutter reduction from
three short runs. `results/four-update-aggregate.json` preserves the aggregate;
all per-frame logs remain available.

### Where the running-loop work goes

Mean stage times for the optimized rollback events in the first paired run:

| Replay + normal updates | Total work | Capture | Restore | Graphics fence | Native stage | Native GPU/upload wait | RAM copy |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 4 + 1 | 13.548 | 0.477 | 0.320 | 9.730 | 7.354 | 3.495 | 0.560 |
| 8 + 1 | 23.030 | 0.870 | 0.301 | 16.577 | 13.248 | 6.310 | 0.950 |
| 12 + 1 | 30.134 | 1.443 | 0.251 | 21.531 | 17.743 | 8.627 | 1.293 |

For 4 + 1, the remaining simulation/runtime residual averages 3.021 ms. The
native stage and waits are nested inside the graphics fence. Optimized normal
cycles have checkpoint p99 around 0.2 ms; the aggregate per-cycle checkpoint
maximum rises when a rollback captures several updates.

Changed-block writeback reduced stored RAM bytes by **21.1–21.2%** across the
running-loop cases. It still read back every native byte. The original versus
optimized four-update mean work was 13.950 versus 13.548 ms in the first pair,
but eight-update mean work did not improve (22.810 versus 23.030 ms). The modest,
inconsistent timing benefit does not establish a deadline guarantee; both
optimizations remain opt-in. The experiment did not isolate the two flags' timing
contributions, so it does not assign a measured speedup to either flag alone.

### Fully drained benchmark retained

Forty windows per case, using the original battle fixture:

| Path | Replay updates | Total p99 / max | Work excluding final drain p99 / max |
| --- | ---: | ---: | ---: |
| Original | 4 | 32.077 / 46.817 | 9.553 / 14.907 |
| Optimized | 4 | 33.897 / 46.665 | 7.603 / 14.873 |
| Original | 8 | 48.148 / 48.291 | 15.072 / 16.048 |
| Optimized | 8 | 48.830 / 49.211 | 19.435 / 22.695 |
| Original | 12 | 48.949 / 52.383 | 20.528 / 20.622 |
| Optimized | 12 | 49.096 / 143.779 | 23.640 / 143.779 |

All 240 fully drained windows exceeded 16.667 ms. Drawable acquisition dominates
presentation-side waiting: original means were 25.917, 26.907, and 42.473 ms for
4/8/12 updates. Presentation GPU waits averaged 0.7–1.1 ms. Explicit pacing was
zero, swapchain waits about 0–0.001 ms, and the present call about 0.02 ms. Thus
“presentation wait” was primarily drawable availability/backpressure, not a
large `present()` call or an explicit pacing sleep. Those asynchronous counters
overlap the replay and cannot simply be subtracted from its cycle budget.

The 143.779 ms stress outlier contained a 70.532 ms restore and 34.774 ms of
captures; native GPU/upload wait was only 7.440 ms. It is retained in the table,
not trimmed. The current counters cannot distinguish host descheduling from VM
remap/deallocation latency inside the checkpoint operations. That remains an
unresolved tail-risk investigation. A fresh 40-window optimized twelve-update
repeat did not reproduce it: total max 48.963 ms, restore max 0.134 ms, and work
excluding drain max 21.402 ms. That does not invalidate or explain the original
outlier. The repeated stress run is retained separately.

## What remains limiting or uncertain

1. **Native framebuffer processing is on every replay's critical path.** In the
   routine case it accounts for about 7.35 ms of the 13.55 ms mean cycle, including
   3.50 ms of uploader/GPU waits. Trace dependencies between framebuffer pairs,
   repeated depth conversions, and barriers next; only remove work after proving
   its final bytes and dependencies remain identical. The current consumer trace
   does not justify dropping framebuffer RAM or native rendering.
2. **Wake timing and presentation queue recovery need work.** Several millisecond
   late wakes consume the routine window's remaining headroom. A missed present
   can leave an extra queued interval permanently. Investigate display-aligned
   release/deadline scheduling and retiring obsolete queued presentations while
   preserving CPU/GPU overlap and all simulation updates. Removing more end-of-
   window fences will not by itself fix native readback or a backed-up drawable
   queue. Even controls have substantial release-to-display latency; the two-
   update input buffer adds another 33.3 ms before its sampled input is simulated.
3. **Checkpoint tail latency is not bounded by this experiment.** The 144 ms
   drained outlier needs OS scheduling/VM attribution if it recurs; its successful
   repeat is not a latency guarantee. No outliers were discarded.
4. **This is one Mac, one stage, short sessions, and scripted delayed input.**
   There is no real network transport/jitter test, cross-machine determinism
   proof, physical input-to-photon measurement, or GPU/interpolation-history
   checkpoint. Full RAM/context agreement does not prove pixel-perfect temporal
   rendering after a correction. Audio/rumble output remains suppressed in the
   checkpoint experiment. Those gates remain before production netplay.

## Correctness and reproduction

All six 720-update delayed-input verification runs passed (original and optimized,
4/8/12), each correcting ten wrong predictions. The altered-input negative
control detected divergence. The optimized original battle suite and final-frame
suppression suite also passed, including altered-input and omitted-stack negative
controls. Their baseline RAM hashes matched 1,077 preserved pre-optimization
hashes across nine logs. Ordinary startup and the changed-block byte/guard test
passed. No replay comparison or memory range was relaxed.


`tests/run_running_loop.sh verify` first records a fully rendered truth run, then
restores its anchor and replays the same sequence with missing input and actual
wrong predictions. Every corrected replay update and every ordinary truth state
must match the **whole 512 MiB RAM hash and raw register/context hash**. Each case
must correct at least one prediction that actually changed state. The two paths
use the same process: raw contexts contain native pointers, so cross-process
register hashes would not be a valid oracle. Independent-process RAM comparisons
remain available in the original battle suite.

```sh
# Normal build (never time the instrumented build).
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake -S . -B build -DBM64_TRACE_MEMORY=OFF
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake --build build --target BM64Recompiled -j 8
sh tests/run_running_loop.sh verify
BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 sh tests/run_battle_replay.sh
BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 sh tests/run_catchup.sh verify

# Run timing cases serially, without other game runs/builds.
sh tests/run_running_loop.sh perf
sh tests/run_drained_profile.sh

clang++ -std=c++17 -O2 tests/changed_writeback.cpp -o build/experiments/changed_writeback
build/experiments/changed_writeback

# Separate tracing build; restore OFF and rebuild before timing again.
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake -S . -B build -DBM64_TRACE_MEMORY=ON
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake --build build --target BM64Recompiled -j 8
sh tests/run_framebuffer_trace.sh
```

The build recipe preserves runtime, RT64, and Plume changes as patches against
pinned upstream submodules. Raw logs and a source/binary/result manifest accompany
the measured results. The saved trace app is an ignored local diagnostic artifact;
it is not the timing executable.
