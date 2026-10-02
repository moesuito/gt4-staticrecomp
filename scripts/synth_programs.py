"""Generate synthetic programs with an independent reference model.

Two modes produce committed fixtures consumed by tests/unit/ee_synth_test.cpp:

- ``straight``: random straight-line programs (`tests/data/synth-straight.txt`);
- ``branching``: structured control-flow programs — countdown loops, conditional
  skips (including likely branches and link branches) and call/return shapes
  (`tests/data/synth-branching.txt`).

The expected final state is computed by a second, independent implementation of
the execution rules (Python, explicit masks). The C++ interpreter executes the
fixtures and must reproduce every expectation; the C++ side also decodes the
words independently, so an encoding slip shows up as a behavioral difference.
Only original synthetic content is produced.
"""

import argparse
import random
from pathlib import Path

MASK32 = 0xFFFFFFFF
MASK64 = 0xFFFFFFFFFFFFFFFF
SIGN_BIT64 = 0x8000000000000000

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


def sign_extend_8(value):
    value &= 0xFF
    return value - 0x100 if value & 0x80 else value


def signed_32(value):
    value &= MASK32
    return value - 0x100000000 if value & 0x80000000 else value


def encode_r(rs, rt, rd, shift, function):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shift << 6) | function


def encode_i(opcode, rs, rt, immediate):
    return (opcode << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


def encode_j(opcode, target):
    return (opcode << 26) | ((target >> 2) & 0x03FFFFFF)


class Machine:
    """Reference model of the supported execution rules."""

    def __init__(self, data_bytes):
        self.registers = [0] * 32
        self.memory = {}
        self.touched_registers = {0}
        self.written_bytes = set()
        self.pc = CODE_BASE
        self.pending = None  # transfer target while the delay slot executes
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


class ProgramBuilder:
    """Ordered words plus the per-address effects used by the simulator."""

    def __init__(self):
        self.words = []
        self.effects = {}

    def address(self):
        return CODE_BASE + 4 * len(self.words)

    def append(self, word, effect):
        self.effects[self.address()] = effect
        self.words.append(word)

    def append_pick(self, rng, names):
        word, effect = PICKERS[rng.choice(names)](rng)
        self.append(word, plain_step(effect))

    def append_addiu(self, rs, rt, immediate):
        self.append(addiu_word(rs, rt, immediate),
                    plain_step(addiu_effect(rs, rt, immediate)))


def pick_registers(rng):
    return (rng.choice(WORKING_REGISTERS + (0,)),
            rng.choice(WORKING_REGISTERS + (0,)),
            rng.choice(WORKING_REGISTERS))


# Word encoders and runtime effects, kept as separate pairs so the branching
# mode can reuse the semantics without applying them at generation time.

def addu_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x21)


def addu_effect(rs, rt, rd):
    return lambda m: m.write_reg32(rd, m.read_reg32(rs) + m.read_reg32(rt))


def subu_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x23)


def subu_effect(rs, rt, rd):
    return lambda m: m.write_reg32(rd, m.read_reg32(rs) - m.read_reg32(rt))


def and_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x24)


def and_effect(rs, rt, rd):
    return lambda m: m.write_reg64(rd, m.read_reg64(rs) & m.read_reg64(rt))


def or_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x25)


def or_effect(rs, rt, rd):
    return lambda m: m.write_reg64(rd, m.read_reg64(rs) | m.read_reg64(rt))


def xor_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x26)


def xor_effect(rs, rt, rd):
    return lambda m: m.write_reg64(rd, m.read_reg64(rs) ^ m.read_reg64(rt))


def machine_less_than_signed_64(left, right):
    return (left ^ SIGN_BIT64) < (right ^ SIGN_BIT64)


def slt_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x2A)


def slt_effect(rs, rt, rd):
    def effect(m):
        result = 1 if machine_less_than_signed_64(m.read_reg64(rs), m.read_reg64(rt)) else 0
        m.write_reg64(rd, result)
    return effect


def sltu_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x2B)


