"""Generate synthetic straight-line programs with an independent reference model.

The output fixture is consumed by tests/unit/ee_synth_test.cpp: the C++
interpreter executes each program and compares the final registers, written
memory bytes and pc against the values this script computed. The model here is
a second, independent implementation of the execution rules (Python, with
explicit masks), so agreement between the two is real evidence — the same idea
as comparing our disassembly with Ghidra.

Only original synthetic content is produced; nothing game-derived is written.
"""

import argparse
import random
from pathlib import Path

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF

CODE_BASE = 0x00100000
DATA_BASE = 0x00100800
DATA_BYTES = 32
WORKING_REGISTERS = tuple(range(1, 16))
BASE_REGISTER = 16  # initialized to DATA_BASE by each memory-using program


def sign_extend_32(value):
    value &= MASK32
    return value | 0xFFFFFFFF00000000 if value & 0x80000000 else value


def sign_extend_16(value):
    value &= 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def signed_32(value):
    value &= MASK32
    return value - 0x100000000 if value & 0x80000000 else value


def encode_r(rs, rt, rd, shift, function):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shift << 6) | function


def encode_i(opcode, rs, rt, immediate):
    return (opcode << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


class Machine:
    """Reference model of the supported straight-line execution rules."""

    def __init__(self, data_bytes):
        self.registers = [0] * 32
        self.memory = {}
        self.touched_registers = {0}
        self.written_bytes = set()
        for offset, byte in enumerate(data_bytes):
            self.memory[DATA_BASE + offset] = byte

    def read_reg64(self, index):
        return 0 if index == 0 else self.registers[index]

    def read_reg32(self, index):
        return self.read_reg64(index) & MASK32

    def write_reg32(self, index, value):
        if index == 0:
            return
        self.registers[index] = sign_extend_32(value)
        self.touched_registers.add(index)

    def write_reg64(self, index, value):
        if index == 0:
            return
        self.registers[index] = value & MASK64
        self.touched_registers.add(index)

    def read_memory(self, address, width):
        value = 0
        for offset in range(width):
            value |= self.memory.get(address + offset, 0) << (8 * offset)
        return value

    def write_memory(self, address, width, value):
        for offset in range(width):
            self.memory[address + offset] = (value >> (8 * offset)) & 0xFF
            self.written_bytes.add(address + offset)

    def effective_address(self, base_register, immediate):
        return (self.read_reg64(base_register) + sign_extend_16(immediate)) & MASK32

    def set_initial(self, index, value):
        self.registers[index] = value & MASK64
        self.touched_registers.add(index)


def pick_registers(rng):
    return (rng.choice(WORKING_REGISTERS + (0,)),
            rng.choice(WORKING_REGISTERS + (0,)),
            rng.choice(WORKING_REGISTERS))


def emit_addu(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg32(rd, machine.read_reg32(rs) + machine.read_reg32(rt))
    return encode_r(rs, rt, rd, 0, 0x21)


def emit_subu(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg32(rd, machine.read_reg32(rs) - machine.read_reg32(rt))
    return encode_r(rs, rt, rd, 0, 0x23)


def emit_and(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg64(rd, machine.read_reg64(rs) & machine.read_reg64(rt))
    return encode_r(rs, rt, rd, 0, 0x24)


def emit_or(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg64(rd, machine.read_reg64(rs) | machine.read_reg64(rt))
    return encode_r(rs, rt, rd, 0, 0x25)


def emit_xor(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg64(rd, machine.read_reg64(rs) ^ machine.read_reg64(rt))
    return encode_r(rs, rt, rd, 0, 0x26)


def emit_slt(machine, rng):
    rs, rt, rd = pick_registers(rng)
    result = 1 if signed_32(machine.read_reg32(rs)) < signed_32(machine.read_reg32(rt)) else 0
    machine.write_reg64(rd, result)
    return encode_r(rs, rt, rd, 0, 0x2A)


def emit_sltu(machine, rng):
    rs, rt, rd = pick_registers(rng)
    result = 1 if machine.read_reg32(rs) < machine.read_reg32(rt) else 0
    machine.write_reg64(rd, result)
    return encode_r(rs, rt, rd, 0, 0x2B)


def emit_daddu(machine, rng):
    rs, rt, rd = pick_registers(rng)
    machine.write_reg64(rd, machine.read_reg64(rs) + machine.read_reg64(rt))
    return encode_r(rs, rt, rd, 0, 0x2D)


def emit_sll(machine, rng):
    shift = rng.randrange(0, 32)
    rt = rng.choice(WORKING_REGISTERS + (0,))
    rd = rng.choice(WORKING_REGISTERS)
    machine.write_reg32(rd, machine.read_reg32(rt) << shift)
    return encode_r(0, rt, rd, shift, 0x00)


def emit_srl(machine, rng):
    shift = rng.randrange(0, 32)
    rt = rng.choice(WORKING_REGISTERS + (0,))
    rd = rng.choice(WORKING_REGISTERS)
    machine.write_reg32(rd, machine.read_reg32(rt) >> shift)
    return encode_r(0, rt, rd, shift, 0x02)


def emit_addiu(machine, rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, rng.randrange(0, 0x100)])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    machine.write_reg32(rt, machine.read_reg32(rs) + sign_extend_16(immediate))
    return encode_i(0x09, rs, rt, immediate)


def emit_andi(machine, rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, 0x00FF])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    machine.write_reg64(rt, machine.read_reg64(rs) & immediate)
    return encode_i(0x0C, rs, rt, immediate)


def emit_ori(machine, rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0x8000, 0x0001])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    machine.write_reg64(rt, machine.read_reg64(rs) | immediate)
    return encode_i(0x0D, rs, rt, immediate)


def emit_lui(machine, rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0x0010, 0x8000])
    rt = rng.choice(WORKING_REGISTERS)
    machine.write_reg32(rt, (immediate << 16) & MASK32)
    return encode_i(0x0F, 0, rt, immediate)


def emit_sw(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 3, 4)
    machine.write_memory(machine.effective_address(BASE_REGISTER, offset), 4, machine.read_reg32(rt))
    return encode_i(0x2B, BASE_REGISTER, rt, offset)


def emit_lw(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 3, 4)
    machine.write_reg32(rt, machine.read_memory(machine.effective_address(BASE_REGISTER, offset), 4))
    return encode_i(0x23, BASE_REGISTER, rt, offset)


def emit_sb(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES)
    machine.write_memory(machine.effective_address(BASE_REGISTER, offset), 1,
                         machine.read_reg64(rt) & 0xFF)
    return encode_i(0x28, BASE_REGISTER, rt, offset)


def emit_lh(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 1, 2)
    halfword = machine.read_memory(machine.effective_address(BASE_REGISTER, offset), 2)
    machine.write_reg32(rt, sign_extend_16(halfword))
    return encode_i(0x21, BASE_REGISTER, rt, offset)


def emit_sd(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 7, 8)
    machine.write_memory(machine.effective_address(BASE_REGISTER, offset), 8, machine.read_reg64(rt))
    return encode_i(0x3F, BASE_REGISTER, rt, offset)


def emit_ld(machine, rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 7, 8)
    machine.write_reg64(rt, machine.read_memory(machine.effective_address(BASE_REGISTER, offset), 8))
    return encode_i(0x37, BASE_REGISTER, rt, offset)


ALL_EMITTERS = {
    "addu": emit_addu, "subu": emit_subu, "and": emit_and, "or": emit_or,
    "xor": emit_xor, "slt": emit_slt, "sltu": emit_sltu, "daddu": emit_daddu,
    "sll": emit_sll, "srl": emit_srl,
    "addiu": emit_addiu, "andi": emit_andi, "ori": emit_ori, "lui": emit_lui,
    "sw": emit_sw, "lw": emit_lw, "sb": emit_sb, "lh": emit_lh,
    "sd": emit_sd, "ld": emit_ld,
}
REGISTER_OPS = ["addu", "subu", "and", "or", "xor", "slt", "sltu", "daddu"]
SHIFT_OPS = ["sll", "srl"]
IMMEDIATE_OPS = ["addiu", "andi", "ori", "lui"]
MEMORY_OPS = ["sw", "lw", "sb", "lh", "sd", "ld"]


def generate(seed, count, require_full_coverage=False):
    """Return the fixture text: `count` deterministic straight-line programs.

    With require_full_coverage=True, raises ValueError unless the generated
    set exercises every implemented operation (used for the committed fixture).
    """
    rng = random.Random(seed)
    lines = [f"# synth-straight v1 seed={seed} count={count}",
             "# Generated by scripts/synth_programs.py. Do not edit by hand."]
    covered = set()
    for index in range(count):
        data_bytes = bytes(rng.randrange(0, 256) for _ in range(DATA_BYTES))
        machine = Machine(data_bytes)
        initial = {}
        for _ in range(rng.randint(3, 6)):
            register = rng.choice(WORKING_REGISTERS)
            initial[register] = rng.choice([
                rng.getrandbits(64), rng.getrandbits(32), 0xFFFFFFFFFFFFFFFF,
                0x80000000, 0xFFFFFFFF80000000, rng.getrandbits(16)]) & MASK64
        for register in sorted(initial):
            machine.set_initial(register, initial[register])

        words = []
        for _ in range(rng.randint(1, 3)):
            name = rng.choice(IMMEDIATE_OPS)
            words.append(ALL_EMITTERS[name](machine, rng))
            covered.add(name)

        use_memory = rng.random() < 0.75
        if use_memory:
            machine.write_reg32(BASE_REGISTER, 0x10 << 16)
            words.append(encode_i(0x0F, 0, BASE_REGISTER, 0x0010))
            machine.write_reg64(BASE_REGISTER, machine.read_reg64(BASE_REGISTER) | 0x800)
            words.append(encode_i(0x0D, BASE_REGISTER, BASE_REGISTER, 0x0800))
            covered.update(("lui", "ori"))

        target_instructions = rng.randint(12, 24)
        while len(words) < target_instructions:
            roll = rng.random()
            if use_memory and roll < 0.22:
                name = rng.choice(MEMORY_OPS)
            elif roll < 0.60:
                name = rng.choice(REGISTER_OPS)
            elif roll < 0.80:
                name = rng.choice(SHIFT_OPS)
            else:
                name = rng.choice(IMMEDIATE_OPS)
            words.append(ALL_EMITTERS[name](machine, rng))
            covered.add(name)

        lines.append(f"program p{index:03d}")
        if use_memory:
            lines.append(f"data {DATA_BASE:08x} {data_bytes.hex()}")
        for register in sorted(initial):
            lines.append(f"init r{register} {initial[register]:016x}")
        for word in words:
            lines.append(f"word {word:08x}")
        for register in sorted(machine.touched_registers):
            lines.append(f"expect r{register} {machine.read_reg64(register):016x}")
        for address in sorted(machine.written_bytes):
            lines.append(f"expect_mem {address:08x} 1 {machine.memory[address]:02x}")
        lines.append(f"expect_pc {CODE_BASE + 4 * len(words):08x}")
        lines.append("end")

    missing = sorted(set(ALL_EMITTERS) - covered)
    if require_full_coverage and missing:
        raise ValueError("generated programs do not cover: " + ", ".join(missing))
    return "\n".join(lines) + "\n"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=20261001)
    parser.add_argument("--count", type=int, default=40)
    parser.add_argument("--output", type=Path,
                        default=Path(__file__).resolve().parents[1] / "tests/data/synth-straight.txt")
    arguments = parser.parse_args(argv)
    text = generate(arguments.seed, arguments.count, require_full_coverage=True)
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {arguments.count} programs to {arguments.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
