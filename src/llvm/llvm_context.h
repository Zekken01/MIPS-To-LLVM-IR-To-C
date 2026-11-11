#pragma once
#include <string>

namespace llvm {

// Thin wrapper around the generated target-independent LLVM IR text. The full
// LLVM C++ API is not available in the installed toolchain (compiler-only
// install: no LLVMCore/LLVMSupport static libs, no llc/opt), so the lifter
// emits well-formed LLVM IR text that can be consumed by a full LLVM
// toolchain (opt/llc) or by an equivalent native implementation for testing.
// See PLAN.md section 3.1 and 8.
class llvm_context
{
public:
    llvm_context() = default;
    explicit llvm_context(std::string module_name);

    void module_name(const std::string& name);
    const std::string& module_name() const;

    void ir(const std::string& text);
    void append_ir(const std::string& text);
    const std::string& ir() const;

    bool write_ir_to_file(const char* path) const;

private:
    std::string m_module_name = "ee_module";
    std::string m_ir;
};

} // namespace llvm