def sltu_effect(rs, rt, rd):
    def effect(m):
        result = 1 if m.read_reg64(rs) < m.read_reg64(rt) else 0
        m.write_reg64(rd, result)
    return effect


def daddu_word(rs, rt, rd):
    return encode_r(rs, rt, rd, 0, 0x2D)


def daddu_effect(rs, rt, rd):
    return lambda m: m.write_reg64(rd, m.read_reg64(rs) + m.read_reg64(rt))


def sll_word(rt, rd, shift):
    return encode_r(0, rt, rd, shift, 0x00)


def sll_effect(rt, rd, shift):
    return lambda m: m.write_reg32(rd, m.read_reg32(rt) << shift)


def srl_word(rt, rd, shift):
    return encode_r(0, rt, rd, shift, 0x02)


def srl_effect(rt, rd, shift):
    return lambda m: m.write_reg32(rd, m.read_reg32(rt) >> shift)


def addiu_word(rs, rt, immediate):
    return encode_i(0x09, rs, rt, immediate)


def addiu_effect(rs, rt, immediate):
    return lambda m: m.write_reg32(rt, m.read_reg32(rs) + sign_extend_16(immediate))


def andi_word(rs, rt, immediate):
    return encode_i(0x0C, rs, rt, immediate)


def andi_effect(rs, rt, immediate):
    return lambda m: m.write_reg64(rt, m.read_reg64(rs) & immediate)


def ori_word(rs, rt, immediate):
    return encode_i(0x0D, rs, rt, immediate)


def ori_effect(rs, rt, immediate):
    return lambda m: m.write_reg64(rt, m.read_reg64(rs) | immediate)


def lui_word(rt, immediate):
    return encode_i(0x0F, 0, rt, immediate)


def lui_effect(rt, immediate):
    return lambda m: m.write_reg32(rt, (immediate << 16) & MASK32)


def sw_word(rt, offset):
    return encode_i(0x2B, BASE_REGISTER, rt, offset)


def sw_effect(rt, offset):
    return lambda m: m.write_memory(m.effective_address(BASE_REGISTER, offset), 4,
                                    m.read_reg32(rt))


def lw_word(rt, offset):
    return encode_i(0x23, BASE_REGISTER, rt, offset)


def lw_effect(rt, offset):
    def effect(m):
        m.write_reg32(rt, m.read_memory(m.effective_address(BASE_REGISTER, offset), 4))
    return effect


def sb_word(rt, offset):
    return encode_i(0x28, BASE_REGISTER, rt, offset)


def sb_effect(rt, offset):
    def effect(m):
        m.write_memory(m.effective_address(BASE_REGISTER, offset), 1,
                       m.read_reg64(rt) & 0xFF)
    return effect


def lh_word(rt, offset):
    return encode_i(0x21, BASE_REGISTER, rt, offset)


def lh_effect(rt, offset):
    def effect(m):
        halfword = m.read_memory(m.effective_address(BASE_REGISTER, offset), 2)
        m.write_reg32(rt, sign_extend_16(halfword))
    return effect


def sd_word(rt, offset):
    return encode_i(0x3F, BASE_REGISTER, rt, offset)


def sd_effect(rt, offset):
    return lambda m: m.write_memory(m.effective_address(BASE_REGISTER, offset), 8,
                                    m.read_reg64(rt))


def ld_word(rt, offset):
    return encode_i(0x37, BASE_REGISTER, rt, offset)


def ld_effect(rt, offset):
    def effect(m):
        m.write_reg64(rt, m.read_memory(m.effective_address(BASE_REGISTER, offset), 8))
    return effect


def lb_word(rt, offset):
    return encode_i(0x20, BASE_REGISTER, rt, offset)


def lb_effect(rt, offset):
    def effect(m):
        byte = m.read_memory(m.effective_address(BASE_REGISTER, offset), 1)
        m.write_reg32(rt, sign_extend_8(byte))
    return effect


def lbu_word(rt, offset):
    return encode_i(0x24, BASE_REGISTER, rt, offset)


