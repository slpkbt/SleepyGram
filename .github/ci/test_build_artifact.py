from argparse import Namespace
from hashlib import sha256
import json
import plistlib
import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch
from zipfile import ZipFile

from build_artifact import (
    MACOS_INSTALLER,
    PLATFORMS,
    _parse_gitlinks,
    submodule_revisions,
    verify_macos_version,
    verify_package,
    verify_release,
)


class ReleaseInstallerTests(unittest.TestCase):
    def test_removal_inventory_covers_only_bundle_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / PLATFORMS["macos-arm64"][0]
            files = [
                "Contents/Info.plist", "Contents/MacOS/SleepyGram",
                "Contents/Helpers/SleepyGramRemovalHelper", "Contents/Resources/Telegram.rcc",
                "Contents/Resources/SleepyGram.icns", "Contents/Resources/LICENSE-SQLCipher.txt",
                "Contents/_CodeSignature/CodeResources",
            ]
            manifest = "Contents/Resources/BuildManifest.json"
            with ZipFile(archive, "w") as package:
                for file in files:
                    package.writestr("SleepyGram.app/" + file, b"fixture")
                package.writestr("SleepyGram.app/" + manifest, json.dumps({
                    "schema_version": 1, "platform": "macos-arm64",
                    "package": {"removal_inventory": files + [manifest]},
                }))
            verify_package(archive, "macos-arm64")
            with ZipFile(archive, "a") as package:
                package.writestr("SleepyGram.app/Contents/Resources/local-export.txt", b"private")
            with self.assertRaisesRegex(RuntimeError, "differs from its removal inventory"):
                verify_package(archive, "macos-arm64")

    def test_requires_native_icon_in_the_macos_bundle(self):
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / PLATFORMS["macos-arm64"][0]
            with ZipFile(archive, "w") as package:
                package.writestr("SleepyGram.app/Contents/Info.plist", b"plist")
                package.writestr("SleepyGram.app/Contents/MacOS/SleepyGram", b"binary")
                package.writestr("SleepyGram.app/Contents/Resources/Telegram.rcc", b"resources")
                package.writestr("SleepyGram.app/Contents/Helpers/SleepyGramRemovalHelper", b"helper")
                package.writestr("SleepyGram.app/Contents/Resources/LICENSE-SQLCipher.txt", b"license")
                package.writestr(
                    "SleepyGram.app/Contents/Resources/BuildManifest.json",
                    json.dumps({"schema_version": 1, "platform": "macos-arm64"}),
                )
            with self.assertRaisesRegex(RuntimeError, r"SleepyGram\.icns"):
                verify_package(archive, "macos-arm64")

            with ZipFile(archive, "a") as package:
                package.writestr("SleepyGram.app/Contents/Resources/SleepyGram.icns", b"icns")
            self.assertEqual(verify_package(archive, "macos-arm64")["platform"], "macos-arm64")

    def test_checks_both_macos_version_fields_independently(self):
        matches = {"CFBundleShortVersionString": "7.5.2", "CFBundleVersion": "7.5.2"}
        with tempfile.TemporaryDirectory() as temporary:
            archive = Path(temporary) / PLATFORMS["macos-arm64"][0]
            for wrong in (None, "CFBundleShortVersionString", "CFBundleVersion"):
                plist = matches.copy()
                if wrong:
                    plist[wrong] = "7.4.3"
                with ZipFile(archive, "w") as package:
                    package.writestr("SleepyGram.app/Contents/Info.plist", plistlib.dumps(plist))
                if wrong:
                    with self.subTest(field=wrong):
                        with self.assertRaisesRegex(RuntimeError, wrong):
                            verify_macos_version(archive, "7.5.2")
                else:
                    verify_macos_version(archive, "7.5.2")

    def test_requires_the_dmg_verified_by_the_mac_build(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = {"commit": "a" * 40, "workflow_run_id": "42", "workflow_run_attempt": "1"}
            args = Namespace(directory=root, source_sha=source["commit"], run_id="42", run_attempt="1")
            for filename, _ in PLATFORMS.values():
                (root / filename).touch()
            with patch("build_artifact.verify_package", return_value={"source": source}):
                with self.assertRaises(FileNotFoundError):
                    verify_release(args)

                installer = root / MACOS_INSTALLER
                installer.write_bytes(b"verified disk image")
                (root / f"{MACOS_INSTALLER}.sha256").write_text(
                    sha256(installer.read_bytes()).hexdigest() + "\n",
                )
                verify_release(args)

                installer.write_bytes(b"different disk image")
                with self.assertRaisesRegex(RuntimeError, "does not match the checksum"):
                    verify_release(args)


class SubmoduleRevisionTests(unittest.TestCase):
    def test_parses_paths_with_spaces_and_parentheses_from_nul_records(self):
        sha = b"1" * 40
        output = b"160000 " + sha + b" 0\tlibraries/source (mirror)\0"

        self.assertEqual(_parse_gitlinks(output), [(b"libraries/source (mirror)", sha)])

    def test_rejects_malformed_trailing_git_index_record(self):
        sha = b"a" * 40
        output = b"160000 " + sha + b" 0\tvalid/path\0trailing garbage"

        with self.assertRaisesRegex(RuntimeError, "Could not safely parse tracked submodule paths"):
            _parse_gitlinks(output)

    def test_rejects_conflicted_gitlink_stages(self):
        sha = b"a" * 40
        other = b"b" * 40
        output = (
            b"160000 " + sha + b" 1\tmodule\0"
            b"160000 " + other + b" 2\tmodule\0"
        )

        with self.assertRaisesRegex(RuntimeError, "unresolved merge"):
            _parse_gitlinks(output)

    def test_reads_recursive_gitlinks_and_effective_dirty_revision(self):
        with tempfile.TemporaryDirectory(prefix="artifact-submodules-") as temporary:
            root = Path(temporary) / "root"
            outer_source = Path(temporary) / "outer-source"
            inner_source = Path(temporary) / "inner-source"
            _init_repo(inner_source, "inner")
            _init_repo(outer_source, "outer")
            _git(outer_source, "-c", "protocol.file.allow=always", "submodule",
                "add", "-q", str(inner_source), "nested (helper)")
            _git(outer_source, "add", ".gitmodules", "nested (helper)")
            _git(outer_source, "commit", "-qm", "add nested submodule")

            _init_repo(root, "root")
            _git(root, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                str(outer_source), "modules/outer (source)")
            _git(root, "add", ".gitmodules", "modules/outer (source)")
            _git(root, "commit", "-qm", "add outer submodule")
            _git(root, "-c", "protocol.file.allow=always", "submodule", "update",
                "--init", "--recursive")

            clean = submodule_revisions(root)
            self.assertEqual(
                {entry["path"] for entry in clean},
                {
                    "modules/outer (source)",
                    "modules/outer (source)/nested (helper)",
                },
            )
            self.assertTrue(all(entry["gitlink_match"] for entry in clean))

            checked_out_outer = root / "modules/outer (source)"
            _git(checked_out_outer, "config", "user.name", "CI Test")
            _git(checked_out_outer, "config", "user.email", "ci-test@example.invalid")
            (checked_out_outer / "change.txt").write_text("advanced\n")
            _git(checked_out_outer, "add", "change.txt")
            _git(checked_out_outer, "commit", "-qm", "advance checked-out submodule")

            dirty = submodule_revisions(root)
            dirty_outer = next(
                entry for entry in dirty
                if entry["path"] == "modules/outer (source)")
            clean_outer = next(
                entry for entry in clean
                if entry["path"] == "modules/outer (source)")
            self.assertFalse(dirty_outer["gitlink_match"])
            self.assertNotEqual(dirty_outer["actual_sha"], clean_outer["actual_sha"])

    def test_rejects_uninitialized_path_that_resolves_to_parent_repo(self):
        with tempfile.TemporaryDirectory(prefix="artifact-submodule-missing-") as temporary:
            root = Path(temporary) / "root"
            child_source = Path(temporary) / "child-source"
            _init_repo(child_source, "child")
            _init_repo(root, "root")
            _git(root, "-c", "protocol.file.allow=always", "submodule", "add", "-q",
                str(child_source), "modules/child")
            _git(root, "add", ".gitmodules", "modules/child")
            _git(root, "commit", "-qm", "add submodule")
            uninitialized = Path(temporary) / "uninitialized"
            subprocess.run(
                ["git", "clone", "-q", "--no-recurse-submodules", str(root), str(uninitialized)],
                check=True,
                stderr=subprocess.PIPE,
            )
            (uninitialized / "modules/child").mkdir(parents=True, exist_ok=True)

            with self.assertRaisesRegex(RuntimeError, "Submodule is not initialized"):
                submodule_revisions(uninitialized)

    def test_records_only_the_known_uninitialized_test262_gitlink(self):
        with tempfile.TemporaryDirectory(prefix="artifact-optional-submodule-") as temporary:
            root = Path(temporary) / "root"
            widget_source = Path(temporary) / "widget-source"
            quickjs_source = Path(temporary) / "quickjs-source"
            test262_source = Path(temporary) / "test262-source"
            path = "Telegram/ThirdParty/widget-sandbox/packages/ios/Sources/CQuickJS/quickjs/test262"

            _init_repo(test262_source, "test262")
            _init_repo(quickjs_source, "quickjs")
            _git(quickjs_source, "-c", "protocol.file.allow=always", "submodule",
                "add", "-q", str(test262_source), "test262")
            _git(quickjs_source, "config", "-f", ".gitmodules",
                "submodule.test262.update", "none")
            _git(quickjs_source, "-c", "protocol.file.allow=always", "submodule",
                "add", "-q", str(test262_source), "required")
            _git(quickjs_source, "add", ".gitmodules", "test262", "required")
            _git(quickjs_source, "commit", "-qm", "add test suite and required fixture")

            _init_repo(widget_source, "widget-sandbox")
            quickjs_path = "packages/ios/Sources/CQuickJS/quickjs"
            _git(widget_source, "-c", "protocol.file.allow=always", "submodule",
                "add", "-q", str(quickjs_source), quickjs_path)
            _git(widget_source, "add", ".gitmodules", quickjs_path)
            _git(widget_source, "commit", "-qm", "add QuickJS")

            _init_repo(root, "root")
            widget_path = "Telegram/ThirdParty/widget-sandbox"
            _git(root, "-c", "protocol.file.allow=always", "submodule",
                "add", "-q", str(widget_source), widget_path)
            _git(root, "add", ".gitmodules", widget_path)
            _git(root, "commit", "-qm", "add widget sandbox")
            _git(root, "-c", "protocol.file.allow=always", "submodule", "update",
                "--init", "--", widget_path)
            _git(root / widget_path, "-c", "protocol.file.allow=always", "submodule",
                "update", "--init", "--", quickjs_path)
            quickjs_checkout = root / widget_path / quickjs_path
            _git(quickjs_checkout, "-c", "protocol.file.allow=always", "submodule",
                "update", "--init", "--", "required")

            entry = next(item for item in submodule_revisions(root) if item["path"] == path)
            expected = _git(quickjs_source, "ls-tree", "HEAD", "test262").split()[2]
            self.assertEqual(entry, {
                "path": path,
                "expected_sha": expected,
                "actual_sha": None,
                "initialized": False,
                "gitlink_match": None,
            })

            _git(quickjs_checkout, "submodule", "deinit", "-f", "--", "required")
            with self.assertRaisesRegex(RuntimeError, "Submodule is not initialized"):
                submodule_revisions(root)


def _git(directory: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(directory), *arguments],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode:
        command = " ".join(arguments)
        raise RuntimeError(f"git {command} failed: {result.stderr.strip()}")
    return result.stdout


def _init_repo(directory: Path, name: str) -> None:
    directory.mkdir(parents=True)
    _git(directory, "init", "-q")
    _git(directory, "config", "user.name", "CI Test")
    _git(directory, "config", "user.email", "ci-test@example.invalid")
    (directory / "README").write_text(name + "\n")
    _git(directory, "add", "README")
    _git(directory, "commit", "-qm", "initial")


if __name__ == "__main__":
    unittest.main()
