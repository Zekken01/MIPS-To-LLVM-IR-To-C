"""Host-only diagnostic imports for standalone LLVM MCJIT regressions.

These tests pass a null runtime pointer and verify guest architectural state.
They do not link the native EE runtime, so register inert C ABI callbacks for
diagnostic symbols used by the generated IR. Real native execution continues
to link and exercise the actual runtime implementations.
"""
import ctypes

from llvmlite import binding as llvm


@ctypes.CFUNCTYPE(
    None, ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_uint64, ctypes.c_uint64,
)
def _guest_v0_trace_stub(runtime, pc, opcode, old_value, new_value):
    pass


@ctypes.CFUNCTYPE(
    None, ctypes.c_void_p,
    ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32, ctypes.c_uint32,
    ctypes.c_uint32, ctypes.c_uint64, ctypes.c_uint32, ctypes.c_uint64,
    ctypes.c_uint64, ctypes.c_uint64, ctypes.c_uint64,
)
def _guest_store_trace_stub(runtime, pc, opcode, effective_address,
                            affected_address, width, source_value, source_lane,
                            base_value, sp, fp, ra):
    pass


def register_ee_diagnostic_stubs():
    # The module globals retain the CFUNCTYPE objects for the JIT lifetime.
    llvm.add_symbol(
        "ee_trace_guest_v0_write",
        ctypes.cast(_guest_v0_trace_stub, ctypes.c_void_p).value,
    )
    llvm.add_symbol(
        "ee_trace_guest_store",
        ctypes.cast(_guest_store_trace_stub, ctypes.c_void_p).value,
    )
