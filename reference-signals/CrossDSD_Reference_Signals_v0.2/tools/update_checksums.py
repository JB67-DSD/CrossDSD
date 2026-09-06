#!/usr/bin/env python3
"""Regenerate SHA256SUMS.txt for every distributed suite file."""

from __future__ import annotations

import hashlib
import os
from pathlib import Path


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    excluded = {"SHA256SUMS.txt", "SHA256SUMS.txt.new", ".DS_Store"}
    files = sorted(
        (
            path
            for path in root.rglob("*")
            if path.is_file()
            and path.name not in excluded
            and "__pycache__" not in path.parts
            and path.suffix not in {".pyc", ".pyo"}
        ),
        key=lambda path: path.relative_to(root).as_posix(),
    )
    temporary = root / "SHA256SUMS.txt.new"
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        for path in files:
            relative = path.relative_to(root).as_posix()
            stream.write(f"{digest(path)}  ./{relative}\n")
    os.replace(temporary, root / "SHA256SUMS.txt")
    print(f"Updated SHA256SUMS.txt with {len(files)} entries.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
