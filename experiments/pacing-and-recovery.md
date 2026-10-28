# Pacing and presentation recovery

Date: 2026-10-24. Apple M5 Pro, 64 GiB, macOS 26.4, Metal.

## Scope and acceptance targets

This extends the [running-loop experiment](running-loop-performance.md). The
input buffer is **two updates in every before/after case**. All simulation
updates, native rendering/readbacks, RAM bytes, and game completion events remain
covered. Changes remain opt-in to the cooperative checkpoint experiment.

Targets fixed before the large sample:

- Routine four-update rollback: no absolute CPU deadline misses in uninjected
  runs, and rollback-event p99 execution at most **13.667 ms**, leaving 3 ms in a 60 Hz interval.
- Hitch recovery: within **four refresh intervals after the injected stall ends**,
  complete three consecutive refreshes whose displayed-state age is at most the
  warmup median +1 ms, capped at two refresh intervals +1 ms. This requires
  recovery of freshness, not just resumption of a regular presentation cadence.
  A separate settled-age check 30–90 refreshes later must not retain extra median
  latency relative to warmup (the final hitch uses the available remaining samples).
  The fixed low-age cap is reported separately from added latency relative to
  warmup, so a baseline that was already slow cannot be mistaken for hitch damage.
- Report all tails, retired/undisplayed updates, frame gaps, and absolute latency.
  A diagnostic completing successfully does not mean its budget target passed.

## Implementation

`BM64_PACING=1` calibrates display phase and period from actual Metal drawable
presentation timestamps during the first half of warmup. Calibration is frozen
before measurement. Releases use the measured approximately 60 Hz period and
phase, an absolute Mach wait, and up to 1.5 ms of final spinning. The cooperative
game thread and critical graphics/upload/presentation workers request macOS
interactive QoS. Spinning spends up to about 9% of one core outside game execution;
it is a deliberate latency/power tradeoff, not free headroom. The original
sleep-based path remains available for the before cases.

`BM64_BOUNDED_PRESENT=1` limits committed, not-yet-displayed drawables to **two**.
Completion is released by the drawable's actual presentation callback, not merely
by GPU command completion. The existing four-entry software present queue remains
bounded. A one-drawable prototype was rejected because it reduced this Metal
path to approximately 30 Hz; its log is retained as exploratory evidence.

The present worker retires display-only events that are already past their
scheduled deadline, or become stale behind a newer submitted event while waiting
for a display slot. Actual displayed-state age also drives recovery: after a
callback shows more than two refreshes +1 ms of latency, a present that spends
more than 0.5 ms waiting for a slot and reaches the deadline's final 0.5 ms is
retired rather than perpetuating the backlog. Two fresh displayed callbacks close
recovery; out-of-order older callbacks cannot reopen it. Events with framebuffer operations are not retired by this
policy. Every event still waits for its workload, releases interpolation waiters,
notifies its present ID, releases owned queue resources, and advances the barrier.
All native framebuffer rendering, RAM writebacks, workload execution, and game
completion events run. Already committed drawables cannot be recalled; the
bound limits their committed tail after a hitch. CPU/GPU overlap remains:
there is no per-update application-side drain.

If a normal update is already late before it starts (or after its rollback replay
finishes), the existing native-only rendering path suppresses its presentation
at source. The normal simulation, checkpoint, native framebuffer processing and
completion events still run. This avoids building high-resolution presentation
work that the present worker would immediately retire. Source-retired and
queue-retired updates are counted separately.

Two small native-path reductions are independently selectable:

- `BM64_INLINE_UPLOADS=1`: the native draw-data and transform uploaders copy their
  input immediately instead of waking a separate CPU worker and then waiting for
  it. Buffer contents, resource growth, GPU copy commands, and barriers are
  unchanged. Other upload paths retain their original behavior.
