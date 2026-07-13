#include <iostream>
#include <cstdint>
#include <array>
#include <iomanip>

// ============================================================================
// 1. Template Metaprogramming Configuration Engine
// ============================================================================
template <size_t BitsPerPageLevel, size_t PageOffsetBits>
struct PageLayout {
    template <size_t Level>
    static constexpr size_t get_shift() {
        // Level 4 = 39, Level 3 = 30, Level 2 = 21, Level 1 = 12
        return PageOffsetBits + ((Level - 1) * BitsPerPageLevel);
    }

    static constexpr uint64_t get_mask() {
        return (1ULL << BitsPerPageLevel) - 1;
    }

    static constexpr uint64_t get_offset_mask() {
        return (1ULL << PageOffsetBits) - 1;
    }
};

// Define standard x86_64 4-Level Paging configuration
using X86_64Layout = PageLayout<9, 12>;

// Architecture flag masks [2]
constexpr uint64_t PTE_PRESENT_MASK = 0x1ULL;
constexpr uint64_t PTE_HUGE_PAGE_MASK = 0x80ULL; // PS bit (Page Size) in CR3 structures [2]
constexpr uint64_t PHYSICAL_ADDR_MASK = 0x000FFFFFFFFFF000ULL;


// ============================================================================
// 2. Metaprogramming Unroller & Static Base Class
// ============================================================================

// Forward declaration of the execution engine loop [1]
template <typename Implementation, size_t CurrentLevel>
struct TableWalkEngine;

template <typename PageWalkerImpl>
class AdvancedPageTableWalker {
public:
    void walk_tables(uint64_t virtual_address) {
        std::cout << "\n=======================================================\n";
        std::cout << "[Base VMM] Resolving Virtual Address: 0x" 
                  << std::hex << std::setw(16) << std::setfill('0') << virtual_address << "\n";
        
        // Start the compile-time recursion engine at Level 4 (PML4) [1]
        // We pass the root index zero as our starting pseudo-physical table address
        TableWalkEngine<PageWalkerImpl, 4>::execute(
            static_cast<PageWalkerImpl*>(this), 
            virtual_address, 
            0x0ULL
        );
    }
};


// ============================================================================
// 3. Compile-Time Walk Engine Loop (Levels 4 down to 2) [1]
// ============================================================================
// ============================================================================
// 3. Compile-Time Walk Engine Loop (Levels 4 down to 2) - FIXED
// ============================================================================
template <typename Implementation, size_t CurrentLevel>
struct TableWalkEngine {
    static void execute(Implementation* impl, uint64_t va, uint64_t table_phys_base) {
        constexpr size_t shift = X86_64Layout::get_shift<CurrentLevel>();
        constexpr uint64_t mask = X86_64Layout::get_mask();
        
        uint64_t index = (va >> shift) & mask;
        std::cout << "[Level " << CurrentLevel << "] Table Base: 0x" << std::hex << table_phys_base 
                  << " | Index: " << std::dec << index << "\n";

        // Query our concrete hardware mock implementation (CRTP static dispatch)
        uint64_t pte = impl->read_pte(table_phys_base, index);

        if (!(pte & PTE_PRESENT_MASK)) {
            std::cout << "[!] PAGE FAULT at Level " << CurrentLevel << "! Entry missing.\n";
            return;
        }

        // Support Huge Pages: If Level 3 (1GB) or Level 2 (2MB) has the Huge Page bit set, stop walking
        if (pte & PTE_HUGE_PAGE_MASK) {
            uint64_t phys_page_base = pte & PHYSICAL_ADDR_MASK;
            uint64_t huge_page_mask = (1ULL << shift) - 1;
            uint64_t final_phys = phys_page_base | (va & huge_page_mask);
            
            // Fixed: Pull string selection into a clear variable to prevent template parsing issues
            const char* size_str = (CurrentLevel == 3) ? "1 GB" : "2 MB";
            
            std::cout << "--> [SUCCESS] Hit Huge Page Flag (" << size_str << ")!\n";
            std::cout << "--> Physical Frame Base: 0x" << std::hex << phys_page_base << "\n";
            std::cout << "--> Translated Physical Address: 0x" << std::hex << final_phys << "\n";
            return;
        }

        // Keep searching: Recurse to the next level down
        uint64_t next_table_base = pte & PHYSICAL_ADDR_MASK;
        
        // Fixed: Isolate the template invocation to prevent syntax confusion
        using NextEngine = TableWalkEngine<Implementation, CurrentLevel - 1>;
        NextEngine::execute(impl, va, next_table_base);
    }
};


