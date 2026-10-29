# Project

- Fork of BM64Recomp focused on four-player Battle with online rollback. Adventure co-op, lockstep, and camera syncing are outside the current scope.
- Preserve four-player controller support, stick calibration, and normal offline play. Replay is currently a same-process diagnostic developed on Apple Silicon; real network and cross-platform determinism remain unproven.

# Working guidelines

- Keep `master` aligned with upstream. Put our changes on `netbattle` or descriptive feature branches; never include `codex` in branch names.
- Machine-specific notes (local paths, the ROM location, the Windows test PC and how to reach it, local helper scripts) are in `LOCAL.md` at the repository root when it exists. It is git-ignored; read it before running builds or remote tests, and keep personal paths and hostnames there, not in tracked files.
- Other chats may edit this checkout concurrently. Preserve unrelated work and stage explicit files.
- Advance simulation at the game-loop boundary, not on controller reads or rendering. Repeated reads use the same frame's inputs; replay uses historical inputs.
- Preserve dependency edits in `experiments/patches/` and keep the build recipe reproducible; avoid submodule references to unpublished commits.
- Keep fixes small and add focused regressions. See `experiments/STATE_REPLAY.md` for setup and checks; input timing checks run with `sh tests/run_replay_input_tests.sh`.
- Commit source, useful fixtures, and concise documentation. Leave ROMs, generated builds/results, and disposable experiments out of commits.
