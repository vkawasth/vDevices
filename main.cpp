#include <iostream>
#include <vector>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>
#include <cassert>

// ============================================================================
// 1. CACHE-ALIGNED TRACKING TYPES & STRUCTS
// ============================================================================
enum class ApiCommandType : uint8_t {
    CudaMalloc = 0x01,
    CudaMemcpy = 0x02,
    CudaLaunch = 0x03
};

#pragma pack(push, 1)
struct CommandPacket {
    ApiCommandType command_id;
    uint32_t payload_bytes;
    uint64_t transaction_token;
};
#pragma pack(pop)

// Enforce strict size check for bitfield transparency
static_assert(sizeof(CommandPacket) == 13, "Packet must map precisely to 13 bytes.");

// ============================================================================
// 2. HIGH-FREQUENCY LOCK-FREE RING BUFFER MPSC CONTROLLER
// ============================================================================
template <size_t Capacity>
class MpscLockFreeRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a strict power of 2 for fast bitmask operations.");

public:
    MpscLockFreeRingBuffer() noexcept : head_(0), tail_(0) {
        // Initialize state slots tracking empty entries
        for (size_t i = 0; i < Capacity; ++i) {
            slot_populated_[i].store(false, std::memory_order_relaxed);
        }
    }

    ~MpscLockFreeRingBuffer() noexcept = default;

    // Enforce Non-Copyable / Non-Moveable physical hardware interface properties
    MpscLockFreeRingBuffer(const MpscLockFreeRingBuffer&) = delete;
    MpscLockFreeRingBuffer& operator=(const MpscLockFreeRingBuffer&) = delete;

    // MULTI-PRODUCER SIDE: Multiple Guest VM threads invoke this concurrently
    [[nodiscard]] bool PushCommand(const CommandPacket& packet) noexcept {
        uint64_t current_head = head_.load(std::memory_order_relaxed);

        while (true) {
            uint64_t current_tail = tail_.load(std::memory_order_acquire);
            
            // Check for structural buffer overflow condition
            if ((current_head - current_tail) >= Capacity) [[unlikely]] {
                return false; 
            }

            // Compare-And-Swap (CAS) loop: Atomically reserve a single ticket slot 
            // across competing multi-threaded guest worker submissions
            if (head_.compare_exchange_weak(current_head, current_head + 1, 
                                            std::memory_order_relaxed, 
                                            std::memory_order_relaxed)) [[likely]] {
                break; // Ticket successfully claimed!
            }
            // If CAS failed because another thread updated head_, current_head is automatically updated
        }

        // Single-cycle power-of-two bitmask lookup replaces slow division modulo rules (%)
        size_t slot_index = static_cast<size_t>(current_head & (Capacity - 1));

        // Write directly to our pre-allocated storage segment
        ring_slots_[slot_index] = packet;

        // Establish an RELEASE fence. This forces the CPU pipeline to dump the packet array
        // mutations to memory BEFORE the sequence marker flag shifts state.
        std::atomic_thread_fence(std::memory_order_release);

        // Signal to the consumer that this slot is now fully written and ready
        slot_populated_[slot_index].store(true, std::memory_order_relaxed);
        return true;
    }

    // SINGLE-CONSUMER SIDE: Executed exclusively by the single Host Driver thread
    [[nodiscard]] bool PopCommand(CommandPacket& out_packet) noexcept {
        uint64_t current_tail = tail_.load(std::memory_order_relaxed);
        uint64_t current_head = head_.load(std::memory_order_acquire);

        // Check if queue is completely empty
        if (current_tail == current_head) {
            return false;
        }

        size_t slot_index = static_cast<size_t>(current_tail & (Capacity - 1));

        // Read readiness marker. If false, a producer claimed the ticket but hasn't finished copying bytes
        if (!slot_populated_[slot_index].load(std::memory_order_relaxed)) {
            return false; // Spin-wait cycle optimization fallback
        }

        // Establish an ACQUIRE fence to guarantee memory sync with producer's release point
        std::atomic_thread_fence(std::memory_order_acquire);

        // Pull the completely written packet frame out
        out_packet = ring_slots_[slot_index];

        // Free the slot allocation tracker flag
        slot_populated_[slot_index].store(false, std::memory_order_relaxed);

        // Advance tail step. Relaxed is safe because only ONE thread ever manipulates tail
        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

private:
    // alignas(64) guarantees that adjacent control metrics sit on unique CPU cache lines,
    // completely neutralizing performance-crushing False Sharing across cores.
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    
    alignas(64) CommandPacket ring_slots_[Capacity];
    alignas(64) std::atomic<bool> slot_populated_[Capacity];
};

// ============================================================================
// 3. TESTING SUITE PIPELINE SIMULATOR
// ============================================================================
int main() {
    std::cout << "=== RUNNING HIGH-PERFORMANCE MPSC RING BUFFER TEST ===\n\n";

    // Allocate an explicit power-of-two capacity ring engine
    MpscLockFreeRingBuffer<1024> buffer;

    // Simulate 3 concurrent Guest VM threads submitting workloads
    std::vector<std::thread> guest_threads;
    for (uint32_t t = 0; t < 3; ++t) {
        guest_threads.emplace_back([&buffer, t]() {
            CommandPacket packet{ ApiCommandType::CudaLaunch, 512, (1000ULL + t) };
            bool ok = buffer.PushCommand(packet);
            if (ok) {
                std::cout << "[Guest Thread " << t << "] Pushed CUDA Command. Token = " << (1000 + t) << "\n";
            }
        });
    }

    // Join producer threads to verify synchronization states
    for (auto& th : guest_threads) {
        th.join();
    }

    std::cout << "\n[Host Server Processing Cycle Engaged]\n";

    // Host Driver thread reads data segments out sequentially until empty
    CommandPacket host_dispatch_packet{};
    size_t processed_count = 0;
    
    while (buffer.PopCommand(host_dispatch_packet)) {
        processed_count++;
        std::cout << "  -> Host Popped & Executed Task " << processed_count 
                  << " | Token Target = " << std::dec << host_dispatch_packet.transaction_token 
                  << " | Cmd ID = 0x" << std::hex << static_cast<int>(host_dispatch_packet.command_id) << "\n";
    }

    assert(processed_count == 3);
    std::cout << "\n=== LOCK-FREE QUEUE VERIFICATION PASSED ===\n";

    return 0;
}
