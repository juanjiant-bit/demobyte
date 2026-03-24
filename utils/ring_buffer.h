#pragma once
// ring_buffer.h — buffer circular SPSC entre cores con barreras de compilador.
// Core1 produce (push), Core0 consume (pop).
#include <cstdint>
#include <cstring>

namespace ring_buffer_detail {
static inline void memory_barrier() {
#if defined(__GNUC__)
    __asm__ volatile("" ::: "memory");
#endif
}
}

template<typename T, uint8_t N>
class RingBuffer {
    static_assert((N & (N - 1)) == 0, "N debe ser potencia de 2");
public:
    bool push(const T& item) {
        const uint8_t write = write_;
        ring_buffer_detail::memory_barrier();
        const uint8_t read = read_;
        const uint8_t next = (uint8_t)((write + 1u) & (N - 1));
        if (next == read) return false;  // lleno
        buf_[write] = item;
        ring_buffer_detail::memory_barrier();
        write_ = next;
        ring_buffer_detail::memory_barrier();
        return true;
    }

    bool pop(T& out) {
        const uint8_t read = read_;
        ring_buffer_detail::memory_barrier();
        const uint8_t write = write_;
        if (read == write) return false;  // vacío
        ring_buffer_detail::memory_barrier();
        out = buf_[read];
        ring_buffer_detail::memory_barrier();
        read_ = (uint8_t)((read + 1u) & (N - 1));
        ring_buffer_detail::memory_barrier();
        return true;
    }

    bool empty() const {
        ring_buffer_detail::memory_barrier();
        const uint8_t read = read_;
        const uint8_t write = write_;
        ring_buffer_detail::memory_barrier();
        return read == write;
    }
    uint8_t size() const {
        ring_buffer_detail::memory_barrier();
        const uint8_t write = write_;
        const uint8_t read = read_;
        ring_buffer_detail::memory_barrier();
        return (uint8_t)((write - read) & (N - 1));
    }
    static constexpr uint8_t capacity() { return (uint8_t)(N - 1); }

private:
    T                buf_[N];
    volatile uint8_t write_ = 0;
    volatile uint8_t read_  = 0;
};
