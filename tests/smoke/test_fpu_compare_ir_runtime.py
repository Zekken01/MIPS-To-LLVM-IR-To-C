#!/usr/bin/env python3
"""JIT-test the four supported R5900 quiet ordered single-precision predicates."""

import ctypes
import pathlib
import struct
import subprocess
import sys
import tempfile

import llvmlite.binding as llvm


FCC0_MASK = 1 << 23
ENTRY_PC = 0x00130000


class EEState(ctypes.Structure):
    _fields_ = [
        ("gpr", (ctypes.c_uint64 * 2) * 32),
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
        ("vu0", (ctypes.c_uint64 * 2) * 32),
        ("fcsr", ctypes.c_uint32),
        ("fpu_acc", ctypes.c_uint32),
    ]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned = (address + 15) & ~15
    return backing, ctypes.cast(aligned, ctypes.POINTER(EEState)).contents


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def compile_jit(fixture, temp_dir, mode, target):
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture), str(ir_path), mode], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    target_machine = target.create_target_machine(opt=0)
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)
    module.verify()

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_fpu_compare_test")
    require(address != 0, f"{mode}: JIT function was not emitted")

    function_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return function_type(address), engine, module, target_machine


def run_case(fixture, temp_dir, target, mode, lhs, rhs, expected_fcc):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, mode, target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)

    # Begin with FCC0 set so false predicates must actively clear it. All other
    # FCSR bits must be preserved by the compare's read/modify/write sequence.
    initial_fcsr = 0xA5923456
    state.fcsr = initial_fcsr
    state.fpr[1] = f32_bits(lhs)
    state.fpr[2] = f32_bits(rhs)

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"{mode}({lhs!r}, {rhs!r}): "
    require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            prefix + f"state exit reason was {state.exit_reason}")
    require(state.pc == ENTRY_PC + 4,
            prefix + f"wrong sequential PC 0x{state.pc:08x}")

    expected_fcsr = (initial_fcsr & ~FCC0_MASK) | (
        FCC0_MASK if expected_fcc else 0
    )
    require(state.fcsr == expected_fcsr,
            prefix + f"FCSR got 0x{state.fcsr:08x}, expected 0x{expected_fcsr:08x}")


def main():
    require(
        len(sys.argv) == 2,
        "usage: test_fpu_compare_ir_runtime.py path/to/ee_fpu_compare_ir_fixture",
    )
    fixture = pathlib.Path(sys.argv[1]).resolve()

    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()

    qnan = struct.unpack("<f", struct.pack("<I", 0x7FC00000))[0]
    cases = [
        ("c_f", 1.0, 1.0, False),
        ("c_f", qnan, qnan, False),
        ("c_eq", 1.5, 1.5, True),
        ("c_eq", 1.5, 2.5, False),
        ("c_eq", 0.0, -0.0, True),
        ("c_eq", qnan, qnan, False),
        ("c_lt", -1.0, 0.0, True),
        ("c_lt", 0.0, 0.0, False),
        ("c_lt", qnan, -1.0, False),
        ("c_le", 0.0, 0.0, True),
        ("c_le", -2.0, -1.0, True),
        ("c_le", 2.0, 1.0, False),
        ("c_le", qnan, 1.0, False),
    ]

    with tempfile.TemporaryDirectory(prefix="ee-fpu-compare-") as directory:
        temp_dir = pathlib.Path(directory)
        for mode, lhs, rhs, expected in cases:
            run_case(fixture, temp_dir, target, mode, lhs, rhs, expected)

    print(
        "fpu_compare_ir_runtime: PASS (C.F/C.EQ/C.LT/C.LE; "
        "ordered NaN handling; signed zero; FCC0-only FCSR update; O0 JIT)"
    )


if __name__ == "__main__":
    main()
