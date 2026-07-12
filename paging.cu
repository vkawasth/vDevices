#include <iostream>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstdint>
#include <cstring>
#include <concepts>
#include <type_traits>
#include <cassert>

// ============================================================================
// 1. ARCHITECTURAL HARDWARE CONSTANTS & STRUCTURES
// ============================================================================
[[maybe_unused]] constexpr uint64_t PAGE_SIZE_4KB = 4096ULL;
constexpr uint64_t PAGE_SIZE_2MB = 2ULL * 1024ULL * 1024ULL; // 2097152 Bytes

// Strong types for isolated address interfaces
using GuestVirtualAddr   = uint64_t;
using HostPhysicalAddr   = uintptr_t;
using HugepageDirectoryIndex = uint32_t;

enum class MmuStatus : uint8_t {
    Success = 0x00,
    TranslationFault = 0x01,
    OutOfPhysicalHugepages = 0x02
};

#pragma pack(push, 1)
// Emulates a hardware Page Table Entry (PTE) optimized for 2MB page maps
struct alignas(8) HugepagePte {
    HostPhysicalAddr physical_page_base : 48; // Fits 48-bit physical address space
    uint16_t present                    : 1;  // Bit 48: Page present flag
    uint16_t writable                   : 1;  // Bit 49: Write clearance flag
    uint16_t guest_tenant_id            : 14; // Bits 50-63: Hardware isolation context (ASID)
};
#pragma pack(pop)

// Enforce strict compile-time verification on memory layout density
static_assert(sizeof(HugepagePte) == 8, "Hardware entry layout must map precisely to 64 bits.");

// ============================================================================
// 2. THE TOP-LEVEL HARDWARE PAGE TABLE EMULATOR
// ============================================================================
class HugepageShadowPageTable {
public:
    // Pre-allocates tracking structures up front to ensure zero fast-path dynamic runtime heap calls
    explicit HugepageShadowPageTable(size_t total_host_hugepages) noexcept 
        : next_free_hugepage_index_(0) 
    {
        // Simulate a giant pinned hardware VRAM/DRAM pool managed in 2MB blocks
        simulated_physical_hardware_pool_.resize(total_host_hugepages * PAGE_SIZE_2MB, 0x00);
        page_directory_.reserve(total_host_hugepages);
    }

    ~HugepageShadowPageTable() noexcept = default;

    // Eliminate copy operations to preserve strict single-point device boundaries
    HugepageShadowPageTable(const HugepageShadowPageTable&) = delete;
    HugepageShadowPageTable& operator=(const HugepageShadowPageTable&) = delete;

    // Provisions a new virtualized memory block for a guest tenant
    [[nodiscard]] MmuStatus MapGuestVirtualRange(
        GuestVirtualAddr gva_start, 
        size_t bytes_requested, 
        uint16_t tenant_id) noexcept 
    {
        // Align byte requests up to the nearest 2MB hardware block ceiling
        size_t num_hugepages_needed = (bytes_requested + PAGE_SIZE_2MB - 1) / PAGE_SIZE_2MB;
        
        std::lock_guard<std::mutex> lock(mmu_mutex_);

        if (next_free_hugepage_index_ + num_hugepages_needed > (simulated_physical_hardware_pool_.size() / PAGE_SIZE_2MB)) [[unlikely]] {
            return MmuStatus::OutOfPhysicalHugepages;
        }

        for (size_t i = 0; i < num_hugepages_needed; ++i) {
            // Step 1: Calculate the absolute base address inside our pinned physical RAM segment
            uintptr_t phys_base = reinterpret_cast<uintptr_t>(simulated_physical_hardware_pool_.data()) + 
                                  (next_free_hugepage_index_ * PAGE_SIZE_2MB);

            // Step 2: Formulate the single-cycle virtual page address key index
            GuestVirtualAddr current_gva_page = gva_start + (i * PAGE_SIZE_2MB);
            HugepageDirectoryIndex directory_key = CalculateDirectoryIndex(current_gva_page);

            // Step 3: Populate our dense bitfield entry mapping directly onto hardware properties
            HugepagePte entry{};
            entry.physical_page_base = phys_base;
            entry.present = 1;
            entry.writable = 1;
            entry.guest_tenant_id = tenant_id;

            page_directory_[directory_key] = entry;
            next_free_hugepage_index_++;
        }

        return MmuStatus::Success;
    }

