#!/usr/bin/env python3
"""Select platform checks from the diff against the actual checkout."""

import json
import os
from pathlib import Path, PurePosixPath
import subprocess
import sys


PLATFORMS = {"linux-x64", "macos-arm64", "windows-x64"}
SOURCE_PLATFORMS = {"linux": "linux-x64", "mac": "macos-arm64", "win": "windows-x64"}
MANUAL = {"all": PLATFORMS, "linux-macos": {"linux-x64", "macos-arm64"},
          "macos-windows": {"macos-arm64", "windows-x64"}}


def for_paths(paths):
    selected = set()
    for path in paths:
        if PurePosixPath(path).name == "CMakeLists.txt" or path.endswith(
                (".cmake", ".h", ".hh", ".hpp", ".hxx", ".inc", ".inl", ".ipp", ".tpp")):
            return PLATFORMS.copy()
        if path == "docs/building-win.md":
            selected.add("windows-x64")
        elif path in {"README.md", "README-RU.md", "AGENTS.md", ".github/ci/README.md",
                      ".github/CONTRIBUTING.md"} or path.startswith("docs/"):
            continue
        elif path.startswith((".github/", "Telegram/build/", "Telegram/ThirdParty/",
                              "Telegram/cmake/", "cmake/")) or path in {".gitmodules", "cmake"}:
            return PLATFORMS.copy()
        elif path.startswith("Telegram/Resources/winrc/"):
            selected.add("windows-x64")
        elif path.startswith("Telegram/SourceFiles/"):
            parts = PurePosixPath(path).parts
            if len(parts) > 4 and parts[2] == "platform" and parts[3] in SOURCE_PLATFORMS:
                selected.add(SOURCE_PLATFORMS[parts[3]])
            elif path.endswith(("_mac.mm", "_osx.m")):
                selected.add("macos-arm64")
            elif path.endswith("_win.cpp"):
                selected.add("windows-x64")
            else:
                selected.add("linux-x64")
        else:
            return PLATFORMS.copy()
    return selected


def git_diff(base):
    if not base or base == "0" * 40:
        return None
    if subprocess.run(["git", "cat-file", "-e", f"{base}^{{commit}}"],
                      stderr=subprocess.DEVNULL).returncode:
        if subprocess.run(["git", "fetch", "--no-tags", "--depth=1", "origin", base]).returncode:
            return None
    result = subprocess.run(["git", "diff", "--no-renames", "--name-only", "-z", base, "HEAD", "--"],
                            stdout=subprocess.PIPE)
    if result.returncode:
        return None
    return [os.fsdecode(path) for path in result.stdout.split(b"\0") if path]


def select(event_name, event, manual, diff=git_diff):
    if event_name == "workflow_dispatch":
        if manual in MANUAL:
            return MANUAL[manual].copy()
        if manual in PLATFORMS:
            return {manual}
        raise ValueError(f"Unknown platform selection: {manual}")
    if event_name == "pull_request":
        base = event["pull_request"]["base"]["sha"]
    elif event_name == "push":
        base = event["before"]
    else:
        return PLATFORMS.copy()
    paths = diff(base)
    if paths is None:
        print("::warning::Could not compare Git revisions. Running all platform checks.")
        return PLATFORMS.copy()
    return for_paths(paths)


def main():
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text(encoding="utf-8"))
    selected = select(os.environ["GITHUB_EVENT_NAME"], event, os.environ.get("BUILD_PLATFORM"))
    print("Platform checks:", ", ".join(sorted(selected)) or "none (docs only or empty diff)")
    with Path(os.environ["GITHUB_OUTPUT"]).open("a", encoding="utf-8") as output:
        for platform in sorted(PLATFORMS):
            print(f"{platform}={str(platform in selected).lower()}", file=output)


if __name__ == "__main__":
    sys.exit(main())
