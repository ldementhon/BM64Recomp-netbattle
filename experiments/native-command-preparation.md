# Native command preparation

2026-10-25 · Apple M5 Pro / Metal / 60 Hz · fixed two-update input buffer.

## Decision

**Remove palette-hash batching. Retain opt-in preparation diagnostics and the
hash-verified comparison harness.** The focused change was correct but did not
produce repeatable end-to-end tail improvement against the proper original control.
No texture cache, alternative hash path, batching flag or template variant remains
in the production renderer.

The final comparison has 636 four-update corrections: 318 per path, six fresh
processes per path, and 6,480 measured cycles per path. Every correction includes
the normal update and checkpoint work. The first three pairs are clean; the other
pairs inject four CPU25, CPU100, or presentation50 ms hitches per process. Pair
order alternates. Normal CPU/GPU overlap and bounded presentation queues remain.

| Path | Clean execution p50 / p95 / p99 / max | CPU deadline misses | Gaps >25 ms |
| --- | --- | ---: | ---: |
| Original | 14.395 / 17.021 / 18.868 / 66.022 ms | 22 | 78 |
| Palette batching | 14.238 / 16.658 / 18.182 / 20.332 ms | 11 | 73 |

Those pooled improvements are not consistent across fresh processes. P95 improves
in **2/6 pairs**, p99 in **3/6**. Including hitches, p95 worsens 16.766 → 16.916 ms,
p99 improves 18.105 → 17.835 ms, and the median is effectively unchanged,
14.221 → 14.235 ms. CPU misses are 71 → 67 and gaps 156 → 153. All three hitch
pairs have more CPU misses with batching. One per-run p99 win is dominated by the
66 ms baseline stall described below; that event is retained in all statistics.

The keep gate required repeatability in at least four pairs, at least 0.25 ms
pooled p99 improvement in both clean/all groups, no increase in pooled CPU misses
or display age, and no retained latency after hitches. The repeatability conditions
fail. A small aggregate win does not justify maintaining another renderer path.
[Full comparison, per-pair deltas, identities and gate](results/prep-specialized-analysis.json).

The rejected change combined consecutive used palette entries into one XXH3
streaming update. It fed exactly the same bytes in exactly the same order, added
no persistent state, and skipped no native rendering, readback, RAM range, update,
or completion event. Its source and frozen executable remain available for audit.

## Validity and concurrent work

The initial shared-checkout comparison was **invalidated**, not used as evidence.
The controller-support chat changed sources and rebuilt the same executable during
those runs. The controller-pak patch and four-controller buffer fix also change the
old RAM baseline and menu-fixture behavior. Independent baseline/scene checks caught
this despite same-process replay passing. No expected hash or replay check was relaxed.
`prep-perf-*`, `prep-final-*`, and the contaminated report are audit material only.

A separate source/build copy (a sibling checkout, `BM64Recomp-preparation`)
freezes the original experiment's gameplay/input/patch baseline. The imported controller
work remains intact in the main checkout. App copies are made after builds complete
and checked against the source executable hash. Each process verifies its executable
before launch and after exit, recording the fixture hash and selected mode too.

The first frozen version (`prep-isolated-*`) used a runtime selection branch inside
the palette loop. Its apparent benefit was treated as provisional: assembly review
showed that the switch remained in the original inner loop. The decisive repeat
(`prep-specialized-*`) uses separate compile-time original/batched functions, with
selection outside hashing work. The original inner loop therefore has no per-entry
experiment switch. All 12 decisive processes use the same frozen executable,
SHA256 `0d5791db8aea584eabbc8ceba2fb7c2238bd3cc205c3dbdb314488765ebb1e68`.

Timing claims apply to this original-baseline experiment, not to the concurrently
imported controller version. The shared version's old-golden incompatibility is an
explicit version boundary, not a reason to exclude RAM ranges. The final renderer
contains only the diagnostics, alongside the preserved controller changes.

## Largest costs

Original clean-path attribution per correction **including its normal update**:

