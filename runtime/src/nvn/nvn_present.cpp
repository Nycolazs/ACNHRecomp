// The NVN present pass (docs/nvn/ANDROID-PORT.md A2; DEBUG-2.md #79): the window texture drawn into the swapchain
// image by a render pass instead of vkCmdBlitImage.
// - Rotated by the swapchain's preTransform: phones scan out portrait, and a pre-rotated image goes to the display
//   controller as is (the compositor rotates otherwise, on the GPU whenever system UI is on top).
// - Fit with bars: a 16:9 picture on a 19.5:9 screen keeps its shape.
// - Alpha 1: an Android surface may take the image's alpha as transparency (compositeAlpha INHERIT), and the game's
//   final image has no meaningful alpha.
// NX_PRESENT_PASS=1 turns it on (FSR and FXAA do too), =0 off (the blit).
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "config.h"
#include "nvn_shaders.h"
#include "nvn_vk.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// glslc -O --target-env=vulkan1.0 of present.vert:
//   #version 450
//   // A triangle covering the target; uv 0..1 across it (y down, as Vulkan framebuffer coordinates).
//   layout(location = 0) out vec2 uv;
//   void main() {
//       vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
//       uv = p;
//       gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
//   }
const uint32_t kVertex[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000018, 0x00000020, 0x00040047, 0x0000000c, 0x0000000b,
    0x0000002a, 0x00040047, 0x00000018, 0x0000001e, 0x00000000, 0x00030047, 0x0000001e, 0x00000002, 0x00050048,
    0x0000001e, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001e, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001e, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001e, 0x00000003, 0x0000000b,
    0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020,
    0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a,
    0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040020, 0x00000017, 0x00000003,
    0x00000007, 0x0004003b, 0x00000017, 0x00000018, 0x00000003, 0x00040017, 0x0000001a, 0x00000006, 0x00000004,
    0x00040015, 0x0000001b, 0x00000020, 0x00000000, 0x0004002b, 0x0000001b, 0x0000001c, 0x00000001, 0x0004001c,
    0x0000001d, 0x00000006, 0x0000001c, 0x0006001e, 0x0000001e, 0x0000001a, 0x00000006, 0x0000001d, 0x0000001d,
    0x00040020, 0x0000001f, 0x00000003, 0x0000001e, 0x0004003b, 0x0000001f, 0x00000020, 0x00000003, 0x0004002b,
    0x0000000a, 0x00000021, 0x00000000, 0x0004002b, 0x00000006, 0x00000023, 0x40000000, 0x0004002b, 0x00000006,
    0x00000025, 0x3f800000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000, 0x00040020, 0x0000002c, 0x00000003,
    0x0000001a, 0x0005002c, 0x00000007, 0x0000002e, 0x00000025, 0x00000025, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4,
    0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010,
    0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010,
    0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
    0x0003003e, 0x00000018, 0x00000016, 0x0005008e, 0x00000007, 0x00000024, 0x00000016, 0x00000023, 0x00050083,
    0x00000007, 0x00000027, 0x00000024, 0x0000002e, 0x00050051, 0x00000006, 0x00000029, 0x00000027, 0x00000000,
    0x00050051, 0x00000006, 0x0000002a, 0x00000027, 0x00000001, 0x00070050, 0x0000001a, 0x0000002b, 0x00000029,
    0x0000002a, 0x00000028, 0x00000025, 0x00050041, 0x0000002c, 0x0000002d, 0x00000020, 0x00000021, 0x0003003e,
    0x0000002d, 0x0000002b, 0x000100fd, 0x00010038,
};