- `BM64_SKIP_EMPTY_SUBMISSIONS=1`: Metal omits a separate wait command buffer only
  when its semaphore count is zero. That buffer has no resource work or completion
  event. Every real command list is still enqueued in order; all nonempty semaphore
  waits, signals, and completion fences remain.

The earlier changed-block writeback and hidden-screen-fence optimizations are on
in **both** before and after cases. No framebuffer ranges were excluded and no
replay hashes were masked.

## Timing and displayed-state age

Every update records scheduled release, actual wake, execution start, execution
finish, and actual presentation time. These are separate fields in `LOOP_FRAME`;
`DISPLAY` preserves callback order/timestamps independently. CPU injection occurs
between wake and execution start, so it is explicit in wall time and deadline
lateness while execution can also be examined separately. Presentation-worker
stalls remain visible in blocked execution and presentation timing.

Absolute CPU deadlines are release + one refresh interval. Absolute presentation
latency is the time from that update's scheduled release to its drawable being
shown. The two-update input buffer adds another approximately 33.3 ms before the
sampled input's simulation; these are not physical input-to-photon measurements.

`tests/analyze_pacing.py` reconstructs which tagged state remains on screen at
**every refresh**, including refreshes with no new drawable. Displayed-state age
is that refresh time minus the displayed update's scheduled release time. Merely
measuring the latency of newly displayed frames would miss the old image aging
during a gap. The reconstruction derives the physical display period from actual callback
intervals, separately from the nominal simulation period, and uses a 0.25 ms
alignment tolerance. This avoids mistaking accumulated 60 Hz clock drift for a
held extra frame. Self-tests cover clock drift, held frames, and sustained recovery.
Tags identify submitted game workloads, not a pixel-level oracle of RT64's
interpolation/history state.

Retired presents remain undisplayed updates and missed individual presentation
deadlines; they are counted, not removed from the result. Recovery is assessed
from the image actually held on screen. A fixed two-refresh age cap also prevents
a slow warmup queue from granting arbitrarily loose recovery targets.

## Workload and controlled hitches

`tests/run_pacing.sh perf` runs six fresh processes for each path, serially. Each
has 120 warmup and 1,080 measured updates, with a four-update correction every
20 updates: **53 corrections per run, 318 per path**. The fixture includes four
players initially, then bombs, deaths, and respawns; later cycles can have fewer
active players. Before/after use exactly the same fixture. No neutral or dead
interval is silently substituted for the earlier four-player workload.

Runs 1–3 have no injected stalls. Run 4 injects 25 ms CPU stalls; run 5 injects
100 ms CPU stalls; run 6 injects 50 ms presentation-worker stalls. Each injected
run has four stalls at known update IDs, coinciding with scheduled corrections.
Eight- and twelve-update runs are separate stress cases. The original fully
drained diagnostic remains intact and has additional named candidate runs.

Hash verification is separate from performance. It first records a fully
rendered reference, then corrects genuinely wrong predicted input in the same
process. It checks every corrected state against all 512 MiB of RAM and raw
register/context state. The verification cases also force 100 ms presentation
stalls, exercising retirement and completion paths. In verification only,
presentation deadlines are relative to each execution start so accumulated
full-RAM hashing overhead does not retire every later normal frame. Hashes and
state comparisons are unchanged. A scheduled presentation-worker hitch attaches
to the next visible event if its original event was source-retired; the log
records both scheduled and actual IDs. Their hash overhead makes their timings
unsuitable for performance conclusions.

## Checkpoint tail attribution

Calls longer than 2 ms emit `CHECKPOINT_SLOW` records with operation, game frame,
slot, copied native bytes, and:

- RAM clear/unmap and remap wall times, separately;
- ownership, RAM, native-stack, metadata, and runtime-state wall/CPU stages;
- total calling-thread CPU time;
- process-wide minor/major fault and voluntary/involuntary switch deltas.

