#!/usr/bin/env python3
import argparse
import base64
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile

from build_artifact import PLATFORMS, safe_member


TRUST_FILES = ("root-public.pem", "manifest.min.json", "manifest.sig")
UPSTREAM_TRUST_DIR = "Telegram/Resources/update"
TEST_TRUST_DIR = f"{UPSTREAM_TRUST_DIR}/sleepygram-test"
ED25519_PUBLIC_PREFIX = bytes.fromhex("302a300506032b6570032100")
TARGETS = {
    "linux-x64": (2, 1, "linux-x64", (), "Updater"),
    "macos-arm64": (1, 2, "mac-arm", ("-arch", "arm64"),
                    "SleepyGram.app/Contents/Frameworks/Updater"),
    "windows-x64": (0, 1, "win-x64", ("-target", "win64"), "Updater.exe"),
}
TAG = re.compile(r"v(0|[1-9][0-9]*)\.(0|[1-9][0-9]{0,2})\.(0|[1-9][0-9]{0,2})")
MAX_DOWNLOAD = 256 * 1024 * 1024
MAX_UNPACKED = 1024 * 1024 * 1024
BLOCK_SIZE = 1024 * 1024


def stable_version(repo: Path) -> tuple[str, int]:
    fields = dict(line.split() for line in (repo / "Telegram/build/version").read_text().splitlines() if line.strip())
    tag = "v" + fields["AppVersionStr"]
    match = TAG.fullmatch(tag)
    if match is None or fields["BetaChannel"] != "0" or fields["AlphaVersion"] != "0":
        raise ValueError("Production update assets require a stable app version.")
    major, minor, patch = map(int, match.groups())
    version = int(fields["AppVersion"])
    if version != (major * 1000 + minor) * 1000 + patch or not 1016 < version <= 999999999:
        raise ValueError("AppVersion must encode AppVersionStr and fit the Packer version range.")
    header = (repo / "Telegram/SourceFiles/core/version.h").read_text()
    if (not re.search(rf"\bAppVersion\s*=\s*{version}\s*;", header)
            or not re.search(rf'\bAppVersionStr\s*=\s*"{re.escape(tag[1:])}"\s*;', header)):
        raise ValueError("The runtime app version differs from Telegram/build/version.")
    return tag, version


def public_der(pem: bytes) -> bytes:
    lines = pem.strip().splitlines()
    if len(lines) < 3 or lines[0] != b"-----BEGIN PUBLIC KEY-----" or lines[-1] != b"-----END PUBLIC KEY-----":
        raise ValueError("The update root must be a public key PEM.")
    result = base64.b64decode(b"".join(lines[1:-1]), validate=True)
    prefix = ED25519_PUBLIC_PREFIX
    if not result.startswith(prefix) or len(result) != len(prefix) + 32:
        raise ValueError("The update root must be an Ed25519 public key.")
    return result


def trust_bundle(repo: Path, output: Path, required: bool) -> None:
    fixtures = repo / TEST_TRUST_DIR
    bundle = os.environ.get("SLEEPYGRAM_UPDATE_PUBLIC_BUNDLE", "")
    if bundle:
        values = json.loads(bundle)
        if not isinstance(values, dict) or set(values) != set(TRUST_FILES):
            raise ValueError("Update public bundle must contain exactly the three public trust files.")
        files = {name: base64.b64decode(values[name], validate=True) for name in TRUST_FILES}
        root = public_der(files["root-public.pem"])
        manifest = json.loads(files["manifest.min.json"])
        fixture_manifest = json.loads((fixtures / "manifest.min.json").read_bytes())
        fixture_keys = {key["x"].rstrip("=") for key in fixture_manifest["keys"]}
        upstream = (repo / UPSTREAM_TRUST_DIR / "root-public.pem").read_bytes()
        if root == public_der(upstream):
            raise ValueError("The production bundle reuses the upstream update root.")
        if (root == public_der((fixtures / "root-public.pem").read_bytes())
                or any(key["x"].rstrip("=") in fixture_keys for key in manifest["keys"])):
            raise ValueError("The production bundle reuses public test keys.")
        root_x = base64.urlsafe_b64encode(root[len(ED25519_PUBLIC_PREFIX):]).decode("ascii").rstrip("=")
        if any(key["x"].rstrip("=") == root_x for key in manifest["keys"]):
            raise ValueError("The offline root key must differ from the release signing keys.")
        if len(files["manifest.sig"]) != 64:
            raise ValueError("Expected a raw 64-byte Ed25519 manifest signature.")
    elif required:
        raise ValueError("Set the SLEEPYGRAM_UPDATE_PUBLIC_BUNDLE repository variable before publishing.")
    else:
        files = {name: (fixtures / name).read_bytes() for name in TRUST_FILES}
    output.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        (output / name).write_bytes(content)


