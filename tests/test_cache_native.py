"""Compile and exercise the production ESP cache against Python wire vectors.

Run on Windows from the ESP project: python tests/test_cache_native.py
No radio, key, or ESP hardware is accessed. Only the mutex/clock SDK calls are
stubbed; production telemetry_cache.c and ESP-IDF's actual cJSON.c are compiled.
"""
import ctypes
import json
import math
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = Path(__file__).resolve().parent


def find_python_project():
    """Support both workspace staging and the sibling deployed ESP project."""
    override = os.environ.get("VFPR_PYTHON_DIR")
    if override:
        candidates = [Path(override).expanduser().resolve()]
    else:
        candidates = [candidate for base in (HERE, *HERE.parents)
                      for candidate in (base / "python/myFpv/vfpr", base / "Python/python/myFpv/vfpr")]
    for candidate in candidates:
        if (candidate / "lora_link/telemetry.py").is_file():
            return candidate
    raise RuntimeError("Cannot find canonical VFPR Python sources. Set VFPR_PYTHON_DIR to the directory containing lora_link.")


sys.path.insert(0, str(find_python_project()))
from lora_link.telemetry import SCHEMAS, BANKS, encode_records, decode_records, merge_patch, _encode_record
from lora_link.protocol import encode_event, encode_result


def set_path(item, path, value):
    parent = item
    for index, key in enumerate(path[:-1]):
        if key not in parent:
            parent[key] = [] if isinstance(path[index + 1], int) else {}
        parent = parent[key]
    key = path[-1]
    if isinstance(key, int):
        while len(parent) <= key:
            parent.append(None)
    parent[key] = value


def full_snapshot():
    result = {}
    for number, (name, fields) in SCHEMAS.items():
        if number == 25:
            continue
        item = {}
        for index, (path, kind) in enumerate(fields):
            # Deliberate nulls, true zero, maximum unsigned, signed negative,
            # doubles and non-ASCII strings expose packing/conversion errors.
            if path == ("mode",): value = "STABILIZE"
            elif path == ("link", "status"): value = "connected"
            elif path == ("_air_link_status",): value = "connected"
            elif path == ("_radio_period_s",): value = 5.0
            elif path == ("_air_ack_age_s",): value = 0.25
            elif path == ("link", "heartbeat_age_s"): value = 0.5
            elif kind == "?": value = False
            elif kind.startswith("s"): value = "value"
            elif kind == "d": value = 34.123456789123 + index
            elif kind == "i": value = -2147483648 + index
            elif kind == "I": value = 4294967295 - index
            elif kind == "H": value = 1500 + index
            elif kind == "B": value = index % 8
            else: value = 0.0 if index % 4 == 0 else (None if index % 4 == 1 else index + 0.125)
            set_path(item, path, value)
        if name == "metadata": result.update(item)
        else:
            item.update(age_s=1.25, stale=False)
            if name in BANKS:
                item["id"] = 0 if name == "batteries" else 1
                result[name] = [item]
            else: result[name] = item
    return result


