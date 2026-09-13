#!/usr/bin/env python3
"""Synthetic regression tests for the layering include scanner."""

from __future__ import annotations

import tempfile
from pathlib import Path

import layering_scan


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


with tempfile.TemporaryDirectory() as td:
    source = Path(td) / "probe.c"
    source.write_bytes(
        b'#inc\\\nlude "HAL/../hal/Audio.H"\n'
        b'# /* gap */ include <Services/./Timebase.h>\n'
        b'#inc\\\r\nlude <UI/UI.H>\r\n'
        b'// #include "hal/commented_line.h"\n'
        b'/* #include "hal/commented_block.h" */\n'
        b'#include MACRO_HEADER\n'
        b'#include_next <hal/next.h>\n'
        b'#include "services/../../hal/up.h"\n'
        b'#include </absolute.h>\n'
        b'#include "unterminated\n')
    rows = [(style, operand)
            for _, style, operand in layering_scan.scan_file(str(source))]
    check(rows == [
        ("q", "hal/audio.h"),
        ("a", "services/timebase.h"),
        ("a", "ui/ui.h"),
        ("x", "macro_header"),
        ("x", "<hal/next.h>"),
        ("q", "../hal/up.h"),
        ("a", "/absolute.h"),
        ("x", "?"),
    ], "include scanning handles splices, comments, styles, and path normalization")

deps = layering_scan.normalize_dependencies(
    "probe.o: include/APP.H \\\n"
    " include/services/../hal/Foo.h \\\r\n"
    " include/ui/./ui.h\n")
check(deps == ["include/app.h", "include/hal/foo.h", "include/ui/ui.h"],
      "dependency normalization handles make continuations, case, and dot segments")

check(layering_scan.classify_production_path("src/apps/clock/app.c") == "app",
      "nested app source is classified")
check(layering_scan.classify_production_path("include/services/timebase.h") == "service",
      "service header is classified")
check(layering_scan.classify_production_path("include/sisu_build_config.h") == "service",
      "root build-profile contract is classified as neutral service policy")
check(layering_scan.classify_production_path("src/main.c") == "composition",
      "composition root is classified")
check(layering_scan.classify_production_path("src/generated/assets_data.c") == "generated",
      "generated source is classified")
with tempfile.TemporaryDirectory() as td:
    leak = Path(td) / "src" / "foo" / "leak.c"
    leak.parent.mkdir(parents=True)
    leak.write_text('#include "app.h"\n#include "hal/battery_hal.h"\n',
                    encoding="utf-8")
    leak_operands = [operand for _, _, operand in layering_scan.scan_file(str(leak))]
    check(leak_operands == ["app.h", "hal/battery_hal.h"],
          "synthetic unknown-directory leak carries both forbidden edges")
    try:
        # Before the fail-closed classifier this file was absent from both the
        # service and app policy buckets, so neither edge was checked.
        layering_scan.classify_production_path("src/foo/leak.c")
    except ValueError as exc:
        check("unregistered production path" in str(exc),
              "unknown-directory failure explains the policy gap")
    else:
        raise AssertionError("unknown production directory must fail closed")

print("layering scanner tests passed")
