"""Check the native compatibility command against an independent SHA-256 implementation."""
import hashlib
import os
from pathlib import Path
import platform
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "build/BM64Recompiled.app/Contents/MacOS/BM64Recompiled"
ROM = ROOT.parent / "Bomberman 64 (USA).z64"
architecture = {"arm64": "arm64", "x86_64": "x86_64"}[platform.machine()]
profile = f"BM64 online v1; macOS {architecture}; fixed Battle profile 1; delay 2".encode()
game_hash = hashlib.sha256(GAME.read_bytes()).digest()
rom_hash = hashlib.sha256(ROM.read_bytes()).digest()
expected = hashlib.sha256(profile + game_hash + rom_hash).hexdigest()
with tempfile.TemporaryDirectory(prefix="bm64-fingerprint-") as temp:
    environment = {key: value for key, value in os.environ.items() if not key.startswith("BM64_")}
    environment["APP_FOLDER_PATH"] = temp
    stored = Path(temp) / "bm64_us.z64"
    stored.symlink_to(ROM)
    result = subprocess.run([str(GAME), "--online-fingerprint"], env=environment,
                            capture_output=True, text=True, timeout=15, check=True)
    assert result.stdout.strip() == expected, (result.stdout, expected)
    stored.unlink()
    missing = subprocess.run([str(GAME), "--online-fingerprint"], env=environment,
                             capture_output=True, text=True, timeout=15)
    assert missing.returncode != 0 and not missing.stdout.strip()
assert hashlib.sha256(GAME.read_bytes()).digest() == game_hash, "executable changed during test"
print(f"PASS: native fingerprint matches independent SHA-256; missing ROM refused; build={game_hash.hex()}")
