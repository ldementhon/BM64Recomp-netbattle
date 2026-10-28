"""Check the native compatibility command against an independent SHA-256 implementation."""
import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--game", type=Path, default=ROOT / ("build/BM64Recompiled.exe" if os.name == "nt"
                                                     else "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"))
parser.add_argument("--rom", type=Path, default=ROOT.parent / "Bomberman 64 (USA).z64")
args = parser.parse_args()
GAME, ROM = args.game.resolve(), args.rom.resolve()
architecture = {"arm64": "arm64", "x86_64": "x86_64", "AMD64": "x86_64"}[platform.machine()]
system = {"Darwin": "macOS", "Windows": "Windows"}[platform.system()]
profile = f"BM64 online v3; {system} {architecture}; fixed Battle profile 1; delay 2".encode()
game_hash = hashlib.sha256(GAME.read_bytes()).digest()
rom_hash = hashlib.sha256(ROM.read_bytes()).digest()
expected = hashlib.sha256(profile + game_hash + rom_hash).hexdigest()
with tempfile.TemporaryDirectory(prefix="bm64-fingerprint-") as temp:
    isolated = Path(temp) / "isolated-\u6e2c\u8a66"
    isolated.mkdir()
    (Path(temp) / "portable.txt").touch()
    environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    environment["APP_FOLDER_PATH"] = str(isolated)
    stored = isolated / "bm64_us.z64"
    shutil.copyfile(ROM, stored)
    result = subprocess.run([str(GAME), "--online-fingerprint"], env=environment,
                            cwd=temp, capture_output=True, text=True, timeout=15, check=True)
    assert result.stdout.strip() == expected, (result.stdout, expected)
    stored.unlink()
    missing = subprocess.run([str(GAME), "--online-fingerprint"], env=environment,
                             cwd=temp, capture_output=True, text=True, timeout=15)
    assert missing.returncode != 0 and not missing.stdout.strip()
    del environment["APP_FOLDER_PATH"]
    shutil.copyfile(ROM, Path(temp) / "bm64_us.z64")
    portable = subprocess.run([str(GAME), "--online-fingerprint"], env=environment,
                              cwd=temp, capture_output=True, text=True, timeout=15, check=True)
    assert portable.stdout.strip() == expected, (portable.stdout, expected)
assert hashlib.sha256(GAME.read_bytes()).digest() == game_hash, "executable changed during test"
print(f"PASS: native fingerprint matches independent SHA-256; missing ROM refused; build={game_hash.hex()}")
print(f"FINGERPRINT: {expected}")
