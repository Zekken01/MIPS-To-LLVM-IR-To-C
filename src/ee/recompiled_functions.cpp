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
        if (std::find(fn.block_pcs.begin(), fn.block_pcs.end(), guest_pc) !=
            fn.block_pcs.end())
        {
            return &fn;
        }
    }

    return nullptr;
}

} // namespace ee
