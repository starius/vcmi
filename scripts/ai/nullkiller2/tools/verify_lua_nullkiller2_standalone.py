#!/usr/bin/env python3
"""Verify LuaNullkiller2 builds while native Nullkiller2 is disabled."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[4]

DEFAULT_CMAKE_OPTIONS = {
    "ENABLE_CLIENT": "ON",
    "ENABLE_VIDEO": "OFF",
    "ENABLE_DISCORD": "OFF",
    "ENABLE_LAUNCHER": "OFF",
    "ENABLE_EDITOR": "OFF",
    "ENABLE_TEST": "OFF",
    "ENABLE_LOBBY": "OFF",
    "ENABLE_TRANSLATIONS": "OFF",
    "ENABLE_MMAI": "OFF",
    "ENABLE_NULLKILLER2_AI": "OFF",
    "ENABLE_LUA_NULLKILLER2_AI": "ON",
    "ENABLE_STUPID_AI": "OFF",
    "ENABLE_BATTLE_AI": "OFF",
}


def run(command: list[str], cwd: Path) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, cwd=cwd, check=True)


def parse_cache(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    values: dict[str, str] = {}
    if not cache.exists():
        raise RuntimeError(f"missing CMake cache: {cache}")

    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")) or "=" not in line:
            continue
        name_type, value = line.split("=", 1)
        name = name_type.split(":", 1)[0]
        values[name] = value
    return values


def verify_cache(build_dir: Path) -> None:
    cache = parse_cache(build_dir)
    expected = {
        "ENABLE_NULLKILLER2_AI": "OFF",
        "ENABLE_LUA_NULLKILLER2_AI": "ON",
    }
    mismatches = [
        f"{name} expected {expected_value}, got {cache.get(name, '<missing>')}"
        for name, expected_value in expected.items()
        if cache.get(name) != expected_value
    ]
    if mismatches:
        raise RuntimeError("standalone LuaNullkiller2 cache check failed: " + "; ".join(mismatches))


def cmake_configure_command(args: argparse.Namespace) -> list[str]:
    command = [
        args.cmake,
        "-S",
        str(args.source_dir),
        "-B",
        str(args.build_dir),
    ]
    if args.generator:
        command.extend(["-G", args.generator])
    for key, value in DEFAULT_CMAKE_OPTIONS.items():
        command.append(f"-D{key}={value}")
    for option in args.cmake_option:
        command.append(option if option.startswith("-D") else f"-D{option}")
    return command


def cmake_build_command(args: argparse.Namespace) -> list[str]:
    command = [args.cmake, "--build", str(args.build_dir), "--target", args.target]
    if args.jobs is not None:
        command.extend(["-j", str(args.jobs)])
    return command


def main() -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Configure and build LuaNullkiller2 with ENABLE_NULLKILLER2_AI=OFF. "
            "Run this in an environment that has VCMI build dependencies installed."
        )
    )
    parser.add_argument("--source-dir", type=Path, default=REPO_ROOT, help="VCMI source tree")
    parser.add_argument("--build-dir", type=Path, required=True, help="dedicated CMake build directory")
    parser.add_argument("--target", default="LuaNullkiller2", help="CMake target to build")
    parser.add_argument("--cmake", default="cmake", help="cmake executable")
    parser.add_argument("--generator", default="Ninja", help="CMake generator; pass an empty value to omit")
    parser.add_argument("--jobs", type=int, help="parallel build jobs")
    parser.add_argument("--fresh", action="store_true", help="remove the build directory before configuring")
    parser.add_argument("--skip-audit", action="store_true", help="skip the LuaNullkiller2 source audit")
    parser.add_argument("--configure-only", action="store_true", help="configure and verify cache without building")
    parser.add_argument(
        "--cmake-option",
        action="append",
        default=[],
        help="additional -D option, for example ENABLE_CLIENT=OFF",
    )
    args = parser.parse_args()

    args.source_dir = args.source_dir.resolve()
    args.build_dir = args.build_dir.resolve()

    if args.fresh and args.build_dir.exists():
        shutil.rmtree(args.build_dir)

    try:
        if not args.skip_audit:
            audit = args.source_dir / "scripts/ai/nullkiller2/tools/audit_lua_nullkiller2.py"
            run([sys.executable, str(audit)], args.source_dir)

        run(cmake_configure_command(args), args.source_dir)
        verify_cache(args.build_dir)

        if not args.configure_only:
            run(cmake_build_command(args), args.source_dir)
    except (subprocess.CalledProcessError, RuntimeError) as exc:
        print(exc, file=sys.stderr)
        return 1

    print(f"LuaNullkiller2 standalone build gate passed for target {args.target}.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
