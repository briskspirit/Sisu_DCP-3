#!/usr/bin/env python3
"""Textual include scanner for the layering guard (tests/run_tests.sh).

Plain grep leaves textual evasion channels open: an include split across a
backslash-newline, hidden behind a comment, written with angle brackets, cased
for a case-insensitive filesystem, or routed through "..". This scanner
strips splices and comments first and normalizes every operand, so the guard
matches what the compiler would actually resolve.

Subcommands (all emit tab-separated lines on stdout):

  includes FILE...   one line per #include directive in each FILE:
                         path<TAB>style<TAB>operand
                     style: q (quoted), a (angle), x (non-literal: computed
                     macro operand or #include_next -- the guard bans these
                     outright so textual matching stays reliable).
                     The operand is posixpath-normalized and lowercased; a
                     leading ".." or "/" survives normalization and is the
                     guard's cue to reject the include as path-relative.
  normdeps           read `cc -MM` output on stdin, emit one normalized
                     (normpath, lowercased) dependency path per line.
  classify FILE...   classify every production source/header as app, service,
                     composition, or generated. An unregistered path fails;
                     this makes a new top-level directory fail closed instead
                     of silently receiving no layering policy.
"""
from __future__ import annotations

import posixpath
import re
import sys

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_DIRECTIVE = re.compile(r"^\s*#\s*(include(?:_next)?)\s*(.*)$")

_ROOT_APP_FILES = {
    "src/app.c",
    "src/app_router.c",
    "src/app_runtime.c",
    "src/app_status_runtime.c",
    "include/app.h",
    "include/app_internal.h",
    "include/app_router.h",
    "include/app_runtime.h",
    "include/app_status_runtime.h",
}
_ROOT_SERVICE_FILES = {
    "include/sisu_build_config.h",
}
_SERVICE_DIRS = ("services", "audio", "storage", "ui", "hal", "diag")


def _norm(operand: str) -> str:
    return posixpath.normpath(operand.strip()).lower()


def scan_file(path: str) -> list[tuple[str, str, str]]:
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    # Order matters: splices first (a comment or directive may be split),
    # then comments (an include may hide behind /* ... */ on the same line).
    text = text.replace("\\\r\n", "").replace("\\\n", "")
    text = _BLOCK_COMMENT.sub(" ", text)
    text = _LINE_COMMENT.sub("", text)
    rows = []
    for line in text.splitlines():
        m = _DIRECTIVE.match(line)
        if not m:
            continue
        keyword, rest = m.group(1), m.group(2).strip()
        if keyword == "include_next":
            rows.append((path, "x", _norm(rest) if rest else "?"))
            continue
        if rest.startswith('"'):
            end = rest.find('"', 1)
            rows.append((path, "q", _norm(rest[1:end])) if end > 0
                        else (path, "x", "?"))
        elif rest.startswith("<"):
            end = rest.find(">", 1)
            rows.append((path, "a", _norm(rest[1:end])) if end > 0
                        else (path, "x", "?"))
        else:
            rows.append((path, "x", _norm(rest) if rest else "?"))
    return rows


def normalize_dependencies(text: str) -> list[str]:
    """Normalize dependency paths from one `cc -MM` makefile fragment."""
    text = text.replace("\\\r\n", " ").replace("\\\n", " ").replace("\\", " ")
    return [_norm(token) for token in text.split() if not token.endswith(":")]


def classify_production_path(path: str) -> str:
    """Return the layering class for one repo-relative production path."""
    normalized = _norm(path)
    if normalized in _ROOT_APP_FILES or normalized.startswith((
            "src/apps/", "include/apps/")):
        return "app"
    if normalized in _ROOT_SERVICE_FILES:
        return "service"
    if normalized == "src/main.c":
        return "composition"
    if normalized.startswith(("src/generated/", "include/generated/")):
        return "generated"
    if normalized.startswith(tuple(
            f"{root}/{directory}/"
            for root in ("src", "include")
            for directory in _SERVICE_DIRS)):
        return "service"
    raise ValueError(f"unregistered production path: {path}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    mode = sys.argv[1]
    if mode == "includes":
        for path in sys.argv[2:]:
            for row in scan_file(path):
                print("\t".join(row))
        return 0
    if mode == "normdeps":
        for dependency in normalize_dependencies(sys.stdin.read()):
            print(dependency)
        return 0
    if mode == "classify":
        try:
            for path in sys.argv[2:]:
                print(f"{path}\t{classify_production_path(path)}")
        except ValueError as exc:
            print(exc, file=sys.stderr)
            return 1
        return 0
    print(f"unknown mode {mode!r}", file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main())
