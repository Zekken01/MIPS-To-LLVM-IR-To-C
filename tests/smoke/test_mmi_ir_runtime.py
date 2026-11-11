#!/usr/bin/env python3
"""Compile and execute generated EE LLVM at each module-builder opt level."""

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
    state = ctypes.cast(aligned_address, ctypes.POINTER(EEState)).contents
    return backing, state


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def run_opt_level(fixture_exe, temp_dir, opt_level, target_machine):
    ir_path = temp_dir / f"mmi_transfer_o{opt_level}.ll"
    subprocess.run([str(fixture_exe), str(ir_path), str(opt_level)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()

    function_address = engine.get_function_address("guest_mmi_transfer_test")
    require(function_address != 0, f"O{opt_level}: JIT did not expose guest function")
    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(function_address)

    backing, state = make_aligned_state()
    _ = backing  # Keep the aligned storage alive throughout the native call.
    state.gpr[4].lo = 0x55
    state.gpr[4].hi = 0x4444444444444444
    state.gpr[5].lo = 0x66
    state.gpr[5].hi = 0x5555555555555555
    state.gpr[6].lo = 0x77
    state.gpr[6].hi = 0x6666666666666666
    state.gpr[14].lo = 0xDEAD
    state.gpr[14].hi = 0x1414141414141414
    state.gpr[8].lo = 0x0123456789ABCDEF
    state.gpr[8].hi = 0xFEDCBA9876543210
    state.gpr[9].lo = 0x00FF00FF00FF00FF
    state.gpr[9].hi = 0x0F0F0F0F0F0F0F0F
    state.hi1 = 0x1111222233334444
    state.lo1 = 0x5555666677778888
    source_a_lo, source_a_hi = state.gpr[8].lo, state.gpr[8].hi
    source_b_lo, source_b_hi = state.gpr[9].lo, state.gpr[9].hi

    result = guest_fn(ctypes.pointer(state), None)

    prefix = f"O{opt_level}: "
    require(result == 3, prefix + f"unexpected exit result: {result}")
    require(state.exit_reason == 3, prefix + f"unexpected exit reason: {state.exit_reason}")
    require(state.pc == 0x0010003C, prefix + f"unexpected dispatcher PC: 0x{state.pc:08x}")
    require(state.gpr[4].lo == 0x1234, prefix + f"ADDIU writeback lost: 0x{state.gpr[4].lo:x}")
    require(state.gpr[4].hi == 0x4444444444444444, prefix + "ADDIU clobbered upper GPR half")
    require(state.gpr[14].lo == 0x5678,
            prefix + f"unused MTHI1 rd field killed cross-block GPR14 writeback: 0x{state.gpr[14].lo:x}")
    require(state.gpr[14].hi == 0x1414141414141414,
            prefix + "cross-block writeback clobbered GPR14 upper half")
    require(state.hi1 == 0x1234, prefix + f"MTHI1 did not read current rs: 0x{state.hi1:x}")
    require(state.gpr[5].lo == 0x1234, prefix + f"MFHI1 result missing: 0x{state.gpr[5].lo:x}")
    require(state.gpr[5].hi == 0x5555555555555555, prefix + "MFHI1 clobbered upper GPR half")
    require(state.lo1 == 0x1234, prefix + f"MTLO1 did not read pending MFHI1 result: 0x{state.lo1:x}")
    require(state.gpr[6].lo == 0x1234, prefix + f"MFLO1 result missing: 0x{state.gpr[6].lo:x}")
    require(state.gpr[6].hi == 0x6666666666666666, prefix + "MFLO1 clobbered upper GPR half")

    a_lo, a_hi = source_a_lo, source_a_hi
    b_lo, b_hi = source_b_lo, source_b_hi
    expected = {
        7: (a_lo & b_lo, a_hi & b_hi),
        10: (a_lo ^ b_lo, a_hi ^ b_hi),
        11: (a_lo | b_lo, a_hi | b_hi),
        12: (~(a_lo | b_lo) & 0xFFFFFFFFFFFFFFFF,
             ~(a_hi | b_hi) & 0xFFFFFFFFFFFFFFFF),
        8: (a_lo & b_lo, a_hi & b_hi),
        13: ((a_lo & b_lo) ^ b_lo, (a_hi & b_hi) ^ b_hi),
    }
    for register, (expected_lo, expected_hi) in expected.items():
        actual = state.gpr[register]
        require(actual.lo == expected_lo and actual.hi == expected_hi,
                prefix + f"MMI logical result GPR{register} wrong: "
                f"got ({actual.hi:016x}:{actual.lo:016x}), "
                f"expected ({expected_hi:016x}:{expected_lo:016x})")

    return engine, module


def main():
    require(len(sys.argv) == 2, "usage: test_mmi_ir_runtime.py path/to/ee_mmi_ir_fixture")
    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    engines = []
    target_machines = []
    modules = []

    with tempfile.TemporaryDirectory(prefix="ee_mmi_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for opt_level in range(4):
            target_machine = target.create_target_machine(opt=0)
            target_machines.append(target_machine)
            result = run_opt_level(fixture_exe, temp_dir, opt_level, target_machine)
            engines.append(result[0])
            modules.append(result[1])

    print("mmi_ir_runtime: PASS (O0-O3; CFG writeback; HI1/LO1; aliasing; upper-GPR preservation; PAND/PXOR/POR/PNOR)")


if __name__ == "__main__":
    main()
