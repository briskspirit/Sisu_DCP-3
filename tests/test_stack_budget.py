#!/usr/bin/env python3
"""Regression tests for the GCC callgraph stack-budget gate."""

from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "check_stack_budget.py"


def graph(frame_b: int = 200, qualifier_b: str = "static",
          recursive: bool = False) -> str:
    back_edge = 'edge: { sourcename: "b" targetname: "root" }\n' if recursive else ""
    return (
        'graph: { title: "fixture"\n'
        'node: { title: "root" label: "root\\nfixture.c:1:1\\n100 bytes (static)" }\n'
        f'node: {{ title: "b" label: "b\\nfixture.c:2:1\\n{frame_b} bytes ({qualifier_b})" }}\n'
        'node: { title: "__indirect_call" label: "Indirect Call Placeholder" shape : ellipse }\n'
        'edge: { sourcename: "root" targetname: "b" }\n'
        'edge: { sourcename: "b" targetname: "__indirect_call" }\n'
        f'{back_edge}'
        '}\n'
    )


def run(ci_text: str, *extra: str) -> subprocess.CompletedProcess[str]:
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        (directory / "fixture.c.ci").write_text(ci_text, encoding="utf-8")
        return subprocess.run(
            ["python3", str(SCRIPT), "--input-dir", str(directory),
             "--root", "core0=root", "--stack-size", "512",
             "--irq-reserve", "64", "--uncertainty-reserve", "64", *extra],
            text=True, capture_output=True, check=False,
        )


def main() -> None:
    ok = run(graph())
    assert ok.returncode == 0, ok.stderr
    assert "direct=300 post-reserve-margin=84" in ok.stdout
    assert "root(100) -> b(200)" in ok.stdout
    assert "indirect-callers=1" in ok.stdout

    over = run(graph(frame_b=300))
    assert over.returncode == 1
    assert "exceed the budget" in over.stderr

    recursive = run(graph(recursive=True))
    assert recursive.returncode == 1
    assert "recursive call cycle" in recursive.stderr

    dynamic = run(graph(qualifier_b="dynamic"))
    assert dynamic.returncode == 1
    assert "unbounded dynamic frame" in dynamic.stderr

    missing = run(graph(), "--root", "missing=not_present")
    assert missing.returncode == 1
    assert "has 0 definitions" in missing.stderr

    print("stack budget tests passed")


if __name__ == "__main__":
    main()
