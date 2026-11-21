#!/usr/bin/env python3
"""Write safe build manifests and verify same-run release artifacts."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import plistlib
import re
import subprocess
import sys
import tarfile
import zipfile


MANIFEST = "build-manifest.json"
MACOS_INSTALLER = "SleepyGram-macos-arm64.dmg"
PLATFORMS = {
    "linux-x64": ("SleepyGram-linux-x64.tar.xz", "tar.xz"),
    "macos-arm64": ("SleepyGram-macos-arm64.zip", "zip"),
    "windows-x64": ("SleepyGram-windows-x64.zip", "zip"),
}
MAX_MANIFEST_SIZE = 1024 * 1024
OPTIONAL_UNINITIALIZED_SUBMODULES = {
    "Telegram/ThirdParty/widget-sandbox/packages/ios/Sources/CQuickJS/quickjs/test262",
}


def command_output(command: list[str], cwd: Path | None = None) -> str | None:
    try:
        result = subprocess.run(
            command, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, check=False,
        )
    except (FileNotFoundError, OSError):
        return None
    if result.returncode not in (0, 1):
        return None
    return result.stdout.strip() or None


def first_line(command: list[str], cwd: Path | None = None) -> str | None:
    output = command_output(command, cwd)
    return next((line.strip() for line in (output or "").splitlines() if line.strip()), None)


def cmake_values(repo_root: Path, build_dir: Path) -> dict[str, str]:
    for candidate in (build_dir / "CMakeCache.txt", build_dir.parent / "CMakeCache.txt"):
        if not candidate.is_file():
            continue
        result = {}
        for line in candidate.read_text(errors="replace").splitlines():
            if line.startswith(("#", "//")) or ":" not in line or "=" not in line:
                continue
            key, _, value = line.partition("=")
            result[key.partition(":")[0]] = value
        return result
    raise RuntimeError("CMakeCache.txt was not found for the build manifest.")


def qt_version(values: dict[str, str], requested: str | None) -> tuple[str, str]:
    for key in ("QT_VERSION", "Qt6Core_VERSION", "Qt5Core_VERSION", "Qt6_VERSION", "Qt5_VERSION"):
        value = values.get(key, "").strip()
        if re.fullmatch(r"\d+\.\d+(?:\.\d+)?", value):
            if requested and value != requested:
                raise RuntimeError(f"CMake selected Qt {value}, expected {requested}.")
            return value, f"CMakeCache:{key}"
    for key, value in values.items():
        match = re.search(r"(?:^|[/\\])Qt-(\d+\.\d+(?:\.\d+)?)(?:[/\\]|$)", value)
        if match:
            version = match.group(1)
            if requested and version != requested:
                raise RuntimeError(f"CMake selected Qt {version}, expected {requested}.")
            return version, f"CMakeCache:{key}"
    if requested:
        return requested, "CI_QT_VERSION"
    raise RuntimeError("Could not determine the Qt version.")


def submodule_revisions(repo_root: Path) -> list[dict[str, str | bool | None]]:
    revisions = []
    _read_submodules(repo_root, b"", revisions)
    return revisions


def _read_submodules(
        directory: Path,
        prefix: bytes,
        revisions: list[dict[str, str | bool | None]]) -> None:
    for path, expected in _parse_gitlinks(
            _git_output(directory, "ls-files", "--stage", "-z")):
        if (
            not path
            or path.startswith(b"/")
            or any(part in (b"", b".", b"..") for part in path.split(b"/"))
            or (os.name == "nt" and b"\\" in path)
        ):
            raise RuntimeError("Unsafe tracked submodule path.")
        full_path = prefix + (b"/" if prefix else b"") + path
        decoded_path = os.fsdecode(full_path)
        child = directory / os.fsdecode(path)
        optional = decoded_path in OPTIONAL_UNINITIALIZED_SUBMODULES
        if child.is_symlink():
            raise RuntimeError(f"Submodule is not initialized: {decoded_path}")
        if optional and (
                not child.exists()
                or (child.is_dir() and not any(child.iterdir()))):
            _record_uninitialized_submodule(revisions, decoded_path, expected)
            continue
        if not child.is_dir():
            raise RuntimeError(f"Submodule is not initialized: {decoded_path}")

        child_prefix = _git_output(child, "rev-parse", "--show-prefix").rstrip(b"\r\n")
        if child_prefix:
            raise RuntimeError(f"Submodule is not initialized: {decoded_path}")
        actual = _git_output(child, "rev-parse", "--verify", "HEAD^{commit}").strip()
        if not re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", actual):
            raise RuntimeError(f"Could not read submodule revision: {decoded_path}")

        revisions.append({
            "path": decoded_path,
            "expected_sha": expected.decode("ascii"),
            "actual_sha": actual.decode("ascii"),
            "initialized": True,
            "gitlink_match": actual == expected,
        })
        _read_submodules(child, full_path, revisions)


def _record_uninitialized_submodule(
        revisions: list[dict[str, str | bool | None]],
        path: str,
        expected: bytes) -> None:
    revisions.append({
        "path": path,
        "expected_sha": expected.decode("ascii"),
        "actual_sha": None,
        "initialized": False,
        "gitlink_match": None,
    })


def _parse_gitlinks(output: bytes) -> list[tuple[bytes, bytes]]:
    if output and not output.endswith(b"\0"):
        raise RuntimeError("Could not safely parse tracked submodule paths.")
    entries = []
    seen = set()
    for record in output.split(b"\0"):
        if not record:
            continue
        header, separator, path = record.partition(b"\t")
        fields = header.split(b" ")
        if not separator or len(fields) != 3:
            raise RuntimeError("Could not safely parse tracked submodule path.")
        mode, revision, stage = fields
        if mode != b"160000":
            continue
        if not re.fullmatch(rb"[0-9a-f]{40}|[0-9a-f]{64}", revision):
            raise RuntimeError("Could not safely parse tracked submodule revision.")
        if stage != b"0":
            raise RuntimeError(f"Submodule has an unresolved merge: {os.fsdecode(path)}")
        if path in seen:
            raise RuntimeError("Duplicate tracked submodule path.")
        seen.add(path)
        entries.append((path, revision))
    return entries


def _git_output(directory: Path, *arguments: str) -> bytes:
    result = subprocess.run(
        ["git", *arguments],
        cwd=directory,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        raise RuntimeError("Could not read git submodule metadata.")
    return result.stdout


def submodule_tree_fingerprint(repo_root: Path) -> str:
    status = command_output(["git", "submodule", "status", "--recursive"], repo_root) or ""
    values = []
    for line in status.splitlines():
        match = re.match(r"^([ +\-U])([0-9a-f]{40})\s+(\S+)", line)
        if not match:
            continue
        _marker, revision, path = match.groups()
        tree = command_output(["git", "-C", path, "rev-parse", "HEAD^{tree}"], repo_root)
        values.append(f"{path}:{tree or revision}")
    return "\n".join(values)


def create_manifest(args: argparse.Namespace) -> None:
    repo_root = args.repo_root.resolve()
    build_dir = args.build_dir.resolve()
    actual_sha = command_output(["git", "rev-parse", "HEAD"], repo_root)
    source_sha = os.environ.get("GITHUB_SHA", "").strip() or actual_sha
    if not source_sha or not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        raise RuntimeError("The source commit SHA is missing or malformed.")
    if actual_sha and actual_sha != source_sha:
        raise RuntimeError("Checked out source does not match GITHUB_SHA.")

    cache = cmake_values(repo_root, build_dir)
    detected_qt, qt_source = qt_version(cache, args.qt_version)
    if args.platform == "windows-x64":
        compiler = command_output(["cl", "/Bv"])
    elif args.platform == "linux-x64":
        compiler = os.environ.get("CI_CONTAINER_CXX")
    else:
        compiler = command_output(["c++", "--version"])
    compiler_lines = [line.strip() for line in (compiler or "").splitlines() if line.strip()]
    toolchain = {
        "runner": {
            "os": os.environ.get("RUNNER_OS", platform.system()),
            "architecture": os.environ.get("RUNNER_ARCH", platform.machine()),
            "image": os.environ.get("ImageOS"),
        },
        "qt": {"version": detected_qt, "source": qt_source},
        "cmake": {
            "version": os.environ.get("CI_CONTAINER_CMAKE") if args.platform == "linux-x64" else first_line(["cmake", "--version"]),
            "generator": cache.get("CMAKE_GENERATOR"),
            "compiler_id": cache.get("CMAKE_CXX_COMPILER_ID"),
            "compiler_version": cache.get("CMAKE_CXX_COMPILER_VERSION"),
            "compiler_runtime": " | ".join(compiler_lines[:3]) or None,
            "osx_architectures": cache.get("CMAKE_OSX_ARCHITECTURES"),
        },
        "container_image": args.container_image or None,
        "windows_sdk": os.environ.get("WINDOWS_SDK_VERSION"),
        "visual_studio": os.environ.get("VisualStudioVersion"),
        "xcode": command_output(["xcodebuild", "-version"]) if args.platform == "macos-arm64" else None,
        "macos_sdk": first_line(["xcrun", "--show-sdk-version"]) if args.platform == "macos-arm64" else None,
    }
    manifest = {
        "schema_version": 1,
        "platform": args.platform,
        "source": {
            "commit": source_sha,
            "workflow_run_id": os.environ.get("GITHUB_RUN_ID", "local"),
            "workflow_run_attempt": os.environ.get("GITHUB_RUN_ATTEMPT", "1"),
        },
        "submodules": submodule_revisions(repo_root),
        "toolchain": toolchain,
        "package": {"archive": PLATFORMS[args.platform][0]},
    }
    output = args.output.resolve()
    if args.macos_bundle is not None:
        bundle = args.macos_bundle.resolve()
        if args.platform != "macos-arm64" or bundle.name != "SleepyGram.app":
            raise RuntimeError("Expected a macOS SleepyGram bundle for the removal inventory.")
        contents = bundle / "Contents"
        if output != contents / "Resources" / "BuildManifest.json" or not contents.is_dir():
            raise RuntimeError("The removal inventory must be sealed into the bundle manifest.")
        inventory = {"Contents/Resources/BuildManifest.json", "Contents/_CodeSignature/CodeResources"}
        for entry in contents.rglob("*"):
            if entry.is_symlink() or not (entry.is_file() or entry.is_dir()):
                raise RuntimeError("The removal inventory requires regular files and directories.")
            if entry.is_file():
                inventory.add(safe_member(entry.relative_to(bundle).as_posix()))
        if len(inventory) > 2048:
            raise RuntimeError("The removal inventory exceeds the supported file count.")
        manifest["package"]["removal_inventory"] = sorted(inventory)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print(f"Wrote {output.name} for {args.platform} at {source_sha}")


def safe_member(name: str) -> str:
    path = PurePosixPath(name)
    if path.is_absolute() or ".." in path.parts or not path.parts:
        raise RuntimeError(f"Unsafe archive path: {name}")
    return path.as_posix()


def archive_members(archive: Path, platform_name: str) -> tuple[set[str], bytes]:
    if PLATFORMS[platform_name][1] == "zip":
        with zipfile.ZipFile(archive) as package:
            names = {safe_member(item.filename) for item in package.infolist() if not item.is_dir()}
            manifest_path = "SleepyGram.app/Contents/Resources/BuildManifest.json" if platform_name == "macos-arm64" else MANIFEST
            info = package.getinfo(manifest_path)
            if info.file_size > MAX_MANIFEST_SIZE:
                raise RuntimeError("Artifact manifest exceeds the 1 MiB safety limit.")
            manifest = package.read(info)
            return names, manifest
    with tarfile.open(archive, "r:xz") as package:
        names = {safe_member(item.name) for item in package.getmembers() if item.isfile()}
        info = package.getmember(MANIFEST)
        if not info.isfile() or info.size > MAX_MANIFEST_SIZE:
            raise RuntimeError("Linux artifact manifest is missing or too large.")
        stream = package.extractfile(info)
        if stream is None:
            raise RuntimeError("Could not read Linux artifact manifest.")
        return names, stream.read()


def verify_macos_version(archive: Path, expected: str) -> None:
    with zipfile.ZipFile(archive) as package:
        info = package.getinfo("SleepyGram.app/Contents/Info.plist")
        if info.file_size > MAX_MANIFEST_SIZE:
            raise RuntimeError("macOS Info.plist exceeds the 1 MiB safety limit.")
        try:
            plist = plistlib.loads(package.read(info))
        except (ValueError, plistlib.InvalidFileException) as error:
            raise RuntimeError("Invalid macOS Info.plist.") from error
    if not isinstance(plist, dict):
        raise RuntimeError("Invalid macOS Info.plist dictionary.")
    for field in ("CFBundleShortVersionString", "CFBundleVersion"):
        if plist.get(field) != expected:
            raise RuntimeError(f"macOS {field} differs from runtime version {expected}.")


def verify_package(
    archive: Path, platform_name: str, macos_version: str | None = None,
) -> dict[str, object]:
    if archive.name != PLATFORMS[platform_name][0]:
        raise RuntimeError(f"Expected artifact name {PLATFORMS[platform_name][0]}.")
    names, manifest_bytes = archive_members(archive, platform_name)
    if platform_name == "linux-x64":
        required = {"SleepyGram", "SleepyGramRemovalHelper", MANIFEST, "LICENSE-SQLCipher.txt"}
        allowed = required | {"Updater"}
    elif platform_name == "windows-x64":
        required = {"SleepyGram.exe", "SleepyGramRemovalHelper.exe", MANIFEST, "LICENSE-SQLCipher.txt"}
        allowed = required | {"Updater.exe", "modules/x64/d3d/d3dcompiler_47.dll"}
    else:
        required = {
            "SleepyGram.app/Contents/Info.plist",
            "SleepyGram.app/Contents/Resources/Telegram.rcc",
            "SleepyGram.app/Contents/Helpers/SleepyGramRemovalHelper",
            "SleepyGram.app/Contents/Resources/SleepyGram.icns",
            "SleepyGram.app/Contents/Resources/BuildManifest.json",
            "SleepyGram.app/Contents/Resources/LICENSE-SQLCipher.txt",
        }
        if not any(name.startswith("SleepyGram.app/Contents/MacOS/") for name in names):
            raise RuntimeError("macOS archive is missing the app executable.")
        allowed = {
            name for name in names
            if name.startswith("SleepyGram.app/") or name.startswith("__MACOSX/")
        }
    missing = required - names
    if missing:
        raise RuntimeError("Artifact is missing required files: " + ", ".join(sorted(missing)))
    if platform_name != "macos-arm64":
        unexpected = names - allowed
        if unexpected:
            raise RuntimeError("Unexpected artifact files: " + ", ".join(sorted(unexpected)[:8]))
    try:
        manifest = json.loads(manifest_bytes)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise RuntimeError("Artifact manifest is not valid JSON.") from error
    if manifest.get("schema_version") != 1 or manifest.get("platform") != platform_name:
        raise RuntimeError("Artifact manifest does not match its platform.")
    if macos_version is not None:
        if platform_name != "macos-arm64" or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", macos_version):
            raise RuntimeError("Expected a numeric three-component macOS bundle version.")
        verify_macos_version(archive, macos_version)
    package = manifest.get("package", {})
    if platform_name == "macos-arm64" and isinstance(package, dict) and "removal_inventory" in package:
        inventory = package["removal_inventory"]
        if not isinstance(inventory, list) or len(inventory) > 2048 or any(
            not isinstance(file, str) or not file.startswith("Contents/") or safe_member(file) != file
            for file in inventory
        ):
            raise RuntimeError("Invalid removal inventory for the macOS bundle.")
        actual = {name.removeprefix("SleepyGram.app/") for name in names if name.startswith("SleepyGram.app/")}
        if len(inventory) != len(actual) or set(inventory) != actual:
            raise RuntimeError("The macOS bundle differs from its removal inventory.")
    return manifest


def verify_release(args: argparse.Namespace) -> None:
    archives = [path for path in args.directory.resolve().rglob("*") if path.is_file() and path.name in {
        spec[0] for spec in PLATFORMS.values()
    }]
    if len(archives) != len(PLATFORMS):
        raise RuntimeError("Release requires exactly one archive for each of the three platforms.")
    by_name = {path.name: path for path in archives}
    if len(by_name) != len(archives):
        raise RuntimeError("Duplicate platform archive found in downloaded artifacts.")
    expected = {
        "commit": args.source_sha,
        "workflow_run_id": str(args.run_id),
        "workflow_run_attempt": str(args.run_attempt),
    }
    for platform_name, (filename, _kind) in PLATFORMS.items():
        if filename not in by_name:
            raise RuntimeError(f"Release artifact is missing: {filename}")
        manifest = verify_package(by_name[filename], platform_name)
        source = manifest.get("source", {})
        for key, value in expected.items():
            if str(source.get(key, "")) != value:
                raise RuntimeError(f"{filename} does not match this run's {key}.")
    macos_package = by_name[PLATFORMS["macos-arm64"][0]].parent
    installer = macos_package / MACOS_INSTALLER
    checksum = macos_package / f"{MACOS_INSTALLER}.sha256"
    digest = hashlib.sha256()
    with installer.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    if checksum.read_text(encoding="ascii").strip() != digest.hexdigest():
        raise RuntimeError("The macOS installer does not match the checksum from the build job.")
    print("Verified all three packages came from this workflow run and source SHA.")


def cache_key(args: argparse.Namespace) -> None:
    repo_root = args.repo_root.resolve()
    digest = hashlib.sha256()
    digest.update(args.platform.encode())
    digest.update(os.environ.get("CI_QT_VERSION", "6.11.2").encode())
    digest.update(os.environ.get("WINDOWS_SDK_VERSION", "").encode())
    digest.update(os.environ.get("ImageOS", "").encode())
    digest.update(os.environ.get("RUNNER_OS", "").encode())
    digest.update(os.environ.get("RUNNER_ARCH", "").encode())
    inputs = [".gitmodules"]
    if args.platform == "linux-x64":
        inputs += ["Telegram/build/docker/centos_env", "Telegram/build/prepare/patches"]
    else:
        inputs += ["Telegram/build/prepare", "Telegram/build/qt_version.py"]
        inputs += ["Telegram/build/sync_ayu_submodules.py"]
    # Use tracked paths, but hash the rendered Dockerfile from the working tree.
    files = subprocess.check_output(["git", "ls-files", "-z", "--", *inputs], cwd=repo_root)
    for name in sorted(set(files.split(b"\0")) - {b""}):
        path = repo_root / os.fsdecode(name)
        if not path.is_file():
            continue
        digest.update(name)
        digest.update(path.read_bytes())
    if args.platform != "linux-x64":
        digest.update(submodule_tree_fingerprint(repo_root).encode())
    tool_versions = ["cmake --version", "ninja --version"]
    if args.platform == "macos-arm64":
        tool_versions += ["clang++ --version", "xcodebuild -version", "xcrun --show-sdk-version"]
    elif args.platform == "windows-x64":
        tool_versions += ["cl /Bv", "where cmake"]
    else:
        tool_versions += ["docker version --format {{.Server.Version}}"]
    for command in tool_versions:
        digest.update(command.encode())
        digest.update((command_output(command.split(), repo_root) or "missing").encode())
    value = digest.hexdigest()
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with Path(output).open("a") as file:
            file.write(f"key={value}\n")
    print(value)


def parser() -> argparse.ArgumentParser:
    root = argparse.ArgumentParser(description=__doc__)
    commands = root.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create", help="write a package build manifest")
    create.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    create.add_argument("--repo-root", type=Path, required=True)
    create.add_argument("--build-dir", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--qt-version")
    create.add_argument("--container-image")
    create.add_argument("--macos-bundle", type=Path)
    create.set_defaults(function=create_manifest)
    package = commands.add_parser("verify-package", help="check archive structure and manifest")
    package.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    package.add_argument("--archive", type=Path, required=True)
    package.add_argument("--macos-version", help="Expected version in both macOS Info.plist keys.")
    package.set_defaults(function=lambda args: verify_package(
        args.archive.resolve(), args.platform, args.macos_version,
    ))
    release = commands.add_parser("verify-release", help="check complete same-run package set")
    release.add_argument("--directory", type=Path, required=True)
    release.add_argument("--source-sha", required=True)
    release.add_argument("--run-id", required=True)
    release.add_argument("--run-attempt", required=True)
    release.set_defaults(function=verify_release)
    cache = commands.add_parser("cache-key", help="calculate an input-sensitive cache key")
    cache.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
    cache.add_argument("--repo-root", type=Path, required=True)
    cache.set_defaults(function=cache_key)
    return root


def main() -> int:
    try:
        args = parser().parse_args()
        args.function(args)
    except (OSError, RuntimeError, subprocess.CalledProcessError, tarfile.TarError, zipfile.BadZipFile, KeyError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
