// Object motion vectors for temporal anti-aliasing (nvn_temporal.cpp): the skinned models' pipelines (characters,
// animals, anything with a bone palette: a draw whose vertex shader reads an integer attribute) tell where each pixel
// was a frame ago, which the camera alone cannot (the player's head bob, a villager's walk).
//
// Vertex shader, before each return of the entry point: gl_Position goes to a history buffer at this frame's slot for
// the vertex, and the same vertex's position of the last frame is read from the last frame's slot; both go to the
// fragment shader (locations 30 and 31, which no ACNH shader uses: its highest is 15). Fragment shader, before each
// return: the two positions, without their frames' jitter, give the motion in UV (the last frame's minus this one's,
// as the camera's motion vectors); it goes to a per-pixel buffer with the fragment's depth in the high half of one
// atomicMin, so the nearest fragment of a skinned model wins; the merge (motion.comp) takes it where that depth is the
// scene's own (nothing else drawn over it), the camera's motion elsewhere.
//
// Everything per draw comes in push constants, the two buffers by address (buffer device address), so the game's
// descriptor sets are untouched (MotionPush, nvn_temporal.cpp). Every pipeline has the patch when object motion is on
// (any draw that moves may use it): what a draw does not use is branched over, so the draws without (most of them)
// cost a few push constant reads. The modules get the PhysicalStorageBuffer64
// addressing model, Int64 and Int64Atomics (the fragment's packed depth and motion).
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <cstdint>

namespace nx::nvn {

namespace {

enum : uint32_t {
    OpExtension = 10, OpExtInstImport = 11, OpExtInst = 12, OpMemoryModel = 14, OpEntryPoint = 15, OpCapability = 17,
    OpTypeBool = 20, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypeRuntimeArray = 29, OpTypeStruct = 30,
    OpTypeVoid = 19, OpTypeFunction = 33, OpConstant = 43, OpFunction = 54, OpFunctionEnd = 56, OpFunctionCall = 57,
    OpLabel = 248, OpPhi = 245, OpSelectionMerge = 247, OpBranch = 249, OpBranchConditional = 250, OpTypePointer = 32, OpVariable = 59, OpLoad = 61,
    OpStore = 62, OpAccessChain = 65, OpDecorate = 71, OpMemberDecorate = 72, OpCompositeConstruct = 80,
    OpCompositeExtract = 81, OpConvertFToU = 109, OpUConvert = 113, OpBitcast = 124, OpIAdd = 128, OpFSub = 131,
    OpISub = 130, OpIMul = 132, OpFMul = 133, OpFDiv = 136, OpLogicalAnd = 167, OpSelect = 169, OpINotEqual = 171,
    OpULessThan = 176, OpFOrdGreaterThan = 186, OpShiftLeftLogical = 196, OpBitwiseOr = 197, OpBitwiseAnd = 199,
    OpNot = 200,
    OpAtomicUMin = 237, OpReturn = 253,
    DecorationBlock = 2, DecorationArrayStride = 6, DecorationBuiltIn = 11, DecorationLocation = 30,
    DecorationOffset = 35,
    BuiltInPosition = 0, BuiltInFragCoord = 15, BuiltInVertexIndex = 42, BuiltInInstanceIndex = 43,
    StorageInput = 1, StorageOutput = 3, StoragePushConstant = 9, StoragePhysicalStorageBuffer = 5349,
    CapabilityInt64 = 11, CapabilityInt64Atomics = 12, CapabilityPhysicalStorageBufferAddresses = 5347,
    AddressingPhysicalStorageBuffer64 = 5348, MemoryAccessAligned = 2, GlslPackHalf2x16 = 58, GlslFMax = 40,
    GlslFClamp = 43,
    ModelVertex = 0, ModelFragment = 4,
};
constexpr uint32_t kCurLocation = 30, kPrevLocation = 31;

uint32_t word_op(uint32_t len, uint32_t op) { return len << 16 | op; }

// A module being patched: its instructions, the types and constants it has (reused: a second OpTypeInt 32 0 is
// invalid), and what the patch adds to each section.
struct Module {
    const std::vector<uint32_t>& in;
    struct Inst {
        size_t at;
        uint32_t op, len;
    };
    std::vector<Inst> insts;
    uint32_t bound = 0, model = 0, entry = 0, glsl = 0;
    size_t entry_at = 0, memory_model_at = 0, first_function = 0, annotations_end = 0, last_capability = 0;
    std::set<uint32_t> capabilities;
    std::map<std::vector<uint32_t>, uint32_t> types;   // {op, operands...} -> id
    std::map<std::vector<uint32_t>, uint32_t> consts;  // {type, value words...} -> id
    std::vector<uint32_t> caps, imports, decorations, globals, interface;
    explicit Module(const std::vector<uint32_t>& code) : in(code) {}

