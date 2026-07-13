#include <iostream>
#include <cstdint>
#include <array>
#include <iomanip>
#include <vector>

// ============================================================================
// 1. Structural Configuration & Metaprogramming Engine
// ============================================================================
template <size_t BitsPerPageLevel, size_t PageOffsetBits>
struct PageLayout {
    template <size_t Level>
    static constexpr size_t get_shift() {
        return PageOffsetBits + ((Level - 1) * BitsPerPageLevel);
    }
    static constexpr uint64_t get_mask() { return (1ULL << BitsPerPageLevel) - 1; }
    static constexpr uint64_t get_offset_mask() { return (1ULL << PageOffsetBits) - 1; }
};

// Define standard x86_64 4-Level Paging configuration
using X86_64Layout = PageLayout<9, 12>;

// Architectural Flag Constants (x86-64 bits)
constexpr uint64_t PTE_PRESENT_MASK   = 1ULL << 0;  // Bit 0: Present flag
constexpr uint64_t PTE_RW_MASK        = 1ULL << 1;  // Bit 1: Read/Write (0=Read-Only, 1=Read/Write)
constexpr uint64_t PTE_US_MASK        = 1ULL << 2;  // Bit 2: User/Supervisor (0=Supervisor, 1=User)
constexpr uint64_t PTE_HUGE_PAGE_MASK = 1ULL << 7;  // Bit 7: Page Size (PS) / Huge Page
constexpr uint64_t PHYSICAL_ADDR_MASK = 0x000FFFFFFFFFF000ULL;

// Enumeration for checking runtime processing contexts
enum class AccessType { READ, WRITE };
enum class PrivilegeLevel { SUPERVISOR, USER };

struct AccessContext {
    AccessType access;
    PrivilegeLevel privilege;
};


// ============================================================================
// 2. Hardware Component Simulation: Translation Lookaside Buffer (TLB)
// ============================================================================
struct TLBEntry {
    uint64_t virtual_page_number = 0;
    uint64_t physical_frame_base = 0;
    uint64_t permissions         = 0; 
    bool is_valid                = false;
};

class SoftwareTLB {
private:
    static constexpr size_t TLB_SIZE = 4;
    std::array<TLBEntry, TLB_SIZE> entries;
    size_t victim_index = 0; // Simple Round-Robin Replacement policy

public:
    SoftwareTLB() { entries.fill(TLBEntry{}); }

    // Scans the cache for a Virtual Page Number (VPN)
    bool lookup(uint64_t vpn, uint64_t& out_pfb, uint64_t& out_perms) {
        for (const auto& entry : entries) {
            if (entry.is_valid && entry.virtual_page_number == vpn) {
                out_pfb = entry.physical_frame_base;
                out_perms = entry.permissions;
                return true;
            }
        }
        return false;
    }

    // Inserts or replaces a translation mapping inside the cache
    void insert(uint64_t vpn, uint64_t pfb, uint64_t perms) {
        entries[victim_index] = TLBEntry{vpn, pfb, perms, true};
        victim_index = (victim_index + 1) % TLB_SIZE; // Advance tracking index
    }

    void flush() {
        entries.fill(TLBEntry{});
        std::cout << "[TLB] Cache flushed successfully.\n";
    }
};


// ============================================================================
// 3. CRTP Architecture Base Framework with Integrated Security Layer
// ============================================================================
template <typename Implementation, size_t CurrentLevel>
struct TableWalkEngine;

template <typename PageWalkerImpl>
class HardwarePageTableWalker {
protected:
    SoftwareTLB tlb;

public:
    // Public validation interface so the external template layout engine can invoke it
    bool validate_permissions(uint64_t pte, const AccessContext& ctx, size_t level) {
        // 1. Present Verification
        if (!(pte & PTE_PRESENT_MASK)) {
            std::cout << "[!] PROTECTION FAULT (Level " << level << "): Page not present.\n";
            return false;
        }
        // 2. Write Policy Violation
        if (ctx.access == AccessType::WRITE && !(pte & PTE_RW_MASK)) {
            std::cout << "[!] PROTECTION FAULT (Level " << level << "): Write attempting on Read-Only space.\n";
            return false;
        }
        // 3. Privilege Isolation Breach
        if (ctx.privilege == PrivilegeLevel::USER && !(pte & PTE_US_MASK)) {
            std::cout << "[!] PROTECTION FAULT (Level " << level << "): User mode execution attempting to access Supervisor space.\n";
            return false;
        }
        return true;
    }

