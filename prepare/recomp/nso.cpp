// recomp/nso.py and recomp/ehframe.py: the NSO's segments, MOD0, .dynamic, symbols, relocations (RELA, JMPREL,
// packed RELR), init/fini arrays; the function bounds of .eh_frame_hdr / .eh_frame.
#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include "analysis.h"

namespace rc {

namespace {

constexpr int64_t DT_NULL = 0, DT_PLTRELSZ = 2, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8, DT_SYMENT = 11,
                  DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_FINI_ARRAY = 26, DT_INIT_ARRAYSZ = 27, DT_FINI_ARRAYSZ = 28,
                  DT_RELRSZ = 35, DT_RELR = 36;

uint32_t rd32(const std::vector<uint8_t>& b, uint64_t a) {
    if (a + 4 > b.size()) throw std::runtime_error("read past the end of the module");
    uint32_t v;
    std::memcpy(&v, b.data() + a, 4);
    return v;
}
uint64_t rd64(const std::vector<uint8_t>& b, uint64_t a) {
    if (a + 8 > b.size()) throw std::runtime_error("read past the end of the module");
    uint64_t v;
    std::memcpy(&v, b.data() + a, 8);
    return v;
}

bool lz4_block(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
    const uint8_t *ip = src, *iend = src + n;
    uint8_t *op = dst, *oend = dst + cap;
    while (ip < iend) {
        uint8_t token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                lit += b;
            } while (b == 255);
        }
        if (size_t(iend - ip) < lit || size_t(oend - op) < lit) return false;
        std::memcpy(op, ip, lit);
        op += lit, ip += lit;
        if (ip >= iend) break;
        if (iend - ip < 2) return false;
        size_t offset = size_t(ip[0] | ip[1] << 8);
        ip += 2;
        if (offset == 0 || offset > size_t(op - dst)) return false;
        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t b;
            do {
                if (ip >= iend) return false;
                b = *ip++;
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (size_t(oend - op) < ml) return false;
        const uint8_t* m = op - offset;
        for (size_t k = 0; k < ml; k++) op[k] = m[k];
        op += ml;
    }
    return op == oend;
}

void parse_mod0(Module& m) {
    uint64_t mod0 = m.u32(4);
    if (mod0 + 4 > m.image.size() || std::memcmp(m.image.data() + mod0, "MOD0", 4) != 0)
        throw std::runtime_error("MOD0 not found");
    int64_t f[5];
    for (int i = 0; i < 5; i++) f[i] = int64_t(int32_t(m.u32(mod0 + 4 + 4 * uint64_t(i)))) + int64_t(mod0);
    int64_t dyn_off = f[0], bss_start = f[1], bss_end = f[2], eh_start = f[3], eh_end = f[4];
    m.bss[0] = uint64_t(bss_start), m.bss[1] = uint64_t(bss_end - bss_start);
    m.eh_frame_hdr[0] = uint64_t(eh_start), m.eh_frame_hdr[1] = uint64_t(eh_end - eh_start);

    uint64_t addr = uint64_t(dyn_off);
    for (;;) {
        int64_t tag = int64_t(m.u64(addr));
        uint64_t val = m.u64(addr + 8);
        addr += 16;
        if (tag == DT_NULL) break;
        m.dynamic.emplace(tag, val);  // setdefault: the first one counts
    }
    auto& dyn = m.dynamic;
    auto need = [&](int64_t t) {
        auto it = dyn.find(t);
        if (it == dyn.end()) throw std::runtime_error("dynamic section without a required tag");
        return it->second;
    };
    uint64_t strtab = need(DT_STRTAB), symtab = need(DT_SYMTAB);
    uint64_t syment = dyn.count(DT_SYMENT) ? dyn[DT_SYMENT] : 24;
    uint64_t nsyms = (strtab - symtab) / syment;  // symtab normally sits right before strtab
    for (uint64_t i = 0; i < nsyms; i++) {
        uint64_t p = symtab + i * syment;
        Symbol s;
        uint32_t st_name = m.u32(p);
        uint8_t st_info = m.image.at(p + 4);
        s.shndx = uint16_t(m.image.at(p + 6) | m.image.at(p + 7) << 8);
        s.value = m.u64(p + 8);
        s.size = m.u64(p + 16);
        s.name = m.cstr(strtab + st_name);
        s.type = st_info & 0xF;
        s.bind = st_info >> 4;
        m.symbols.push_back(std::move(s));
    }
    auto relocs = [&](uint64_t start, uint64_t size) {
        for (uint64_t a = start; a < start + size; a += 24) {
            uint64_t info = m.u64(a + 8);
            m.relocs.push_back({m.u64(a), uint32_t(info & 0xFFFFFFFF), uint32_t(info >> 32), int64_t(m.u64(a + 16))});
        }
    };
    auto relr = [&](uint64_t start, uint64_t size) {
        uint64_t where = 0;
        for (uint64_t a = start; a < start + size; a += 8) {
            uint64_t e = m.u64(a);
            if ((e & 1) == 0) {
                m.relocs.push_back({e, R_AARCH64_RELATIVE, 0, int64_t(m.u64(e))});
                where = e + 8;
            } else {
                for (int k = 1; k < 64; k++)
                    if ((e >> k) & 1) {
                        uint64_t off = where + uint64_t(k - 1) * 8;
                        m.relocs.push_back({off, R_AARCH64_RELATIVE, 0, int64_t(m.u64(off))});
                    }
                where += 63 * 8;
            }
        }
    };
    if (dyn.count(DT_RELA)) relocs(dyn[DT_RELA], need(DT_RELASZ));
    if (dyn.count(DT_JMPREL)) relocs(dyn[DT_JMPREL], need(DT_PLTRELSZ));
    if (dyn.count(DT_RELR)) relr(dyn[DT_RELR], need(DT_RELRSZ));

    // init/fini arrays hold RELATIVE-relocated pointers: the addends ({offset: reloc}: the last one per offset)
    std::map<uint64_t, const Reloc*> rel_at;
    for (const Reloc& r : m.relocs) rel_at[r.offset] = &r;
    auto ptr_array = [&](uint64_t start, uint64_t size) {
        std::vector<uint64_t> out;
        for (uint64_t a = start; a < start + size; a += 8) {
            auto it = rel_at.find(a);
            out.push_back(it != rel_at.end() && it->second->type == R_AARCH64_RELATIVE ? uint64_t(it->second->addend)
                                                                                      : m.u64(a));
        }
        return out;
    };
    if (dyn.count(DT_INIT_ARRAY)) m.init_array = ptr_array(dyn[DT_INIT_ARRAY], need(DT_INIT_ARRAYSZ));
    if (dyn.count(DT_FINI_ARRAY)) m.fini_array = ptr_array(dyn[DT_FINI_ARRAY], need(DT_FINI_ARRAYSZ));
}

}  // namespace