def lbu_effect(rt, offset):
    def effect(m):
        m.write_reg32(rt, m.read_memory(m.effective_address(BASE_REGISTER, offset), 1))
    return effect


def sra_word(rt, rd, shift):
    return encode_r(0, rt, rd, shift, 0x03)


def sra_effect(rt, rd, shift):
    def effect(m):
        value = m.read_reg32(rt)
        if shift == 0:
            result = value
        else:
            fill = 0xFFFFFFFF if value & 0x80000000 else 0
            result = (value >> shift) | (fill & (0xFFFFFFFF << (32 - shift)))
        m.write_reg32(rd, result)
    return effect


def slti_word(rs, rt, immediate):
    return encode_i(0x0A, rs, rt, immediate)


def slti_effect(rs, rt, immediate):
    def effect(m):
        bound = sign_extend_16(immediate) & MASK64
        result = 1 if machine_less_than_signed_64(m.read_reg64(rs), bound) else 0
        m.write_reg64(rt, result)
    return effect


def sltiu_word(rs, rt, immediate):
    return encode_i(0x0B, rs, rt, immediate)


def sltiu_effect(rs, rt, immediate):
    def effect(m):
        bound = sign_extend_16(immediate) & MASK64
        result = 1 if m.read_reg64(rs) < bound else 0
        m.write_reg64(rt, result)
    return effect


def xori_word(rs, rt, immediate):
    return encode_i(0x0E, rs, rt, immediate)


def xori_effect(rs, rt, immediate):
    return lambda m: m.write_reg64(rt, m.read_reg64(rs) ^ immediate)


# Per-operation pickers: choose operands, return (word, effect) without applying.

def pick_addu(rng):
    rs, rt, rd = pick_registers(rng)
    return addu_word(rs, rt, rd), addu_effect(rs, rt, rd)


def pick_subu(rng):
    rs, rt, rd = pick_registers(rng)
    return subu_word(rs, rt, rd), subu_effect(rs, rt, rd)


def pick_and(rng):
    rs, rt, rd = pick_registers(rng)
    return and_word(rs, rt, rd), and_effect(rs, rt, rd)


def pick_or(rng):
    rs, rt, rd = pick_registers(rng)
    return or_word(rs, rt, rd), or_effect(rs, rt, rd)


def pick_xor(rng):
    rs, rt, rd = pick_registers(rng)
    return xor_word(rs, rt, rd), xor_effect(rs, rt, rd)


def pick_slt(rng):
    rs, rt, rd = pick_registers(rng)
    return slt_word(rs, rt, rd), slt_effect(rs, rt, rd)


def pick_sltu(rng):
    rs, rt, rd = pick_registers(rng)
    return sltu_word(rs, rt, rd), sltu_effect(rs, rt, rd)


def pick_daddu(rng):
    rs, rt, rd = pick_registers(rng)
    return daddu_word(rs, rt, rd), daddu_effect(rs, rt, rd)


def pick_sll(rng):
    shift = rng.randrange(0, 32)
    rt = rng.choice(WORKING_REGISTERS + (0,))
    rd = rng.choice(WORKING_REGISTERS)
    return sll_word(rt, rd, shift), sll_effect(rt, rd, shift)


def pick_srl(rng):
    shift = rng.randrange(0, 32)
    rt = rng.choice(WORKING_REGISTERS + (0,))
    rd = rng.choice(WORKING_REGISTERS)
    return srl_word(rt, rd, shift), srl_effect(rt, rd, shift)


def pick_addiu(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, rng.randrange(0, 0x100)])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return addiu_word(rs, rt, immediate), addiu_effect(rs, rt, immediate)


def pick_andi(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, 0x00FF])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return andi_word(rs, rt, immediate), andi_effect(rs, rt, immediate)


def pick_ori(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0x8000, 0x0001])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return ori_word(rs, rt, immediate), ori_effect(rs, rt, immediate)


