# Final-frame catch-up experiment

2026-10-24 · Apple M5 Pro, 64 GiB · macOS 26.4 · ARM64.

The experiment can restore and replay 4, 8, or 12 updates while submitting only the
final update to the onscreen renderer. Full 512 MiB RAM and register checks remain
in place. This is **presentation suppression**, not elimination of every GPU operation:
RT64's native-resolution framebuffer writes are part of the state being checked.
**Correctness passes; the fully fenced 16.7 ms budget does not.**

## Implementation and correctness

A first attempt skipped display-list processing entirely. It diverged on the first
replayed update: CPU register hashes agreed, but 15 RAM pages differed
([failed diagnostic](results/suppressed-first.txt)). RT64's `State::fullSync` renders
native framebuffer data and copies it back to RDRAM; omitting that work changes RAM.
No RAM ranges were excluded to make the test pass.

The implemented path keeps display-list parsing, native framebuffer rendering and
writeback, SP/DP completion events, and display-list extension notifications. For
intermediate updates it skips RT64's onscreen workload submission, early presentation,
and VI screen-update calls. The final update submits normally, with interpolation
disabled for that workload so it cannot synthesize extra frames through old history.
The ordinary rendering path and `--single-battle` startup shortcut retain their behavior.

Each catch-up window starts from an ordinary, fully rendered baseline. It restores
the starting checkpoint, runs the same inputs, and refreshes the rolling checkpoint
history on every replayed update. It then resumes ordinary simulation from the
corrected state before the next window. Graphics counters assert that the replay
submitted the baseline final update's workload count and suppressed all earlier ones.
An RT64-side assertion also rejects intermediate display lists that advance the
onscreen workload or presentation IDs.

Final completion is explicitly fenced **after the last replayed display list**.
The fence waits for RT64's workload and present IDs and drains both queues, including
the present API call. It does not measure physical display scanout or input-to-photon
latency. Merely timing `update_screen` is insufficient because it can precede the
final display list in the game's scheduling order.

Three device message registrations (AI, SI, DP) previously used a shared saved value;
they now have independent storage for every checkpoint slot.

Validation includes:

- 100 four-update windows, 50 eight-update windows, and 34 twelve-update windows,
  beginning at update 1945: **1,208 replayed updates** compared with full RAM and
  Hudson/N64 register hashes. Movement, bombs and deaths are covered by the existing fixture.
- Ten twelve-update windows across stage selection into battle: another 120 comparisons.
- Existing 1/4/8/12-update, 400-update lifecycle, 120-update entry, and rolling-history
  tests, with their original full-render behavior and negative controls.
- Changed-input and omitted-stack negative controls with suppression enabled.
- Independent baseline comparisons against the full-render lifecycle recording.
- Normal threaded startup, plus a visual check after catch-up at battle update 2100.
  The screenshot is `build/experiments/catchup-visual.png`; it is a scene/artifact check,
  not a pixel-perfect rendering comparison.

An exploratory omitted-stack test over only 12 entry updates resumed an invalid
native continuation and crashed ([log](results/catchup-negative-stacks-12-crash.txt));
that is not counted as a passing test. The suite uses the established 120-update
entry negative control, which reports RAM divergence instead.

## Timing methodology

The timing runs disable full-RAM/register hashing. All correctness runs keep it
and their timing figures must not be used as performance evidence. Each timing case
uses 100 consecutive windows, fresh settings, the same battle input fixture and
start boundary 1945. The cases run serially, including the all-updates-rendered controls.

The timed interval starts immediately before restore and ends after final presentation
queue completion. It includes restore, simulation, required native framebuffer
writebacks, COW faults, renewed checkpoints after every replayed update, and final
onscreen work. The separate final-wait column measures the explicit queue-drain wait;
subtracting it does **not** make the remaining time a complete frame budget.
Normal baseline CPU simulation and its verification/printing are outside the interval.
Already queued baseline graphics work can finish inside the measured interval.
The windows cover 400, 800 and 1,200 successive baseline updates respectively, so
later samples include quieter gameplay and are not identical scene distributions.

