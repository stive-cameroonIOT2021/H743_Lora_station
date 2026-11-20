"""Exercise the production SX1278 driver with a real 64-byte SPI-host limit.

No radio, serial port, key or target hardware is accessed. The fake SPI host
models register accesses and the persistent 256-byte SX1278 FIFO pointer.
Run: python tests/test_radio_native.py
"""
import ctypes
import math
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

HERE = Path(__file__).resolve().parent
Byte = ctypes.c_ubyte


class NativeRadio(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = Path(os.environ.get("VFPR_HOST_GCC", r"C:\msys64\mingw64\bin\gcc.exe" if os.name == "nt" else "gcc"))
        cls.build_directory = tempfile.TemporaryDirectory(prefix="vfpr-radio-native-")
        output = Path(cls.build_directory.name) / ("radio.dll" if os.name == "nt" else "radio.so")
        arguments = [str(gcc), "-shared", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                     "-I" + str(HERE / "radio_stubs"), "-I" + str(HERE.parent / "main"),
                     str(HERE / "radio_harness.c"), "-o", str(output), "-lm"]
        if os.name != "nt": arguments.insert(2, "-fPIC")
        environment = os.environ.copy()
        environment["PATH"] = str(gcc.parent) + os.pathsep + environment["PATH"]
        build = subprocess.run(arguments, env=environment, capture_output=True, text=True)
        if build.returncode: raise RuntimeError(build.stdout + build.stderr)
        cls.directory = os.add_dll_directory(str(gcc.parent)) if os.name == "nt" else None
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.host_transfer.argtypes = [ctypes.c_int, Byte, ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.host_transmit.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        cls.lib.host_prepare_packet.argtypes = [ctypes.c_void_p, ctypes.c_size_t, Byte]
        cls.lib.host_poll.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_size_t)]
        cls.lib.host_copy_fifo.argtypes = [ctypes.c_void_p]
        cls.lib.host_copy_registers.argtypes = [ctypes.c_void_p]
        cls.lib.host_set_pointer.argtypes = [Byte]
        cls.lib.host_fail_call.argtypes = [ctypes.c_uint]

    @classmethod
    def tearDownClass(cls):
        if os.name == "nt":
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(cls.lib._handle))
            cls.directory.close()
        cls.build_directory.cleanup()

    def setUp(self):
        self.lib.host_reset()

    @staticmethod
    def payload(length):
        return (Byte * length)(*((index * 173 + 29) & 255 for index in range(length)))

    def test_fifo_write_and_read_across_cpu_limit(self):
        # 63 bytes exactly fits including address; 64 first exceeded old code.
        # 72 is an encrypted ACK, 248 our largest frame, 255 the radio limit.
        for length in (1, 62, 63, 64, 72, 126, 127, 192, 248, 252, 253, 255):
            with self.subTest(length=length):
                self.lib.host_reset()
                payload = self.payload(length)
                self.lib.host_set_pointer(211)
                self.assertEqual(self.lib.host_transfer(1, 0, payload, length), 0)
                self.assertEqual(self.lib.host_fifo_calls(), math.ceil(length / 63))
                self.assertLessEqual(self.lib.host_maximum_wire_bytes(), 64)
                self.lib.host_set_pointer(211)
                received = (Byte * length)()
                self.assertEqual(self.lib.host_transfer(0, 0, received, length), 0)
                self.assertEqual(bytes(received), bytes(payload))
                self.assertEqual(self.lib.host_get_pointer(), (211 + length) & 255)

    def test_public_receive_full_packet_with_wrapped_fifo(self):
        for length in (63, 64, 72, 248, 255):
            with self.subTest(length=length):
                self.lib.host_reset()
                payload = self.payload(length)
                self.lib.host_prepare_packet(payload, length, 211)
                output = (Byte * 255)()
                count = ctypes.c_size_t()
                self.assertEqual(self.lib.host_poll(output, ctypes.byref(count)), 1)
                self.assertEqual(count.value, length)
                self.assertEqual(bytes(output[:length]), bytes(payload))
                self.assertLessEqual(self.lib.host_maximum_wire_bytes(), 64)

    def test_public_transmit_full_packet(self):
        for length in (63, 64, 72, 248, 255):
            with self.subTest(length=length):
                self.lib.host_reset()
                payload = self.payload(length)
                self.assertEqual(self.lib.host_transmit(payload, length), 0)
                stored = (Byte * 256)()
                self.lib.host_copy_fifo(stored)
                self.assertEqual(bytes(stored[:length]), bytes(payload))
                registers = (Byte * 128)()
                self.lib.host_copy_registers(registers)
                self.assertEqual(registers[0x22], length)
                self.assertLessEqual(self.lib.host_maximum_wire_bytes(), 64)

    def test_register_block_does_not_turn_into_fifo(self):
        values = self.payload(3)
        self.assertEqual(self.lib.host_transfer(1, 6, values, 3), 0)
        output = (Byte * 3)()
        self.assertEqual(self.lib.host_transfer(0, 6, output, 3), 0)
        self.assertEqual(bytes(output), bytes(values))
        self.assertEqual(self.lib.host_fifo_calls(), 0)

    def test_large_register_block_rejected_before_io(self):
        self.assertEqual(self.lib.host_transfer(1, 6, self.payload(64), 64), 0x104)
        self.assertEqual(self.lib.host_spi_calls(), 0)

    def test_payload_over_radio_limit_rejected_before_io(self):
        self.assertEqual(self.lib.host_transfer(1, 0, self.payload(256), 256), 0x104)
        self.assertEqual(self.lib.host_transmit(self.payload(256), 256), 0x104)
        self.assertEqual(self.lib.host_spi_calls(), 0)

    def test_failed_write_chunk_returns_error_and_stops(self):
        self.lib.host_fail_call(2)
        self.assertEqual(self.lib.host_transfer(1, 0, self.payload(255), 255), -1)
        self.assertEqual(self.lib.host_spi_calls(), 2)
        self.assertEqual(self.lib.host_get_pointer(), 63)

    def test_failed_read_chunk_does_not_copy_unread_bytes(self):
        payload = self.payload(255)
        self.lib.host_prepare_packet(payload, 255, 0)
        self.lib.host_set_pointer(0)
        output = (Byte * 255)(*([0xcd] * 255))
        self.lib.host_fail_call(2)
        self.assertEqual(self.lib.host_transfer(0, 0, output, 255), -1)
        self.assertEqual(bytes(output[:63]), bytes(payload[:63]))
        self.assertEqual(bytes(output[63:]), b"\xcd" * (255 - 63))
        self.assertEqual(self.lib.host_spi_calls(), 2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
