#include "object_emitter.h"
#include <cstdio>

namespace llvm {

bool object_emitter::emit_object(const llvm_context& /*ctx*/,
                                 const char* /*out_path*/,
                                 std::string* error)
{
    if (error) {
        *error = "native object emission requires the full LLVM C++ API and "
                 "llc/opt, which are not installed in this toolchain (compiler-"
                 "only LLVM package). LLVM IR text is emitted instead; see "
                 "build/recompiled/ee_module.ll";
    }
    return false;
}

} // namespace llvm
