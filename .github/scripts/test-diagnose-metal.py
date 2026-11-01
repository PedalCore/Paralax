"""Portable tests for the CI-only contention experiment; never touches a real Metal cache."""

import contextlib
import importlib.util
import io
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


spec = importlib.util.spec_from_file_location("diagnose_metal", Path(__file__).with_name("diagnose-metal.py"))
diagnostic = importlib.util.module_from_spec(spec)
spec.loader.exec_module(diagnostic)

TARGET = "SceneRenderer: BRDF LUT survives output and internal target allocation"
CPU = "SceneSerializer: very deep hierarchies load and save without recursion or quadratic work"
REFERENCE = "SceneRenderer: fixed look matches the reviewed reference image"
LIT = "SceneRenderer: renders a lit, shadowed PBR scene without validation errors"
SHADOW = "SceneRenderer: wide shadow filters do not shadow an unobstructed plane"
PHASES = ["Alone1", "CPU1", "GPU1", "GPU2", "GPU3", "CPU2", "Alone2"]
SELECTED = [[TARGET], [TARGET, CPU], [TARGET, REFERENCE], [TARGET, REFERENCE],
            [TARGET, REFERENCE, LIT, SHADOW], [TARGET, CPU], [TARGET]]


class ContentionTests(unittest.TestCase):
    def exercise(self, fault=None, phase_index=0, config="Release", flat=False):
        with tempfile.TemporaryDirectory() as directory:
            build = Path(directory)
            binary = build / "bin" if flat else build / "bin" / config
            binary.mkdir(parents=True)
            (binary / "Paralax-Tests").touch()
            output = build / "Paralax-Tests" / "TestOutput"
            output.mkdir(parents=True)
            logs = build / "Testing" / "Temporary"
            logs.mkdir(parents=True)
            for image in diagnostic.IMAGES:
                (output / image).write_text("baseline")
            (logs / "LastTest.log").write_text("baseline")
            (logs / "CTestCostData.txt").write_text("baseline costs")
            calls, clears = [], []

            def clear():
                index = len(clears)
                clears.append(index)
                if fault == "cache" and index == phase_index:
                    raise OSError("injected cache failure")
                return "Removed mocked cache\n"

            def run(command, env, stdout, stderr, check):
                index = len(calls)
                calls.append(command)
                active = fault if index == phase_index else None
                expected = SELECTED[index]
                self.assertEqual(len(clears), index + 1)
                self.assertEqual(command[command.index("--parallel") + 1], str(len(expected)))
                self.assertEqual(command[command.index("--build-config") + 1], config)
                self.assertIn("--no-tests=error", command)
                selector = command[command.index("--tests-regex") + 1]
                for name in {name for group in SELECTED for name in group}:
                    self.assertEqual(bool(re.search(selector, name)), name in expected)
                self.assertIsNone(re.search(selector, TARGET + " extra"))
                self.assertIsNone(re.search(selector, "extra " + TARGET))
                self.assertEqual(env["PX_TRACE_BRDF_BARRIERS"], "0")
                self.assertEqual(env["MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS"], "1")
                self.assertEqual(env["PX_REQUIRE_GPU"], "1")
                self.assertNotIn("MVK_CONFIG_FAST_MATH_ENABLED", env)
                self.assertFalse((logs / "LastTest.log").exists())
                self.assertFalse((logs / "CTestCostData.txt").exists())
                self.assertFalse(any((output / name).exists() for name in diagnostic.IMAGES))
                mode = "discrete resource indexes" if active == "mode" else "Metal argument buffers"
                stdout.write(f"Descriptor sets binding resources using {mode}.\n")
                (logs / "LastTest.log").write_text(f"1/1 Testing: {TARGET}\nGPU device creation epoch_ms: {index + 1}\n")
                (logs / "CTestCostData.txt").write_text(PHASES[index])
                if REFERENCE in expected and active != "image":
                    (output / "R1-Reference.png").write_text(PHASES[index])
                suite = ET.Element("testsuite")
                names = expected[:-1] if active == "missing" else list(expected)
                if active == "extra":
                    names.append("Unexpected test")
                if active == "duplicate":
                    names.append(expected[0])
                for case_index, name in enumerate(names):
                    case = ET.SubElement(suite, "testcase", name=name, status="run")
                    if active in ("failure", "skipped") and case_index == len(names) - 1:
                        ET.SubElement(case, active)
                path = Path(command[command.index("--output-junit") + 1])
                if active != "no_xml":
                    ET.ElementTree(suite).write(path)
                    if active == "bad_xml":
                        path.write_text("<broken")
                return subprocess.CompletedProcess(command, 8 if active == "exit" else 0)

            with patch.object(sys, "argv", ["diagnose-metal.py", str(build), "--config", config, "--contention-only"]), \
                    patch.object(diagnostic, "clear_metal_cache", side_effect=clear), \
                    patch.object(diagnostic.subprocess, "run", side_effect=run), \
                    contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                status = diagnostic.main()
            self.assertEqual(status, int(fault is not None))
            count = phase_index if fault == "cache" else len(PHASES)
            self.assertEqual(len(calls), count)
            self.assertEqual(len(clears), count + (fault == "cache"))
            root, = (output / "CacheComparison").iterdir()
            self.assertFalse((root / "Cache.txt").exists())
            self.assertEqual((root / "Baseline" / "R1-Reference.png").read_text(), "baseline")
            for index in range(count):
                directory = root / PHASES[index]
                for name in ("Cache.txt", "Test.log", "BindingMode.txt", "LastTest.log", "CaseOutcomes.txt"):
                    self.assertTrue((directory / name).is_file(), (PHASES[index], name))
                if fault == "failure" and index == phase_index:
                    outcomes = (directory / "CaseOutcomes.txt").read_text()
                    role = "target" if len(SELECTED[index]) == 1 else "partner"
                    self.assertIn(f"{role}: FAIL:", outcomes)

    def test_success_configurations(self):
        for config in ("Debug", "Release"):
            for flat in (False, True):
                with self.subTest(config=config, flat=flat):
                    self.exercise(config=config, flat=flat)

    def test_failures_are_preserved_without_skipping_later_phases(self):
        for index in range(len(PHASES)):
            for fault in ("exit", "mode", "missing", "extra", "duplicate", "failure", "skipped", "no_xml", "bad_xml", "cache"):
                with self.subTest(phase=PHASES[index], fault=fault):
                    self.exercise(fault=fault, phase_index=index)
        for index in (2, 3, 4):
            self.exercise(fault="image", phase_index=index)

    def test_modes_are_mutually_exclusive(self):
        for mode in ("--isolated-only", "--concurrency-only"):
            with patch.object(sys, "argv", ["diagnose-metal.py", "--contention-only", mode]), \
                    patch.object(diagnostic, "clear_metal_cache") as clear, \
                    patch.object(diagnostic.subprocess, "run") as run, contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as error:
                    diagnostic.main()
                self.assertEqual(error.exception.code, 2)
                clear.assert_not_called()
                run.assert_not_called()


if __name__ == "__main__":
    unittest.main()
