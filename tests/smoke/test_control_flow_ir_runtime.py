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


def run_case(fixture_exe, temp_dir, target, retained, mode, function_name, setup, verify, expected_exit=3):
    print(f"control-flow case: {mode}", flush=True)
    target_machine = target.create_target_machine(opt=0)
    retained.append(target_machine)
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture_exe), mode, str(ir_path)], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    retained.extend((engine, module))
    print(f"{mode}: JIT finalized", flush=True)

    address = engine.get_function_address(function_name)
    require(address != 0, f"{mode}: JIT did not expose {function_name}")

    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    guest_fn = guest_fn_type(address)

    backing, state = make_aligned_state()
    _ = backing
    setup(state)

    print(f"{mode}: entering JIT function", flush=True)
    result = guest_fn(ctypes.pointer(state), None)
    print(f"{mode}: JIT returned {result}", flush=True)
    require(result == expected_exit,
            f"{mode}: expected exit reason {expected_exit}, got {result}")
    require(state.exit_reason == expected_exit,
            f"{mode}: state exit reason {state.exit_reason}, expected {expected_exit}")
    verify(state)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_flow_ir_runtime.py fixture_executable")

    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    retained = []

    with tempfile.TemporaryDirectory(prefix="ee-control-flow-") as temp:
        temp_dir = pathlib.Path(temp)

        def check_beq_taken(state):
            state.gpr[2].lo = 0
            state.gpr[2].hi = 0x2222333344445555

        def verify_beq_taken(state):
            require(state.pc == 0x00100014,
                    f"beq taken: delay slot changed sampled condition: PC=0x{state.pc:08x}")
            require(state.gpr[2].lo == 1,
                    f"beq taken: delay-slot ADDIU did not execute: {state.gpr[2].lo}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "beq taken: delay slot clobbered upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "beq_taken",
                 "guest_beq_taken_delay_test", check_beq_taken, verify_beq_taken)

        def check_beq_not_taken(state):
            state.gpr[2].lo = 1
            state.gpr[2].hi = 0x2222333344445555

        def verify_beq_not_taken(state):
            require(state.pc == 0x00100008,
                    f"beq not-taken: delay slot changed sampled condition: PC=0x{state.pc:08x}")
            require(state.gpr[2].lo == 0,
                    f"beq not-taken: delay-slot ADDIU did not execute: {state.gpr[2].lo}")
            require(state.gpr[2].hi == 0x2222333344445555,
                    "beq not-taken: delay slot clobbered upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "beq_not_taken",
                 "guest_beq_not_taken_delay_test", check_beq_not_taken, verify_beq_not_taken)

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

        run_case(fixture_exe, temp_dir, target, retained, "jr",
                 "guest_jr_delay_test", check_jr, verify_jr)

        def check_jrra(state):
            state.gpr[31].lo = 0x00300000
            state.gpr[31].hi = 0x3131313131313131

        def verify_jrra(state):
            require(state.pc == 0x00300000,
                    f"jr $ra: delay slot changed captured target: 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x1234,
                    "jr $ra: delay-slot ADDIU did not execute")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "jr $ra: delay slot clobbered the upper GPR half")

        run_case(fixture_exe, temp_dir, target, retained, "jrra",
                 "guest_jr_return_test", check_jrra, verify_jrra, expected_exit=0)

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

        run_case(fixture_exe, temp_dir, target, retained, "jal",
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

        run_case(fixture_exe, temp_dir, target, retained, "jalr",
                 "guest_jalr_delay_test", check_jalr, verify_jalr)

        def check_beq_taken(state):
            state.gpr[2].lo = 0

        def verify_beq_taken(state):
            require(state.pc == 0x00100014,
                    f"beq_taken: delay slot changed captured outcome: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 1,
                    "beq_taken: delay-slot ADDIU did not execute")

        run_case(fixture_exe, temp_dir, target, retained, "beq_taken",
                 "guest_beq_taken_delay_test", check_beq_taken, verify_beq_taken)

        def check_beq_not_taken(state):
            state.gpr[2].lo = 1

        def verify_beq_not_taken(state):
            require(state.pc == 0x00100008,
                    f"beq_not_taken: delay slot changed captured outcome: 0x{state.pc:08x}")
            require(state.gpr[2].lo == 0,
                    "beq_not_taken: delay-slot ADDIU did not execute")

        run_case(fixture_exe, temp_dir, target, retained, "beq_not_taken",
                 "guest_beq_not_taken_delay_test", check_beq_not_taken,
                 verify_beq_not_taken)

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
            def setup(state, taken=taken):
                check_link(state, taken)
            def verify(state, taken=taken):
                verify_bltzal(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bltzal",
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
            run_case(fixture_exe, temp_dir, target, retained, "bltzall",
                     "guest_bltzall_delay_test", setup, verify)

        def check_bgez_link(state, taken):
            state.gpr[2].lo = 1 if taken else 0xFFFFFFFFFFFFFFFF
            state.gpr[31].lo = 0xDEADBEEF
            state.gpr[31].hi = 0x3131313131313131
            state.gpr[8].lo = 0x55555555

        def verify_bgezal(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bgezal: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    "bgezal: link must be written on either outcome")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bgezal: link write clobbered upper GPR half")
            require(state.gpr[8].lo == 0x00100008,
                    "bgezal: ordinary delay slot did not execute/read link")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_bgez_link(state, taken)
            def verify(state, taken=taken):
                verify_bgezal(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bgezal",
                     "guest_bgezal_delay_test", setup, verify)

        def verify_bgezall(state, taken):
            expected_pc = 0x00100014 if taken else 0x00100008
            require(state.pc == expected_pc,
                    f"bgezall: expected PC 0x{expected_pc:08x}, got 0x{state.pc:08x}")
            require(state.gpr[31].lo == 0x00100008,
                    "bgezall: link must be written on either outcome")
            require(state.gpr[31].hi == 0x3131313131313131,
                    "bgezall: link write clobbered upper GPR half")
            expected_t0 = 0x00100008 if taken else 0x55555555
            require(state.gpr[8].lo == expected_t0,
                    "bgezall: delay-slot annulment mismatch")

        for taken in (True, False):
            def setup(state, taken=taken):
                check_bgez_link(state, taken)
            def verify(state, taken=taken):
                verify_bgezall(state, taken)
            run_case(fixture_exe, temp_dir, target, retained, "bgezall",
                     "guest_bgezall_delay_test", setup, verify)

    print("control_flow_ir_runtime: PASS (JR return/dispatch; JAL/JALR; BEQ condition sampling; BLTZAL/BGEZAL/BLTZALL/BGEZALL taken and not-taken)")


if __name__ == "__main__":
    main()
