#include "ee_hot_loop_diagnostics.h"
#include <cassert>
#include <cstdio>
#include <string>

static std::string serialize(const ee::c_ee_dispatch_hotspots& hotspots)
{
    FILE* file = std::tmpfile();
    assert(file != nullptr);
    hotspots.write_json(file);
    assert(std::fflush(file) == 0);
    assert(std::fseek(file, 0, SEEK_SET) == 0);
    std::string json;
    char buffer[2048];
    size_t count = 0u;
    while ((count = std::fread(buffer, 1u, sizeof(buffer), file)) != 0u)
    {
        json.append(buffer, count);
    }
    assert(std::fclose(file) == 0);
    return json;
}

int main()
{
    ee::c_ee_dispatch_hotspots empty;
    const std::string baseline = serialize(empty);
    assert(baseline.find("\"schema\": \"ee-m2-dispatch-hotspots-v1\"") != std::string::npos);
    assert(baseline.find("\"observations\": 0") != std::string::npos);
    assert(baseline.find("\"latest_state\": null") != std::string::npos);

    ee::c_ee_dispatch_hotspots hotspots(2u, 2u);
    ee::s_ee_hot_loop_state initial{};
    initial.m_intc_mask = 7u;
    hotspots.record(1u, 0x172e20u, 0x172e48u, initial);
    hotspots.record(2u, 0x172e20u, 0x172e48u, initial);
    ee::s_ee_hot_loop_state changed = initial;
    changed.m_gs_fields = 1u;
    changed.m_gif_chcr = 0x100u;
    changed.m_gif_write_sequence = 4u;
    hotspots.record(3u, 0x1730c0u, 0x172e20u, changed);
    hotspots.record(4u, 0x102670u, 0x1730c0u, changed);
    hotspots.record(5u, 0x172e20u, 0x172e48u, changed);
    assert(hotspots.observations() == 5u);
    assert(hotspots.untracked_pc_samples() == 1u);
    assert(hotspots.untracked_edge_samples() == 1u);
    const std::string json = serialize(hotspots);
    assert(json.find("\"visits\": 3") != std::string::npos);
    assert(json.find("\"untracked_pc_samples\": 1") != std::string::npos);
    assert(json.find("\"total_state_changes\": 1") != std::string::npos);
    assert(json.find("\"gs_fields\": 1") != std::string::npos);
    assert(json.find("\"gif_chcr\": 256") != std::string::npos);
    assert(json.find("\"gif_write_sequence\": 4") != std::string::npos);

    ee::c_ee_dispatch_hotspots changes;
    ee::s_ee_hot_loop_state state{};
    for (uint32_t i = 0u; i < 100u; ++i)
    {
        state.m_gs_fields = i;
        changes.record(i + 1u, 0x172e20u, 0x172e48u, state);
    }
    const std::string bounded = serialize(changes);
    assert(bounded.find("\"total_state_changes\": 99") != std::string::npos);
    assert(bounded.find("\"dispatch\": 2,") == std::string::npos);
    assert(bounded.find("\"dispatch\": 100,") != std::string::npos);
    return 0;
}
