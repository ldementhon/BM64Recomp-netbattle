# Fresh-session prompt: finish Mac/Windows online rollback Battle

> **Update 2026-10-27:** the six Windows multiplayer review fixes are implemented.
> See the current status and newest entry in `online-lobby-plan.md`; everything
> below is historical context. The relay now uses protocol 3 and the private
> game channel uses version 5; desync checks warn instead of ending the match. Windows regressions, a cross-platform startup
> comparison, and a 1,000-update Mac/Windows LAN match now pass. The latest plan
> entry records the tested fingerprints and limits; earlier fingerprints are
> obsolete. Mixed-platform delay, physical controllers, and audio still need
> validation on this pair.

> **Update 2026-10-26:** online processes now render display-only by
> default, which keeps GPU output out of game RAM. `BM64_RENDER_TO_RAM=1` restores
> the old mode for diagnostics; network matches refuse it. Windows RX 9070 XT
> replays pass, Mac/Windows cross-build histories match, and the RDNA 4 note
> below no longer blocks determinism. The per-frame presentation wait now
> applies only to render-to-RAM, which roughly halves replay cost on the Mac.
> `patches/rt64-display-textures.patch` fixes a texture-eviction race in this
> mode. Windows runs the current build.
> Later the same day: P1 can choose the stage online, and `bm64-lobby` accepts up
> to four fingerprints so a validated Mac/Windows pair shares a room (steps in
> `server/README.md`). The next step is a live Mac/Windows match.

> **Update 2026-10-26 (later):** the Windows replay failure is attributed to the
> PC's RX 9070 XT (RDNA 4), not to replay state. It is a rare one-frame depth
> glitch that also occurs outside replay. Re-executing an unchanged native command
> list reproduces it; the same image on the PC's integrated AMD GPU does not. A
> dummy bottom-of-pipe timestamp after every draw removes it, matching Mesa's
> documented GFX12 Hi-Z/Hi-S workaround. No mitigation is retained yet. Read the
> newest implementation-record entry in `online-lobby-plan.md` and use its
> re-execution metric instead of replay pass/fail. Windows is back on normal
> sources. Online audio now uses a virtual AI clock with first-simulation output
> (commit `f8fee66`), and online rumble is silent by construction.

Continue the work in this checkout toward a
playable four-player rollback Battle on Apple Silicon Mac and Windows x64.
Read `AGENTS.md`, `experiments/online-lobby-plan.md` (especially the current
remaining-work list and newest implementation entries), and
`experiments/STATE_REPLAY.md`. This handoff is dated 2026-10-25. Verify the current
checkout before acting; later edits may supersede it.

Work autonomously through the acceptance criteria, make focused logical commits,
and record evidence and remaining blockers in the plan. Do not stop at another
proposal. Do not start by rewriting the working lobby or relaxing determinism.

## Scope and working agreements

- Branch at handoff: `netbattle-development` (now `netbattle`). Never put `codex` in a branch name.
- Battle only. Fixed player colors; P1 advances shared game menus. The lobby locks
  human/CPU assignments. Preserve four-player controller support, calibration,
  personal input preferences, and normal offline play.
- **New user decision: online rumble may be disabled for the first playable
  version.** It is not a blocker. Silence it reliably, including transitions, and
  preserve offline behavior. Real audio and replay-safe audio output still matter.
- Preserve concurrent work. There are unrelated untracked experiment reports,
  tests, and performance patches, plus intentional dirty dependency checkouts.
  Do not reset, clean, stash everything, or stage everything. Commit dependency
  fixes as isolated patches in `experiments/patches/`, not unpublished gitlinks.
- Do not commit ROMs, binaries, generated logs, or disposable instrumentation.
  Temporary patches/results live in ignored `build/experiments/` on this machine;
  they are not available merely by cloning Git elsewhere.
- Advance simulation at the game-loop boundary, never at input reads/rendering.
  Preserve full 512 MiB RAM/register replay checks and deliberate negative controls.
  Cross-process comparisons may normalize only understood process-local pointers.
  Never omit framebuffer pages or accept approximate hashes to get a pass.
- No subagents have been requested. No datacenter server has been rented/deployed.
  Prepare a concrete deployment for review before spending money or opening a service.

## What already works, and what does not

