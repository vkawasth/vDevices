#include <iostream>
#include <vector>
#include <cstdint>
#include <bit>
#include <concepts>
#include <mutex>
#include <cassert>
#include <cstdlib> 
#include <cstring> // Explicitly links std::memset

// ============================================================================
// 1. ARCHITECTURAL CONFIGURATIONS
// ============================================================================
constexpr size_t BLOCK_SIZE_BYTES = 2ULL * 1024ULL * 1024ULL; // Each block is 2MB (Slab Size)
constexpr size_t TOTAL_SLAB_BLOCKS = 64;                       // 64 blocks = 128MB total managed pool
constexpr size_t ARENA_ALIGNMENT    = 2ULL * 1024ULL * 1024ULL; // Align full arena to 2MB boundaries for Hugepage mapping

enum class AllocatorStatus : uint8_t {
    Success = 0x00,
    OutOfMemory = 0x01,
    InvalidFreeAddress = 0x02
};

// Custom nodiscard payload containing our allocation token properties
struct [[nodiscard]] AllocationResult {
    void* physical_cpu_ptr{nullptr};
    uint32_t block_index{0};
    AllocatorStatus status{AllocatorStatus::Success};
};

// ============================================================================
// 2. HIGH-FREQUENCY O(1) BITMAPPED CPU SLAB ALLOCATOR
// ============================================================================
class CpuSlabAllocator {
public:
    // RAII Pre-Allocation: Reserve the full page-aligned backing pool footprint once at startup
    CpuSlabAllocator() noexcept 
        : base_cpu_pool_ptr_(nullptr), 
          allocation_bitmap_(0xFFFFFFFFFFFFFFFFULL) // All 64 bits set to 1 (1 = Available, 0 = Occupied)
    {
        size_t total_arena_bytes = TOTAL_SLAB_BLOCKS * BLOCK_SIZE_BYTES;
        
        // std::aligned_alloc guarantees that our 128MB block starts precisely on a 2MB page boundary.
        base_cpu_pool_ptr_ = std::aligned_alloc(ARENA_ALIGNMENT, total_arena_bytes);
        
        if (!base_cpu_pool_ptr_) [[unlikely]] {
            std::cerr << "[Hardware Fault] Critical initialization failure: OS failed to allocate aligned arena.\n";
        } else {
            // Hot-warm the memory footprint to eliminate runtime soft page faults
            std::memset(base_cpu_pool_ptr_, 0, total_arena_bytes);
        }
    }

    ~CpuSlabAllocator() noexcept {
        if (base_cpu_pool_ptr_) {
            std::free(base_cpu_pool_ptr_); // Safely tear down memory block back to the system pool
        }
    }

    // Eliminate copy operations to maintain unified single-point resource tracking boundaries
    CpuSlabAllocator(const CpuSlabAllocator&) = delete;
    CpuSlabAllocator& operator=(const CpuSlabAllocator&) = delete;

    // HIGH-FREQUENCY ENTRY: O(1) bit-scanning allocations bypassing all OS traps
    [[nodiscard]] AllocationResult AllocateTensorBlock() noexcept {
        if (!base_cpu_pool_ptr_) [[unlikely]] {
            return {nullptr, 0, AllocatorStatus::OutOfMemory};
        }

        std::lock_guard<std::mutex> lock(allocator_mutex_);

        // Step 1: Scan for the first available block using C++20 bit counting
        uint32_t free_index = static_cast<uint32_t>(std::countr_zero(~allocation_bitmap_));

        if (free_index >= TOTAL_SLAB_BLOCKS) [[unlikely]] {
            return {nullptr, 0, AllocatorStatus::OutOfMemory}; // Entire 128MB pool is full
        }

        // Step 2: Flip the target bit from 1 (Available) to 0 (Occupied) inside our tracking register
        allocation_bitmap_ &= ~(1ULL << free_index);

        // Step 3: Single-cycle relative address pointer scaling calculation
        uint8_t* byte_ptr = reinterpret_cast<uint8_t*>(base_cpu_pool_ptr_);
        void* allocated_block_ptr = byte_ptr + (static_cast<size_t>(free_index) * BLOCK_SIZE_BYTES);

        return {allocated_block_ptr, free_index, AllocatorStatus::Success};
    }

