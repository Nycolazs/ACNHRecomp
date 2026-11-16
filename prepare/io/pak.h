// What the pack writers (romfs_pak.h, spirv_pak.h) share: progress, cancelling, the result, and the output file,
// written as <out>.part and renamed to <out> once complete (a pack cut short by a crash or a full disk is never
// taken for a whole one: the runtime trusts the index it reads).
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>

namespace nx::prepare {

struct PakProgress {
    uint64_t files_done = 0, files_total = 0;
    uint64_t bytes_done = 0, bytes_total = 0;  // of the pack being written
};

struct PakOptions {
    // Called from the writing thread every few MB and at the end; keep it cheap.
    std::function<void(const PakProgress&)> progress;
    const std::atomic<bool>* cancel = nullptr;  // set it to stop: the result is then `cancelled`, and no file is left
};

struct PakResult {
    bool ok = false;
    bool cancelled = false;
    std::string error;  // English, for logs: what failed and on which file
    uint64_t files = 0;
    uint64_t bytes = 0;  // the pack's size
};

namespace detail {

// The pack being written: <out>.part, buffered; finish() flushes it to the disk and renames it to <out>, and a
// PakOut destroyed before that removes the .part.
class PakOut {
public:
    explicit PakOut(std::string out);
    ~PakOut();
    PakOut(const PakOut&) = delete;
    PakOut& operator=(const PakOut&) = delete;

    bool open(std::string& error);
    bool write(const void* data, size_t n, std::string& error);
    bool zeros(uint64_t n, std::string& error);
    bool finish(std::string& error);  // flush, sync, close, rename
    uint64_t written() const { return written_; }

private:
    std::string out_, part_;
    FILE* f_ = nullptr;
    bool done_ = false;
    uint64_t written_ = 0;
};

// Copies `size` bytes of the file `src` into `out` in 1 MiB pieces, calling `tick(bytes)` after each; false (and why)
// when the file cannot be read, is not `size` bytes long any more, or `cancel` was set.
bool copy_file_into(const std::string& src, uint64_t size, PakOut& out, const std::atomic<bool>* cancel,
                    const std::function<void(uint64_t)>& tick, bool& cancelled, std::string& error);

// Calls `progress` at most every 16 MiB or 256 files (and always when `last`).
class ProgressPacer {
public:
    ProgressPacer(const PakOptions& opt, uint64_t files_total, uint64_t bytes_total);
    void update(uint64_t files_done, uint64_t bytes_done, bool last = false);

private:
    const PakOptions& opt_;
    PakProgress p_;
    uint64_t last_files_ = 0, last_bytes_ = 0;
};

}  // namespace detail

}  // namespace nx::prepare
