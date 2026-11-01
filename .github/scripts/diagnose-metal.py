#!/usr/bin/env python3
"""Run unchanged renderer tests under GPU contention, preserving each result."""

import concurrent.futures
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    build = Path(sys.argv[1] if len(sys.argv) > 1 else "build").resolve()
    candidates = [build / "bin" / "Paralax-Tests", build / "bin" / "Debug" / "Paralax-Tests"]
    executable = next((path for path in candidates if path.is_file()), None)
    if executable is None:
        print("Paralax-Tests executable is missing", file=sys.stderr)
        return 1

    output = build / "Paralax-Tests" / "TestOutput" / "Contention"
    output.mkdir(parents=True, exist_ok=True)
    # A fresh directory keeps artifacts from previous attempts out of this run.
    run = Path(tempfile.mkdtemp(prefix="run-", dir=output))
    environment = dict(os.environ, PX_REQUIRE_GPU="1")
    environment.pop("MVK_CONFIG_FAST_MATH_ENABLED", None)
    cases = [
        ("reference", "SceneRenderer: fixed look matches the reviewed reference image", ["R1-Reference.png"]),
        ("hdr", "SceneRenderer: built-in HDR rotation and intensity control image based surface lighting",
         ["R2-Environment-18.png", "R2-Environment-19.png", "R2-Rotated-18.png", "R2-Rotated-19.png"]),
    ]

    def execute(name, test_name, images):
        destination = run / name
        destination.mkdir()
        with (destination / "Test.log").open("w") as log:
            try:
                result = subprocess.run([str(executable), f"--test-case={test_name}"], cwd=destination, env=environment,
                                        stdout=log, stderr=subprocess.STDOUT, timeout=300, check=False)
                status = result.returncode
            except subprocess.TimeoutExpired:
                log.write("\nRenderer test timed out after 300 seconds\n")
                status = 124
        # A non-matching doctest filter exits successfully: require the test's images too.
        if status == 0 and not all((destination / "TestOutput" / image).is_file() for image in images):
            status = 1
        print(f"{name}: exit {status}", flush=True)
        return status

    failed = False
    # Four processes match CI's parallelism; each owns its device and output directory.
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        for case, test_name, images in cases:
            for iteration in range(1, 3):
                failed |= execute(f"{case}-serial-{iteration}", test_name, images) != 0
            for batch in range(1, 6):
                futures = [pool.submit(execute, f"{case}-batch-{batch}-worker-{worker}", test_name, images)
                           for worker in range(1, 5)]
                results = [future.result() for future in futures]
                failed |= any(status != 0 for status in results)
    print(f"Diagnostic artifacts: {run}", flush=True)
    return int(failed)


if __name__ == "__main__":
    sys.exit(main())
