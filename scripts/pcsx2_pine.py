"""Talk to a running PCSX2 over the PINE protocol and verify memory images.

The local PCSX2 must have EnablePINE = true in its PCSX2.ini (the tool does not
change any configuration itself). Protocol, from the PCSX2 sources:

- request: u32 little-endian total size, then one or more commands;
- each command starts with a one-byte opcode; memory commands carry a u32
  little-endian address;
- read commands: 0x00/0x01/0x02/0x03 for 8/16/32/64 bits; 0x08 version, 0x0B
  title, 0x0C serial, 0x0F status;
- reply: u32 little-endian size, one result byte (0 = OK, 0xFF = failed),
  then the values of every command in order. Requests can batch many
  commands, which is how large reads stay fast.

Only observation is performed here: the tool never writes emulator memory.
"""

import argparse
import socket
import struct
import sys
from pathlib import Path

PINE_OK = 0x00
PINE_FAIL = 0xFF
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 28011

READ64 = 0x03
MSG_VERSION = 0x08
MSG_SAVE_STATE = 0x09
MSG_LOAD_STATE = 0x0A
MSG_TITLE = 0x0B
MSG_ID = 0x0C
MSG_STATUS = 0x0F

STATUS_NAMES = {0: "running", 1: "paused", 2: "shutdown"}

# One request must stay below PCSX2's 650000-byte request buffer and the reply
# below its 450000-byte reply buffer.
MAX_READ64_PER_REQUEST = 32768


class PineError(RuntimeError):
    pass


def build_request(commands):
    """Prefix a sequence of encoded commands with the total message size."""
    body = b"".join(commands)
    total = 4 + len(body)
    if total > 650000:
        raise PineError("Request exceeds the PINE message size limit")
    return struct.pack("<I", total) + body


def encode_read64(address):
    return bytes([READ64]) + struct.pack("<I", address)


def encode_status():
    return bytes([MSG_STATUS])


def encode_version():
    return bytes([MSG_VERSION])


def encode_title():
    return bytes([MSG_TITLE])


def encode_serial():
    return bytes([MSG_ID])


def encode_save_state(slot):
    return bytes([MSG_SAVE_STATE, slot & 0xFF])


def encode_load_state(slot):
    return bytes([MSG_LOAD_STATE, slot & 0xFF])


def parse_reply(reply):
    """Return the payload after the result byte; raise on PINE failures."""
    if len(reply) < 5:
        raise PineError(f"Reply too short: {len(reply)} bytes")
    size = struct.unpack_from("<I", reply, 0)[0]
    if size != len(reply):
        raise PineError(f"Reply size {size} does not match {len(reply)} bytes")
    if reply[4] != PINE_OK:
        raise PineError("PCSX2 reported a failed command (is a game running?)")
    return reply[5:]


def parse_string(payload):
    length = struct.unpack_from("<I", payload, 0)[0]
    return payload[4:4 + length].split(b"\x00", 1)[0].decode("utf-8", "replace")


def plan_read_chunks(address, size, reads_per_request=None):
    """Split one read into aligned 8-byte-read chunks.

    Returns (aligned_start, chunks) where each chunk is (start, read_count) and
    the chunks together cover [aligned_start, aligned_start + 8 * total).
    """
    if reads_per_request is None:
        reads_per_request = MAX_READ64_PER_REQUEST
    if size <= 0:
        raise PineError("Read size must be positive")
    start = address & ~7
    end = address + size
    end_aligned = (end + 7) & ~7
    total_reads = (end_aligned - start) // 8
    chunks = []
    position = 0
    while position < total_reads:
        count = min(reads_per_request, total_reads - position)
        chunks.append((start + 8 * position, count))
        position += count
    return start, chunks


class PineClient:
    """One TCP connection to the emulator; requests are sent one by one."""

    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=10.0):
        try:
            self._socket = socket.create_connection((host, port), timeout=timeout)
        except OSError as error:
            raise PineError(f"Cannot connect to PINE at {host}:{port}: {error}") from error

    def close(self):
        self._socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _exchange(self, commands):
        request = build_request(commands)
        self._socket.sendall(request)
        header = self._recv_exactly(4)
        size = struct.unpack_from("<I", header, 0)[0]
        if size < 5 or size > 450000:
            raise PineError(f"Invalid reply size {size}")
        rest = self._recv_exactly(size - 4)
        return parse_reply(header + rest)

    def _recv_exactly(self, count):
        data = b""
        while len(data) < count:
            block = self._socket.recv(count - len(data))
            if not block:
                raise PineError("PCSX2 closed the connection")
            data += block
        return data

    def version(self):
        return parse_string(self._exchange([encode_version()]))

    def status(self):
        payload = self._exchange([encode_status()])
        return struct.unpack_from("<I", payload, 0)[0]

    def title(self):
        return parse_string(self._exchange([encode_title()]))

    def serial(self):
        return parse_string(self._exchange([encode_serial()]))

    def save_state(self, slot):
        self._exchange([encode_save_state(slot)])

    def load_state(self, slot):
        self._exchange([encode_load_state(slot)])

    def read_bytes(self, address, size):
        """Read `size` bytes of EE memory starting at `address`."""
        aligned_start, chunks = plan_read_chunks(address, size)
        data = bytearray()
        for start, count in chunks:
            payload = self._exchange(
                [encode_read64(start + 8 * index) for index in range(count)])
            expected = count * 8
            if len(payload) != expected:
                raise PineError(
                    f"Short read: got {len(payload)} bytes for {count} words")
            data += payload
        offset = address - aligned_start
        return bytes(data[offset:offset + size])


