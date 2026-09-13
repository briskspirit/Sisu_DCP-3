#!/usr/bin/env python3
"""Gate firmware stack budgets from GCC's per-TU callgraph information."""

from __future__ import annotations

import argparse
import re
import sys
from dataclasses import dataclass
from pathlib import Path


NODE_RE = re.compile(
    r'^node: \{ title: "([^"]+)" label: "([^"\\]*(?:\\.[^"\\]*)*)"'
)
EDGE_RE = re.compile(
    r'^edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"'
)
FRAME_RE = re.compile(r"\n(\d+) bytes \(([^)]+)\)")
INDIRECT_NODE = "__indirect_call"


class StackBudgetError(RuntimeError):
    pass


@dataclass(frozen=True)
class Frame:
    name: str
    size: int
    qualifier: str


@dataclass
class CallGraph:
    frames: dict[str, Frame]
    edges: dict[str, set[str]]


def parse_callgraphs(directory: Path) -> CallGraph:
    files = sorted(directory.rglob("*.ci"))
    if not files:
        raise StackBudgetError(f"no GCC .ci files found under {directory}")

    frames: dict[str, Frame] = {}
    edges: dict[str, set[str]] = {}
    for path in files:
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            node = NODE_RE.match(line)
            if node:
                title, encoded_label = node.groups()
                label = encoded_label.replace(r"\n", "\n")
                name = label.splitlines()[0]
                match = FRAME_RE.search(label)
                if match:
                    frame = Frame(name, int(match.group(1)), match.group(2))
                    previous = frames.get(title)
                    if previous is not None and previous != frame:
                        raise StackBudgetError(
                            f"conflicting frame records for {title}: "
                            f"{previous} vs {frame}"
                        )
                    frames[title] = frame
                continue

            edge = EDGE_RE.match(line)
            if edge:
                caller, callee = edge.groups()
                edges.setdefault(caller, set()).add(callee)

    return CallGraph(frames=frames, edges=edges)


def resolve_root(graph: CallGraph, function_name: str) -> str:
    matches = [title for title, frame in graph.frames.items()
               if frame.name == function_name]
    if len(matches) != 1:
        raise StackBudgetError(
            f"root {function_name!r} has {len(matches)} definitions; expected 1"
        )
    return matches[0]


def maximum_path(graph: CallGraph, root: str) -> tuple[int, list[str]]:
    memo: dict[str, tuple[int, list[str]]] = {}
    active: list[str] = []

    def visit(node: str) -> tuple[int, list[str]]:
        if node in active:
            cycle = active[active.index(node):] + [node]
            names = " -> ".join(graph.frames[item].name for item in cycle)
            raise StackBudgetError(f"reachable recursive call cycle: {names}")
        if node in memo:
            return memo[node]

        frame = graph.frames[node]
        if "dynamic" in frame.qualifier and "bounded" not in frame.qualifier:
            raise StackBudgetError(
                f"unbounded dynamic frame reachable at {frame.name}"
            )

        active.append(node)
        best_size = 0
        best_path: list[str] = []
        for callee in graph.edges.get(node, ()):
            if callee == INDIRECT_NODE:
                continue
            if callee not in graph.frames:
                continue
            size, path = visit(callee)
            if size > best_size:
                best_size = size
                best_path = path
        active.pop()

        result = (frame.size + best_size, [node] + best_path)
        memo[node] = result
        return result

    return visit(root)


def reachable_edge_counts(graph: CallGraph, root: str) -> tuple[int, int]:
    seen: set[str] = set()
    pending = [root]
    indirect_callers = 0
    unknown_edges = 0
    while pending:
        node = pending.pop()
        if node in seen:
            continue
        seen.add(node)
        callees = graph.edges.get(node, ())
        if INDIRECT_NODE in callees:
            indirect_callers += 1
        for callee in callees:
            if callee == INDIRECT_NODE:
                continue
            if callee not in graph.frames:
                unknown_edges += 1
            elif callee not in seen:
                pending.append(callee)
    return indirect_callers, unknown_edges


def format_path(graph: CallGraph, path: list[str]) -> str:
    return " -> ".join(
        f"{graph.frames[node].name}({graph.frames[node].size})" for node in path
    )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--root", action="append", required=True,
                        metavar="LABEL=FUNCTION")
    parser.add_argument("--stack-size", type=int, required=True)
    parser.add_argument("--irq-reserve", type=int, required=True)
    parser.add_argument("--uncertainty-reserve", type=int, required=True)
    parser.add_argument("--report", type=Path)
    args = parser.parse_args(argv)

    try:
        if min(args.stack_size, args.irq_reserve,
               args.uncertainty_reserve) < 0:
            raise StackBudgetError("stack sizes and reserves must be nonnegative")
        usable = args.stack_size - args.irq_reserve - args.uncertainty_reserve
        if usable <= 0:
            raise StackBudgetError("reserves consume the complete stack")

        graph = parse_callgraphs(args.input_dir)
        lines = [
            f"[stack] size={args.stack_size} irq_reserve={args.irq_reserve} "
            f"indirect/library_reserve={args.uncertainty_reserve} "
            f"direct_budget={usable}"
        ]
        failed = False
        for root_spec in args.root:
            if "=" not in root_spec:
                raise StackBudgetError(
                    f"invalid --root {root_spec!r}; expected LABEL=FUNCTION"
                )
            label, function_name = root_spec.split("=", 1)
            root = resolve_root(graph, function_name)
            usage, path = maximum_path(graph, root)
            indirect, unknown = reachable_edge_counts(graph, root)
            margin = usable - usage
            lines.append(
                f"[stack] {label}: direct={usage} post-reserve-margin={margin} "
                f"indirect-callers={indirect} unknown-callees={unknown}"
            )
            lines.append(f"[stack] {label} path: {format_path(graph, path)}")
            if margin < 0:
                failed = True

        output = "\n".join(lines) + "\n"
        sys.stdout.write(output)
        if args.report is not None:
            args.report.parent.mkdir(parents=True, exist_ok=True)
            args.report.write_text(output, encoding="utf-8")
        if failed:
            raise StackBudgetError("one or more reachable paths exceed the budget")
    except StackBudgetError as exc:
        print(f"[stack] ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
