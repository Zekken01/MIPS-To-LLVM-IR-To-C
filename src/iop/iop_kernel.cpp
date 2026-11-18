#include "iop_kernel.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace iop
{
namespace
{
bool valid_range(size_t size, uint64_t offset, uint64_t length)
{
    return offset <= size && length <= size - offset;
}

uint32_t read32(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint32_t>(bytes[offset]) |
           (static_cast<uint32_t>(bytes[offset + 1u]) << 8u) |
           (static_cast<uint32_t>(bytes[offset + 2u]) << 16u) |
           (static_cast<uint32_t>(bytes[offset + 3u]) << 24u);
}

uint16_t read16(const std::vector<uint8_t>& bytes, size_t offset)
{
    return static_cast<uint16_t>(
        bytes[offset] |
        (static_cast<uint32_t>(bytes[offset + 1u]) << 8u)
    );
}

uint64_t align16(uint64_t value)
{
    return (value + 15u) & ~uint64_t(15u);
}

e_kernel_stop stop_for_guest(e_step_result result)
{
    switch (result)
    {
    case e_step_result::memory_fault:
        return e_kernel_stop::memory_fault;
    case e_step_result::overflow:
        return e_kernel_stop::arithmetic_overflow;
    case e_step_result::breakpoint:
        return e_kernel_stop::breakpoint;
    default:
        return e_kernel_stop::unsupported_instruction;
    }
}
} // namespace

c_kernel::c_kernel()
{
    reset();
}

void c_kernel::reset()
{
    m_cpu.bind_hardware(nullptr);
    m_cpu.reset();
    m_hardware.reset();
    m_rom0_boot_active = false;
    m_allocations.clear();
    m_heap_allocations.clear();
    m_libraries.clear();
    m_modules.clear();
    m_threads.clear();
    m_syscall = {};
    m_cycles = 0u;
    m_next_module_id = 1u;
    m_next_thread_id = 1u;
    m_last_scheduled_id = 0u;
    m_current_thread_id = 0u;
}

bool c_kernel::start_rom0_boot(const std::vector<uint8_t>& image)
{
    // The ROM reset path is exclusive with the experimental module
    // bootstrap. A real BIOS is never represented as an IRX thread.
    if (m_rom0_boot_active || !m_modules.empty() ||
        !m_threads.empty() || !m_libraries.empty() ||
        !m_allocations.empty() ||
        !m_cpu.install_rom0(image))
    {
        return false;
    }
    m_cpu.bind_hardware(&m_hardware);
    if (!m_cpu.reset_rom0())
    {
        m_cpu.bind_hardware(nullptr);
        return false;
    }
    m_cycles = 0u;
    m_current_thread_id = 0u;
    m_rom0_boot_active = true;
    return true;
}

bool c_kernel::rom0_boot_active() const
{
    return m_rom0_boot_active;
}

c_hw& c_kernel::hardware()
{
    return m_hardware;
}

const c_hw& c_kernel::hardware() const
{
    return m_hardware;
}

c_cpu& c_kernel::cpu()
{
    return m_cpu;
}

const c_cpu& c_kernel::cpu() const
{
    return m_cpu;
}

uint64_t c_kernel::cycles() const
{
    return m_cycles;
}

uint32_t c_kernel::current_thread_id() const
{
    return m_current_thread_id;
}

bool c_kernel::register_library(
    const std::string& name, uint16_t ordinal, uint32_t entry
)
{
    const uint32_t physical = entry & 0x1fffffffu;
    if (name.empty() || name.size() > 8u ||
        (entry & 3u) != 0u || physical >= c_cpu::k_ram_size)
    {
        return false;
    }

    auto& library = m_libraries[name];
    return library.emplace(ordinal, entry).second;
}

uint32_t c_kernel::allocate(uint32_t size)
{
    const uint64_t required = align16(size);
    if (required == 0u || required > c_cpu::k_ram_size - k_user_base)
    {
        return 0u;
    }

    uint64_t candidate = k_user_base;
    for (const auto& item : m_allocations)
    {
        const uint64_t start = item.first;
        const uint64_t end = start + item.second;
        if (candidate + required <= start)
        {
            break;
        }
        candidate = align16(std::max(candidate, end));
    }

    if (candidate + required > c_cpu::k_ram_size)
    {
        return 0u;
    }

    const uint32_t base = static_cast<uint32_t>(candidate);
    m_allocations.emplace(base, static_cast<uint32_t>(required));
    return base;
}

uint32_t c_kernel::allocate_heap(uint32_t size)
{
    if (m_rom0_boot_active)
    {
        return 0u;
    }
    const uint32_t address = allocate(size);
    if (address != 0u)
    {
        m_heap_allocations.emplace(address, m_allocations.at(address));
    }
    return address;
}

bool c_kernel::free_heap(uint32_t address)
{
    if (m_rom0_boot_active ||
        m_heap_allocations.erase(address) == 0u)
    {
        return false;
    }
    return m_allocations.erase(address) == 1u;
}

uint32_t c_kernel::heap_max_free() const
{
    uint64_t candidate = k_user_base;
    uint64_t largest = 0u;
    for (const auto& item : m_allocations)
    {
        if (item.first > candidate)
        {
            largest = std::max(largest, item.first - candidate);
        }
        candidate = align16(std::max(
            candidate, static_cast<uint64_t>(item.first) + item.second
        ));
    }
    largest = std::max(
        largest, static_cast<uint64_t>(c_cpu::k_ram_size) - candidate
    );
    return static_cast<uint32_t>(largest);
}

uint32_t c_kernel::heap_total_free() const
{
    uint64_t candidate = k_user_base;
    uint64_t total = 0u;
    for (const auto& item : m_allocations)
    {
        if (item.first > candidate)
        {
            total += item.first - candidate;
        }
        candidate = align16(std::max(
            candidate, static_cast<uint64_t>(item.first) + item.second
        ));
    }
    total += static_cast<uint64_t>(c_cpu::k_ram_size) - candidate;
    return static_cast<uint32_t>(total);
}

bool c_kernel::estimate_module_size(
    const std::vector<uint8_t>& bytes, uint32_t* size
) const
{
    if (size == nullptr || bytes.size() < 52u ||
        bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' ||
        bytes[4] != 1u || bytes[5] != 1u)
    {
        return false;
    }

    const uint32_t phoff = read32(bytes, 28u);
    const uint32_t phnum = read16(bytes, 44u);
    const uint32_t phentsize = read16(bytes, 42u);
    if (phentsize < 32u || phnum == 0u ||
        !valid_range(bytes.size(), phoff,
                     static_cast<uint64_t>(phnum) * phentsize))
    {
        return false;
    }

    bool found_load = false;
    bool found_module = false;
    uint32_t memsize = 0u;
    uint32_t iopmod = 0u;
    for (uint32_t index = 0u; index < phnum; ++index)
    {
        const size_t at =
            static_cast<size_t>(phoff) + index * phentsize;
        switch (read32(bytes, at))
        {
        case 1u:
            if (found_load)
            {
                return false;
            }
            found_load = true;
            memsize = read32(bytes, at + 20u);
            break;
        case 0x70000080u:
            if (found_module)
            {
                return false;
            }
            found_module = true;
            iopmod = read32(bytes, at + 4u);
            break;
        default:
            break;
        }
    }

    if (!found_load || !found_module ||
        !valid_range(bytes.size(), iopmod, 26u))
    {
        return false;
    }

    const uint64_t total =
        static_cast<uint64_t>(read32(bytes, iopmod + 12u)) +
        read32(bytes, iopmod + 16u) +
        read32(bytes, iopmod + 20u);
    const uint64_t required = align16(std::max<uint64_t>(
        memsize, total
    ));
    if (required == 0u ||
        required > c_cpu::k_ram_size - k_user_base)
    {
        return false;
    }

    *size = static_cast<uint32_t>(required);
    return true;
}

s_module_load_result c_kernel::load_module(
    const std::vector<uint8_t>& bytes
)
{
    s_module_load_result result{};
    uint32_t required = 0u;
    if (!estimate_module_size(bytes, &required))
    {
        result.m_status = e_irx_load_result::invalid_module;
        return result;
    }

    const uint32_t base = allocate(required);
    if (base == 0u)
    {
        result.m_status = e_irx_load_result::insufficient_ram;
        return result;
    }

    const auto resolve = [this](
        const std::string& library, uint16_t ordinal, uint32_t* target
    )
    {
        const auto exports = m_libraries.find(library);
        if (exports == m_libraries.end() || target == nullptr)
        {
            return false;
        }
        const auto entry = exports->second.find(ordinal);
        if (entry == exports->second.end())
        {
            return false;
        }
        *target = entry->second;
        return true;
    };

    // Reject already registered exports before the IRX loader writes
    // any bytes. This prevents partially installed module state when
    // an authentic library collides with another module or HLE service.
    const auto accept_exports = [this](
        const std::vector<s_irx_export>& exports
    )
    {
        for (const s_irx_export& item : exports)
        {
            const auto library = m_libraries.find(item.m_library);
            if (library != m_libraries.end() &&
                library->second.count(item.m_ordinal) != 0u)
            {
                return false;
            }
        }
        return true;
    };

    s_irx_module image{};
    result.m_status = load_irx(
        m_cpu, bytes, base, &image, resolve, accept_exports
    );
    if (result.m_status != e_irx_load_result::success)
    {
        m_allocations.erase(base);
        return result;
    }

    // All exports were checked against the existing registry and each
    // other on the staged image. Publish only after a successful load.
    for (const s_irx_export& item : image.m_exports)
    {
        m_libraries[item.m_library].emplace(
            item.m_ordinal, item.m_entry
        );
    }
    const uint32_t id = m_next_module_id++;
    m_modules.emplace(id, s_kernel_module{id, std::move(image)});
    result.m_id = id;
    return result;
}

const s_kernel_module* c_kernel::module(uint32_t id) const
{
    const auto found = m_modules.find(id);
    return found == m_modules.end() ? nullptr : &found->second;
}

uint32_t c_kernel::create_thread(
    uint32_t entry, uint32_t gp, uint32_t priority,
    uint32_t stack_size, uint32_t a0, uint32_t a1
)
{
    const uint32_t physical = entry & 0x1fffffffu;
    if (entry == 0u || (entry & 3u) != 0u ||
        physical >= c_cpu::k_ram_size ||
        stack_size < 64u)
    {
        return 0u;
    }

    const uint32_t stack_base = allocate(stack_size);
    if (stack_base == 0u)
    {
        return 0u;
    }
    const uint32_t allocated_size = m_allocations.at(stack_base);
    for (uint32_t offset = 0u;
         offset < allocated_size; offset += 4u)
    {
        m_cpu.write32(stack_base + offset, 0u);
    }

    s_kernel_thread thread{};
    thread.m_id = m_next_thread_id++;
    thread.m_priority = priority;
    thread.m_stack_base = stack_base;
    thread.m_stack_size = allocated_size;
    thread.m_cpu.m_pc = entry;
    thread.m_cpu.m_next_pc = entry + 4u;
    thread.m_cpu.m_gpr[28] = gp;
    thread.m_cpu.m_gpr[29] =
        stack_base + allocated_size - 16u;
    thread.m_cpu.m_gpr[4] = a0;
    thread.m_cpu.m_gpr[5] = a1;
    const uint32_t id = thread.m_id;
    m_threads.emplace(id, std::move(thread));
    return id;
}

uint32_t c_kernel::create_module_thread(
    uint32_t module_id, uint32_t priority, uint32_t stack_size
)
{
    const s_kernel_module* loaded = module(module_id);
    if (loaded == nullptr)
    {
        return 0u;
    }
    return create_thread(
        loaded->m_irx.m_entry,
        loaded->m_irx.m_gp,
        priority, stack_size
    );
}

const s_kernel_thread* c_kernel::thread(uint32_t id) const
{
    const auto found = m_threads.find(id);
    return found == m_threads.end() ? nullptr : &found->second;
}

bool c_kernel::start_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::dormant)
    {
        return false;
    }
    found->second.m_status = e_thread_status::ready;
    return true;
}