Thread CPU versus wall time distinguishes CPU consumption from time off-core or
blocked, but does not prove a specific kernel cause. Fault/switch counters cover
the whole process and may include concurrent renderer activity. Verification
hashes touch all RAM and can themselves make remap costs higher than timed play.
`BM64_CHECKPOINT_STALL=1` supplies a separately labelled 25 ms diagnostic injection
at frame 2100 to validate attribution; it is excluded from benchmark runs.

## Final measured results

The final executable completed **636 four-update rollback events** across twelve
fresh processes: 318 before and 318 after. Each path includes 6,480 measured
cycles; the first three runs per path supply 3,240 uninjected cycles and 159
rollback events. Results below use the final policy, not the earlier prototypes.
Raw timestamps and per-run analyses are in [pacing-analysis.json](results/pacing-analysis.json);
pooled execution/display summaries are in [pacing-summary.json](results/pacing-summary.json).
Quantiles in the analyzer use the nearest-rank definition.

### Uninjected four-update comparison

Each rollback execution includes restore, four replay updates with their
checkpoints, the new normal update, and its checkpoint. It retains normal
CPU/GPU overlap. All figures are milliseconds unless stated otherwise.

| Metric | Before | After |
| --- | ---: | ---: |
| Rollback + normal execution p50 / p95 | 13.760 / 16.325 | 12.520 / 14.710 |
| Rollback + normal execution p99 / maximum | 18.350 / 19.066 | 15.463 / 15.548 |
| Normal-only execution p99 / maximum | 4.255 / 25.568 | 3.953 / 9.907 |
| Wake lateness p99 / maximum, all cycles | 6.406 / 9.047 | 0.001 / 0.011 |
| Absolute CPU deadline misses | 47 / 3,240 | **0 / 3,240** |
| New-image release-to-display latency p50 / p99 | 61.560 / 77.504 | **33.346 / 33.361** |
| Maximum new-image latency | 77.529 | 50.026 |
| Display gaps over 25 ms | 6 | **14** |
| Maximum display gap | 33.335 | 33.335 |
| Maximum held-state age, including missing refreshes | 77.527 | 50.048 |
| Undisplayed updates / retired at source / retired in queue | 0 / 0 / 0 | 14 / 0 / 14 |

CPU deadlines now pass this sample, but **reliable smooth 60 Hz with 3 ms of
rollback headroom is not established**. Rollback p99 leaves only **1.204 ms** of
a 16.667 ms interval. An all-cycle p99 of 13.374 ms would look better because most
cycles have no rollback; it must not substitute for the rollback-event tail.
The revised path trades lingering old images for occasional skipped display
updates: cadence regressed from 6 to 14 gaps despite lower display latency.
All simulation updates still executed.

The individual clean runs had 0/0/0 CPU misses and 2/2/10 display gaps. The worst
rollback (15.548 ms) finished before its CPU deadline, but its presentation was
retired later. CPU submission finishing inside the interval is therefore not a
presentation guarantee. All 3,240 per-update one-refresh display deadlines were
missed on each path, even with a 1 ms tolerance: the revised stable pipeline is
about **two refreshes**, not one.

The fixed two-update input buffer adds approximately 33.333 ms before simulation.
Thus the clean median scheduled-input-to-display estimate is approximately
**94.9 ms before and 66.7 ms after**. These estimates include the buffer; the
33.346 ms figure above does not. Hardware input polling, physical scanout position,
and pixel response were not measured. No input delay was removed to improve the
comparison.

### Controlled hitch recovery

Each row represents four injected stalls, coinciding with corrections. Recovery
requires three consecutive refreshes meeting the freshness limit. These ranges
start at the *actual* end of the injected stall, not its scheduled end.

