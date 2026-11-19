#include "ee_dmac.h"
#include <cassert>

int main()
{
    ee::c_ee_dmac dmac;
    dmac.reset();
    auto initial = dmac.gif_channel_state();
    assert(initial.m_chcr == 0u && initial.m_madr == 0u &&
           initial.m_qwc == 0u && initial.m_tadr == 0u);

    const ee::s_ee_u128 madr{0x4000u, 0u};
    const ee::s_ee_u128 qwc{5u, 0u};
    const ee::s_ee_u128 tadr{0x8000u, 0u};
    assert(ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_gif_base + 0x10u, 4u, &madr));
    assert(ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_gif_base + 0x20u, 4u, &qwc));
    assert(ee::c_ee_dmac::mmio_write(
        &dmac, ee::c_ee_dmac::k_gif_base + 0x30u, 4u, &tadr));

    auto current = dmac.gif_channel_state();
    assert(current.m_chcr == 0u && current.m_madr == 0x4000u &&
           current.m_qwc == 5u && current.m_tadr == 0x8000u);
    assert(dmac.gif_trace_count() == 3u);
    ee::s_ee_u128 observed{};
    assert(ee::c_ee_dmac::mmio_read(
        &dmac, ee::c_ee_dmac::k_gif_base + 0x20u, 4u, &observed));
    assert(observed.m_lo == 5u);
    assert(dmac.gif_trace_count() == 3u);

    dmac.reset();
    current = dmac.gif_channel_state();
    assert(current.m_qwc == 0u && current.m_madr == 0u &&
           dmac.gif_trace_count() == 0u);
    return 0;
}
