#include <iostream>
#include <vector>
#include <atomic>
#include <cstdint>
#include <concepts>
#include <type_traits>
#include <thread>
#include <cassert>

enum class ApiCommandType : uint8_t { CudaMalloc = 0x01, CudaMemcpy = 0x02, CudaLaunch = 0x03 };

#pragma pack(push, 1)
struct CommandPacket {
    ApiCommandType command_id;
    uint32_t payload_bytes;
    uint64_t transaction_token;
};
#pragma pack(pop)

// Compile-Time Meta-Check to verify packet alignment guarantees
static_assert(sizeof(CommandPacket) == 13, "Network packet size must map exactly to 13 bytes.");

template <size_t Capacity>
class MpscLockFreeQueue {
    // Template Metaprogramming Check: Force capacity bounds to power-of-two shapes
    static_assert((Capacity & (Capacity - 1)) == 0, "Buffer capacity must be a strict power of 2.");

public:
    MpscLockFreeQueue() noexcept : head_(0), tail_(0) {
        for (size_t i = 0; i < Capacity; ++i) {
            slot_populated_[i].store(false, std::memory_order_relaxed);
        }
    }

    ~MpscLockFreeQueue() noexcept = default;
    MpscLockFreeQueue(const MpscLockFreeQueue&) = delete;
    MpscLockFreeQueue& operator=(const MpscLockFreeQueue&) = delete;

    // MULTI-PRODUCER ENTRY PATH: Concurrent Guest VM Threads push requests
    template <typename T>
    [[nodiscard]] bool PushCommand(T&& packet) noexcept requires std::is_trivially_copyable_v<std::decay_t<T>> {
        uint64_t current_head = head_.load(std::memory_order_relaxed);

        while (true) {
            uint64_t current_tail = tail_.load(std::memory_order_acquire);
            if ((current_head - current_tail) >= Capacity) [[unlikely]] {
                return false; // Queue buffer overflow
            }

            // CAS Ticket loop: Atomically reserve a single sequence slot across threads
            if (head_.compare_exchange_weak(current_head, current_head + 1, 
                                            std::memory_order_relaxed, 
                                            std::memory_order_relaxed)) [[likely]] {
                break;
            }
        }

        // Single-cycle power-of-two bitmask lookup replaces slow division modulo (%)
        size_t slot_index = static_cast<size_t>(current_head & (Capacity - 1));
        ring_slots_[slot_index] = std::forward<T>(packet);

        // Hardware fence forces payload writes to RAM before the completion flag flips
        std::atomic_thread_fence(std::memory_order_release);
        slot_populated_[slot_index].store(true, std::memory_order_relaxed);
        return true;
    }

    // SINGLE-CONSUMER PATH: Invoked exclusively by the single Host Driver Scheduler thread
    [[nodiscard]] bool PopCommand(CommandPacket& out_packet) noexcept {
        uint64_t current_tail = tail_.load(std::memory_order_relaxed);
        uint64_t current_head = head_.load(std::memory_order_acquire);

        if (current_tail == current_head) return false; // Queue is completely empty

        size_t slot_index = static_cast<size_t>(current_tail & (Capacity - 1));
        if (!slot_populated_[slot_index].load(std::memory_order_relaxed)) {
            return false; // Producer claimed ticket but is still copying payload bytes
        }

        // Acquire fence forces the CPU cache lines to sync with the producer's write point
        std::atomic_thread_fence(std::memory_order_acquire);
        out_packet = ring_slots_[slot_index];
        slot_populated_[slot_index].store(false, std::memory_order_relaxed);

        tail_.store(current_tail + 1, std::memory_order_release);
        return true;
    }

private:
    // alignas(64) isolates adjacent tracking fields onto unique hardware cache lines, preventing False Sharing
    alignas(64) std::atomic<uint64_t> head_;
    alignas(64) std::atomic<uint64_t> tail_;
    alignas(64) CommandPacket ring_slots_[Capacity];
    alignas(64) std::atomic<bool> slot_populated_[Capacity];
};

int main() {
    std::cout << "=== RUNNING HIGH-PERFORMANCE MPSC RING BUFFER ===\n\n";
    MpscLockFreeQueue<1024> queue;

    std::vector<std::thread> producers;
    for (uint32_t i = 0; i < 3; ++i) {
        producers.emplace_back([&queue, i]() {
            CommandPacket cmd{ ApiCommandType::CudaLaunch, 256, (5000ULL + i) };
            if (queue.PushCommand(cmd)) {
                std::cout << "[Guest Thread " << i << "] Pushed Packet. Token: " << (5000 + i) << "\n";
            }
        });
    }

    for (auto& t : producers) t.join();

    std::cout << "\n[Host Scheduler Engaged]\n";
    CommandPacket out_cmd{};
    size_t count = 0;
    while (queue.PopCommand(out_cmd)) {
        count++;
        std::cout << "  -> Popped Task " << count << " | Token: " << out_cmd.transaction_token << "\n";
    }

    assert(count == 3);
    return 0;
}

