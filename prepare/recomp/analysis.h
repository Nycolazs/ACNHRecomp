// The module and its analysis (ACNHRecomp's recomp/nso.py, ehframe.py, analysis.py): segments, symbols and
// relocations of an NSO; function bounds, PLT stubs and jump tables of its text.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace rc {

constexpr unsigned R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026,
                   R_AARCH64_RELATIVE = 1027;

struct Symbol {
    std::string name;
    uint64_t value = 0, size = 0;
    unsigned type = 0, bind = 0, shndx = 0;
    bool imported() const { return shndx == 0; }
};

struct Reloc {
    uint64_t offset = 0;
    uint32_t type = 0;
    uint32_t sym = 0;
    int64_t addend = 0;
};

struct Module {
    std::string name;
    std::vector<uint8_t> image;
    uint64_t text[2] = {}, ro[2] = {}, data[2] = {}, bss[2] = {};  // (start, size)
    std::vector<Symbol> symbols;
    std::vector<Reloc> relocs;
    std::map<int64_t, uint64_t> dynamic;
    std::vector<uint64_t> init_array, fini_array;
    uint64_t eh_frame_hdr[2] = {};
    uint32_t u32(uint64_t a) const;
    uint64_t u64(uint64_t a) const;
    bool in_text(uint64_t a) const { return a >= text[0] && a < text[0] + text[1]; }
    bool in_image(int64_t a) const { return a >= 0 && uint64_t(a) < image.size(); }
    std::string cstr(uint64_t a) const;
};

// A path given as UTF-8 (every path of RecompOptions, on every platform): a std::string opened as it is would be read
// in the ANSI code page on Windows, where a folder named "ção" is then not found (or another one is).
inline std::filesystem::path upath(const std::string& s) { return std::filesystem::path(std::u8string(s.begin(), s.end())); }

// nso.load: throws std::runtime_error
Module load_nso(const std::string& path, const std::string& name);
Module load_nso_bytes(std::vector<uint8_t> raw, const std::string& name);

struct Fde {
    uint64_t start = 0, size = 0, lsda = 0, personality = 0;
};
std::vector<Fde> parse_ehframe(const Module& m);  // sorted by start

// addr -> (mnemonic, op_str) for the whole text (Capstone's lite disassembly, skipdata, ".byte" left out)
class Insns {
public:
    uint64_t t0 = 0;
    size_t n = 0;
    void init(uint64_t start, uint64_t size);
    void add(uint64_t a, const char* mn, const char* op);
    bool contains(uint64_t a) const { return index(a) >= 0; }
    // nullptr when there is no instruction at a
    const std::string* mnemonic(uint64_t a) const {
        int64_t i = index(a);
        return i < 0 ? nullptr : &names_[mn_[size_t(i)]];
    }
    const std::string& op_str(uint64_t a) const { return ops_[op_[size_t(index(a))]]; }
    // get(a, ("", "")): the pair, empty strings when absent
    std::pair<const std::string&, const std::string&> get(uint64_t a) const {
        int64_t i = index(a);
        if (i < 0) return {empty_, empty_};
        return {names_[mn_[size_t(i)]], ops_[op_[size_t(i)]]};
    }
    template <class Fn>
    void each(Fn&& fn) const {  // items(): in address order
        for (size_t i = 0; i < n; i++)
            if (mn_[i]) fn(t0 + 4 * i, names_[mn_[i]], ops_[op_[i]]);
    }
    void done() { op_ids_.clear(), op_ids_.rehash(0), mn_ids_.clear(); }

private:
    int64_t index(uint64_t a) const {
        if (a < t0 || ((a - t0) & 3)) return -1;
        uint64_t i = (a - t0) >> 2;
        return i < n && mn_[i] ? int64_t(i) : -1;
    }
    std::vector<uint16_t> mn_;
    std::vector<uint32_t> op_;
    std::vector<std::string> names_{""}, ops_;
    std::unordered_map<std::string, uint16_t> mn_ids_;
    std::unordered_map<std::string, uint32_t> op_ids_;
    std::string empty_;
};

struct Function {
    uint64_t start = 0, end = 0;
    bool fde = false;
    std::string name;
};

struct Analysis {
    const Module* module = nullptr;
    Insns insns;
    std::vector<Function> functions;  // sorted by start
    std::map<uint64_t, std::string> plt;           // stub -> import name
    std::map<uint64_t, std::string> plt_local;     // stub -> the module's own export it reaches
    std::map<uint64_t, std::string> got_imports;   // got slot -> import name
    std::map<uint64_t, std::string> got_local;
    std::map<uint64_t, std::vector<uint64_t>> jump_tables;  // br -> sorted targets
    std::map<uint64_t, std::string> names;         // addr -> symbol name
    std::vector<uint64_t> unresolved_br;
};

// analysis.analyze(m, log, patched)
void analyze(const Module& m, Analysis& an, const std::function<void(const std::string&)>& log,
             const std::vector<uint64_t>& patched = {});

}  // namespace rc