| Stage | Wall p50 / p99 |
| --- | --- |
| Texture/tile preparation | 1.934 / 2.446 ms |
| RAM stores | 0.569 / 0.875 ms |
| Draw command encoding | 0.393 / 0.587 ms |
| Writeback command encoding | 0.391 / 0.604 ms |
| Setup commands | 0.206 / 0.362 ms |
| Draw-data upload preparation | 0.065 / 0.475 ms |
| Native GPU execution | 2.215 / 3.995 ms |
| Submit-to-GPU scheduling | 0.939 / 2.889 ms |
| Texture-upload readiness wait | 0.001 / 0.766 ms |
| CPU upload-worker wait | 0.001 / 0.002 ms |

Tile preparation is CPU work: thread CPU median/p99 is 1.926/2.293 ms. The trial
gives 1.895/2.193 ms, a small reduction that does not reliably carry through to full
rollback tails. Exploratory fine-grained traces suggest sampling/hashing is larger than emulated
tile loads; those higher-overhead traces are not acceptance evidence. Descriptor
updates and framebuffer construction are small in the decisive coarse measurements.

Native-stage total is 7.672/10.635 ms p50/p99; its CPU fence wait (3.512/5.502 ms)
**overlaps** GPU execution/scheduling. These views and individual quantiles are not
additive. Native residual wall time is 0.324/0.802 ms. Another 2.103/3.702 ms lies
outside instrumented fullSync, including display-list interpretation and tracking;
it is not mislabeled as GPU work or pure command encoding.

`BM64_PREP_PROFILE=1` enables disjoint coarse CPU/wall buckets. `front` covers the
start of fullSync, precedes `native_stage`, and excludes earlier display-list work.
`BM64_PREP_DETAIL=1` adds per-tile timers nested inside `tiles`. Their clock overhead
is noticeable, so they are disabled in performance comparisons. Unsupported
thread-CPU-clock platforms return zero CPU counters. The early `prep-attribution-2`
log has only child instrumentation: zero coarse buckets mean missing coverage.

## Presentation and hitch recovery

Median and p99 new-image display age remain approximately 33.3 ms. Adding the fixed
two-update input buffer gives a median input-age estimate of **66.7 ms**, not a
physical input-to-photon measurement. Clean held-image maximum is 133.34 → 50.00 ms,
including the baseline stall. With injected hitches it is 166.67 ms on both paths.
Missing and retired presentations remain in cadence/deadline/held-age accounting.
The existing two-refresh pipeline misses a strict one-refresh presentation deadline
on every measured cycle, even when calibrated cadence is regular.

| Hitch | Original recovery range | Trial recovery range |
| --- | --- | --- |
| CPU 25 ms | 4.372–4.470 refreshes | 4.411–4.461 refreshes |
| CPU 100 ms | 4.982–5.952 refreshes | 4.939–5.969 refreshes |
| Presentation 50 ms | 3.882–3.969 refreshes | 3.790–4.009 refreshes |

Recovery requires three consecutive fresh refreshes after the actual hitch ends.
Original meets the four-refresh target in 4/12 cases, trial in 3/12. Neither retains
extra settled display latency. The trial does not consistently improve recovery.

## Slow-event attribution

The 66.022 ms original-path correction is **not** a single slow checkpoint or long
GPU render. Capture totals 1.925 ms, restore 1.423 ms, graphics wait 27.194 ms, leaving
35.480 ms of simulation/runtime/scheduling residual. Within the renderer:

- GPU execution is 2.162 ms, while submit-to-GPU scheduling is 3.752 ms.
- RAM stores take 8.128 ms wall time but only 0.620 ms thread CPU.
- Tile preparation takes 3.710 ms wall time but 1.104 ms thread CPU.

This identifies substantial time off CPU/in waits; it does not establish a kernel
root cause for the remaining residual. [Correlated cycle and stage data](results/prep-specialized-outlier.json)
remain in the timing pool.

A separate trial capture takes 5.360 ms wall / 0.346 ms CPU, with 5.176 ms in VM
clear and 34 involuntary switches. Another restore takes 3.190 ms wall / 0.362 ms
CPU, with 3.157 ms in the remap stage. These localize the stalls without claiming
all stage wall time is VM computation. The original 144 ms event did not recur in
the decisive comparison or final validation. In the latter, the largest capture
is 3.184 ms and restore is 17.254 ms. That restore occurs during exhaustive
alternate-stage verification, with 17.224 ms in remapping, 15.125 ms thread CPU
and 345 involuntary switches; it is not a clean-loop performance sample.
[Final slow-checkpoint records](results/prep-specialized-final-slow-checkpoints.json)
retain all events above the existing threshold. Slow-checkpoint attribution remains enabled.

