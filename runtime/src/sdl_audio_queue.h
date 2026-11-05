#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nx::plat {

struct AudioPending {
    const uint8_t* data;
    size_t size, pos;
    void* ctx;
};

// The HLE caps its audio queue at 12 buffers; extra slots leave room for transitions between output paths.
class SdlAudioQueue {
public:
    static constexpr size_t kCapacity = 16;

    bool push_back(AudioPending pending) {
        if (size_ == kCapacity) return false;
        size_t tail = head_ + size_;
        if (tail >= kCapacity) tail -= kCapacity;
        entries_[tail] = pending;
        ++size_;
        return true;
    }

    AudioPending& front() { return entries_[head_]; }

    void pop_front() {
        entries_[head_] = {};
        head_ = next(head_);
        --size_;
    }

    bool empty() const { return size_ == 0; }
    size_t size() const { return size_; }

private:
    static size_t next(size_t index) { return index + 1 == kCapacity ? 0 : index + 1; }

    std::array<AudioPending, kCapacity> entries_{};
    size_t head_ = 0;
    size_t size_ = 0;
};

}  // namespace nx::plat