// glslc -O --target-env=vulkan1.0 of tools/shaders/present/present.frag (the touch controls drawn per grid cell).
const uint32_t kFragment[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x000000e7, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000022, 0x000000ce, 0x00030010, 0x00000004, 0x00000007, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000b,
    0x00000001, 0x00000023, 0x00000010, 0x00050048, 0x0000000b, 0x00000002, 0x00000023, 0x00000020, 0x00050048,
    0x0000000b, 0x00000003, 0x00000023, 0x00000030, 0x00040047, 0x00000022, 0x0000001e, 0x00000000, 0x00040047,
    0x0000004b, 0x00000021, 0x00000000, 0x00040047, 0x0000004b, 0x00000022, 0x00000000, 0x00040047, 0x0000007c,
    0x00000006, 0x00000010, 0x00040047, 0x0000007f, 0x00000006, 0x00000010, 0x00030047, 0x00000080, 0x00000002,
    0x00050048, 0x00000080, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000080, 0x00000001, 0x00000023,
    0x00000200, 0x00040047, 0x00000082, 0x00000021, 0x00000001, 0x00040047, 0x00000082, 0x00000022, 0x00000000,
    0x00040047, 0x000000ce, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040017, 0x0000000a,
    0x00000006, 0x00000004, 0x0006001e, 0x0000000b, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00040020,
    0x0000000c, 0x00000009, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000009, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000009,
    0x0000000a, 0x00040018, 0x00000017, 0x00000007, 0x00000002, 0x0004002b, 0x00000006, 0x00000018, 0x3f800000,
    0x0004002b, 0x00000006, 0x00000019, 0x00000000, 0x00040020, 0x00000021, 0x00000001, 0x00000007, 0x0004003b,
    0x00000021, 0x00000022, 0x00000001, 0x0004002b, 0x0000000e, 0x00000025, 0x00000001, 0x0004002b, 0x0000000e,
    0x0000002c, 0x00000002, 0x00040017, 0x00000035, 0x00000006, 0x00000003, 0x0006002c, 0x00000035, 0x00000038,
    0x00000019, 0x00000019, 0x00000019, 0x00020014, 0x00000039, 0x0005002c, 0x00000007, 0x0000003b, 0x00000019,
    0x00000019, 0x00040017, 0x0000003c, 0x00000039, 0x00000002, 0x0005002c, 0x00000007, 0x00000042, 0x00000018,
    0x00000018, 0x00090019, 0x00000048, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x00000049, 0x00000048, 0x00040020, 0x0000004a, 0x00000000, 0x00000049, 0x0004003b,
    0x0000004a, 0x0000004b, 0x00000000, 0x0004002b, 0x0000000e, 0x0000004d, 0x00000003, 0x00040015, 0x00000059,
    0x00000020, 0x00000000, 0x0004002b, 0x00000059, 0x0000005a, 0x00000002, 0x00040020, 0x0000005b, 0x00000009,
    0x00000006, 0x00040017, 0x00000061, 0x0000000e, 0x00000002, 0x0004002b, 0x00000006, 0x00000065, 0x41000000,
    0x0004002b, 0x00000006, 0x00000066, 0x40800000, 0x0005002c, 0x00000007, 0x00000067, 0x00000065, 0x00000066,
    0x0005002c, 0x00000061, 0x0000006a, 0x0000000f, 0x0000000f, 0x0004002b, 0x0000000e, 0x0000006b, 0x00000007,
    0x0005002c, 0x00000061, 0x0000006c, 0x0000006b, 0x0000004d, 0x0004002b, 0x00000059, 0x00000070, 0x00000001,
    0x0004002b, 0x0000000e, 0x00000073, 0x00000008, 0x0004002b, 0x00000059, 0x00000075, 0x00000000, 0x0004002b,
    0x00000059, 0x0000007b, 0x00000020, 0x0004001c, 0x0000007c, 0x0000000a, 0x0000007b, 0x00040017, 0x0000007d,
    0x00000059, 0x00000004, 0x0004002b, 0x00000059, 0x0000007e, 0x00000008, 0x0004001c, 0x0000007f, 0x0000007d,
    0x0000007e, 0x0004001e, 0x00000080, 0x0000007c, 0x0000007f, 0x00040020, 0x00000081, 0x00000002, 0x00000080,
    0x0004003b, 0x00000081, 0x00000082, 0x00000002, 0x00040020, 0x00000087, 0x00000002, 0x00000059, 0x00040020,
    0x0000009b, 0x00000002, 0x0000000a, 0x0004002b, 0x00000059, 0x000000a5, 0x00000003, 0x0006002c, 0x00000035,
    0x000000b7, 0x00000018, 0x00000018, 0x00000018, 0x0004002b, 0x00000006, 0x000000c0, 0x3ba3d70a, 0x0004002b,
    0x00000006, 0x000000c8, 0x40400000, 0x00040020, 0x000000cd, 0x00000003, 0x0000000a, 0x0004003b, 0x000000cd,
    0x000000ce, 0x00000003, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005,
    0x00050041, 0x00000010, 0x00000011, 0x0000000d, 0x0000000f, 0x0004003d, 0x0000000a, 0x00000012, 0x00000011,
    0x00050051, 0x00000006, 0x0000001a, 0x00000012, 0x00000000, 0x00050051, 0x00000006, 0x0000001b, 0x00000012,
    0x00000001, 0x00050051, 0x00000006, 0x0000001c, 0x00000012, 0x00000002, 0x00050051, 0x00000006, 0x0000001d,
    0x00000012, 0x00000003, 0x00050050, 0x00000007, 0x0000001e, 0x0000001a, 0x0000001b, 0x00050050, 0x00000007,
    0x0000001f, 0x0000001c, 0x0000001d, 0x00050050, 0x00000017, 0x00000020, 0x0000001e, 0x0000001f, 0x0004003d,
    0x00000007, 0x00000023, 0x00000022, 0x00050091, 0x00000007, 0x00000024, 0x00000020, 0x00000023, 0x00050041,
    0x00000010, 0x00000026, 0x0000000d, 0x00000025, 0x0004003d, 0x0000000a, 0x00000027, 0x00000026, 0x0007004f,
    0x00000007, 0x00000028, 0x00000027, 0x00000027, 0x00000000, 0x00000001, 0x00050081, 0x00000007, 0x00000029,
    0x00000024, 0x00000028, 0x00050041, 0x00000010, 0x0000002d, 0x0000000d, 0x0000002c, 0x0004003d, 0x0000000a,
    0x0000002e, 0x0000002d, 0x0007004f, 0x00000007, 0x0000002f, 0x0000002e, 0x0000002e, 0x00000000, 0x00000001,
    0x00050083, 0x00000007, 0x00000030, 0x00000029, 0x0000002f, 0x0007004f, 0x00000007, 0x00000033, 0x0000002e,
    0x0000002e, 0x00000002, 0x00000003, 0x00050088, 0x00000007, 0x00000034, 0x00000030, 0x00000033, 0x000500be,
    0x0000003c, 0x0000003d, 0x00000034, 0x0000003b, 0x0004009b, 0x00000039, 0x0000003e, 0x0000003d, 0x000300f7,
    0x00000040, 0x00000000, 0x000400fa, 0x0000003e, 0x0000003f, 0x00000040, 0x000200f8, 0x0000003f, 0x000500bc,
    0x0000003c, 0x00000043, 0x00000034, 0x00000042, 0x0004009b, 0x00000039, 0x00000044, 0x00000043, 0x000200f9,
    0x00000040, 0x000200f8, 0x00000040, 0x000700f5, 0x00000039, 0x00000045, 0x0000003e, 0x00000005, 0x00000044,
    0x0000003f, 0x000300f7, 0x00000047, 0x00000000, 0x000400fa, 0x00000045, 0x00000046, 0x00000047, 0x000200f8,
    0x00000046, 0x0004003d, 0x00000049, 0x0000004c, 0x0000004b, 0x00050041, 0x00000010, 0x0000004e, 0x0000000d,
    0x0000004d, 0x0004003d, 0x0000000a, 0x0000004f, 0x0000004e, 0x0007004f, 0x00000007, 0x00000050, 0x0000004f,
    0x0000004f, 0x00000000, 0x00000001, 0x0007004f, 0x00000007, 0x00000054, 0x0000004f, 0x0000004f, 0x00000002,
    0x00000003, 0x00050085, 0x00000007, 0x00000055, 0x00000034, 0x00000054, 0x00050081, 0x00000007, 0x00000056,
    0x00000050, 0x00000055, 0x00050057, 0x0000000a, 0x00000057, 0x0000004c, 0x00000056, 0x0008004f, 0x00000035,
    0x00000058, 0x00000057, 0x00000057, 0x00000000, 0x00000001, 0x00000002, 0x000200f9, 0x00000047, 0x000200f8,
    0x00000047, 0x000700f5, 0x00000035, 0x000000e0, 0x00000038, 0x00000040, 0x00000058, 0x00000046, 0x00060041,
    0x0000005b, 0x0000005c, 0x0000000d, 0x00000025, 0x0000005a, 0x0004003d, 0x00000006, 0x0000005d, 0x0000005c,
    0x000500ba, 0x00000039, 0x0000005e, 0x0000005d, 0x00000019, 0x000300f7, 0x00000060, 0x00000000, 0x000400fa,
    0x0000005e, 0x0000005f, 0x00000060, 0x000200f8, 0x0000005f, 0x00050085, 0x00000007, 0x00000068, 0x00000029,
    0x00000067, 0x0004006e, 0x00000061, 0x00000069, 0x00000068, 0x0008000c, 0x00000061, 0x0000006d, 0x00000001,
    0x0000002d, 0x00000069, 0x0000006a, 0x0000006c, 0x00050051, 0x0000000e, 0x00000072, 0x0000006d, 0x00000001,
    0x00050084, 0x0000000e, 0x00000074, 0x00000072, 0x00000073, 0x00050051, 0x0000000e, 0x00000077, 0x0000006d,
    0x00000000, 0x00050080, 0x0000000e, 0x00000078, 0x00000074, 0x00000077, 0x000500c3, 0x0000000e, 0x00000084,
    0x00000078, 0x0000002c, 0x000500c7, 0x0000000e, 0x00000086, 0x00000078, 0x0000004d, 0x00070041, 0x00000087,
    0x00000088, 0x00000082, 0x00000025, 0x00000084, 0x00000086, 0x0004003d, 0x00000059, 0x00000089, 0x00000088,
    0x000200f9, 0x0000008a, 0x000200f8, 0x0000008a, 0x000700f5, 0x00000035, 0x000000e2, 0x000000e0, 0x0000005f,
    0x000000e5, 0x0000008d, 0x000700f5, 0x00000059, 0x000000de, 0x00000089, 0x0000005f, 0x00000097, 0x0000008d,
    0x000500ab, 0x00000039, 0x00000090, 0x000000de, 0x00000075, 0x000400f6, 0x0000008c, 0x0000008d, 0x00000000,
    0x000400fa, 0x00000090, 0x0000008b, 0x0000008c, 0x000200f8, 0x0000008b, 0x0006000c, 0x0000000e, 0x00000093,
    0x00000001, 0x00000049, 0x000000de, 0x00050082, 0x00000059, 0x00000095, 0x000000de, 0x00000070, 0x000500c7,
    0x00000059, 0x00000097, 0x000000de, 0x00000095, 0x00060041, 0x0000009b, 0x0000009c, 0x00000082, 0x0000000f,
    0x00000093, 0x0004003d, 0x0000000a, 0x0000009d, 0x0000009c, 0x00050051, 0x00000006, 0x000000a1, 0x00000029,
    0x00000000, 0x00050051, 0x00000006, 0x000000a3, 0x0000009d, 0x00000000, 0x00050083, 0x00000006, 0x000000a4,
    0x000000a1, 0x000000a3, 0x00060041, 0x0000005b, 0x000000a6, 0x0000000d, 0x00000025, 0x000000a5, 0x0004003d,
    0x00000006, 0x000000a7, 0x000000a6, 0x00050085, 0x00000006, 0x000000a8, 0x000000a4, 0x000000a7, 0x00050051,
    0x00000006, 0x000000aa, 0x00000029, 0x00000001, 0x00050051, 0x00000006, 0x000000ac, 0x0000009d, 0x00000001,
    0x00050083, 0x00000006, 0x000000ad, 0x000000aa, 0x000000ac, 0x00050050, 0x00000007, 0x000000ae, 0x000000a8,
    0x000000ad, 0x0006000c, 0x00000006, 0x000000af, 0x00000001, 0x00000042, 0x000000ae, 0x00050051, 0x00000006,
    0x000000b2, 0x0000009d, 0x00000002, 0x000500b8, 0x00000039, 0x000000b3, 0x000000af, 0x000000b2, 0x000300f7,
    0x000000b5, 0x00000000, 0x000400fa, 0x000000b3, 0x000000b4, 0x000000bc, 0x000200f8, 0x000000b4, 0x00050051,
    0x00000006, 0x000000b9, 0x0000009d, 0x00000003, 0x00060050, 0x00000035, 0x000000ba, 0x000000b9, 0x000000b9,
    0x000000b9, 0x0008000c, 0x00000035, 0x000000bb, 0x00000001, 0x0000002e, 0x000000e2, 0x000000b7, 0x000000ba,
    0x000200f9, 0x000000b5, 0x000200f8, 0x000000bc, 0x00050081, 0x00000006, 0x000000c1, 0x000000b2, 0x000000c0,
    0x000500b8, 0x00000039, 0x000000c2, 0x000000af, 0x000000c1, 0x000300f7, 0x000000c4, 0x00000000, 0x000400fa,
    0x000000c2, 0x000000c3, 0x000000c4, 0x000200f8, 0x000000c3, 0x00050051, 0x00000006, 0x000000c7, 0x0000009d,
    0x00000003, 0x00050085, 0x00000006, 0x000000c9, 0x000000c7, 0x000000c8, 0x0007000c, 0x00000006, 0x000000ca,
    0x00000001, 0x00000025, 0x00000018, 0x000000c9, 0x00060050, 0x00000035, 0x000000cb, 0x000000ca, 0x000000ca,
    0x000000ca, 0x0008000c, 0x00000035, 0x000000cc, 0x00000001, 0x0000002e, 0x000000e2, 0x000000b7, 0x000000cb,
    0x000200f9, 0x000000c4, 0x000200f8, 0x000000c4, 0x000700f5, 0x00000035, 0x000000e6, 0x000000e2, 0x000000bc,
    0x000000cc, 0x000000c3, 0x000200f9, 0x000000b5, 0x000200f8, 0x000000b5, 0x000700f5, 0x00000035, 0x000000e5,
    0x000000bb, 0x000000b4, 0x000000e6, 0x000000c4, 0x000200f9, 0x0000008d, 0x000200f8, 0x0000008d, 0x000200f9,
    0x0000008a, 0x000200f8, 0x0000008c, 0x000200f9, 0x00000060, 0x000200f8, 0x00000060, 0x000700f5, 0x00000035,
    0x000000df, 0x000000e0, 0x00000047, 0x000000e2, 0x0000008c, 0x00050051, 0x00000006, 0x000000d0, 0x000000df,
    0x00000000, 0x00050051, 0x00000006, 0x000000d1, 0x000000df, 0x00000001, 0x00050051, 0x00000006, 0x000000d2,
    0x000000df, 0x00000002, 0x00070050, 0x0000000a, 0x000000d3, 0x000000d0, 0x000000d1, 0x000000d2, 0x00000018,
    0x0003003e, 0x000000ce, 0x000000d3, 0x000100fd, 0x00010038,
};

