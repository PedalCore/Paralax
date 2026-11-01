#!/usr/bin/env python3
"""Compare full suites and isolated cases after clearing the ephemeral runner Metal cache."""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


IMAGES = ["R1-Reference.png", "R2-Environment-18.png", "R2-Environment-19.png",
          "R2-Rotated-18.png", "R2-Rotated-19.png"]
FAILURES = ["ReferenceLightingFailure", "HDRLightingFailure-18", "HDRLightingFailure-19"]


def clear_metal_cache():
    # This experiment must never clear a developer's cache when invoked locally.
    if sys.platform != "darwin" or os.environ.get("GITHUB_ACTIONS") != "true":
        raise RuntimeError("Metal cache clearing is restricted to macOS GitHub Actions runners")
    result = subprocess.run(["getconf", "DARWIN_USER_CACHE_DIR"], capture_output=True, text=True, check=True)
    raw = result.stdout.strip()
    root = Path(raw)
    if not raw or not root.is_absolute() or not root.is_dir() or root.resolve() == Path("/"):
        raise RuntimeError("getconf did not return a valid user cache directory")
    cache = root / "com.apple.metal"
    if cache.is_symlink() or cache.resolve().parent != root.resolve():
        raise RuntimeError("Refusing a redirected Metal cache path")
    if cache.exists():
        if not cache.is_dir():
            raise RuntimeError("Metal cache path is not a directory")
        print(f"Removing runner cache: {cache}", flush=True)
        shutil.rmtree(cache)
        return f"Removed {cache}\n"
    return f"Cache absent: {cache}; cache clearing was not exercised\n"


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    candidates = [build / "bin" / "Paralax-Tests", build / "bin" / "Debug" / "Paralax-Tests"]
    if not any(path.is_file() for path in candidates):
        print("Paralax-Tests executable is missing", file=sys.stderr)
        return 1
    output = build / "Paralax-Tests" / "TestOutput"
    comparison = output / "CacheComparison"
    comparison.mkdir(parents=True, exist_ok=True)
    run = Path(tempfile.mkdtemp(prefix="run-", dir=comparison))
    gpu_events = []

    def capture(destination):
        destination.mkdir(parents=True, exist_ok=True)
        for name in IMAGES:
            if (output / name).is_file():
                shutil.copy2(output / name, destination / name)
        for name in FAILURES:
            if (output / name).is_dir():
                shutil.copytree(output / name, destination / name)
        costs = build / "Testing" / "Temporary" / "CTestCostData.txt"
        if costs.is_file():
            shutil.copy2(costs, destination / "CTestCostData.txt")
        log = build / "Testing" / "Temporary" / "LastTest.log"
        if log.is_file():
            shutil.copy2(log, destination / "LastTest.log")
            blocks = re.split(r"^\d+/\d+ Testing: (.+)$", log.read_text(errors="replace"), flags=re.MULTILINE)
            for index in range(1, len(blocks), 2):
                for timestamp in re.findall(r"GPU device creation epoch_ms: (\d+)", blocks[index + 1]):
                    gpu_events.append((int(timestamp), destination.name, blocks[index]))
        gpu_events.sort()
        (run / "GPUOrder.txt").write_text("GPUTestContext device-creation markers, UTC epoch milliseconds\n" +
                                         "".join(f"{timestamp} {phase}: {name}\n" for timestamp, phase, name in gpu_events))
        for report in destination.glob("*/LightingTextures.txt"):
            text = report.read_text()
            test_name = next((line.removeprefix("TestCase=") for line in text.splitlines() if line.startswith("TestCase=")), "")
            matching = [time for time, phase, name in gpu_events if phase == destination.name and name == test_name]
            first = "unknown"
            if matching and gpu_events:
                earliest = gpu_events[0][0]
                if min(matching) > earliest:
                    first = "no"
                else:
                    ties = {(phase, name) for time, phase, name in gpu_events if time == earliest}
                    first = "yes" if len(ties) == 1 else "tied at millisecond precision"
            report.write_text(text + f"FirstObservedGPUTestOnRunner={first}\nOrderingEvidence=../../GPUOrder.txt (GPUTestContext markers)\n")

    capture(run / "Baseline")
    try:
        cache_report = clear_metal_cache()
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        (run / "Cache.txt").write_text(f"Cache operation failed: {error}\n")
        print(f"Cache operation failed: {error}", file=sys.stderr)
        return 1
    (run / "Cache.txt").write_text(cache_report)
    print(cache_report.strip(), flush=True)
    environment = dict(os.environ, PX_REQUIRE_GPU="1", MVK_CONFIG_LOG_LEVEL="3", VK_LOADER_DEBUG="layer", PX_TRACE_BRDF_BARRIERS="1")
    environment.pop("MVK_CONFIG_FAST_MATH_ENABLED", None)
    failed = False
    # Return to argument buffers after a discrete pass to reduce the warm-cache/order confound.
    phases = [("AfterCacheClear", "1", "1"), ("WarmRepeat", "1", "1"),
              ("SilentPassThrough", "passthrough", "1"), ("DiscreteBindings", "passthrough", "0"),
              ("ArgumentBuffersAgain", "passthrough", "1"), ("DiscreteBindingsAgain", "passthrough", "0")]
    phases = [(name, trace, bindings, None) for name, trace, bindings in phases]
    isolated = [("Normal", "SceneRenderer: BRDF LUT survives output and internal target allocation"),
                ("BeforeRender", "SceneRenderer: BRDF LUT survives target preparation before frame recording"),
                ("Stages", "SceneRenderer: BRDF LUT survives each target preparation stage")]
    # Each case starts after its own clear. Reverse round two to expose other persistent order effects.
    for round_number, cases in [(1, isolated), (2, list(reversed(isolated)))]:
        phases.extend((f"Isolated{round_number}{name}", "1", "1", test_case) for name, test_case in cases)
    for phase, trace, argument_buffers, test_case in phases:
        environment["PX_TRACE_BRDF_BARRIERS"] = trace
        environment["MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS"] = argument_buffers
        destination = run / phase
        destination.mkdir()
        if test_case is not None:
            try:
                cache_report = clear_metal_cache()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                (destination / "Cache.txt").write_text(f"Cache operation failed; case not run: {error}\n")
                print(f"{phase}: cache operation failed: {error}", file=sys.stderr)
                return 1
            (destination / "Cache.txt").write_text(cache_report)
            print(f"{phase}: {cache_report.strip()}", flush=True)
        # Each phase must contain its own images, including when a test aborts before saving one.
        for name in IMAGES:
            (output / name).unlink(missing_ok=True)
        for name in FAILURES:
            if (output / name).exists():
                shutil.rmtree(output / name)
        (build / "Testing" / "Temporary" / "LastTest.log").unlink(missing_ok=True)
        # Learned costs prioritize slow/failed tests and otherwise change the baseline's scheduling order.
        (build / "Testing" / "Temporary" / "CTestCostData.txt").unlink(missing_ok=True)
        command = ["ctest", "--test-dir", str(build), "--build-config", "Debug", "--parallel", "1" if test_case is not None else "4",
                   "--timeout", "300", "--no-tests=error", "--verbose", "--output-junit", str(destination / "Results.xml")]
        if test_case is not None:
            command.extend(["--tests-regex", f"^{re.escape(test_case)}$"])
        with (destination / "Test.log").open("w") as log:
            result = subprocess.run(command, env=environment, stdout=log, stderr=subprocess.STDOUT, check=False)
        status = result.returncode
        modes = set(re.findall(r"Descriptor sets binding resources using ([^.]+)\.",
                               (destination / "Test.log").read_text(errors="replace")))
        expected_mode = "discrete resource indexes" if argument_buffers == "0" else "Metal argument buffers"
        (destination / "BindingMode.txt").write_text(f"Requested MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS={argument_buffers}\n" +
                                                   "Observed: " + ", ".join(sorted(modes)) + "\n")
        if modes != {expected_mode}:
            print(f"{phase}: unexpected or missing binding-mode evidence: {sorted(modes)}", flush=True)
            status = 1
        if status == 0 and test_case is None and not all((output / name).is_file() for name in IMAGES):
            status = 1
        failed |= status != 0
        capture(destination)
        print(f"{phase}: exit {status}", flush=True)
    print(f"Diagnostic artifacts: {run}", flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
