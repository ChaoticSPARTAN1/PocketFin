# SPDX-License-Identifier: GPL-3.0-or-later
# Additional terms and permissions: ADDITIONAL-TERMS.md
"""Chip32 loader for the Pocket Jellyfin core: the SDK loader plus LOADF for every save slot.

Why: core.json names a Chip32 loader (framework.chip32_vm). Analogue's Chip32 documentation: the
program "takes over the entire load process"; the minimum is CORE, LOADF for the applicable data
slots, then HOST 0x4002. Under Chip32 the Pocket loads NO slot by itself, and it writes a slot's
entry (id, size) into the core's Dataslot ID/Size table only when that slot is loaded. The SDK's
loader (upstream/openfpgaSDK/runtime/pocket/loader.bin) never executes LOADF, so on the Pocket:
  * the .sav files are never copied into the save window (CRAM0, 0x20100000...),
  * the table has no entry for any save slot, so the kernel refuses every size commit
    (openfpgaOS save.c nvslot_commit_size, "has NO datatable entry") and fopen("slot:N","wb")
    fails with EIO (syscall.c open_nv_fd),
  * at Quit / power-off the Pocket reads each save slot's entry, finds id 0 / size 0 ("ID does
    not match expected value. Did you corrupt the BRAM?") and skips the write-back.
A Pocket debug log showed exactly that: OPEN/READ/CLOSE of slot 2, CORE 0,
HOST 0x4002, not one "Updating core data slot BRAM table" line, then ten skipped readbacks.

The SDK tree stays untouched and its assembler (bass) is not installed here, so the package's
loader is the pinned SDK binary (checked by SHA-256) with one branch redirected to a short tail:

    first_start:                      ; first boot only (R13 bit 1 clear)
            or    r13,#bit_initdone   ; moved from 0x88
            ld    r1,#10              ; one pair per save slot, result deliberately ignored:
            loadf r1                  ;   a missing .sav must not stop the boot (same as the
            ...                       ;   PC Engine / SNES cores' loaders)
            ld    r1,#19
            loadf r1
            jp    start_core          ; 0x8c: ld r0,#0x4002 / host r0,r0 / exit 0

A reload (R13 bit 1 set: HOST 0x4000 then start_core) does not LOADF again: the save window and
the table entries survive the core reset (pocket_save_exit_patch.py), and reloading the card
files there would discard whatever the app saved since boot.

check_loader() runs any loader in a small Chip32 interpreter and refuses one that does not
LOADF every save slot between CORE and HOST 0x4002. PC-only: no card, no device.
"""
import hashlib

SDK_LOADER_SHA256 = 'fc1f8d37eb0fb322006b4428f0b59d2801c9b74978bd3f24103f5f9bbfca3c99'
HOST_RESET, HOST_INIT = 0x4000, 0x4002
BRANCH_AT, FIRST_START, START_CORE = 0x7E, 0x88, 0x8C


def _jp(condition, address):
    """jp (6), jp nz (7), jp z (8): low byte = address / 2, high nibble = condition."""
    return bytes([(address >> 1) & 0xFF, condition << 4 | (address >> 9) & 0xF])


def build_loader(stock, save_slots):
    """Pinned SDK loader bytes -> the package's loader (see the module text)."""
    if hashlib.sha256(stock).hexdigest() != SDK_LOADER_SHA256:
        raise ValueError('The SDK loader changed: review pocket_loader.py against its new loader.asm')
    if (stock[BRANCH_AT:BRANCH_AT + 2] != _jp(8, FIRST_START) or
            stock[FIRST_START:FIRST_START + 4] != bytes([0x0D, 0x0A, 0x02, 0x00]) or          # or r13,#2
            stock[START_CORE:START_CORE + 8] != bytes([0x00, 0x08, 0x02, 0x40, 0x00, 0x5C, 0x00, 0x46])):
        raise ValueError('The SDK loader is not laid out as expected')
    image = bytearray(stock) + bytes(len(stock) & 1)      # instructions sit on even addresses
    tail = len(image)
    image[BRANCH_AT:BRANCH_AT + 2] = _jp(8, tail)         # jp z,first_start -> the tail
    image += stock[FIRST_START:FIRST_START + 4]           # or r13,#bit_initdone
    for slot in save_slots:
        if not 0 <= slot <= 0xFFFF:
            raise ValueError('Invalid slot id')
        image += bytes([0x01, 0x08, slot & 0xFF, slot >> 8, 0x01, 0x53])   # ld r1,#slot / loadf r1
    image += _jp(6, START_CORE)
    if len(image) > 0x800:                                # the loader's .ini buffer starts at 0x0800
        raise ValueError('Loader grew into its own scratch buffer')
    return bytes(image)


class Chip32Error(ValueError):
    pass