    bool parse() {
        if (in.size() < 5) return false;
        bound = in[3];
        first_function = in.size();
        for (size_t i = 5; i < in.size();) {
            uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
            if (!len || i + len > in.size()) return false;
            insts.push_back({i, op, len});
            const uint32_t* w = &in[i];
            if (op == OpCapability) capabilities.insert(w[1]), last_capability = insts.size();
            else if (op == OpExtInstImport && std::string(reinterpret_cast<const char*>(w + 2)) == "GLSL.std.450") glsl = w[1];
            else if (op == OpMemoryModel) memory_model_at = i;
            else if (op == OpEntryPoint && !entry) model = w[1], entry = w[2], entry_at = i;
            else if (op >= OpTypeBool - 1 && op <= 39 && op != OpTypeStruct && len >= 2) {  // OpTypeVoid .. OpTypePipe
                std::vector<uint32_t> k{op};
                for (uint32_t j = 2; j < len; j++) k.push_back(w[j]);
                types.emplace(k, w[1]);
            } else if (op == OpConstant && len >= 4) {
                std::vector<uint32_t> k{w[1]};
                for (uint32_t j = 3; j < len; j++) k.push_back(w[j]);
                consts.emplace(k, w[2]);
            } else if (op == OpFunction && first_function == in.size()) {
                first_function = i;
            }
            const bool header = op == OpCapability || op == OpExtension || op == OpExtInstImport || op == OpMemoryModel ||
                                op == OpEntryPoint || op == 16 || (op >= 2 && op <= 8) || op == 330 || op == OpDecorate ||
                                op == OpMemberDecorate || op == 73 || op == 74 || op == 332 || op == 5632 || op == 5633 ||
                                op == 317;
            if (header) annotations_end = insts.size();
            i += len;
        }
        return entry && memory_model_at && first_function < in.size();
    }
    uint32_t id() { return bound++; }
    uint32_t type(uint32_t op, std::vector<uint32_t> operands) {
        std::vector<uint32_t> k{op};
        k.insert(k.end(), operands.begin(), operands.end());
        if (auto it = types.find(k); it != types.end()) return it->second;
        uint32_t r = id();
        globals.push_back(word_op(uint32_t(2 + operands.size()), op));
        globals.push_back(r);
        globals.insert(globals.end(), operands.begin(), operands.end());
        types.emplace(k, r);
        return r;
    }
    uint32_t constant(uint32_t t, std::vector<uint32_t> value) {
        std::vector<uint32_t> k{t};
        k.insert(k.end(), value.begin(), value.end());
        if (auto it = consts.find(k); it != consts.end()) return it->second;
        uint32_t r = id();
        globals.insert(globals.end(), {word_op(uint32_t(3 + value.size()), OpConstant), t, r});
        globals.insert(globals.end(), value.begin(), value.end());
        consts.emplace(k, r);
        return r;
    }
    uint32_t variable(uint32_t ptr_type, uint32_t storage) {
        uint32_t r = id();
        globals.insert(globals.end(), {word_op(4, OpVariable), ptr_type, r, storage});
        return r;
    }
    void decorate(std::vector<uint32_t> words) {
        decorations.push_back(word_op(uint32_t(words.size() + 1), OpDecorate));
        decorations.insert(decorations.end(), words.begin(), words.end());
    }
    void member_decorate(std::vector<uint32_t> words) {
        decorations.push_back(word_op(uint32_t(words.size() + 1), OpMemberDecorate));
        decorations.insert(decorations.end(), words.begin(), words.end());
    }
    void capability(uint32_t c) {
        if (capabilities.insert(c).second) caps.insert(caps.end(), {word_op(2, OpCapability), c});
    }
    // A built-in Input variable the shader has (decorated BuiltIn b), or a new one of `ptr_type`.
    uint32_t builtin_input(uint32_t b, uint32_t ptr_type) {
        for (const Inst& n : insts) {
            const uint32_t* w = &in[n.at];
            if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == b) return w[1];
        }
        uint32_t v = variable(ptr_type, StorageInput);
        decorate({v, DecorationBuiltIn, b});
        interface.push_back(v);
        return v;
    }
    // The patched module: `code` (its instructions; result ids from id()) in a function of its own, called before each
    // return of the entry point (the game's main returns from more than one place).
    bool emit(const std::vector<uint32_t>& code, std::vector<uint32_t>& out) {
        const uint32_t void_t = type(OpTypeVoid, {}), fn_t = type(OpTypeFunction, {void_t}), fn = id(), label = id();
        out.assign(in.begin(), in.begin() + 5);
        static const char ext[] = "SPV_KHR_physical_storage_buffer";
        std::vector<uint32_t> ext_words((sizeof ext + 3) / 4, 0);
        std::memcpy(ext_words.data(), ext, sizeof ext);
        uint32_t function = 0;
        int returns = 0;
        bool ext_done = false;
        for (size_t k = 0; k < insts.size(); k++) {
            const Inst& n = insts[k];
            const uint32_t* w = &in[n.at];
            if (n.at == first_function) out.insert(out.end(), globals.begin(), globals.end());
            if (!ext_done && n.op != OpCapability) {  // after the capabilities
                out.insert(out.end(), caps.begin(), caps.end());
                out.push_back(word_op(uint32_t(1 + ext_words.size()), OpExtension));
                out.insert(out.end(), ext_words.begin(), ext_words.end());
                ext_done = true;
            }
            if (k == annotations_end) out.insert(out.end(), decorations.begin(), decorations.end());
            if (n.op == OpFunction) function = w[2];
            if (n.op == OpReturn && function == entry) out.insert(out.end(), {word_op(4, OpFunctionCall), void_t, id(), fn}), returns++;
            if (n.op == OpFunctionEnd) function = 0;
            if (n.op == OpMemoryModel) {
                out.insert(out.end(), imports.begin(), imports.end());  // after every extension (the shader's own too)
                out.insert(out.end(), {w[0], AddressingPhysicalStorageBuffer64, w[2]});
                continue;
            }
            if (n.op == OpEntryPoint && n.at == entry_at) {
                out.push_back(word_op(uint32_t(n.len + interface.size()), OpEntryPoint));
                out.insert(out.end(), w + 1, w + n.len);
                out.insert(out.end(), interface.begin(), interface.end());
                continue;
            }
            out.insert(out.end(), w, w + n.len);
        }
        if (!returns) return false;
        out.insert(out.end(), {word_op(5, OpFunction), void_t, fn, 0, fn_t, word_op(2, OpLabel), label});
        out.insert(out.end(), code.begin(), code.end());
        out.insert(out.end(), {word_op(1, OpReturn), word_op(1, OpFunctionEnd)});
        out[3] = bound;
        return true;
    }
};

// The push constant block both stages declare (MotionPush): two buffer addresses, eight words, two vec2, the texture
// LOD bias (patch_mip_bias_push).
struct Push {
    uint32_t var = 0, ptr_hist_t = 0, ptr_pix_t = 0, u32 = 0, v2f = 0, pc_u32 = 0, pc_v2f = 0, pc_hist = 0, pc_pix = 0;
};
Push declare_push(Module& m, uint32_t f32) {
    Push p;
    p.u32 = m.type(OpTypeInt, {32, 0});
    const uint32_t u64 = m.type(OpTypeInt, {64, 0});
    const uint32_t v4f = m.type(OpTypeVector, {f32, 4});
    p.v2f = m.type(OpTypeVector, {f32, 2});
    // History: vec4[]; pixels: uint64[] (structs: a fresh one each, decorated here).
    const uint32_t hist_arr = m.id(), hist_s = m.id(), pix_arr = m.id(), pix_s = m.id();
    m.globals.insert(m.globals.end(), {word_op(3, OpTypeRuntimeArray), hist_arr, v4f, word_op(3, OpTypeStruct), hist_s, hist_arr,
                                       word_op(3, OpTypeRuntimeArray), pix_arr, u64, word_op(3, OpTypeStruct), pix_s, pix_arr});
    m.decorate({hist_arr, DecorationArrayStride, 16});
    m.decorate({hist_s, DecorationBlock});
    m.member_decorate({hist_s, 0, DecorationOffset, 0});
    m.decorate({pix_arr, DecorationArrayStride, 8});
    m.decorate({pix_s, DecorationBlock});
    m.member_decorate({pix_s, 0, DecorationOffset, 0});
    p.ptr_hist_t = m.type(OpTypePointer, {StoragePhysicalStorageBuffer, hist_s});
    p.ptr_pix_t = m.type(OpTypePointer, {StoragePhysicalStorageBuffer, pix_s});
    const uint32_t block = m.id();
    m.globals.insert(m.globals.end(), {word_op(15, OpTypeStruct), block, p.ptr_hist_t, p.ptr_pix_t, p.u32, p.u32, p.u32, p.u32,
                                       p.u32, p.u32, p.u32, p.u32, p.v2f, p.v2f, f32});
    m.decorate({block, DecorationBlock});
    const uint32_t offsets[13] = {0, 8, 16, 20, 24, 28, 32, 36, 40, 44, 48, 56, 64};
    for (uint32_t k = 0; k < 13; k++) m.member_decorate({block, k, DecorationOffset, offsets[k]});
    p.var = m.variable(m.type(OpTypePointer, {StoragePushConstant, block}), StoragePushConstant);
    p.pc_u32 = m.type(OpTypePointer, {StoragePushConstant, p.u32});
    p.pc_v2f = m.type(OpTypePointer, {StoragePushConstant, p.v2f});
    p.pc_hist = m.type(OpTypePointer, {StoragePushConstant, p.ptr_hist_t});
    p.pc_pix = m.type(OpTypePointer, {StoragePushConstant, p.ptr_pix_t});
    m.capability(CapabilityPhysicalStorageBufferAddresses);
    m.capability(CapabilityInt64);
    return p;
}

// Instruction helpers for the inserted code.
struct Code {
    Module& m;
    std::vector<uint32_t> w;
    uint32_t op(uint32_t o, uint32_t type, std::vector<uint32_t> args) {
        uint32_t r = m.id();
        w.push_back(word_op(uint32_t(3 + args.size()), o));
        w.push_back(type), w.push_back(r);
        w.insert(w.end(), args.begin(), args.end());
        return r;
    }
    void raw(std::vector<uint32_t> words) { w.insert(w.end(), words.begin(), words.end()); }
    // if (cond) { then() }: a selection merging into a new block, which the code goes on in. Returns the then-block.
    template <class F>
    uint32_t when(uint32_t cond, F&& then) {
        const uint32_t body = m.id(), merge = m.id();
        raw({word_op(3, OpSelectionMerge), merge, 0, word_op(4, OpBranchConditional), cond, body, merge});
        raw({word_op(2, OpLabel), body});
        then();
        raw({word_op(2, OpBranch), merge, word_op(2, OpLabel), merge});
        return body;
    }
    uint32_t load(uint32_t type, uint32_t ptr) { return op(OpLoad, type, {ptr}); }
    uint32_t member(const Push& p, uint32_t ptr_t, uint32_t type, uint32_t index) {
        return load(type, op(OpAccessChain, ptr_t, {p.var, m.constant(p.u32, {index})}));
    }
};

}  // namespace