class NativeCache(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        gcc = Path(os.environ.get("VFPR_HOST_GCC", r"C:\msys64\mingw64\bin\gcc.exe"))
        sdk = Path(os.environ.get("IDF_PATH", r"C:\Users\Administrator\esp\v5.5.1\esp-idf"))
        cjson = sdk / "components/json/cJSON"
        cls.build_directory = tempfile.TemporaryDirectory(prefix="vfpr-cache-native-")
        output = Path(cls.build_directory.name) / "telemetry_cache_native.dll"
        arguments = [str(gcc), "-shared", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1", "-g",
                     "-I" + str(HERE / "stubs"), "-I" + str(HERE.parent / "main"), "-I" + str(cjson),
                     str(HERE / "cache_harness.c"), str(HERE.parent / "main/telemetry_cache.c"),
                     str(cjson / "cJSON.c"), "-o", str(output)]
        environment = os.environ.copy()
        environment["PATH"] = str(gcc.parent) + os.pathsep + environment["PATH"]
        build = subprocess.run(arguments, env=environment, capture_output=True, text=True)
        if build.returncode:
            raise RuntimeError(build.stdout + build.stderr)
        cls.directory = os.add_dll_directory(str(gcc.parent))
        cls.lib = ctypes.CDLL(str(output))
        cls.lib.host_init.restype = ctypes.c_int
        cls.lib.host_reset.argtypes = [ctypes.c_uint64, ctypes.c_int64]
        cls.lib.host_accept.argtypes = [ctypes.c_uint, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_uint, ctypes.c_int64]
        cls.lib.host_accept.restype = ctypes.c_int
        cls.lib.host_snapshot.argtypes = [ctypes.c_int64]
        cls.lib.host_snapshot.restype = ctypes.c_void_p
        cls.lib.host_live_snapshot.argtypes = [ctypes.c_int64]
        cls.lib.host_live_snapshot.restype = ctypes.c_void_p
        cls.lib.host_free.argtypes = [ctypes.c_void_p]
        cls.lib.host_fail_at.argtypes = [ctypes.c_long]
        cls.lib.host_live_allocations.restype = ctypes.c_long
        cls.lib.host_allocation_calls.restype = ctypes.c_long
        assert cls.lib.host_init() == 0

    @classmethod
    def tearDownClass(cls):
        # Windows holds loaded DLLs open. Release it before removing the
        # temporary build, keeping generated binaries out of the project.
        ctypes.windll.kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
        ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(cls.lib._handle))
        cls.directory.close()
        cls.build_directory.cleanup()

    def setUp(self):
        self.lib.host_reset(17, 1000000)

    def accept(self, payload, *, kind=1, now=2000000, session=17):
        storage = ctypes.create_string_buffer(payload)
        return self.lib.host_accept(kind, session, storage, len(payload), now)

    def snapshot(self, now=2000000):
        pointer = self.lib.host_snapshot(now)
        self.assertTrue(pointer)
        try: return json.loads(ctypes.string_at(pointer))
        finally: self.lib.host_free(pointer)

    def live_snapshot(self, now=2000000):
        pointer = self.lib.host_live_snapshot(now)
        self.assertTrue(pointer)
        try: return json.loads(ctypes.string_at(pointer))
        finally: self.lib.host_free(pointer)

    def populate_all_pages(self):
        for page in ("overview", "sensors", "motors", "health"):
            for packet in encode_records(full_snapshot(), page):
                self.assertEqual(self.accept(packet), 0)

    def test_live_projection_matches_full_age_session_and_core_fields(self):
        self.populate_all_pages()
        self.assertEqual(self.accept(encode_event(9, 4, .5, "test warning"), kind=4), 0)
        full = self.snapshot(22000000)
        live = self.live_snapshot(22000000)
        expected = {"session", "target", "mode", "armed", "link", "elapsed_s", "received_messages",
                    "bad_data_messages", "_radio_period_s", "_air_ack_age_s", "_air_rtt_ms", "_air_link_status",
                    "attitude", "battery", "gps", "position", "motion", "rc", "imus", "status_text", "cache"}
        self.assertEqual(set(live), expected)
        self.assertEqual(live, {key: full[key] for key in expected})
        self.assertTrue(live["attitude"]["stale"])
        self.assertEqual(live["status_text"][0]["text"], "test warning")
        self.assertNotIn("_cache_received_us", json.dumps(live))
        self.assertNotIn("_cache_metadata_us", live)
        for key in ("outputs", "esc", "health", "batteries", "last_result", "navigation"):
            self.assertNotIn(key, live)

    def test_live_missing_inputs_and_restart_are_explicit(self):
        for snapshot in (self.live_snapshot(),):
            for key in ("attitude", "battery", "gps", "position", "motion", "rc"):
                self.assertIsNone(snapshot[key])
            self.assertEqual(snapshot["imus"], [])
            self.assertEqual(snapshot["status_text"], [])
        self.assertEqual(self.accept(_encode_record(2, 0, {"roll_deg": 0})), 0)
        self.assertEqual(self.live_snapshot()["attitude"]["roll_deg"], 0)
        self.lib.host_reset(18, 6000000)
        restarted = self.live_snapshot(7000000)
        self.assertEqual(restarted["session"], "0000000000000012")
        self.assertIsNone(restarted["attitude"])
        self.assertEqual(restarted["cache"]["session_age_s"], 1)

    def test_live_projection_uses_fewer_allocations_and_leaves_source_ages_unchanged(self):
        self.populate_all_pages()
        self.lib.host_fail_at(0)
        full = self.snapshot()
        full_calls = self.lib.host_allocation_calls()
        self.lib.host_fail_at(0)
        live = self.live_snapshot()
        live_calls = self.lib.host_allocation_calls()
        self.assertLess(live_calls, full_calls, "The fast path must not clone then discard the full cache")
        self.assertLess(len(json.dumps(live)), len(json.dumps(full)))
        self.live_snapshot(22000000)
        later_full = self.snapshot(3000000)
        self.assertAlmostEqual(later_full["attitude"]["age_s"], 2.25)
        self.assertFalse(later_full["attitude"]["stale"])

    def test_live_snapshot_allocation_failures_do_not_leak_or_mutate_cache(self):
        self.populate_all_pages()
        baseline = self.lib.host_live_allocations()
        for index in range(1, 1000):
            self.lib.host_fail_at(index)
            pointer = self.lib.host_live_snapshot(22000000)
            self.lib.host_fail_at(0)
            if pointer: self.lib.host_free(pointer)
            self.assertEqual(self.lib.host_live_allocations(), baseline, f"live snapshot leak at {index}")
            if pointer: break
        else:
            self.fail("No successful compact snapshot after 999 allocation sites")
        self.assertEqual(self.snapshot()["attitude"]["roll_deg"], 0)
        self.assertFalse(self.snapshot()["attitude"]["stale"])

    def assert_tree(self, actual, expected):
        if isinstance(expected, dict):
            for key, value in expected.items():
                self.assertIn(key, actual)
                self.assert_tree(actual[key], value)
        elif isinstance(expected, list):
            self.assertEqual(len(actual), len(expected))
            for left, right in zip(actual, expected): self.assert_tree(left, right)
        elif isinstance(expected, float): self.assertAlmostEqual(actual, expected, delta=max(1e-7, abs(expected) * 1e-12))
        else: self.assertEqual(actual, expected)

    def test_all_pages_match_python_decoder(self):
        source = full_snapshot()
        expected = {}
        for page in ("overview", "sensors", "motors", "health"):
            for packet in encode_records(source, page):
                self.assertLessEqual(len(packet), 192)
                merge_patch(expected, decode_records(packet))
                self.assertEqual(self.accept(packet), 0)
        self.assert_tree(self.snapshot(), expected)
        self.assertFalse(self.snapshot()["armed"])
        self.assertEqual(self.snapshot()["attitude"]["roll_deg"], 0)
        self.assertIsNone(self.snapshot()["attitude"]["pitch_deg"])

    def test_bank_merge_order_and_explicit_empty(self):
        self.assertEqual(self.accept(_encode_record(8, 3, {"temperature_c": 35})), 0)
        self.assertEqual(self.accept(_encode_record(8, 1, {"temperature_c": 0})), 0)
        self.assertEqual([item["id"] for item in self.snapshot()["imus"]], [1, 3])
        self.assertEqual(self.accept(_encode_record(8, 1, {"temperature_c": None})), 0)
        self.assertIsNone(self.snapshot()["imus"][0]["temperature_c"])
        self.assertEqual(self.accept(_encode_record(8, 255, None)), 0)
        self.assertEqual(self.snapshot()["imus"], [])

    def test_freshness_and_return_path_ages(self):
        source = full_snapshot()
        self.assertEqual(self.accept(encode_records(source)[0]), 0)
        output = self.snapshot(22000000)
        self.assertAlmostEqual(output["attitude"]["age_s"], 21.25)
        self.assertTrue(output["attitude"]["stale"])
        self.assertAlmostEqual(output["link"]["heartbeat_age_s"], 20.5)
        self.assertEqual(output["link"]["status"], "heartbeat_lost")
        self.assertEqual(output["_air_link_status"], "degraded")
        self.assertEqual(self.snapshot(50000000)["_air_link_status"], "lost")
        self.assertAlmostEqual(self.snapshot(3000000)["link"]["heartbeat_age_s"], 1.5)

    def test_expired_and_unsupported_results_do_not_apply_records(self):
        for status in (1, 2):
            packet = encode_result(42, 2, status, _encode_record(2, 0, {"roll_deg": 99}))
            self.assertEqual(self.accept(packet, kind=3), 0)
            output = self.snapshot(5000000)
            self.assertIsNone(output["attitude"])
            self.assertEqual(output["last_result"]["status"], status)
            self.assertEqual(output["last_result"]["request_id"], 42)
            self.assertEqual(output["last_result"]["age_s"], 3)
        self.assertEqual(self.accept(encode_result(43, 2, 0, _encode_record(2, 0, {"roll_deg": 99})), kind=3), 0)
        self.assertEqual(self.snapshot()["attitude"]["roll_deg"], 99)

    def test_event_utf8_age_dedup_and_bound(self):
        for index in range(7):
            packet = encode_event(index, 2, 0.75, "GPS: αβγ\nfix")
            self.assertEqual(self.accept(packet, kind=4), 0)
        output = self.snapshot(4000000)
        self.assertEqual([event["event_id"] for event in output["status_text"]], [3, 4, 5, 6])
        self.assertEqual(output["status_text"][-1]["text"], "GPS: αβγ fix")
        self.assertEqual(output["status_text"][-1]["age_s"], 2.75)
        self.assertEqual(self.accept(encode_event(0, 1, 0, "duplicate"), kind=4), 0)
        self.assertEqual([event["event_id"] for event in self.snapshot()["status_text"]], [3, 4, 5, 6])
        self.assertNotEqual(self.accept(struct.pack("<IBI", 33, 1, 0) + b"\xc0\xaf", kind=4), 0)

    def test_atomic_rejection_of_bad_typed_records(self):
        initial = _encode_record(2, 0, {"roll_deg": 10})
        self.assertEqual(self.accept(initial), 0)
        valid = _encode_record(2, 0, {"roll_deg": 20})
        cases = [b"\x02", _encode_record(3, 0, {"latitude_deg": 23})[:-1],
                 struct.pack("<BBHBII", 2, 0, 13, 1, 0, 1) + struct.pack("<f", math.nan),
                 struct.pack("<BBHBII", 2, 0, 13, 1, 0, 1) + struct.pack("<f", math.inf),
                 struct.pack("<BBHBII", 2, 0, 9, 1, 0, 1 << 20),
                 struct.pack("<BBHBII", 2, 0, 9, 4, 0, 0),
                 struct.pack("<BBHBII", 25, 0, 10, 1, 0, 1) + b"\x02",
                 struct.pack("<BBHBII", 1, 0, 10, 1, 0, 4) + b"\xff",
                 _encode_record(8, 250, {"temperature_c": 99}),
                 struct.pack("<BBHBII", 2, 0, 10, 0, 0, 0) + b"\x00"]
        for malformed in cases:
            self.assertNotEqual(self.accept(valid + malformed), 0, malformed.hex())
            self.assertEqual(self.snapshot()["attitude"]["roll_deg"], 10)
        self.assertEqual(self.snapshot()["cache"]["invalid_frames"], len(cases))

    def test_unknown_record_skipped_and_summary_wins(self):
        source = full_snapshot()
        summary = encode_records(source)[0]
        metadata = _encode_record(1, 0, {"mode": "OLD", "_air_link_status": None})
        # Match canonical merge preference despite metadata being later.
        from lora_link.telemetry import RECORD_HEADER
        _, _, length = RECORD_HEADER.unpack_from(summary)
        summary = summary[:length + 4]
        packet = struct.pack("<BBH", 99, 12, 3) + b"abc" + summary + metadata
        self.assertEqual(self.accept(packet), 0)
        self.assertEqual(self.snapshot()["mode"], "STABILIZE")
        self.assertEqual(self.snapshot()["_air_link_status"], "connected")

    def test_session_reset_clears_readings_events_and_results(self):
        self.assertEqual(self.accept(_encode_record(2, 0, {"roll_deg": 2})), 0)
        self.lib.host_reset(18, 6000000)
        output = self.snapshot(7000000)
        self.assertIsNone(output["attitude"])
        self.assertEqual(output["session"], "0000000000000012")
        self.assertEqual(output["cache"]["accepted_frames"], 0)
        self.assertEqual(output["cache"]["session_age_s"], 1)
        self.assertNotEqual(self.accept(_encode_record(2, 0, {"roll_deg": 99})), 0)
        self.assertIsNone(self.snapshot()["attitude"])

    def test_concurrent_snapshots_and_updates(self):
        packet = encode_records(full_snapshot())[0]
        failures = []
        def reader():
            try:
                for _ in range(200):
                    output = self.snapshot(3000000)
                    if output["attitude"] is not None:
                        assert output["attitude"]["roll_deg"] == 0
            except BaseException as error: failures.append(error)
        threads = [threading.Thread(target=reader) for _ in range(3)]
        for thread in threads: thread.start()
        for _ in range(200): self.assertEqual(self.accept(packet), 0)
        for thread in threads: thread.join()
        self.assertEqual(failures, [])

    def test_allocation_failures_leave_cache_unchanged_without_leaks(self):
        initial = _encode_record(2, 0, {"roll_deg": 10})
        update = encode_records(full_snapshot())[0]
        for fail_at in range(1, 1000):
            self.lib.host_reset(17, 1000000)
            self.assertEqual(self.accept(initial), 0)
            live = self.lib.host_live_allocations()
            self.lib.host_fail_at(fail_at)
            error = self.accept(update)
            self.lib.host_fail_at(0)
            if error == 0:
                break
            self.assertEqual(error, 0x101, fail_at)
            self.assertEqual(self.lib.host_live_allocations(), live, f"allocation leak at {fail_at}")
            self.assertEqual(self.snapshot()["attitude"]["roll_deg"], 10, fail_at)
        else:
            self.fail("No successful allocation sweep after 999 allocation sites")


if __name__ == "__main__":
    unittest.main(verbosity=2)
