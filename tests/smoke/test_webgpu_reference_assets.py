#!/usr/bin/env python3
"""Validate native WebGPU source import and compute-pipeline entry points.

The reference renderer is intentionally not linked into ee_core until the
GS-state adapter and native WGPU dependency build are complete.
"""
from pathlib import Path
import re
import sys


def main():
    root = Path(sys.argv[1]).resolve()
    source = root / "third_party" / "ps2_web_recomp_gpu"
    gpu = (source / "gpu.cpp").read_text(encoding="utf-8")
    shader = (source / "gs.wgsl").read_text(encoding="utf-8")
    blit = (source / "blit.wgsl").read_text(encoding="utf-8")
    font = (source / "font.wgsl").read_text(encoding="utf-8")
    header = (source / "gpu.hpp").read_text(encoding="utf-8")

    # These are the nine actual wgpu compute pipelines in the native
    # source and their corresponding WGSL shader entry points.
    expected = (
        "upload", "copy_read", "copy_write", "load_clut",
        "display", "raster", "resolve", "snapshot", "decode"
    )
    for entry in expected:
        assert re.search(r"@compute\b[^{}]*?fn\s+" + entry + r"\s*\(",
                         shader, re.DOTALL), f"missing GS WGSL pipeline {entry}"
        assert re.search(r'\b"' + entry + r'"', gpu), (
            f"missing renderer dispatch entry {entry}"
        )

    for binding in range(9):
        assert re.search(r"@group\(0\)\s*@binding\(" +
                         str(binding) + r"\)", shader), (
            f"GS shader binding {binding} absent"
        )
    assert "WGPUDevice g_device" in gpu
    assert "bool attach_surface" in gpu
    assert "WGPUSurface" in gpu
    assert "enum class Mode { off, mirror, primary }" in header
    assert re.search(r"@vertex\\s+fn\\s+vs\\s*\\(", blit)\n    assert re.search(r"@fragment\\s+fn\\s+fs\\s*\\(", blit)
    assert "@fragment" in font or "@compute" in font or "@vertex" in font
    # Source import must not drag browser frontend code into this repo.
    assert not (root / "third_party" / "ps2_web_recomp_gpu" / "web").exists()
    print("webgpu_reference_assets: PASS (9 pipelines, 9 bindings, native sources)")


if __name__ == "__main__":
    main()
