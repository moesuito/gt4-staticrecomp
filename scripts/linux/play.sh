#!/usr/bin/env bash
# TASK-074: start the Linux build of GT4 v2.00 with the settings the Windows launcher passes by default
# (launcher/crates/gt4l-core/src/launch.rs: add_common and coregs20_environment), until a Linux launcher exists.
#
# Usage: scripts/linux/play.sh <disc image> <memory card file> [game arguments...]
#
# Environment (each optional):
#   GT4_LINUX_BUILD     build folder holding gt4_linux_window (default: private/linux074-release-build)
#   GT4_LINUX_ELF       the reconstructed game executable (default: private/linux074-ee-producer/SCUS_973.28.elf)
#   GT4_LINUX_RENDERER  coregs2 (default) or coregs1 (the native Vulkan backend)
#   GT4_HW_SCALE        CoreGS 2.0 internal resolution multiplier, 1..12 (default 3)
#   GT4_LINUX_FXAA      1 (default) or 0
# Any other GT4_* variable already set (GT4_FULLSCREEN=1, GT4_OUTPUT_SIZE=2560x1080, GT4_PRESENT_MODE=mailbox,
# GT4_TRIGGER_FULL_RANGE=0, GT4_CAMERA_360=0, ...) is passed on unchanged.
set -euo pipefail

if [[ $# -lt 2 ]]; then
    echo "Usage: scripts/linux/play.sh <disc image> <memory card file> [game arguments...]" >&2
    exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
disc=$1
card=$2
shift 2
build=${GT4_LINUX_BUILD:-$root/private/linux074-release-build}
elf=${GT4_LINUX_ELF:-$root/private/linux074-ee-producer/SCUS_973.28.elf}

# The launcher's fixed settings: races at 480p, the game's own 16:9, the display aspect from the window, fifo.
export GT4_FORCE_480P=${GT4_FORCE_480P:-1}
export GT4_FORCE_WIDESCREEN=${GT4_FORCE_WIDESCREEN:-1}
export GT4_DISPLAY_ASPECT=${GT4_DISPLAY_ASPECT:-auto}
export GT4_PRESENT_MODE=${GT4_PRESENT_MODE:-fifo}
export GT4_TRIGGER_FULL_RANGE=${GT4_TRIGGER_FULL_RANGE:-1}

if [[ ${GT4_LINUX_RENDERER:-coregs2} == coregs2 ]]; then
    export GT4_RENDERER=hw GT4_HW_CORE=2
    export GT4_HW_SCALE=${GT4_HW_SCALE:-3}
    if [[ ${GT4_LINUX_FXAA:-1} == 1 ]]; then export GT4_HW_POSTFX=${GT4_HW_POSTFX:-fxaa}; fi
    export GT4_HW_SHADER_CACHE=${GT4_HW_SHADER_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/gt4recomp/shaders}
    mkdir -p "$GT4_HW_SHADER_CACHE"
fi

exec "$build/gt4_linux_window" "$elf" --window --ntsc --disc "$disc" --memcard "$card" "$@"