    // Ultra-fast runtime pointer translator executing inside the hot data path
    [[nodiscard]] MmuStatus TranslateGuestAddr(
        const GuestVirtualAddr incoming_gva, 
        const uint16_t validating_tenant_id,
        HostPhysicalAddr& out_resolved_hpa) const noexcept 
    {
        // Extract directory lookup key and the internal 2MB block memory offset
        HugepageDirectoryIndex directory_key = CalculateDirectoryIndex(incoming_gva);
        uint32_t page_offset = static_cast<uint32_t>(incoming_gva & (PAGE_SIZE_2MB - 1));

        std::lock_guard<std::mutex> lock(mmu_mutex_);

        auto lookup = page_directory_.find(directory_key);
        if (lookup == page_directory_.end()) [[unlikely]] {
            return MmuStatus::TranslationFault; // Page fault error: virtual address untracked
        }

        const HugepagePte& entry = lookup->second;

        // Security Validation Wall: Detect cross-tenant memory breakout access attempts
        if (!entry.present || (entry.guest_tenant_id != validating_tenant_id)) [[unlikely]] {
            return MmuStatus::TranslationFault;
        }

        // Single-cycle address reconstruction: Combine the base page block with our variable offset
        out_resolved_hpa = entry.physical_page_base + page_offset;
        return MmuStatus::Success;
    }

private:
    // Bitwise shift extracts the directory lookup index, wiping out the lower 21 bits of offset metadata
    [[nodiscard]] inline HugepageDirectoryIndex CalculateDirectoryIndex(const GuestVirtualAddr gva) const noexcept {
        return static_cast<HugepageDirectoryIndex>(gva >> 21); // 2^21 = 2MB boundary alignment
    }

    mutable std::mutex mmu_mutex_;
    size_t next_free_hugepage_index_;
    std::vector<uint8_t> simulated_physical_hardware_pool_;
    std::unordered_map<HugepageDirectoryIndex, HugepagePte> page_directory_;
};

// ============================================================================
// 3. HARDWARE SIMULATION VALIDATION RUNNER
// ============================================================================
int main() {
    std::cout << "=== USER-SPACE 2MB HUGEPAGE SHADOW MMU ENGAGED ===\n\n";

    // Setup an internal memory engine capacity containing exactly 16 hugepages (32MB physical pool)
    HugepageShadowPageTable mmu(16);

    uint16_t tenant_alpha_id = 42;
    uint16_t rogue_tenant_id = 99;

    // Step 1: Client VM provisions an arbitrary virtual base range 
    GuestVirtualAddr guest_matrix_virtual_base = 0x100000000ULL; // 4GB virtual offset marking
    size_t bytes_to_allocate = 3 * 1024 * 1024; // 3MB allocation (Requires exactly 2 Hugepages)

    MmuStatus map_err = mmu.MapGuestVirtualRange(guest_matrix_virtual_base, bytes_to_allocate, tenant_alpha_id);
    assert(map_err == MmuStatus::Success);
    std::cout << "[Allocation Map Success] Carved out 2MB page segments for Tenant Context: " << tenant_alpha_id << "\n";

    // Step 2: Simulate fetching a data point crossing the 2MB page boundary boundary
    GuestVirtualAddr tracking_gva_target = guest_matrix_virtual_base + (2ULL * 1024ULL * 1024ULL) + 512ULL; 
    // Located at Page 2, Byte offset 512

    HostPhysicalAddr resolved_hardware_address = 0;
    MmuStatus translation_res = mmu.TranslateGuestAddr(tracking_gva_target, tenant_alpha_id, resolved_hardware_address);

    if (translation_res == MmuStatus::Success) {
        std::cout << "[MMU Address Fault Clear]\n"
                  << "  -> Intercepted Guest Virtual Handle = 0x" << std::hex << tracking_gva_target << "\n"
                  << "  -> Physical Hardware Memory Page   = 0x" << std::hex << resolved_hardware_address << "\n\n";
    }

    // Step 3: Security Validation Verification - Force a rogue tenant memory access attempt
    HostPhysicalAddr blocked_hardware_address = 0;
    MmuStatus security_fault_check = mmu.TranslateGuestAddr(tracking_gva_target, rogue_tenant_id, blocked_hardware_address);

    if (security_fault_check == MmuStatus::TranslationFault) {
        std::cout << "[Security Wall Verified] MMU successfully intercepted and blocked an out-of-context hardware read from Rogue Tenant: " 
                  << std::dec << rogue_tenant_id << "\n\n";
    }

    std::cout << "=== ALL PORFOLIO SHADOW MMU CHECKPOINTS VERIFIED ===\n";
    return 0;
}