    void translate_address(uint64_t virtual_address, AccessContext ctx) {
        std::cout << "\n=======================================================================\n";
        std::cout << "[MMU Engine] Context: " 
                  << (ctx.privilege == PrivilegeLevel::USER ? "USER" : "SUPERVISOR") << " "
                  << (ctx.access == AccessType::WRITE ? "WRITE" : "READ") << "\n";
        std::cout << "[MMU Engine] Processing VA: 0x" << std::hex << std::setw(16) << std::setfill('0') << virtual_address << "\n";

        uint64_t vpn = virtual_address >> 12; // Lower 12 bits represent the page offset
        uint64_t cached_pfb = 0;
        uint64_t cached_perms = 0;

        // Step A: Attempt fast-path processing via TLB Cache
        if (tlb.lookup(vpn, cached_pfb, cached_perms)) {
            std::cout << "--> [TLB HIT] Bypassed hardware page walk engine!\n";
            if (!validate_permissions(cached_perms, ctx, 0)) return; // 0 denotes a unified cache block check
            
            uint64_t offset = virtual_address & X86_64Layout::get_offset_mask();
            std::cout << "--> [SUCCESS] Translated Physical Address: 0x" << std::hex << (cached_pfb | offset) << "\n";
            return;
        }

        std::cout << "[TLB MISS] Initiating 4-Level MMU Page Table Walk...\n";
        
        // Step B: Slow-path table walk starting from root (PML4) physical index base 0x0
        using RootEngine = TableWalkEngine<PageWalkerImpl, 4>;
        RootEngine::execute(static_cast<PageWalkerImpl*>(this), virtual_address, 0x0ULL, ctx, &tlb);
    }

    void flush_tlb() { tlb.flush(); }
};


// ============================================================================
// 4. Compile-Time Page Table Walk Implementation
// ============================================================================
template <typename Implementation, size_t CurrentLevel>
struct TableWalkEngine {
    static void execute(Implementation* impl, uint64_t va, uint64_t table_phys_base, const AccessContext& ctx, SoftwareTLB* tlb_ptr) {
        constexpr size_t shift = X86_64Layout::get_shift<CurrentLevel>();
        constexpr uint64_t mask = X86_64Layout::get_mask();
        
        uint64_t index = (va >> shift) & mask;
        std::cout << "  [L" << CurrentLevel << "] Core Entry Pointer: 0x" << std::hex << table_phys_base << " | Index: " << std::dec << index << "\n";

        uint64_t pte = impl->read_pte(table_phys_base, index);

        // Permissions must be structurally valid at EVERY directory tier along the resolution path
        if (!impl->validate_permissions(pte, ctx, CurrentLevel)) return;

        // Check if we hit an early Terminal Huge Page allocation node
        if (pte & PTE_HUGE_PAGE_MASK) {
            uint64_t phys_page_base = pte & PHYSICAL_ADDR_MASK;
            uint64_t huge_page_mask = (1ULL << shift) - 1;
            uint64_t final_phys = phys_page_base | (va & huge_page_mask);
            
            const char* size_str = (CurrentLevel == 3) ? "1 GB" : "2 MB";
            std::cout << "--> [SUCCESS] Hit Large Framework Boundary (" << size_str << ")\n";
            std::cout << "--> Translated Physical Address: 0x" << std::hex << final_phys << "\n";

            // Populate TLB tracking maps to skip future structural processing overhead
            tlb_ptr->insert(va >> 12, phys_page_base, pte);
            return;
        }

        // Descend sequentially down to the adjacent execution layer
        uint64_t next_table_base = pte & PHYSICAL_ADDR_MASK;
        using NextEngine = TableWalkEngine<Implementation, CurrentLevel - 1>;
        NextEngine::execute(impl, va, next_table_base, ctx, tlb_ptr);
    }
};

