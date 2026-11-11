#include "llvm_context.h"
#include <cstdio>
#include <fstream>

namespace llvm {

llvm_context::llvm_context(std::string module_name)
    : m_module_name(std::move(module_name))
{
}

void llvm_context::module_name(const std::string& name) { m_module_name = name; }
const std::string& llvm_context::module_name() const { return m_module_name; }

void llvm_context::ir(const std::string& text) { m_ir = text; }
void llvm_context::append_ir(const std::string& text) { m_ir += text; }
const std::string& llvm_context::ir() const { return m_ir; }

bool llvm_context::write_ir_to_file(const char* path) const
{
    std::FILE* f = std::fopen(path, "wb");
    if (!f) {
        return false;
    }
    const bool ok = (std::fwrite(m_ir.data(), 1, m_ir.size(), f) == m_ir.size());
    std::fclose(f);
    return ok;
}

} // namespace llvm
