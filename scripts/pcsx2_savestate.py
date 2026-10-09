"""Read PCSX2 savestates (ZIP containers) and extract CPU state.

Format facts from the PCSX2 sources (SaveState.cpp, SaveState.h, R5900.h):

- a savestate is a ZIP with a stored version entry (`PCSX2 Savestate
  Version.id`: u32 save version plus a version string), memory entries
  (`eeMemory.bin` is the full 32 MiB EE RAM) and `PCSX2 Internal
  Structures.dat`, the raw freeze stream of the VM internals;
- `Freeze()` serializes values with a raw memory copy, so the internal stream
  holds host structs verbatim;
- the `cpuRegs` block starts right after a 32-byte zero-padded `cpuRegs` tag
  and holds `struct cpuRegisters`: GPR[32] as 16-byte slots (the first 8
  bytes are the 64-bit register), HI, LO, CP0 (32 x u32), sa, IsDelaySlot,
  pc, code, then scheduler state.

Python 3.14's zipfile reads the Zstandard entries natively. The tool only
reads local files; it never touches a running emulator.
"""

import argparse
import struct
import sys
import zipfile
from pathlib import Path

VERSION_ENTRY = "PCSX2 Savestate Version.id"
STRUCTURES_ENTRY = "PCSX2 Internal Structures.dat"
MEMORY_ENTRY = "eeMemory.bin"

GPR_SLOT = 16
HI_OFFSET = 32 * GPR_SLOT  # 512
LO_OFFSET = HI_OFFSET + 16  # 528
CP0_OFFSET = LO_OFFSET + 16  # 544
PC_OFFSET = 680

# PCSX2 v2.9.114 / aa7ab430: R5900.h and Counters.h. These raw host
# structures are not a stable interchange format; reject unaudited versions.
TIMING_SAVE_VERSION = 0x9A590000
TIMING_BUILD_VERSION = "v2.9.114"
EE_CYCLE_OFFSET = 1088
COUNTER_SIZE = 32

REGISTER_NAMES = [
    "zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
    "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
    "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
    "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra",
]

CP0_STATUS = 12
CP0_CAUSE = 13
CP0_EPC = 14


def find_tag(data, name):
    """Offset of a 32-byte zero-padded freeze tag, or None."""
    needle = name.encode("ascii")
    position = data.find(needle + b"\x00")
    while position >= 0:
        tag = data[position:position + 32]
        if tag == needle + b"\x00" * (32 - len(needle)):
            return position
        position = data.find(needle + b"\x00", position + 1)
    return None


def read_cpu_state(structures):
    """Decode pc, GPRs, HI/LO and key CP0 registers from the freeze stream."""
    tag = find_tag(structures, "cpuRegs")
    if tag is None:
        raise ValueError("The freeze stream has no cpuRegs tag")
    base = tag + 32
    state = {}
    state["pc"] = struct.unpack_from("<I", structures, base + PC_OFFSET)[0]
    state["gpr"] = [
        struct.unpack_from("<Q", structures, base + index * GPR_SLOT)[0]
        for index in range(32)
    ]
    state["hi"] = struct.unpack_from("<Q", structures, base + HI_OFFSET)[0]
    state["lo"] = struct.unpack_from("<Q", structures, base + LO_OFFSET)[0]
    state["cp0"] = {
        "status": struct.unpack_from("<I", structures, base + CP0_OFFSET + CP0_STATUS * 4)[0],
        "cause": struct.unpack_from("<I", structures, base + CP0_OFFSET + CP0_CAUSE * 4)[0],
        "epc": struct.unpack_from("<I", structures, base + CP0_OFFSET + CP0_EPC * 4)[0],
    }
    return state


def read_entry(path, name):
    with zipfile.ZipFile(path) as archive:
        return archive.read(name)


def read_timing_state(structures, save_version, build_version):
    """Read raw timing fields for the audited layout, without inferring gates.

    Counter counts are lazy: MMIO reads may include work since start_cycle.
    Emulator cycles do not establish physical-console instruction costs.
    """
    if save_version != TIMING_SAVE_VERSION:
        raise ValueError(f"Timing layout not verified for save version 0x{save_version:08x}")
    # Multiple releases share the save version despite changing raw layouts.
    # The embedded build label is a compatibility check, not binary attestation.
    if build_version != TIMING_BUILD_VERSION:
        raise ValueError(f"Timing layout not verified for build {build_version!r}")
    cpu_tag = find_tag(structures, "cpuRegs")
    subsystem_tag = find_tag(structures, "EE-Subsystems")
    if cpu_tag is None or subsystem_tag is None:
        raise ValueError("Timing requires cpuRegs and EE-Subsystems freeze tags")
    cycle_position = cpu_tag + 32 + EE_CYCLE_OFFSET
    counter_position = subsystem_tag + 32
    if cycle_position + 8 > len(structures) or counter_position + 4 * COUNTER_SIZE > len(structures):
        raise ValueError("Truncated timing structures")
    cycle = struct.unpack_from("<Q", structures, cycle_position)[0]
    counters = []
    for index in range(4):
        count, mode, target, hold, rate, interrupt, start_cycle = struct.unpack_from(
            "<6IQ", structures, counter_position + index * COUNTER_SIZE)
        counters.append({
            "count": count, "mode": mode, "target": target, "hold": hold,
            "rate": rate, "interrupt": interrupt, "start_cycle": start_cycle,
        })
    return {"ee_cycle": cycle, "counters": counters}


