#!/usr/bin/env python3
"""Native dispatcher chunks preserve ownership order and stable cache inputs."""
import importlib.util
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "link_native_objects", ROOT / "tools" / "link_native_objects.py"
)
linker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(linker)


def make_function(index):
    address = 0x100000 + index * 0x10
    return {
        "name": f"sub_{address:08x}",
        "host_symbol": f"guest_fn_{address:08x}",
        "vaddr": address,
        "block_pcs": [address, address + 4],
        "ir_path": f"sub_{address:08x}.ll",
    }


class ChunkDispatcherTests(unittest.TestCase):
    def test_256_function_boundary_preserves_cached_chunk(self):
        base = [make_function(i) for i in range(256)]
        expanded = base + [make_function(256)]
        initial = linker.build_dispatch_chunk_source(base, 0, 0)
        expanded_first = linker.build_dispatch_chunk_source(expanded[:256], 0, 0)
        self.assertEqual(initial, expanded_first)
        self.assertIn("entries[255] = &guest_fn_00100ff0;", initial)
        self.assertNotIn("guest_fn_00101000", initial)
        tail = linker.build_dispatch_chunk_source(expanded[256:], 256, 1)
        self.assertIn("entries[256] = &guest_fn_00101000;", tail)
        self.assertNotIn("entries[255]", tail)

        meta = {"entry_point": "0x00100008", "functions_discovered": 300}
        before = linker.build_chunked_dispatch_source(meta, base, 1)
        after = linker.build_chunked_dispatch_source(meta, expanded, 2)
        self.assertIn("compiled.function_count = 256u;", before)
        self.assertIn("compiled.function_count = 257u;", after)
        self.assertNotIn("ee_dispatch_chunk_0001(", before)
        self.assertIn("ee_dispatch_chunk_0001(", after)
        self.assertIn("entries.data()", after)
        self.assertIn("ee_dispatch_run(cpu, runtime, g_dispatch_table,", after)

    def test_chunk_ownership_uses_original_metadata_order(self):
        inputs = [make_function(i) for i in range(3)]
        first = linker.build_dispatch_chunk_source(inputs[:2], 0, 0)
        second = linker.build_dispatch_chunk_source(inputs[2:], 2, 1)
        self.assertIn("entries[0] = &guest_fn_00100000;", first)
        self.assertIn("entries[1] = &guest_fn_00100010;", first)
        self.assertIn("entries[2] = &guest_fn_00100020;", second)
        self.assertIn("compiled->functions.push_back", first)
        self.assertIn("compiled->functions.push_back", second)

    def test_cpp_escaping(self):
        item = make_function(0)
        item["name"] = "with \"quoted\" name"
        item["ir_path"] = "C:\\guest\\sub.ll"
        source = linker.build_dispatch_chunk_source([item], 0, 0)
        self.assertIn('with \\"quoted\\" name', source)
        self.assertIn('C:\\\\guest\\\\sub.ll', source)


if __name__ == "__main__":
    unittest.main()
