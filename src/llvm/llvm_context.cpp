#include "llvm_context.h"
#include <cerrno>
#include <cstdio>
#include <cstring>

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
    if (!f)
    {
        std::fprintf(stderr, "llvm_context: cannot open '%s': %s\n",
                     path, std::strerror(errno));
        return false;
    }

    const size_t written = std::fwrite(m_ir.data(), 1, m_ir.size(), f);
    const int close_result = std::fclose(f);
    if (written != m_ir.size() || close_result != 0)
    {
        std::fprintf(stderr,
                     "llvm_context: short write for '%s' (written=%zu, expected=%zu, close=%d): %s\n",
                     path, written, m_ir.size(), close_result,
                     std::strerror(errno));
        return false;
    }

    return true;
}

} // namespace llvm
