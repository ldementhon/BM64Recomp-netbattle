# Invalidated exploratory comparison

Concurrent controller-source changes and rebuilds contaminated these runs. The numbers below are retained for audit only and cannot support a keep/remove decision. Use native-command-preparation.md and prep-isolated-* results.

# Native command preparation

2026-10-25 · Apple M5 Pro / Metal / 60 Hz · fixed two-update input buffer.

## Method

The preparation probe attributes graphics-thread wall time and thread CPU time to
the start of fullSync, draw-data upload preparation, RSP preparation, texture/tile
processing, framebuffer construction, descriptor updates, setup commands, draw
commands, writeback commands, and the final RAM stores. GPU execution, scheduling,
CPU upload waits, and completion/wake delays retain their separate existing counters.
These are two views of overlapping work; do not sum CPU fence waits and the GPU
interval they cover. The fullSync front-end bucket precedes `native_stage`; the remaining
coarse buckets are disjoint. Uninstrumented work remains a residual, not GPU time.

`BM64_PREP_PROFILE=1` enables coarse preparation timing. `BM64_PREP_DETAIL=1`
additionally times individual tile loads and sampling/hash operations; these child
buckets overlap the tile total and add noticeable per-call measurement overhead.
Fine timing is used for attribution only, never the A/B performance comparison.
The early `prep-attribution-2` run measured only those child buckets; its zero coarse
buckets are uninstrumented coverage, not zero work. Other platforms without a
thread CPU clock report CPU counters as zero; wall counters remain available.

The focused experiment combines consecutive used texture-palette entries into one
XXH3 streaming update. It feeds exactly the same bytes in exactly the same order,
with no cache or persistent state. Every texture load, framebuffer render/readback,
RAM write, simulation update, completion event, and full-state comparison remains.
The original and trial paths use the same executable, selected by an opt-in flag.

Six blocks alternate before/after order. Each fresh process has 120 warmup cycles
and 1,080 measured cycles, with 53 four-update corrections. The first three blocks
are clean; the last three inject CPU hitches of 25/100 ms and presentation hitches
of 50 ms, four per process. Each path therefore has 318 corrections across 6,480
measured cycles. The timing includes the normal update, capture, restore, replay,
and real running-loop CPU/GPU overlap. No final drain is added to the running loop.

The keep decision requires repeatable tail improvement without added display age;
a lower average or one favorable run does not suffice. The existing reliability
target remains p99 <=13.667 ms, no clean CPU deadline misses, and recovery within
four refresh intervals with no retained extra settled display latency. Absolute
release-to-display age is measured from actual Metal display timestamps; adding the
two-update input buffer gives an input-age estimate, not physical input-to-photon
latency. Held-frame age and missed/retired presentations are included.

## Results and decision

**Remove palette batching; retain the opt-in preparation diagnostics.**

| Path | Clean execution p50 / p95 / p99 / max | Clean CPU misses | Clean display gaps >25 ms |
| --- | --- | ---: | ---: |
| Original | 12.349 / 14.344 / 14.802 / 15.229 ms | 0 | 4 |
| Palette batching | 11.672 / 14.484 / 15.532 / 18.642 ms | 1 | 10 |

Including hitches, execution p99 is 14.778 ms original versus 15.471 ms trial;
CPU deadline misses are 38 versus 39, and display gaps 20 versus 25. Only 2/6
paired per-run p99s improve. Clean median tile preparation drops from 1.567 to
1.444 ms, but its p99 rises from 1.939 to 2.017 ms; across all runs even its median
is essentially unchanged (1.485 versus 1.498 ms). The improvement seen in the first
pair is not a repeatable tail improvement. This does not establish that batching
caused the worse outliers; it does establish insufficient evidence to retain it.

Median/p99 new-image display age remains approximately 33.33/33.33 ms on both
paths. The input-buffer-inclusive estimate remains approximately 66.7 ms. Clean
held-image age reaches 50.00 ms; with hitches it reaches 150.00 ms on both paths.
The strict one-refresh presentation deadline is missed for every measured cycle
because the existing display pipeline is two refreshes old. Cadence and calibrated
pipeline misses must not be described as meeting that absolute latency deadline.

Recovery from CPU25 / CPU100 / presentation50 hitches takes 4.439–4.455 /
4.938–4.983 / 4.016–4.134 refreshes on the original path. The trial ranges are
4.440–4.497 / 4.940–4.995 / 3.353–4.151. No run retains extra settled latency.
Original: 0/12 within four refreshes; trial: 1/12. All recover within five.

The slowest trial correction (18.642 ms) includes 4.657 ms of aggregate checkpoint
capture. The slow-call trace identifies one 2.569 ms capture, 0.486 ms thread CPU,
and 2.383 ms in the VM-clear stage, with 28 involuntary switches. It localizes a
wall-time stall during that stage; it does not prove VM computation caused it.
The original 144 ms stall did not recur. This event remains in every tail metric.