def pick_lui(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0x0010, 0x8000])
    rt = rng.choice(WORKING_REGISTERS)
    return lui_word(rt, immediate), lui_effect(rt, immediate)


def pick_sw(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 3, 4)
    return sw_word(rt, offset), sw_effect(rt, offset)


def pick_lw(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 3, 4)
    return lw_word(rt, offset), lw_effect(rt, offset)


def pick_sb(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES)
    return sb_word(rt, offset), sb_effect(rt, offset)


def pick_lh(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 1, 2)
    return lh_word(rt, offset), lh_effect(rt, offset)


def pick_sd(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 7, 8)
    return sd_word(rt, offset), sd_effect(rt, offset)


def pick_ld(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES - 7, 8)
    return ld_word(rt, offset), ld_effect(rt, offset)


def pick_lb(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES)
    return lb_word(rt, offset), lb_effect(rt, offset)


def pick_lbu(rng):
    rt = rng.choice(WORKING_REGISTERS)
    offset = rng.randrange(0, DATA_BYTES)
    return lbu_word(rt, offset), lbu_effect(rt, offset)


def pick_sra(rng):
    shift = rng.randrange(0, 32)
    rt = rng.choice(WORKING_REGISTERS + (0,))
    rd = rng.choice(WORKING_REGISTERS)
    return sra_word(rt, rd, shift), sra_effect(rt, rd, shift)


def pick_slti(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, rng.randrange(0, 0x100)])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return slti_word(rs, rt, immediate), slti_effect(rs, rt, immediate)


def pick_sltiu(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0xFFFF, 0x8000, rng.randrange(0, 0x100)])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return sltiu_word(rs, rt, immediate), sltiu_effect(rs, rt, immediate)


def pick_xori(rng):
    immediate = rng.choice([rng.randrange(0, 0x10000), 0x8000, 0x0001])
    rs = rng.choice(WORKING_REGISTERS + (0,))
    rt = rng.choice(WORKING_REGISTERS)
    return xori_word(rs, rt, immediate), xori_effect(rs, rt, immediate)


PICKERS = {
    "addu": pick_addu, "subu": pick_subu, "and": pick_and, "or": pick_or,
    "xor": pick_xor, "slt": pick_slt, "sltu": pick_sltu, "daddu": pick_daddu,
    "sll": pick_sll, "srl": pick_srl, "sra": pick_sra,
    "addiu": pick_addiu, "andi": pick_andi, "ori": pick_ori, "xori": pick_xori,
    "slti": pick_slti, "sltiu": pick_sltiu, "lui": pick_lui,
    "sw": pick_sw, "lw": pick_lw, "sb": pick_sb, "lb": pick_lb,
    "lbu": pick_lbu, "lh": pick_lh, "sd": pick_sd, "ld": pick_ld,
}
REGISTER_OPS = ["addu", "subu", "and", "or", "xor", "slt", "sltu", "daddu"]
SHIFT_OPS = ["sll", "srl", "sra"]
IMMEDIATE_OPS = ["addiu", "andi", "ori", "xori", "slti", "sltiu", "lui"]
MEMORY_OPS = ["sw", "lw", "sb", "lb", "lbu", "lh", "sd", "ld"]
BRANCHLESS_OPS = REGISTER_OPS + SHIFT_OPS + IMMEDIATE_OPS


def plain_step(effect):
    """Wrap a plain instruction effect so it also advances the pc past the word."""
    def step(m):
        effect(m)
        m.pc += 4
    return step


def straight_emitter(picker):
    """Straight-line emitters apply the effect immediately (generation-time model)."""
    def emit(machine, rng):
        word, effect = picker(rng)
        effect = plain_step(effect)
        effect(machine)
        return word, effect
    return emit


ALL_EMITTERS = {name: straight_emitter(picker) for name, picker in PICKERS.items()}

WRITES_RD = {"addu", "subu", "and", "or", "xor", "slt", "sltu", "daddu", "sll", "srl"}