constexpr size_t kOverlayMax = 32;  // Overlay.shape[32] in present.frag
constexpr size_t kOverlayBytes = sizeof(float) * 4 * kOverlayMax + sizeof(uint32_t) * 32;  // + Overlay.cell

struct Push {
    float rot[4], off[4], box[4], crop[4];
    float hdr[4];  // HDR shader only (tools/shaders/hdr/present_hdr.hlsl): UI nits, peak nits, gamma, output preset
};

#include "nvn_fsr_spirv.inc"
#include "nvn_aa_spirv.inc"
#include "nvn_hdr_spirv.inc"

struct Pass {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool failed = false;
    // FSR 1 (NX_FSR=1): EASU draws the shown part of the texture, upscaled, into `inter` (the size of the picture on
    // the target); the main pass then sharpens it (RCAS) into the swapchain image.
    VkRenderPass easu_pass = VK_NULL_HANDLE;
    VkPipeline easu = VK_NULL_HANDLE, rcas = VK_NULL_HANDLE;
    VkImage inter = VK_NULL_HANDLE;
    VkDeviceMemory inter_memory = VK_NULL_HANDLE;
    VkImageView inter_view = VK_NULL_HANDLE;
    VkFramebuffer inter_fb = VK_NULL_HANDLE;
    uint32_t inter_w = 0, inter_h = 0;
    bool fsr_logged = false, fsr_failed = false;
    // FXAA (video.antialiasing = fxaa, NX_AA=fxaa): the main pass with tools/shaders/aa/fxaa.frag; not with FSR.
    VkPipeline fxaa = VK_NULL_HANDLE;
    bool fxaa_logged = false, fxaa_failed = false;
} g;

