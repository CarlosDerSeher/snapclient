"""
Pre-build script: generate EMBED_FILES / EMBED_TXTFILES assembly stubs.

ESP-IDF converts embedded binary/text files into assembly stubs (e.g.
favicon.ico.S) via CMake CUSTOM_COMMANDs.  PlatformIO skips those commands
because it drives ninja directly on object-file targets.  This script runs
`cmake --build . --target <name>.S` for every such stub before ninja starts,
so the source files exist when the assembler is invoked.
"""

import subprocess
import os
import re

Import("env")  # noqa: F821  (PlatformIO injects this)


def generate_embedded_stubs():
    build_dir = env.subst("$BUILD_DIR")
    ninja_file = os.path.join(build_dir, "build.ninja")

    if not os.path.isfile(ninja_file):
        print("pre_build.py: build.ninja not found, skipping stub generation.")
        return

    # Find all *.S targets produced by CUSTOM_COMMAND (the embed stubs).
    # In build.ninja they appear as:
    #   build foo.ico.S | ...: CUSTOM_COMMAND ...
    stub_pattern = re.compile(r"^build ([^|:\s]+\.S)(?:\s*\|[^:]*)?:\s*CUSTOM_COMMAND")
    stubs = []
    with open(ninja_file) as f:
        for line in f:
            m = stub_pattern.match(line)
            if m:
                stubs.append(m.group(1))

    if not stubs:
        return

    for stub in stubs:
        full_path = os.path.join(build_dir, stub)
        if os.path.isfile(full_path):
            continue  # already generated, skip
        print(f"pre_build.py: generating {stub} ...")
        result = subprocess.run(
            ["cmake", "--build", ".", "--target", stub],
            cwd=build_dir,
        )
        if result.returncode != 0:
            print(f"pre_build.py: WARNING — cmake failed to generate {stub}")


generate_embedded_stubs()