def instruction_writes(word, name):
    """Guest register written by a branchless instruction."""
    return (word >> 11) & 0x1F if name in WRITES_RD else (word >> 16) & 0x1F


def pick_random_plain(rng, names, avoid_register=None):
    for _ in range(100):
        name = rng.choice(names)
        word, effect = PICKERS[name](rng)
        if avoid_register is None or instruction_writes(word, name) != avoid_register:
            return word, plain_step(effect)
    raise ValueError("could not avoid the protected register")


# Conditions and branch building. Branches compare against zero in the
# branching mode, so both outcomes are reachable from a controlled register.

def machine_less_than_zero(value):
    return (value & SIGN_BIT64) != 0


def machine_greater_or_equal_zero(value):
    return (value & SIGN_BIT64) == 0


def machine_less_or_equal_zero(value):
    return value == 0 or (value & SIGN_BIT64) != 0


def machine_greater_than_zero(value):
    return value != 0 and (value & SIGN_BIT64) == 0


ZERO_CONDITIONS = {
    "equal": lambda register: (lambda m: m.read_reg64(register) == 0),
    "not_equal": lambda register: (lambda m: m.read_reg64(register) != 0),
    "le_zero": lambda register: (lambda m: machine_less_or_equal_zero(m.read_reg64(register))),
    "gt_zero": lambda register: (lambda m: machine_greater_than_zero(m.read_reg64(register))),
    "lt_zero": lambda register: (lambda m: machine_less_than_zero(m.read_reg64(register))),
    "ge_zero": lambda register: (lambda m: machine_greater_or_equal_zero(m.read_reg64(register))),
}

BRANCHES = {
    "beq": {"opcode": 0x04, "condition": "equal", "link": False, "likely": False},
    "bne": {"opcode": 0x05, "condition": "not_equal", "link": False, "likely": False},
    "blez": {"opcode": 0x06, "condition": "le_zero", "link": False, "likely": False},
    "bgtz": {"opcode": 0x07, "condition": "gt_zero", "link": False, "likely": False},
    "beql": {"opcode": 0x14, "condition": "equal", "link": False, "likely": True},
    "bnel": {"opcode": 0x15, "condition": "not_equal", "link": False, "likely": True},
    "bltz": {"reghimm": 0x00, "condition": "lt_zero", "link": False, "likely": False},
    "bgez": {"reghimm": 0x01, "condition": "ge_zero", "link": False, "likely": False},
    "bltzl": {"reghimm": 0x02, "condition": "lt_zero", "link": False, "likely": True},
    "bgezl": {"reghimm": 0x03, "condition": "ge_zero", "link": False, "likely": True},
    "bltzal": {"reghimm": 0x10, "condition": "lt_zero", "link": True, "likely": False},
    "bgezal": {"reghimm": 0x11, "condition": "ge_zero", "link": True, "likely": False},
    "bltzall": {"reghimm": 0x12, "condition": "lt_zero", "link": True, "likely": True},
    "bgezall": {"reghimm": 0x13, "condition": "ge_zero", "link": True, "likely": True},
}
BRANCH_NAMES = list(BRANCHES)


def branch_immediate(branch_address, target):
    offset = (target - (branch_address + 4)) // 4
    if not -0x8000 <= offset <= 0x7FFF:
        raise ValueError("branch target out of range")
    return offset & 0xFFFF


def conditional_branch_effect(condition, link, likely, target):
    def effect(m):
        if condition(m):
            if link:
                m.write_reg64(31, m.pc + 8)
            m.pending = target
            m.pc += 4
        else:
            m.pc += 8 if likely else 4
    return effect


def build_zero_branch(name, register, branch_address, target):
    spec = BRANCHES[name]
    immediate = branch_immediate(branch_address, target)
    if "reghimm" in spec:
        word = encode_i(0x01, register, spec["reghimm"], immediate)
    else:
        word = encode_i(spec["opcode"], register, 0, immediate)
    condition = ZERO_CONDITIONS[spec["condition"]](register)
    return word, conditional_branch_effect(condition, spec["link"], spec["likely"], target)


