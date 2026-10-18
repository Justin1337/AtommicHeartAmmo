#!/usr/bin/env python3
"""Rebuild and run all portable C++ tests; fail on the first error.

The game process is never opened. AddressSanitizer and UndefinedBehaviorSanitizer
check the fixtures and parser. LeakSanitizer is disabled for /proc-restricted
runtimes; that limitation is also documented in TESTS.md.
"""
from pathlib import Path
import os
import subprocess

project = Path(__file__).resolve().parent.parent
suites = ["core_tests", "resolver_tests", "pattern_tests", "runtime_tests", "feature_tests"]
flags = ["-std=c++17", "-Wall", "-Wextra", "-Werror", "-pedantic",
         "-fsanitize=address,undefined", "-g"]
environment = os.environ.copy()
environment["ASAN_OPTIONS"] = "detect_leaks=0"
for suite in suites:
    print(f"SUITE: {suite}", flush=True)
    subprocess.run(["g++", *flags, f"tests/{suite}.cpp", "-o", f"tests/{suite}"],
                   cwd=project, check=True)
    subprocess.run([str(project / "tests" / suite)], cwd=project,
                   env=environment, check=True)
print("ALL FIVE SUITES PASSED", flush=True)
