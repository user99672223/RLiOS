// Lock-free single-producer / single-consumer triple buffer with latest-wins semantics.
// Producer never blocks on the consumer and vice versa (CLAUDE.md section 4: Sim never waits
// on Render). T must be trivially copyable; snapshots are plain structs.
#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

namespace rlios::sim {

template <class T> class TripleBuffer {
    static_assert(std::is_trivially_copyable_v<T>, "snapshots must be plain data");

  public:
    // Producer: buffer to fill, then call publish().
    T& back() { return buf_[back_]; }
    void publish() {
        const uint32_t old = middle_.exchange(back_ | kDirty, std::memory_order_acq_rel);
        back_ = old & 3u;
    }

    // Consumer: returns true if a newer snapshot was swapped in. front() is stable until the
    // next acquire().
    bool acquire() {
        if ((middle_.load(std::memory_order_relaxed) & kDirty) == 0)
            return false;
        const uint32_t old = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = old & 3u;
        return true;
    }
    const T& front() const { return buf_[front_]; }

  private:
    static constexpr uint32_t kDirty = 4u;
    T buf_[3]{};
    uint32_t back_ = 0;  // producer-private
    uint32_t front_ = 2; // consumer-private
    std::atomic<uint32_t> middle_{1};
};

} // namespace rlios::sim