uint32_t Module::u32(uint64_t a) const { return rd32(image, a); }
uint64_t Module::u64(uint64_t a) const { return rd64(image, a); }

std::string Module::cstr(uint64_t a) const {
    if (a >= image.size()) throw std::runtime_error("string past the end of the module");
    const void* z = std::memchr(image.data() + a, 0, image.size() - a);
    if (!z) throw std::runtime_error("unterminated string in the module");
    // decode("utf-8", "replace"): names are ASCII in practice; bytes are kept as they are
    return std::string(reinterpret_cast<const char*>(image.data() + a), static_cast<const uint8_t*>(z) - (image.data() + a));
}

Module load_nso_bytes(std::vector<uint8_t> raw, const std::string& name) {
    if (raw.size() < 0x100 || std::memcmp(raw.data(), "NSO0", 4) != 0) throw std::runtime_error(name + ": not an NSO");
    uint32_t flags = rd32(raw, 0x0C);
    struct Seg {
        uint32_t mem_off;
        std::vector<uint8_t> blob;
    } segs[3];
    for (int i = 0; i < 3; i++) {
        uint32_t file_off = rd32(raw, 0x10 + 0x10 * uint64_t(i)), mem_off = rd32(raw, 0x14 + 0x10 * uint64_t(i)),
                 size = rd32(raw, 0x18 + 0x10 * uint64_t(i)), comp = rd32(raw, 0x60 + 4 * uint64_t(i));
        if (uint64_t(file_off) + comp > raw.size()) throw std::runtime_error(name + ": segment outside the file");
        segs[i].mem_off = mem_off;
        if (flags & (1u << i)) {
            segs[i].blob.resize(size);
            if (!lz4_block(raw.data() + file_off, comp, segs[i].blob.data(), size))
                throw std::runtime_error(name + ": a segment does not decompress");
        } else {
            segs[i].blob.assign(raw.begin() + file_off, raw.begin() + file_off + comp);
        }
    }
    uint64_t data_end = segs[2].mem_off + segs[2].blob.size();
    Module m;
    m.name = name;
    m.image.assign(data_end, 0);
    for (auto& s : segs) std::memcpy(m.image.data() + s.mem_off, s.blob.data(), s.blob.size());
    m.text[0] = segs[0].mem_off, m.text[1] = segs[0].blob.size();
    m.ro[0] = segs[1].mem_off, m.ro[1] = segs[1].blob.size();
    m.data[0] = segs[2].mem_off, m.data[1] = segs[2].blob.size();
    m.bss[0] = data_end, m.bss[1] = rd32(raw, 0x3C);
    parse_mod0(m);
    return m;
}