The lobby, server, seat assignments, readiness/revision checks, synchronized start,
input history/relay, bounded rollback, and disposable game-process ownership exist.
Automated tests cover independently delayed/lost/reordered inputs and lifecycle
cases. The Mac Online Battle screen has been exercised. Four local Mac processes
have completed scripted three-round rollback matches with injected delay/loss.
Windows x64 builds the full game and passes real lifecycle checks.

These are tested prototypes, not a completed mixed-platform multiplayer release.
No repeated two-human Mac/Windows match or four-remote-human acceptance run has
passed. Compatibility intentionally rejects unproved build pairs. Step 1 remains
open despite substantial implementation of later infrastructure.

The two distinct correctness problems are:

1. **Intermittent Windows same-process replay mismatch.** Replaying identical
   input after restore sometimes changes guest-visible framebuffer bytes.
2. **Mac/Windows rendering mismatch.** Even successful same-machine replay runs
   can disagree across platforms. Normal CPU/GPU floating-point and rasterization
   behavior is not sufficiently identical.

Resolve and test these separately. Passing one does not establish the other.

## First action: recover the interrupted Windows control

The user deliberately shut down the Windows PC during testing. Do not diagnose
the SSH timeout as a renderer crash or repeatedly poll an offline machine. Check
availability when appropriate; useful local audio/controller work can continue
while waiting.

Authorized access and the private workspace (SSH alias, Windows root, source and
ROM locations) are in the Windows test PC section of `LOCAL.md`, which is
machine-specific and not in Git. `<root>` below is that Windows root.

The user explicitly authorized copying their local US ROM to this PC; it is
already there. Do not ask again or copy it elsewhere. The machine is Windows 11
x64, Ryzen 7 9800X3D, Radeon RX 9070 XT, 64 GB RAM. It has VS2022 and private
portable LLVM 19.1.3/CMake/Ninja/Python tools under the test root.

Inspect these existing remote files before launching anything:

- `windows-depth-sync-control-build.log` and
  `windows-depth-sync-control-desktop.log`.
- `game/source/experiments/results/depth-{none,barriers,repeat}-windows.txt`.
- The actual source hashes, `game/source/source-bundle.json`, any private
  `BM64Recompiled.exe` process, and `BM64-private-test-*` scheduled task.

The interrupted batch runs three modes of the same diagnostic image: no depth
trace, depth transitions only, and full per-draw readback. All keep the same native
fallback shader selection and no-MSAA configuration. **The batch echoes the first
two case exit codes but returns only the third case's status. Inspect each result;
a successful overall exit is not proof that all three passed.** Its outcome is
currently unknown.

All three diagnostic source files were restored locally:
`rt64_state.cpp`, `rt64_framebuffer_renderer.cpp`, and
`rt64_framebuffer_renderer.h` in `lib/rt64/src/`. Windows restoration is pending.
The exact diagnostic is archived at
`build/experiments/depth-draw-attribution.patch`. Checked restoration payloads are
`windows-restored-depth-draw-{delta.json,delta.tar.gz}` and
`windows-restored-depth-draw.cmd` in that same local directory.

Use the existing hash-checked transfer helpers (`apply-render-delta.py` and
`restore-render-delta.py`) after reading them. They check old actual/manifest hashes
and new payload hashes, update the manifest, touch copied source timestamps, build,
and run a bounded desktop test. **Touch timestamps:** earlier experiments silently
reused stale Windows shaders because archive timestamps preceded build outputs.
Use new log/staging names; an existing staging directory is deliberately refused.
If the interrupted update did not reach its expected source hashes, reconcile the
actual state; do not bypass the checks.

GUI game tests must use the existing bounded `run-windows-desktop.ps1` scheduled
task wrapper. Direct SSH runs are in session 0 and fail swapchain creation.
Do not start overlapping games/builds or infer completion from an SSH disconnect.
Inspect authoritative logs and processes first. The old
`final-native-stability-cleanup.json` predates the retained texture-readback fix;
do not blindly use it to revert that fix.

## Windows same-machine failure: evidence and next experiments

The normal renderer has failed during startup at varying updates around
1382–1557, sometimes only on a later restore/fresh run. The common differences
are three adjacent pixels: words at `0x11c9e0/4` (color) and `0x21c9e0/4` (depth).
Some traces report additional pages; inspect each complete failure. Registers
agree. Two 280-row fresh baseline histories matched even when a later restore
failed, so this is not explained by different fresh-process inputs.

