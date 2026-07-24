#!/usr/bin/env python3
"""Verify that every non-system PE import is present in a portable bundle."""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


SYSTEM_DLLS = {
    "advapi32.dll",
    "authz.dll",
    "bcrypt.dll",
    "bcryptprimitives.dll",
    "cfgmgr32.dll",
    "comctl32.dll",
    "combase.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "d2d1.dll",
    "d3d9.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dnsapi.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxgi.dll",
    "gdi32.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "mpr.dll",
    "msvcrt.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shcore.dll",
    "shell32.dll",
    "shlwapi.dll",
    "user32.dll",
    "userenv.dll",
    "uuid.dll",
    "uxtheme.dll",
    "version.dll",
    "winhttp.dll",
    "winmm.dll",
    "winspool.drv",
    "ws2_32.dll",
    "wtsapi32.dll",
}
IMPORT_RE = re.compile(r"^\s*DLL Name:\s*(\S+)\s*$", re.MULTILINE)


def is_system(name: str) -> bool:
    lowered = name.casefold()
    return (
        lowered in SYSTEM_DLLS
        or lowered.startswith("api-ms-win-")
        or lowered.startswith("ext-ms-win-")
    )


def imports(objdump: str, path: pathlib.Path) -> list[str]:
    result = subprocess.run(
        [objdump, "-p", str(path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return IMPORT_RE.findall(result.stdout)


def is_x86_64_pe(objdump: str, path: pathlib.Path) -> bool:
    result = subprocess.run(
        [objdump, "-f", str(path)],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    return (
        "file format pei-x86-64" in result.stdout
        and "architecture: i386:x86-64" in result.stdout
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--objdump", required=True)
    parser.add_argument("bundle", type=pathlib.Path)
    args = parser.parse_args()

    binaries = sorted(
        path
        for path in args.bundle.rglob("*")
        if path.is_file() and path.suffix.casefold() in {".dll", ".exe"}
    )
    inventory: dict[str, pathlib.Path] = {}
    duplicate_names: list[str] = []
    for path in binaries:
        name = path.name.casefold()
        if name in inventory:
            duplicate_names.append(
                f"{path.relative_to(args.bundle)} duplicates "
                f"{inventory[name].relative_to(args.bundle)}"
            )
        else:
            inventory[name] = path

    unresolved: list[str] = []
    wrong_architecture: list[str] = []
    report: list[str] = []
    for path in binaries:
        relative = path.relative_to(args.bundle)
        if not is_x86_64_pe(args.objdump, path):
            wrong_architecture.append(str(relative))
        for imported in imports(args.objdump, path):
            classification = "bundled"
            if is_system(imported):
                classification = "windows-system"
            elif imported.casefold() not in inventory:
                classification = "MISSING"
                unresolved.append(f"{relative}: {imported}")
            report.append(f"{relative}\t{imported}\t{classification}")

    if duplicate_names:
        print("Duplicate PE basenames:", file=sys.stderr)
        print("\n".join(duplicate_names), file=sys.stderr)
        return 1
    if wrong_architecture:
        print("PE files are not native x86_64 Windows binaries:", file=sys.stderr)
        print("\n".join(wrong_architecture), file=sys.stderr)
        return 1
    if unresolved:
        print("Unresolved non-system PE imports:", file=sys.stderr)
        print("\n".join(unresolved), file=sys.stderr)
        return 1

    print("# architecture: native Windows PE x86_64")
    print("binary\timport\tclassification")
    print("\n".join(report))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