bool patch_motion_vertex(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    Module m(in);
    if (!m.parse() || m.model != ModelVertex) return false;
    // gl_Position: a gl_PerVertex member or a variable decorated Position (as nvn_spirv_clip.cpp finds it).
    uint32_t block = 0, member = 0, position_var = 0;
    std::map<uint32_t, uint32_t> out_ptr;  // Output pointer type -> pointee
    for (const Module::Inst& n : m.insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpMemberDecorate && n.len >= 5 && w[3] == DecorationBuiltIn && w[4] == BuiltInPosition) block = w[1], member = w[2];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationBuiltIn && w[3] == BuiltInPosition) position_var = w[1];
        else if (n.op == OpTypePointer && n.len == 4 && w[2] == StorageOutput) out_ptr[w[1]] = w[3];
    }
    uint32_t pos_var = 0;
    for (const Module::Inst& n : m.insts) {
        const uint32_t* w = &in[n.at];
        if (n.op != OpVariable || n.len < 4 || w[3] != StorageOutput) continue;
        auto p = out_ptr.find(w[1]);
        if (p == out_ptr.end()) continue;
        if ((block && p->second == block) || (!block && w[2] == position_var)) pos_var = w[2];
    }
    if (!pos_var) return false;
    const uint32_t f32 = m.type(OpTypeFloat, {32});
    const uint32_t v4f = m.type(OpTypeVector, {f32, 4});
    const uint32_t i32 = m.type(OpTypeInt, {32, 1});
    const uint32_t boolean = m.type(OpTypeBool, {});
    const uint32_t bv4 = m.type(OpTypeVector, {boolean, 4});
    Push p = declare_push(m, f32);
    const uint32_t out_v4 = m.type(OpTypePointer, {StorageOutput, v4f});
    const uint32_t in_i32 = m.type(OpTypePointer, {StorageInput, i32});
    const uint32_t psb_v4 = m.type(OpTypePointer, {StoragePhysicalStorageBuffer, v4f});
    const uint32_t vid_var = m.builtin_input(BuiltInVertexIndex, in_i32);
    const uint32_t iid_var = m.builtin_input(BuiltInInstanceIndex, in_i32);
    uint32_t cur_var = m.variable(out_v4, StorageOutput), prev_var = m.variable(out_v4, StorageOutput);
    m.decorate({cur_var, DecorationLocation, kCurLocation});
    m.decorate({prev_var, DecorationLocation, kPrevLocation});
    m.interface.push_back(cur_var), m.interface.push_back(prev_var);
    const uint32_t zero = m.constant(p.u32, {0});
    const uint32_t fzero = m.constant(f32, {0});
    const uint32_t vzero = m.id();
    m.globals.insert(m.globals.end(), {word_op(7, 44 /* OpConstantComposite */), v4f, vzero, fzero, fzero, fzero, fzero});

    Code c{m, {}};
    uint32_t pos_ptr = pos_var;
    if (block) pos_ptr = c.op(OpAccessChain, out_v4, {pos_var, m.constant(p.u32, {member})});
    const uint32_t pos = c.load(v4f, pos_ptr);
    const uint32_t hist = c.member(p, p.pc_hist, p.ptr_hist_t, 0);
    const uint32_t write_off = c.member(p, p.pc_u32, p.u32, 2), read_off = c.member(p, p.pc_u32, p.u32, 3);
    const uint32_t min_vertex = c.member(p, p.pc_u32, p.u32, 4), range = c.member(p, p.pc_u32, p.u32, 5);
    const uint32_t first_instance = c.member(p, p.pc_u32, p.u32, 6);
    const uint32_t vid = c.op(OpBitcast, p.u32, {c.load(i32, vid_var)}), iid = c.op(OpBitcast, p.u32, {c.load(i32, iid_var)});
    const uint32_t v = c.op(OpISub, p.u32, {vid, min_vertex}), inst = c.op(OpISub, p.u32, {iid, first_instance});
    const uint32_t in_range = c.op(OpULessThan, boolean, {v, range});
    const uint32_t slot = c.op(OpIAdd, p.u32, {v, c.op(OpIMul, p.u32, {inst, range})});
    const uint32_t wok = c.op(OpLogicalAnd, boolean, {c.op(OpINotEqual, boolean, {write_off, zero}), in_range});
    c.when(wok, [&] {  // this frame's position kept
        const uint32_t wptr = c.op(OpAccessChain, psb_v4, {hist, zero, c.op(OpIAdd, p.u32, {write_off, slot})});
        c.raw({word_op(5, OpStore), wptr, pos, MemoryAccessAligned, 16});
    });
    const uint32_t rok = c.op(OpLogicalAnd, boolean, {c.op(OpINotEqual, boolean, {read_off, zero}), in_range});
    const uint32_t before = m.id();  // the block the read branches from: a label of its own, for the phi
    c.raw({word_op(2, OpBranch), before, word_op(2, OpLabel), before});
    const uint32_t old = m.id();
    const uint32_t read = c.when(rok, [&] {  // the last frame's
        const uint32_t rptr = c.op(OpAccessChain, psb_v4, {hist, zero, c.op(OpIAdd, p.u32, {read_off, slot})});
        c.raw({word_op(6, OpLoad), v4f, old, rptr, MemoryAccessAligned, 16});
    });
    const uint32_t prev = c.op(OpPhi, v4f, {old, read, vzero, before});
    c.raw({word_op(3, OpStore), cur_var, pos});
    c.raw({word_op(3, OpStore), prev_var, prev});
    (void)bv4;
    return m.emit(c.w, out);
}