def jal_effect(target):
    def effect(m):
        m.write_reg64(31, m.pc + 8)
        m.pending = target
        m.pc += 4
    return effect


def j_effect(target):
    def effect(m):
        m.pending = target
        m.pc += 4
    return effect


def jr_ra_effect():
    def effect(m):
        m.pending = m.read_reg64(31) & MASK32
        m.pc += 4
    return effect


def simulate(machine, effects, exit_address, step_limit=10000):
    """Run effects until the pc reaches exit_address; returns the step count."""
    steps = 0
    while machine.pc != exit_address:
        if steps >= step_limit:
            raise ValueError("program did not reach its exit address")
        effect = effects.get(machine.pc)
        if effect is None:
            raise ValueError(f"no instruction effect at 0x{machine.pc:08x}")
        if machine.pending is not None:
            effect(machine)              # the delay slot executes first...
            machine.pc = machine.pending  # ...then the transfer takes effect
            machine.pending = None
        else:
            effect(machine)
        steps += 1
    return steps


# Structured shapes. Every shape terminates by construction; the loop body and
# delay slots avoid writing the counter register through rejection sampling.

def build_loop_shape(builder, rng):
    counter = rng.choice(WORKING_REGISTERS)
    iterations = rng.randint(1, 5)
    builder.append_addiu(0, counter, iterations)
    loop_address = builder.address()
    builder.append(*pick_random_plain(rng, BRANCHLESS_OPS, avoid_register=counter))
    builder.append_addiu(counter, counter, 0xFFFF)  # counter -= 1
    branch_address = builder.address()
    builder.append(*build_zero_branch("bne", counter, branch_address, loop_address))
    builder.append(*pick_random_plain(rng, BRANCHLESS_OPS, avoid_register=counter))


def build_skip_shape(builder, rng, branch_name):
    tested = rng.choice(WORKING_REGISTERS)
    value = rng.choice([0, 1, 0xFFFF, 0x8000, 0x7FFF])
    builder.append_addiu(0, tested, value)
    branch_address = builder.address()
    delay_op = pick_random_plain(rng, BRANCHLESS_OPS)
    body_count = rng.randint(1, 2)
    body_ops = [pick_random_plain(rng, BRANCHLESS_OPS) for _ in range(body_count)]
    final_op = pick_random_plain(rng, BRANCHLESS_OPS)
    target = branch_address + 8 + 4 * body_count
    builder.append(*build_zero_branch(branch_name, tested, branch_address, target))
    builder.append(*delay_op)
    for word, effect in body_ops:
        builder.append(word, effect)
    builder.append(*final_op)


def build_call_shape(builder, rng):
    function_body = [pick_random_plain(rng, BRANCHLESS_OPS) for _ in range(rng.randint(1, 2))]
    main_body = [pick_random_plain(rng, BRANCHLESS_OPS) for _ in range(rng.randint(1, 2))]
    final_ops = [pick_random_plain(rng, BRANCHLESS_OPS) for _ in range(rng.randint(1, 2))]
    delay0 = pick_random_plain(rng, BRANCHLESS_OPS)
    # Layout, starting at the builder's current address:
    #   [j main][delay] [function body...][jr ra][delay] [main body...][jal][delay][final...]
    function_address = builder.address() + 8
    main_address = function_address + 4 * (len(function_body) + 2)
    builder.append(encode_j(0x02, main_address), j_effect(main_address))
    builder.append(*delay0)
    for word, effect in function_body:
        builder.append(word, effect)
    builder.append(encode_r(31, 0, 0, 0, 0x08), jr_ra_effect())
    builder.append(*pick_random_plain(rng, BRANCHLESS_OPS))
    for word, effect in main_body:
        builder.append(word, effect)
    builder.append(encode_j(0x03, function_address), jal_effect(function_address))
    builder.append(*pick_random_plain(rng, BRANCHLESS_OPS))
    for word, effect in final_ops:
        builder.append(word, effect)