## Final validation

The final original-path executable passed the full validation suite. All **1,077
saved pre-optimization RAM hashes** match, with no excluded memory ranges. Full
RAM/register replay corrected all 53 deliberately wrong predictions in each of
six long runs: 4/8/12-update rollback with presentation hitches, two alternate
stages, and a CPU-player fixture. Battle lifecycle/entry-transition, checkpoint
ring, catch-up replay, and all five negative controls pass. Memory, writeback,
recovery and analyzer unit checks pass too. The archived hash variant matched
274,356 differential cases under ASan/UBSan; correctness did not motivate removal.

With preparation timers disabled, three additional fresh original-path runs give
159 four-update corrections over 3,240 measured cycles:

| Case | Execution p50 / p95 / p99 / max | CPU misses | Gaps >25 ms |
| --- | --- | ---: | ---: |
| Four updates | 14.117 / 16.490 / 17.543 / 18.800 ms | 8 | 70 |
| Eight updates (18 corrections) | 24.328 / 26.685 / 26.685 / 26.685 ms | 18 | 19 |
| Twelve updates (18 corrections) | 32.164 / 35.519 / 35.519 / 35.519 ms | 32 | 18 |

CPU misses and gaps count all measured cycles, including normal updates. The
four-update gap maximum is 33.335 ms; held-image age reaches 50.003 ms. Display
age median/p99 is 33.331/33.334 ms, or approximately 66.7 ms with the fixed input
buffer added. Eight/twelve-update held-image maxima are 50.002/66.670 ms. Neither
the 13.667 ms execution-headroom target nor zero CPU misses is met. These clean
confirmation runs do not replace the paired hitch-recovery results above.

The preserved fully drained diagnostic has 40 windows per rollback length.
Total p50/p99 is 31.744/46.605 ms (4), 31.735/32.008 ms (8), and 48.561/48.844 ms
(12). Work excluding the final presentation wait is 6.924/15.355, 12.231/14.221,
and 18.019/20.365 ms respectively. This latter measure reconstructs rounded profile
terms; nearest-rank p99 equals the maximum for 40 samples. It excludes the running
loop's normal-update/checkpoint context and is not a 60 Hz acceptance result.

The final shared build, with the separate controller import preserved, also passes
ordinary startup, the single-battle handoff/stay-running check, short full-state
battle replay (two corrected predictions), and a negative-input control. It is a
separate gameplay/input version, so legacy golden hashes and timing claims above
apply to the isolated baseline. The menu check confirms the handoff, not visual
scene inspection. [Validation](results/prep-specialized-final-validation.txt) and
[final timing data](results/prep-specialized-final-analysis.json) preserve failures
of the timing target alongside passing correctness results.

## Next limiting factor

GPU pass-level attribution should distinguish drawing from conversion/readback
inside the existing native command buffer, preserving submissions and required
results. CPU upload waits and descriptor construction are not useful targets now.
Display-list processing and scheduling/checkpoint stalls also need attribution.
Neither tested path meets the 13.667 ms p99 headroom target or zero clean CPU misses.
No perceptible-feel improvement or reliable four-update 60 Hz rollback is claimed.

## Reproduction

Use the frozen executable and SHA256 from `results/prep-specialized-image.json`
for the archived trial, or `results/prep-specialized-final-image.json` for final
validation, as `BM64_EXECUTABLE` and `BM64_EXPECTED_EXECUTABLE_SHA`.

- `sh tests/run_native_preparation.sh perf`: six alternating A/B pairs.
- `sh tests/run_preparation_validation.sh perf`: three fresh final-path runs and 8/12 stress.
- `sh tests/run_preparation_validation.sh verify`: full-state and negative-control suites.
- `sh tests/run_preparation_validation.sh broader`: alternate stages and a CPU player.
- `sh tests/run_preparation_validation.sh drained`: preserved fully drained diagnostic.
- `sh tests/run_tmem_hash_equivalence.sh`: reconstruct and test the archived trial header.

The frozen source/root patch and renderer/runtime patches preserve the original
baseline without reverting controller work in the shared checkout. Do not use the
legacy fixture/golden validator against an arbitrary rebuilt shared executable.

[Source, executable, fixture, patch and result manifest](results/prep-specialized-manifest.json) records the final artifacts.
