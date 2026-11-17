#include "ee_vsync_flags.h"
#include "ee_memory.h"

#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{

int g_failed = 0;

void check(bool accepted, const char* description)
{
    if (!accepted)
    {
        std::printf("FAIL: %s\n", description);
        ++g_failed;
    }
}

} // namespace

int main()
{
    ee::ee_memory ram(0x1000u);
    ee::c_ee_vsync_flags flags{};

    ram.write32(0x100u, 0x11223344u);
    ram.write64(0x108u, 0x8877665544332211ull);
    check(flags.register_guest_pointers(&ram, 0x100u, 0x108u),
          "registration accepts aligned, backed EE RAM pointers");
    check(flags.flag_address() == 0x100u &&
          flags.csr_address() == 0x108u,
          "registration retains both guest pointers");
    check(ram.read32(0x100u) == 0x11223344u &&
          ram.read64(0x108u) == 0x8877665544332211ull,
          "registration never synthesizes VSYNC, CSR or guest writes");

    // Reject malformed input atomically, preserving prior registration
    // and guest data. A direct-mapped address is not automatically backed.
    for (uint32_t invalid : {0x101u, 0xffeu, 0x1000u, 0x80001000u})
    {
        check(!flags.register_guest_pointers(&ram, invalid, 0x108u) &&
              flags.flag_address() == 0x100u &&
              flags.csr_address() == 0x108u,
              "invalid flag destination cannot partially replace registration");
    }
    for (uint32_t invalid : {0x109u, 0xffcu, 0x1000u, 0x80001000u})
    {
        check(!flags.register_guest_pointers(&ram, 0x100u, invalid) &&
              flags.flag_address() == 0x100u &&
              flags.csr_address() == 0x108u,
              "invalid CSR destination cannot partially replace registration");
    }
    check(!flags.register_guest_pointers(nullptr, 0x100u, 0x108u),
          "registration requires memory provenance");
    check(ram.read32(0x100u) == 0x11223344u &&
          ram.read64(0x108u) == 0x8877665544332211ull,
          "refused registration never changes either guest value");

    // The test supplies a synthetic event *explicitly*: production must
    // wait for a genuine GS vertical-blank producer to call this method.
    const uint64_t real_csr = 0x0123456789abcdefull;
    check(!flags.deliver_actual_vsync(nullptr, real_csr),
          "event delivery requires memory");
    check(flags.deliver_actual_vsync(&ram, real_csr) &&
          ram.read32(0x100u) == 1u &&
          ram.read64(0x108u) == real_csr,
          "only explicit VSync delivery updates the registered flag and actual CSR");

    // Re-registering with nullable pointers clears the corresponding
    // consumer, without backfilling or inventing a historical event.
    ram.write32(0x100u, 0x10203040u);
    ram.write64(0x108u, 0x1122334455667788ull);
    check(flags.register_guest_pointers(&ram, 0, 0x108u) &&
          ram.read32(0x100u) == 0x10203040u &&
          ram.read64(0x108u) == 0x1122334455667788ull,
          "partial re-registration does not imply VSync");
    check(flags.deliver_actual_vsync(&ram, 0xfedcba9876543210ull) &&
          ram.read32(0x100u) == 0x10203040u &&
          ram.read64(0x108u) == 0xfedcba9876543210ull,
          "event delivery honors a null flag pointer");
    check(flags.register_guest_pointers(&ram, 0x100u, 0u),
          "null CSR pointer registration");
    check(flags.deliver_actual_vsync(&ram, real_csr) &&
          ram.read32(0x100u) == 1u &&
          ram.read64(0x108u) == 0xfedcba9876543210ull,
          "event delivery honors a null CSR pointer");
    check(flags.register_guest_pointers(&ram, 0u, 0u) &&
          flags.flag_address() == 0u &&
          flags.csr_address() == 0u,
          "null pointer pair clears both destinations");

    std::printf("ee_vsync_flag_test: %d failures\n", g_failed);
    return g_failed == 0 ? 0 : 1;
}