Module load_nso(const std::string& path, const std::string& name) {
    std::ifstream f(upath(path), std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + path);
    std::vector<uint8_t> raw((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return load_nso_bytes(std::move(raw), name.empty() ? path : name);
}

// ---- eh_frame ----

namespace {

constexpr uint8_t DW_EH_PE_omit = 0xFF, DW_EH_PE_absptr = 0x00, DW_EH_PE_uleb128 = 0x01, DW_EH_PE_udata2 = 0x02,
                  DW_EH_PE_udata4 = 0x03, DW_EH_PE_udata8 = 0x04, DW_EH_PE_sleb128 = 0x09, DW_EH_PE_sdata2 = 0x0A,
                  DW_EH_PE_sdata4 = 0x0B, DW_EH_PE_sdata8 = 0x0C, DW_EH_PE_pcrel = 0x10, DW_EH_PE_datarel = 0x30;

uint8_t at(const std::vector<uint8_t>& b, uint64_t p) {
    if (p >= b.size()) throw std::runtime_error("eh_frame: read past the end");
    return b[p];
}
uint64_t uleb(const std::vector<uint8_t>& b, uint64_t& p) {
    uint64_t r = 0;
    int s = 0;
    for (;;) {
        uint8_t x = at(b, p++);
        if (s < 64) r |= uint64_t(x & 0x7F) << s;
        s += 7;
        if (!(x & 0x80)) return r;
    }
}
int64_t sleb(const std::vector<uint8_t>& b, uint64_t& p) {
    uint64_t r = 0;
    int s = 0;
    for (;;) {
        uint8_t x = at(b, p++);
        if (s < 64) r |= uint64_t(x & 0x7F) << s;
        s += 7;
        if (!(x & 0x80)) {
            if ((x & 0x40) && s < 64) r -= uint64_t(1) << s;
            return int64_t(r);
        }
    }
}

// an encoded pointer at p (moved past it)
uint64_t read_ptr(const std::vector<uint8_t>& b, uint64_t& p, uint8_t enc, uint64_t datarel = 0) {
    if (enc == DW_EH_PE_omit) return 0;
    uint8_t fmt = enc & 0x0F;
    uint64_t pos = p, v;
    auto le = [&](int n) {
        uint64_t x = 0;
        for (int k = 0; k < n; k++) x |= uint64_t(at(b, p + uint64_t(k))) << (8 * k);
        p += uint64_t(n);
        return x;
    };
    if (fmt == DW_EH_PE_absptr || fmt == DW_EH_PE_udata8 || fmt == DW_EH_PE_sdata8) v = le(8);
    else if (fmt == DW_EH_PE_udata4) v = le(4);
    else if (fmt == DW_EH_PE_sdata4) v = uint64_t(int64_t(int32_t(le(4))));
    else if (fmt == DW_EH_PE_udata2) v = le(2);
    else if (fmt == DW_EH_PE_sdata2) v = uint64_t(int64_t(int16_t(le(2))));
    else if (fmt == DW_EH_PE_uleb128) v = uleb(b, p);
    else if (fmt == DW_EH_PE_sleb128) v = uint64_t(sleb(b, p));
    else throw std::runtime_error("eh_frame: pointer encoding");
    uint8_t app = enc & 0x70;
    if (app == DW_EH_PE_pcrel) v += pos;
    else if (app == DW_EH_PE_datarel) v += datarel;
    else if (app) throw std::runtime_error("eh_frame: pointer application");
    return v;
}

struct Cie {
    uint8_t fde_enc, lsda_enc;
    uint64_t pers;
};

Cie parse_cie(const std::vector<uint8_t>& b, uint64_t p) {
    uint64_t q = p + 8;  // length + CIE id
    uint8_t version = at(b, q++);
    std::string aug;
    while (at(b, q)) aug += char(b[q++]);
    q++;
    uleb(b, q);  // code alignment
    sleb(b, q);  // data alignment
    if (version == 1) q++;
    else uleb(b, q);
    Cie c{DW_EH_PE_absptr, DW_EH_PE_omit, 0};
    if (!aug.empty() && aug[0] == 'z') {
        uleb(b, q);
        for (size_t k = 1; k < aug.size(); k++) {
            char ch = aug[k];
            if (ch == 'R') c.fde_enc = at(b, q++);
            else if (ch == 'L') c.lsda_enc = at(b, q++);
            else if (ch == 'P') {
                uint8_t penc = at(b, q);
                q++;
                c.pers = read_ptr(b, q, penc);
            } else if (ch == 'S' || ch == 'B') {
            } else break;
        }
    }
    return c;
}

}  // namespace

std::vector<Fde> parse_ehframe(const Module& m) {
    const auto& b = m.image;
    uint64_t hdr = m.eh_frame_hdr[0];
    if (!hdr) return {};
    uint8_t version = at(b, hdr), frame_enc = at(b, hdr + 1), count_enc = at(b, hdr + 2), table_enc = at(b, hdr + 3);
    if (version != 1) throw std::runtime_error("eh_frame_hdr version");
    uint64_t p = hdr + 4;
    read_ptr(b, p, frame_enc);
    uint64_t count = read_ptr(b, p, count_enc);
    std::map<uint64_t, Cie> cies;
    std::vector<Fde> fdes;
    for (uint64_t k = 0; k < count; k++) {
        read_ptr(b, p, table_enc, hdr);
        uint64_t fde = read_ptr(b, p, table_enc, hdr);
        uint64_t cie_ptr = fde + 4 - m.u32(fde + 4);
        auto it = cies.find(cie_ptr);
        if (it == cies.end()) it = cies.emplace(cie_ptr, parse_cie(b, cie_ptr)).first;
        const Cie& c = it->second;
        uint64_t q = fde + 8;
        Fde f;
        f.start = read_ptr(b, q, c.fde_enc);
        f.size = read_ptr(b, q, c.fde_enc & 0x0F);
        if (c.lsda_enc != DW_EH_PE_omit) {
            uleb(b, q);
            f.lsda = read_ptr(b, q, c.lsda_enc);
        }
        f.personality = c.pers;
        fdes.push_back(f);
    }
    std::stable_sort(fdes.begin(), fdes.end(), [](const Fde& a, const Fde& c) { return a.start < c.start; });
    return fdes;
}

}  // namespace rc