def read_elf_segment(elf_path, vaddr, size):
    """Extract file-backed bytes for [vaddr, vaddr + size) from a little-endian ELF."""
    data = Path(elf_path).read_bytes()
    if data[:4] != b"\x7fELF":
        raise PineError(f"Not an ELF file: {elf_path}")
    elf_class = data[4]
    if elf_class == 1:  # ELF32
        phoff = struct.unpack_from("<I", data, 0x1C)[0]
        phentsize = struct.unpack_from("<H", data, 0x2A)[0]
        phnum = struct.unpack_from("<H", data, 0x2C)[0]
        entries = []
        for index in range(phnum):
            offset = phoff + index * phentsize
            p_type, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _flags, _align = \
                struct.unpack_from("<IIIIIIII", data, offset)
            entries.append((p_type, p_offset, p_vaddr, p_filesz))
    elif elf_class == 2:  # ELF64
        phoff = struct.unpack_from("<Q", data, 0x20)[0]
        phentsize = struct.unpack_from("<H", data, 0x36)[0]
        phnum = struct.unpack_from("<H", data, 0x38)[0]
        entries = []
        for index in range(phnum):
            offset = phoff + index * phentsize
            p_type, _flags, p_offset, p_vaddr, _paddr, p_filesz, _memsz, _align = \
                struct.unpack_from("<IIQQQQQQ", data, offset)
            entries.append((p_type, p_offset, p_vaddr, p_filesz))
    else:
        raise PineError("Unsupported ELF class")
    for p_type, p_offset, p_vaddr, p_filesz in entries:
        if p_type == 1 and vaddr >= p_vaddr and vaddr + size <= p_vaddr + p_filesz:
            start = p_offset + (vaddr - p_vaddr)
            return data[start:start + size]
    raise PineError(f"No PT_LOAD segment covers 0x{vaddr:08x} + {size}")


def compare_images(expected, observed, base_address, max_differences=16):
    """Compare two byte strings; return (matching, difference list)."""
    if len(expected) != len(observed):
        raise PineError("Image sizes differ")
    differences = []
    matching = 0
    for index, (left, right) in enumerate(zip(expected, observed)):
        if left == right:
            matching += 1
        elif len(differences) < max_differences:
            differences.append((base_address + index, left, right))
    return matching, differences


def command_status(client, _arguments):
    names = STATUS_NAMES.get(client.status(), "unknown")
    print(f"status: {names}")
    return 0


def command_info(client, _arguments):
    print(f"version: {client.version()}")
    print(f"status:  {STATUS_NAMES.get(client.status(), 'unknown')}")
    try:
        print(f"title:   {client.title()}")
        print(f"serial:  {client.serial()}")
    except PineError as error:
        print(f"game:    {error}")
    return 0


def command_read(client, arguments):
    address = int(arguments.address, 0)
    size = int(arguments.size, 0)
    output = Path(arguments.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    data = client.read_bytes(address, size)
    output.write_bytes(data)
    print(f"read {len(data)} bytes from 0x{address:08x} into {output}")
    return 0


def command_verify_elf(client, arguments):
    vaddr = int(arguments.vaddr, 0)
    size = int(arguments.size, 0)
    expected = read_elf_segment(arguments.elf, vaddr, size)
    observed = client.read_bytes(vaddr, size)
    matching, differences = compare_images(expected, observed, vaddr)
    print(f"compared {size} bytes at 0x{vaddr:08x}: {matching} matching, "
          f"{size - matching} differing")
    for address, left, right in differences:
        print(f"  first differences: 0x{address:08x}: "
              f"expected {left:02x}, observed {right:02x}")
    if arguments.report:
        report = Path(arguments.report)
        report.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"vaddr=0x{vaddr:08x} size={size} matching={matching} "
                 f"differing={size - matching}"]
        lines += [f"0x{address:08x} expected={left:02x} observed={right:02x}"
                  for address, left, right in differences]
        report.write_text("\n".join(lines) + "\n", encoding="utf-8")
        print(f"report written to {report}")
    return 0 if matching == size else 1


def command_save_state(client, arguments):
    client.save_state(arguments.slot)
    print(f"savestate saved to slot {arguments.slot}")
    return 0


def command_load_state(client, arguments):
    client.load_state(arguments.slot)
    print(f"savestate loaded from slot {arguments.slot}")
    return 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    subparsers = parser.add_subparsers(dest="command", required=True)

    subparsers.add_parser("status", help="Print the emulator status")

    subparsers.add_parser("info", help="Version, status, title and serial")

    read_parser = subparsers.add_parser("read", help="Read EE memory into a file")
    read_parser.add_argument("address")
    read_parser.add_argument("size")
    read_parser.add_argument("output")

    save_parser = subparsers.add_parser("save-state", help="Save a savestate to a slot")
    save_parser.add_argument("slot", type=int)

    load_parser = subparsers.add_parser(
        "load-state", help="Load a savestate from a slot (changes emulator state)")
    load_parser.add_argument("slot", type=int)

    verify_parser = subparsers.add_parser(
        "verify-elf", help="Compare a local ELF segment against live EE memory")
    verify_parser.add_argument("elf")
    verify_parser.add_argument("vaddr")
    verify_parser.add_argument("size")
    verify_parser.add_argument("--report")

    arguments = parser.parse_args(argv)
    handlers = {
        "status": command_status,
        "info": command_info,
        "read": command_read,
        "verify-elf": command_verify_elf,
        "save-state": command_save_state,
        "load-state": command_load_state,
    }
    try:
        with PineClient(arguments.host, arguments.port) as client:
            return handlers[arguments.command](client, arguments)
    except PineError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
