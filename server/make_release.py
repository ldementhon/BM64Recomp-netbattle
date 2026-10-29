"""Package the staged Mac and Windows builds as one online release.

Reads the release number from src/online/release.hpp, checks that both staged
executables were built with it and that no ROM is staged, then writes
release-vN.txt (the server's list of accepted builds) and the two zips.
Runs on macOS, which can compute both platforms' fingerprints.
"""
import argparse
import hashlib
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument("--rom", type=Path, default=ROOT.parent / "Bomberman 64 (USA).z64",
                    help="the US ROM players use; it is hashed, never packaged")
parser.add_argument("--share", type=Path, default=ROOT / "build/share",
                    help="folder holding staging-mac/ and staging-windows/; outputs go here")
args = parser.parse_args()
share = args.share.resolve()


def fail(message):
    sys.exit(f"make_release: {message}")


release = int(re.search(r"#define BM64_ONLINE_RELEASE (\d+)", (ROOT / "src/online/release.hpp").read_text()).group(1))
profiles = {platform: profile for profile, platform in re.findall(
    r'"(BM64 online v\d+; (macOS arm64|Windows x86_64); [^"]*)"', (ROOT / "src/online/fingerprint.hpp").read_text())}
builds = [
    ("macOS-arm64", profiles["macOS arm64"], share / "staging-mac",
     "BM64Recompiled/BM64Recompiled.app/Contents/MacOS/BM64Recompiled", "mac"),
    ("Windows-x86_64", profiles["Windows x86_64"], share / "staging-windows",
     "BM64Recompiled/BM64Recompiled.exe", "windows"),
]

rom_hash = hashlib.sha256(args.rom.read_bytes()).digest()
marker = f"Online v{release}".encode() + b"\0"
n64_headers = (b"\x80\x37\x12\x40", b"\x37\x80\x40\x12", b"\x40\x12\x37\x80")
fingerprints = {}
for label, profile, staging, executable, _ in builds:
    for path in staging.rglob("*"):
        if path.is_file() and not path.is_symlink():
            with path.open("rb") as file:
                header = file.read(4)
            if path.suffix.lower() in {".z64", ".n64", ".v64"} or header in n64_headers:
                fail(f"{path} looks like a ROM; remove it from the staging folder")
    game = (staging / executable).read_bytes()
    if marker not in game:
        fail(f"{staging / executable} was not built as online v{release}; restage it")
    fingerprints[label] = hashlib.sha256(profile.encode() + hashlib.sha256(game).digest() + rom_hash).hexdigest()
subprocess.run(["codesign", "--verify", "--deep", str(share / "staging-mac/BM64Recompiled/BM64Recompiled.app")], check=True)

release_file = share / f"release-v{release}.txt"
text = (f"# BM64 online v{release}. Written by server/make_release.py; pass this file to bm64-lobby.\n"
        f"release {release}\n" + "".join(f"{label} {fingerprint}\n" for label, fingerprint in fingerprints.items()))
# Players compare release numbers, so one number must always mean the same files.
if release_file.exists() and release_file.read_text() != text:
    fail(f"v{release} was already packaged with different files; bump BM64_ONLINE_RELEASE in src/online/release.hpp")

zips = {}
for _, _, staging, _, name in builds:
    archive = share / f"BM64Recompiled-v{release}-{name}.zip"
    archive.unlink(missing_ok=True)
    if name == "mac":
        subprocess.run(["ditto", "-c", "-k", "--keepParent", "BM64Recompiled", str(archive)], cwd=staging, check=True)
    else:
        subprocess.run(["zip", "-qrX", str(archive), "BM64Recompiled"], cwd=staging, check=True)
    zips[name] = archive
release_file.write_text(text)

for archive in zips.values():
    print(f"{archive}  ({archive.stat().st_size / 1e6:.1f} MB)")
print(release_file)
for label, fingerprint in fingerprints.items():
    print(f"  {label} {fingerprint}")
print(f"\nServer command:\n  build/lobby/bm64-lobby 0.0.0.0 6464 {release_file.relative_to(ROOT) if release_file.is_relative_to(ROOT) else release_file}")