// The fragment shader's colour output 0 when it is a vec4 of floats (a variable decorated Location 0), or 0.
uint32_t output_location0(const Module& m, uint32_t v4f) {
    uint32_t var = 0;
    for (const Module::Inst& n : m.insts) {
        const uint32_t* w = &m.in[n.at];
        if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation && w[3] == 0) var = w[1];
    }
    if (!var) return 0;
    uint32_t ptr_type = 0;
    for (const Module::Inst& n : m.insts) {
        const uint32_t* w = &m.in[n.at];
        if (n.op == OpVariable && n.len >= 4 && w[2] == var && w[3] == StorageOutput) ptr_type = w[1];
    }
    for (const Module::Inst& n : m.insts) {
        const uint32_t* w = &m.in[n.at];
        if (n.op == OpTypePointer && n.len == 4 && w[1] == ptr_type) return w[2] == StorageOutput && w[3] == v4f ? var : 0;
    }
    return 0;
}

bool patch_motion_fragment(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    Module m(in);
    if (!m.parse() || m.model != ModelFragment) return false;
    const uint32_t f32 = m.type(OpTypeFloat, {32});
    const uint32_t v4f = m.type(OpTypeVector, {f32, 4});
    const uint32_t boolean = m.type(OpTypeBool, {});
    Push p = declare_push(m, f32);
    m.capability(CapabilityInt64Atomics);
    const uint32_t u64 = m.type(OpTypeInt, {64, 0});
    const uint32_t in_v4 = m.type(OpTypePointer, {StorageInput, v4f});
    const uint32_t psb_u64 = m.type(OpTypePointer, {StoragePhysicalStorageBuffer, u64});
    const uint32_t frag = m.builtin_input(BuiltInFragCoord, in_v4);
    uint32_t cur_var = m.variable(in_v4, StorageInput), prev_var = m.variable(in_v4, StorageInput);
    m.decorate({cur_var, DecorationLocation, kCurLocation});
    m.decorate({prev_var, DecorationLocation, kPrevLocation});
    m.interface.push_back(cur_var), m.interface.push_back(prev_var);
    if (!m.glsl) {  // PackHalf2x16
        static const char name[] = "GLSL.std.450";
        m.glsl = m.id();
        std::vector<uint32_t> words((sizeof name + 3) / 4, 0);
        std::memcpy(words.data(), name, sizeof name);
        m.imports.push_back(word_op(uint32_t(2 + words.size()), OpExtInstImport));
        m.imports.push_back(m.glsl);
        m.imports.insert(m.imports.end(), words.begin(), words.end());
    }
    const uint32_t zero = m.constant(p.u32, {0}), one = m.constant(p.u32, {1});
    const uint32_t fzero = m.constant(f32, {0}), fhalf = m.constant(f32, {0x3f000000u});
    const uint32_t device = m.constant(p.u32, {1}), relaxed = zero;
    const uint32_t thirty_two = m.constant(p.u32, {32});
    const uint32_t none = m.constant(u64, {0xffffffffu, 0xffffffffu});

    Code c{m, {}};
    const uint32_t cur = c.load(v4f, cur_var), prev = c.load(v4f, prev_var), fc = c.load(v4f, frag);
    const uint32_t pix = c.member(p, p.pc_pix, p.ptr_pix_t, 1);
    const uint32_t flags = c.member(p, p.pc_u32, p.u32, 7), width = c.member(p, p.pc_u32, p.u32, 8);
    const uint32_t jc = c.member(p, p.pc_v2f, p.v2f, 10), jp = c.member(p, p.pc_v2f, p.v2f, 11);
    auto x = [&](uint32_t v, uint32_t i) { return c.op(OpCompositeExtract, f32, {v, i}); };
    const uint32_t on = c.op(OpINotEqual, boolean, {c.op(OpBitwiseAnd, p.u32, {flags, one}), zero});
    c.when(on, [&] {  // a draw with object motion: the rest only for it
    const uint32_t cw = x(cur, 3), pw = x(prev, 3);
    const uint32_t ok = c.op(OpLogicalAnd, boolean, {c.op(OpFOrdGreaterThan, boolean, {cw, fzero}),
                                                     c.op(OpFOrdGreaterThan, boolean, {pw, fzero})});
    // NDC without the jitter; the motion in UV: x right, y down (NVN's NDC +Y is the top row).
    const uint32_t cx = c.op(OpFSub, f32, {c.op(OpFDiv, f32, {x(cur, 0), cw}), x(jc, 0)});
    const uint32_t cy = c.op(OpFSub, f32, {c.op(OpFDiv, f32, {x(cur, 1), cw}), x(jc, 1)});
    const uint32_t px = c.op(OpFSub, f32, {c.op(OpFDiv, f32, {x(prev, 0), pw}), x(jp, 0)});
    const uint32_t py = c.op(OpFSub, f32, {c.op(OpFDiv, f32, {x(prev, 1), pw}), x(jp, 1)});
    const uint32_t mx = c.op(OpFMul, f32, {c.op(OpFSub, f32, {px, cx}), fhalf});
    const uint32_t my = c.op(OpFMul, f32, {c.op(OpFSub, f32, {cy, py}), fhalf});
    const uint32_t mv = c.op(OpCompositeConstruct, p.v2f, {mx, my});
    const uint32_t packed = c.op(OpExtInst, p.u32, {m.glsl, GlslPackHalf2x16, mv});
    const uint32_t depth = c.op(OpBitcast, p.u32, {x(fc, 2)});
    const uint32_t key = c.op(OpBitwiseOr, u64, {c.op(OpShiftLeftLogical, u64, {c.op(OpUConvert, u64, {depth}), thirty_two}),
                                                 c.op(OpUConvert, u64, {packed})});
    const uint32_t ix = c.op(OpConvertFToU, p.u32, {x(fc, 0)}), iy = c.op(OpConvertFToU, p.u32, {x(fc, 1)});
    const uint32_t idx = c.op(OpSelect, p.u32, {ok, c.op(OpIAdd, p.u32, {c.op(OpIMul, p.u32, {iy, width}), ix}), zero});
    const uint32_t value = c.op(OpSelect, u64, {ok, key, none});
    const uint32_t ptr = c.op(OpAccessChain, psb_u64, {pix, zero, idx});
    c.op(OpAtomicUMin, u64, {ptr, device, relaxed, value});
    });
    // A translucent draw (flags 2: alpha blended, 4: added): how much it covers the pixel, from its colour output's
    // alpha or brightness, the largest of the frame's into the buffer's second half (`reactive` its first entry), kept
    // inverted so the atomicMin and the clear to all ones serve it too (motion.comp: temporal AA's reactive mask).
    if (const uint32_t color = output_location0(m, v4f)) {
        const uint32_t two = m.constant(p.u32, {2}), six = m.constant(p.u32, {6});
        const uint32_t fone = m.constant(f32, {0x3f800000u}), f255 = m.constant(f32, {0x437f0000u});
        const uint32_t translucent = c.op(OpINotEqual, boolean, {c.op(OpBitwiseAnd, p.u32, {flags, six}), zero});
        c.when(translucent, [&] {
            const uint32_t col = c.load(v4f, color);
            const uint32_t bright = c.op(OpExtInst, f32, {m.glsl, GlslFMax, c.op(OpExtInst, f32, {m.glsl, GlslFMax, x(col, 0), x(col, 1)}),
                                                           x(col, 2)});
            const uint32_t blended = c.op(OpINotEqual, boolean, {c.op(OpBitwiseAnd, p.u32, {flags, two}), zero});
            const uint32_t v = c.op(OpExtInst, f32, {m.glsl, GlslFClamp, c.op(OpSelect, f32, {blended, x(col, 3), bright}), fzero, fone});
            const uint32_t u = c.op(OpConvertFToU, p.u32, {c.op(OpFMul, f32, {v, f255})});
            const uint32_t reactive = c.member(p, p.pc_u32, p.u32, 9);
            const uint32_t ix = c.op(OpConvertFToU, p.u32, {x(fc, 0)}), iy = c.op(OpConvertFToU, p.u32, {x(fc, 1)});
            const uint32_t idx = c.op(OpIAdd, p.u32, {reactive, c.op(OpIAdd, p.u32, {c.op(OpIMul, p.u32, {iy, width}), ix})});
            const uint32_t ptr = c.op(OpAccessChain, psb_u64, {pix, zero, idx});
            c.op(OpAtomicUMin, u64, {ptr, device, relaxed, c.op(OpNot, u64, {c.op(OpUConvert, u64, {u})})});
        });
    }
    return m.emit(c.w, out);
}