bool c_kernel::wake_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::blocked)
    {
        return false;
    }
    found->second.m_status = e_thread_status::ready;
    found->second.m_wake_cycle = 0u;
    return true;
}

bool c_kernel::stop_thread(uint32_t id)
{
    const auto found = m_threads.find(id);
    if (found == m_threads.end() ||
        found->second.m_status == e_thread_status::faulted)
    {
        return false;
    }
    found->second.m_status = e_thread_status::dormant;
    found->second.m_wake_cycle = 0u;
    return true;
}

bool c_kernel::sleep_current(uint64_t delay_cycles)
{
    const auto found = m_threads.find(m_current_thread_id);
    if (found == m_threads.end() ||
        found->second.m_status != e_thread_status::running ||
        delay_cycles == 0u ||
        delay_cycles > std::numeric_limits<uint64_t>::max() -
                       m_cycles)
    {
        return false;
    }
    found->second.m_status = e_thread_status::blocked;
    found->second.m_wake_cycle = m_cycles + delay_cycles;
    return true;
}

void c_kernel::bind_syscall(t_kernel_syscall handler)
{
    m_syscall = std::move(handler);
}

uint32_t c_kernel::next_ready() const
{
    uint32_t priority = std::numeric_limits<uint32_t>::max();
    uint32_t candidate = 0u;
    uint32_t wrapped = 0u;

    for (const auto& item : m_threads)
    {
        const s_kernel_thread& thread = item.second;
        if (thread.m_status != e_thread_status::ready)
        {
            continue;
        }
        if (thread.m_priority < priority)
        {
            priority = thread.m_priority;
            candidate = 0u;
            wrapped = 0u;
        }
        if (thread.m_priority == priority)
        {
            if (wrapped == 0u)
            {
                wrapped = thread.m_id;
            }
            if (thread.m_id > m_last_scheduled_id &&
                candidate == 0u)
            {
                candidate = thread.m_id;
            }
        }
    }
    return candidate == 0u ? wrapped : candidate;
}