| Revised path hitch | Recovery completion, refresh intervals | CPU deadline misses in that run | Maximum display gap | Maximum held-state age | Later extra median age vs warmup |
| --- | ---: | ---: | ---: | ---: | ---: |
| CPU 25 ms | 4.439–4.493 | 9 | 50.003 ms | 66.670 ms | <0.001 ms |
| CPU 100 ms | 4.939–4.942 | 28 | 133.340 ms | 150.052 ms | 0.009–0.045 ms |
| Presentation worker 50 ms | 4.020–4.106 | 4 | 83.337 ms | 100.049 ms | 0.009–0.045 ms |

**0/12 meet the predeclared four-refresh target; all 12 recover within five
refresh intervals and none retain an extra display interval in the settled
window.** Five is the measured upper bound for these injected cases, not a
replacement acceptance target. On the original path none reached the fixed
low-age recovery cap within the recorded remainder. Their settled median age was
33.5–50.9 ms above warmup, with actual settled ages around 64–82 ms. This is a
comparison of measured windows, not proof that a single hitch caused every later
millisecond of baseline queue growth.

Across all six runs, including injected hitches and catch-up backlog, CPU deadline
misses fell from **150 to 41 out of 6,480**; gaps over 25 ms increased from **18 to
46**. Revised runs retired 36 updates at source and 50 in the present queue;
all 86 remain in the undisplayed/deadline-miss count. Maximum new-image latency
was 50.026 ms, but the image held during a stall reached **150.052 ms** of age.
This distinction prevents the queue policy from appearing artificially fast by
counting only frames it ultimately shows. The drawable bound stayed at two in
every revised run.

### Stress cases and fully drained diagnostic

The running-loop stress cases each contain 18 corrections across 1,080 measured
cycles, with the same two-update input buffer. They are separate from the routine
four-update sample.

| Rollback length | Execution p50 / maximum | CPU deadline misses | Display gaps >25 ms | Maximum held age |
| --- | ---: | ---: | ---: | ---: |
| 8 | 21.460 / 24.019 ms | 18 | 24 | 66.670 ms |
| 12 | 30.397 / 32.574 ms | 25 | 22 | 66.670 ms |

Both exceed a single refresh interval. Stable new-image latency remains about
33.3 ms, while held-state age exposes the gaps caused by stress recovery.

The original fully drained harness remains available; the candidate was also
run for 40 windows at each length. It restores the older four-player battle
fixture, includes the final presentation fence, and does not include a new normal
update. Its figures are not interchangeable with running-loop execution.

| Drained replay length | Total p50 / maximum | Final wait p50 | Work excluding final wait p50 / maximum |
| --- | ---: | ---: | ---: |
| 4 | 31.715 / 46.613 ms | 24.799 ms | 6.914 / 14.875 ms |
| 8 | 31.681 / 32.182 ms | 18.889 ms | 12.816 / 14.442 ms |
| 12 | 48.568 / 48.991 ms | 30.162 ms | 18.342 / 20.667 ms |

All 120 drained windows exceed 16.667 ms. The bounded display path makes the
serialized presentation tail visible; the work-only column is attribution, not a
claim that those windows meet the end-to-end budget. The existing unbounded,
fully drained diagnostic and its earlier outlier logs are retained.

### Remaining limiting work and checkpoint tails

For clean four-update events, the revised median graphics-worker fence is
**8.913 ms**, native framebuffer stage **6.903 ms**, native upload/GPU wait
**3.520 ms**, and RAM copy **0.498 ms**. These are nested/overlapping counters,
not additive costs. Before values were 9.711, 7.377, 3.581, and 0.537 ms.
Required native processing remains the largest recurring serial cost. Upload
handoff and empty Metal wait-buffer removal are the smallest concrete safe
reductions implemented here; no data-bearing render, readback, fence, or signal
was removed. The combined before/after test does not isolate the benefit of each
flag from pacing and QoS.

The next bottleneck is finishing native work early enough to leave time for the
asynchronous screen workload and drawable submission before the refresh boundary.
The graphics fence and native wait still dominate; occasional checkpoint mapping
tails can consume the remaining margin. Before removing another synchronization,
the next useful measurement is to split the native wait into CPU uploader wait,
GPU render/readback execution, and command scheduling delay. A GPU timestamp trace
would establish which part can safely shrink. The earlier bounded framebuffer
consumer trace is not sufficient evidence to skip writebacks in other battles or
transitions. This revision makes no such omission.

