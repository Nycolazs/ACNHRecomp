#version 450
// Host pixel -> the picture: `rot` turns host uv into upright uv (the swapchain's preTransform undone), `box` is where
// the picture sits in the upright target (uv, fit with bars), `crop` the part of the texture shown. Alpha is 1.
// Over it, the touch controls: `n` circles (x, y, radius in target heights, opacity) in upright uv. `cell` is a coarse
// 8x4 grid over the upright target, a bit per circle that reaches the cell: a pixel tests only those (every pixel
// testing every circle was ~8 ms a frame on an Adreno 650 at 2400x1080).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform Overlay {
    vec4 shape[32];
    uvec4 cell[8];
    uvec4 label[8];  // a word per circle: its letters (label_at)
} ov;
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w)
    vec4 off;   // xy: rot offset; z: number of circles; w: target width / height (upright)
    vec4 box;   // x0 y0 w h
    vec4 crop;  // u0 v0 du dv
} p;
// The button's letters (#86): at most two, 5x7 pixels each, centred in its circle. `code` is (first + 1) | (second + 1)
// << 8 into "ABXYRLZ+-" (platform_android_input.cpp), 0: none; `l` the position from the circle's centre in radii (y down).
const uint kGlyphLo[9] = uint[9](0x231fc62eu, 0x231f463eu, 0x22a22a31u, 0x08422a31u, 0x254f463eu, 0x21084210u, 0x2082083fu, 0x084f9080u, 0x000f8000u);  // rows 0-5, 5 bits each
const uint kGlyphHi[9] = uint[9](0x11u, 0x1eu, 0x11u, 0x04u, 0x11u, 0x1fu, 0x1fu, 0x00u, 0x00u);  // row 6
bool label_at(uint code, vec2 l) {
    uint g0 = code & 255u, g1 = (code >> 8) & 255u;
    if (g0 == 0u || g0 > 9u || g1 > 9u) return false;
    float n = g1 != 0u ? 2.0 : 1.0;
    float px = g1 != 0u ? 0.13 : 0.16;              // a font pixel, in radii: the letters stay well inside the circle
    vec2 q = l / px + vec2(6.0 * n - 1.0, 7.0) * 0.5;  // font pixels from the text's top left (a pixel between letters)
    if (q.x < 0.0 || q.y < 0.0 || q.x >= 6.0 * n - 1.0 || q.y >= 7.0) return false;
    int col = int(q.x), row = int(q.y), which = col / 6, c = col - which * 6;
    if (c > 4) return false;
    uint g = (which == 0 ? g0 : g1) - 1u;
    uint bits = row < 6 ? (kGlyphLo[g] >> uint(row * 5)) & 31u : kGlyphHi[g];
    return ((bits >> uint(4 - c)) & 1u) != 0u;
}
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 b = (up - p.box.xy) / p.box.zw;
    vec3 c = vec3(0.0);
    if (all(greaterThanEqual(b, vec2(0.0))) && all(lessThanEqual(b, vec2(1.0))))
        c = texture(src, p.crop.xy + b * p.crop.zw).rgb;
    if (p.off.z > 0.0) {
        ivec2 g = clamp(ivec2(up * vec2(8.0, 4.0)), ivec2(0), ivec2(7, 3));
        int ci = g.y * 8 + g.x;
        uint m = ov.cell[ci >> 2][ci & 3];
        while (m != 0u) {
            int i = findLSB(m);
            m &= m - 1u;
            vec4 s = ov.shape[i];
            float d = length(vec2((up.x - s.x) * p.off.w, up.y - s.y));
            if (d < s.z) {
                vec2 l = vec2((up.x - s.x) * p.off.w, up.y - s.y) / s.z;
                c = mix(c, vec3(1.0), label_at(ov.label[i >> 2][i & 3], l) ? min(1.0, s.w * 3.0) : s.w);
            }
            else if (d < s.z + 0.005)
                c = mix(c, vec3(1.0), min(1.0, s.w * 3.0));
        }
    }
    color = vec4(c, 1.0);
}