Already tried; none is an accepted fix:

- Drain prior presentation before each native display list: retained as a
  conservative ordering boundary, but later Windows runs still fail.
- Force one native fallback shader path: still fails, e.g. update 1418.
- Disable MSAA: still fails, e.g. 1490, in the same four words.
- Reload native color/depth targets from guest RAM each display list: fails at 1480.
- Disable alpha/coverage rejection: fails at 1427, with depth-only differences.
- Disable decal depth rejection: fails at 1382.

Evidence narrowing the cause:

- At a failed update, all 28 traced CPU draw arrays and frame/dither counters match.
- GPU position/UV/color output readbacks match despite a depth failure at 1385.
- A larger trace fails at 1542 with all ten sampled GPU input/output buffers equal
  at that update; all 21,429 raster records through the failure also match.
  Sampled geometry is finite and indices are in range. Earlier unused rectangle
  face-index fields differ, but not at that failed update.
- Actual GPU frame constants match through failure at 1557. Only `gpuTiles` differs
  among that trace's captured buffers: eleven texture slot indices change, other
  tile fields match. Reused slots can be valid. **Actual texture contents and their
  bound descriptors have not been proved equal.**
- Most recent experiment: capture single-sample depth after every native draw.
  All 74,643 replay records match 24,881 baseline records in an 8×3 region
  (`x=46..53`, `y=182..184`), and all 280 startup boundaries pass three restores.
  The relevant depth-writing indexed draw starts at face-index offset 120;
  its ordinal varies (38/39/40). Image SHA-256:
  `45ee821a2268f7c519fa0b55ca1f36257709a027cca037931601ab090d5d9d58`.
  This instrumentation adds transitions/copies and may suppress the bug.

Next work should answer a specific question per experiment:

1. Recover/complete the three-mode control. Repeat independent fresh runs where
   needed to distinguish a stable effect from the known intermittent pass.
2. If barriers/readback change the outcome, narrow their placement around the
   depth-writing draw. Separate transitions, actual copies, allocation changes,
   and CPU timing effects. Do not retain a per-draw stall as a fix without locating
   the violated ordering/resource requirement and measuring its cost.
3. If a failure is captured, compare depth before/after that draw and then the
   relevant primitives, pixel rejection, and bound resources. Check actual texture
   contents/descriptor mapping, and any untraced framebuffer constants or generated
   test-Z indices used by that draw. These are hypotheses, not established causes.
4. Keep only a demonstrated minimal fix. Revalidate the uninstrumented image with
   repeated startup, battle, round, and match histories and negative controls.

Useful local evidence under `build/experiments/`:

- `depth-readback-fixed-windows.txt`, its `.comparison.json`, and
  `compare-depth-draw-trace.py`.
- `replay-{gpu,upload,frame-fixed}-comparison.json` and corresponding Windows logs.
- `native-replay-input-comparison.json` and `native-input-replay-windows.txt`.
- `gpu-replay-attribution.patch` and `native-stability-diagnostics.patch`.

Archived patches overlap. Read/check them and selectively reuse a probe; do not
stack them blindly or promote their intentionally disabled shader checks.

## Cross-platform mismatch: what the prototypes established

CPU RSP transform multiply/add rounding has a retained fix. Shader-side prototypes
are archived, **not active in the normal build**:

- Ordered matrix arithmetic, strict Metal compilation, and DXC `-Od` match sampled
  pre-division clip positions. Hardware division still differed.
- An integer IEEE binary32 divide removes sampled position differences. Its CPU
  regression passed 2,130,968 edge/random cases at O0/O2 on both platforms.
- Ordered directional-light arithmetic and exact normalization division remove
  sampled lighting differences. Screen/UV/color buffers then match byte for byte.
- Full RAM still differs. A short boundary-1601 fixture differs in 50 framebuffer
  bytes across 12 pages. Raw rendered color already differs before guest writeback,
  so changing writeback rounding alone cannot solve it.
- A diagnostic triangle shader with ordinary interpolation differs in 45,377 pixels.
  Explicit ordered edge/perspective arithmetic plus the descriptor fix reduces
  this to 1,684 pixels, each one RGBA16-UNORM unit apart.