// FSR, FXAA and the sharpening are read while the game runs (the settings screen changes them, video.upscaler,
// video.antialiasing, video.fsr_sharpness_pct): a variable set before the game (NX_FSR, NX_AA, NX_FSR_SHARP) wins.
template <class T, class F>
T live_setting(std::atomic<uint64_t>& gen, std::atomic<T>& value, F&& read) {
    const uint64_t g = config_generation();
    if (gen.load(std::memory_order_acquire) != g) value.store(read(), std::memory_order_relaxed), gen.store(g, std::memory_order_release);
    return value.load(std::memory_order_relaxed);
}

bool fsr_on() {
    static std::atomic<uint64_t> gen{~0ull};
    static std::atomic<bool> v{false};
    return live_setting(gen, v, [] {
        if (const char* e = config_env_override("NX_FSR")) return e[0] == '1';
        return config_get("video.upscaler", plat::kDefaultUpscaler) == "fsr";  // phones: on (platform.h)
    });
}

bool fxaa_on() {
    static std::atomic<uint64_t> gen{~0ull};
    static std::atomic<bool> v{false};
    return live_setting(gen, v, [] {
        if (const char* e = config_env_override("NX_AA")) return std::strcmp(e, "fxaa") == 0;
        return config_get("video.antialiasing", "off") == "fxaa";
    });
}

