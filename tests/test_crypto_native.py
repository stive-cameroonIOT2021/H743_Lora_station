"""Compile production codec and crypto with ESP-IDF's actual mbedTLS sources.

Only boot entropy hardware calls are stubbed. Python cryptography/OpenSSL is
an independent AES-256-GCM oracle. No device or private build key is accessed.
Run with a Python environment containing cryptography.
"""
import ctypes as C
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from cryptography.hazmat.primitives.ciphers.aead import AESGCM

HERE = Path(__file__).resolve().parent
HEADER = struct.Struct("<2sBBBBQIIH")
KEY = bytes(range(32))  # Public, deterministic test key only.
NONCE = bytes(range(16))


class Frame(C.Structure):
    _fields_ = [("kind", C.c_uint8), ("src", C.c_uint8), ("dst", C.c_uint8),
                ("session", C.c_uint64), ("sequence", C.c_uint32), ("reply_to", C.c_uint32),
                ("payload_len", C.c_uint16), ("payload", C.c_void_p)]


def buffer(data):
    return C.create_string_buffer(bytes(data), max(1, len(data)))


class NativeCrypto(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = Path(os.environ.get("VFPR_HOST_GCC", r"C:\msys64\mingw64\bin\gcc.exe"))
        sdk = Path(os.environ.get("IDF_PATH", r"C:\Users\Administrator\esp\v5.5.1\esp-idf"))
        mbedtls = sdk / "components/mbedtls/mbedtls"
        cls.temporary = tempfile.TemporaryDirectory(prefix="vfpr-crypto-native-")
        output = Path(cls.temporary.name) / "link_crypto_native.dll"
        sources = [HERE / "crypto_harness.c", HERE.parent / "main/link_protocol.c"]
        sources += [mbedtls / f"library/{name}.c" for name in
                    ("aes", "block_cipher", "gcm", "platform", "platform_util", "constant_time")]
        arguments = [str(gcc), "-shared", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                     '-DMBEDTLS_CONFIG_FILE="host_mbedtls_config.h"',
                     "-I" + str(HERE / "crypto_stubs"), "-I" + str(HERE.parent / "main"),
                     "-I" + str(mbedtls / "include"), "-I" + str(mbedtls / "library"),
                     *map(str, sources), "-o", str(output)]
        environment = os.environ.copy()
        environment["PATH"] = str(gcc.parent) + os.pathsep + environment["PATH"]
        build = subprocess.run(arguments, env=environment, capture_output=True, text=True)
        if build.returncode:
            cls.temporary.cleanup()
            raise RuntimeError(build.stdout + build.stderr)
        cls.dll_directory = os.add_dll_directory(str(gcc.parent))
        cls.lib = C.CDLL(str(output))
        cls.lib.host_encode.argtypes = [C.c_uint8, C.c_uint8, C.c_uint8, C.c_uint64,
            C.c_uint32, C.c_uint32, C.c_void_p, C.c_uint16, C.c_void_p, C.c_void_p,
            C.c_void_p, C.c_size_t, C.POINTER(C.c_size_t)]
        cls.lib.host_decode.argtypes = [C.c_void_p, C.c_size_t, C.c_void_p, C.c_uint8,
            C.c_uint8, C.c_void_p, C.c_size_t, C.POINTER(Frame)]
        cls.lib.link_crypto_parse_key.argtypes = [C.c_char_p, C.c_void_p]
        cls.lib.link_crypto_parse_key.restype = C.c_bool
        cls.lib.link_crypto_seal.argtypes = [C.c_void_p, C.c_void_p, C.c_void_p, C.c_size_t,
            C.c_void_p, C.c_size_t, C.c_void_p, C.c_void_p, C.c_void_p]
        cls.lib.link_crypto_seal.restype = C.c_bool
        cls.lib.link_crypto_open.argtypes = list(cls.lib.link_crypto_seal.argtypes)
        cls.lib.link_crypto_open.restype = C.c_bool
        cls.lib.link_crypto_next_nonce.argtypes = [C.c_void_p]
        cls.lib.link_crypto_next_nonce.restype = C.c_bool
        cls.lib.link_crypto_nonce_available.restype = C.c_bool
        cls.lib.link_crypto_nonces_used.restype = C.c_uint64
        cls.lib.link_crypto_session.restype = C.c_uint64
        cls.lib.host_entropy_order_ok.restype = C.c_bool

    @classmethod
    def tearDownClass(cls):
        C.windll.kernel32.FreeLibrary.argtypes = [C.c_void_p]
        C.windll.kernel32.FreeLibrary(C.c_void_p(cls.lib._handle))
        cls.dll_directory.close()
        cls.temporary.cleanup()

    def seal(self, payload, *, key=KEY, nonce=NONCE, aad=b"header"):
        ciphertext, tag = buffer(bytes(len(payload))), buffer(bytes(16))
        self.assertTrue(self.lib.link_crypto_seal(buffer(key), buffer(nonce), buffer(aad), len(aad),
            buffer(payload), len(payload), ciphertext, tag, None))
        return ciphertext.raw[:len(payload)] + tag.raw

    def encode(self, payload, *, kind=1, src=1, dst=2):
        wire, length = buffer(bytes(248)), C.c_size_t(999)
        error = self.lib.host_encode(kind, src, dst, 0x1020304050607080, 0xffffffff,
            0xabcdef01, buffer(payload), len(payload), buffer(KEY), buffer(NONCE),
            wire, len(wire), C.byref(length))
        self.assertEqual(error, 0)
        return wire.raw[:length.value]

    def decode(self, wire, *, key=KEY, capacity=192, src=1, dst=2):
        plaintext = buffer(b"\xa5" * capacity)
        frame = Frame()
        C.memset(C.byref(frame), 0xa5, C.sizeof(frame))
        error = self.lib.host_decode(buffer(wire), len(wire), buffer(key), src, dst,
                                     plaintext, capacity, C.byref(frame))
        return error, plaintext, frame

    def test_fixed_aes256_gcm_vectors_and_lengths(self):
        for length in (0, 1, 15, 16, 17, 31, 32, 191, 192):
            payload = bytes(range(length))
            for aad in (b"", bytes(range(24))):
                with self.subTest(length=length, aad=len(aad)):
                    self.assertEqual(self.seal(payload, aad=aad), AESGCM(KEY).encrypt(NONCE, payload, aad))
        # Public zero-key/zero-IV vector, with a full 128-bit IV.
        self.assertEqual(self.seal(b"", key=bytes(32), nonce=bytes(16), aad=b""),
                         bytes.fromhex("8312357a2111ea33b1b67eb2fe05fe86"))
        self.assertEqual(AESGCM(bytes(32)).encrypt(bytes(16), b"", b""),
                         bytes.fromhex("8312357a2111ea33b1b67eb2fe05fe86"))

    def test_both_wire_directions_and_plaintext_ownership(self):
        for length in (0, 1, 16, 192):
            payload = bytes(range(length))
            header = HEADER.pack(b"HL", 2, 1, 1, 2, 0x1020304050607080, 0xffffffff, 0xabcdef01, length)
            expected = header + NONCE + AESGCM(KEY).encrypt(NONCE, payload, header)
            self.assertEqual(self.encode(payload), expected)
            error, plaintext, frame = self.decode(expected)
            self.assertEqual(error, 0)
            self.assertEqual(plaintext.raw[:length], payload)
            self.assertEqual(frame.payload_len, length)
            self.assertEqual(frame.payload, C.addressof(plaintext))
            self.assertEqual((frame.session, frame.sequence, frame.reply_to),
                             (0x1020304050607080, 0xffffffff, 0xabcdef01))
            self.assertEqual(AESGCM(KEY).decrypt(NONCE, self.encode(payload)[40:], header), payload)

    def test_every_wire_bit_is_authenticated_or_rejected_and_cleared(self):
        wire = self.encode(bytes(range(192)))
        for index in range(len(wire)):
            for bit in range(8):
                corrupted = bytearray(wire)
                corrupted[index] ^= 1 << bit
                error, plaintext, frame = self.decode(corrupted)
                self.assertNotEqual(error, 0, (index, bit))
                self.assertEqual(plaintext.raw, bytes(192), (index, bit))
                self.assertEqual(bytes(frame), bytes(C.sizeof(frame)), (index, bit))

    def test_wrong_key_capacity_address_legacy_and_lengths_clear_outputs(self):
        wire = self.encode(b"secret payload")
        for changed, arguments in [(wire, {"key": bytes(32)}), (wire, {"capacity": 1}),
                (wire, {"src": 3}), (wire, {"dst": 3}), (wire + b"\0", {}),
                (wire[:2] + b"\1" + wire[3:], {})] + [(wire[:i], {}) for i in range(len(wire))]:
            error, plaintext, frame = self.decode(changed, **arguments)
            self.assertNotEqual(error, 0)
            self.assertEqual(plaintext.raw, bytes(len(plaintext)))
            self.assertEqual(bytes(frame), bytes(C.sizeof(frame)))

    def test_direct_auth_failure_clears_plaintext(self):
        payload, aad = b"never release unauthenticated data", b"header"
        sealed = AESGCM(KEY).encrypt(NONCE, payload, aad)
        wrong_tag = sealed[-16:-1] + bytes([sealed[-1] ^ 1])
        output = buffer(b"\xa5" * len(payload))
        self.assertFalse(self.lib.link_crypto_open(buffer(KEY), buffer(NONCE), buffer(aad), len(aad),
            buffer(sealed[:-16]), len(payload), buffer(wrong_tag), output, None))
        self.assertEqual(output.raw, bytes(len(payload)))

    def test_strict_key_parser(self):
        output = buffer(b"\xa5" * 32)
        for value in (None, b"", b"00" * 32, b"01" * 31, b"01" * 33,
                      b" " + b"01" * 32, b"01" * 31 + b"0g"):
            self.assertFalse(self.lib.link_crypto_parse_key(value, output))
            self.assertEqual(output.raw, bytes(32))
        for value in (KEY.hex().encode(), KEY.hex().upper().encode()):
            self.assertTrue(self.lib.link_crypto_parse_key(value, output))
            self.assertEqual(output.raw, KEY)

    def test_z_nonce_entropy_order_idempotence_and_exhaustion(self):
        nonce = buffer(bytes(16))
        self.assertFalse(self.lib.link_crypto_nonce_available())
        self.assertFalse(self.lib.link_crypto_next_nonce(nonce))
        self.lib.link_crypto_init()
        self.assertTrue(self.lib.host_entropy_order_ok())
        self.assertEqual(self.lib.link_crypto_session(), 0x1716151413121110)
        for counter in range(3):
            self.assertTrue(self.lib.link_crypto_next_nonce(nonce))
            self.assertEqual(nonce.raw, bytes(range(0x10, 0x1c)) + struct.pack("<I", counter))
            self.lib.link_crypto_init()  # Must never restart/reseed on the same boot.
        self.assertTrue(self.lib.host_entropy_order_ok())
        self.assertEqual(self.lib.link_crypto_nonces_used(), 3)
        self.lib.host_last_nonce()
        self.assertTrue(self.lib.link_crypto_next_nonce(nonce))
        self.assertEqual(nonce.raw[-4:], b"\xff" * 4)
        self.assertFalse(self.lib.link_crypto_nonce_available())
        self.assertEqual(self.lib.link_crypto_nonces_used(), 1 << 32)
        for _ in range(2):
            self.lib.link_crypto_init()
            self.assertFalse(self.lib.link_crypto_next_nonce(nonce))
            self.assertEqual(nonce.raw, bytes(16))


if __name__ == "__main__":
    unittest.main(verbosity=2)
