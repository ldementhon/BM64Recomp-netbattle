# State replay development

This branch contains four-player controller support and an opt-in, same-process
state replay experiment developed on Apple Silicon macOS. Normal play uses the
threaded runtime. The experiment does not yet provide network multiplayer.

## Dependency changes and building

The dependency revisions remain pinned to their upstream commits. Local runtime
and renderer changes are stored in these patches:

- `patches/runtime-checkpoint.patch`: deterministic scheduling, checkpoint state,
  and diagnostic runtime hooks.
- `patches/rt64-catchup.patch`: replay presentation suppression, changed-block
  writeback, and timing instrumentation.
- `patches/plume-presentation-probe.patch`: Metal presentation timing.
- `patches/hlslpp-stdlib.patch`: standard-library include needed by the Mac build.

Initialize submodules with `git submodule update --init --recursive`, then apply
these patches through `sh experiments/build-macos.sh /path/to/your/US-ROM.z64`.
The script accepts already-applied patches. Patched submodules will appear dirty
in Git; their changes are preserved in the patch files, not new submodule commits.
Update the corresponding patch when changing dependency code.

The build recipe requires Apple Silicon macOS, CMake, Ninja, an Xcode Metal
toolchain, LLVM 18 (default `/opt/homebrew/opt/llvm@18`), and a user-supplied US 1.0
big-endian ROM. It verifies the ROM and external tool revisions before building.
Prepare these checkouts under `../tools`, or set `BM64_TOOLS_ROOT`:

| Tool | Repository | Commit |
| --- | --- | --- |
| N64Recomp | https://github.com/N64Recomp/N64Recomp | `98bf104b1b5ed83126af8bcab0cc964782617dbf` |
| bm64 | https://github.com/bomberhackers/bm64 | `c7c5fd5a73a9d5969673535030fc4f1df956fa5f` |
| SDL2 | https://github.com/libsdl-org/SDL | `fb1497566c5a05e2babdcf45ef0ab5c7cca2c4ae` |

The script builds native SDL2 rather than using an installed compatibility layer.
`LLVM_ROOT`, `BUILD_JOBS`, and `BM64_TRACE_MEMORY` are optional overrides.

## Checks

Run from the repository root after applying the dependency patches:

```sh
python3 tests/run_controller_tests.py
sh tests/run_replay_input_tests.sh
sh tests/run_coroutine_replay.sh
sh tests/run_game_session_tests.sh
sh tests/run_comparable_context_tests.sh
mkdir -p build/experiments
for test in checkpoint_memory changed_writeback presentation_recovery battle_state; do
    clang++ -std=c++20 -O2 -pthread "tests/$test.cpp" -o "build/experiments/$test"
    "build/experiments/$test"
done
```

The controller runner uses `build/compile_commands.json`, which the Mac build
recipe enables. Coroutine checks run at both `-O0` and `-O2`.

Replay input checks also run at both optimization levels. Boundary N is the start
of simulation update N: every controller read during that update uses input N,
and boundary N+1 checks the resulting state. Rendering and controller reads do not
advance this counter. Restoring a checkpoint restores the frame and fast-boot
input clock; replay reads historical scripted inputs, never live controllers.
The delayed-input fixture treats player 1 as local and players 2–4 as remote,
holding each remote pad's last input during a simulated delivery gap, then
replaying the corrected inputs. It simulates a shared delivery gap, not a network
transport or independent packet arrival times.
Delivery gaps that cannot be corrected before the run ends are skipped, so a
successful verification also checks the final state after all corrections.

Real-game probes require a built app and `../Bomberman 64 (USA).z64`. They use
temporary settings and saves and write disposable logs to `experiments/results/`:

```sh
python3 tests/run_startup_probe.py
python3 tests/run_single_battle_probe.py
sh tests/run_battle_replay.sh
sh tests/run_catchup.sh verify
sh tests/run_simplified_validation.sh verify
BM64_SESSION_REAL_GAME=1 sh tests/run_game_session_tests.sh
python3 tests/run_independent_clients.py
python3 tests/run_match_settings.py
python3 tests/run_online_transition_tests.py startup
python3 tests/run_online_transition_tests.py round
python3 tests/run_online_transition_tests.py match
```

The checked-in text fixtures are scripted inputs, not generated results. Keep
timing runs serial and separate from hash verification; `perf` modes are available
in the catch-up, running-loop, and simplified-validation runners. Historical
trial patches, raw results, and reports are intentionally excluded from commits.

## Limits

Snapshots retain native continuations and restore them at their original
addresses. They cannot be transferred between processes or used as portable save
files. GPU history is not snapshotted. The diagnostic uses a 16-slot history,
retained-allocation limits, and an OS-thread generation cap, so long sessions and
cross-machine determinism remain unproven. Reliable 60 Hz rollback remains an
unmet target.

Some scripted battle timings were developed before the controller changes.
Rerun the real-game checks on the combined build before relying on their results;
archived measurements from earlier executables do not validate this combination.

The online lobby work is tracked in `online-lobby-plan.md`. Its separate
`COMPARABLE_V1` diagnostic hashes all RAM and canonical guest registers across
fresh processes; it preserves the existing same-process checks and rejects unknown
register pointers. Matching it does not certify native continuation/runtime queue
or cross-platform state. The session-owner checks use disposable child processes;
they do not yet detect normal game completion or retain a real server connection.
`--online-test` is an isolated settings diagnostic, not a playable online mode.
The old update-1600 boot handoff reaches Stage Select, not Player Select.
Use `BM64_BOOT_TO_PLAYER_SELECT=1` with the checkpoint runner (automatic with
`--online-test`) to stop scripted boot at the actual menu input loop. The hook
reaches boundary 1374 in the recorded fixture; `BM64_HOLD_FRAME=1402` produces a
visible Player Select capture. The readiness marker is included in checkpoints
and both state digests. Regenerate game code with `./N64Recomp bm64.us.toml` when
changing these hooks, then rebuild; never edit or commit generated functions.
The startup check rewinds across entry, handoff, and exit to Stage Select.

Overlay 17 serves both round results and the final match winner. The next-round
regression proves that acknowledging an ordinary round resumes battle. The match
regression uses only controller inputs to win three rounds, then crosses from
scene 20 to scene 21 at boundary 5401 after acknowledging the third star. The
completion predicate supports only this tested first-to-three single-battle
preset. It must eventually be acted on only at a confirmed simulation boundary;
no live network confirmation or return-to-lobby path is implemented yet.
