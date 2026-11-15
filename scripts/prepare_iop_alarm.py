"""Derive runtime/src/iop/libs.cpp with the IOP alarm fix (wheel force-feedback slowdown).

The donor's IOP kernel never removes a cancelled alarm from ``g_alarms``: ``CancelAlarm`` only marks it
inactive and ``fire_alarm`` returns without erasing an inactive entry, so every ``CancelAlarm`` call scans a
map that grows without bound. GT4's Logitech wheel driver (``LGDEV.IRX``) cancels and re-arms an 8 ms alarm on
each poll, so while racing the map grows by ~125 entries per second and each ``CancelAlarm`` scans the whole
map; the IOP emulation time then rises quadratically and the frame rate degrades race after race. It only shows
with a wheel because the gamepad path never starts the LGDEV polling loop.

The derived ``libs.cpp`` makes ``CancelAlarm`` erase the matching alarms (the scan stays over the handful of
armed alarms) and makes ``fire_alarm`` erase an inactive entry when its timer fires, so nothing accumulates.
Everything else is the donor's.

Overlay sections are anchored on unique donor text. Removing them must restore the normalized input, as
scripts/prepare_perf_runtime.py checks for its overlays.
"""

import argparse
import hashlib
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DONOR_LIBS = 'runtime/src/iop/libs.cpp'
# ps2-web-recomp 1b4808793a12c45125262fd2d5f50a9ec9f7c020, runtime/src/iop/libs.cpp as checked out on Windows (CRLF).
DONOR_LIBS_SHA256 = '768d67abea4d1bc2ee61d0da12e2198e9dcc1f617a553631aaf4bafebe7b40ff'

# (name, first line of the donor text, first line after it, the text that replaces it). The derived file keeps the
# donor's includes; the build gives it runtime/src/iop first, then native/, but the fix needs no native header.
SECTIONS = [
    ('fire_alarm', 'void fire_alarm(std::uint64_t key) {\n', '    const Alarm a = it->second;\n',
     'void fire_alarm(std::uint64_t key) {\n'
     '    auto it = g_alarms.find(key);\n'
     '    if (it == g_alarms.end()) return;\n'
     '    if (!it->second.active) {\n'
     '        g_alarms.erase(it);\n'
     '        return;\n'
     '    }\n'),
    ('cancel_alarm', '    auto cancel_alarm = [](Cpu &c) {\n',
     '    def("thbase", 37, "CancelAlarm", cancel_alarm);\n',
     '    auto cancel_alarm = [](Cpu &c) {\n'
     '        for (auto it = g_alarms.begin(); it != g_alarms.end();) {\n'
     '            if (it->second.handler == arg(c, 0) && it->second.arg == arg(c, 1))\n'
     '                it = g_alarms.erase(it);\n'
     '            else\n'
     '                ++it;\n'
     '        }\n'
     '        c.r[2] = 0;\n'
     '    };\n'),
]


def apply_overlay(source):
    """Returns the derived text and the donor text of each section, to check the way back."""
    removed = []
    for name, start, end, replacement in SECTIONS:
        if source.count(start) != 1:
            raise ValueError(f'{name}: the start anchor must appear exactly once in the donor source')
        first = source.index(start)
        last = source.index(end, first)
        removed.append((replacement, source[first:last]))
        source = source[:first] + replacement + source[last:]
    return source, removed


def remove_overlay(source, removed):
    for replacement, original in reversed(removed):
        if source.count(replacement) != 1:
            raise ValueError('derived source lacks an overlay section')
        source = source.replace(replacement, original, 1)
    return source


def read_donor(assets_root):
    raw = (assets_root / 'private/references/ps2-web-recomp' / DONOR_LIBS).read_bytes()
    if hashlib.sha256(raw).hexdigest() != DONOR_LIBS_SHA256:
        raise ValueError(f'Donor source differs from the pinned version: {DONOR_LIBS}')
    return raw.decode('utf-8').replace('\r\n', '\n')


def prepare(output, assets_root=None, donor=None):
    assets_root = Path(assets_root or ROOT).resolve()
    output = output.resolve()
    private = assets_root / 'private'
    if not output.is_relative_to(private) or output == private:
        raise ValueError('Derived donor code must stay inside the assets checkout private/')
    source = read_donor(assets_root) if donor is None else donor.decode('utf-8').replace('\r\n', '\n')
    derived, removed = apply_overlay(source)
    if remove_overlay(derived, removed) != source:
        raise ValueError('IOP alarm overlay does not reverse to the donor input')
    output.mkdir(parents=True, exist_ok=False)
    (output / 'libs.cpp').write_text(derived, encoding='utf-8', newline='\n')
    manifest = {
        'schema_version': 1,
        'donor_source': DONOR_LIBS,
        'donor_sha256': DONOR_LIBS_SHA256,
        'libs_sha256': hashlib.sha256(derived.encode('utf-8')).hexdigest(),
        'overlay': 'IOP alarm leak fix: CancelAlarm erases matched alarms and fire_alarm erases inactive entries',
    }
    (output / 'runtime.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8', newline='\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('--assets-root', type=Path)
    options = parser.parse_args()
    prepare(options.output, options.assets_root)