// The HDR shader's brightness settings, read while the game runs: the UI's white (video.hdr_ui_nits; the scene's is
// its tonemap's), the display's peak (video.hdr_peak_nits) and the gamma the game's picture is decoded with
// (video.hdr_gamma: 2.2, as most displays show SDR, or srgb). 203 nits is BT.2408's reference and graphics white.
// The tonemap replacements (spirv_hdr) also take the scene's white (video.hdr_game_nits), the tonemap
// (video.hdr_tonemap: hdr, or vanilla: the game's curve, clipped at its white), an exposure in stops and the F2
// panel's grading: highlights, shadows, contrast and saturation as 0..100 (50 neutral, RenoDX's scale), temperature
// and tint as -100..100 (0 neutral).
struct HdrLevels {
    float ui_nits = 203, peak_nits = 1000, gamma = 1;
    float game_nits = 203, tonemap = 1, exposure = 0;
    float highlights = 1, shadows = 1, contrast = 1, saturation = 1, temperature = 0, tint = 0;
};

HdrLevels hdr_levels() {
    static std::atomic<uint64_t> gen{~0ull};
    static HdrLevels v;
    static std::mutex lock;
    std::lock_guard lk(lock);
    if (gen.load(std::memory_order_relaxed) != config_generation()) {
        gen = config_generation();
        v.ui_nits = std::clamp(config_get_float("video.hdr_ui_nits", 203), 80.0f, 500.0f);
        v.peak_nits = std::clamp(config_get_float("video.hdr_peak_nits", 1000), 400.0f, 10000.0f);
        v.gamma = config_get("video.hdr_gamma", "2.2") == "srgb" ? 0.0f : 1.0f;  // renodx GAMMA_CORRECTION_*
        v.game_nits = std::clamp(config_get_float("video.hdr_game_nits", 203), 80.0f, 500.0f);
        const std::string tm = config_get("video.hdr_tonemap", "hdr");
        v.tonemap = tm == "vanilla" ? 0.0f : tm == "renodrt" ? 2.0f : 1.0f;  // acnh_tonemap.hlsli FinishTonemap
        v.exposure = std::clamp(config_get_float("video.hdr_exposure", 0), -2.0f, 2.0f);
        auto grade = [](const char* key) { return std::clamp(config_get_float(key, 50), 0.0f, 100.0f) * 0.02f; };
        v.highlights = grade("video.hdr_highlights"), v.shadows = grade("video.hdr_shadows");
        v.contrast = grade("video.hdr_contrast"), v.saturation = grade("video.hdr_saturation");
        v.temperature = std::clamp(config_get_float("video.hdr_temperature", 0), -100.0f, 100.0f);
        v.tint = std::clamp(config_get_float("video.hdr_tint", 0), -100.0f, 100.0f);
    }
    return v;
}

float fsr_sharpness() {  // stops of sharpening: 0 the most, 2 none (FSR's own default is 0.2)
    static std::atomic<uint64_t> gen{~0ull};
    static std::atomic<float> v{0.2f};
    return live_setting(gen, v, [] {
        if (const char* e = config_env_override("NX_FSR_SHARP")) return std::max(0.0f, std::min(2.0f, float(std::atof(e))));
        const int pct = std::clamp(std::atoi(config_get("video.fsr_sharpness_pct", "80").c_str()), 0, 100);
        return float(100 - pct) / 50.0f;  // as config.cpp's fsr_sharpness
    });
}

// The intermediate image for a picture of w x h pixels on the target, made again when the size changes.
bool fsr_inter(uint32_t w, uint32_t h) {
    if (g.inter && g.inter_w == w && g.inter_h == h) return true;
    VkDevice d = vk_device();
    if (g.inter) {  // a new size means a new window: the swapchain was remade, which waited for the queue
        p_vkDestroyFramebuffer(d, g.inter_fb, nullptr);
        p_vkDestroyImageView(d, g.inter_view, nullptr);
        p_vkDestroyImage(d, g.inter, nullptr);
        p_vkFreeMemory(d, g.inter_memory, nullptr);
        g.inter = VK_NULL_HANDLE, g.inter_view = VK_NULL_HANDLE, g.inter_fb = VK_NULL_HANDLE;
        g.inter_memory = VK_NULL_HANDLE;
    }
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &g.inter) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, g.inter, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (p_vkAllocateMemory(d, &ai, nullptr, &g.inter_memory) != VK_SUCCESS) return false;
    p_vkBindImageMemory(d, g.inter, g.inter_memory, 0);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = g.inter, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = ci.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (p_vkCreateImageView(d, &vi, nullptr, &g.inter_view) != VK_SUCCESS) return false;
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = g.easu_pass, fi.attachmentCount = 1, fi.pAttachments = &g.inter_view;
    fi.width = w, fi.height = h, fi.layers = 1;
    if (p_vkCreateFramebuffer(d, &fi, nullptr, &g.inter_fb) != VK_SUCCESS) return false;
    g.inter_w = w, g.inter_h = h;
    return true;
}

VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m);
    return m;
}