    // HIGH-FREQUENCY DEALLOCATION: O(1) bitwise release path
    [[nodiscard]] AllocatorStatus FreeTensorBlock(void* block_ptr) noexcept {
        if (!block_ptr || !base_cpu_pool_ptr_) [[unlikely]] {
            return AllocatorStatus::InvalidFreeAddress;
        }

        std::lock_guard<std::mutex> lock(allocator_mutex_);

        // Step 1: Verify the memory coordinate sits inside our managed arena boundary
        uint8_t* target_ptr = reinterpret_cast<uint8_t*>(block_ptr);
        uint8_t* base_ptr = reinterpret_cast<uint8_t*>(base_cpu_pool_ptr_);

        if (target_ptr < base_ptr || target_ptr >= (base_ptr + (TOTAL_SLAB_BLOCKS * BLOCK_SIZE_BYTES))) [[unlikely]] {
            return AllocatorStatus::InvalidFreeAddress;
        }

        // Step 2: Calculate the exact target block index via simple pointer arithmetic division
        size_t byte_offset = static_cast<size_t>(target_ptr - base_ptr);
        
        // Assert perfect block alignment to stop corrupted memory sweeps from parsing unaligned offsets
        if ((byte_offset % BLOCK_SIZE_BYTES) != 0) [[unlikely]] {
            return AllocatorStatus::InvalidFreeAddress;
        }

        uint32_t block_index = static_cast<uint32_t>(byte_offset / BLOCK_SIZE_BYTES);

        // Step 3: Flip the target bit back to 1 (Available) via an immediate bitwise OR
        allocation_bitmap_ |= (1ULL << block_index);

        return AllocatorStatus::Success;
    }

    // FIX 1: Marked the mutex as mutable below so it can be safely locked inside this const method
    [[nodiscard]] uint64_t get_bitmap() const noexcept {
        std::lock_guard<std::mutex> lock(allocator_mutex_);
        return allocation_bitmap_;
    }

private:
    // mutable permits modification of the lock status inside const diagnostic hooks
    mutable std::mutex allocator_mutex_; 
    void* base_cpu_pool_ptr_;
    uint64_t allocation_bitmap_; // Each bit manages exactly one 2MB block segment
};

// ============================================================================
// 3. TESTING PIPELINE LOGIC
// ============================================================================
int main() {
    std::cout << "=== RUNNING HIGH-PERFORMANCE CPU SLAB ALLOCATOR ===\n\n";

    CpuSlabAllocator allocator;

    // Verify initial bitmap contains all bits available (0xFFFFFFFFFFFFFFFF)
    std::cout << "[Initial State] Free Allocation Bitmap = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    // Step 1: Simulate rapid allocation requests for machine learning weights
    std::cout << "[Allocation Phase] Extracting two 2MB tensor blocks from host arena...\n";
    AllocationResult tensor_a = allocator.AllocateTensorBlock();
    AllocationResult tensor_b = allocator.AllocateTensorBlock();

    if (tensor_a.status != AllocatorStatus::Success || tensor_b.status != AllocatorStatus::Success) {
        std::cerr << "Memory allocation crash. Check system memory limits.\n";
        return -1;
    }

    std::cout << "  -> Tensor A Assigned Block Index = " << std::dec << tensor_a.block_index 
              << " | Host Memory Address = 0x" << std::hex << reinterpret_cast<uintptr_t>(tensor_a.physical_cpu_ptr) << "\n";
    // FIX 2: Corrected the typo from physical_gpu_ptr to physical_cpu_ptr
    std::cout << "  -> Tensor B Assigned Block Index = " << std::dec << tensor_b.block_index 
              << " | Host Memory Address = 0x" << std::hex << reinterpret_cast<uintptr_t>(tensor_b.physical_cpu_ptr) << "\n";

    // Verify the bitmap registers the modification (Lower 2 bits should drop to 0 -> ...FFFC)
    std::cout << "[Modified State] Current Allocation Bitmap = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    // Step 2: Release the first block back to our internal pool
    std::cout << "[Deallocation Phase] Freeing Tensor A (Block 0) to local reuse cache...\n";
    AllocatorStatus free_status = allocator.FreeTensorBlock(tensor_a.physical_cpu_ptr);
    assert(free_status == AllocatorStatus::Success);

    // Verify bit 0 pops back up to 1 (Inverted from FFFC back to FFFD)
    std::cout << "[Restored State] Final Allocation Bitmap   = 0x" << std::hex << allocator.get_bitmap() << "\n\n";

    assert((allocator.get_bitmap() & 0x01) == 1); // Verify block 0 is marked available again
    std::cout << "=== ALL CPU SLAB ALLOCATOR VALIDATIONS GRACEFULLY PASSED ===\n";

    return 0;
}