- Byte-encoded UV with 2× MSAA leaves 615 one-unit resolved-component differences;
  it does not prove per-sample UV equality. A no-MSAA low-24-bit depth diagnostic
  yields byte-identical first-frame captures and four matching replay boundaries.
  These are diagnostic colors and a short boot fixture, not normal Battle proof.

Reuse `build/experiments/explicit-interpolation-diagnostic.patch` and
`ordered-native-arithmetic-and-readback.patch`, with their adjacent tests/results.
The plan lists exact images, comparisons, and excluded stale-shader/failed runs.

After Windows replay stability is established, isolate any deterministic arithmetic
to native guest-visible rendering. Restore normal texture/color/alpha/depth behavior;
evaluate texture sampling, interpolation, depth, MSAA resolve, color math, blending,
and writeback in bounded stages. Also cover positional lights, texture generation,
and other scenes that the sampled directional-light fixture did not certify.
Preserve offline rendering and evaluate performance only after correctness.

## Retained fixes and current checkout

Important commits already retained:

- `3accb8e`: CPU RSP transform rounding.
- `83ca74a`: restore native frame/noise/dither counters with checkpoints.
- `ff8792b`: serialize checkpoint native rendering after prior presentation.
- `691042d`: retain Windows shader compile/link diagnostic text.
- `6eb02c6`: static samplers no longer shift D3D12 buffer/texture heap offsets.
  `bm64_d3d12_descriptor_test` passes; restoring the original bug makes it fail.
- `8bb4890`: texture-to-buffer copy uses the source texture for sample positions,
  fixing a null dereference. `bm64_d3d12_copy_test` passes exact padded depth
  readback. A separate deliberate reversal test is prepared but has not run.

The latest normal local Mac executable is
`build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled`, SHA-256
`4465bed2d32ded468a0b747c9eed07f1417d1a7ae719af76fb00000a091ae990`.
It was unchanged by the newest Windows diagnostics. The last normal Windows
image before the readback fix is
`a87f2be3b2418211b5fbfbf7efe84c7c91992ac71de5f4742f5e8872c5e9b37b`.
Both have short-control evidence; neither hash is a mixed-platform certificate.
The current remote image/source state after shutdown must be inspected.

Primary code areas: `src/game/checkpoint.inc`, `src/main/rt64_render_context.cpp`,
`lib/rt64/src/hle/rt64_state.cpp`, `lib/rt64/src/render/rt64_framebuffer_renderer.cpp`,
the RT64 shaders, and `lib/rt64/src/contrib/plume/plume_d3d12.cpp`.
Lobby/session code lives in `src/online/`, UI in `src/ui/ui_online.cpp`, and the
standalone server/tests in `server/`. Follow existing patterns.

## Finish the remaining acceptance in order

1. Prove same-machine replay on each platform, then complete comparable startup,
   active battle, next-round, and match-end histories across the exact build pair.
   Include two fresh processes, repeated restores, and input/omitted-stack negative
   controls. Keep executable/ROM/fixture/settings identities with every result.
   Existing runners include `run_independent_clients.py`,
   `run_online_transition_tests.py startup|round|match`, and
   `run_battle_replay.sh`; read their platform setup before invoking them.
2. Revalidate cancellation and two consecutive real sessions with isolated settings,
   equal Ready digests, preserved personal settings, and no leftover process/task.
   Recheck the existing room, relay, input-history, channel, owner, and fingerprint
   tests on the resulting builds. Do not recreate already implemented infrastructure.
3. Enable real audio with no duplicate replay output and record any confirmation
   delay. Disable online rumble. Verify visual history recovery and completion
   events. Check real controller routing/calibration and P1 shared menus, then
   have two humans complete repeated Mac/Windows matches without desyncs.
4. Prepare the dedicated Linux relay and arrange four humans on separate machines.
   Run at least ten complete matches and 30 minutes, including controlled delay/loss
   and a real remote session. Check disconnect recovery and collect feedback.
5. Measure RTT/jitter/loss, correction depth/frequency, stalls, CPU/display deadlines,
   frame-gap tails, state age, and recovery. Keep the comparison buffer at two;
   preserve 8/12-update stress cases. Measure the presentation-wait cost. Do not
   claim reliable 60 Hz or physical input-to-photon latency without measurements.

Update the main plan after meaningful findings. Distinguish historical exact-run
passes, current validated behavior, hypotheses, and blockers. Do not declare the
goal complete until the playable acceptance criteria actually pass.