def trace_loader(image, files, r13=0, steps=200000):
    """Run a loader the way the Pocket does; returns (events, exit code, r13).

    files: {slot id: bytes} for the slots that have a file. Events are what the host is asked to
    do, in order: ('OPEN', slot), ('READ', n), ('CLOSE',), ('CORE', id), ('LOADF', slot, found),
    ('HOST', command). Only the instructions the openfpgaOS loader uses are implemented; anything
    else raises, so an unknown loader is refused rather than guessed at.
    """
    ram = bytearray(0x2000)
    if len(image) > len(ram):
        raise Chip32Error('Loader larger than the Chip32 RAM')
    ram[:len(image)] = image
    reg = [0] * 16
    reg[13] = r13
    zero = False
    opened, events, pc = None, [], 2

    def arithmetic(op, a, b):
        nonlocal zero
        low = op & 7
        if low == 0:                                               # ld: flags unchanged
            return b
        if low in (6, 7):                                          # cmp, bit: flags only
            zero = ((a - b) if low == 6 else (a & b)) & 0xFFFFFFFF == 0
            return a
        result = {1: a & b, 2: a | b, 3: a ^ b, 4: a + b, 5: a - b}[low] & 0xFFFFFFFF
        zero = result == 0
        return result

    for _ in range(steps):
        if pc & 1 or pc + 3 >= len(ram):
            raise Chip32Error(f'Program counter left the loader (0x{pc:x})')
        a, op = ram[pc], ram[pc + 1]
        x, y = a & 15, a >> 4
        size = 2
        if 0x08 <= op <= 0x0F:                                     # op rX,#imm16
            reg[x] = arithmetic(op, reg[x], ram[pc + 2] | ram[pc + 3] << 8)
            size = 4
        elif 0x28 <= op <= 0x2F:                                   # op rX,rY
            reg[x] = arithmetic(op, reg[x], reg[y])
        elif op == 0x32:                                           # ld.b rX,(rY)
            reg[x] = ram[reg[y] & 0x1FFF]
        elif op >> 4 in (6, 7, 8):                                 # jp / jp nz / jp z
            if op >> 4 == 6 or (op >> 4 == 8) == zero:
                pc = a << 1 | (op & 15) << 9
                continue
        elif op == 0x56:                                           # open rX,rY: Z = opened, rY = length
            data = files.get(reg[x])
            events.append(('OPEN', reg[x]))
            zero, opened = data is not None, data
            if data is not None:
                reg[y] = len(data)
        elif op == 0x59:                                           # read rX,rY: at most 4 KB into RAM
            count = reg[y]
            events.append(('READ', count))
            zero = opened is not None and count <= 0x1000 and count <= len(opened)
            if zero:
                start = reg[x] & 0x1FFF
                if start + count > len(ram):
                    raise Chip32Error('read past the Chip32 RAM')
                ram[start:start + count] = opened[:count]
        elif op == 0x57:                                           # close (flags kept)
            events.append(('CLOSE',))
            opened = None
        elif op == 0x5B:                                           # core rX
            events.append(('CORE', reg[x]))
        elif op == 0x53:                                           # loadf rX: Z = loaded
            zero = reg[x] in files
            events.append(('LOADF', reg[x], zero))
        elif op == 0x5C:                                           # host rX,rY
            events.append(('HOST', reg[x]))
        elif op == 0x46:                                           # exit n
            return events, a, reg[13]
        elif op == 0x45:                                           # err rX,rY (error handler only)
            reg[x] = reg[y] = 0
        elif op in (0x40, 0x41):                                   # printf / hex (error handler only)
            pass
        else:
            raise Chip32Error(f'Instruction {a:02x} {op:02x} at 0x{pc:x} is not one this check knows')
        pc += size
    raise Chip32Error('Loader did not finish')


def loaded_slots(image, files):
    """Slots a first boot LOADFs after CORE and before HOST 0x4002 (the ones the Pocket can save)."""
    events, code, _ = trace_loader(image, files)
    if code != 0 or ('HOST', HOST_INIT) not in events or not any(e[0] == 'CORE' for e in events):
        return []
    start = next(i for i, e in enumerate(events) if e[0] == 'CORE')
    return [e[1] for e in events[start:events.index(('HOST', HOST_INIT))] if e[0] == 'LOADF']


def check_loader(image, save_slots, ini=b'[os]\nVARIANT=os25\n'):
    """Refuse a Chip32 loader under which the Pocket would never write the saves back."""
    wanted = sorted(save_slots)
    present = {2: ini, **{slot: b'' for slot in wanted}}
    for name, files in (('every .sav present', present), ('no .sav on the card yet', {2: ini})):
        events, code, r13 = trace_loader(image, files)
        if code != 0 or events[-1] != ('HOST', HOST_INIT) or ('CORE', 0) not in events:
            raise ValueError(f'Loader does not start core 0 ({name})')
        if sorted(loaded_slots(image, files)) != wanted:
            raise ValueError(
                f'Chip32 loader does not LOADF every save slot before HOST 0x4002 ({name}): the Pocket '
                'would load no .sav, write no datatable entry and skip every save at quit')
        again, code, _ = trace_loader(image, files, r13)
        if code != 0 or again != [('HOST', HOST_RESET), ('HOST', HOST_INIT)]:
            raise ValueError('A reload must only reset and restart the core (the save window is kept)')
