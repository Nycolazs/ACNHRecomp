// The pack writers' output file and progress (pak.h).
#include "pak.h"

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <io.h>  // _commit, _fileno
#else
#include <unistd.h>  // fsync
#endif

#include "io_util.h"

namespace nx::prepare::detail {

namespace {

std::string os_error(const char* what, const std::string& path) {
    return std::string(what) + " " + path + ": " + std::strerror(errno);
}

}  // namespace

PakOut::PakOut(std::string out) : out_(std::move(out)), part_(out_ + ".part") {}

PakOut::~PakOut() {
    if (f_) std::fclose(f_);
    if (!done_) {
        std::error_code ec;
        fs::remove(u8(part_), ec);
    }
}

bool PakOut::open(std::string& error) {
    f_ = open_file(u8(part_), "wb");
    if (!f_) return error = os_error("cannot create", part_), false;
    std::setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    return true;
}

bool PakOut::write(const void* data, size_t n, std::string& error) {
    if (n && std::fwrite(data, 1, n, f_) != n) return error = os_error("cannot write", part_), false;
    written_ += n;
    return true;
}

bool PakOut::zeros(uint64_t n, std::string& error) {
    static const uint8_t z[4096] = {};
    while (n) {
        const size_t k = n < sizeof z ? size_t(n) : sizeof z;
        if (!write(z, k, error)) return false;
        n -= k;
    }
    return true;
}

bool PakOut::finish(std::string& error) {
    bool ok = std::fflush(f_) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f_)) == 0;
#else
    ok = ok && fsync(fileno(f_)) == 0;
#endif
    if (!ok) return error = os_error("cannot write", part_), false;
    const bool closed = std::fclose(f_) == 0;
    f_ = nullptr;
    if (!closed) return error = os_error("cannot write", part_), false;
    std::error_code ec;
    fs::rename(u8(part_), u8(out_), ec);  // replaces an older pack (MoveFileEx with REPLACE_EXISTING on Windows)
    if (ec) return error = "cannot rename " + part_ + " to " + out_ + ": " + ec.message(), false;
    done_ = true;
    return true;
}

bool copy_file_into(const std::string& src, uint64_t size, PakOut& out, const std::atomic<bool>* cancel,
                    const std::function<void(uint64_t)>& tick, bool& cancelled, std::string& error) {
    FILE* f = open_file(u8(src), "rb");
    if (!f) return error = os_error("cannot open", src), false;
    std::unique_ptr<FILE, int (*)(FILE*)> hold(f, std::fclose);
    static thread_local std::vector<uint8_t> buf(1 << 20);
    uint64_t left = size;
    while (left) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return cancelled = true, false;
        const size_t want = left < buf.size() ? size_t(left) : buf.size();
        const size_t n = std::fread(buf.data(), 1, want, f);
        if (n != want) {
            if (std::ferror(f)) return error = os_error("cannot read", src), false;
            return error = src + ": shorter than when the folder was listed (it changed while being packed)", false;
        }
        if (!out.write(buf.data(), n, error)) return false;
        left -= n;
        if (tick) tick(n);
    }
    if (std::fgetc(f) != EOF) return error = src + ": longer than when the folder was listed (it changed while being packed)", false;
    return true;
}

ProgressPacer::ProgressPacer(const PakOptions& opt, uint64_t files_total, uint64_t bytes_total) : opt_(opt) {
    p_.files_total = files_total;
    p_.bytes_total = bytes_total;
}

void ProgressPacer::update(uint64_t files_done, uint64_t bytes_done, bool last) {
    if (!opt_.progress) return;
    if (!last && files_done - last_files_ < 256 && bytes_done - last_bytes_ < (16u << 20)) return;
    p_.files_done = last_files_ = files_done;
    p_.bytes_done = last_bytes_ = bytes_done;
    opt_.progress(p_);
}

}  // namespace nx::prepare::detail
