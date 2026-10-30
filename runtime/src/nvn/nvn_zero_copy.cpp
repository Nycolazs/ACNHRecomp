// Zero copy (Android; NX_ZERO_COPY=0: off). A memory pool the CPU writes through its cache and the GPU reads (meshes,
// vertex and index data: ~80 MB of ACNH's pools) has its storage replaced by Vulkan memory exported as a
// dma-buf and mapped at the same address (MAP_FIXED): the game's bytes stay where they were, and a VkBuffer covers
// them, so the replay can bind them where it copied them for every draw (memcpy and dedup were ~15% of the replay
// thread on a Snapdragon 865). Turnip offers a device-local, host-visible, coherent and cached memory type there.
// Not the pools the CPU writes uncached (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED: the game's per-frame streaming rings):
// the GPU, reading them when it runs, saw the next frame's bytes (black spikes along the grass and the river banks).
// NX_ZERO_COPY_FLAGS=<hex>: the CPU access bits taken (6: both kinds). S20 FE island: replay 24.6 -> 23.0 ms.
// The replay binds those bytes in place (zero_copy_range) where it copied them: vertex and index buffers, and uniform
// blocks whose reads stay within the bound data (Maxwell reads 0 past it; here the pool's next bytes would be read).
// Not ranges a compute shader of this submit wrote, or whose earlier results are still to come (late write-backs):
// those keep their copies (nvn_written.cpp). The GPU then reads the game's bytes when it runs, not as they were when
// the replay got to them.
#include "nvn.h"
#include "nvn_draw_state.h"
#include "nvn_vk.h"
#include "platform.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__ANDROID__)
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace nx::nvn {

using namespace nx::vk;

namespace {

bool wanted() {
#if defined(__ANDROID__)
    static const bool on = [] {
        const char* e = std::getenv("NX_ZERO_COPY");
        return !(e && e[0] == '0');
    }();
    return on;
#else
    return false;
#endif
}

}  // namespace

bool zero_copy_device_extensions() { return wanted(); }

void zero_copy_pool(MemoryPool& p) {
#if defined(__ANDROID__)
    // NVN memory pool flags: CPU_NO_ACCESS 1, CPU_UNCACHED 2, CPU_CACHED 4, GPU_NO_ACCESS 8, PHYSICAL 0x100,
    // VIRTUAL 0x200. Pools the CPU writes and the GPU reads, with storage of their own.
    static const unsigned only = [] {  // the CPU access bits taken: CPU_CACHED (4)
        const char* e = std::getenv("NX_ZERO_COPY_FLAGS");
        return e ? unsigned(std::strtoul(e, nullptr, 16)) : 4u;
    }();
    if (!wanted() || !p.memory || !p.size || !(p.flags & only) || (p.flags & 0x309) || !vk_device()) return;
    const long page = sysconf(_SC_PAGESIZE);
    if (p.memory % page || p.size % page) return;
    static int logged = 0;
    auto fail = [&](const char* what, int code) {
        if (logged++ < 10) log("zero copy: pool %llx (%llu KB, flags %x): %s (%d)", (unsigned long long)p.memory,
                               (unsigned long long)(p.size >> 10), p.flags, what, code);
    };
    VkDevice dev = vk_device();
    VkExternalMemoryBufferCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext};
    bi.size = p.size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
               VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    VkBuffer buf = VK_NULL_HANDLE;
    if (VkResult r = p_vkCreateBuffer(dev, &bi, nullptr, &buf); r != VK_SUCCESS) return fail("vkCreateBuffer", r);
    VkMemoryRequirements req{};
    p_vkGetBufferMemoryRequirements(dev, buf, &req);
    const VkMemoryPropertyFlags want =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkExportMemoryAllocateInfo exp{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &exp};
    ded.buffer = buf;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = memory_type(req.memoryTypeBits, want);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (VkResult r = p_vkAllocateMemory(dev, &ai, nullptr, &mem); r != VK_SUCCESS) {
        p_vkDestroyBuffer(dev, buf, nullptr);
        return fail("vkAllocateMemory", r);
    }
    p_vkBindBufferMemory(dev, buf, mem, 0);
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = mem, gi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    int fd = -1;
    VkResult r = p_vkGetMemoryFdKHR ? p_vkGetMemoryFdKHR(dev, &gi, &fd) : VK_ERROR_EXTENSION_NOT_PRESENT;
    void* staging = r == VK_SUCCESS ? mmap(nullptr, p.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    if (staging == MAP_FAILED) {
        if (fd >= 0) close(fd);
        p_vkDestroyBuffer(dev, buf, nullptr), p_vkFreeMemory(dev, mem, nullptr);
        return fail(r == VK_SUCCESS ? "mmap of the dma-buf" : "vkGetMemoryFdKHR", r == VK_SUCCESS ? errno : r);
    }
    void* guest = reinterpret_cast<void*>(p.memory);
    std::memcpy(staging, guest, p.size);  // what the game wrote before it made the pool
    void* at = mmap(guest, p.size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    munmap(staging, p.size);
    close(fd);
    if (at != guest) {  // MAP_FIXED replaced the old pages only if it succeeded: they are intact otherwise
        p_vkDestroyBuffer(dev, buf, nullptr), p_vkFreeMemory(dev, mem, nullptr);
        return fail("mmap at the pool's address", errno);
    }
    p.zc_buffer = buf, p.zc_memory = mem;
    static uint64_t pools = 0, bytes = 0;
    pools++, bytes += p.size;
    if (logged++ < 10 || (pools & 63) == 0)
        log("zero copy: pool %llx (%llu KB, flags %x) on GPU memory (%llu pools, %llu MB)", (unsigned long long)p.memory,
            (unsigned long long)(p.size >> 10), p.flags, (unsigned long long)pools, (unsigned long long)(bytes >> 20));
#else
    (void)p;
#endif
}

bool zero_copy_range(uint64_t gpu, uint64_t bytes, uint64_t align, Upload& out) {
    if (!wanted() || !gpu || !bytes) return false;
    uint64_t off = 0;
    MemoryPool* p = pool_at_gpu(gpu, &off);
    if (!p || !p->zc_buffer || off % std::max<uint64_t>(align, 1) || off + bytes > p->size) return false;
    const uint8_t* host = reinterpret_cast<const uint8_t*>(p->memory + off);
    if (replay::written_overlaps(host, bytes) || late_pending(host, bytes)) return false;
    out = {VkBuffer(p->zc_buffer), off, const_cast<uint8_t*>(host)};
    static std::atomic<uint64_t> hits{0};
    if (++hits % 200000 == 1) log("zero copy: %llu bindings in place so far", (unsigned long long)hits.load());
    return true;
}

uint64_t zero_copy_uniform_align() {
    static const uint64_t a = [] {
        VkPhysicalDeviceProperties props{};
        p_vkGetPhysicalDeviceProperties(vk_physical(), &props);
        return std::max<uint64_t>({props.limits.minUniformBufferOffsetAlignment, props.limits.minStorageBufferOffsetAlignment, 16});
    }();
    return a;
}

// Freed pools' buffers and memory, kept until the GPU is done with every submit that could read them: the submit
// being replayed when the game freed the pool included (destroying them at once hung the GPU).
struct Retired {
    uint64_t after;  // free once frame_completed() reaches it
    VkBuffer buffer;
    VkDeviceMemory memory;
};
std::mutex g_retired_lock;
std::vector<Retired> g_retired;

void collect_retired() {  // g_retired_lock held
    const uint64_t done = frame_completed();
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(), [&](const Retired& r) {
                        if (r.after > done) return false;
                        p_vkDestroyBuffer(vk_device(), r.buffer, nullptr);
                        p_vkFreeMemory(vk_device(), r.memory, nullptr);
                        return true;
                    }),
                    g_retired.end());
}

void zero_copy_release(MemoryPool& p) {
#if defined(__ANDROID__)
    if (!p.zc_buffer) return;
    // Ordinary memory back under the guest's address at once (the game frees it to its heap, which reuses it), its
    // bytes kept; the GPU's allocation keeps the dma-buf's pages for the submits still reading them.
    void* guest = reinterpret_cast<void*>(p.memory);
    void* tmp = mmap(nullptr, p.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (tmp != MAP_FAILED) std::memcpy(tmp, guest, p.size);
    mmap(guest, p.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (tmp != MAP_FAILED) std::memcpy(guest, tmp, p.size), munmap(tmp, p.size);
    std::lock_guard lk(g_retired_lock);
    g_retired.push_back({frame_submitted() + 1, VkBuffer(p.zc_buffer), VkDeviceMemory(p.zc_memory)});
    p.zc_buffer = p.zc_memory = nullptr;
    collect_retired();
#else
    (void)p;
#endif
}

}  // namespace nx::nvn
