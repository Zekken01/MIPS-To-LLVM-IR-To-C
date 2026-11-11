#include "ee_recomp.h"
#include "ee_instruction.h"
#include "ee_opcode.h"
#include "ee_decoder.h"
#include "ee_runtime.h"
#include "ee_state.h"
#include "elf_loader.h"
#include "function_analyzer.h"
#include "instruction_lifter.h"
#include "llvm_context.h"
#include "module_builder.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <tuple>
#include <utility>
#include <vector>

namespace ee {

namespace
{
std::string sanitize_name(const std::string& name)
{
    std::string out;
    out.reserve(name.size());
    for (char c : name)
    {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '.')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('_');
        }
    }
    if (out.empty()) out = "fn";
    return out;
}

std::string json_escape(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(c); break;
        }
    }
    return out;
}

bool opcode_supported(const s_ee_instruction& ins)
{
    if (ins.valid != e_ee_valid::VALID) return false;
    s_lifted_op op = lift(ins);
    return op.kind != LOP_UNSUPPORTED;
}

std::string u32hex(uint32_t v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "0x%x", v);
    return buf;
}

} // namespace

bool ee_recomp(const char* elf_path,
               const char* output_dir,
               s_recompiled_elf* result,
               std::string* error,
               const s_recomp_options& opts)
{
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);

    elf_loader loader;
    if (!loader.load_file(elf_path, error)) return false;

    result->path = elf_path;
    result->entry_point = loader.entry_point();
    result->machine = loader.machine();
    result->segment_count = (uint32_t)loader.segments().size();

    // ------------------------------------------------------------------
    // Worklist-based code discovery: start from entry points and follow
    // control-flow edges. Only decode instructions that are actually
    // reachable from a function entry — data embedded in executable
    // segments is never decoded, so it never pollutes coverage.
    // ------------------------------------------------------------------
    std::map<uint32_t, s_ee_instruction> global_insns;
    std::set<uint32_t> worklist;
    std::set<uint32_t> discovered;

    auto seed_entry = [&](uint32_t ep) {
        if (ep != 0 && loader.is_executable_address(ep)) {
            worklist.insert(ep);
        }
    };
    seed_entry(loader.entry_point());
    for (const auto& sym : loader.symbols())
    {
        if (sym.is_function && !sym.is_undefined && sym.value != 0)
            seed_entry(sym.value);
    }

    while (!worklist.empty())
    {
        uint32_t pc = *worklist.begin();
        worklist.erase(worklist.begin());
        if (!discovered.insert(pc).second) continue;   // already decoded

        uint32_t word = 0;
        if (!loader.read32_at(pc, &word)) continue;    // not readable: stop trace

        s_ee_instruction ins = decode(pc, word);
        global_insns[pc] = ins;

        const std::set<uint32_t> successors = collect_successors(ins);
        worklist.insert(successors.begin(), successors.end());
    }

    // Function entry points: entry point + STT_FUNC symbols in executable
    // segments, plus any call-target addresses discovered during the walk.
    std::vector<uint32_t> entries;
    if (loader.entry_point() != 0 &&
        loader.is_executable_address(loader.entry_point())) {
        entries.push_back(loader.entry_point());
    }
    for (const auto& sym : loader.symbols())
    {
        if (sym.is_function && !sym.is_undefined && sym.value != 0 &&
            loader.is_executable_address(sym.value)) {
            entries.push_back(sym.value);
        }
    }
    // Add any call-target addresses found during the worklist walk that
    // weren't already in the symbol table.
    std::set<uint32_t> call_targets;
    for (const auto& kv : global_insns)
    {
        const s_ee_instruction& ins = kv.second;
        const bool direct_jal =
            ins.flow == e_ee_flow::FLOW_CALL &&
            ins.opcode == e_ee_opcode::OP_JAL;
        const bool regimm_link =
            ins.valid == e_ee_valid::VALID &&
            ins.opcode == e_ee_opcode::OP_REGIMM &&
            (ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZAL) ||
             ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZAL) ||
             ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BLTZALL) ||
             ins.rt == static_cast<uint8_t>(e_ee_opcode::REGIMM_BGEZALL));
        if (!direct_jal && !regimm_link)
            continue;

        // Conditional branch-and-link forms are FLOW_BRANCH/FLOW_BRANCH_LIKELY
        // for CFG execution, but their taken destination is still a function
        // entry and must be emitted independently like a JAL callee.
        const uint32_t target = direct_jal
            ? ins.jump_target
            : static_cast<uint32_t>(ins.branch_target);
        if (loader.is_executable_address(target))
            call_targets.insert(target);
    }
    for (uint32_t ct : call_targets)
    {
        bool already = false;
        for (uint32_t e : entries) if (e == ct) { already = true; break; }
        if (!already) entries.push_back(ct);
    }

    std::sort(entries.begin(), entries.end());
    entries.erase(std::unique(entries.begin(), entries.end()), entries.end());

    function_analyzer analyzer;
    llvm::llvm_context ctx("ee_game");

    // ------------------------------------------------------------------
    // Function-scoped coverage: every PC belonging to a CFG built from a
    // discovered entry. Data regions are never decoded, so they never
    // appear here.
    // ------------------------------------------------------------------
    std::set<uint32_t> function_pcs;
    std::set<uint32_t> analyzed_entries;
    for (uint32_t ep : entries)
    {
        if (!analyzed_entries.insert(ep).second) continue;
        std::vector<s_ee_function> fns =
            analyzer.analyze(global_insns, {ep}, entries);
        if (fns.empty()) continue;
        for (const auto& bb : fns[0].blocks)
        {
            for (uint32_t pc = bb.start_pc; pc < bb.end_pc; pc += 4)
            {
                if (global_insns.count(pc))
                    function_pcs.insert(pc);
            }
        }
    }

    uint32_t total_insns = 0, total_supported = 0, total_unsupported = 0;
    std::map<uint16_t, std::pair<uint32_t, uint32_t>> coverage;
    std::map<std::tuple<uint16_t, uint16_t, uint16_t>, std::pair<uint32_t, uint32_t>> sub_cov;

    auto record_sub = [&](const s_ee_instruction& ins, bool ok) {
        uint16_t op = (uint16_t)ins.opcode;
        uint16_t a = 0, b = 0;
        switch (ins.opcode) {
        case e_ee_opcode::OP_SPECIAL:
        case e_ee_opcode::OP_MMI:
            a = ins.funct; b = 0; break;
        case e_ee_opcode::OP_REGIMM:
            a = ins.rt; b = 0; break;
        case e_ee_opcode::OP_COP0:
        case e_ee_opcode::OP_COP1:
        case e_ee_opcode::OP_COP2:
            a = ins.rs; b = ins.funct; break;
        default:
            return; // no sub-histogram
        }
        auto& cell = sub_cov[{op, a, b}];
        cell.first++;
        if (ok) cell.second++;
    };

    for (uint32_t pc : function_pcs)
    {
        auto it = global_insns.find(pc);
        if (it == global_insns.end()) continue;
        const s_ee_instruction& ins = it->second;
        total_insns++;
        coverage[(uint16_t)ins.opcode].first++;
        bool ok = opcode_supported(ins);
        if (ok) total_supported++; else total_unsupported++;
        coverage[(uint16_t)ins.opcode].second += ok ? 1u : 0u;
        record_sub(ins, ok);
    }

    std::set<uint32_t> emitted_blocks;

    auto emit_entry = [&](uint32_t entry) -> bool {
        // Pass full entry set so CFGs stop at other functions (stop-at-other-entry).
        std::vector<s_ee_function> fns =
            analyzer.analyze(global_insns, {entry}, entries);
        if (fns.empty()) return false;
        s_ee_function& fn = fns[0];
        fn.name = loader.find_symbol(entry) ?
                  loader.find_symbol(entry)->name : ("sub_" + u32hex(entry));

        // Emit only blocks not emitted for a previous entry.
        bool any_new = false;
        for (const auto& bb : fn.blocks)
        {
            if (!emitted_blocks.insert(bb.start_pc).second) continue;
            any_new = true;
        }
        if (!any_new) return false;

        llvm::module_builder builder(ctx, opts.opt_level);
        builder.emit_function_cfg(fn, global_insns, fn.name.c_str());
        std::string ir_name = sanitize_name(fn.name) + "_" + u32hex(entry) + ".ll";
        std::string ir_path = std::string(output_dir) + "/" + ir_name;
        ctx.write_ir_to_file(ir_path.c_str());

        s_recompiled_fn rfn;
        rfn.name = fn.name;
        rfn.vaddr = entry;
        rfn.ir_path = ir_path;
        rfn.block_count = (uint32_t)fn.blocks.size();
        rfn.block_pcs.reserve(fn.blocks.size());
        for (const auto& bb : fn.blocks)
            rfn.block_pcs.push_back(bb.start_pc);
        rfn.insn_count = 0;
        rfn.supported_count = 0;
        rfn.unsupported_count = 0;
        for (const auto& bb : fn.blocks)
        {
            for (uint32_t pc = bb.start_pc; pc < bb.end_pc; pc += 4)
            {
                auto it = global_insns.find(pc);
                if (it == global_insns.end()) continue;
                rfn.insn_count++;
                if (opcode_supported(it->second)) rfn.supported_count++;
                else rfn.unsupported_count++;
            }
        }
        if (!fn.blocks.empty()) {
            rfn.size = (fn.blocks.rbegin()->start_pc + 4) - entry;
        }

        result->functions.push_back(std::move(rfn));

        std::printf("  %-48s pc=0x%08x insns=%u supported=%u unsupported=%u blocks=%u\n",
                    result->functions.back().name.c_str(), entry,
                    result->functions.back().insn_count,
                    result->functions.back().supported_count,
                    result->functions.back().unsupported_count,
                    result->functions.back().block_count);
        return true;
    };

    result->functions_discovered = (uint32_t)entries.size();

    if (opts.only_function != 0)
    {
        // Single-function mode: still use full entry set for CFG bounds.
        bool found = false;
        for (uint32_t ep : entries)
        {
            if (ep == opts.only_function)
            {
                found = true;
                break;
            }
        }
        if (!found && loader.is_executable_address(opts.only_function))
        {
            // Allow an explicit address even if not in the harvested set.
            entries.push_back(opts.only_function);
            std::sort(entries.begin(), entries.end());
            entries.erase(std::unique(entries.begin(), entries.end()),
                          entries.end());
            found = true;
        }
        if (!found)
        {
            if (error)
                *error = "only_function address not found in executable image";
            return false;
        }
        emit_entry(opts.only_function);
    }
    else
    {
        for (uint32_t ep : entries)
        {
            if (opts.max_functions != 0 &&
                result->functions.size() >= opts.max_functions)
            {
                std::printf("  (capped at --max-functions=%u; %zu of %u entries emitted)\n",
                            opts.max_functions, result->functions.size(),
                            (unsigned)entries.size());
                break;
            }
            emit_entry(ep);
        }
    }

    result->function_count = (uint32_t)result->functions.size();
    result->function_scoped_pcs = (uint32_t)function_pcs.size();
    result->total_insns = total_insns;
    result->total_supported = total_supported;
    result->total_unsupported = total_unsupported;
    // Linear sweep is now identical to function-scoped: both only cover
    // actually-decoded code. The old "full segment sweep" no longer exists.
    result->linear_insns = total_insns;
    result->linear_supported = total_supported;
    result->linear_unsupported = total_unsupported;
    for (auto& kv : coverage)
        result->coverage.push_back({kv.first, kv.second});
    std::sort(result->coverage.begin(), result->coverage.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (auto& kv : sub_cov)
    {
        s_recompiled_elf::s_sub_cov sc;
        sc.opcode = std::get<0>(kv.first);
        sc.sub = std::get<1>(kv.first);
        sc.sub2 = std::get<2>(kv.first);
        sc.total = kv.second.first;
        sc.supported = kv.second.second;
        result->sub_coverage.push_back(sc);
    }
    // Sort sub-coverage by unsupported descending for report convenience.
    std::sort(result->sub_coverage.begin(), result->sub_coverage.end(),
              [](const s_recompiled_elf::s_sub_cov& a,
                 const s_recompiled_elf::s_sub_cov& b) {
                  const uint32_t ua = a.total - a.supported;
                  const uint32_t ub = b.total - b.supported;
                  if (ua != ub) return ua > ub;
                  return a.total > b.total;
              });

    // Write ee_functions.json
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"elf\": \"" << json_escape(elf_path) << "\",\n";
        os << "  \"entry_point\": \"" << u32hex(result->entry_point) << "\",\n";
        os << "  \"machine\": " << result->machine << ",\n";
        os << "  \"segment_count\": " << result->segment_count << ",\n";
        os << "  \"function_count\": " << result->function_count << ",\n";
        os << "  \"functions_discovered\": " << result->functions_discovered << ",\n";
        os << "  \"scope\": \"function\",\n";
        os << "  \"total_insns\": " << result->total_insns << ",\n";
        os << "  \"total_supported\": " << result->total_supported << ",\n";
        os << "  \"total_unsupported\": " << result->total_unsupported << ",\n";
        os << "  \"linear_insns\": " << result->linear_insns << ",\n";
        os << "  \"linear_supported\": " << result->linear_supported << ",\n";
        os << "  \"linear_unsupported\": " << result->linear_unsupported << ",\n";
        os << "  \"functions\": [\n";
        for (size_t i = 0; i < result->functions.size(); ++i)
        {
            const auto& f = result->functions[i];
            os << "    {\n";
            os << "      \"name\": \"" << json_escape(f.name) << "\",\n";
            os << "      \"vaddr\": \"" << u32hex(f.vaddr) << "\",\n";
            os << "      \"size\": " << f.size << ",\n";
            os << "      \"insn_count\": " << f.insn_count << ",\n";
            os << "      \"supported_count\": " << f.supported_count << ",\n";
            os << "      \"unsupported_count\": " << f.unsupported_count << ",\n";
            os << "      \"block_count\": " << f.block_count << ",\n";
            os << "      \"block_pcs\": [";
            for (size_t j = 0; j < f.block_pcs.size(); ++j)
            {
                if (j != 0) os << ", ";
                os << "\"" << u32hex(f.block_pcs[j]) << "\"";
            }
            os << "],\n";
            os << "      \"ir_path\": \"" << json_escape(f.ir_path) << "\"\n";
            os << "    }" << (i + 1 < result->functions.size() ? "," : "") << "\n";
        }
        os << "  ]\n";
        os << "}\n";
        std::string json_path = std::string(output_dir) + "/ee_functions.json";
        std::FILE* f = std::fopen(json_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    // Write ee_coverage.json (function-scoped primary + linear + sub-ops)
    {
        std::ostringstream os;
        os << "{\n";
        os << "  \"scope\": \"function\",\n";
        os << "  \"functions_discovered\": " << result->functions_discovered << ",\n";
        os << "  \"function_scoped_pcs\": " << result->function_scoped_pcs << ",\n";
        os << "  \"total_insns\": " << result->total_insns << ",\n";
        os << "  \"total_supported\": " << result->total_supported << ",\n";
        os << "  \"total_unsupported\": " << result->total_unsupported << ",\n";
        if (result->total_insns)
            os << "  \"supported_pct\": "
               << (100.0 * result->total_supported / result->total_insns) << ",\n";
        os << "  \"linear_insns\": " << result->linear_insns << ",\n";
        os << "  \"linear_supported\": " << result->linear_supported << ",\n";
        os << "  \"linear_unsupported\": " << result->linear_unsupported << ",\n";
        if (result->linear_insns)
            os << "  \"linear_supported_pct\": "
               << (100.0 * result->linear_supported / result->linear_insns) << ",\n";
        os << "  \"opcode_coverage\": [\n";
        for (size_t i = 0; i < result->coverage.size(); ++i)
        {
            const auto& c = result->coverage[i];
            os << "    {\"opcode\": " << (uint32_t)c.first
               << ", \"total\": " << c.second.first
               << ", \"supported\": " << c.second.second << "}"
               << (i + 1 < result->coverage.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
        os << "  \"linear_opcode_coverage\": [\n";
        for (size_t i = 0; i < result->linear_coverage.size(); ++i)
        {
            const auto& c = result->linear_coverage[i];
            os << "    {\"opcode\": " << (uint32_t)c.first
               << ", \"total\": " << c.second.first
               << ", \"supported\": " << c.second.second << "}"
               << (i + 1 < result->linear_coverage.size() ? "," : "") << "\n";
        }
        os << "  ],\n";
        os << "  \"sub_coverage\": [\n";
        for (size_t i = 0; i < result->sub_coverage.size(); ++i)
        {
            const auto& s = result->sub_coverage[i];
            os << "    {\"opcode\": " << (uint32_t)s.opcode
               << ", \"sub\": " << (uint32_t)s.sub
               << ", \"sub2\": " << (uint32_t)s.sub2
               << ", \"total\": " << s.total
               << ", \"supported\": " << s.supported << "}"
               << (i + 1 < result->sub_coverage.size() ? "," : "") << "\n";
        }
        os << "  ]\n";
        os << "}\n";
        std::string cov_path = std::string(output_dir) + "/ee_coverage.json";
        std::FILE* f = std::fopen(cov_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    // Human-readable markdown report.
    {
        std::ostringstream os;
        const double pct = result->total_insns
            ? 100.0 * result->total_supported / result->total_insns : 0.0;
        const double lpct = result->linear_insns
            ? 100.0 * result->linear_supported / result->linear_insns : 0.0;
        os << "# Opcode coverage — function-scoped\n\n";
        os << "ELF: `" << elf_path << "`\n\n";
        os << "| Metric | Value |\n|--------|------:|\n";
        os << "| Functions discovered | " << result->functions_discovered << " |\n";
        os << "| Unique CFG PCs | " << result->function_scoped_pcs << " |\n";
        os << "| **Function-scoped supported** | ** "
           << result->total_supported << " / " << result->total_insns
           << " (" << pct << "%)** |\n";
        os << "| Linear sweep supported | "
           << result->linear_supported << " / " << result->linear_insns
           << " (" << lpct << "%) |\n\n";

        os << "## Primary opcodes (function-scoped)\n\n";
        os << "| Opcode | Total | Supported | Unsup | Pct |\n";
        os << "|-------:|------:|----------:|------:|----:|\n";
        auto cov_sorted = result->coverage;
        std::sort(cov_sorted.begin(), cov_sorted.end(),
                  [](const auto& a, const auto& b) {
                      return (a.second.first - a.second.second) >
                             (b.second.first - b.second.second);
                  });
        for (const auto& c : cov_sorted)
        {
            uint32_t t = c.second.first, s = c.second.second, u = t - s;
            if (t == 0) continue;
            double p = 100.0 * s / t;
            os << "| 0x" << std::hex << (unsigned)c.first << std::dec
               << " | " << t << " | " << s << " | " << u
               << " | " << p << "% |\n";
        }

        os << "\n## Top unsupported sub-ops (SPECIAL/REGIMM/COP*/MMI)\n\n";
        os << "| Opcode | sub | sub2 | Total | Unsup |\n";
        os << "|-------:|----:|-----:|------:|------:|\n";
        size_t shown = 0;
        for (const auto& s : result->sub_coverage)
        {
            uint32_t u = s.total - s.supported;
            if (u == 0) continue;
            os << "| 0x" << std::hex << (unsigned)s.opcode
               << " | 0x" << (unsigned)s.sub
               << " | 0x" << (unsigned)s.sub2 << std::dec
               << " | " << s.total << " | " << u << " |\n";
            if (++shown >= 40) break;
        }
        std::string md_path = std::string(output_dir) + "/OPCODE_COVERAGE.md";
        std::FILE* f = std::fopen(md_path.c_str(), "wb");
        if (f) {
            std::fwrite(os.str().data(), 1, os.str().size(), f);
            std::fclose(f);
        }
    }

    return true;
}

} // namespace ee