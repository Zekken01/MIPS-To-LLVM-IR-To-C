#!/usr/bin/env python3
"""JIT-test DIV1/DIVU1 semantics, including zero and signed overflow."""

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


def make_aligned_state():
    backing = ctypes.create_string_buffer(ctypes.sizeof(EEState) + 15)
    address = ctypes.addressof(backing)
    aligned_address = (address + 15) & ~15
    return backing, ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_case(fixture, temp_dir, opt_level, case, target_machine):
    ir_path = temp_dir / f"div1_case{case}_o{opt_level}.ll"
    subprocess.run(
        [str(fixture), str(ir_path), str(opt_level), str(case)],
        check=True,
    )
    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_div1_test")
    require(address != 0, f"O{opt_level} case {case}: missing guest function")
    guest_fn = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )(address)

    backing, state = make_aligned_state()
    _ = backing
    state.hi1 = 0x1111222233334444
    state.lo1 = 0x5555666677778888

    if case == 0:  # signed -17 / 5
        state.gpr[4].lo = (-17) & 0xFFFFFFFFFFFFFFFF
        state.gpr[5].lo = 5
        expected_hi, expected_lo = 0xFFFFFFFFFFFFFFFE, 0xFFFFFFFFFFFFFFFD
    elif case == 1:  # signed INT_MIN / -1: defined overflow result
        state.gpr[4].lo = 0x80000000
        state.gpr[5].lo = 0xFFFFFFFF
        expected_hi, expected_lo = 0, 0xFFFFFFFF80000000
    elif case == 2:  # signed divide by zero: deterministic no-op
        state.gpr[4].lo = (-17) & 0xFFFFFFFFFFFFFFFF
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0x1111222233334444, 0x5555666677778888
    elif case == 3:  # unsigned 0xfffffffe / 3
        state.gpr[4].lo = 0xFFFFFFFE
        state.gpr[5].lo = 3
        expected_hi, expected_lo = 2, 0x55555554
    else:  # unsigned divide by zero: deterministic no-op
        state.gpr[4].lo = 0xFFFFFFFE
        state.gpr[5].lo = 0
        expected_hi, expected_lo = 0x1111222233334444, 0x5555666677778888

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"O{opt_level} case {case}: "
    require(result == 3, prefix + f"unexpected exit result {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason {state.exit_reason}")
    require(state.hi1 == expected_hi,
            prefix + f"HI1 got 0x{state.hi1:016x}, expected 0x{expected_hi:016x}")
    require(state.lo1 == expected_lo,
            prefix + f"LO1 got 0x{state.lo1:016x}, expected 0x{expected_lo:016x}")
    require(state.gpr[6].lo == expected_hi,
            prefix + f"MFHI1 got 0x{state.gpr[6].lo:016x}")
    require(state.gpr[7].lo == expected_lo,
            prefix + f"MFLO1 got 0x{state.gpr[7].lo:016x}")
    return engine, module


def main():
    require(len(sys.argv) == 2, "usage: test_div1_ir_runtime.py path/to/ee_div1_ir_fixture")
    fixture = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    engines, modules, target_machines = [], [], []

    with tempfile.TemporaryDirectory(prefix="ee_div1_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            target_machine = target.create_target_machine(opt=0)
            target_machines.append(target_machine)
            for case in range(5):
                engine, module = run_case(
                    fixture, temp_dir, opt_level, case, target_machine
                )
                engines.append(engine)
                modules.append(module)

    print("div1_ir_runtime: PASS (O0-O3; DIV1/DIVU1; signed quotient/remainder; INT_MIN/-1; divide-by-zero preservation)")


if __name__ == "__main__":
    main()
