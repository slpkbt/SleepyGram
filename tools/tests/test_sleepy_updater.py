#!/usr/bin/env python3
"""Exercise the test-trust Updater with isolated native fixture installations."""

import argparse
import ctypes
import hashlib
import lzma
import os
from pathlib import Path
import platform
import plistlib
import re
import shutil
import struct
import sys
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
TRUST = ROOT / "Telegram/Resources/update/sleepygram-test"
SIGNER = ROOT / "tools/tests/fixtures/sleepygram-update-leaf.pem"
KEY_ID = b"sleepygram-ci-fixture"
APP_SOURCE = r"""
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <wchar.h>
typedef wchar_t arg_t;
#define ARG(text) L##text
#define OPEN _wfopen
#define CMP wcscmp
#define PATH_PRINT(destination, size, folder, name) \
    swprintf(destination, size, L"%ls/%ls", folder, name)
#define ENTRY wmain
#define WAIT() Sleep(20)
#else
#include <unistd.h>
typedef char arg_t;
#define ARG(text) text
#define OPEN fopen
#define CMP strcmp
#define PATH_PRINT(destination, size, folder, name) \
    snprintf(destination, size, "%s/%s", folder, name)
#define ENTRY main
#define WAIT() usleep(20000)
#endif

static int marker(const arg_t *directory, const arg_t *name, int write) {
    arg_t path[8192];
    const int length = PATH_PRINT(path, 8192, directory, name);
    if (length < 0 || length >= 8192) return 0;
    FILE *file = OPEN(path, write ? ARG("wb") : ARG("rb"));
    if (file) {
        if (write) fputs(GENERATION, file);
        fclose(file);
    }
    return file != NULL;
}

int ENTRY(int argc, arg_t **argv) {
    const arg_t *probe = NULL;
    const arg_t *workdir = NULL;
    const arg_t *key = NULL;
    for (int i = 1; i + 1 < argc; ++i) {
        if (!CMP(argv[i], ARG("-sleepy-update-probe"))) probe = argv[i + 1];
        if (!CMP(argv[i], ARG("-workdir"))) workdir = argv[i + 1];
        if (!CMP(argv[i], ARG("-key"))) key = argv[i + 1];
    }
    if (!key || CMP(key, ARG("profile-test")) || !workdir) return 1;
    if (!probe) return !marker(workdir, ARG("relaunch"), 1);
    if (!strcmp(GENERATION, "bad")) {
        for (int i = 0; i != 10; ++i) WAIT();
        return 2;
    }
    if (!marker(probe, ARG("ready"), 1)) return 3;
    for (int i = 0; i != 500; ++i) {
        if (marker(probe, ARG("abort"), 0)) return 4;
        if (strcmp(GENERATION, "paused") && marker(probe, ARG("commit"), 0)) {
            return !marker(probe, ARG("released"), 1);
        }
        WAIT();
    }
    return 5;
}
"""


def sized(data: bytes) -> bytes:
    return struct.pack("<I", len(data)) + data


def signed_package(source: Path, package: Path, version: int, target, openssl: str) -> None:
    manifest = (TRUST / "manifest.min.json").read_bytes()
    manifest_sig = (TRUST / "manifest.sig").read_bytes()
    files = [path for path in source.rglob("*") if path.is_file()]
    raw = bytearray(struct.pack(">II", version, len(files)))
    for path in sorted(files):
        name = path.relative_to(source).as_posix().encode("utf-16-be")
        content = path.read_bytes()
        raw.extend(struct.pack(">I", len(name)) + name)
        raw.extend(struct.pack(">II", len(content), len(content)) + content)
        raw.extend(struct.pack("B", bool(path.stat().st_mode & 0o111)))
    payload = struct.pack("<I", len(raw)) + lzma.compress(raw)
    region = struct.pack("<4sIBBBQQ", b"TDUP", 2, 0, *target, version << 32, 1)
    region += sized(manifest)
    signing = package.with_suffix(".signing")
    signing.write_bytes(region + hashlib.sha256(payload).digest())
    signature = subprocess.run(
        [openssl, "pkeyutl", "-sign", "-rawin", "-inkey", str(SIGNER), "-in", str(signing)],
        check=True, capture_output=True,
    ).stdout
    package.write_bytes(region + sized(manifest_sig) + struct.pack("<I", 1)
                        + sized(KEY_ID) + sized(signature) + sized(payload))


def pack_windows(source: Path, package: Path, version: int, wrong_target: bool,
                 packer: Path) -> None:
    target = "winarm" if wrong_target else "win64"
    arch = "arm" if wrong_target else "x64"
    entries = [option for path in sorted(source.iterdir()) for option in ("-path", str(path))]
    result = subprocess.run(
        [str(packer), "-target", target, "-version", str(version), "-channel", "stable",
         "-keys-loc", str(TRUST), "-local-key", str(SIGNER),
         "-local-key-id", KEY_ID.decode(), *entries],
        cwd=package.parent, capture_output=True,
    )
    assert result.returncode == 0, result.stdout.decode(errors="replace")
    (package.parent / f"td-update-win-{arch}-{version}").replace(package)


