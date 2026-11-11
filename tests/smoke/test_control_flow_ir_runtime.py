#!/usr/bin/env python3
"""JIT regression: an unsupported indirect JR must still execute its delay slot."""

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


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_control_flow_ir_runtime.py fixture_executable")

    fixture_exe = pathlib.Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix="ee-control-flow-") as temp:
        ir_path = pathlib.Path(temp) / "indirect_jr_delay.ll"
        subprocess.run([str(fixture_exe), str(ir_path)], check=True)

        module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
        module.verify()
        module.triple = llvm.get_default_triple()

        target = llvm.Target.from_default_triple()
        target_machine = target.create_target_machine()
        module.data_layout = str(target_machine.target_data)

        engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
        engine.add_module(module)
        engine.finalize_object()

        address = engine.get_function_address("guest_indirect_jr_delay_test")
        require(address != 0, "JIT did not expose the indirect JR fixture")

        guest_fn_type = ctypes.CFUNCTYPE(
            ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
        )
        guest_fn = guest_fn_type(address)

        backing, state = make_aligned_state()
        _ = backing
        state.gpr[2].lo = 0x00200000
        state.gpr[8].lo = 0xDEADBEEF
        state.gpr[8].hi = 0x8877665544332211

        result = guest_fn(ctypes.pointer(state), None)

        require(result == 4, f"expected EE_EXIT_UNSUPPORTED (4), got {result}")
        require(state.exit_reason == 4,
                f"expected unsupported exit reason 4, got {state.exit_reason}")
        require(state.pc == 0x00100000,
                f"unsupported JR exit PC changed: 0x{state.pc:08x}")
        require(state.gpr[8].lo == 0x1234,
                f"delay-slot ADDIU did not execute: 0x{state.gpr[8].lo:x}")
        require(state.gpr[8].hi == 0x8877665544332211,
                "delay-slot ADDIU clobbered the upper 64-bit GPR half")

    print("control_flow_ir_runtime: PASS")


if __name__ == "__main__":
    main()