// The same bias read from the motion patch's push constants (member 12, MotionPush::bias) at each sample instead of a
// constant: the runtime pushes it per draw (nvn_temporal.cpp: temporal AA's, at its upscaling ratio, into the 3D
// scene; 0 elsewhere and with temporal AA off), so it follows the settings screen while the game runs. False: the
// module has no such block (the motion patch did not take), or no sample to bias.
bool patch_mip_bias_push(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    constexpr uint32_t OpImageSampleImplicitLod = 87, OpImageSampleProjImplicitLod = 91, ImageOperandsBias = 1;
    constexpr uint32_t StoragePushConstantC = 9, kBiasMember = 12;
    if (in.size() < 5) return false;
    uint32_t bound = in[3], model = ~0u, var = 0, var_ptr = 0, u32 = 0, c12 = 0, ptr_f32 = 0, samples = 0;
    size_t first_function = in.size();
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> pointers;  // pointer type -> (storage, pointee)
    std::map<uint32_t, std::vector<uint32_t>> structs;          // struct -> member types
    std::map<uint32_t, bool> floats32;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) return false;
        const uint32_t* w = &in[i];
        if (op == OpEntryPoint && model == ~0u) model = w[1];
        else if (op == OpTypeFloat && len == 3) floats32[w[1]] = w[2] == 32;
        else if (op == OpTypeInt && len == 4 && w[2] == 32 && w[3] == 0) u32 = w[1];
        else if (op == OpTypePointer && len == 4) pointers[w[1]] = {w[2], w[3]};
        else if (op == OpTypeStruct) structs[w[1]] = std::vector<uint32_t>(w + 2, w + len);
        else if (op == OpVariable && len >= 4 && w[3] == StoragePushConstantC) var = w[2], var_ptr = w[1];
        else if (op == OpFunction && first_function == in.size()) first_function = i;
        else if ((op == OpImageSampleImplicitLod || op == OpImageSampleProjImplicitLod) && !(len > 5 && (w[5] & ImageOperandsBias)))
            samples++;
        i += len;
    }
    if (model != ModelFragment || !samples || !var || !u32 || first_function == in.size()) return false;
    const auto vp = pointers.find(var_ptr);
    if (vp == pointers.end()) return false;
    const auto st = structs.find(vp->second.second);
    if (st == structs.end() || st->second.size() <= kBiasMember || !floats32[st->second[kBiasMember]]) return false;
    const uint32_t f32 = st->second[kBiasMember];
    for (const auto& [id, pt] : pointers)
        if (pt.first == StoragePushConstantC && pt.second == f32) ptr_f32 = id;
    std::vector<uint32_t> globals;
    if (!ptr_f32) ptr_f32 = bound++, globals.insert(globals.end(), {word_op(4, OpTypePointer), ptr_f32, StoragePushConstantC, f32});
    c12 = bound++;
    globals.insert(globals.end(), {word_op(4, OpConstant), u32, c12, kBiasMember});
    out.assign(in.begin(), in.begin() + 5);
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        const uint32_t* w = &in[i];
        if (i == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if ((op == OpImageSampleImplicitLod || op == OpImageSampleProjImplicitLod) && !(len > 5 && (w[5] & ImageOperandsBias))) {
            const uint32_t ptr = bound++, bias = bound++;
            out.insert(out.end(), {word_op(5, OpAccessChain), ptr_f32, ptr, var, c12, word_op(4, OpLoad), f32, bias, ptr});
            // Result type, result, sampled image, coordinate, then the operands mask: Bias (bit 0) comes first.
            out.push_back(word_op(len + (len > 5 ? 1 : 2), op));
            out.insert(out.end(), w + 1, w + 5);
            out.push_back((len > 5 ? w[5] : 0) | ImageOperandsBias);
            out.push_back(bias);
            if (len > 6) out.insert(out.end(), w + 6, w + len);
        } else {
            out.insert(out.end(), w, w + len);
        }
        i += len;
    }
    out[3] = bound;
    return true;
}

