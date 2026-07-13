#include <iostream>
#include <cstdint>
#include <array>
#include <iomanip>

// ==========================================
// 1. Template Metaprogramming Layer
// ==========================================

// Compile-time structure to calculate page layout attributes automatically
template <size_t BitsPerPageLevel, size_t PageOffsetBits>
struct PageLayout {
    // Calculates the required bit shift for a given 1-indexed level (e.g., Level 4, Level 3)
    template <size_t Level>
    static constexpr size_t get_shift() {
        return PageOffsetBits + ((Level - 1) * BitsPerPageLevel);
    }

    // Calculates the bitmask based on how many bits represent an index at each level
    static constexpr uint64_t get_mask() {
        return (1ULL << BitsPerPageLevel) - 1;
    }
};

// Standard x86_64 architecture constants evaluated via layout engine
// 9 bits per level (512 entries), 12 bits for a standard 4KB page offset
using X86_64Layout = PageLayout<9, 12>;


// ==========================================
// 2. CRTP Base Interface Class
// ==========================================
template <typename PageWalkerImpl>
class PageTableWalker {
public:
    void walk_tables(uint64_t virtual_address) {
        std::cout << "\n[Walker Base] Initiating walk for VA: 0x" 
                  << std::hex << std::setw(16) << std::setfill('0') << virtual_address << "\n";

        // Metaprogramming engine calculates the precise Level 4 shift at compile time (12 + 3 * 9 = 39)
        constexpr size_t L4_SHIFT = X86_64Layout::get_shift<4>();
        constexpr uint64_t L4_MASK = X86_64Layout::get_mask();

        uint64_t l4_index = (virtual_address >> L4_SHIFT) & L4_MASK;
        
        std::cout << "[Walker Base] Extracted Level 4 Index: " << std::dec << l4_index << "\n";

        // CRTP Static Dispatch down to the concrete hardware implementation
        uint64_t entry = static_cast<PageWalkerImpl*>(this)->read_pte(l4_index);
        
        constexpr uint64_t PRESENT_BIT = 0x1;
        if (entry & PRESENT_BIT) { 
            std::cout << "[Walker Base] Entry is valid (Present Bit set).\n";
            static_cast<PageWalkerImpl*>(this)->on_present_entry(entry);
        } else {
            std::cout << "[Walker Base] Page Fault! Entry present bit is 0.\n";
        }
    }
};


// ==========================================
// 3. Concrete Derived Hardware Implementation
// ==========================================
class X86_64Walker : public PageTableWalker<X86_64Walker> {
private:
    // Dummy structural mock mimicking a Level 4 Page Map Table (PML4)
    // Bit 0 = Present, Bits 12-51 = Dummy physical page framework address
    std::array<uint64_t, 512> mock_pml4_table;

public:
    X86_64Walker() {
        // Initialize our dummy physical framework mapping
        mock_pml4_table.fill(0x0); 
        
        // Populate entry 5 with a valid physical frame address + Present Bit (0x1)
        mock_pml4_table[5] = (0x00007FFFF7000ULL) | 0x1; 
        
        // Populate entry 42 with an unmapped descriptor (Present Bit missing)
        mock_pml4_table[42] = (0x00007FFFFF000ULL) | 0x0; 
    }

    // Fulfills the interface contract required by CRTP Base
    uint64_t read_pte(uint64_t idx) {
        if (idx >= mock_pml4_table.size()) {
            std::cout << "  [X86_64-HW] Index out of structural boundaries!\n";
            return 0;
        }
        uint64_t entry = mock_pml4_table[idx];
        std::cout << "  [X86_64-HW] Read raw PTE from PML4[" << idx << "]: 0x" 
                  << std::hex << entry << "\n";
        return entry;
    }

    // Fulfills the interface action required by CRTP Base
    void on_present_entry(uint64_t entry) {
        // Strip the low architecture tracking flags to extract the target address
        uint64_t next_table_phys_addr = entry & 0x000FFFFFFFFFF000ULL;
        std::cout << "  [X86_64-HW] Action: Navigating to next level physical base block: 0x" 
                  << std::hex << next_table_phys_addr << "\n";
    }
};


// ==========================================
// 4. Execution Routine
// ==========================================
int main() {
    X86_64Walker walker;

    // Test Case 1: An address that maps directly to L4 Index 5 (Present)
    // 5ULL << 39 = 0x0000028000000000
    uint64_t valid_address = 0x0000028000000000ULL;
    walker.walk_tables(valid_address);

    // Test Case 2: An address that maps directly to L4 Index 42 (Not Present)
    // 42ULL << 39 = 0x0000150000000000
    uint64_t invalid_address = 0x0000150000000000ULL;
    walker.walk_tables(invalid_address);

    return 0;
}

