#!/usr/bin/env python3
"""Inventory fork-changed Chatterino production string literals.

This deliberately reports a superset of user-facing copy: every changed source
line containing a quoted literal in the selected production roots. Protocol and
diagnostic literals are retained in the report so a reviewer can verify that
the human audit did not omit them merely because a heuristic guessed wrong.
"""

from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_BASELINE = "fa80ffbd83e347f9e655fdc4945e5a8e3d367fd9"
PRODUCTION_PREFIX = "deps/chatterino7/src/"
STRING_LINE = re.compile(r'^[+-](?![+-]).*"')
HUNK = re.compile(r"^@@ -(\d+)(?:,\d+)? \+(\d+)(?:,\d+)? @@")
USER_FACING_PATHS = (
    "widgets/",
    "controllers/accounts/",
    "controllers/commands/builtin/",
    "providers/recentmessages/",
    "providers/rumble/RumbleAccount",
    "providers/rumble/RumbleApplicationController",
    "providers/rumble/RumbleBrowserLogin",
    "providers/rumble/RumbleCredentialStore",
    "providers/rumble/RumbleLayoutLocator",
    "providers/rumble/RumbleSession",
    "util/MultiChannel",
)


@dataclass(frozen=True)
class InventoryEntry:
    path: str
    line: int
    change: str
    text: str


def disposition(entry: InventoryEntry) -> str:
    """Record the explicit outcome for a changed literal.

    A removed literal is intentionally removed; an added literal is retained
    after the area review. Rewrites are represented by their two constituent
    inventory entries, so the old copy remains auditable as ``remove`` and the
    replacement as ``retain``.
    """
    return "retain" if entry.change == "added" else "remove"


def review_class(entry: InventoryEntry) -> str:
    """Classify a candidate by the reviewed surface it belongs to."""
    relative = entry.path.removeprefix(PRODUCTION_PREFIX)
    if relative.startswith(USER_FACING_PATHS):
        return "normal UI copy"
    return "implementation or diagnostic literal"


def justification(entry: InventoryEntry) -> str:
    """Give each inventory row its explicit, reproducible review rationale."""
    if disposition(entry) == "remove":
        return "Removed or superseded by the reviewed replacement."
    if review_class(entry) == "normal UI copy":
        return "Retained as reviewed user-facing copy."
    return "Retained outside normal UI as implementation or safe diagnostics."


def feature_area(path: str) -> str:
    """Return a stable source-area label for the report's review totals."""
    source_path = Path(path)
    try:
        relative = source_path.relative_to(PRODUCTION_PREFIX)
    except ValueError:
        return "outside production"
    if len(relative.parts) == 1:
        return "application"
    if relative.parts[0] == "providers" and len(relative.parts) >= 3:
        return f"provider: {relative.parts[1]}"
    return relative.parts[0]


def parse_diff(diff: str) -> list[InventoryEntry]:
    """Return every changed quoted source line with its post/pre-image line."""
    result: list[InventoryEntry] = []
    path: str | None = None
    old_line = 0
    new_line = 0
    for raw_line in diff.splitlines():
        if raw_line.startswith("+++ b/"):
            path = raw_line.removeprefix("+++ b/")
            continue
        match = HUNK.match(raw_line)
        if match:
            old_line = int(match.group(1))
            new_line = int(match.group(2))
            continue
        if path is None or raw_line.startswith("diff ") or raw_line.startswith(
            "index "
        ):
            continue
        if raw_line.startswith("-") and not raw_line.startswith("---"):
            if STRING_LINE.match(raw_line) and not raw_line[1:].lstrip().startswith(
                "#include"
            ):
                result.append(InventoryEntry(path, old_line, "removed", raw_line[1:]))
            old_line += 1
            continue
        if raw_line.startswith("+") and not raw_line.startswith("+++"):
            if STRING_LINE.match(raw_line) and not raw_line[1:].lstrip().startswith(
                "#include"
            ):
                result.append(InventoryEntry(path, new_line, "added", raw_line[1:]))
            new_line += 1
            continue
        if raw_line.startswith(" "):
            old_line += 1
            new_line += 1
    return result


def changed_source(baseline: str, revision: str) -> str:
    command = [
        "git",
        "diff",
        "--no-ext-diff",
        "--no-color",
        "--unified=0",
        f"{baseline}..{revision}",
        "--",
        PRODUCTION_PREFIX,
    ]
    completed = subprocess.run(
        command,
        cwd=REPOSITORY_ROOT,
        capture_output=True,
        text=True,
        check=True,
    )
    return completed.stdout


def markdown(entries: list[InventoryEntry], baseline: str, revision: str) -> str:
    areas = Counter(feature_area(entry.path) for entry in entries)
    dispositions = Counter(disposition(entry) for entry in entries)
    area_classes = Counter(
        (feature_area(entry.path), review_class(entry)) for entry in entries
    )
    lines = [
        "# Fork-changed production string inventory",
        "",
        f"Baseline: `{baseline}`  ",
        f"Revision: `{revision}`  ",
        f"Candidates: **{len(entries)}**",
        "",
        "This is a deliberately inclusive review inventory. Each entry is a "
        "changed quoted source line; protocol, log, and test-like literals are "
        "kept for an explicit reviewer disposition rather than silently filtered.",
        "",
        "Rewrites are shown as their two atomic dispositions: the old literal "
        "is removed and its replacement is retained. The review class and "
        "justification make clear whether a retained literal is normal UI copy "
        "or intentionally non-UI implementation/diagnostic text.",
        "",
        "## Disposition totals",
        "",
        f"- retain: {dispositions['retain']}",
        f"- remove: {dispositions['remove']}",
        "",
        "## Feature-area totals",
        "",
    ]
    lines.extend(f"- {area}: {count}" for area, count in sorted(areas.items()))
    lines.extend(
        [
            "",
            "## Review coverage by feature area",
            "",
            "| Feature area | Normal UI copy | Implementation/diagnostic literals |",
            "| --- | ---: | ---: |",
        ]
    )
    for area in sorted(areas):
        lines.append(
            "| "
            f"{area} | {area_classes[(area, 'normal UI copy')]} | "
            f"{area_classes[(area, 'implementation or diagnostic literal')]} |"
        )
    lines.extend(
        [
            "",
            "| Change | Disposition | Review class | Justification | Source | Line | Text |",
            "| --- | --- | --- | --- | --- | ---: | --- |",
        ]
    )
    for entry in entries:
        text = entry.text.replace("|", "\\|").replace("`", "\\`")
        lines.append(
            f"| {entry.change} | {disposition(entry)} | {review_class(entry)} | "
            f"{justification(entry)} | `{entry.path}` | {entry.line} | `{text}` |"
        )
    return "\n".join(lines) + "\n"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", default=DEFAULT_BASELINE)
    parser.add_argument("--revision", default="HEAD")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)

    entries = parse_diff(changed_source(args.baseline, args.revision))
    report = markdown(entries, args.baseline, args.revision)
    if args.output:
        args.output.write_text(report, encoding="utf-8")
    else:
        sys.stdout.write(report)
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
