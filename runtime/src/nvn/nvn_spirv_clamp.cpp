// HDR: a UNORM colour target clamps what a fragment shader writes to 0..1 (and NaN to 0); the RGBA16F the game's RGB10A2
// screen target becomes with HDR (nvn_image.cpp) does not. The UI and the transitions' overlays drawn over the tonemap's
// picture write past 1 (a white flash with a gain of several) and relied on it: as floats they flashed at the display's
// peak. Their fragment module gets the clamp back here: before each return of the entry point, every float colour
// output is loaded, NClamp'ed to 0..1 (NaN gives 0) and stored again. The tonemap replacements are left alone.
#include <cstring>

#include "nvn_pipeline.h"

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpTypeFloat = 22,
    OpTypeVector = 23, OpTypePointer = 32, OpConstant = 43, OpConstantComposite = 44, OpFunction = 54,
    OpFunctionEnd = 56, OpVariable = 59, OpLoad = 61, OpStore = 62, OpDecorate = 71, OpReturn = 253,
    DecorationBuiltIn = 11, StorageClassOutput = 3, GlslNClamp = 81,
};

}  // namespace

bool patch_clamp_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3], entry = 0, f32 = 0, glsl = 0;
    size_t memory_model = in.size(), first_function = in.size();
    std::vector<uint32_t> builtins;
    std::vector<std::pair<uint32_t, uint32_t>> vectors;   // float vector type -> components
    std::vector<std::pair<uint32_t, uint32_t>> pointers;  // Output pointer type -> pointee
    struct Output {
        uint32_t var, type, comps;
    };
    std::vector<Output> outputs;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpExtInstImport && n.len >= 3 && std::strncmp(reinterpret_cast<const char*>(&w[2]), "GLSL.std.450", 12) == 0)
            glsl = w[1];
        else if (n.op == OpMemoryModel && memory_model == in.size()) memory_model = n.at;
        else if (n.op == OpEntryPoint && n.len >= 3 && !entry) entry = w[2];
        else if (n.op == OpDecorate && n.len >= 3 && w[2] == DecorationBuiltIn) builtins.push_back(w[1]);
        else if (n.op == OpTypeFloat && n.len == 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && f32 && w[2] == f32) vectors.push_back({w[1], w[3]});
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageClassOutput) pointers.push_back({w[1], w[3]});
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageClassOutput) {
            bool builtin = false;
            for (uint32_t id : builtins) builtin |= id == w[2];
            if (builtin) continue;  // gl_FragDepth and the like: not colours
            for (auto& [ptr, pointee] : pointers) {
                if (ptr != w[1]) continue;
                if (pointee == f32) outputs.push_back({w[2], pointee, 1});
                for (auto& [vec, comps] : vectors)
                    if (vec == pointee) outputs.push_back({w[2], pointee, comps});
            }
        } else if (n.op == OpFunction && first_function == in.size()) first_function = n.at;
    }
    if (!entry || !f32 || outputs.empty() || first_function == in.size() || memory_model == in.size()) return false;

    // 0 and 1 for each output type (scalar constants, then composites of them).
    std::vector<uint32_t> types;
    const uint32_t zero = bound++, one = bound++;
    const float fz = 0.0f, fo = 1.0f;
    uint32_t zb, ob;
    std::memcpy(&zb, &fz, 4), std::memcpy(&ob, &fo, 4);
    types.insert(types.end(), {(4u << 16) | OpConstant, f32, zero, zb, (4u << 16) | OpConstant, f32, one, ob});
    std::vector<std::pair<uint32_t, std::pair<uint32_t, uint32_t>>> bounds;  // type -> (zero, one)
    for (const Output& o : outputs) {
        bool known = o.comps == 1;
        for (auto& b : bounds) known |= b.first == o.type;
        if (known) continue;
        uint32_t z = bound++, w1 = bound++;
        types.push_back(((3 + o.comps) << 16) | OpConstantComposite), types.push_back(o.type), types.push_back(z);
        for (uint32_t c = 0; c < o.comps; c++) types.push_back(zero);
        types.push_back(((3 + o.comps) << 16) | OpConstantComposite), types.push_back(o.type), types.push_back(w1);
        for (uint32_t c = 0; c < o.comps; c++) types.push_back(one);
        bounds.push_back({o.type, {z, w1}});
    }
    auto bounds_of = [&](const Output& o) -> std::pair<uint32_t, uint32_t> {
        if (o.comps == 1) return {zero, one};
        for (auto& b : bounds)
            if (b.first == o.type) return b.second;
        return {zero, one};
    };
    std::vector<uint32_t> import;  // GLSL.std.450, when the module has none
    if (!glsl) {
        glsl = bound++;
        import = {(6u << 16) | OpExtInstImport, glsl, 0, 0, 0, 0};
        std::memcpy(&import[2], "GLSL.std.450\0\0\0", 16);
    }

    std::vector<uint32_t> body;
    bool in_entry = false;
    int patched = 0;
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunction) in_entry = w[2] == entry;
        else if (n.op == OpFunctionEnd) in_entry = false;
        if (in_entry && n.op == OpReturn) {
            for (const Output& o : outputs) {
                const uint32_t loaded = bound++, clamped = bound++;
                const auto [lo, hi] = bounds_of(o);
                body.insert(body.end(), {(4u << 16) | OpLoad, o.type, loaded, o.var});
                body.insert(body.end(), {(8u << 16) | OpExtInst, o.type, clamped, glsl, GlslNClamp, loaded, lo, hi});
                body.insert(body.end(), {(3u << 16) | OpStore, o.var, clamped});
            }
            patched++;
        }
        body.insert(body.end(), w, w + n.len);
    }
    if (!patched) return false;

    out.assign(in.begin(), in.begin() + 5);
    out[3] = bound;
    out.insert(out.end(), in.begin() + 5, in.begin() + memory_model);
    out.insert(out.end(), import.begin(), import.end());
    out.insert(out.end(), in.begin() + memory_model, in.begin() + first_function);
    out.insert(out.end(), types.begin(), types.end());
    out.insert(out.end(), body.begin(), body.end());
    return true;
}

}  // namespace nx::nvn