void c_kernel::wake_due()
{
    for (auto& item : m_threads)
    {
        s_kernel_thread& thread = item.second;
        if (thread.m_status == e_thread_status::blocked &&
            thread.m_wake_cycle != 0u &&
            thread.m_wake_cycle <= m_cycles)
        {
            thread.m_status = e_thread_status::ready;
            thread.m_wake_cycle = 0u;
        }
    }
}

s_kernel_run_result c_kernel::run(
    uint64_t cycle_budget, uint32_t quantum
)
{
    s_kernel_run_result result{};
    if (quantum == 0u)
    {
        quantum = 1u;
    }
    m_current_thread_id = 0u;

    // Unlike an IRX entrypoint, the actual IOP firmware begins at the
    // R3000 reset vector. Execute its own instruction stream directly.
    // Until BIOS kernel imports, interrupts and IOP MMIO are modeled,
    // stop at the first unsupported service without inventing a
    // successful firmware boot.
    if (m_rom0_boot_active)
    {
        uint64_t left = cycle_budget;
        while (left != 0u)
        {
            const uint32_t slice = static_cast<uint32_t>(
                std::min<uint64_t>(left, quantum)
            );
            const s_run_result stepped = m_cpu.run(slice);
            m_cycles += stepped.m_instructions;
            result.m_executed += stepped.m_instructions;
            left -= stepped.m_instructions;
            if (stepped.m_result == e_step_result::budget_exhausted)
            {
                continue;
            }
            result.m_stop =
                stepped.m_result == e_step_result::syscall
                    ? e_kernel_stop::unhandled_syscall
                    : stop_for_guest(stepped.m_result);
            result.m_guest_pc = stepped.m_pc;
            result.m_opcode = stepped.m_opcode;
            return result;
        }
        result.m_stop = e_kernel_stop::budget_exhausted;
        return result;
    }

    uint64_t remaining = cycle_budget;
    while (remaining != 0u)
    {
        wake_due();
        const uint32_t id = next_ready();
        if (id == 0u)
        {
            uint64_t next_wake =
                std::numeric_limits<uint64_t>::max();
            for (const auto& item : m_threads)
            {
                const s_kernel_thread& thread = item.second;
                if (thread.m_status == e_thread_status::blocked &&
                    thread.m_wake_cycle > m_cycles)
                {
                    next_wake = std::min(
                        next_wake, thread.m_wake_cycle
                    );
                }
            }
            if (next_wake ==
                std::numeric_limits<uint64_t>::max())
            {
                m_cycles += remaining;
                result.m_stop = e_kernel_stop::idle;
                break;
            }

            const uint64_t skipped = std::min(
                remaining, next_wake - m_cycles
            );
            m_cycles += skipped;
            remaining -= skipped;
            continue;
        }

        s_kernel_thread& thread = m_threads.at(id);
        thread.m_status = e_thread_status::running;
        m_current_thread_id = id;
        m_last_scheduled_id = id;
        m_cpu.state() = thread.m_cpu;

        const uint32_t slice = static_cast<uint32_t>(
            std::min<uint64_t>(remaining, quantum)
        );
        const s_run_result step_result = m_cpu.run(slice);
        m_cycles += step_result.m_instructions;
        result.m_executed += step_result.m_instructions;
        remaining -= step_result.m_instructions;

        if (step_result.m_result == e_step_result::syscall)
        {
            // A handled syscall consumes one guest instruction, including
            // its kernel HLE boundary. It is never silently ignored.
            const uint32_t number =
                m_cpu.state().m_gpr[2];
            const bool handled =
                m_syscall && m_syscall(*this, number);
            if (!handled)
            {
                thread.m_status = e_thread_status::faulted;
                result.m_stop =
                    e_kernel_stop::unhandled_syscall;
                result.m_thread_id = id;
                result.m_guest_pc = step_result.m_pc;
                result.m_opcode = step_result.m_opcode;
                thread.m_cpu = m_cpu.state();
                m_current_thread_id = 0u;
                return result;
            }
            m_cpu.resume_syscall();
            ++m_cycles;
            ++result.m_executed;
            --remaining;
        }
        else if (step_result.m_result !=
                 e_step_result::budget_exhausted)
        {
            thread.m_status = e_thread_status::faulted;
            result.m_stop = stop_for_guest(
                step_result.m_result
            );
            result.m_thread_id = id;
            result.m_guest_pc = step_result.m_pc;
            result.m_opcode = step_result.m_opcode;
            thread.m_cpu = m_cpu.state();
            m_current_thread_id = 0u;
            return result;
        }

        thread.m_cpu = m_cpu.state();
        if (thread.m_status == e_thread_status::running)
        {
            thread.m_status = e_thread_status::ready;
        }
        m_current_thread_id = 0u;
    }

    return result;
}
} // namespace iop
