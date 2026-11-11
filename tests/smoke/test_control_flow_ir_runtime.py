#!/usr/bin/env python3
"""JIT regressions for indirect/direct transfers and link/delay-slot ordering."""

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


def run_case(fixture_exe, temp_dir, target_machine, mode, function_name, setup, verify):
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture_exe), mode, str(ir_path)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()

    address = engine.get_function_address(function_name)
    require(address != 0, f"{mode}: JIT did not expose {function_name}")

    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(address)

    backing, state = make_aligned_state()
    _ = backing
    setup(state)

    result = guest_fn(ctypes.pointer(state), None)
    require(result == 3, f"{mode}: expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            f"{mode}: expected dispatch exit reason 3, got {state.exit_reason}")
    verify(state)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_flow_ir_runtime.py fixture_executable")

    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    target_machine = target.create_target_machine(opt=0)

    with tempfile.TemporaryDirectory(prefix="ee-control-flow-") as temp:
        temp_dir = pathlib.Path(temp)

        def check_jr(state):
            state.gpr[2].lo = 0x00200000
            state.gpr[2].hi = 0x2222333344445555

        def verify_jr(state):
            require(state.pc == 0x00200000,
                    f"jr: delay slot changed captured target: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0x1234,
                    f"jr: delay-slot ADDIU did not execute: 0x{state.gpr[2].lo:x}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "jr: delay slot clobbered the upper GPR half")

        run_case(fixture_exe, temp_dir, target_machine, "jr",
                 "guest_jr_delay_test", check_jr, verify_jr)

        def check_jal(state):
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0xAAAAAAAA

        def verify_jal(state):
            require(state.pc == 0x00102000,
                    f"jal: wrong dispatched target: 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"jal: wrong link value: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "jal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    f"jal: delay slot did not observe PC+8: 0x{state.gpr[8].lo:x}")

        run_case(fixture_exe, temp_dir, target_machine, "jal",
                 "guest_jal_delay_test", check_jal, verify_jal)

        def check_jalr(state):
            state.gpr[8].lo = 0x00200000
            state.gpr[8].hi = 0x8888888888888888
            state.gpr[9].lo = 0xAAAAAAAA

        def verify_jalr(state):
            require(state.pc == 0x00200000,
                    f"jalr: target was not captured before rd/rs alias write: 0x{state.pc:08x}")
            require(state.gpr[8].lo == 0x00100008,
                    f"jalr: rd did not receive PC+8: 0x{state.gpr[8].lo:x}")
            require(state.gpr[8].hi == 0x8888888888888888,
                    "jalr: link write clobbered upper GPR half")
            require(state.gpr[9].lo == 0x00100009,
                    f"jalr: delay slot did not observe new link: 0x{state.gpr[9].lo:x}")

        run_case(fixture_exe, temp_dir, target_machine, "jalr",
                 "guest_jalr_delay_test", check_jalr, verify_jalr)

        def check_link(state, taken):
            state.gpr[2].lo = 0xFFFFFFFFFFFFFFFF if taken else 1
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0x55555555

        def verify_bltzal(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bltzal: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"bltzal: link must be written on either outcome: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bltzal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    "bltzal: ordinary delay slot did not execute/read link")

        for taken in (True, False):
            mode = "bltzal_taken" if taken else "bltzal_not_taken"
            def setup(state, taken=taken):
                check_link(state, taken)
            def verify(state, taken=taken):
                verify_bltzal(state, taken)
            run_case(fixture_exe, temp_dir, target_machine, "bltzal",
                     "guest_bltzal_delay_test", setup, verify)

        def verify_bltzall(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bltzall: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    f"bltzall: link must be written on either outcome: 0x{state.gpr[31].lo:x}")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bltzall: link write clobbered upper GPR half")
            expected_t0 = 0x00100008 if taken else 0x55555555
            require(state.gpr[8].lo == expected_t0,
                    f"bltzall: delay-slot annulment mismatch: 0x{state.gpr[8].lo:x}")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_link(state, taken)
            def verify(state, taken=taken):
                verify_bltzall(state, taken)
            run_case(fixture_exe, temp_dir, target_machine, "bltzall",
                     "guest_bltzall_delay_test", setup, verify)

    print("control_flow_ir_runtime: PASS (JR/JAL/JALR; BLTZAL/BLTZALL taken and not-taken)")


if __name__ == "__main__":
    main()
