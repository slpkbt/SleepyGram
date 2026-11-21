from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from select_platforms import PLATFORMS, for_paths, git_diff, select


class PlatformSelectionTest(unittest.TestCase):
    def test_docs_only_and_sdk_input(self):
        self.assertEqual(for_paths(["README.md", "docs/releases/v7.5.0.md"]), set())
        self.assertEqual(for_paths(["docs/building-win.md"]), {"windows-x64"})

    def test_shared_source_and_platform_changes(self):
        self.assertEqual(for_paths(["Telegram/SourceFiles/main/main_account.cpp"]), {"linux-x64"})
        self.assertEqual(for_paths(["Telegram/SourceFiles/platform/win/tray_win.cpp"]), {"windows-x64"})
        self.assertEqual(for_paths(["Telegram/SourceFiles/platform/mac/tray_mac.mm",
                                    "Telegram/SourceFiles/main/main_account.cpp"]),
                         {"macos-arm64", "linux-x64"})
        self.assertEqual(for_paths(["Telegram/SourceFiles/platform/linux/tray_linux.cpp"]), {"linux-x64"})

    def test_headers_build_and_dependency_changes(self):
        for path in ("Telegram/SourceFiles/main/main_account.h",
                     "Telegram/SourceFiles/platform/win/CMakeLists.txt", "cmake", ".gitmodules",
                     "Telegram/lib_base", "Telegram/build/docker/centos_env/build.sh",
                     ".github/workflows/build.yml", "Telegram/ThirdParty/widget-sandbox"):
            with self.subTest(path=path):
                self.assertEqual(for_paths([path]), PLATFORMS)

    def test_event_selection(self):
        self.assertEqual(select("workflow_dispatch", {}, "all"), PLATFORMS)
        self.assertEqual(select("workflow_dispatch", {}, "macos-windows"), {"macos-arm64", "windows-x64"})
        self.assertEqual(select("workflow_dispatch", {}, "linux-macos"), {"macos-arm64", "linux-x64"})
        for platform in PLATFORMS:
            self.assertEqual(select("workflow_dispatch", {}, platform), {platform})
        self.assertEqual(select("push", {"before": "push-base"}, "", lambda _: None), PLATFORMS)
        self.assertEqual(select("push", {"before": "push-base"}, "", lambda base: [f"docs/{base}.md"]), set())
        event = {"pull_request": {"base": {"sha": "pr-base"}, "head": {"sha": "pr-head"}}}
        def diff(base):
            self.assertEqual(base, "pr-base")
            return ["Telegram/SourceFiles/platform/mac/tray_mac.mm"]
        self.assertEqual(select("pull_request", event, "", diff), {"macos-arm64"})

    def test_diff_reports_both_paths_of_a_platform_rename(self):
        with tempfile.TemporaryDirectory() as root:
            subprocess.run(["git", "init", "-q", root], check=True)
            def git(*arguments, **kwargs):
                return subprocess.run(["git", "-C", root, *arguments], check=True, **kwargs)
            old = Path(root) / "Telegram/SourceFiles/platform/mac/test.mm"
            new = Path(root) / "Telegram/SourceFiles/platform/win/test.cpp"
            old.parent.mkdir(parents=True)
            new.parent.mkdir(parents=True)
            old.write_text("int main() {}\n", encoding="utf-8")
            git("add", ".")
            git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-qm", "Base")
            base = git("rev-parse", "HEAD", capture_output=True, text=True).stdout.strip()
            git("mv", str(old), str(new))
            git("-c", "user.name=Test", "-c", "user.email=test@example.com", "commit", "-qm", "Move")
            run = subprocess.run
            with patch("select_platforms.subprocess.run",
                       side_effect=lambda args, **kwargs: run(args, cwd=root, **kwargs)):
                self.assertEqual(for_paths(git_diff(base)), {"macos-arm64", "windows-x64"})


if __name__ == "__main__":
    unittest.main()
