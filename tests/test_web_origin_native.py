"""Compile and exercise the production Host/Origin allowlist, without hardware."""
import ctypes
import os
from pathlib import Path
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent


class WebOriginTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = Path(os.environ.get("VFPR_HOST_GCC", r"C:\msys64\mingw64\bin\gcc.exe" if os.name == "nt" else "gcc"))
        cls.build_directory = tempfile.TemporaryDirectory(prefix="vfpr-web-origin-")
        output = Path(cls.build_directory.name) / ("origin.dll" if os.name == "nt" else "origin.so")
        command = [str(gcc), "-shared", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
                   str(HERE.parent / "main" / "web_origin.c"), "-o", str(output)]
        if os.name != "nt": command.insert(2, "-fPIC")
        environment = os.environ.copy()
        environment["PATH"] = str(gcc.parent) + os.pathsep + environment["PATH"]
        subprocess.run(command, env=environment, capture_output=True, text=True, check=True)
        cls.directory = os.add_dll_directory(str(gcc.parent)) if os.name == "nt" else None
        cls.library = ctypes.CDLL(str(output))
        cls.check = cls.library.web_origin_allowed
        cls.check.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_char_p]
        cls.check.restype = ctypes.c_bool

    @classmethod
    def tearDownClass(cls):
        if os.name == "nt":
            ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(cls.library._handle))
            cls.directory.close()
        cls.build_directory.cleanup()

    def allowed(self, host, origin, site="same-origin"):
        encode = lambda value: None if value is None else value.encode("ascii")
        return self.check(encode(host), encode(origin), encode(site))

    def test_both_local_entry_points_and_default_port_are_supported(self):
        for name in ("espground.local", "192.168.4.1"):
            for host in (name, name + ":80"):
                for origin in ("http://" + name, "http://" + name + ":80"):
                    with self.subTest(host=host, origin=origin):
                        self.assertTrue(self.allowed(host, origin))
                        self.assertTrue(self.allowed(host, origin, None))

    def test_dns_case_is_insensitive(self):
        self.assertTrue(self.allowed("ESPGROUND.LOCAL:80", "http://EspGround.Local"))

    def test_cross_authority_and_unrelated_hosts_are_rejected(self):
        for host, origin in (
            ("espground.local", "http://192.168.4.1"),
            ("192.168.4.1", "http://espground.local"),
            ("evil.example", "http://evil.example"),
            ("espground.local.evil.example", "http://espground.local.evil.example"),
            ("espground.local@evil.example", "http://espground.local@evil.example"),
        ):
            with self.subTest(host=host, origin=origin): self.assertFalse(self.allowed(host, origin))

    def test_malformed_hosts_origins_and_nondefault_ports_are_rejected(self):
        for host in (None, "", " espground.local", "espground.local ", "espground.local.",
                     "espground.local:81", "espground.local:080", "espground.local/path", "192.168.4.1:8080"):
            with self.subTest(host=host): self.assertFalse(self.allowed(host, "http://espground.local"))
        for origin in (None, "", "null", "https://espground.local", "http://espground.local:81",
                       "http://espground.local/", "http://espground.local?x=1", "http://espground.local#x",
                       "http://user@espground.local", " http://espground.local", "http://espground.local\r\n"):
            with self.subTest(origin=origin): self.assertFalse(self.allowed("espground.local", origin))

    def test_fetch_metadata_cannot_override_origin_validation(self):
        for site in ("cross-site", "same-site", "none", "", "Same-Origin"):
            with self.subTest(site=site):
                self.assertFalse(self.allowed("espground.local", "http://espground.local", site))
        self.assertFalse(self.allowed("espground.local", "http://evil.example", "same-origin"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
