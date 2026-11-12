#!/usr/bin/env python3
import importlib.util
import subprocess
import tempfile
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALLER_PATH = REPO_ROOT / "tools" / "vu0_diff" / "integrate_pcsx2_test.py"


def load_installer():
    spec = importlib.util.spec_from_file_location("pcsx2_vu0_installer", INSTALLER_PATH)
    if spec is None or spec.loader is None:
        raise RuntimeError("could not load PCSX2 integration installer")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def make_git_checkout(root):
    root.mkdir(parents=True, exist_ok=True)
    subprocess.run(["git", "init", "-q", str(root)], check=True)
    subprocess.run(
        [
            "git", "-C", str(root),
            "-c", "user.name=VU0 installer test",
            "-c", "user.email=vu0-installer@example.invalid",
            "commit", "--allow-empty", "-qm", "fixture checkout",
        ],
        check=True,
    )
    return subprocess.run(
        ["git", "-C", str(root), "rev-parse", "HEAD"],
        check=True,
        capture_output=True,
        text=True,
    ).stdout.strip()


class PCSX2VU0InstallerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name) / "pcsx2"
        self.revision = make_git_checkout(self.root)
        self.core_dir = self.root / "tests" / "ctest" / "core"
        self.core_dir.mkdir(parents=True)
        self.cmake_path = self.core_dir / "CMakeLists.txt"
        self.cmake_path.write_text(
            "add_pcsx2_test(core_test\n"
            "\tpatch_tests.cpp\n"
            "\tMockMemoryInterface.h\n"
            "\tStubHost.cpp\n"
            ")\n",
            encoding="utf-8",
        )
        self.adapter_source = Path(self.temp.name) / "adapter.cpp"
        self.fixture_source = Path(self.temp.name) / "fixture.h"
        self.sha256_source = Path(self.temp.name) / "sha256.h"
        self.adapter_source.write_text("// adapter fixture\n", encoding="utf-8")
        self.fixture_source.write_text("// fixture header\n", encoding="utf-8")
        self.sha256_source.write_text("// sha256 fixture\n", encoding="utf-8")
        self.installer = load_installer()
        self.installer.PCSX2_REVISION = self.revision

    def tearDown(self):
        self.temp.cleanup()

    def test_install_is_idempotent_and_uninstall_restores_cmake(self):
        original_cmake = self.cmake_path.read_text(encoding="utf-8")

        self.installer.install(
            self.root, self.adapter_source, self.fixture_source, self.sha256_source
        )
        installed_cmake = self.cmake_path.read_text(encoding="utf-8")

        self.assertIn(self.installer.CMAKE_INSERTION, installed_cmake)
        self.assertEqual(
            (self.root / self.installer.TEST_RELATIVE_PATH).read_bytes(),
            self.adapter_source.read_bytes(),
        )
        self.assertEqual(
            (self.root / self.installer.FIXTURE_RELATIVE_PATH).read_bytes(),
            self.fixture_source.read_bytes(),
        )
        self.assertEqual(
            (self.root / self.installer.SHA256_RELATIVE_PATH).read_bytes(),
            self.sha256_source.read_bytes(),
        )

        self.installer.install(
            self.root, self.adapter_source, self.fixture_source, self.sha256_source
        )
        self.assertEqual(
            self.cmake_path.read_text(encoding="utf-8"), installed_cmake
        )

        self.installer.uninstall(self.root)
        self.assertEqual(
            self.cmake_path.read_text(encoding="utf-8"), original_cmake
        )
        self.assertFalse(
            (self.root / self.installer.TEST_RELATIVE_PATH).exists()
        )
        self.assertFalse(
            (self.root / self.installer.FIXTURE_RELATIVE_PATH).exists()
        )
        self.assertFalse(
            (self.root / self.installer.SHA256_RELATIVE_PATH).exists()
        )

    def test_wrong_revision_is_rejected_without_mutation(self):
        self.installer.PCSX2_REVISION = "0" * 40
        original_cmake = self.cmake_path.read_text(encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "revision mismatch"):
            self.installer.install(
                self.root, self.adapter_source, self.fixture_source, self.sha256_source
            )

        self.assertEqual(
            self.cmake_path.read_text(encoding="utf-8"), original_cmake
        )
        self.assertFalse(
            (self.root / self.installer.TEST_RELATIVE_PATH).exists()
        )

    def test_unexpected_cmake_layout_is_rejected(self):
        self.cmake_path.write_text("add_pcsx2_test(core_test)\n", encoding="utf-8")

        with self.assertRaisesRegex(RuntimeError, "expected CMake anchor"):
            self.installer.install(
                self.root, self.adapter_source, self.fixture_source, self.sha256_source
            )

        self.assertFalse(
            (self.root / self.installer.TEST_RELATIVE_PATH).exists()
        )

    def test_uninstall_refuses_unknown_cmake_mutation(self):
        self.installer.install(
            self.root, self.adapter_source, self.fixture_source, self.sha256_source
        )
        self.cmake_path.write_text(
            "add_pcsx2_test(core_test)\n"
            "\tvu0_issue_order_test.cpp\n",
            encoding="utf-8",
        )

        with self.assertRaisesRegex(RuntimeError, "unknown CMake layout"):
            self.installer.uninstall(self.root)

        self.assertTrue(
            (self.root / self.installer.TEST_RELATIVE_PATH).exists()
        )
        self.assertTrue(
            (self.root / self.installer.FIXTURE_RELATIVE_PATH).exists()
        )
        self.assertTrue(
            (self.root / self.installer.SHA256_RELATIVE_PATH).exists()
        )


if __name__ == "__main__":
    unittest.main()
