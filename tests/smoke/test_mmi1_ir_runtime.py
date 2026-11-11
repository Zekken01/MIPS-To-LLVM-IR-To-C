#!/usr/bin/env python3
"""JIT-test exact MMI1 packed absolute/compare/min/saturating arithmetic."""

import ctypes
import pathlib
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


class U128(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint64), ("hi", ctypes.c_uint64)]


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", U128 * 32),
        ("hi", ctypes.c_uint64),
        ("lo", ctypes.c_uint64),
        ("hi1", ctypes.c_uint64),
        ("lo1", ctypes.c_uint64),
        ("sa", ctypes.c_uint32),
        ("pc", ctypes.c_uint32),
        ("exit_reason", ctypes.c_uint32),
        ("pad", ctypes.c_uint32),
        ("cop0", ctypes.c_uint32 * 32),
        ("fpr", ctypes.c_uint64 * 32),
        ("vu0", U128 * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
    ]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def pack_lanes(lanes, width):
    mask = (1 << width) - 1
    value = 0
    for index, lane in enumerate(lanes):
        value |= (lane & mask) << (index * width)
    return value


def signed_lane(value, width):
    sign = 1 << (width - 1)
    return value - (1 << width) if value & sign else value


def lane_operation(lhs, rhs, width, operation):
    count = 128 // width
    mask = (1 << width) - 1
    result = []
    for index in range(count):
        shift = index * width
        a = (lhs >> shift) & mask
        b = (rhs >> shift) & mask

        if operation == "abs_sat":
            signed_a = signed_lane(a, width)
            value = min(abs(signed_a), (1 << (width - 1)) - 1)
        elif operation == "equal":
            value = mask if a == b else 0
        elif operation == "min_signed":
            value = min(signed_lane(a, width), signed_lane(b, width))
        elif operation == "add_unsigned_sat":
            value = min(a + b, mask)
        elif operation == "sub_unsigned_sat":
            value = max(a - b, 0)
        else:
            raise AssertionError(f"unknown lane operation: {operation}")

        result.append(value & mask)
    return pack_lanes(result, width)


def pad_s_bh(lhs, rhs):
    result = []
    for index in range(8):
        a = (lhs >> (index * 16)) & 0xFFFF
        b = (rhs >> (index * 16)) & 0xFFFF
        value = a - b if index < 4 else a + b
        result.append(value & 0xFFFF)
    return pack_lanes(result, 16)


def pext_upper(lhs, rhs, width):
    lane_count = 128 // width
    start = lane_count // 2
    mask = (1 << width) - 1
    result = []
    for index in range(start, lane_count):
        a = (lhs >> (index * width)) & mask
        b = (rhs >> (index * width)) & mask
        # Each output pair is {rs lane, rt lane} in bit notation, which
        # means rt is the lower lane and rs is the upper lane.
        result.extend((b, a))
    return pack_lanes(result, width)


def qfsrv(lhs, rhs, shift):
    mask128 = (1 << 128) - 1
    if shift == 0:
        return rhs
    return ((lhs << (128 - shift)) | (rhs >> shift)) & mask128


def split_u128(value):
    mask64 = (1 << 64) - 1
    return value & mask64, (value >> 64) & mask64


def run_opt_level(fixture_exe, temp_dir, opt_level, shift_amount):
    ir_path = temp_dir / f"mmi1_o{opt_level}_sa{shift_amount}.ll"
    subprocess.run([str(fixture_exe), str(ir_path), str(opt_level)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()

    target = llvm.Target.from_default_triple()
    target_machine = target.create_target_machine(opt=0)
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()

    address = engine.get_function_address("guest_mmi1_test")
    require(address != 0, f"O{opt_level}: JIT did not expose guest function")
    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(address)

    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    aligned = (ctypes.addressof(backing) + 15) & ~15
    state = ctypes.cast(aligned, ctypes.POINTER(EEState)).contents

    lhs_words = [0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678]
    rhs_words = [0x7FFFFFFF, 0x80000000, 0x00000002, 0x87654321]
    lhs = pack_lanes(lhs_words, 32)
    rhs = pack_lanes(rhs_words, 32)
    state.gpr[4].lo, state.gpr[4].hi = split_u128(lhs)
    state.gpr[5].lo, state.gpr[5].hi = split_u128(rhs)
    state.sa = shift_amount

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"O{opt_level} SA={shift_amount}: "
    require(result == 3, prefix + f"unexpected exit result {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason {state.exit_reason}")

    expected_values = {
        6: lane_operation(lhs, 0, 32, "abs_sat"),
        7: lane_operation(lhs, 0, 16, "abs_sat"),
        8: lane_operation(lhs, rhs, 32, "equal"),
        9: lane_operation(lhs, rhs, 16, "equal"),
        10: lane_operation(lhs, rhs, 8, "equal"),
        11: lane_operation(lhs, rhs, 32, "min_signed"),
        12: lane_operation(lhs, rhs, 16, "min_signed"),
        13: lane_operation(lhs, rhs, 32, "add_unsigned_sat"),
        14: lane_operation(lhs, rhs, 32, "sub_unsigned_sat"),
        15: lane_operation(lhs, rhs, 16, "add_unsigned_sat"),
        16: lane_operation(lhs, rhs, 16, "sub_unsigned_sat"),
        17: lane_operation(lhs, rhs, 8, "add_unsigned_sat"),
        18: lane_operation(lhs, rhs, 8, "sub_unsigned_sat"),
        19: pad_s_bh(lhs, rhs),
        20: pext_upper(lhs, rhs, 8),
        21: pext_upper(lhs, rhs, 16),
        22: pext_upper(lhs, rhs, 32),
        23: qfsrv(lhs, rhs, shift_amount),
        # Final PCEQW intentionally aliases rd with rs.
        4: lane_operation(lhs, rhs, 32, "equal"),
    }

    for register, expected in expected_values.items():
        expected_lo, expected_hi = split_u128(expected)
        actual = state.gpr[register]
        require(actual.lo == expected_lo and actual.hi == expected_hi,
                prefix + f"GPR{register}: got ({actual.hi:016x}:{actual.lo:016x}), "
                f"expected ({expected_hi:016x}:{expected_lo:016x})")

    # This lane set checks INT_MIN saturation in both ABS variants, word
    # unsigned-add carry, unsigned-subtract underflow, and repeated equal lanes.
    require(state.gpr[6].lo >> 32 == 0x7FFFFFFF,
            prefix + "PABSW failed to clamp INT_MIN to INT_MAX")
    require((state.gpr[7].lo >> 16) & 0xFFFF == 0x7FFF,
            prefix + "PABSH failed to clamp INT_MIN to INT_MAX")
    return engine, module, target_machine


def main():
    require(len(sys.argv) == 2, "usage: test_mmi1_ir_runtime.py path/to/ee_mmi1_ir_fixture")
    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()

    engines = []
    modules = []
    target_machines = []
    with tempfile.TemporaryDirectory(prefix="ee_mmi1_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            for shift_amount in (0, 8, 40, 120):
                engine, module, target_machine = run_opt_level(
                    fixture_exe, temp_dir, opt_level, shift_amount
                )
                engines.append(engine)
                modules.append(module)
                target_machines.append(target_machine)

    print("mmi1_ir_runtime: PASS (O0-O3; PABSW/PABSH; PADSBH; PCEQW/PCEQH/PCEQB; PMINW/PMINH; PADDU*/PSUBU*; PEXTU*/QFSRV; aliasing)")


if __name__ == "__main__":
    main()