// ============================================================================
// 4. Base Template Termination: Level 1 (The 4KB Translation Target) [1]
// ============================================================================
template <typename Implementation>
struct TableWalkEngine<Implementation, 1> { // Specialized template terminal condition [1]
    static void execute(Implementation* impl, uint64_t va, uint64_t table_phys_base) {
        constexpr size_t shift = X86_64Layout::get_shift<1>();
        constexpr uint64_t mask = X86_64Layout::get_mask();
        
        uint64_t index = (va >> shift) & mask;
        std::cout << "[Level 1] Table Base: 0x" << std::hex << table_phys_base 
                  << " | Index: " << std::dec << index << "\n";

        uint64_t pte = impl->read_pte(table_phys_base, index);

        if (!(pte & PTE_PRESENT_MASK)) {
            std::cout << "[!] PAGE FAULT at Level 1 (PT)! Frame entry is missing.\n";
            return;
        }

        // Extract physical framework base and tack on standard 12-bit page offset
        uint64_t physical_frame_base = pte & PHYSICAL_ADDR_MASK;
        uint64_t page_offset = va & X86_64Layout::get_offset_mask();
        uint64_t final_physical_address = physical_frame_base | page_offset;

        std::cout << "--> [SUCCESS] Translated Standard 4KB Page Target.\n";
        std::cout << "--> Physical Frame Base: 0x" << std::hex << physical_frame_base << "\n";
        std::cout << "--> Translated Physical Address: 0x" << std::hex << final_physical_address << "\n";
    }
};


// ============================================================================
// 5. Hardware Simulator Mock Environment
// ============================================================================
class SimulatedMemorySystem : public AdvancedPageTableWalker<SimulatedMemorySystem> {
public:
    // Simple reader that simulates an multi-level physical backing layer
    uint64_t read_pte(uint64_t table_phys_base, uint64_t index) {
        // Test Address 1 Path: Maps Standard 4KB resolution
        if (table_phys_base == 0x00000ULL && index == 0x05) return 0x10000ULL | PTE_PRESENT_MASK;  // To L3
        if (table_phys_base == 0x10000ULL && index == 0x0A) return 0x20000ULL | PTE_PRESENT_MASK;  // To L2
        if (table_phys_base == 0x20000ULL && index == 0x0F) return 0x30000ULL | PTE_PRESENT_MASK;  // To L1
        if (table_phys_base == 0x30000ULL && index == 0x01) return 0x7FFFFFFF000ULL | PTE_PRESENT_MASK; // Final Page Frame

        // Test Address 2 Path: Maps a 2MB Huge Page directly out of Level 2 [2]
        if (table_phys_base == 0x00000ULL && index == 0x42) return 0x80000ULL | PTE_PRESENT_MASK;  // To L3
        if (table_phys_base == 0x80000ULL && index == 0x00) return 0x90000ULL | PTE_PRESENT_MASK;  // To L2
        if (table_phys_base == 0x90000ULL && index == 0x02) {
            // Returns base address with both PRESENT and HUGE_PAGE flag configurations checked [2]
            return 0xAAAAAA000ULL | PTE_PRESENT_MASK | PTE_HUGE_PAGE_MASK; 
        }

        // Anything else triggers page faults
        return 0x0ULL; 
    }
};


// ============================================================================
// 6. Test Orchestration Execution
// ============================================================================
int main() {
    SimulatedMemorySystem vmm;

    // Test Scenario 1: Standard 4KB Page Translation
    // Indexes: L4=5, L3=10, L2=15, L1=1, Offset=0xABC
    uint64_t standard_va = (5ULL << 39) | (10ULL << 30) | (15ULL << 21) | (1ULL << 12) | 0xABCULL;
    vmm.walk_tables(standard_va);

    // Test Scenario 2: 2MB Huge Page Translation [2]
    // Indexes: L4=42, L3=0, L2=2, Offset/Lower=0x1F5000 (Lower 21 bits are huge page properties) [2]
    uint64_t huge_va = (42ULL << 39) | (0ULL << 30) | (2ULL << 21) | 0x1F5000ULL;
    vmm.walk_tables(huge_va);

    // Test Scenario 3: Page Fault Execution
    uint64_t unmapped_va = 0xDEADBEEF0000ULL;
    vmm.walk_tables(unmapped_va);

    return 0;
}

