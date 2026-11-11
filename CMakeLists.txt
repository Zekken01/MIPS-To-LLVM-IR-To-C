cmake_minimum_required(VERSION 3.18)
project(ps2_recompiler C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# Use the LLVM-provided clang/clang++ and lld-link when available. LLVM is kept
# optional: the core decoder/runtime build without LLVM (PLAN.md section 6).
if(NOT CMAKE_C_COMPILER)
    set(CMAKE_C_COMPILER "clang")
endif()
if(NOT CMAKE_CXX_COMPILER)
    set(CMAKE_CXX_COMPILER "clang++")
endif()

if(EXISTS "/usr/bin/ld.lld" OR EXISTS "/usr/local/bin/ld.lld")
  set(CMAKE_EXE_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_SHARED_LINKER_FLAGS "-fuse-ld=lld")
  set(CMAKE_MODULE_LINKER_FLAGS "-fuse-ld=lld")
endif()

include_directories(include include/ee src/llvm src/lift src/runtime)

add_library(ee_core STATIC
    src/ee/decoder.cpp
    src/ee/opcode_tables.cpp
    src/ee/function_analyzer.cpp
    src/elf/elf_loader.cpp
    src/llvm/llvm_context.cpp
    src/llvm/module_builder.cpp
    src/llvm/object_emitter.cpp
    src/lift/instruction_lifter.cpp
    src/lift/native_codegen.cpp
    src/runtime/ee_memory.cpp
    src/runtime/ee_exceptions.cpp
)

add_executable(ee_smoke_test
    tests/smoke/smoke_test.cpp
    tests/smoke/native_smoke.cpp
)
target_link_libraries(ee_smoke_test PRIVATE ee_core)

# Build a native object for the reference/interpreter smoke harness. This is
# deliberately not described as LLVM IR object emission: the actual LLVM text
# path is verified separately by tools/verify_ir.py when llvmlite is available.
add_custom_command(
    OUTPUT ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
    COMMAND ${CMAKE_CXX_COMPILER} -O2 -Wall -Wextra -c
        -I${CMAKE_SOURCE_DIR}/include/ee
        -I${CMAKE_SOURCE_DIR}/src/runtime
        ${CMAKE_SOURCE_DIR}/tests/smoke/native_smoke.cpp
        -o ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Emitting native object for lifted guest function"
    VERBATIM
)
add_custom_target(ee_emit_obj DEPENDS
    ${CMAKE_BINARY_DIR}/recompiled/guest_fn_0x100000.obj
)

add_executable(ee_multi_test
    tests/smoke/multi_function_test.cpp
)
target_link_libraries(ee_multi_test PRIVATE ee_core)

add_executable(ee_min_test
    tests/smoke/min_test.cpp
)
target_link_libraries(ee_min_test PRIVATE ee_core)

# Full-game static recompiler: load an EE ELF, discover functions, build CFGs,
# lift every instruction, emit per-function LLVM IR text + coverage metadata.
add_executable(ee_recomp
    src/tools/ee_recomp_main.cpp
    src/ee/ee_recomp.cpp
)
target_link_libraries(ee_recomp PRIVATE ee_core)

# Decoder regression test (MMI classification, VALID/UNSUPPORTED contract).
add_executable(ee_decoder_test tests/decoder/decoder_test.cpp)
target_link_libraries(ee_decoder_test PRIVATE ee_core)

# Emit a real LLVM module for a JIT-backed runtime test of HI1/LO1 transfers
# and the four supported MMI logical operations. llvmlite is optional so the
# core C++ build remains usable without the Python LLVM binding.
add_executable(ee_mmi_ir_fixture tests/smoke/mmi_ir_fixture.cpp)
target_link_libraries(ee_mmi_ir_fixture PRIVATE ee_core)

# Dedicated JIT runtime coverage for the EE's second divide pipeline.
add_executable(ee_div1_ir_fixture tests/smoke/div1_ir_fixture.cpp)
target_link_libraries(ee_div1_ir_fixture PRIVATE ee_core)

# Dedicated JIT runtime coverage for exactly lowered MMI1 packed operations.
add_executable(ee_mmi1_ir_fixture tests/smoke/mmi1_ir_fixture.cpp)
target_link_libraries(ee_mmi1_ir_fixture PRIVATE ee_core)

find_package(Python3 COMPONENTS Interpreter QUIET)
set(EE_HAVE_LLVM_JIT_TEST FALSE)
if(Python3_Interpreter_FOUND)
    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import llvmlite.binding"
        RESULT_VARIABLE EE_LLVM_BINDING_RESULT
        OUTPUT_QUIET ERROR_QUIET
    )
    if(EE_LLVM_BINDING_RESULT EQUAL 0)
        set(EE_HAVE_LLVM_JIT_TEST TRUE)
    endif()
endif()

enable_testing()
add_test(NAME decoder COMMAND ee_decoder_test)
add_test(NAME multi_function COMMAND ee_multi_test)
add_test(NAME smoke COMMAND ee_smoke_test)
if(EE_HAVE_LLVM_JIT_TEST)
    add_test(NAME mmi_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi_ir_runtime.py
                $<TARGET_FILE:ee_mmi_ir_fixture>)
else()
    message(STATUS "llvmlite not found: omitting mmi_ir_runtime CTest")
endif()
if(EE_HAVE_LLVM_JIT_TEST)
    add_test(NAME div1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_div1_ir_runtime.py
                $<TARGET_FILE:ee_div1_ir_fixture>)
    add_test(NAME mmi1_ir_runtime
        COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_SOURCE_DIR}/tests/smoke/test_mmi1_ir_runtime.py
                $<TARGET_FILE:ee_mmi1_ir_fixture>)
endif()
set_tests_properties(multi_function smoke PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
