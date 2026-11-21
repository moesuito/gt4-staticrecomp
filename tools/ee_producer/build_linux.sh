#!/usr/bin/env bash
# TASK-074 L1: the Linux counterpart of tools/ee_producer/build.cmd (the full CORE-to-library recipe).
#
# Usage: tools/ee_producer/build_linux.sh <assets-root> <fresh-producer-directory> [c++ compiler]
#
# The assets root must hold the verified donor (private/references/ps2-web-recomp, checked out with
# core.autocrlf=true; see docs/linux/L1-GAME-LIBRARY.md), private/fingerprint-check/CORE.GT4 and the Linux tooling
# environment private/tooling-venv/bin/python with scripts/requirements.txt. The producer directory must be a fresh
# immediate child of that private/ folder. The result is a Linux candidate library (linux-library.json), never a
# production pin.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: tools/ee_producer/build_linux.sh assets-root fresh-producer-directory [c++ compiler]" >&2
    exit 2
fi
assets=$(realpath "$1")
producer=$(realpath -m "$2")
compiler=${3:-clang++}
python="$assets/private/tooling-venv/bin/python"
donor="$assets/private/references/ps2-web-recomp"
cd "$(dirname "$0")/../.."

"$python" scripts/ee_producer.py prepare --assets-root "$assets" --output "$producer"
cmake -S tools/ee_producer -B "$producer/tool-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$compiler" -DGT4_DONOR="$donor"
cmake --build "$producer/tool-build" --target eerecomp
"$python" scripts/ee_producer.py generate --output "$producer" --translator "$producer/tool-build/eerecomp"
cmake -S tools/ee_producer -B "$producer/game-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER="$compiler" -DGT4_DONOR="$donor" -DGT4_GENERATED="$producer/generated" \
    -DGT4_BUILD_TRANSLATOR=OFF
cmake --build "$producer/game-build" --target ps2game
"$python" scripts/ee_producer.py record-library --output "$producer"
