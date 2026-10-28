# Native framebuffer attribution and optimization

Historical experiment: the two candidate rendering optimizations described here
were later removed after an independent four-way comparison. See
[the current decisions and delivery results](optimization-decisions-and-delivery.md).

Date: 2026-10-24. Apple M5 Pro, 64 GiB, macOS 26.4, Metal, approximately 60 Hz.
This follows [pacing and recovery](pacing-and-recovery.md). The input buffer stays
at **two updates**, with the existing fresh-settings 2x MSAA. No framebuffer byte
ranges or replay hashes are excluded.

**Result: modestly less native work and no measured display-latency increase, but
the four-update tail/headroom target remains unmet.** The clean median complete
rollback cycle improved by 0.337 ms; its p99 did not improve. The new attribution
separates the recurring native cost from checkpoint and scheduling tails.

## What is now measured

The former `native_gpu_upload_wait_ms` remains for historical comparison. A new
`NATIVE_PROFILE` record separates:

| Field | Meaning |
| --- | --- |
| `CPU_upload_wait_ms` | Waiting for native draw/shader/tile CPU upload workers before submission. Synchronous CPU copying belongs to native preparation, not this wait. |
| `texture_wait_ms` | The separate texture-cache readiness wait. This can contain CPU and GPU texture-upload work; it is not mislabeled as native rendering. |
| `submit_ms` | CPU wall time submitting the native command list. |
| `fence_wait_ms` | CPU wall time waiting for its existing completion fence. |
| `GPU_execute_ms` | Metal command buffer GPU end minus GPU start. |
| `submit_to_GPU_ms` | From immediately before registering the completion handler/committing to GPU start: driver scheduling and queue delay together. |
| `GPU_to_callback_ms` | GPU end to entry into the completed handler. |
| `callback_to_wake_ms` | Completed-handler entry to return from the existing CPU fence wait. Includes the small timing callback and semaphore notification. |