Clean-run checkpoint capture totals per cycle have p99 **0.547 ms**, maximum
**1.258 ms**; rollback restores have p99 **2.139 ms**, maximum **4.018 ms**.
The detailed trace locates two real slow restores at frames 2802 and 2922 in
RAM remapping: **2.086/3.925 ms wall**, versus **0.262/0.285 ms thread CPU**.
A later capture at frame 2745 spent **3.830 ms clearing the prior mapping**,
within 4.154 ms total. These records include fault/scheduler deltas. They identify
the stage and off-core/blocking component, not a proven operating-system cause.

The 144 ms outlier did not recur in the final performance/drained sample. The
separate canary produced 27.231 ms total capture, correctly assigning 27.052 ms to
the explicit injected stall and only 0.139 ms to RAM work. This validates the
attribution path without treating a synthetic stall as a performance observation.

## Correctness and limits

- Full 512 MiB RAM and raw context checks passed all three 1,200-update delayed
  input runs: 53 genuinely wrong predictions corrected per run at lengths 4/8/12.
  Each exercised 53 source-retired normal presentations and four 100 ms
  presentation-worker stalls, plus queue retirement.
- The original battle, transition, rolling-history, and final-frame-only catch-up
  suites pass. Altered-input and omitted-stack negative controls still detect
  divergence. All **1,077** saved pre-writeback-optimization baseline RAM hashes
  match the final build.
- Checkpoint-memory and recovery-state unit tests, held-age analyzer tests, and
  ordinary threaded startup pass. Native submodule patches reverse-check against
  the tested tree; the manifest records source, executable, and result hashes.
- This remains one Mac/display, one stage/fixture, short fresh sessions, and an
  experimental cooperative runtime. Sustained thermal/load behavior, variable
  refresh displays, multi-monitor changes, minimized windows, other battles,
  cross-machine determinism, committed audio, and pixel-level rollback artifacts
  are not established. The two-second missing-presentation-callback timeout fails
  this diagnostic instead of silently relaxing its drawable bound.

The result supports continuing the rollback experiment, but does **not** yet
justify promising hitch-free four-update rollback at 60 Hz. The concrete gains
are substantially better wake timing, lower display latency, and removal of
persistent post-hitch queue delay in this sample; the outstanding gates are
rollback tail headroom, occasional display gaps, and the four-refresh recovery
criterion.

## Reproduce

The final executable is a non-instrumented `BM64_TRACE_MEMORY=OFF` build. The
pinned runtime, RT64, and Plume edits are preserved in `experiments/patches` and
applied by the existing build recipe.

```sh
TOOLCHAINS=com.apple.dt.toolchain.Metal.32023.883 \
  cmake --build build --target BM64Recompiled -j 8
sh tests/run_pacing.sh verify
BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 \
BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 \
  sh tests/run_battle_replay.sh
BM64_PACING=1 BM64_BOUNDED_PRESENT=1 BM64_INLINE_UPLOADS=1 \
BM64_SKIP_EMPTY_SUBMISSIONS=1 BM64_INCREMENTAL_WRITEBACK=1 BM64_SKIP_HIDDEN_FENCE=1 \
  sh tests/run_catchup.sh verify
sh tests/run_pacing.sh perf
sh tests/run_pacing.sh drained
python3 tests/analyze_pacing.py --self-test
python3 tests/analyze_pacing.py experiments/results/pacing-perf-*.txt \
  experiments/results/pacing-stress-*.txt --output experiments/results/pacing-analysis.json
```

All benchmark processes use fresh settings/saves and the user's existing local
ROM. Timing cases must not run concurrently with another game run or build.
