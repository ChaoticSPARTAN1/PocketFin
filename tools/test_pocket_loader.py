# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Does the Pocket ever write the Jellyfin core's saves back? The loader decides. PC-only.

core.json names a Chip32 loader, so the Pocket loads a data slot, and writes its entry in the
core's Dataslot ID/Size table, only when the loader executes LOADF for it (Analogue's Chip32
documentation; the Pocket's own debug logs). This test runs the SDK loader and the package's
loader (tools/pocket_loader.py) in a small Chip32 interpreter and models the three parties
from their sources and logs:

  host load   an entry (id, file size) at the slot's declaration index for each LOADFed slot
              (normal-flow log: "Updating core data slot BRAM table. Slot ID [10] idx 1");
  kernel      fopen("slot:N","wb") / fclose() commit a size only when the id scan finds the
              slot's entry (openfpgaOS file.c datatable_entry_scan_for_slot, save.c
              nvslot_commit_size); otherwise the open fails with EIO (syscall.c open_nv_fd);
  host quit   each nonvolatile slot is read at its declaration index; a different id word is
              "ID does not match expected value. Did you corrupt the BRAM?" and no write-back;
              otherwise the .sav is written with the size word's length.

The SDK loader must reproduce what a Pocket debug log showed (OPEN/READ/CLOSE of
the .ini, CORE 0, HOST 0x4002, no table write, ten skipped saves). The package's loader must
load and write back all ten. What a PC cannot show: that LOADF itself
behaves as documented on current Pocket firmware, and the bridge's first read of the save window.
"""
import sys
import unittest

from toolpaths import ROOT
from package_release import slot_tables, LOG_SLOT, CACHE_SLOTS
from pocket_loader import (build_loader, check_loader, loaded_slots, trace_loader, Chip32Error,
                           HOST_INIT, HOST_RESET)

STOCK = (ROOT / 'upstream/openfpgaSDK/runtime/pocket/loader.bin').read_bytes()
SAVES = [LOG_SLOT, *CACHE_SLOTS]
INI = b'[os]\r\nELF=pjlive.elf\r\nARGS=\r\nVARIANT=os25\r\n'   # the packaged pjlive.ini as Windows writes it
SEED = 32768                                                    # installer seed size
WRITES = {LOG_SLOT: 1234, CACHE_SLOTS[0]: 200}                  # what the app saves in a session


def session(loader, files):
    """One boot, one app session, one quit -> ({slot: bytes written back to the card}, app errors)."""
    definitions, _ = slot_tables(['pack00.pjs'])
    index = {d['id']: i for i, d in enumerate(definitions)}
    table = [0] * 128
    for slot in loaded_slots(loader, files):                    # host: Request Write + table entry
        table[index[slot] * 2] = slot
        table[index[slot] * 2 + 1] = len(files.get(slot, b''))
    errors = []
    for slot, size in WRITES.items():                           # kernel: id scan, then SAVE_DT_WORD
        entry = next((e for e in range(64) if (table[e * 2] & 0xFFFF) == slot), None)
        if entry is None:
            errors.append(slot)                                 # fopen(.., "wb") -> EIO
        else:
            table[entry * 2 + 1] = size
    written = {}
    for definition in definitions:                              # host: unload
        if definition.get('nonvolatile'):
            slot, entry = definition['id'], index[definition['id']]
            if table[entry * 2] == slot and table[entry * 2 + 1]:
                written[slot] = table[entry * 2 + 1]
    return written, errors


class LoaderSaves(unittest.TestCase):
    def setUp(self):
        self.files = {2: INI, **{slot: bytes(SEED) for slot in SAVES}}
        self.loader = build_loader(STOCK, SAVES)

    def test_sdk_loader_reproduces_the_device_log(self):
        events, code, _ = trace_loader(STOCK, self.files)
        self.assertEqual(events, [('OPEN', 2), ('READ', 43), ('CLOSE',), ('CORE', 0), ('HOST', HOST_INIT)])
        self.assertEqual(code, 0)
        written, errors = session(STOCK, self.files)
        self.assertEqual(written, {})                           # "Skipping readback" x 10
        self.assertEqual(errors, list(WRITES))                  # the app could not even open them
        with self.assertRaises(ValueError):
            check_loader(STOCK, SAVES)

    def test_package_loader_loads_and_saves_every_slot(self):
        events, code, r13 = trace_loader(self.loader, self.files)
        self.assertEqual(code, 0)
        self.assertEqual(events[:4], [('OPEN', 2), ('READ', 43), ('CLOSE',), ('CORE', 0)])
        self.assertEqual(events[4:-1], [('LOADF', slot, True) for slot in SAVES])
        self.assertEqual(events[-1], ('HOST', HOST_INIT))
        written, errors = session(self.loader, self.files)
        self.assertEqual(errors, [])
        self.assertEqual(written, {slot: WRITES.get(slot, SEED) for slot in SAVES})
        check_loader(self.loader, SAVES)
        # a reload resets and restarts only: the save window and its table entries are kept
        self.assertEqual(trace_loader(self.loader, self.files, r13)[0], [('HOST', HOST_RESET), ('HOST', HOST_INIT)])

    def test_missing_saves_do_not_stop_the_boot(self):
        events, code, _ = trace_loader(self.loader, {2: INI})
        self.assertEqual(code, 0)
        self.assertEqual([e for e in events if e[0] == 'LOADF'], [('LOADF', slot, False) for slot in SAVES])
        self.assertEqual(events[-1], ('HOST', HOST_INIT))

    def test_everything_else_in_the_sdk_loader_is_unchanged(self):
        changed = [i for i, (a, b) in enumerate(zip(STOCK, self.loader)) if a != b]
        self.assertEqual(changed, [0x7E])                       # jp z,first_start -> the tail
        self.assertEqual(len(self.loader), 266)
        for ini, core in ((INI, 0), (b'[os]\nVARIANT=os30\n', 1), (None, 0)):
            files = dict(self.files)
            if ini is None:
                del files[2]
            else:
                files[2] = ini
            for loader in (STOCK, self.loader):
                self.assertIn(('CORE', core), trace_loader(loader, files)[0])

    def test_a_changed_sdk_loader_is_refused(self):
        with self.assertRaises(ValueError):
            build_loader(STOCK + b'\0', SAVES)
        with self.assertRaises(Chip32Error):
            trace_loader(b'\0\0\xff\xff', {})


if __name__ == '__main__':
    sys.exit(not unittest.main(exit=False).result.wasSuccessful())