void destroy_targets() {
    VkDevice d = vk_device();
    for (VkFramebuffer f : g.framebuffers) p_vkDestroyFramebuffer(d, f, nullptr);
    for (VkImageView v : g.views) p_vkDestroyImageView(d, v, nullptr);
    g.framebuffers.clear(), g.views.clear();
    if (g.inter_fb) p_vkDestroyFramebuffer(d, g.inter_fb, nullptr);
    if (g.inter_view) p_vkDestroyImageView(d, g.inter_view, nullptr);
    if (g.inter) p_vkDestroyImage(d, g.inter, nullptr);
    if (g.inter_memory) p_vkFreeMemory(d, g.inter_memory, nullptr);
    g.inter_fb = VK_NULL_HANDLE, g.inter_view = VK_NULL_HANDLE, g.inter = VK_NULL_HANDLE;
    g.inter_memory = VK_NULL_HANDLE, g.inter_w = g.inter_h = 0;
}

// A full-target triangle drawn with the fragment shader `code` in render pass `pass` (g.layout).
bool build_pipeline(VkRenderPass pass, const uint32_t* code, size_t bytes, VkPipeline* out) {
    VkDevice d = vk_device();
    VkShaderModule vs = module(kVertex, sizeof kVertex), fs = module(code, bytes);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2, gp.pStages = stages;
    gp.pVertexInputState = &vin, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp;
    gp.pRasterizationState = &rs, gp.pMultisampleState = &ms, gp.pColorBlendState = &cb, gp.pDynamicState = &ds;
    gp.layout = g.layout, gp.renderPass = pass;
    VkResult r = p_vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, out);
    p_vkDestroyShaderModule(d, vs, nullptr);
    p_vkDestroyShaderModule(d, fs, nullptr);
    return r == VK_SUCCESS;
}

// FSR's render pass (RGBA16F, left readable by the main pass) and the two pipelines.
bool make_fsr() {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep[2] = {};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL, dep[0].dstSubpass = 0;  // the last frame's RCAS has read it
    dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].srcSubpass = 0, dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;  // RCAS reads what EASU wrote
    dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 2, rpi.pDependencies = dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.easu_pass) != VK_SUCCESS) return false;
    return build_pipeline(g.easu_pass, kEasu, sizeof kEasu, &g.easu) &&
           build_pipeline(g.render_pass, kRcas, sizeof kRcas, &g.rcas);
}

bool make_pipeline(VkFormat format) {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1, rpi.pDependencies = &dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.render_pass) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &g.sampler},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;

    // HDR: the picture is encoded for the display by the main pass, and FSR's and FXAA's shaders write it as it is: off.
    const bool hdr = g_hdr != HdrOutput::Off;
    VkResult ok = (hdr ? build_pipeline(g.render_pass, kHdrPresent, sizeof kHdrPresent, &g.pipeline)
                       : build_pipeline(g.render_pass, kFragment, sizeof kFragment, &g.pipeline))
                      ? VK_SUCCESS
                      : VK_ERROR_UNKNOWN;
    if (ok == VK_SUCCESS && hdr) log("nvn/vk: HDR present (%s)%s", g_hdr == HdrOutput::ScRgb ? "scRGB" : "HDR10",
                                     fsr_on() || fxaa_on() ? ": FSR and FXAA off" : "");
    if (ok == VK_SUCCESS && !hdr && fsr_on() && !make_fsr()) log("nvn/vk: FSR unavailable: plain scaling");
    if (ok == VK_SUCCESS && !hdr && fxaa_on() && !build_pipeline(g.render_pass, kFxaa, sizeof kFxaa, &g.fxaa))
        log("nvn/vk: FXAA unavailable: no anti-aliasing");
    g.format = format;
    return ok == VK_SUCCESS;
}

}  // namespace

HdrOutput hdr_requested() {
    static const HdrOutput v = [] {
        const char* e = std::getenv("NX_HDR");  // set from video.hdr by config_apply
        return !e ? HdrOutput::Off : std::strcmp(e, "scrgb") == 0 ? HdrOutput::ScRgb
                                   : std::strcmp(e, "hdr10") == 0 ? HdrOutput::Hdr10
                                                                  : HdrOutput::Off;
    }();
    return v;
}

void hdr_shader_settings(float out[4][4]) {
    const HdrLevels l = hdr_levels();
    out[0][0] = l.peak_nits, out[0][1] = l.game_nits, out[0][2] = l.ui_nits, out[0][3] = l.gamma;
    out[1][0] = l.tonemap, out[1][1] = l.exposure, out[1][2] = l.highlights, out[1][3] = l.shadows;
    out[2][0] = l.contrast, out[2][1] = l.saturation, out[2][2] = l.temperature, out[2][3] = l.tint;
    out[3][0] = out[3][1] = out[3][2] = out[3][3] = 0;
}

// The settings screen on the HDR swapchain (ui.cpp): 0 SDR, 1 scRGB, 2 HDR10; the UI brightness and the gamma (2.2 or
// sRGB) its colours are decoded with, as the game's UI is.
int hdr_ui_params(float& ui_nits, bool& gamma22) {
    if (g_hdr == HdrOutput::Off) return 0;
    const HdrLevels l = hdr_levels();
    ui_nits = l.ui_nits, gamma22 = l.gamma != 0;
    return g_hdr == HdrOutput::ScRgb ? 1 : 2;
}

int hdr_tonemap_mode() { return int(hdr_levels().tonemap); }  // 0 vanilla, 1 HDR, 2 RenoDRT (nvn_screenshot.cpp)

bool present_pass_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_PASS");
#if defined(__ANDROID__)
        return !e || e[0] != '0';
#else
        // FSR, FXAA and HDR are passes of this one
        return (e && e[0] == '1') || fsr_on() || fxaa_on() || hdr_requested() != HdrOutput::Off;
#endif
    }();
    return on;
}

