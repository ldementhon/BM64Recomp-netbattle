# Optimization decisions and frame delivery

2026-10-24 · Apple M5 Pro / Metal / 60 Hz · fixed two-update input buffer.

## Keep or remove

**Remove both native shared-buffer uploads and lazy dither initialization. Keep
CPU/GPU/scheduling attribution and the full-state test harness.** This is a
complexity-versus-evidence decision, not a claim that the optimizations never help.

Sixteen serial fresh processes tested four configurations in a balanced order.
Each configuration contributes 212 four-update corrections and 4,320 measured
cycles. Every correction includes the new normal update and checkpoint work.
The tested executable matches the previous native-manifest executable hash.

| Configuration | Execution median / p99 | CPU deadline misses | Display gaps >25 ms |
| --- | ---: | ---: | ---: |
| Original | 13.429 / 17.615 ms | 9 | 73 |
| Dither only | 13.400 / 17.217 ms | 7 | 75 |
| Shared buffers only | 13.032 / 17.896 ms | 8 | 54 |
| Both | 12.997 / 17.948 ms | 9 | 84 |

Dither's pooled median benefit is only 0.029 ms; it improves the per-run median in
2/4 blocks. Shared buffers improve the median in 3/4 blocks and pooled gaps, but
execution p99 worsens in 3/4 blocks, with essentially unchanged deadline misses.
Both combined do not improve tail reliability. Native-stage median improvements
are 0.125 ms for dither and 0.156 ms for shared buffers. These results do not justify
maintaining an additional buffer storage mode and duplicated shader pipelines for
our routine rollback target. No configuration meets the 13.667 ms p99 headroom gate.

The shared-buffer capability, storage-mode transitions, shader variant, selectors,
and their runtime options have been removed. Earlier inline CPU uploads and other
previously tested pacing/writeback changes remain. GPU timing diagnostics remain.

[Per-run results](results/ablation-analysis.json) retain paired deltas, readback
volume, execution tails, and displayed-state age. [Archive manifest](results/ablation-manifest.json)
identifies the pre-removal executable and patches. The executable is retained under
`build/experiments/native-ablation/` for local reproduction. The historical ablation
runner requires `BM64_EXECUTABLE` pointing at that archived executable; running its
old option names against a current executable would not reproduce the comparison.

## Presentation investigation

The original-phase trace puts a normal frame at the presentation call approximately
5 ms after release, followed by about 28 ms until the reported actual display time.
Upload workers and the final GPU fence do not explain that wait. Display callback
arrival is measured separately from its reported display timestamp.

Short phase probes retain 120 warmup updates, 360 measured cycles, 17 corrections,
and the same input buffer. These are exploratory, not reliability claims. Moving
release earlier by 4/8/12 ms produces approximately 37.3/41.3/45.3 ms median display
latency versus 33.3 ms at the original phase. Later releases by 4/8/12 ms produce
approximately 46.0/42.0/38.0 ms, despite fewer cadence gaps. Display callbacks arrive
about 1 ms after the reported display time. Smooth cadence alone can therefore
conceal a retained extra refresh of state age. None of these phase changes is
accepted as a latency improvement.

A follow-up probes whether recovery can reject presentation-only work based on
its earliest queued display slot, in addition to submission lateness. It preserves
simulation updates, native writes, framebuffer operations, and completion events.

## Checkpoint mapping investigation

A separate experiment replaces an owned snapshot mapping at the same address
instead of deallocating it and asking for another address. It still covers all
512 MiB, uses copy-on-write, and preserves historical images. The memory test covers
ring eviction, future sparse writes, guard mappings, storage-mode transitions, and
size changes. Both allocation paths pass it; the short full-state correction test
also passes. The paired timing comparison determines whether this deserves retention.


The completed mapping comparison has six fresh runs per path, 318 corrections
per path, and twelve controlled hitches per path. **Remove mapping reuse too.**

| Mapping metric | Original | Reuse |
| --- | ---: | ---: |
| Clean execution p50 / p99 | 13.855 / 18.699 ms | 13.458 / 16.798 ms |
| Clean capture-per-cycle p99 | 0.669 ms | 0.648 ms |
| Clean CPU deadline misses | 5 | 3 |
| All-run CPU deadline misses | 50 | 49 |
| All-run display gaps >25 ms | 124 | 136 |
| All-run execution p99 | 17.121 ms | 17.544 ms |