def asset_name(platform: str) -> str:
    return f"SleepyGram-update-{platform}.tdup"


def pack(args: argparse.Namespace) -> None:
    _, version = stable_version(args.repo_root)
    stage = args.stage.resolve(strict=True)
    output = args.output.resolve(strict=True)
    _, _, target_name, extra_flags, updater = TARGETS[args.platform]
    staging_root = stage.parent if args.platform == "macos-arm64" else stage
    if not (staging_root / updater).is_file():
        raise ValueError(f"The {args.platform} package is missing {updater}.")
    entries = [stage] if args.platform == "macos-arm64" else sorted(stage.iterdir())
    paths = [flag for entry in entries for flag in ("-path", str(entry))]
    name = asset_name(args.platform)
    subprocess.run([
        args.packer.resolve(strict=True),
        "-version", str(version), "-channel", "stable", *extra_flags,
        "-keys-loc", str(args.keys.resolve(strict=True)),
        "-emit-signing-input", str(output / f"{name}.signing-input"), *paths,
    ], cwd=output, check=True)
    (output / f"td-update-{target_name}-{version}.unsigned").rename(output / f"{name}.unsigned")
    verify_candidate(output, args.platform, version, args.keys, args.archive)


def read_exact(stream, size: int) -> bytes:
    data = stream.read(size)
    if len(data) != size:
        raise ValueError("Truncated update input.")
    return data


def integer(stream, fmt: str) -> int:
    return struct.unpack(fmt, read_exact(stream, struct.calcsize(fmt)))[0]


def digest_stream(stream, size: int) -> bytes:
    digest = hashlib.sha256()
    while size:
        block = read_exact(stream, min(size, BLOCK_SIZE))
        digest.update(block)
        size -= len(block)
    return digest.digest()


def package_inventory(archive: Path, platform: str) -> dict[str, tuple]:
    inventory = {}
    if PLATFORMS[platform][1] == "zip":
        with zipfile.ZipFile(archive) as package:
            for entry in package.infolist():
                if entry.is_dir() or (platform == "macos-arm64" and entry.filename.startswith("__MACOSX/")):
                    continue
                name = safe_member(entry.filename)
                mode = entry.external_attr >> 16
                if name in inventory or (mode & 0o170000) not in (0, 0o100000):
                    raise ValueError(f"Duplicate or non-regular archive member: {name}")
                executable = bool(mode & 0o111) if platform == "macos-arm64" else None
                with package.open(entry) as stream:
                    inventory[name] = (entry.file_size, digest_stream(stream, entry.file_size), executable)
    else:
        with tarfile.open(archive, "r|xz") as package:
            for entry in package:
                if entry.isdir():
                    continue
                name = safe_member(entry.name)
                if name in inventory or not entry.isfile():
                    raise ValueError(f"Duplicate or non-regular archive member: {name}")
                with package.extractfile(entry) as stream:
                    inventory[name] = (entry.size, digest_stream(stream, entry.size), bool(entry.mode & 0o111))
    if TARGETS[platform][-1] not in inventory:
        raise ValueError(f"The {platform} archive is missing its Updater helper.")
    return inventory


def unpack(stream, platform: str, output) -> None:
    windows = platform == "windows-x64"
    properties = read_exact(stream, 5) if windows else b""
    remaining = integer(stream, "<I")
    if not 0 < remaining <= MAX_UNPACKED:
        raise ValueError("The TDUP payload exceeds the updater's size limits.")
    if windows:
        codec, dictionary = struct.unpack("<BI", properties)
        lc = codec % 9
        lp = (codec // 9) % 5
        pb = codec // 45
        if not 4096 <= dictionary <= 64 * 1024 * 1024 or (lc, lp, pb) != (4, 0, 2):
            raise ValueError("Unexpected Windows LZMA properties.")
        # The SDK's LzmaCompress omits an end marker. Feed liblzma a
        # size-aware LZMA-Alone header without changing the TDUP wire format.
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE, memlimit=MAX_UNPACKED)
        decoder.decompress(properties + struct.pack("<Q", remaining))
    else:
        decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=MAX_UNPACKED)
    while chunk := stream.read(BLOCK_SIZE):
        while True:
            if decoder.eof:
                raise ValueError("Unexpected trailing compressed update data.")
            data = decoder.decompress(chunk, max_length=BLOCK_SIZE)
            remaining -= len(data)
            if remaining < 0 or decoder.unused_data:
                raise ValueError("Invalid TDUP compressed payload size.")
            output.write(data)
            if decoder.eof or decoder.needs_input:
                break
            chunk = b""
    if remaining or not decoder.eof:
        raise ValueError("Incomplete compressed update data.")
    output.seek(0)


