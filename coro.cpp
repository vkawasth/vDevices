#include <iostream>
#include <coroutine>
#include <concepts>
#include <cstdint>

// ============================================================================
// 1. THE LOW-LEVEL COROUTINE ARCHITECTURE PRIMITIVES
// ============================================================================

struct AsyncTask {
    // The compiler forces us to declare a matching promise_type inside the return object
    struct promise_type {
        AsyncTask get_return_object() noexcept {
            return AsyncTask{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        
        // Controls if the coroutine executes immediately or starts suspended
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        
        void unhandled_exception() { std::terminate(); }
        void return_void() noexcept {}
    };

    std::coroutine_handle<promise_type> handle;

    explicit AsyncTask(std::coroutine_handle<promise_type> h) noexcept : handle(h) {}
    ~AsyncTask() { if (handle) handle.destroy(); } // RAII ownership cleanup

    // Disable copy semantics to guarantee single-point ownership properties
    AsyncTask(const AsyncTask&) = delete;
    AsyncTask& operator=(const AsyncTask&) = delete;

    // Standard move semantics
    AsyncTask(AsyncTask&& other) noexcept : handle(other.handle) { other.handle = nullptr; }
    AsyncTask& operator=(AsyncTask&& other) noexcept {
        if (this != &other) {
            if (handle) handle.destroy();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }

    // Explicit execution hook used by the host scheduler loop
    bool Resume() noexcept {
        if (handle && !handle.done()) {
            handle.resume();
            return !handle.done(); // Returns true if there are more steps remaining
        }
        return false;
    }
};

// Custom hardware-level awaitable mapping an asynchronous network event
struct NetworkDataAwaiter {
    uint32_t channel_id;

    // await_ready: Returns true if data is already in cache, bypassing suspension
    bool await_ready() const noexcept { return false; }

    // await_suspend: Executed right as the coroutine pauses. 
    // Passes the compiler handle, allowing you to log it into an event loop or epoll mapping.
    void await_suspend(std::coroutine_handle<> h) const noexcept {
        std::cout << "  -> [Hardware Awaiter] Coroutine suspended on Channel " << channel_id 
                  << ". Registering handle with network card engine.\n";
    }

    // await_resume: Executed the moment the handle is woken up
    void await_resume() const noexcept {
        std::cout << "  -> [Hardware Awaiter] Resuming execution path. Inbound network frames ready.\n";
    }
};

// ============================================================================
// 2. THE HIGH-FREQUENCY DATA PIPELINE COROUTINE
// ============================================================================

AsyncTask ProcessVirtualizedGpuPipeline() {
    std::cout << "[Pipeline Step 1] Coroutine started. Fetching initial header configuration...\n";
    
    // Suspend and wait for Channel 1 data frames
    co_await NetworkDataAwaiter{ .channel_id = 1 };

    std::cout << "[Pipeline Step 2] Processing matrix payloads. Validating shadow pointers...\n";
    
    // Suspend and wait for Channel 2 data frames
    co_await NetworkDataAwaiter{ .channel_id = 2 };

    std::cout << "[Pipeline Step 3] Executing kernel launch. Coroutine task complete.\n";
}

int main() {
    std::cout << "=== RUNNING NATIVE C++20 SYSTEM COROUTINE ===\n\n";

    // Instantiating the coroutine does NOT execute the code body due to initial_suspend
    AsyncTask job = ProcessVirtualizedGpuPipeline();

    std::cout << "[Host Event Loop] Advancing task to Step 1...\n";
    job.Resume();

    std::cout << "\n[Host Event Loop] Simulated Network Interrupt triggered on Channel 1. Resuming pipeline...\n";
    job.Resume();

    std::cout << "\n[Host Event Loop] Simulated Network Interrupt triggered on Channel 2. Finalizing pipeline...\n";
    job.Resume();

    std::cout << "\n=== NATIVE COROUTINE EXECUTION GRACEFULLY COMPLETE ===\n";
    return 0;
}

