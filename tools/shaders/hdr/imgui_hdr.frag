// The settings screen (Dear ImGui) on an HDR swapchain: ImGui's fragment shader (backends/vulkan/glsl_shader.frag, same
// interface) with its sRGB-encoded colours decoded and put at the UI brightness. scRGB: linear BT.709, 1 = 80 nits.
// HDR10: BT.2020 PQ (ImGui then blends in PQ: soft edges only). runtime/src/ui/ui.cpp swaps it in for ImGui's when the
// swapchain is HDR, with the constants' defaults patched (ImGui passes no specialization).
#version 450 core
layout(location = 0) out vec4 fColor;
layout(set = 0, binding = 0) uniform sampler2D sTexture;
layout(location = 0) in struct { vec4 Color; vec2 UV; } In;

layout(constant_id = 0) const float kNits = 203.0;  // UI brightness
layout(constant_id = 1) const int kPq = 0;          // 0 scRGB, 1 HDR10
layout(constant_id = 2) const int kGamma22 = 1;     // decode as 2.2 (video.hdr_gamma), else sRGB

vec3 decode(vec3 c) {
    c = max(c, vec3(0.0));
    if (kGamma22 != 0) return pow(c, vec3(2.2));
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), greaterThan(c, vec3(0.04045)));
}

vec3 pq(vec3 nits) {
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    vec3 y = pow(clamp(nits / 10000.0, 0.0, 1.0), vec3(m1));
    return pow((c1 + c2 * y) / (1.0 + c3 * y), vec3(m2));
}

void main() {
    vec4 c = In.Color * texture(sTexture, In.UV.st);
    vec3 l = decode(c.rgb);
    if (kPq != 0) {
        const mat3 bt709_to_bt2020 = mat3(0.6274040, 0.0690970, 0.0163916, 0.3292820, 0.9195400, 0.0880132,
                                          0.0433136, 0.0113612, 0.8955950);
        fColor = vec4(pq(bt709_to_bt2020 * l * kNits), c.a);
    } else {
        fColor = vec4(l * (kNits / 80.0), c.a);
    }
}