def verify_inventory(stream, platform: str, version: int, archive: Path) -> None:
    inventory = package_inventory(archive, platform)
    if integer(stream, ">I") != version or integer(stream, ">I") != len(inventory):
        raise ValueError("TDUP version or file count differs from the packaged build.")
    for _ in range(len(inventory)):
        name_size = integer(stream, ">I")
        if not 0 < name_size <= 65536 or name_size % 2:
            raise ValueError("Invalid TDUP file name length.")
        name = read_exact(stream, name_size).decode("utf-16-be")
        size, digest, executable = inventory.pop(name, (None, None, None))
        if size is None or integer(stream, ">I") != size:
            raise ValueError(f"Unexpected TDUP file: {name}")
        array_size = integer(stream, ">I")
        if array_size != size and not (array_size == 0xFFFFFFFF and size == 0):
            raise ValueError(f"Invalid TDUP file size: {name}")
        if digest_stream(stream, size) != digest:
            raise ValueError(f"TDUP file differs from the manual archive: {name}")
        if platform != "windows-x64" and integer(stream, "B") != int(executable):
            raise ValueError(f"TDUP execute permission differs from the manual archive: {name}")
    if stream.read(1):
        raise ValueError("Unexpected trailing TDUP file entries.")


def verify_candidate(directory: Path, platform: str, version: int, keys: Path, archive: Path) -> None:
    name = asset_name(platform)
    unsigned = directory / f"{name}.unsigned"
    signing = directory / f"{name}.signing-input"
    os_id, arch_id, *_ = TARGETS[platform]
    manifest = (keys / "manifest.min.json").read_bytes()
    manifest_sig = (keys / "manifest.sig").read_bytes()
    if unsigned.stat().st_size >= MAX_DOWNLOAD - 1024 or signing.stat().st_size > len(manifest) + 64:
        raise ValueError(f"The {name} candidate exceeds the updater's download limit.")
    with unsigned.open("rb") as stream:
        header = read_exact(stream, 31)
        magic, format_, channel, target_os, arch, number, created, size = struct.unpack("<4sIBBBQQI", header)
        if (magic, format_, channel, target_os, arch, number, size) != (
                b"TDUP", 2, 0, os_id, arch_id, version << 32, len(manifest)) or not created:
            raise ValueError(f"Wrong TDUP header: {name}")
        if read_exact(stream, size) != manifest:
            raise ValueError(f"TDUP carries a different manifest: {name}")
        if integer(stream, "<I") != len(manifest_sig) or read_exact(stream, len(manifest_sig)) != manifest_sig:
            raise ValueError(f"TDUP carries a different manifest signature: {name}")
        if integer(stream, "<I") != 0:
            raise ValueError(f"TDUP candidate is already signed: {name}")
        size = integer(stream, "<I")
        pos = stream.tell()
        if not size or pos + size != unsigned.stat().st_size:
            raise ValueError(f"Wrong TDUP payload length: {name}")
        if signing.read_bytes() != header + manifest + digest_stream(stream, size):
            raise ValueError(f"Incorrect TDUP signing input: {name}")
        stream.seek(pos)
        with tempfile.TemporaryFile() as unpacked:
            unpack(stream, platform, unpacked)
            verify_inventory(unpacked, platform, version, archive)
    print(f"Verified unsigned {name} against the manual archive and trust bundle.")


def verify(args: argparse.Namespace) -> None:
    _, version = stable_version(args.repo_root)
    for platform, (name, _) in PLATFORMS.items():
        directory = args.directory / f"package-{platform}"
        verify_candidate(directory, platform, version, args.keys, directory / name)


def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare SleepyGram TDUP candidates.")
    sub = parser.add_subparsers(required=True, dest="command")
    trust = sub.add_parser("trust")
    trust.add_argument("--output", type=Path, required=True)
    trust.add_argument("--require-production", action="store_true")
    package = sub.add_parser("pack")
    package.add_argument("--platform", choices=TARGETS, required=True)
    for name in ("stage", "keys", "archive", "output", "packer"):
        package.add_argument(f"--{name}", type=Path, required=True)
    check = sub.add_parser("verify")
    check.add_argument("--directory", type=Path, required=True)
    check.add_argument("--keys", type=Path, required=True)
    for command in (trust, package, check):
        command.add_argument("--repo-root", type=Path, default=Path.cwd())
    args = parser.parse_args()
    try:
        if args.command == "trust":
            trust_bundle(args.repo_root, args.output, args.require_production)
        elif args.command == "pack":
            pack(args)
        else:
            verify(args)
    except (ValueError, OSError, lzma.LZMAError, KeyError, TypeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))


if __name__ == "__main__":
    main()
