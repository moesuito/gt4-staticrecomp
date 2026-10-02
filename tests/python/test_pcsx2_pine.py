"""Offline checks for the PINE client: encoding, replies, chunking, ELF, server path."""

import socket
import struct
import sys
import threading
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "scripts"))

import pcsx2_pine as pine


class ProtocolTests(unittest.TestCase):
    def test_build_request_prefixes_total_size(self):
        request = pine.build_request([pine.encode_read64(0x00100000)])
        self.assertEqual(struct.unpack_from("<I", request, 0)[0], len(request))
        self.assertEqual(request[4], pine.READ64)
        self.assertEqual(request[5:9], struct.pack("<I", 0x00100000))

    def test_parse_reply_ok_and_fail(self):
        ok = struct.pack("<I", 10) + bytes([pine.PINE_OK]) + b"12345"
        self.assertEqual(pine.parse_reply(ok), b"12345")
        fail = struct.pack("<I", 5) + bytes([pine.PINE_FAIL])
        with self.assertRaises(pine.PineError):
            pine.parse_reply(fail)
        with self.assertRaises(pine.PineError):
            pine.parse_reply(b"\x05")

    def test_plan_read_chunks_aligns_and_batches(self):
        start, chunks = pine.plan_read_chunks(0x00100001, 15)
        self.assertEqual(start, 0x00100000)
        self.assertEqual(chunks, [(0x00100000, 2)])
        start, chunks = pine.plan_read_chunks(0x00100000, 40, reads_per_request=2)
        self.assertEqual(start, 0x00100000)
        self.assertEqual(chunks, [(0x00100000, 2), (0x00100010, 2), (0x00100020, 1)])

    def test_string_parsing(self):
        payload = struct.pack("<I", 4) + b"abc\x00"
        self.assertEqual(pine.parse_string(payload), "abc")

    def test_state_commands_encode_slot(self):
        self.assertEqual(pine.encode_save_state(3), bytes([0x09, 3]))
        self.assertEqual(pine.encode_load_state(9), bytes([0x0A, 9]))

    def test_compare_images_reports_differences(self):
        matching, differences = pine.compare_images(b"\x00\x01\x02", b"\x00\xff\x02", 0x1000)
        self.assertEqual(matching, 2)
        self.assertEqual(differences, [(0x1001, 0x01, 0xFF)])


class ElfTests(unittest.TestCase):
    def make_elf32(self, tmp_path):
        ident = b"\x7fELF" + bytes([1, 1, 1]) + bytes(9)
        header = struct.pack("<HHIIIIIHHHHHH", 2, 8, 1, 0x1000, 52, 0, 0, 52, 32, 1, 0, 0, 0)
        program = struct.pack("<IIIIIIII", 1, 0x100, 0x1000, 0x1000, 16, 16, 5, 0x1000)
        data = bytearray(0x100 + 16)
        data[:52] = ident + header
        data[52:84] = program
        data[0x100:0x110] = bytes(range(16))
        path = tmp_path / "synthetic.elf"
        path.write_bytes(bytes(data))
        return path

    def test_extracts_the_segment_bytes(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            path = self.make_elf32(Path(folder))
            self.assertEqual(pine.read_elf_segment(path, 0x1004, 4), bytes([4, 5, 6, 7]))

    def test_missing_segment_is_an_error(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            path = self.make_elf32(Path(folder))
            with self.assertRaises(pine.PineError):
                pine.read_elf_segment(path, 0x9000, 4)


class FakePineServer(threading.Thread):
    """Speaks just enough PINE to serve deterministic memory to the client."""

    PATTERN = bytes((index * 7) & 0xFF for index in range(0x4000))

    def __init__(self):
        super().__init__(daemon=True)
        self._listener = socket.socket()
        self._listener.bind(("127.0.0.1", 0))
        self._listener.listen(1)
        self.port = self._listener.getsockname()[1]
        self.request_count = 0
        self.saved_slots = []
        self.loaded_slots = []

    def run(self):
        connection, _ = self._listener.accept()
        with connection:
            while True:
                header = self._recv(connection, 4)
                if len(header) < 4:
                    return
                total = struct.unpack_from("<I", header, 0)[0]
                body = self._recv(connection, total - 4)
                if len(body) < total - 4:
                    return
                self.request_count += 1
                payload = self._handle(body)
                reply = (struct.pack("<I", 5 + len(payload))
                         + bytes([pine.PINE_OK]) + payload)
                connection.sendall(reply)

    @staticmethod
    def _recv(connection, count):
        data = b""
        while len(data) < count:
            block = connection.recv(count - len(data))
            if not block:
                break
            data += block
        return data

    def _handle(self, body):
        payload = bytearray()
        index = 0
        while index < len(body):
            opcode = body[index]
            index += 1
            if opcode == pine.READ64:
                address = struct.unpack_from("<I", body, index)[0]
                index += 4
                payload += self.PATTERN[address:address + 8]
            elif opcode == pine.MSG_STATUS:
                payload += struct.pack("<I", 0)
            elif opcode == pine.MSG_SAVE_STATE:
                self.saved_slots.append(body[index])
                index += 1
            elif opcode == pine.MSG_LOAD_STATE:
                self.loaded_slots.append(body[index])
                index += 1
            else:
                raise AssertionError(f"unexpected opcode {opcode}")
        return bytes(payload)


class ClientTests(unittest.TestCase):
    def test_reads_match_the_served_pattern(self):
        server = FakePineServer()
        server.start()
        with pine.PineClient(port=server.port) as client:
            self.assertEqual(client.status(), 0)
            self.assertEqual(client.read_bytes(0x100, 32), FakePineServer.PATTERN[0x100:0x120])
            # An unaligned request slices the aligned words correctly.
            self.assertEqual(client.read_bytes(0x1003, 5), FakePineServer.PATTERN[0x1003:0x1008])
            # Larger reads batch many commands into few requests.
            big = client.read_bytes(0x1000, 4096)
            self.assertEqual(big, FakePineServer.PATTERN[0x1000:0x2000])
        self.assertEqual(server.request_count, 4)

    def test_connection_failure_is_a_clean_error(self):
        with self.assertRaises(pine.PineError):
            pine.PineClient(port=1)  # nothing listens on port 1

    def test_state_slot_commands_round_trip(self):
        server = FakePineServer()
        server.start()
        with pine.PineClient(port=server.port) as client:
            client.save_state(9)
            client.load_state(3)
        self.assertEqual(server.saved_slots, [9])
        self.assertEqual(server.loaded_slots, [3])


if __name__ == "__main__":
    unittest.main()