The clean execution result is better, but the measured capture reduction is tiny,
and restore itself was unchanged by this experiment. One original-path restore
outlier reached 5.329 ms. In the reuse path, a 3.930 ms capture spent 3.881 ms inside
remapping; the original path's largest capture spent 4.779 ms clearing an old
mapping. The costly VM operation has moved rather than disappeared. The wider
comparison does not establish a reliable deadline, cadence, or recovery benefit.
No tail samples were discarded. [Full paired mapping results](results/delivery-perf-analysis.json)
retain all hitches and per-run variation.


## Queue-age probe decision

The additional recovery heuristic estimates the next available display slot from
actual displayed callbacks and the committed-drawable count. It correctly removed
the extra refresh retained by the later-phase probes, but does not establish a
better routine policy:

| Short probe | Median / p99 new-image latency | Gaps >25 ms / 360 cycles |
| --- | ---: | ---: |
| Original phase, age heuristic | 33.332 / 50.000 ms | 4 |
| Release 4 ms later, age heuristic | 29.333 / 46.001 ms | 15 |
| Release 8 ms later, age heuristic | 25.333 / 42.001 ms | 17 |

The lower median accompanies more skipped presentations, almost one per correction
in the 8 ms case. Held-state p99 is 46.002/42.002 ms in the shifted cases. A 100 ms
CPU hitch still takes 5.899 refresh intervals to satisfy the sustained-recovery
criterion at the original phase; the 100 ms presentation-worker probe takes 4.063.
Neither retains extra settled latency, but neither meets the four-refresh target.

**Remove the queue-age heuristic and retain the original release phase.** These
short exploratory samples cannot justify exchanging steady cadence for lower
median image age, and they did not solve the routine hitch case. Presentation
stage and callback-arrival tracing remain opt-in diagnostics. The release-phase
parameter remains only as a diagnostic for reproducing this experiment; no new
phase is enabled by default or by the final validation runner.

After a 100 ms CPU hitch that coincides with a correction, preserving every update
requires the correction plus overdue normal updates. This consumes part of the
recovery interval before presentation can show fresh state. The measured display
pipeline then adds delay, and the recovery criterion requires three consecutive
fresh refreshes. These are distinct costs; better average native work alone does
not guarantee four-refresh recovery. The trace locates the post-submission delay,
but does not separate compositor buffering from all internal driver scheduling.

## Reproduction and archives

Current-path validation uses `tests/run_simplified_validation.sh` with `perf`,
`verify`, `broader`, or `drained`. Input delay stays at two updates. Historical
experiment runners require `BM64_EXECUTABLE` explicitly to prevent silently
benchmarking removed switches against a current build:

- Native four-way comparison: `build/experiments/native-ablation/BM64Recompiled.app/Contents/MacOS/BM64Recompiled`.
- Mapping and initial phase probes: `build/experiments/delivery-mapping/BM64Recompiled.app/Contents/MacOS/BM64Recompiled`.
- Queue-age probes: `build/experiments/queue-age/BM64Recompiled.app/Contents/MacOS/BM64Recompiled`.

Each archive is local build output, not a distributable ROM or a committed binary.
The experimental implementation patches and raw results are retained separately.


## Final simplified-path timing

These are fresh runs of the final simplified path, not a paired claim that removing
code improved performance. Natural batch variation is substantial. Six fresh
processes contribute 318 four-update corrections and 6,480 measured cycles;
three are clean and three inject four hitches each. Input buffer, MSAA, release
phase, and the two-drawable limit remain unchanged.

| Final clean metric (159 corrections / 3,240 cycles) | Result |
| --- | ---: |
| Complete execution p50 / p95 / p99 / max | 12.801 / 16.246 / 18.437 / 20.133 ms |
| CPU deadline misses | 10 |
| Display gaps >25 ms | 46 |
| New-image release-to-display p50 / p99 / max | 33.326 / 33.334 / 50.000 ms |
| Maximum held-state age, including missed refreshes | 66.670 ms |

