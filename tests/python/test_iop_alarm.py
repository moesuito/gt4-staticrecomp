"""scripts/prepare_iop_alarm.py: the IOP alarm capsule's derived iop/libs.cpp (wheel force-feedback slowdown).

The tests run on a synthetic donor with every edit point, so they need no private/ checkout; the last test checks the real
pinned donor when it is present.
"""
import hashlib
import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_iop_alarm', ROOT / 'scripts/prepare_iop_alarm.py')
alarm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(alarm)
REAL_DONOR = ROOT / 'private/references/ps2-web-recomp' / alarm.DONOR_LIBS

# The donor's structure around the edit points (not the donor's text): fire_alarm and the CancelAlarm HLE.
SYNTHETIC_DONOR = '''#include "kernel.hpp"
#include "hw.hpp"
#include "ps2/state.hpp"

#include <map>

namespace ps2::iop::kernel {

struct Alarm {
    std::uint32_t handler, arg;
    bool active;
};

std::map<std::uint64_t, Alarm> g_alarms;
std::uint64_t g_next_alarm = 1;

void fire_alarm(std::uint64_t key) {
    auto it = g_alarms.find(key);
    if (it == g_alarms.end() || !it->second.active) return;
    const Alarm a = it->second;
    g_alarms.erase(it);
}

void set_alarm() { g_alarms[g_next_alarm++] = Alarm{0, 0, true}; }

void install() {
    auto cancel_alarm = [](Cpu &c) {
        for (auto &[k, a] : g_alarms)
            if (a.handler == arg(c, 0) && a.arg == arg(c, 1)) a.active = false;
        c.r[2] = 0;
    };
    def("thbase", 37, "CancelAlarm", cancel_alarm);
    def("thbase", 38, "iCancelAlarm", cancel_alarm);
}

} // namespace ps2::iop::kernel
'''


class IopAlarmRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.assets = Path(self.folder.name)
        (self.assets / 'private').mkdir()

    def tearDown(self):
        self.folder.cleanup()

    def test_every_edit_point_is_found_once(self):
        derived, _ = alarm.apply_overlay(SYNTHETIC_DONOR)
        self.assertEqual(derived.count('if (it == g_alarms.end()) return;'), 1)
        self.assertEqual(derived.count('if (!it->second.active) {'), 1)
        self.assertEqual(derived.count('it = g_alarms.erase(it);'), 1)
        self.assertNotIn('a.active = false;', derived)

    def test_cancelled_alarms_are_erased_not_just_deactivated(self):
        derived, _ = alarm.apply_overlay(SYNTHETIC_DONOR)
        self.assertIn('g_alarms.erase(it);', derived)  # both fire_alarm (inactive) and cancel_alarm erase
        self.assertNotIn('!it->second.active) return;', derived)

    def test_overlay_reverses_to_the_donor(self):
        derived, removed = alarm.apply_overlay(SYNTHETIC_DONOR)
        self.assertEqual(alarm.remove_overlay(derived, removed), SYNTHETIC_DONOR)

    def test_unknown_or_repeated_donor_text_is_refused(self):
        with self.assertRaises(ValueError):
            alarm.apply_overlay('// not the donor iop/libs.cpp\n')
        with self.assertRaises(ValueError):
            alarm.apply_overlay(SYNTHETIC_DONOR + 'void fire_alarm(std::uint64_t key) {\n')

    def test_pinned_donor_hash_is_checked(self):
        donor = self.assets / 'private/references/ps2-web-recomp' / alarm.DONOR_LIBS
        donor.parent.mkdir(parents=True)
        donor.write_bytes(SYNTHETIC_DONOR.encode('utf-8'))
        with self.assertRaisesRegex(ValueError, 'differs from the pinned version'):
            alarm.prepare(self.assets / 'private' / 'alarm', self.assets)
        self.assertEqual(len(alarm.DONOR_LIBS_SHA256), 64)

    def test_prepare_writes_the_capsule_with_its_hash(self):
        out = self.assets / 'private' / 'alarm'
        receipt = alarm.prepare(out, self.assets, donor=SYNTHETIC_DONOR.encode('utf-8'))
        written = (out / 'libs.cpp').read_bytes()
        self.assertEqual(receipt['libs_sha256'], hashlib.sha256(written).hexdigest())
        self.assertEqual(receipt['donor_source'], alarm.DONOR_LIBS)
        self.assertEqual(receipt['donor_sha256'], alarm.DONOR_LIBS_SHA256)
        on_disk = json.loads((out / 'runtime.json').read_text(encoding='utf-8'))
        self.assertEqual(on_disk, receipt)
        with self.assertRaises(FileExistsError):
            alarm.prepare(out, self.assets, donor=SYNTHETIC_DONOR.encode('utf-8'))

    def test_output_must_stay_in_private(self):
        with self.assertRaisesRegex(ValueError, 'private'):
            alarm.prepare(self.assets / 'outside', self.assets, donor=SYNTHETIC_DONOR.encode('utf-8'))

    @unittest.skipUnless(REAL_DONOR.exists(), 'needs the pinned donor checkout under private/references')
    def test_real_donor_matches_the_pin_and_derives(self):
        raw = REAL_DONOR.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), alarm.DONOR_LIBS_SHA256)
        normalized = raw.decode('utf-8').replace('\r\n', '\n')
        derived, removed = alarm.apply_overlay(normalized)
        self.assertEqual(alarm.remove_overlay(derived, removed), normalized)


if __name__ == '__main__':
    unittest.main()