[All paired results and individual runs](results/prep-analysis.json) include wake
lateness, native GPU timing, capture/restore, missed and retired updates, and held
image age. Rendering/readback byte counts and native pixel counts match within
every pair. The trial passes 274,356 whole-texture hash comparisons against pinned
upstream code under AddressSanitizer/UndefinedBehaviorSanitizer, plus the short
full-state battle replay and negative input test. Correctness alone does not justify
keeping it.

## Largest costs and next limiter

Original clean-path attribution per correction **including its normal update**:

| Work | Wall p50 / p99 | Interpretation |
| --- | --- | --- |
| Texture/tile preparation | 1.567 / 1.939 ms | CPU p50 1.563 ms; largest CPU preparation bucket |
| RAM stores | 0.481 / 0.663 ms | Required final framebuffer bytes, existing changed-byte writes |
| Draw command encoding | 0.296 / 0.407 ms | Smaller than texture preparation |
| Writeback command encoding | 0.288 / 0.396 ms | Conversion/readback commands, excludes GPU execution |
| Setup command encoding | 0.152 / 0.202 ms | Upload copies, barriers, RSP commands |
| Draw-data preparation/upload | 0.051 / 0.309 ms | CPU preparation, separate from upload worker waits |
| Native GPU execution | 2.330 / 3.881 ms | Aggregate rendering, conversion, readback |
| Submit-to-GPU scheduling | 0.872 / 1.326 ms | Separate from GPU execution |
| Texture-upload readiness wait | 0.000 / 0.768 ms | Occasional tail cost |
| CPU uploader wait | 0.000 / 0.001 ms | Not the bottleneck |

Front-end, RSP, framebuffer construction, and descriptor updates each have medians
below 0.06 ms. Native stage total is 6.840 / 8.399 ms p50/p99; its CPU fence wait
(3.636 / 4.998 ms) overlaps the GPU timeline. Individual quantiles are not additive.
Graphics work outside these fullSync measurements is another 1.833 / 2.802 ms
p50/p99 (computed per event), including display-list interpretation and tracking.
It is not all attributed to command encoding. Within the native stage, the remaining
unattributed wall time is 0.220 / 0.704 ms p50/p99. Those boundaries limit the claim
that texture preparation is the largest *measured preparation stage*, not all CPU
work in the renderer.
The fine probe identifies sampling/hashing as larger than emulated tile loads, but
its per-operation clock overhead prevents treating those figures as production cost.

The next useful measurement is GPU pass-level attribution inside the native command
buffer—drawing versus conversion/readback—while preserving the existing submissions
and required results. It can identify a worthwhile reduction; descriptor caching or
another tiny CPU special case is not supported by these measurements. Checkpoint
and scheduling variability also remain relevant to end-to-end tails.

## Final validation

A separate post-removal confirmation (coarse preparation clocks disabled) has
159 four-update corrections across three fresh processes / 3,240 measured cycles:

| Final path | p50 / p95 / p99 / max |
| --- | --- |
| Four-update correction + normal update | 13.794 / 16.866 / 18.748 / 24.093 ms |
| Normal update | 2.760 / 3.592 / 4.239 / 12.253 ms |
| Capture work per cycle | 0.096 / 0.303 / 0.651 / 2.476 ms |
| Restore per correction | 0.244 / 0.482 / 1.100 / 1.477 ms |
| Wake lateness | 0.001 / 0.515 / 0.522 / 7.969 ms |

There are 16 CPU deadline misses and 65 display gaps above 25 ms. New-image age
is 33.330 / 33.334 / 66.662 ms median/p99/max; held-image age reaches 66.670 ms.
This confirmation is slower than the earlier paired controls and is reported
separately, not pooled into the A/B result or attributed causally to removing the
trial. It reinforces that run-to-run timing variation and the 3 ms headroom target
remain unresolved. We cannot claim reliable four-update 60 Hz rollback.

Eight-update stress: 18 corrections, 23.626 / 30.456 ms median/max, 17 CPU misses,
19 display gaps. Twelve-update stress: 18 corrections, 32.920 / 38.428 ms median/max,
33 CPU misses, 19 display gaps. Both retain approximately 33.3 ms median display
age and reach 66.67 ms held-image age. Stress cases do not meet the routine budget.

[Final timing details](results/prep-final-analysis.json) preserve scheduled release,
actual wake, execution, actual presentation, and checkpoint summaries. Correctness
and fully drained suite results are being finalized.

## Reproduction

`sh tests/run_preparation_validation.sh perf` runs three fresh clean processes plus
8/12-update stress. `verify`, `broader`, and `drained` select the other suites.
`sh tests/run_tmem_hash_equivalence.sh` reconstructs the discarded trial header
from its archived patch and checks it against the pinned original algorithm.

The historical A/B runner requires `BM64_EXECUTABLE` pointing to a rebuilt trial.
Apply `patches/palette-batching-trial.patch` from the RT64 directory and
`patches/palette-batching-selector.patch` from the recomp root in a separate
checkout/build before using it. The timed executable's hash is recorded in the
manifest. An attempted archive copy overlapped the later rebuild and failed its
hash check; that invalid copy was removed. The source patches and raw results are
preserved, but no byte-identical timing executable is archived.

