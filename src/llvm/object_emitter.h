#pragma once
#include "llvm_context.h"
#include <string>

namespace llvm {

// Emits a native object file from a verified LLVM module. The installed LLVM
// is a compiler-only package (no LLVMCore/LLVMSupport static libraries and no
// llc/opt), so target machine creation and object emission cannot be
// implemented here. This component is the integration point for a full LLVM
// toolchain: once llc/opt and the C++ API libraries are available, implement
// target machine + emission here (PLAN.md sections 3.1, 6, 8).
class object_emitter
{
public:
    object_emitter() = default;

    // Emit a native object for `ctx` into `out_path`. Returns false and fills
    // `error` when the LLVM toolchain required for codegen is unavailable.
    bool emit_object(const llvm_context& ctx, const char* out_path,
                     std::string* error = nullptr);
};

} // namespace llvm
