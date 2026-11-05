#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace nx {

// Fixed 200 ms stereo sample queue; push() drops the oldest sample when full. The caller serializes access.
class AudioMixRing {
public:
    static constexpr size_t kCapacity = 48000 * 2 / 5;

    void push(int16_t sample) {
        if (size_ == kCapacity) {
            head_ = next(head_);
            --size_;
        }
        size_t tail = head_ + size_;
        if (tail >= kCapacity) tail -= kCapacity;
        samples_[tail] = sample;
        ++size_;
    }

    bool pop_front(int16_t& sample) {
        if (size_ == 0) return false;
        sample = samples_[head_];
        head_ = next(head_);
        --size_;
        return true;
    }

    void clear() {
        head_ = 0;
        size_ = 0;
    }

    size_t size() const { return size_; }

private:
    static size_t next(size_t index) { return index + 1 == kCapacity ? 0 : index + 1; }

    std::array<int16_t, kCapacity> samples_{};
    size_t head_ = 0;
    size_t size_ = 0;
};

}  // namespace nx
