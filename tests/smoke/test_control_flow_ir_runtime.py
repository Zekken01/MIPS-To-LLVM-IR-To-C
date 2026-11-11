#!/usr/bin/env python3
"""JIT regressions for direct and register-indirect MIPS control transfers."""

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


def compile_case(fixture_exe, temp_dir, mode, target_machine):
    ir_path = temp_dir / f"{mode}.ll"
    subprocess.run([str(fixture_exe), str(ir_path), mode], check=True)

    module = llvm.parse_assembly(ir_path.read_text(encoding="utf-8"))
    module.verify()
    module.triple = llvm.get_default_triple()
    module.data_layout = str(target_machine.target_data)

    engine = llvm.create_mcjit_compiler(llvm.parse_assembly(""), target_machine)
    engine.add_module(module)
    engine.finalize_object()
    address = engine.get_function_address("guest_control_flow_test")
    require(address != 0, f"{mode}: JIT did not expose guest function")

    guest_fn_type = ctypes.CFUNCTYPE(
        ctypes.c_uint32, ctypes.POINTER(EEState), ctypes.c_void_p
    )
    return engine, module, guest_fn_type(address)


def run_case(fixture_exe, temp_dir, mode, target_machine):
    engine, module, guest_fn = compile_case(
        fixture_exe, temp_dir, mode, target_machine
    )
    backing, state = make_aligned_state()
    _ = (backing, engine, module)

    state.gpr[31].hi = 0x3131313131313131
    state.gpr[16].hi = 0x1616161616161616
    state.gpr[17].hi = 0x1717171717171717
    state.gpr[8].hi = 0x0808080808080808
    state.gpr[25].hi = 0x2525252525252525

    if mode == "jal":
        state.gpr[31].lo = 0xDEADBEEF
        state.gpr[17].lo = 0xBAD
        expected_pc = 0x00101000
        expected_exit = 3
        expected_return = 3
    elif mode == "jalr":
        state.gpr[8].lo = 0x00234560
        state.gpr[16].lo = 0xAAAAAAAA
        state.gpr[17].lo = 0xBBBBBBBB
        expected_pc = 0x00234560
        expected_exit = 3
        expected_return = 3
    elif mode == "jalr_alias":
        state.gpr[8].lo = 0x00245670
        state.gpr[17].lo = 0xBBBBBBBB
        expected_pc = 0x00245670
        expected_exit = 3
        expected_return = 3
    elif mode == "jr_ra":
        state.gpr[31].lo = 0x00345670
        expected_pc = 0x00345670
        expected_exit = 0
        expected_return = 0
    elif mode == "jr_t9":
        state.gpr[25].lo = 0x00456780
        expected_pc = 0x00456780
        expected_exit = 3
        expected_return = 3
    else:
        raise AssertionError(f"unknown test mode: {mode}")

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"{mode}: "
    require(result == expected_return,
            prefix + f"wrong function exit: {result}")
    require(state.exit_reason == expected_exit,
            prefix + f"wrong state exit reason: {state.exit_reason}")
    require(state.pc == expected_pc,
            prefix + f"wrong dispatched PC: 0x{state.pc:08x}")

    if mode == "jal":
        require(state.gpr[31].lo == 0x00100008,
                prefix + "JAL did not write PC+8 to $ra")
        require(state.gpr[17].lo == 0x00100008,
                prefix + "JAL link was not visible in its delay slot")
        require(state.gpr[31].hi == 0x3131313131313131,
                prefix + "JAL clobbered the upper half of $ra")
    elif mode == "jalr":
        require(state.gpr[16].lo == 0x00100008,
                prefix + "JALR did not write PC+8 to encoded rd")
        require(state.gpr[17].lo == 0x00100008,
                prefix + "JALR link was not visible in its delay slot")
        require(state.gpr[16].hi == 0x1616161616161616,
                prefix + "JALR clobbered the upper half of rd")
    elif mode == "jalr_alias":
        require(state.gpr[8].lo == 0x00100008,
                prefix + "JALR rd=rs did not write the link")
        require(state.gpr[17].lo == 0x00100008,
                prefix + "JALR rd=rs delay slot did not read the link")
        require(state.gpr[8].hi == 0x0808080808080808,
                prefix + "JALR rd=rs clobbered the upper half")
    elif mode == "jr_ra":
        require(state.gpr[31].lo == 0x1234,
                prefix + "JR $ra delay slot did not overwrite $ra")
    elif mode == "jr_t9":
        require(state.gpr[25].lo == 0x4321,
                prefix + "JR $t9 delay slot did not overwrite $t9")


def main():
    require(
        len(sys.argv) == 2,
        "usage: test_control_flow_ir_runtime.py path/to/ee_control_flow_ir_fixture",
    )
    fixture_exe = pathlib.Path(sys.argv[1]).resolve()

    llvm.initialize_native_target()
    llvm.initialize_native_asmprinter()
    target = llvm.Target.from_default_triple()
    target_machine = target.create_target_machine(opt=0)

    modes = ("jal", "jalr", "jalr_alias", "jr_ra", "jr_t9")
    with tempfile.TemporaryDirectory(prefix="ee_control_flow_ir_") as directory:
        temp_dir = pathlib.Path(directory)
        for mode in modes:
            run_case(fixture_exe, temp_dir, mode, target_machine)

    print(
        "control_flow_ir_runtime: PASS "
        "(JAL/JALR link timing, JALR rd and rd=rs alias, "
        "JR target snapshot, delay slots, return/dispatch PC)"
    )


if __name__ == "__main__":
    main()