def command_timing(arguments):
    version, version_text = savestate_version(arguments.savestate)
    timing = read_timing_state(
        read_entry(arguments.savestate, STRUCTURES_ENTRY), version, version_text)
    print(f"save version: 0x{version:08x} ({version_text})")
    print(f"EE cycle:     {timing['ee_cycle']}")
    print("Counters are raw/lazy, not instantaneous MMIO reads.")
    for index, counter in enumerate(timing["counters"]):
        print(f"timer {index}: count={counter['count']} mode=0x{counter['mode']:08x} "
              f"target={counter['target']} hold={counter['hold']} rate={counter['rate']} "
              f"interrupt={counter['interrupt']} start_cycle={counter['start_cycle']}")
    return 0


def savestate_version(path):
    data = read_entry(path, VERSION_ENTRY)
    version = struct.unpack_from("<I", data, 0)[0]
    text = data[4:].split(b"\x00", 1)[0].decode("utf-8", "replace")
    return version, text


def bios_description(structures):
    # FreezeBios: 32-byte tag, u32 checksum, 256-byte description.
    return structures[36:36 + 256].split(b"\x00", 1)[0].decode("utf-8", "replace")


def command_info(_arguments):
    path = _arguments.savestate
    version, text = savestate_version(path)
    with zipfile.ZipFile(path) as archive:
        names = archive.namelist()
    print(f"save version: 0x{version:08x} ({text})")
    print(f"entries:      {len(names)}")
    print(f"bios:         {bios_description(read_entry(path, STRUCTURES_ENTRY))}")
    state = read_cpu_state(read_entry(path, STRUCTURES_ENTRY))
    print(f"pc:           0x{state['pc']:08x}")
    return 0


def command_registers(_arguments):
    path = _arguments.savestate
    state = read_cpu_state(read_entry(path, STRUCTURES_ENTRY))
    print(f"pc 0x{state['pc']:08x}  hi 0x{state['hi']:016x}  lo 0x{state['lo']:016x}")
    print(f"cp0 status 0x{state['cp0']['status']:08x}  cause 0x{state['cp0']['cause']:08x} "
          f" epc 0x{state['cp0']['epc']:08x}")
    for index, name in enumerate(REGISTER_NAMES):
        print(f"{name:>4} = 0x{state['gpr'][index]:016x}")
    if _arguments.with_memory:
        memory = read_entry(path, MEMORY_ENTRY)
        pc = state["pc"]
        if pc + 4 <= len(memory):
            word = struct.unpack_from("<I", memory, pc)[0]
            print(f"word at pc: 0x{word:08x}")
    return 0


def command_extract(_arguments):
    output = Path(_arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    data = read_entry(_arguments.savestate, _arguments.entry)
    output.write_bytes(data)
    print(f"extracted {len(data)} bytes of {_arguments.entry} into {output}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    info_parser = subparsers.add_parser("info", help="Version, BIOS and pc summary")
    info_parser.add_argument("savestate", type=Path)

    registers_parser = subparsers.add_parser(
        "registers", help="Print pc, HI/LO, CP0 and all 32 GPRs")
    registers_parser.add_argument("savestate", type=Path)
    registers_parser.add_argument("--with-memory", action="store_true",
                                  help="Also print the instruction word at pc")

    extract_parser = subparsers.add_parser("extract", help="Extract one zip entry")
    extract_parser.add_argument("savestate", type=Path)
    extract_parser.add_argument("entry")
    extract_parser.add_argument("output")

    timing_parser = subparsers.add_parser(
        "timing", help="Read raw timing fields from the verified v2.9.114 layout")
    timing_parser.add_argument("savestate", type=Path)

    arguments = parser.parse_args(argv)
    handlers = {
        "info": command_info,
        "registers": command_registers,
        "extract": command_extract,
        "timing": command_timing,
    }
    try:
        return handlers[arguments.command](arguments)
    except (OSError, ValueError, KeyError, zipfile.BadZipFile) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