def wait_for(path: Path, timeout: int = 15) -> bytes:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file() and (data := path.read_bytes()):
            return data
        time.sleep(0.02)
    raise AssertionError(f"Timed out waiting for {path.name}")


def set_linux_subreaper(enabled: bool) -> None:
    pr_set_child_subreaper = 36
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.prctl(pr_set_child_subreaper, ctypes.c_ulong(enabled), 0, 0, 0) != 0:
        error = ctypes.get_errno()
        raise OSError(error, os.strerror(error))


def exercise(helper: Path, openssl: str, cc: str, packer: Path | None = None) -> None:
    system = platform.system()
    mac = system == "Darwin"
    windows = system == "Windows"
    arch = platform.machine()
    if not (mac or windows or (system == "Linux" and arch == "x86_64")):
        raise RuntimeError("The native fixture supports macOS, Windows x64 and Linux x86-64.")
    if windows and packer is None:
        raise RuntimeError("The Windows fixture requires --packer.")
    version_file = ROOT / "Telegram/SourceFiles/core/version.h"
    running = int(re.search(r"AppVersion = (\d+)", version_file.read_text()).group(1))
    target = (1, 2 if arch == "arm64" else 1) if mac else (2, 1)
    suffix = ".exe" if windows else ""
    relative_exe = Path("SleepyGram.app/Contents/MacOS/SleepyGram" if mac else "SleepyGram" + suffix)
    relative_helper = Path("SleepyGram.app/Contents/Frameworks/Updater" if mac else "Updater" + suffix)
    with tempfile.TemporaryDirectory(prefix="sleepy-updater-test-") as temporary:
        base = Path(temporary).resolve()
        app_source = base / "fixture.c"
        parents = subprocess.Popen([sys.executable, "-c", "pass"])
        assert parents.wait() == 0
        dead_pid = str(parents.pid)
        generations = {}
        for generation in ("old", "new", "bad", "paused"):
            source = base / generation
            source.mkdir()
            app = source / relative_exe
            app.parent.mkdir(parents=True, exist_ok=True)
            app_source.write_text(f'#define GENERATION "{generation}"\n' + APP_SOURCE)
            if windows:
                command = [cc, "/nologo", "/O2", f"/Fe{app}", f"/Fo{base / 'fixture.obj'}",
                           str(app_source)]
            else:
                command = [cc, str(app_source), "-o", str(app)]
            subprocess.run(command, check=True, cwd=base, capture_output=True)
            updater = source / relative_helper
            updater.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(helper, updater)
            if mac:
                bundle = source / "SleepyGram.app"
                plist = dict(CFBundleIdentifier="com.sleepygram.desktop",
                             CFBundleExecutable="SleepyGram", CFBundlePackageType="APPL")
                with (bundle / "Contents/Info.plist").open("wb") as file:
                    plistlib.dump(plist, file)
                subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(bundle)],
                               check=True, capture_output=True)
            else:
                (source / "LICENSE-SQLCipher.txt").write_text(generation)
                if windows and generation != "old":
                    module = source / "modules/x64/d3d/d3dcompiler_47.dll"
                    module.parent.mkdir(parents=True)
                    module.write_bytes(b"module")
            generations[generation] = source

        if mac:
            install = base / "ordinary-restart"
            install.mkdir()
            bundle = install / "Renamed SleepyGram.app"
            shutil.copytree(generations["old"] / "SleepyGram.app", bundle)
            executable = bundle / "Contents/MacOS/SleepyGram"
            workdir = install / "profile"
            workdir.mkdir()
            command = [str(helper), "--restart", str(executable), str(workdir), dead_pid,
                       "profile-test", "-debug", "-tosettings"]
            subprocess.run(command, check=True, capture_output=True, timeout=45)
            assert wait_for(workdir / "relaunch") == b"old"
            assert not (install / ".sleepy-update").exists()
            print("ordinary_restart: ok", flush=True)

        scenarios = [
            ("commit", "new", b"new", 0),
            ("failed_probe", "bad", b"old", 1),
            ("wrong_target", "new", b"old", 1),
            ("tampering", "new", b"old", 1),
            ("downgrade", "new", b"old", 1),
            ("unexpected_file", "new", b"old", 1),
            ("linked_destination", "new", b"old", 1),
            ("intent_recovery", "new", b"old", 0),
            ("crash_recovery", "paused", b"old", 0),
        ]
        if windows:
            scenarios.append(("restore_existing_module", "bad", b"old", 1))
        for scenario, generation, expected, exitcode in scenarios:
            if windows and scenario == "linked_destination":
                continue
            install = base / scenario
            shutil.copytree(generations["old"], install)
            workdir = install / "profile"
            (workdir / "tdata").mkdir(parents=True)
            profile_file = workdir / "tdata" / "identity"
            profile_file.write_bytes(b"profile")
            neighbor = install / "neighbor.txt"
            neighbor.write_bytes(b"neighbor")
            module = install / "modules/x64/d3d/d3dcompiler_47.dll"
            if scenario == "restore_existing_module":
                module.parent.mkdir(parents=True)
                module.write_bytes(b"previous module")
                (module.parent / "custom.txt").write_bytes(b"custom")
            link = None
            if scenario == "linked_destination":
                link = (install / "SleepyGram.app/Contents/Link" if mac
                        else install / "LICENSE-SQLCipher.txt")
                if not mac:
                    link.unlink()
                link.symlink_to(neighbor)
            executable = install / relative_exe
            transaction = install / ".sleepy-update" / "transaction"
            package = base / f"{scenario}.tdup"
            requested = running if scenario == "downgrade" else running + 1
            signed_target = (0, 1) if scenario == "wrong_target" else target
            source = generations[generation]
            if scenario == "unexpected_file":
                source = base / "invalid-layout"
                shutil.copytree(generations["new"], source)
                (source / "tdata").mkdir()
                (source / "tdata" / "identity").write_bytes(b"replacement")
            if windows:
                pack_windows(source, package, requested, scenario == "wrong_target", packer)
            else:
                signed_package(source, package, requested, signed_target, openssl)
            if scenario == "tampering":
                data = bytearray(package.read_bytes())
                data[-1] ^= 1
                package.write_bytes(data)
            command = [str(helper), "--apply", str(package), str(executable), str(workdir),
                       str(running), str(requested), dead_pid, "-key", "profile-test"]
            orphan_probe = None
            keep_zombie = sys.platform == "linux" and scenario == "crash_recovery"
            if scenario in ("intent_recovery", "crash_recovery"):
                parent = None
                try:
                    if keep_zombie:
                        set_linux_subreaper(True)
                    if scenario == "intent_recovery":
                        parent = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(60)"])
                        command[7] = str(parent.pid)
                    process = subprocess.Popen(command, stderr=subprocess.PIPE)
                    try:
                        wait_for(transaction / ("journal" if parent else "commit"))
                        if parent:
                            assert not (transaction / "stage").exists()
                        elif keep_zombie:
                            orphan_probe = int(wait_for(transaction / "probe-pid"))
                    finally:
                        process.kill()
                        process.wait(timeout=10)
                        process.stderr.close()
                finally:
                    if parent:
                        parent.terminate()
                        parent.wait(timeout=10)
                assert executable.is_file()
                assert not (workdir / "relaunch").exists()
                command = [str(helper), "--recover", str(executable), "", dead_pid]
            try:
                result = subprocess.run(command, capture_output=True, timeout=45)
                if orphan_probe is not None:
                    # Keep the exited probe unreaped until recovery finishes.
                    reaped, status = os.waitpid(orphan_probe, os.WNOHANG)
                    assert reaped == orphan_probe and os.waitstatus_to_exitcode(status) == 4
            finally:
                if keep_zombie:
                    set_linux_subreaper(False)
            diagnostic = result.stderr.decode(errors="replace")
            assert result.returncode == exitcode, (scenario, diagnostic)
            if scenario == "failed_probe":
                assert "exited before readiness (status 2)" in diagnostic, diagnostic
                assert "Restarted the installed app without confirming this update." in diagnostic, diagnostic
            assert wait_for(workdir / "relaunch") == expected, scenario
            assert profile_file.read_bytes() == b"profile" and neighbor.read_bytes() == b"neighbor"
            assert not transaction.exists(), scenario
            if link:
                assert link.is_symlink()
            elif not mac:
                assert (install / "LICENSE-SQLCipher.txt").read_bytes() == expected
            if windows:
                if scenario == "commit":
                    assert module.read_bytes() == b"module"
                elif scenario == "restore_existing_module":
                    assert module.read_bytes() == b"previous module"
                    assert (module.parent / "custom.txt").read_bytes() == b"custom"
                else:
                    assert not (install / "modules").exists(), scenario
            print(f"{scenario}: ok", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--openssl", default="openssl")
    parser.add_argument("--cc", default="cl" if os.name == "nt" else "cc")
    parser.add_argument("--packer", type=Path)
    args = parser.parse_args()
    packer = args.packer.resolve(strict=True) if args.packer else None
    exercise(args.helper.resolve(strict=True), args.openssl, args.cc, packer)


if __name__ == "__main__":
    main()
