# Embed original GS WGSL as a UTF-8 C++ string with external linkage.
# Only used when EE_ENABLE_WEBGPU=ON; no browser tools required.
if(NOT DEFINED SRC OR NOT DEFINED OUT OR NOT DEFINED NAME)
    message(FATAL_ERROR "SRC, OUT, and NAME must be specified")
endif()
file(READ "${SRC}" shader)
string(FIND "${shader}" ")EE_WEBGPU_SHADER\"" collision)
if(NOT collision EQUAL -1)
    message(FATAL_ERROR "WGSL contains the raw-string closing delimiter")
endif()
file(WRITE "${OUT}" "// Generated from ${SRC}; MIT license: LICENSE.upstream\n"
    "extern const char ${NAME}[] = R\"EE_WEBGPU_SHADER(\n"
    "${shader}\n"
    ")EE_WEBGPU_SHADER\";\n")