// Temporal AA's texture LOD bias (NVIDIA's and AMD's guides: log2(render / display) - 1, so -1 at the picture's own
// size): every implicit-LOD sample of a fragment shader gets a Bias operand; the jittered frames resolve the finer
// mip the accumulation then holds. Explicit LODs (textureLod: the bloom chain's levels) and depth compares keep
// theirs, which a sampler's mipLodBias would not. A sample with a bias of its own is left alone.
bool patch_mip_bias(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, float bias) {
    constexpr uint32_t OpImageSampleImplicitLod = 87, OpImageSampleProjImplicitLod = 91, ImageOperandsBias = 1;
    if (in.size() < 5) return false;
    uint32_t bound = in[3], f32 = 0, constant = 0, samples = 0, model = ~0u;
    size_t first_function = in.size();
    uint32_t bias_bits;
    std::memcpy(&bias_bits, &bias, 4);
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        if (!len || i + len > in.size()) return false;
        const uint32_t* w = &in[i];
        if (op == OpEntryPoint && model == ~0u) model = w[1];
        else if (op == OpTypeFloat && len == 3 && w[2] == 32) f32 = w[1];
        else if (op == OpConstant && len == 4 && f32 && w[1] == f32 && w[3] == bias_bits) constant = w[2];
        else if (op == OpFunction && first_function == in.size()) first_function = i;
        else if ((op == OpImageSampleImplicitLod || op == OpImageSampleProjImplicitLod) && !(len > 5 && (w[5] & ImageOperandsBias)))
            samples++;
        i += len;
    }
    if (model != ModelFragment || !samples || !f32 || first_function == in.size()) return false;
    std::vector<uint32_t> globals;
    if (!constant) constant = bound++, globals = {word_op(4, OpConstant), f32, constant, bias_bits};
    out.assign(in.begin(), in.begin() + 5);
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16, op = in[i] & 0xffff;
        const uint32_t* w = &in[i];
        if (i == first_function) out.insert(out.end(), globals.begin(), globals.end());
        if ((op == OpImageSampleImplicitLod || op == OpImageSampleProjImplicitLod) && !(len > 5 && (w[5] & ImageOperandsBias))) {
            // Result type, result, sampled image, coordinate, then the operands mask: Bias (bit 0) comes first.
            out.push_back(word_op(len + (len > 5 ? 1 : 2), op));
            out.insert(out.end(), w + 1, w + 5);
            out.push_back((len > 5 ? w[5] : 0) | ImageOperandsBias);
            out.push_back(constant);
            if (len > 6) out.insert(out.end(), w + 6, w + len);
        } else {
            out.insert(out.end(), w, w + len);
        }
        i += len;
    }
    out[3] = bound;
    return true;
}

}  // namespace nx::nvn