def generate(seed, count, require_full_coverage=False):
    """Return the straight-line fixture text (mode=straight)."""
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
            word, _effect = ALL_EMITTERS[name](machine, rng)
            words.append(word)
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
            word, _effect = ALL_EMITTERS[name](machine, rng)
            words.append(word)
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

    missing = sorted(set(PICKERS) - covered)
    if require_full_coverage and missing:
        raise ValueError("generated programs do not cover: " + ", ".join(missing))
    return "\n".join(lines) + "\n"


def generate_branching(seed, count, require_full_coverage=False):
    """Return the branching fixture text (mode=branching)."""
    rng = random.Random(seed)
    lines = [f"# synth-branching v1 seed={seed} count={count}",
             "# Generated by scripts/synth_programs.py. Do not edit by hand."]
    covered = set()
    skip_counter = 0
    odd_counter = 0
    for index in range(count):
        machine = Machine(bytes(DATA_BYTES))
        initial = {}
        for _ in range(rng.randint(3, 6)):
            register = rng.choice(WORKING_REGISTERS)
            initial[register] = rng.choice([
                rng.getrandbits(64), rng.getrandbits(32), 0xFFFFFFFFFFFFFFFF,
                0x80000000, 0xFFFFFFFF80000000, rng.getrandbits(16)]) & MASK64
        for register in sorted(initial):
            machine.set_initial(register, initial[register])

        builder = ProgramBuilder()
        for _ in range(rng.randint(1, 3)):
            builder.append_pick(rng, BRANCHLESS_OPS)

        if index % 2 == 0:
            branch_name = BRANCH_NAMES[skip_counter % len(BRANCH_NAMES)]
            skip_counter += 1
            build_skip_shape(builder, rng, branch_name)
            covered.add(branch_name)
        elif odd_counter % 2 == 0:
            odd_counter += 1
            build_loop_shape(builder, rng)
            covered.add("bne")
        else:
            odd_counter += 1
            build_call_shape(builder, rng)
            covered.update(("jal", "jr"))

        exit_address = CODE_BASE + 4 * len(builder.words)
        steps = simulate(machine, builder.effects, exit_address)

        lines.append(f"program b{index:03d}")
        for register in sorted(initial):
            lines.append(f"init r{register} {initial[register]:016x}")
        for word in builder.words:
            lines.append(f"word {word:08x}")
        for register in sorted(machine.touched_registers):
            lines.append(f"expect r{register} {machine.read_reg64(register):016x}")
        lines.append(f"steps {steps}")
        lines.append(f"expect_pc {exit_address:08x}")
        lines.append("end")

    missing = sorted((set(BRANCH_NAMES) | {"jal", "jr"}) - covered)
    if require_full_coverage and missing:
        raise ValueError("generated programs do not cover: " + ", ".join(missing))
    return "\n".join(lines) + "\n"


DEFAULT_OUTPUTS = {
    "straight": Path(__file__).resolve().parents[1] / "tests/data/synth-straight.txt",
    "branching": Path(__file__).resolve().parents[1] / "tests/data/synth-branching.txt",
}
DEFAULT_SEEDS = {"straight": 20261001, "branching": 20261002}
DEFAULT_COUNTS = {"straight": 40, "branching": 30}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("straight", "branching"), default="straight")
    parser.add_argument("--seed", type=int, default=None)
    parser.add_argument("--count", type=int, default=None)
    parser.add_argument("--output", type=Path, default=None)
    arguments = parser.parse_args(argv)
    seed = arguments.seed if arguments.seed is not None else DEFAULT_SEEDS[arguments.mode]
    count = arguments.count if arguments.count is not None else DEFAULT_COUNTS[arguments.mode]
    output = arguments.output if arguments.output is not None else DEFAULT_OUTPUTS[arguments.mode]
    if arguments.mode == "straight":
        text = generate(seed, count, require_full_coverage=True)
    else:
        text = generate_branching(seed, count, require_full_coverage=True)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"wrote {count} {arguments.mode} programs to {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