// Specialized base structural resolution handler (Level 1 Terminal)
template <typename Implementation>
struct TableWalkEngine<Implementation, 1> {
    static void execute(Implementation* impl, uint64_t va, uint64_t table_phys_base, const AccessContext& ctx, SoftwareTLB* tlb_ptr) {
        constexpr size_t shift = X86_64Layout::get_shift<1>();
        constexpr uint64_t mask = X86_64Layout::get_mask();
        
        uint64_t index = (va >> shift) & mask;
        std::cout << "  [L1] Core Entry Pointer: 0x" << std::hex << table_phys_base << " | Index: " << std::dec << index << "\n";

        uint64_t pte = impl->read_pte(table_phys_base, index);

        if (!impl->validate_permissions(pte, ctx, 1)) return;

        uint64_t physical_frame_base = pte & PHYSICAL_ADDR_MASK;
        uint64_t page_offset = va & X86_64Layout::get_offset_mask();
        uint64_t final_physical_address = physical_frame_base | page_offset;

        std::cout << "--> [SUCCESS] Reached Standard 4KB Page Endpoint.\n";
        std::cout << "--> Translated Physical Address: 0x" << std::hex << final_physical_address << "\n";

        // Cache the newly resolved translation path mapping inside the TLB hardware matrix
        tlb_ptr->insert(va >> 12, physical_frame_base, pte);
    }
};


// ============================================================================
// 5. Simulated Memory System Backing Environment
// ============================================================================
class HardwarePlatformSimulator : public HardwarePageTableWalker<HardwarePlatformSimulator> {
public:
    // Generates static mocked register entries mimicking complex OS memory layouts
    uint64_t read_pte(uint64_t table_phys_base, uint64_t index) {
        // Standard User Space Target (VA path 1)
        // Set US bit (0x4) and RW bit (0x2) on structural tiers to allow generic passage

        // Standard User Space Target (VA path 1)
        // Set US bit (0x4) and RW bit (0x2) on structural tiers to allow generic passage
        if (table_phys_base == 0x00000ULL && index == 0x05) return 0x10000ULL | PTE_PRESENT_MASK | PTE_RW_MASK | PTE_US_MASK;
        if (table_phys_base == 0x10000ULL && index == 0x0A) return 0x20000ULL | PTE_PRESENT_MASK | PTE_RW_MASK | PTE_US_MASK;
        if (table_phys_base == 0x20000ULL && index == 0x0F) return 0x30000ULL | PTE_PRESENT_MASK | PTE_RW_MASK | PTE_US_MASK;
        
        // Terminal L1 Node has strict read-only permissions configured (PTE_RW_MASK omitted)
        if (table_phys_base == 0x30000ULL && index == 0x01) return 0x7FFF000ULL | PTE_PRESENT_MASK | PTE_US_MASK;

        // Kernel Protected Space Target (VA path 2)
        // US bit (0x4) is left omitted to block non-privileged hardware applications
        if (table_phys_base == 0x00000ULL && index == 0x80) return 0x40000ULL | PTE_PRESENT_MASK | PTE_RW_MASK; 
        if (table_phys_base == 0x40000ULL && index == 0x00) return 0x50000ULL | PTE_PRESENT_MASK | PTE_RW_MASK;
        if (table_phys_base == 0x50000ULL && index == 0x00) return 0x60000ULL | PTE_PRESENT_MASK | PTE_RW_MASK;
        if (table_phys_base == 0x60000ULL && index == 0x00) return 0x9FFF000ULL | PTE_PRESENT_MASK | PTE_RW_MASK;

        return 0x0ULL; // Triggers Page Fault
    }
};

// ============================================================================
// 6. Execution Test Harness
// ============================================================================
int main() {
    HardwarePlatformSimulator mmu;

    // Synthesize target addresses using bit offsets
    uint64_t user_va   = (5ULL << 39)  | (10ULL << 30) | (15ULL << 21) | (1ULL << 12) | 0x111ULL;
    uint64_t kernel_va = (128ULL << 39)| (0ULL << 30)  | (0ULL << 21)  | (0ULL << 12)  | 0x222ULL;

    // Test Scenario 1: Processing a Valid User Read access (Will Miss TLB, Walk Tables)
    mmu.translate_address(user_va, {AccessType::READ, PrivilegeLevel::USER});

    // Test Scenario 2: Repeating the same translation request (Will Hit TLB, Instant Access)
    mmu.translate_address(user_va, {AccessType::READ, PrivilegeLevel::USER});

    // Test Scenario 3: Checking Permission Fault - User attempting a Write onto a Read-Only page
    mmu.translate_address(user_va, {AccessType::WRITE, PrivilegeLevel::USER});

    // Test Scenario 4: Checking Privilege Fault - User trying to read supervisor space
    mmu.translate_address(kernel_va, {AccessType::READ, PrivilegeLevel::USER});

    // Test Scenario 5: Successful Supervisor Access to kernel space (Will Walk Tables)
    mmu.translate_address(kernel_va, {AccessType::READ, PrivilegeLevel::SUPERVISOR});

    return 0;
}

