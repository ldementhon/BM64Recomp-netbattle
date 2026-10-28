#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
mkdir -p build/experiments/preparation
# Do not replace this reference with the implementation under test.
git -C lib/rt64 show 71b5906f0b991459744dabfcc68d04616ac71797:src/common/rt64_tmem_hasher.h > build/experiments/preparation/original_tmem_hasher.h
python3 - <<'PYTEST'
from pathlib import Path
import subprocess
base=Path('build/experiments/preparation')
trial=base/'trial'
(trial/'src/common').mkdir(parents=True,exist_ok=True)
(trial/'src/common/rt64_tmem_hasher.h').write_bytes((base/'original_tmem_hasher.h').read_bytes())
patch=Path('experiments/patches/palette-batching-trial.patch').read_text().split('diff --git a/src/hle/rt64_rdp_tmem.cpp')[0]
subprocess.run(['patch','-p1','--batch'],cwd=trial,input=patch,text=True,check=True)
PYTEST
clang++ -std=c++20 -O2 -fsanitize=address,undefined -Ilib/rt64/src -Ilib/rt64/src/contrib -Ibuild/experiments/preparation tests/tmem_hash_equivalence.cpp -o build/experiments/preparation/tmem_hash_equivalence
build/experiments/preparation/tmem_hash_equivalence
