#version 450
// The touch controls alone, drawn over a picture FSR's sharpening, FXAA or the HDR shader already put on the target:
// those shaders do not draw them, and the controls vanished on Android with any of them on. White (crop.x: 1 in SDR,
// the UI's white as the HDR target encodes it), with the circles' coverage as alpha, blended over the target (src
// alpha, 1 - src alpha): the same result as present.frag's mix() of each circle. Same set and push constants.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform Overlay {
    vec4 shape[32];
    uvec4 cell[8];
} ov;
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w)
    vec4 off;   // xy: rot offset; z: number of circles; w: target width / height (upright)
    vec4 box;
    vec4 crop;  // x: the target's white
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    ivec2 g = clamp(ivec2(up * vec2(8.0, 4.0)), ivec2(0), ivec2(7, 3));
    int ci = g.y * 8 + g.x;
    uint m = p.off.z > 0.0 ? ov.cell[ci >> 2][ci & 3] : 0u;
    if (m == 0u) discard;
    float a = 0.0;
    while (m != 0u) {
        int i = findLSB(m);
        m &= m - 1u;
        vec4 s = ov.shape[i];
        float d = length(vec2((up.x - s.x) * p.off.w, up.y - s.y));
        if (d < s.z)
            a += (1.0 - a) * s.w;
        else if (d < s.z + 0.005)
            a += (1.0 - a) * min(1.0, s.w * 3.0);
    }
    if (a <= 0.0) discard;
    color = vec4(vec3(p.crop.x), a);
}
