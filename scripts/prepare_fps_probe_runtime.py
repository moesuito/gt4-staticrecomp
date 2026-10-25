"""Derive the TASK-082 fps probe runtime capsule from pinned MIT donor runtime sources (slice300).

The capsule holds three derived donor files that replace their donor originals in the build
(native/CMakeLists.txt, GT4_LICENSE_GUARD_SOURCE):
  * dispatch.cpp: license guard + display aspect + fps probe overlay.
  * kernel.cpp: halt forensic dump + kernel frame counters.
  * cdvdfsv.cpp: FSV read trace.
"""

import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

import prepare_license_guard as lg
import prepare_perf_runtime as perf

FPS_PROBE_OVERLAY_PATH = ROOT / 'scripts/fps_probe_overlay.inc'
FPS_PROBE_OVERLAY_ANCHORS = {
    'includes': ('after', '#include "display_aspect.hpp"\n'),
    'helpers': ('before', 'void dispatch(Cpu &c, std::uint32_t, std::uint32_t depth) {\n'),
    'hook': ('after', '    if (pc == gt4::display_aspect::kRaceDisplayRender) [[unlikely]] gt4::display_aspect::on_race_display_render(g_ram, kRamSize, c.r[4].w[0]);\n'),
}

PREFIX = '// TASK-082 fps probe runtime capsule: MIT donor derivation; see native/THIRD_PARTY.md.\n'
CAPSULE_FILES = ('cdvdfsv.cpp', 'dispatch.cpp', 'kernel.cpp')


def apply_fps_probe(source):
    """TASK-082 frame loop probe overlay on top of aspect-enabled dispatch."""
    return perf.apply_overlay(source, FPS_PROBE_OVERLAY_PATH, FPS_PROBE_OVERLAY_ANCHORS, 'fps probe')


def overlay_sha256(path):
    return hashlib.sha256(path.read_bytes().replace(b'\r\n', b'\n')).hexdigest()


def derive_sources(assets_root=None):
    """Returns {capsule file name: derived text (LF)}."""
    donor = Path(assets_root or ROOT) / 'private/references/ps2-web-recomp/runtime/src'
    (dispatch_name, dispatch_sha), = lg.DISPATCH_PIN.items()
    (halt_name, halt_sha), = perf.HALT_PIN.items()
    (fsv_name, fsv_sha), = perf.FSV_PIN.items()

    dispatch = lg.read_pinned(donor, dispatch_name, dispatch_sha)
    dispatch = lg.apply_license_guard(dispatch)
    dispatch = lg.apply_display_aspect(dispatch)
    dispatch = apply_fps_probe(dispatch)

    kernel = perf.apply_halt_dump(lg.read_pinned(donor, halt_name, halt_sha))
    kernel = perf.apply_kernel_frame_counters(kernel)

    return {
        'dispatch.cpp': PREFIX + dispatch,
        'kernel.cpp': kernel,
        'cdvdfsv.cpp': perf.apply_fsv_trace(lg.read_pinned(donor, fsv_name, fsv_sha)),
    }


def prepare(output, assets_root=None):
    output = Path(output).resolve()
    private = (Path(assets_root or ROOT) / 'private').resolve()
    if output.parent != private:
        raise ValueError(f'Capsule must be an immediate child of {private}, got {output.parent}')
    if output.exists():
        raise ValueError(f'Preserve existing output; directory already exists: {output}')

    sources = derive_sources(assets_root)
    output.mkdir(parents=True, exist_ok=False)

    receipt = {
        'schema_version': 1,
        'donor_revision': '1b4808793a12c45125262fd2d5f50a9ec9f7c020',
        'donor_sha256': {**lg.DISPATCH_PIN, **perf.HALT_PIN, **perf.FSV_PIN},
        'license_guard_profile': 'task017-guard-v2+task080-aspect-v2+task082-fps-probe-v1',
        'license_guard_overlay_sha256': overlay_sha256(lg.GUARD_OVERLAY_PATH),
        'display_aspect_overlay_sha256': overlay_sha256(lg.ASPECT_OVERLAY_PATH),
        'fps_probe_overlay_sha256': overlay_sha256(FPS_PROBE_OVERLAY_PATH),
        'halt_dump_overlay_sha256': overlay_sha256(perf.HALT_OVERLAY_PATH),
        'fsv_trace_overlay_sha256': overlay_sha256(perf.FSV_OVERLAY_PATH),
        'frame_counters_profile': 'task084-frame-counters-v1',
        'source_sha256': {},
    }

    for name in CAPSULE_FILES:
        path = output / name
        path.write_text(sources[name], encoding='utf-8', newline='\n')
        receipt['source_sha256'][name] = hashlib.sha256(path.read_bytes()).hexdigest()

    receipt_text = json.dumps(receipt, indent=2) + '\n'
    (output / 'runtime.json').write_text(receipt_text, encoding='utf-8', newline='\n')

    # Also write reviewed pin file
    pin_receipt = dict(receipt)
    pin_receipt['receipt_sha256'] = hashlib.sha256(receipt_text.encode('utf-8')).hexdigest()
    pin_receipt['build_variable'] = 'GT4_LICENSE_GUARD_SOURCE'

    pin_path = ROOT / f'docs/inputs/{output.name}.json'
    pin_path.write_text(json.dumps(pin_receipt, indent=2) + '\n', encoding='utf-8', newline='\n')

    return receipt


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare(args.output, args.assets_root), indent=2))
