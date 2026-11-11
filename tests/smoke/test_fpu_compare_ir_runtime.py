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


def run_cvt_w_case(fixture, temp_dir, target, source_bits, expected_bits, label):
    guest_fn, engine, module, target_machine = compile_jit(
        fixture, temp_dir, "cvt_w", target
    )
    backing, state = aligned_state()
    _ = (backing, engine, module, target_machine)

    state.fpr[1] = source_bits
    state.fpr[6] = 0xAABBCCDD11223344
    initial_fcsr = 0xA5923456
    state.fcsr = initial_fcsr

    result = guest_fn(ctypes.pointer(state), None)
    prefix = f"cvt_w({label}, 0x{source_bits:08x}): "
    require(result == 3, prefix + f"expected EE_EXIT_DISPATCH (3), got {result}")
    require(state.exit_reason == 3,
            prefix + f"state exit reason was {state.exit_reason}")
    require(state.pc == ENTRY_PC + 4,
            prefix + f"wrong sequential PC 0x{state.pc:08x}")
    require(state.fpr[6] == expected_bits,
            prefix + f"FPR[6] got 0x{state.fpr[6]:016x}, "
                     f"expected 0x{expected_bits:016x}")
    require(state.fpr[1] == source_bits,
            prefix + "source FPR was modified")
    require(state.fcsr == initial_fcsr,
            prefix + f"CVT.W.S unexpectedly modified FCSR: 0x{state.fcsr:08x}")


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
        # The EE comparison path first normalises exponents: denormals become
        # signed zero, while Inf/NaN become signed FMAX. Therefore equal positive
        # NaNs compare equal after normalisation, rather than using host IEEE
        # unordered-comparison behaviour directly.
        ("c_eq", qnan, qnan, True),
        ("c_eq", float("inf"), qnan, True),
        ("c_eq", 1.0e-45, 0.0, True),  # denormal flushes to +0
        ("c_lt", -1.0, 0.0, True),
        ("c_lt", 0.0, 0.0, False),
        ("c_lt", qnan, -1.0, False),  # +FMAX is not less than -1
        ("c_lt", -1.0, qnan, True),   # -1 is less than +FMAX
        ("c_le", 0.0, 0.0, True),
        ("c_le", -2.0, -1.0, True),
        ("c_le", 2.0, 1.0, False),
        ("c_le", qnan, 1.0, False),
        ("c_le", -1.0, qnan, True),
    ]

    # CVT.W.S uses the EE's truncating conversion and saturates values whose
    # exponent is outside the signed 32-bit range. These expected results match
    # the PCSX2 EE interpreter's CVT_W implementation.
    cvt_cases = [
        (0x40700000, 0x00000003, "positive fraction truncates"),
        (0xC0700000, 0x00000000FFFFFFFD, "negative fraction truncates toward zero"),
        (0x3F000000, 0x00000000, "positive half"),
        (0xBF000000, 0x00000000, "negative half"),
        (0x00000000, 0x00000000, "positive zero"),
        (0x80000000, 0x00000000, "negative zero"),
        (0x00000001, 0x00000000, "positive denormal"),
        (0x80000001, 0x00000000, "negative denormal"),
        (0x4EFFFFFF, 0x000000007FFFFF80, "largest value below 2^31"),
        (0xCEFFFFFF, 0x0000000080000080, "negative value just above -2^31"),
        (0x4F000000, 0x000000007FFFFFFF, "positive 2^31 saturates"),
        (0xCF000000, 0x0000000080000000, "negative 2^31 clamps"),
        (0xCF000001, 0x0000000080000000, "negative overflow saturates"),
        (0x7F800000, 0x000000007FFFFFFF, "positive infinity saturates"),
        (0xFF800000, 0x0000000080000000, "negative infinity saturates"),
        (0x7FC00000, 0x000000007FFFFFFF, "positive quiet NaN saturates"),
        (0xFFC00000, 0x0000000080000000, "negative quiet NaN saturates"),
    ]

    with tempfile.TemporaryDirectory(prefix="ee-fpu-compare-") as directory:
        temp_dir = pathlib.Path(directory)
        for mode, lhs, rhs, expected in cases:
            run_case(fixture, temp_dir, target, mode, lhs, rhs, expected)
        for source_bits, expected_bits, label in cvt_cases:
            run_cvt_w_case(
                fixture, temp_dir, target, source_bits, expected_bits, label
            )

    print(
        "fpu_compare_ir_runtime: PASS (C.F/C.EQ/C.LT/C.LE; CVT.W.S; "
        "EE NaN/Inf normalisation, integer saturation, truncation, FCSR "
        "preservation; O0 JIT)"
    )


if __name__ == "__main__":
    main()
