#!/usr/bin/env python3
"""Run controller tests using an existing CMake build's compiler and SDL2 setup."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

root = Path(__file__).resolve().parents[1]
build = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else root / "build"
entry = next(e for e in json.loads((build / "compile_commands.json").read_text())
             if e["file"].endswith("/src/game/input.cpp"))
flags = shlex.split(entry["command"])
output = flags.index("-o")
del flags[output:output + 2]
flags.remove("-c")
flags.remove(entry["file"])
cache = dict(line.split("=", 1) for line in (build / "CMakeCache.txt").read_text().splitlines()
             if "=" in line and not line.startswith(("#", "//")))
sdl_dir = next((Path(v) for k, v in cache.items() if k.startswith("SDL2_DIR:")), None)
sdl_config = os.environ.get("SDL2_CONFIG", str(sdl_dir.parents[2] / "bin/sdl2-config") if sdl_dir else "sdl2-config")
sdl_flags = shlex.split(subprocess.check_output([sdl_config, "--libs"], text=True))
sdl_flags += ["-Wl,-rpath," + flag[2:] for flag in sdl_flags if flag.startswith("-L")]
strip_flag = "-Wl,-dead_strip" if sys.platform == "darwin" else "-Wl,--gc-sections"
with tempfile.TemporaryDirectory(prefix="bm64-controller-tests-") as temp:
    for name, sources in (
        ("stick", ["tests/stick_calibration_test.cpp"]),
        ("ports", ["tests/controller_ports_test.cpp", "src/game/input.cpp", "src/game/controls.cpp",
                   "src/game/config.cpp", "lib/N64ModernRuntime/librecomp/src/files.cpp"]),
    ):
        executable = str(Path(temp) / name)
        subprocess.run(flags + ["-I" + str(root / "lib/N64ModernRuntime/librecomp/include/librecomp"),
                       "-ffunction-sections", "-fdata-sections", strip_flag]
                       + [str(root / s) for s in sources] + sdl_flags + ["-o", executable],
                       cwd=entry["directory"], check=True)
        subprocess.run([executable, temp], check=True)