GPU start/end are read in the completed handler, using Metal's documented Mach
host-time domain. See [Apple's GPU timing documentation](https://developer.apple.com/documentation/metal/mtlcommandbuffer/gpustarttime).
No extra GPU drain or timestamp-query dispatch is inserted. The callback runs
before the original fence signal. Missing/out-of-order timestamps increment an
invalid counter; the analyzer rejects invalid samples rather than substituting
CPU wait time for GPU execution.

Submission plus fence wait and the four GPU-timeline intervals are **two views of
the same interval**, not additive categories. The analyzer reports their closure
error. GPU elapsed time can include contention; it is not a hardware utilization
or per-shader busy-time measurement. The timed command list includes native
rendering, buffer copies, conversion, and readback. CPU encoding/preparation and
RAM copies remain visible in the enclosing native-stage and copy counters.

A separate five-second CPU stack sample is retained as
[diagnostic evidence](results/native-cpu-sample.txt). Its run is excluded from
performance comparisons. It showed native fence blocking and Metal encoder
creation/recording among the relevant stacks; it was used for attribution, not
as a replacement for measured execution durations.

## Changes retained for the candidate

`BM64_NATIVE_SHARED_UPLOADS=1` lets synchronous native **plain buffer** inputs use
CPU-visible shared storage on Metal devices reporting unified memory. Typed
texture-buffer views keep their existing GPU-private storage. The native path
writes the same ranges and bytes directly into the shared input, avoiding its
staging-to-private GPU blit and the barrier for that removed copy. GPU read
barriers, native rendering, writebacks, and completion fences remain.

Safety depends on the existing ownership boundaries: a native command list is
fenced before its buffers can be reused, and the workload ring prevents a writer
from reusing a workload still consumed by the screen renderer. CPU uploads finish
before commit. Asynchronous upload callers retain the staging path. Switching
storage mode or growing a buffer recreates its views and uploads the full range,
not just the incremental suffix. These boundaries follow Metal's requirements
for [synchronizing CPU and GPU buffer access](https://developer.apple.com/documentation/metal/synchronizing-cpu-and-gpu-work).
Other backends advertise no support for this option and retain the existing path.

`BM64_NATIVE_LAZY_DITHER=1` selects a color-writeback shader that initializes the
random seed only for NOISE dithering. The seed is unused by magic-square, Bayer,
and disabled dithering. The NOISE branch retains the original integer generator;
all floating-point conversion, pixel addressing, workgroup dimensions, and writes
are unchanged. Both shader variants retain the upstream `-O0` HLSL setting. This
is not a change to the game's RNG or to the dithering pattern. Both original and
candidate shaders are in the same executable for the comparison.

Both switches are opt-in to checkpoint mode. The prior pacing, two-drawable bound,
retirement policy, hidden-frame fence optimization, inline uploads, changed-block
RAM stores, and empty Metal wait-buffer removal remain identical before/after.
Normal play defaults are unchanged.

## Experiments rejected or narrowed

- Sharing **all** native input buffers reduced some CPU copy/encoding work but
  increased measured GPU execution (initial median 3.184 ms versus 2.489 ms per
  five-update rollback cycle). The final candidate excludes typed texture-buffer
  views; it does not assume shared storage is universally faster.
- Disabling upstream dummy GPU keep-alive work increased submission-to-GPU-start
  delays and worsened tail execution. That switch was removed; keep-alive behavior
  is unchanged in the final comparison.
- Lazy dithering alone and the combined narrower candidate were checked in short
  replay probes before the larger comparison. Those exploratory timings are not
  pooled into final before/after results.

The exploratory logs are `native-attribution-before`, `-shared`, `-no-idle`,
`-shared-no-idle`, `-dither`, and `-selective`. The initial shader fixture selected
non-noise color dithering. Actual noise-heavy scenes remain outside the battle
fixture's coverage even though the NOISE expression is unchanged.

## Final comparison method

`tests/run_native_framebuffer.sh perf` uses twelve fresh processes, six per path,
with 120 warmup and 1,080 measured updates each. A four-update correction occurs
every 20 updates: **318 rollback events per path**, 636 total. Before/after order
alternates between paired runs. Builds and other game instances do not run
concurrently with timing tests.

Runs 1–3 are uninjected (159 corrections and 3,240 measured cycles per path).
Run 4 has four 25 ms CPU hitches; run 5 four 100 ms CPU hitches; run 6 four 50 ms
presentation-worker hitches. Eight/twelve updates remain separate stress cases.
The fully drained diagnostic is retained and run for 40 windows per length.

The previous acceptance targets remain: rollback-event p99 <=13.667 ms (3 ms
headroom), no clean CPU deadline misses, and hitch recovery within four refresh
intervals after actual stall end, requiring three consecutive fresh refreshes.
Absolute display latency, held-image age during missing refreshes, and retired
updates are all reported. The fixed input buffer contributes another approximately
33.333 ms before the scheduled simulation release; this is not a physical
input-to-photon test. Hash-heavy correctness runs are separate from timing runs.

## Measured results

[Per-run and pooled results](results/native-analysis.json) retain the raw timing
logs, hitch cases, undisplayed updates, and the two equivalent GPU-timeline views.
The following clean comparison contains three fresh processes and 159 corrections
per path. Each execution includes restore, four replay updates and their
checkpoints, the new normal update, and its checkpoint.

| Clean four-update metric | Before | Candidate |
| --- | ---: | ---: |
| Complete execution p50 / p95 | 13.448 / 15.567 ms | 13.111 / 15.700 ms |
| Complete execution p99 / maximum | 17.829 / 19.992 ms | **18.040 / 18.623 ms** |
| Native stage p50 / p99 | 7.212 / 9.912 ms | **7.052 / 9.398 ms** |
| Absolute CPU deadline misses | 5 / 3,240 | 4 / 3,240 |
| Wake lateness p99 / maximum | 0.520 / 3.341 ms | 0.521 / 1.985 ms |
| New-image release-to-display latency p50 / p99 | 33.327 / 49.998 ms | **33.327 / 33.334 ms** |
| Maximum new-image latency | 66.659 ms | 50.001 ms |
| Gaps over 25 ms | 74 | 32 |
| Maximum held-state age | 83.337 ms | 50.002 ms |
| Undisplayed updates, including source/queue retirement | 77 | 32 |

Typical execution headroom increases from 3.219 to **3.556 ms**, a **0.337 ms**
(2.5%) median execution reduction. Native-stage median falls by 0.160 ms (2.2%).
However, candidate p99 exceeds the 16.667 ms interval by **1.373 ms**, before
allowing for late wakes, and misses the 13.667 ms headroom target. The p95 also
fails that target. This is not a demonstrated tail-latency solution.

The three paired native-stage medians vary: the candidate is slower in the first
pair and faster in the other two. These are small gains relative to run-to-run
variation. The earlier single probe's 13.913 ms maximum is **not** used as the
reliability result. This fresh batch also has more wake jitter and display gaps
on its unchanged baseline than the previous pacing report; comparison is against
the current interleaved control runs, not a favorable historical sample.

The fixed buffer contributes another approximately 33.333 ms, so estimated median
scheduled-input-to-display latency is **66.7 ms on both paths**. All individual
one-refresh presentation deadlines are still missed, including retired updates:
the stable pipeline is two refreshes. The candidate improves the measured display
tail/cadence without changing release phase, input delay, queue bound, or retirement
policy. It does not eliminate gaps.

### Native attribution

Values below are **per five-update correction cycle**, not per update.

| Native interval | Before p50 / p99 | Candidate p50 / p99 |
| --- | ---: | ---: |
| CPU upload-worker wait | 0.001 / 0.001 ms | 0.001 / 0.001 ms |
| Texture readiness wait | 0.000 / 0.747 ms | 0.000 / 0.783 ms |
| CPU submission | 0.039 / 0.069 ms | 0.039 / 0.065 ms |
| CPU fence wait | 3.547 / 6.357 ms | 3.459 / 4.391 ms |
| GPU execution | 2.240 / 5.473 ms | 2.185 / 2.887 ms |
| Submission to GPU start | 0.963 / 1.699 ms | 0.856 / 1.327 ms |
| GPU end to completed handler | 0.332 / 0.513 ms | 0.327 / 0.417 ms |
| Completed handler to CPU wake | 0.041 / 0.063 ms | 0.040 / 0.051 ms |
| RAM copy/writeback | 0.521 / 0.664 ms | 0.518 / 0.760 ms |

The upload workers are not the bottleneck in this configuration. GPU execution,
submission-to-start delay, and native preparation/encoding remain significant.
The GPU median improvement is small, and GPU p95 is slightly worse (2.621 versus
2.557 ms); the lower observed p99 must not be read as proof of a uniformly faster
GPU or a causal explanation for every outlier. Scheduling includes driver/queue
latency, not only time doing CPU work. No completion wait was removed.

There are **801 valid native GPU samples on each clean path**, including cycles
with multiple native batches, and zero invalid timestamps. The maximum closure
error between the GPU timeline and CPU submission+fence interval is **0.006 ms**.
Native readback volume is identical: **366,336,000 bytes per clean path**.
The candidate avoids **8,745 input staging blits / 53,964,576 copied bytes** during
those correction events. Across all 318 candidate corrections this is 17,490
blits / 107,929,152 bytes. These are input staging copies, not removed framebuffer
readbacks or RDRAM writes.

### Controlled hitches and stress

Both paths run twelve controlled hitches. No hitch retains an extra display
interval in its later settling window. Candidate recovery still misses the fixed
four-refresh target in **10/12** cases; all twelve recover within **six** refresh
intervals. That six-refresh observed bound is not a replacement target.

| Hitch, four occurrences each | Before recovery range | Candidate recovery range | Candidate maximum gap / held age |
| --- | ---: | ---: | ---: |
| CPU 25 ms | 4.380–7.397 refreshes | 4.379–4.442 refreshes | 50.003 / 83.337 ms |
| CPU 100 ms | 5.879–5.948 refreshes | 5.848–5.879 refreshes | 150.007 / 166.675 ms |
| Presentation worker 50 ms | 3.847–4.252 refreshes | 3.792–4.040 refreshes | 83.337 / 100.005 ms |

Across all 6,480 measured cycles per path, including injected hitches and backlog,
CPU deadline misses are **53 before / 47 after**, and gaps over 25 ms are
**132 / 76**. Candidate retirement accounts for all 121 undisplayed updates:
37 at source and 84 in the queue. Maximum held-state age during a hitch reaches
166.675 ms even though newly displayed frames have much lower latency.

The 8/12-update running-loop stress cases each have 18 corrections. Execution
median/maximum is **21.591/25.463 ms** for eight and **31.000/34.774 ms** for twelve.
They have 15/28 CPU deadline misses and 38/18 display gaps respectively; both
reach 66.670 ms held-state age. Neither fits a single refresh interval.

### What still limits headroom

The new attribution rules out CPU upload-worker waiting as a useful next target.
The native stage still consumes about 7 ms of a correction cycle: several
milliseconds are preparation/encoding and RAM work outside the measured submission
wait, plus roughly 2.2 ms of actual GPU execution and 0.9 ms getting it started.
The current API aggregates render/resolve/conversion/readback within each native
command list. A further rendering optimization needs per-pass GPU attribution;
removing required writebacks or reducing MSAA would change the tested contract.

Checkpoint mapping tails also consume the remaining margin. The two worst clean
candidate corrections are **18.623 and 18.040 ms**, with capture totals of **3.915
and 4.502 ms**. The latter contains a **3.941 ms** capture call, including
**3.800 ms** clearing an old mapping. A separate **2.791 ms** restore spent
**2.750 ms** in RAM remapping. The trace retains thread CPU and process-wide
fault/scheduler deltas; these locate time but do not prove a kernel cause.
Two other clean misses occur with ordinary checkpoint durations, so mapping
outliers are not the sole remaining problem. Late wakes and the rest of the
execution/presentation path still matter. No outlier is excluded from the tails.

The original 144 ms checkpoint outlier did not recur. Across the final timing,
long verification, and drained native runs, the largest logged checkpoint call
was a 7.087 ms restore in the hash-heavy four-update verification run: 7.062 ms
in remapping, with 5.274 ms total thread CPU. It is retained as diagnostic evidence,
not pooled into the running-loop performance comparison.

### Fully drained diagnostic

The original final-presentation wait remains included. These are 40 windows per
length, using the older four-player fixture. They do not include a new normal
update after each replay window and deliberately drain the pipeline, so they are
not the running-loop acceptance test.

| Replay updates | Total p50 / p99 / maximum | Work excluding final wait p50 / p99 / maximum |
| --- | ---: | ---: |
| 4 | 31.586 / 31.866 / 31.964 ms | 6.962 / 7.676 / 10.157 ms |
| 8 | 31.670 / 31.988 / 32.142 ms | 12.300 / 13.849 / 14.133 ms |
| 12 | 48.370 / 48.843 / 64.438 ms | 19.217 / 48.407 / 64.438 ms |

All 120 windows completed with final-update-only presentation and all 120 exceed
16.667 ms when the final wait is included. The twelve-update work tail is retained;
this run also records several 2–4 ms capture/remapping calls. Subtracting the final
wait does not turn this diagnostic into a realistic overlapping-loop measurement.

## Correctness and scope

[Final validation](results/native-validation.txt) passes:

- Three 1,200-update full-state runs, correcting 53 deliberately wrong predictions
  each at lengths 4, 8, and 12. Each includes four 100 ms presentation hitches,
  source retirement, and queue retirement.
- Original 1/4/8/12 battle replay, the 400-update lifecycle and rolling checkpoint
  tests, and the 120-update battle-entry transition.
- Catch-up verification over 100 four-update, 50 eight-update, and 34 twelve-update
  windows, plus battle entry with rendering suppression.
- Altered-input and omitted-stack negative controls still detect divergence.
- All **1,077 saved pre-optimization RAM hashes** match, including overlapping
  baselines from independent launches on this Mac.
- Ordinary startup without experiment flags; memory, changed-writeback, presentation
  recovery, and analysis units; syntax, patch applicability, and app signature checks.

Replay still compares raw CPU context and the entire 512 MiB RAM image. No memory
exclusion, reduced hash, missing native writeback, or skipped game completion event
is used to obtain a pass. The timing acceptance check deliberately returns failure
in [native-target-check.txt](results/native-target-check.txt); functional completion
and replay correctness do not imply the budget is met.

This is one Mac, display, build, and scripted battle fixture. RT64 history, audio
commitment, cross-machine determinism, other stages and noise-heavy dithering still
need broader validation before production rollback. The two candidate options remain
experimental; ordinary play behavior is unchanged. Source, executable, patches, and
result hashes are recorded in [native-manifest.json](results/native-manifest.json).

## Reproduce

```sh
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake --build build --target BM64Recompiled -j 8
sh tests/run_native_framebuffer.sh perf
sh tests/run_native_framebuffer.sh verify
BM64_NATIVE_SHARED_UPLOADS=1 BM64_NATIVE_LAZY_DITHER=1 \
BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 \
BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 \
  sh tests/run_battle_replay.sh
BM64_NATIVE_SHARED_UPLOADS=1 BM64_NATIVE_LAZY_DITHER=1 \
BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 \
BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 \
  sh tests/run_catchup.sh verify
sh tests/run_native_framebuffer.sh drained
python3 tests/analyze_native_framebuffer.py experiments/results/native-perf-*.txt \
  experiments/results/native-stress-*.txt --output experiments/results/native-analysis.json
```

The executable is RelWithDebInfo, `BM64_TRACE_MEMORY=OFF`, using the user's existing
local ROM. Renderer and Metal changes are preserved in the existing submodule
patches. Earlier manifests remain historical; filenames reused by the battle and
startup suites contain the most recent run.
