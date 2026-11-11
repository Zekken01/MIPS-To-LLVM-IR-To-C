#include "ee_recomp.h"

#include <algorithm>

namespace ee
{

const s_recompiled_fn* find_recompiled_function(
    const s_recompiled_elf& compiled,
    uint32_t guest_pc)
{
    for (const auto& fn : compiled.functions)
    {
        // module_builder's entry switch accepts zero as a legacy initial-PC
        // sentinel and redirects it to the translated function's first block.
        // Mirror that rule here so the host dispatcher can select the entry
        // function before invoking its generated switch.
        if (guest_pc == 0 && fn.vaddr == compiled.entry_point)
            return &fn;

        if (std::find(fn.block_pcs.begin(), fn.block_pcs.end(), guest_pc) !=
            fn.block_pcs.end())
        {
            return &fn;
        }
    }

    return nullptr;
}

} // namespace ee
