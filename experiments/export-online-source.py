"""Export this working tree and its generated guest code for private platform tests.

No ROM, Git metadata, or local build products are included. Dependency patch files
that are intentionally untracked inside submodules are included explicitly.
"""
import argparse
import hashlib
import io
import json
from pathlib import Path
import subprocess
import tarfile
import time

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output", type=Path)
args = parser.parse_args()


def git(*arguments):
    return subprocess.check_output(["git", *arguments], cwd=ROOT)


files = set(git("ls-files", "--recurse-submodules", "-z").decode().split("\0")) - {""}
files.update({"lib/N64ModernRuntime/ultramodern/include/ultramodern/checkpoint.hpp",
              "lib/rt64/src/common/rt64_changed_writeback.h", "lib/rt64/src/common/rt64_rollback_probe.h",
              "experiments/export-online-source.py", "experiments/build-windows.ps1"})
for folder in ("RecompiledFuncs", "RecompiledPatches", "rsp"):
    files.update(str(path.relative_to(ROOT)) for path in (ROOT / folder).rglob("*") if path.is_file())
for required in ("RecompiledPatches/patches.c", "RecompiledPatches/patches_bin.c", "rsp/aspMain.cpp"):
    if required not in files or not (ROOT / required).is_file():
        raise SystemExit(f"Generate guest code before exporting: missing {required}")
if not list((ROOT / "RecompiledFuncs").glob("*.c")):
    raise SystemExit("Generate RecompiledFuncs before exporting")

manifest = {"root_commit": git("rev-parse", "HEAD").decode().strip(),
            "submodules": git("submodule", "status", "--recursive").decode().splitlines(),
            "files": {}}
args.output.parent.mkdir(parents=True, exist_ok=True)
with tarfile.open(args.output, "w:gz") as archive:
    for relative in sorted(files):
        path = ROOT / relative
        if path.suffix.lower() in {".z64", ".n64", ".v64"}:
            raise SystemExit(f"Refusing ROM in source export: {relative}")
        if not path.is_file():
            raise SystemExit(f"Missing tracked source or uninitialized dependency: {relative}")
        content = path.read_bytes()
        info = tarfile.TarInfo("source/" + relative)
        info.size, info.mode = len(content), path.stat().st_mode & 0o777
        info.mtime = int(path.stat().st_mtime)
        archive.addfile(info, io.BytesIO(content))
        manifest["files"][relative] = hashlib.sha256(content).hexdigest()
    content = json.dumps(manifest, indent=2, sort_keys=True).encode() + b"\n"
    info = tarfile.TarInfo("source/source-bundle.json")
    info.size, info.mode = len(content), 0o644
    info.mtime = int(time.time())
    archive.addfile(info, io.BytesIO(content))
with args.output.open('rb') as exported:
    digest = hashlib.sha256()
    for chunk in iter(lambda: exported.read(1024 * 1024), b''):
        digest.update(chunk)
print(f"Exported {len(files)} files to {args.output}; SHA-256={digest.hexdigest()}")