| Replayed updates | Work excluding final wait, p99 | Fully fenced total, p50 / p99 | Render-every-update control, total p99 |
| --- | ---: | ---: | ---: |
| 4 | 8.620 ms | 31.676 / 33.302 ms | 66.113 ms |
| 8 | 18.168 ms | 31.782 / 48.138 ms | 99.306 ms |
| 12 | 21.842 ms | 48.433 / 49.258 ms | 133.084 ms |

All 300 suppressed timing windows exceeded 16.667 ms when fully fenced. The
four-update *work* samples fit (maximum 15.035 ms); eight- and twelve-update work
had maxima of 20.741 and 23.255 ms. Suppression cuts fully fenced p99 by roughly
2–2.7× versus rendering every replayed update, but does not yet establish a 60 Hz
rollback loop.

Final queue-drain waits had medians of 24.371, 19.075 and 29.668 ms. The baseline's
previously submitted rendering can still be in flight when restore begins, so this
wait includes outstanding work and presentation pacing, not just a newly rendered
frame's GPU execution. Production rendering is normally pipelined; a forced drain
is a conservative end-to-end check, not a prediction of achievable steady-state FPS.
The work-only figures are useful for planning but omit completion of the final image.

Evidence: suppressed [4](results/catchup-perf-4.txt), [8](results/catchup-perf-8.txt),
[12](results/catchup-perf-12.txt); controls [4](results/catchup-control-4.txt),
[8](results/catchup-control-8.txt), [12](results/catchup-control-12.txt).
Exact source, binary and result hashes are in
[catchup-manifest.json](results/catchup-manifest.json).

## Reproduce

From the checkout root, build with the recipe in [README.md](README.md). It now
applies both the runtime checkpoint patch and
[the RT64 suppression patch](patches/rt64-catchup.patch) idempotently.

```sh
sh tests/run_battle_replay.sh
sh tests/run_catchup.sh verify
# Run separately, with no other game tests competing for CPU/GPU time:
sh tests/run_catchup.sh perf
python3 tests/run_startup_probe.py
```

The runners use isolated settings/saves and a 180-second deadline per child.
`BM64_CATCHUP_FRAMES=4|8|12`, `BM64_CATCHUP_SAMPLES`, and `BM64_CATCHUP_VERIFY=1`
control individual runs through `tests/run_checkpoint_probe.py`.
`BM64_CATCHUP_RENDER_ALL=1` is the timing reference. `BM64_SUPPRESS_REPLAY=1` enables
final-update-only presentation for the older single-anchor replay harness.

To inspect the game, use `BM64_HOLD_FRAME=2100`, `BM64_CHECKPOINT_FRAME=1980`,
`BM64_CATCHUP_FRAMES=12`, and `BM64_CATCHUP_SAMPLES=10` with verification enabled.
That deliberately pauses the run and must not be used for timing.

## Limits and next step

GPU history is not snapshotted, and this experiment does not establish pixel-perfect
rollback or cross-machine determinism. Audio remains suppressed in checkpoint mode.
It is a same-process test on this Mac, with the existing bounded native-thread history.

Removing all intermediate native rendering would require proving which framebuffer
writes gameplay reads and implementing an equivalent path for those effects. The
failed whole-display-list skip is evidence that dropping the writes silently is not
valid under the current full-RAM equality contract. Any further speed work should
profile these writebacks separately from swapchain/presentation waits.

## Running-loop follow-up

The [next experiment](running-loop-performance.md) preserves this fully drained
benchmark and adds a paced loop with actual Metal display timestamps, delayed-input
corrections, and framebuffer-consumer tracing. Its findings supersede inference
from replay submission time alone; four-update rollback remains marginal.
