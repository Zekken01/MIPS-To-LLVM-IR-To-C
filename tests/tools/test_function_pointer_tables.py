#!/usr/bin/env python3
"""Regression for guest-anchored native function-pointer table expansion."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

PROJECT_ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "run_game", PROJECT_ROOT / "tools" / "run_game.py"
)
run_game = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(run_game)


class PointerTableTests(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.addCleanup(self.scratch.cleanup)
        self.image = Path(self.scratch.name) / "guest.elf"
        blob = bytearray(0x500)
        blob[:6] = b"\x7fELF\x01\x01"
        struct.pack_into("<H", blob, 18, 8)  # MIPS
        struct.pack_into("<I", blob, 28, 52)  # phoff
        struct.pack_into("<H", blob, 42, 32)  # phentsize
        struct.pack_into("<H", blob, 44, 2)   # phnum
        # First LOAD contains executable code, second contains data.
        struct.pack_into(
            "<IIIIIIII", blob, 52,
            1, 0x100, 0x100000, 0x100000, 0x200, 0x200, 5, 0x10
        )
        struct.pack_into(
            "<IIIIIIII", blob, 84,
            1, 0x300, 0x200000, 0x200000, 0x200, 0x200, 6, 0x10
        )
        self.entries = [0x100040 + index * 0x10 for index in range(8)]
        for index, pc in enumerate(self.entries):
            struct.pack_into("<I", blob, 0x320 + index * 4, pc)
            struct.pack_into("<I", blob, 0x100 + pc - 0x100000, 0x03e00008)
        # A duplicate-only array isn't evidence of a function-pointer table.
        for index in range(5):
            struct.pack_into("<I", blob, 0x380 + index * 4, 0x100150)
        # A short sequence doesn't establish a table either.
        for index in range(3):
            struct.pack_into("<I", blob, 0x3a0 + index * 4, 0x100190)
        self.image.write_bytes(blob)

    def discover(self, target, limit=16):
        return run_game.pointer_table_siblings(
            self.image, target, run_game.executable_segments(self.image),
            limit,
        )

    def test_direct_callees_skip_delay_slots_and_stop_at_branches(self):
        image = bytearray(self.image.read_bytes())
        first, second, delay, after_branch = self.entries[:4]

        def jal(pc):
            return 0x0c000000 | (pc >> 2)

        struct.pack_into("<8I", image, 0x100,
                         jal(first), jal(delay), jal(second), 0,
                         jal(first), 0, 0x10000000, jal(after_branch))
        self.image.write_bytes(image)
        discover = run_game.direct_call_prefix
        self.assertEqual(discover(self.image, 0x100000, set(), 8), [first, second])
        self.assertEqual(discover(self.image, 0x100000, {first}, 8), [second])
        self.assertEqual(discover(self.image, 0x100000, set(), 1), [first])
        self.assertEqual(discover(self.image, 0x100000, set(), 0), [])

    def test_guest_anchor_discovers_real_siblings(self):
        candidates = self.discover(self.entries[4])
        self.assertEqual(len(candidates), 7)
        self.assertEqual(set(candidates), set(self.entries) - {self.entries[4]})
        self.assertEqual(candidates[:2], [self.entries[3], self.entries[5]])

    def test_cap_and_unrelated_anchors(self):
        self.assertEqual(self.discover(self.entries[4], limit=2),
                         [self.entries[3], self.entries[5]])
        self.assertEqual(self.discover(self.entries[4], limit=0), [])
        self.assertEqual(self.discover(0x100150), [])
        self.assertEqual(self.discover(0x100190), [])
        self.assertEqual(self.discover(0x200020), [])  # non-executable
        self.assertEqual(self.discover(0x100049), [])  # unaligned

    def make_guarded_switch(self):
        """Six indexed entries, only three unique, all reached through JR v1."""
        blob = bytearray(self.image.read_bytes())
        # Branch at 0x00100100: sltiu v1,6; beqz guard; nop.
        # Build the guest table base from LUI/ADDIU, not from host hints.
        instructions = (
            0x2c610006,  # sltiu at, v1, 6
            0x10200010,  # beq at, zero, default (past the indexed branch)
            0x00000000,  # delay slot
            0x3c050020,  # lui a1, 0x0020
            0x00031880,  # sll v1, v1, 2
            0x24a50100,  # addiu a1, a1, 0x100
            0x00651821,  # addu v1, v1, a1
            0x8c630000,  # lw v1, 0(v1)
            0x00600008,  # jr v1
            0x00000000,  # delay slot
        )
        for index, opcode in enumerate(instructions):
            struct.pack_into("<I", blob, 0x200 + index * 4, opcode)
        # Guest table 0x00200100 is file offset 0x400.
        target = 0x100160
        siblings = (0x100174, 0x100188)
        for index, pc in enumerate(
            (target, *siblings, target, target, target)
        ):
            struct.pack_into("<I", blob, 0x400 + index * 4, pc)
        self.image.write_bytes(blob)
        return target, siblings

    def discover_branch(self, target, limit=16):
        return run_game.branch_anchored_jump_table_siblings(
            self.image, target, run_game.executable_segments(self.image),
            limit,
        )

    def test_branch_anchored_three_target_switch(self):
        target, siblings = self.make_guarded_switch()
        # Generic callback-table discovery stays conservative.
        self.assertEqual(self.discover(target), [])
        self.assertEqual(self.discover_branch(target), list(siblings))
        self.assertEqual(self.discover_branch(target, limit=1),
                         [siblings[0]])
        self.assertEqual(self.discover_branch(target, limit=0), [])
        self.assertEqual(self.discover_branch(0x10001c), [])
        self.assertEqual(self.discover_branch(0x100161), [])

    def test_branch_delay_slot_saves_argument(self):
        target, siblings = self.make_guarded_switch()
        image = bytearray(self.image.read_bytes())
        struct.pack_into("<I", image, 0x208, 0x0080802d)  # daddu s0,a0,zero
        self.image.write_bytes(image)
        self.assertEqual(self.discover_branch(target), list(siblings))

    def test_branch_anchored_switch_rejects_malformed_provenance(self):
        target, _ = self.make_guarded_switch()
        original = self.image.read_bytes()
        for offset, invalid in (
            (0x200, 0x2c610081),  # guard bounds larger than 128
            (0x200, 0x2c810006),  # guard bounds a different index
            (0x204, 0x00000000),  # missing bounds-check branch
            (0x208, 0x0080182d),  # delay slot overwrites checked index
            (0x208, 0xac800000),  # delay slot store could change table data
            (0x20c, 0x3c030020),  # base overwrites index
            (0x210, 0x00031840),  # shift by one, not pointer width
            (0x214, 0x24a50101),  # misaligned table base
            (0x218, 0x00451821),  # adds a different register
            (0x21c, 0x8c620000),  # loads a different JR register
            (0x404, 0x200020),    # sibling non-executable, anchor remains
        ):
            with self.subTest(offset=offset):
                image = bytearray(original)
                struct.pack_into("<I", image, offset, invalid)
                self.image.write_bytes(image)
                self.assertEqual(self.discover_branch(target), [])

    def test_guard_result_must_preserve_index(self):
        target, _ = self.make_guarded_switch()
        image = bytearray(self.image.read_bytes())
        struct.pack_into("<II", image, 0x200, 0x2c630006, 0x10600010)
        self.image.write_bytes(image)
        self.assertEqual(self.discover_branch(target), [])

    def test_branch_anchored_switch_requires_file_backed_table(self):
        target, _ = self.make_guarded_switch()
        blob = bytearray(self.image.read_bytes())
        # Lie about the data segment's backed size: no readable entries.
        struct.pack_into("<I", blob, 84 + 16, 0x80)
        self.image.write_bytes(blob)
        self.assertEqual(self.discover_branch(target), [])

    def test_truncated_segment_rejected(self):
        blob = bytearray(self.image.read_bytes())
        struct.pack_into("<I", blob, 52 + 16, len(blob))
        self.image.write_bytes(blob)
        with self.assertRaisesRegex(ValueError, "PT_LOAD exceeds"):
            self.discover(self.entries[4])


if __name__ == "__main__":
    unittest.main()
