#!/usr/bin/env python3
"""Replay the public arm64 7.4.4 -> 7.5.1 update with the actual old helper."""

import hashlib
import os
import platform
import plistlib
import shutil
import signal
import subprocess
import tempfile
import time
from pathlib import Path

RELEASES = "https://github.com/slpkbt/SleepyGram/releases/download"
OLD_ZIP = (
    "v7.4.4/SleepyGram-macos-arm64.zip",
    "33c779ddeac5dda42635588d24872e11064df1dbcdcae86ebc4f69ef624a92f7",
)
NEW_ZIP = (
    "v7.5.1/SleepyGram-macos-arm64.zip",
    "86c4016a5ef7d124082c8b7569588e3f59d2777290bbb73a83a4604bd2f4e4e8",
)
NEW_TDUP = (
    "v7.5.1/SleepyGram-update-macos-arm64.tdup",
    "bf5d67b6ebad1f7441a70fcd1a7e340eadc33a251cae22c90a8b877e588cc9c8",
)


def fetch(spec: tuple[str, str], destination: Path) -> None:
    path, expected = spec
    subprocess.run(
        [
            "curl", "--fail", "--silent", "--show-error", "--location", "--retry", "2",
            "--proto", "=https", "--proto-redir", "=https", "--max-time", "300",
            "--output", str(destination), f"{RELEASES}/{path}",
        ],
        check=True,
    )
    with destination.open("rb") as handle:
        if hashlib.file_digest(handle, "sha256").hexdigest() != expected:
            raise RuntimeError(f"SHA-256 mismatch for {path}")


def bundle_version(app: Path) -> str:
    with (app / "Contents/Info.plist").open("rb") as handle:
        info = plistlib.load(handle)
    if info.get("CFBundleIdentifier") != "com.sleepygram.desktop":
        raise RuntimeError("Unexpected published bundle identifier.")
    return info["CFBundleVersion"]


def file_hash(path: Path) -> str:
    with path.open("rb") as handle:
        return hashlib.file_digest(handle, "sha256").hexdigest()


def assessment(app: Path, label: str) -> None:
    # Gatekeeper assessment is additional evidence, not a launch override or
    # a test of Finder quarantine and user-specific policy.
    if Path("/usr/bin/syspolicy_check").is_file():
        command = ["/usr/bin/syspolicy_check", "distribution", str(app)]
    else:
        command = ["/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=2", str(app)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=45)
    print(
        f"Distribution assessment for {label} (bundle version {bundle_version(app)}): "
        f"status {result.returncode}", flush=True,
    )
    print((result.stdout + result.stderr).strip(), flush=True)


def launched_profile_pid(executable: Path, profile: Path) -> int | None:
    prefix = f"{executable} -key sg-update-rehearsal -workdir {profile}"
    processes = subprocess.check_output(["ps", "-axww", "-o", "pid=,command="], text=True)
    for row in processes.splitlines():
        pid, _, command = row.lstrip().partition(" ")
        if command.strip() == prefix:
            return int(pid)
    return None


def main() -> None:
    if (platform.system(), platform.machine()) != ("Darwin", "arm64"):
        raise RuntimeError("Run the published updater rehearsal on macOS arm64.")
    with tempfile.TemporaryDirectory(prefix="sleepygram-published-update-") as temporary:
        root = Path(temporary).resolve()
        archive = root / "old.zip"
        package = root / "update.tdup"
        published_zip = root / "new.zip"
        fetch(OLD_ZIP, archive)
        fetch(NEW_TDUP, package)
        fetch(NEW_ZIP, published_zip)
        install = root / "install"
        install.mkdir()
        subprocess.run(["/usr/bin/ditto", "-x", "-k", str(archive), str(install)], check=True)
        app = install / "SleepyGram.app"
        executable = app / "Contents/MacOS/SleepyGram"
        published = root / "published"
        published.mkdir()
        # Compare the actual executable; both releases shipped stale plist versions.
        subprocess.run(["/usr/bin/ditto", "-x", "-k", str(published_zip), str(published)], check=True)
        expected = file_hash(published / "SleepyGram.app/Contents/MacOS/SleepyGram")
        if file_hash(executable) == expected:
            raise RuntimeError("Starting executable already matches the target release.")
        subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
        assessment(app, "published v7.4.4")
        profile = root / "profile"
        (profile / "tdata").mkdir(parents=True)
        sentinel = profile / "tdata" / "rehearsal-sentinel"
        sentinel.write_bytes(b"untouched")
        helper = root / "Updater"
        shutil.copy2(app / "Contents/Frameworks/Updater", helper)
        parent = subprocess.Popen(["/usr/bin/true"])
        if parent.wait(timeout=5) != 0:
            raise RuntimeError("Could not obtain an exited test process.")
        try:
            with (root / "updater.log").open("wb") as log:
                result = subprocess.run(
                    [
                        str(helper), "--apply", str(package), str(executable), str(profile),
                        "7004004", "7005001", str(parent.pid), "-key", "sg-update-rehearsal",
                    ],
                    cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=180,
                )
            report = (root / "updater.log").read_text(errors="replace")
            print(report[-16_384:], flush=True)
            matches = file_hash(executable) == expected
            print(f"Updater exit status {result.returncode}, matches v7.5.1 executable: {matches}", flush=True)
            if result.returncode != 0 or not matches:
                raise RuntimeError("Published upgrade did not install 7.5.1.")
            if sentinel.read_bytes() != b"untouched":
                raise RuntimeError("Published upgrade touched isolated profile data.")
            if (install / ".sleepy-update" / "transaction").exists():
                raise RuntimeError("Published upgrade did not retire its journal.")
            subprocess.run(["/usr/bin/codesign", "--verify", "--deep", "--strict", str(app)], check=True)
            assessment(app, "updated to v7.5.1")
            deadline = time.monotonic() + 10
            while launched_profile_pid(executable, profile) is None:
                if time.monotonic() >= deadline:
                    raise RuntimeError("The real updated app did not remain running after relaunch.")
                time.sleep(0.1)
            print("Published 7.4.4 -> 7.5.1 update and relaunch verified.", flush=True)
        finally:
            if (pid := launched_profile_pid(executable, profile)) is not None:
                try:
                    os.kill(pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    main()
