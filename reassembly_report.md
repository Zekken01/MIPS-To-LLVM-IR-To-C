# Source Snapshot Reassembly Report

Payload parts: 17
- source_snapshot_part_00.b64: 10000 base64 characters
- source_snapshot_part_01.b64: 9999 base64 characters
- source_snapshot_part_02.b64: 10000 base64 characters
- source_snapshot_part_03.b64: 10000 base64 characters
- source_snapshot_part_04.b64: 10000 base64 characters
- source_snapshot_part_05.b64: 10000 base64 characters
- source_snapshot_part_06.b64: 10000 base64 characters
- source_snapshot_part_07.b64: 10000 base64 characters
- source_snapshot_part_08.b64: 16528 base64 characters
- source_snapshot_part_09.b64: 10000 base64 characters
- source_snapshot_part_10.b64: 10000 base64 characters
- source_snapshot_part_11.b64: 10000 base64 characters
- source_snapshot_part_12a.b64: 5000 base64 characters
- source_snapshot_part_12b.b64: 5000 base64 characters
- source_snapshot_part_13a.b64: 5000 base64 characters
- source_snapshot_part_13b.b64: 5000 base64 characters
- source_snapshot_part_14.b64: 1192 base64 characters
joined-as-is failed: Incorrect padding
decode-each-part: ZIP entries=41, bytes=110788
Rejected ZIP member .gitignore: BadZipFile
Rejected ZIP member CMakeLists.txt: BadZipFile
Rejected ZIP member build.sh: BadZipFile
Rejected ZIP member include/ee/ee_cfg.h: BadZipFile
Rejected ZIP member include/ee/ee_decoder.h: BadZipFile
Rejected ZIP member include/ee/ee_exceptions.h: BadZipFile
Rejected ZIP member include/ee/ee_instruction.h: BadZipFile
Rejected ZIP member include/ee/ee_opcode.h: BadZipFile
Rejected ZIP member include/ee/ee_recomp.h: BadZipFile
Rejected ZIP member include/ee/ee_runtime.h: BadZipFile
Rejected ZIP member include/ee/ee_state.h: BadZipFile
Rejected ZIP member include/ee/elf_loader.h: BadZipFile
Rejected ZIP member include/ee/function_analyzer.h: BadZipFile
Rejected ZIP member include/ee/instruction_lifter.h: BadZipFile
Rejected ZIP member include/ee/mmi_ops.h: BadZipFile
Rejected ZIP member include/ee/native_codegen.h: BadZipFile
Rejected ZIP member src/ee/decoder.cpp: BadZipFile
Rejected ZIP member src/ee/ee_recomp.cpp: BadZipFile
Rejected ZIP member src/ee/function_analyzer.cpp: BadZipFile
Rejected ZIP member src/ee/opcode_tables.cpp: BadZipFile
Rejected ZIP member src/elf/elf_loader.cpp: BadZipFile
Rejected ZIP member src/lift/instruction_lifter.cpp: BadZipFile
Rejected ZIP member src/lift/native_codegen.cpp: BadZipFile
Rejected ZIP member src/llvm/llvm_context.cpp: BadZipFile
Rejected ZIP member src/llvm/llvm_context.h: BadZipFile
Rejected ZIP member src/llvm/module_builder.cpp: BadZipFile
Rejected ZIP member src/llvm/module_builder.h: BadZipFile
Rejected ZIP member src/llvm/object_emitter.cpp: BadZipFile
Rejected ZIP member src/llvm/object_emitter.h: BadZipFile
Rejected ZIP member src/runtime/ee_exceptions.cpp: BadZipFile
Rejected ZIP member src/runtime/ee_memory.cpp: BadZipFile
Rejected ZIP member src/runtime/ee_memory.h: BadZipFile
Rejected ZIP member src/tools/ee_recomp_main.cpp: BadZipFile
Rejected ZIP member tests/decoder/decoder_test.cpp: BadZipFile
Rejected ZIP member tests/smoke/smoke_test.cpp: BadZipFile
decode-each-part: independently CRC-verified members=6
joined-strip-padding: no readable ZIP directory (BadZipFile: File is not a zip file)
decode-each-part: CRC-verified local entries=37
joined-strip-padding: CRC-verified local entries=11
Recovered files: 37
- .gitignore (116 bytes)
- CMakeLists.txt (4092 bytes)
- build.sh (856 bytes)
- include/ee/ee_cfg.h (1292 bytes)
- include/ee/ee_decoder.h (427 bytes)
- include/ee/ee_exceptions.h (4854 bytes)
- include/ee/ee_instruction.h (1967 bytes)
- include/ee/ee_opcode.h (8160 bytes)
- include/ee/ee_recomp.h (3123 bytes)
- include/ee/ee_runtime.h (1864 bytes)
- include/ee/ee_state.h (1572 bytes)
- include/ee/mmi_ops.h (7832 bytes)
- include/ee/native_codegen.h (2237 bytes)
- src/ee/decoder.cpp (36912 bytes)
- src/ee/ee_recomp.cpp (21802 bytes)
- src/ee/function_analyzer.cpp (7813 bytes)
- src/ee/opcode_tables.cpp (2499 bytes)
- src/elf/elf_loader.cpp (8672 bytes)
- src/lift/instruction_lifter.cpp (12882 bytes)
- src/lift/native_codegen.cpp (20439 bytes)
- src/llvm/llvm_context.cpp (830 bytes)
- src/llvm/llvm_context.h (937 bytes)
- src/llvm/module_builder.cpp (114891 bytes)
- src/llvm/module_builder.h (5036 bytes)
- src/llvm/object_emitter.cpp (591 bytes)
- src/llvm/object_emitter.h (885 bytes)
- src/runtime/ee_exceptions.cpp (6071 bytes)
- src/runtime/ee_memory.cpp (5170 bytes)
- src/runtime/ee_memory.h (1696 bytes)
- src/tools/ee_recomp_main.cpp (3743 bytes)
- tests/decoder/decoder_test.cpp (10829 bytes)
- tests/smoke/min_test.cpp (338 bytes)
- tests/smoke/mmi_ir_fixture.cpp (3472 bytes)
- tests/smoke/multi_function_test.cpp (20294 bytes)
- tests/smoke/native_smoke.cpp (34245 bytes)
- tests/smoke/test_mmi_ir_runtime.py (5985 bytes)
- tools/verify_ir.py (3525 bytes)
Missing required files: none
