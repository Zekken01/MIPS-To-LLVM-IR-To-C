#include "ee_decoder.h"
#include "ee_isa_inventory.h"
#include "instruction_lifter.h"
#include <cassert>
#include <sstream>
#include <string>

int main()
{
    ee::c_ee_isa_inventory inventory;
    const ee::s_ee_instruction addiu = ee::decode(0x1000u, 0x24020001u);
    const ee::s_ee_instruction mfsa = ee::decode(0x1004u, 0x00003028u);
    const ee::s_ee_instruction sc = ee::decode(0x1008u, 0xe0850000u);

    assert(addiu.valid == ee::e_ee_valid::VALID);
    assert(mfsa.valid == ee::e_ee_valid::UNSUPPORTED);
    assert(sc.valid == ee::e_ee_valid::INVALID);
    const bool addiu_lowers = ee::lift(addiu).kind != ee::LOP_UNSUPPORTED;
    assert(addiu_lowers);
    inventory.record(addiu, addiu_lowers);
    inventory.record(mfsa, false);
    inventory.record(sc, false);

    // A newly introduced valid decoder opcode that the lifter does not
    // handle must appear as valid_lift_gap, never as supported execution.
    const ee::s_ee_instruction introduced = ee::decode(0x100cu, 0x24030002u);
    assert(introduced.valid == ee::e_ee_valid::VALID);
    inventory.record(introduced, false);

    assert(inventory.totals().m_total == 4u);
    assert(inventory.totals().m_decoder_valid == 2u);
    assert(inventory.totals().m_decoder_unsupported == 1u);
    assert(inventory.totals().m_decoder_invalid == 1u);
    assert(inventory.totals().m_lifter_accepts == 1u);
    assert(inventory.totals().m_valid_lift_gap == 1u);

    std::ostringstream output;
    inventory.write_json(output, true);
    const std::string json = output.str();
    assert(json.find("\"schema\": \"ee-m2-cfg-isa-inventory-v1\"") != std::string::npos);
    assert(json.find("\"scope\": \"selected_function_cfgs\"") != std::string::npos);
    assert(json.find("\"reached\": \"unmeasured_without_guest_trace\"") != std::string::npos);
    assert(json.find("\"classification\": \"valid_lift_gap\"") != std::string::npos);
    assert(json.find("\"decoder_invalid\": 1") != std::string::npos);
    return 0;
}