void present_pass_release() { destroy_targets(); }  // before the swapchain whose images they view is destroyed
bool present_pass_failed() { return g.failed; }

bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform) {
    if (!present_pass_wanted() || g.failed) return false;
    destroy_targets();
    if (g.format != format && g.pipeline) {  // a new swapchain format: a new render pass and pipeline
        VkDevice d = vk_device();
        p_vkDestroyPipeline(d, g.pipeline, nullptr);
        p_vkDestroyRenderPass(d, g.render_pass, nullptr);
        g.pipeline = VK_NULL_HANDLE, g.render_pass = VK_NULL_HANDLE;
        if (g.easu) p_vkDestroyPipeline(d, g.easu, nullptr);
        if (g.rcas) p_vkDestroyPipeline(d, g.rcas, nullptr);
        if (g.easu_pass) p_vkDestroyRenderPass(d, g.easu_pass, nullptr);
        g.easu = g.rcas = VK_NULL_HANDLE, g.easu_pass = VK_NULL_HANDLE;
        if (g.fxaa) p_vkDestroyPipeline(d, g.fxaa, nullptr);
        g.fxaa = VK_NULL_HANDLE;
    }
    if (!g.pipeline && !make_pipeline(format)) {
        log("nvn/vk: present pass unavailable: frames are blitted");
        g.failed = true;
        return false;
    }
    VkDevice d = vk_device();
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        p_vkCreateImageView(d, &vi, nullptr, &v);
        g.views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.render_pass, fi.attachmentCount = 1, fi.pAttachments = &v;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateFramebuffer(d, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        g.framebuffers.push_back(fb);
    }
    g.extent = extent, g.transform = transform;
    return true;
}

bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index) {
    if (!g.pipeline || index >= g.framebuffers.size() || !src.view || src.levels != 1 || src.layers != 1 ||
        src.view_type != VK_IMAGE_VIEW_TYPE_2D || !(src.aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The set and the circles' uniform data come from this submit's slot (descriptor pool and upload ring, reset when
    // the slot is retired): nothing a frame still in flight reads is written. Per swapchain image they were not safe
    // (MAILBOX can hand an image back before the frame that last drew into it finished; review of DEBUG-2.md #88).
    VkDescriptorSet set = descriptor_set(g.set_layout);
    Upload ubo = upload_alloc(kOverlayBytes, 256);
    std::memset(ubo.ptr, 0, kOverlayBytes);
    VkDescriptorImageInfo ii{g.sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{ubo.buffer, ubo.offset, kOverlayBytes};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = set, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);

    Push p{};
    // Host uv -> upright uv, undoing the clockwise turn the display applies (as vk_present.cpp).
    static const float kRot[4][6] = {{1, 0, 0, 1, 0, 0}, {0, -1, 1, 0, 0, 1}, {-1, 0, 0, -1, 1, 1}, {0, 1, -1, 0, 1, 0}};
    int r = g.transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? 1
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                     : 0;
    std::memcpy(p.rot, kRot[r], 4 * sizeof(float));
    p.off[0] = kRot[r][4], p.off[1] = kRot[r][5];
    // The part of the texture shown (nvnWindowSetCrop), fit into the upright target keeping its aspect.
    float cx = 0, cy = 0, cw = float(src.width), ch = float(src.height);
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(src.width) &&
        crop[1] + crop[3] <= int32_t(src.height))
        cx = float(crop[0]), cy = float(crop[1]), cw = float(crop[2]), ch = float(crop[3]);
    p.crop[0] = cx / float(src.width), p.crop[1] = cy / float(src.height);
    p.crop[2] = cw / float(src.width), p.crop[3] = ch / float(src.height);
    bool sideways = r == 1 || r == 3;
    float tw = float(sideways ? g.extent.height : g.extent.width), th = float(sideways ? g.extent.width : g.extent.height);
    float scale = std::min(tw / cw, th / ch), bw = cw * scale / tw, bh = ch * scale / th;
    p.box[0] = (1 - bw) / 2, p.box[1] = (1 - bh) / 2, p.box[2] = bw, p.box[3] = bh;
    if (r == 0) plat::set_picture_box(p.box[0], p.box[1], p.box[2], p.box[3]);  // for mouse clicks (hle_hid.cpp)
    // The on-screen touch controls (touch_layout.cpp, Android), in upright uv; none on PC.
    plat::OverlayShape shapes[kOverlayMax];
    size_t count = plat::touch_overlay(shapes, kOverlayMax);
    auto* dst = reinterpret_cast<float*>(ubo.ptr);
    for (size_t k = 0; k < count; k++)
        dst[4 * k] = shapes[k].x, dst[4 * k + 1] = shapes[k].y, dst[4 * k + 2] = shapes[k].r, dst[4 * k + 3] = shapes[k].alpha;
    p.off[2] = float(count), p.off[3] = tw / th;
    // The 8x4 grid of present.frag: a bit per circle (with its rim) in every cell its bounding box reaches.
    auto* cells = reinterpret_cast<uint32_t*>(dst + 4 * kOverlayMax);
    for (size_t k = 0; k < count; k++) {
        const float ry = shapes[k].r + 0.005f, rx = ry / std::max(0.01f, tw / th);
        const int x0 = std::clamp(int((shapes[k].x - rx) * 8.0f), 0, 7), x1 = std::clamp(int((shapes[k].x + rx) * 8.0f), 0, 7);
        const int y0 = std::clamp(int((shapes[k].y - ry) * 4.0f), 0, 3), y1 = std::clamp(int((shapes[k].y + ry) * 4.0f), 0, 3);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) cells[y * 8 + x] |= 1u << k;
    }

    // FSR 1: upscale the shown part into `inter` (the picture's size on the target), then sharpen it in the main pass.
    // FSR and FXAA turned on in the settings while the game runs: their pipelines are made now.
    const bool hdr = g_hdr != HdrOutput::Off;
    const bool fsr = !hdr && fsr_on(), fxaa = !hdr && !fsr && fxaa_on();
    if (hdr) {
        const HdrLevels l = hdr_levels();
        p.hdr[0] = l.ui_nits, p.hdr[1] = l.peak_nits, p.hdr[2] = l.gamma, p.hdr[3] = float(g_hdr);
    }
    if (fsr && !g.easu && !g.fsr_failed && !(g.fsr_failed = !make_fsr())) g.fsr_logged = false;
    if (fxaa && !g.fxaa && !g.fxaa_failed) g.fxaa_failed = !build_pipeline(g.render_pass, kFxaa, sizeof kFxaa, &g.fxaa);
    uint32_t out_w = uint32_t(bw * tw + 0.5f), out_h = uint32_t(bh * th + 0.5f);
    VkDescriptorSet rcas_set = VK_NULL_HANDLE;
    if (fsr && g.easu && g.rcas && r == 0 && out_w > cw * 1.01f && out_h > ch * 1.01f && fsr_inter(out_w, out_h)) {
        if (!g.fsr_logged)
            log("nvn/vk: FSR 1: %.0fx%.0f -> %ux%u (sharpness %.2f stops)", cw, ch, out_w, out_h, fsr_sharpness()),
                g.fsr_logged = true;
        struct { float a[4], b[4], c[4], d[4]; } e{};
        e.a[0] = cw / float(out_w), e.a[1] = ch / float(out_h);
        e.a[2] = 0.5f * e.a[0] - 0.5f, e.a[3] = 0.5f * e.a[1] - 0.5f;
        e.b[0] = cx, e.b[1] = cy, e.b[2] = cw, e.b[3] = ch;
        VkRenderPassBeginInfo eb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        eb.renderPass = g.easu_pass, eb.framebuffer = g.inter_fb, eb.renderArea = {{0, 0}, {out_w, out_h}};
        p_vkCmdBeginRenderPass(cb, &eb, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport ev{0, 0, float(out_w), float(out_h), 0, 1};
        VkRect2D es{{0, 0}, {out_w, out_h}};
        p_vkCmdSetViewport(cb, 0, 1, &ev);
        p_vkCmdSetScissor(cb, 0, 1, &es);
        p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.easu);
        p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
        p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof e, &e);
        p_vkCmdDraw(cb, 3, 1, 0, 0);
        p_vkCmdEndRenderPass(cb);
        rcas_set = descriptor_set(g.set_layout);
        VkDescriptorImageInfo ri{g.sampler, g.inter_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet rw[2] = {w[0], w[1]};
        rw[0].dstSet = rcas_set, rw[0].pImageInfo = &ri;
        rw[1].dstSet = rcas_set;
        p_vkUpdateDescriptorSets(vk_device(), 2, rw, 0, nullptr);
        p.crop[0] = float(out_w), p.crop[1] = float(out_h), p.crop[2] = std::exp2(-fsr_sharpness()), p.crop[3] = 0;
    }

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.render_pass, rb.framebuffer = g.framebuffers[index], rb.renderArea = {{0, 0}, g.extent};
    p_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    VkRect2D sc{{0, 0}, g.extent};
    p_vkCmdSetViewport(cb, 0, 1, &v);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    if (!rcas_set && fxaa && g.fxaa && !g.fxaa_logged)
        log("nvn/vk: FXAA: %.0fx%.0f -> %ux%u", cw, ch, out_w, out_h), g.fxaa_logged = true;
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, rcas_set ? g.rcas : fxaa && g.fxaa ? g.fxaa : g.pipeline);
    VkDescriptorSet main_set = rcas_set ? rcas_set : set;
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &main_set, 0, nullptr);
    p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    return true;
}

// NX_PRESENT_TEST=<w>x<h>[:<90|180|270>] (headless, with NX_DUMP_FRAMES): the pass draws into an offscreen image
// of that size and transform, and that image is dumped in place of the window texture. It checks the rotation, the
// fit and the alpha without a phone (a Galaxy S23 held sideways: 1080x2340:90).
GpuImage* present_pass_test_target() {
    static GpuImage img;
    static bool tried = false;
    if (tried) return img.image ? &img : nullptr;
    tried = true;
    const char* e = std::getenv("NX_PRESENT_TEST");
    uint32_t w = 0, h = 0, deg = 0;
    if (!e || std::sscanf(e, "%ux%u:%u", &w, &h, &deg) < 2 || !w || !h) return nullptr;
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8G8B8A8_UNORM, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &img.image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    p_vkAllocateMemory(d, &ai, nullptr, &img.memory);
    p_vkBindImageMemory(d, img.image, img.memory, 0);
    img.format = VK_FORMAT_R8G8B8A8_UNORM, img.width = w, img.height = h;
    VkSurfaceTransformFlagBitsKHR t = deg == 90    ? VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR
                                      : deg == 180 ? VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR
                                      : deg == 270 ? VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR
                                                   : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (!present_pass_targets(img.format, {img.image}, {w, h}, t)) {
        log("nvn/vk: present test: the pass is off (NX_PRESENT_PASS=1)");
        return nullptr;
    }
    log("nvn/vk: present test: %ux%u, turned %u degrees", w, h, deg);
    return &img;
}

}  // namespace nx::nvn
