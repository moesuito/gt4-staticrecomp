"""Build an original, tiny MIPS ELF for mode-qualified PCSX2 controls.

This is not game code and contains no BIOS implementation. The fixture has
no controller or filesystem interaction. Its self-loop is a measurement latch,
not proof that a syscall or interrupt was handled correctly. Capture the CPU
exception entry and return separately with the debugger.
"""

import argparse
import hashlib
import json
from pathlib import Path
import struct

ENTRY_ADDRESS = 0x00100000
BRANCH_ADDRESS = ENTRY_ADDRESS + 0x60
SLOT_ADDRESS = BRANCH_ADDRESS + 4
FALLTHROUGH_ADDRESS = BRANCH_ADDRESS + 8
TARGET_ADDRESS = BRANCH_ADDRESS + 0x14
LATCH_ADDRESS = BRANCH_ADDRESS + 0x18
FILE_CODE_OFFSET = 0x1000


def program_words(branch, taken, slot, service):
    """Explicit encodings; B+4+4*4 is the target B+0x14."""
    if branch not in {"plain", "beq", "bgez", "beql"}:
        raise ValueError("Unsupported branch fixture")
    if slot not in {"syscall", "break", "effect"}:
        raise ValueError("Unsupported slot fixture")
    if not 0 <= service <= 0x7FFF:
        raise ValueError("Service must fit a nonnegative ADDIU immediate")
    branch_words = {
        "plain": 0,
        "beq": 0x11000004,
        "bgez": 0x05010004,
        "beql": 0x51000004,
    }
    slot_words = {
        "syscall": 0x0000000C,
        "break": 0x0000000D,
        "effect": 0x26100001,
    }
    # BEQ/BEQL false: t0=1. BGEZ false: t0=-1, explicitly sign-extended.
    if taken:
        operand = 0
    elif branch == "bgez":
        operand = 0xFFFF
    else:
        operand = 1
    latch_jump = 0x08000000 | (LATCH_ADDRESS >> 2)
    setup_and_markers = [
        0x3C1D01FF,                  # lui sp,0x01ff
        0x37BDFF00,                  # ori sp,sp,0xff00 (stack in 32 MiB RAM)
        0x00002021,                  # addu a0,zero,zero (gp)
        0x3C050011,                  # lui a1,0x0011 (stack base 0x00110000)
        0x3C060001,                  # lui a2,1 (stack size 0x10000)
        0x3C070010,                  # lui a3,0x0010
        0x34E78000,                  # ori a3,a3,0x8000 (zeroed argument buffer)
        0x3C080010,                  # lui t0,0x0010
        0x35080000 | (LATCH_ADDRESS & 0xFFFF), # root function: self-loop latch
        0x2403003C,                  # addiu v1,zero,0x3c (SetupThread)
        0x0000000C,                  # syscall, outside any slot
        0x0040E821,                  # addu sp,v0,zero: use BIOS-selected stack
        0x24030000 | service,        # addiu v1,zero,service
        0x24080000 | operand,        # addiu t0,zero,condition operand
        0x2402FFFF,                  # addiu v0,zero,-1 (answer sentinel)
        0x24100000,                  # addiu s0,zero,0 (effect-slot marker)
        0x24110000,                  # addiu s1,zero,0 (continuation marker)
    ]
    padding_words = (BRANCH_ADDRESS - ENTRY_ADDRESS) // 4 - len(setup_and_markers)
    return setup_and_markers + [0] * padding_words + [
        branch_words[branch],
        slot_words[slot],
        0x24110011,                  # not-taken / sequential continuation
        latch_jump,
        0x00000000,
        0x24110022,                  # branch target continuation
        latch_jump,                 # self-loop, no exit service or BREAK
        0x00000000,
    ]


def build_elf(words):
    """ELF32 little-endian ET_EXEC/EM_MIPS with one read/write/execute load."""
    code = b"".join(struct.pack("<I", word) for word in words)
    identification = b"\x7fELF" + bytes([1, 1, 1, 0]) + bytes(8)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH", identification,
        2, 8, 1, ENTRY_ADDRESS, 52, 0, 0x20924001,
        52, 32, 1, 0, 0, 0,
    )
    load = struct.pack(
        "<8I", 1, FILE_CODE_OFFSET, ENTRY_ADDRESS, ENTRY_ADDRESS,
        len(code), 0x20000, 7, 0x1000, # writable zero-fill for args/stack
    )
    return header + load + bytes(FILE_CODE_OFFSET - len(header) - len(load)) + code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="Private/ignored ELF destination")
    parser.add_argument("--branch", choices=["plain", "beq", "bgez", "beql"], required=True)
    parser.add_argument("--taken", choices=["yes", "no"], required=True)
    parser.add_argument("--slot", choices=["syscall", "break", "effect"], required=True)
    parser.add_argument("--service", type=lambda text: int(text, 0), required=True,
                        help="Explicit verified service; no inferred/default BIOS policy")
    arguments = parser.parse_args()
    words = program_words(arguments.branch, arguments.taken == "yes",
                          arguments.slot, arguments.service)
    image = build_elf(words)
    # Refuse overwrites; existing inputs/captures may be owner work.
    with arguments.output.open("xb") as stream:
        stream.write(image)
    print(json.dumps({
        "sha256": hashlib.sha256(image).hexdigest(), "size": len(image),
        "branch": arguments.branch, "taken": arguments.taken,
        "slot": arguments.slot, "service": arguments.service,
        "addresses": {
            "entry": hex(ENTRY_ADDRESS), "branch": hex(BRANCH_ADDRESS),
            "slot": hex(SLOT_ADDRESS), "fallthrough": hex(FALLTHROUGH_ADDRESS),
            "target": hex(TARGET_ADDRESS), "latch": hex(LATCH_ADDRESS),
        },
    }, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
