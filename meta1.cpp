#include <iostream>
#include <cstdint>
#include <concepts>

// ============================================================================
// 1. Hardware Capability Policies
// ============================================================================
struct LegacyHardwarePolicy { 
    static constexpr bool HasNoExecute = false; 
};

struct ModernHardwarePolicy { 
    static constexpr bool HasNoExecute = true; 
};


// ============================================================================
// 2. C++20 Concepts (Compile-Time Interface Constraints)
// ============================================================================
template <typename T>
concept AdvancedSecurityHardware = T::HasNoExecute == true;

template <typename T>
concept BasicSecurityHardware = T::HasNoExecute == false;


// ============================================================================
// 3. Security Engine Utilizing C++20 Constraints
// ============================================================================
template <typename Policy>
class SecurityEngine {
public:
    // This method compiles ONLY if the policy satisfies the AdvancedSecurityHardware concept
    void verify_execution_allowed(uint64_t pte) requires AdvancedSecurityHardware<Policy> {
        std::cout << "[Security Engine] Evaluating modern 64-bit NX (No-Execute) bit protection.\n";
        
        constexpr uint64_t NX_BIT_MASK = 1ULL << 63; // Bit 63 is the NX bit in x86_64
        if (pte & NX_BIT_MASK) {
            std::cout << "  [!] SECURITY VIOLATION: Execution attempted on a non-executable page!\n";
        } else {
            std::cout << "  [SUCCESS] Code execution allowed on this page region.\n";
        }
    }

    // This method compiles ONLY if the policy satisfies the BasicSecurityHardware concept
    void verify_execution_allowed(uint64_t pte) requires BasicSecurityHardware<Policy> {
        std::cout << "[Security Engine] Legacy platform detected. NX checks safely bypassed at zero runtime cost.\n";
    }
};


// ============================================================================
// 4. Execution Routine
// ============================================================================
int main() {
    // Standard mock page table entries (PTEs)
    uint64_t safe_pte = 0x00007FFFF7000ULL | 0x1; // Just physical address + present flag
    uint64_t malicious_pte = (1ULL << 63) | 0x00007FFFF7000ULL | 0x1; // NX Bit set!

    std::cout << "=== Test 1: Simulating Modern Hardware Context ===\n";
    SecurityEngine<ModernHardwarePolicy> modern_engine;
    
    // Will compile down to execute the strict bitmask checks
    modern_engine.verify_execution_allowed(safe_pte);
    modern_engine.verify_execution_allowed(malicious_pte);

    std::cout << "\n=== Test 2: Simulating Legacy Hardware Context ===\n";
    SecurityEngine<LegacyHardwarePolicy> legacy_engine;
    
    // The exact same function call targets an entirely different, zero-cost code overload
    legacy_engine.verify_execution_allowed(safe_pte);
    legacy_engine.verify_execution_allowed(malicious_pte);

    return 0;
}

