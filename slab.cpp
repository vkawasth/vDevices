#include <iostream>
#include <bit>
#include <concepts>
#include <mutex>
#include <cassert>
#include <array>
#include <type_traits>

enum class MetaAllocStatus : uint8_t { 
    Success = 0x00, 
    OutOfMemory = 0x01, 
    InvalidAddress = 0x02 
};

// ============================================================================
// 1. TEMPLATE METAPROGRAMMING LAYOUT BUILDER
// ============================================================================

// Compile-time type constraint checking if memory requirements align perfectly
template <size_t RequiredBytes, size_t BlockSize>
concept PerfectSlabMatch = (RequiredBytes <= BlockSize) && (BlockSize % 8 == 0);

template <typename T, size_t Index>
struct [[nodiscard]] MetaAllocationResult {
    T* memory_ptr;
    size_t assigned_index;
    MetaAllocStatus status;
};

// Compile-Time Meta-Loop Unroller: Searches for a free bit at build time if needed,
// but here we use the modern C++20 hardware intrinsic to ensure O(1) state scanning.
template <size_t BlockSize, size_t TotalBlocks>
class MetaprogrammedSlabAllocator {
    static_assert(TotalBlocks <= 64, "Bitmapped allocations are capped at 64 blocks for single-register execution.");

public:
    MetaprogrammedSlabAllocator() noexcept 
        : allocation_bitmap_(0xFFFFFFFFFFFFFFFFULL) // 1 = Available, 0 = Occupied
    {
        // Zero-initialize our pre-allocated, cache-aligned stack arena.
        // Bypasses malloc, mmap, and all runtime OS page-size traps entirely!
        static_buffer_pool_.fill(0);
    }

    ~MetaprogrammedSlabAllocator() noexcept = default;
    
    MetaprogrammedSlabAllocator(const MetaprogrammedSlabAllocator&) = delete;
    MetaprogrammedSlabAllocator& operator=(const MetaprogrammedSlabAllocator&) = delete;

    // HIGH-FREQUENCY ALLOCATION: Completely handled via User-Space Compile-Time Offsets
    template <typename TensorType>
    [[nodiscard]] MetaAllocationResult<TensorType, 0> AllocateTensor() noexcept 
        requires PerfectSlabMatch<sizeof(TensorType), BlockSize> 
    {
        std::lock_guard<std::mutex> lock(allocator_mutex_);

        // FIX: To find the first 1-bit (Available) using countr_zero, we do NOT invert the bitmap.
        // We count trailing zeros of the RAW bitmap. If block 0 is free (bit 0 is 1), trailing zeros = 0.
        // If all blocks are occupied (bitmap is 0), countr_zero returns 64.
        uint32_t free_slot = static_cast<uint32_t>(std::countr_zero(allocation_bitmap_));

        if (free_slot >= TotalBlocks) [[unlikely]] {
            return {nullptr, 0, MetaAllocStatus::OutOfMemory};
        }

        // Flip the target bit from 1 (Available) to 0 (Occupied)
        allocation_bitmap_ &= ~(1ULL << free_slot);

        // Compile-time determined constant shift calculation applied to the flat byte array
        size_t byte_offset = static_cast<size_t>(free_slot) * BlockSize;
        TensorType* resolved_ptr = reinterpret_cast<TensorType*>(&static_buffer_pool_[byte_offset]);

        return {resolved_ptr, static_cast<size_t>(free_slot), MetaAllocStatus::Success};
    }

    // HIGH-FREQUENCY DEALLOCATION
    template <typename TensorType>
    [[nodiscard]] MetaAllocStatus FreeTensor(TensorType* ptr) noexcept {
        if (!ptr) [[unlikely]] return MetaAllocStatus::InvalidAddress;

        std::lock_guard<std::mutex> lock(allocator_mutex_);
        uint8_t* target = reinterpret_cast<uint8_t*>(ptr);
        uint8_t* base = static_buffer_pool_.data();

        // Bounds tracking verification
        if (target < base || target >= (base + (TotalBlocks * BlockSize))) [[unlikely]] {
            return MetaAllocStatus::InvalidAddress;
        }

        size_t byte_offset = static_cast<size_t>(target - base);
        if ((byte_offset % BlockSize) != 0) [[unlikely]] return MetaAllocStatus::InvalidAddress;

        uint32_t index = static_cast<uint32_t>(byte_offset / BlockSize);
        
        // Re-enable the availability bit via standard single-cycle bitwise OR
        allocation_bitmap_ |= (1ULL << index);
        return MetaAllocStatus::Success;
    }

    [[nodiscard]] uint64_t get_bitmap() const noexcept {
        std::lock_guard<std::mutex> lock(allocator_mutex_);
        return allocation_bitmap_;
    }

private:
    mutable std::mutex allocator_mutex_;
    uint64_t allocation_bitmap_;
    
    // The Ultimate Metaprogrammed Sandbox: Flat, cache-contiguous stack memory array.
    // Completely standalone from OS fragmentation issues or Apple/Intel page-size conflicts.
    alignas(64) std::array<uint8_t, TotalBlocks * BlockSize> static_buffer_pool_;
};

// ============================================================================
// 2. TESTING PIPELINE LOGIC
// ============================================================================

// Mocking dense neural network weight buffers to verify our type-constraints
struct alignas(8) WeightTensorAlpha {
    float weights[1024]; // 4096 Bytes
};

int main() {
    std::cout << "=== RUNNING METAPROGRAMMED O(1) SLAB ALLOCATOR ===\n\n";

    // Build-Time Configuration: 8192 Byte (8KB) static slabs, 32 total blocks capacity
    MetaprogrammedSlabAllocator<8192, 32> allocator;

    std::cout << "[Initial State] Free Allocation Bitmap = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    // Step 1: Allocate memory via compiler-checked concept validations
    std::cout << "[Allocation Phase] Extracting two metaprogrammed tensor slots...\n";
    auto res_a = allocator.AllocateTensor<WeightTensorAlpha>();
    auto res_b = allocator.AllocateTensor<WeightTensorAlpha>();

    if (res_a.status != MetaAllocStatus::Success || res_b.status != MetaAllocStatus::Success) {
        std::cerr << "[Fatal Failure] Allocator rejected request.\n";
        return -1;
    }

    std::cout << "  -> Tensor A Allocated on Block Index = " << std::dec << res_a.assigned_index 
              << " | Target Address = 0x" << std::hex << reinterpret_cast<uintptr_t>(res_a.memory_ptr) << "\n";
    std::cout << "  -> Tensor B Allocated on Block Index = " << std::dec << res_b.assigned_index 
              << " | Target Address = 0x" << std::hex << reinterpret_cast<uintptr_t>(res_b.memory_ptr) << "\n";

    // Verify bit modifications: Bits 0 and 1 flip to 0 -> Output ends in ...FFFC
    std::cout << "[Modified State] Current Allocation Bitmap = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    // Step 2: Release block 0 back to our user-space tracking registry
    std::cout << "[Deallocation Phase] Freeing Tensor A (Index 0) via bitfields...\n";
    MetaAllocStatus free_ok = allocator.FreeTensor(res_a.memory_ptr);
    assert(free_ok == MetaAllocStatus::Success);

    // Verify bit 0 returns to 1 -> Output ends in ...FFFD
    std::cout << "[Restored State] Final Allocation Bitmap   = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    assert((allocator.get_bitmap() & 0x01) == 1);
    std::cout << "=== METAPROGRAMMED VALIDATION GRACEFULLY PASSED ===\n";

    return 0;
}