Across all runs including hitches: 54 CPU deadline misses, 99 display gaps over
25 ms, and maximum held-state age 166.675 ms. Two of twelve hitches meet the
four-refresh recovery target; ten fail it. All recover within six refreshes and
none retains an extra refresh in its settling window. Recovery ranges are
4.349–4.457 refreshes for 25 ms CPU stalls, 4.879–5.951 for 100 ms CPU stalls, and
3.913–4.081 for 50 ms presentation stalls.

Eight/twelve-update stress cases each contain 18 corrections. Execution median /
maximum is 22.511 / 27.191 ms and 28.819 / 36.167 ms respectively. They incur 18/23
CPU deadline misses and 22/19 display gaps. Neither fits one refresh interval.

The fixed input buffer adds approximately 33.333 ms to the release-to-display
measurement: the scheduled-input-to-display median estimate remains about
66.7 ms. This is frame-tag timing, not a physical button-to-photon measurement or
a claim of a human-perceived improvement. Scene inspection verifies that actual
battles render; human side-by-side feel testing remains necessary before promoting
a future optimization.

[Final timing analysis](results/simplified-analysis.json) retains all tail samples,
missed/retired presentations, hitches, and stress cases. The
[target check](results/simplified-target-check.txt) reports failure, separately from
replay correctness. Full 512 MiB RAM and raw CPU context checks remain unchanged.


## Broader correctness and remaining limits

The fully drained diagnostic remains intact, with forty windows at each length:

| Updates | Total p50 / p99 / maximum | Work excluding final wait p50 / p99 / maximum |
| --- | ---: | ---: |
| 4 | 31.502 / 32.158 / 46.160 ms | 7.066 / 9.012 / 14.763 ms |
| 8 | 31.660 / 48.319 / 48.635 ms | 12.422 / 18.795 / 19.730 ms |
| 12 | 48.256 / 48.909 / 64.609 ms | 19.781 / 28.837 / 30.892 ms |

All 120 windows complete; all exceed one refresh when the final presentation wait
is included. The subtracted column is diagnostic only and does not replace the
overlapping running-loop test.

The 144 ms checkpoint outlier did not recur. The largest final logged checkpoint
call is a 12.193 ms restore during hash-heavy four-update verification, including
12.171 ms in RAM remapping and 11.908 ms total thread CPU. This is outside the timing
sample and is retained in its verification log. Wall and thread-CPU measurements
distinguish it from mere sleep/wake delay without proving a specific kernel cause.

The final original-stage 4/8/12-update runs each complete 1,200 updates and correct
53 deliberately wrong predictions, with four 100 ms presentation hitches per run.
All original battle, lifecycle, entry-transition, rolling checkpoint, catch-up,
altered-input, and omitted-stack controls pass. All 1,077 historical baseline RAM
hashes match. Native writeback ranges and full-state comparisons are unchanged.

Coverage now also includes two additional stages (overlays 87 and 88, selected
through the normal menus) and a Rock Garden fixture with a CPU-controlled first
player. Each added fixture completes 1,200 updates and 53 wrong-prediction
corrections at length four. Initial 12-update windows pass three restores each;
scene screenshots were inspected to confirm distinct actual battles. The fixture
changes are small input additions to the common trace, not injected game state.
These are correctness runs, not performance certification for the added stages.

The first long added-stage attempt was correctly rejected because checkpoint
2050 put all prediction windows between input changes. Moving the checkpoint to
2045 aligned the existing trace with actual changes; the assertion requiring wrong
predictions was retained. Harness setup errors and that rejected attempt are
recorded in [the runner note](results/simplified-runner-note.txt).

One Mac/build/display is still the limit of this evidence. It does not establish
cross-machine determinism, rollback of RT64 temporal history, audio commitment,
all battle modes, round completion/restart, or human-perceived responsiveness.
The next substantial performance work should target native command preparation
and the checkpoint VM stalls, with per-stage evidence before choosing a redesign.
The current tests do not justify another shader/upload flag or a phase change.

[Validation summary](results/simplified-validation.txt) separates functional
checks from the failed timing gate. [Final manifest](results/decision-manifest.json)
records the tested binary, source, archived experiments, and result hashes.
