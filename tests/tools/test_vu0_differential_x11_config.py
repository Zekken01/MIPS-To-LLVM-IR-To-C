#!/usr/bin/env python3
"""Guard the pinned PCSX2 Linux build against missing X11 imported targets."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[2]
DRIVER = ROOT / "tools" / "vu0_diff" / "run_differential.py"
WORKFLOW = ROOT / ".github" / "workflows" / "vu0-differential.yml"
README = ROOT / "tools" / "vu0_diff" / "README.md"


class VU0DifferentialX11ConfigTests(unittest.TestCase):
    def test_local_driver_keeps_x11_enabled_on_linux(self):
        source = DRIVER.read_text(encoding="utf-8")
        self.assertNotIn("-DX11_API=OFF", source)
        self.assertIn('if platform.system() == "Linux":', source)
        self.assertIn('pcsx2_cmake_args.append("-DX11_API=ON")', source)

    def test_github_workflow_installs_xi_and_enables_x11(self):
        source = WORKFLOW.read_text(encoding="utf-8")
        package_block = source.split("sudo apt-get install -y", 1)[1].split(
            "\n\n", 1
        )[0]
        self.assertIn("libx11-dev", package_block)
        self.assertIn("libxrandr-dev", package_block)
        self.assertIn("libxi-dev", package_block)
        self.assertIn("-DX11_API=ON", source)
        self.assertIn("-DWAYLAND_API=OFF", source)

    def test_manual_linux_instructions_enable_x11(self):
        source = README.read_text(encoding="utf-8")
        self.assertIn("-DX11_API=ON", source)
        self.assertIn("-DWAYLAND_API=OFF", source)


if __name__ == "__main__":
    unittest.main()
