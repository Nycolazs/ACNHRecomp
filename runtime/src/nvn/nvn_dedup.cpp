// Per-submit upload reuse (docs/nvn/DEBUG.md #30). A draw's buffers are copied from guest memory into the upload
// ring when it is replayed; the same camera block, scene constants and vertex buffers were copied again for every
// draw that bound them (~130 KB per draw, 50-70 MB per frame). Within one submit guest memory only changes through
// inline uniform updates (UpdateUniformBuffer, which invalidates what it overlaps) and compute results (ranges a
// dispatch writes are read from their written copy, nvn_written.cpp, and invalidated here): a copy made once can
// serve every later draw of the submit that binds the same bytes.
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <memory_resource>

#include "nvn_draw_state.h"

namespace nx::nvn::replay {

namespace {

struct Entry {
    uint64_t size = 0, padded = 0;
    Upload upload;
};
// By host address; several sizes of one address are rare, so the latest one is kept. Its nodes come from an arena
// emptied with it at each submit (~2,000 inserts a frame were as many mallocs and frees: ~3% of the replay thread on a
// Snapdragon 865).
// Per replaying thread (docs/PARALLEL_REPLAY.md): a worker shares copies within its own segments.
struct Copies {
    std::pmr::monotonic_buffer_resource arena{1 << 20};
    std::pmr::map<const uint8_t*, Entry> map{&arena};
};
PerThread<Copies> g_cp;
PerThread<uint64_t> g_saved, g_copied, g_data, g_copies_n, g_big_pads;  // g_data: bytes of guest data among g_copied

// In front of the map (a tree walk per bound buffer of every draw, and most are hits): a slot per address hash with a
// copy of that address's entry in (*g_cp).map. A slot of the current generation always equals the map's entry: both are
// written together, dedup_invalidate clears the slot of each entry it erases, dedup_reset starts a new generation.
struct Front {
    const uint8_t* p = nullptr;
    uint64_t generation = 0;
    Entry entry;
};
PerThread<TlsArray<Front, 1024>> g_front;
PerThread<uint64_t> g_generation{1};
Front& front_of(const uint8_t* p) { return (*g_front)[(uintptr_t(p) >> 4 ^ uintptr_t(p) >> 14) & 1023]; }

}  // namespace

PerThread<uint64_t> g_upload_epoch{1};

Upload dedup_upload(const uint8_t* p, uint64_t size, uint64_t padded, uint64_t align) {
    write_back_before_read(p, size, "dedup");
    if (!p || !size) {
        Upload u = upload_alloc(padded, align);
        std::memset(u.ptr, 0, padded);
        return u;
    }
    static const bool off = std::getenv("NX_NO_DEDUP") != nullptr;  // bisecting: a fresh copy every time
    Front& f = front_of(p);
    const Entry* e = nullptr;
    if (!off && f.generation == (*g_generation) && f.p == p) {
        e = &f.entry;  // the map's entry for p
    } else if (auto it = off ? (*g_cp).map.end() : (*g_cp).map.find(p); it != (*g_cp).map.end()) {
        f = {p, (*g_generation), it->second};
        e = &f.entry;
    }
    if (e && e->size == size && e->padded == padded && e->upload.offset % align == 0) {
        (*g_saved) += padded;
        return e->upload;
    }
    Upload u = upload_alloc(padded, align);
    std::memcpy(u.ptr, p, size);
    if (padded > size) std::memset(u.ptr + size, 0, padded - size);
    (*g_cp).map[p] = {size, padded, u};
    f = {p, (*g_generation), {size, padded, u}};
    (*g_copied) += padded, (*g_data) += size, (*g_copies_n)++, (*g_big_pads) += padded >= 0x10000;
    return u;
}

void dedup_reset() {
    // The map is made again after the arena's release: MSVC's std::map allocates its head node from that arena.
    Copies& c = *g_cp;
    std::destroy_at(&c.map);
    c.arena.release();
    std::construct_at(&c.map, &c.arena);
    (*g_generation)++, (*g_upload_epoch)++;
}

void dedup_invalidate(const uint8_t* p, uint64_t size) {
    (*g_upload_epoch)++;
    // Entries starting before p may still reach into it: look back over the few that can.
    auto it = (*g_cp).map.lower_bound(p + size);
    while (it != (*g_cp).map.begin()) {
        --it;
        if (it->first + it->second.size <= p) {
            if (p - it->first > (64ll << 20)) break;  // nothing larger than 64 MB is copied
            continue;
        }
        if (Front& f = front_of(it->first); f.p == it->first) f.generation = 0;
        it = (*g_cp).map.erase(it);
    }
}

// c[0], the NVN driver's constants, as far as shaders read them (docs/nvn/TRANSLATOR.md): buffer descriptors
// {u64 GPU address; u32 size} of what is bound, which compute shaders search to find the buffer an address falls in
// (they wrote garbage with a zero table), and the maximum point size.
std::array<uint8_t, 0x1000> driver_constants(ShaderStage stage, int st) {
    std::array<uint8_t, 0x1000> c0{};
    auto put = [&](uint32_t at, const Range& r) {
        if (at + 12 > c0.size()) return;
        std::memcpy(c0.data() + at, &r.address, 8);
        uint32_t size = uint32_t(std::min<uint64_t>(r.size, 0xFFFFFFFFu));
        std::memcpy(c0.data() + at + 8, &size, 4);
    };
    if (stage == ShaderStage::Compute) {
        for (int n = 0; n < 16; n++) put(0x210 + 0x10 * n, (*g_state).uniforms[st][n]);
        for (int n = 0; n < 16; n++) put(0x310 + 0x10 * n, (*g_state).storage[st][n]);
    } else {
        int hw = stage == ShaderStage::Fragment ? 4 : 0;  // Maxwell stage numbers: vertex 0, fragment 4
        for (int n = 0; n < 16; n++) put(0x110 + 0x100 * hw + 0x10 * n, (*g_state).storage[st][n]);
        float max_point = 64.0f;
        std::memcpy(c0.data() + 0x610, &max_point, 4);
    }
    return c0;
}

void dedup_log() {
    if ((*g_copied) + (*g_saved))
        log("nvn: uploads copied %.1f MB (%.1f MB of data, %llu copies, %llu padded to 64 KB), reused %.1f MB",
            double((*g_copied)) / 1e6, double((*g_data)) / 1e6, (unsigned long long)(*g_copies_n), (unsigned long long)(*g_big_pads),
            double((*g_saved)) / 1e6);
    (*g_copied) = (*g_saved) = (*g_data) = (*g_copies_n) = (*g_big_pads) = 0;
}

}  // namespace nx::nvn::replay
