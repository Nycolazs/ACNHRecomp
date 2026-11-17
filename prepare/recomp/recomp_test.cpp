// Host test of the recompiler's analysis (recomp/analysis.cpp) on a tiny hand-made module (no game code): a function
// whose FDE covers a jump table, with a second entry in its middle, before the table's br.
//   recomp_test
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "analysis.h"

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) failures++;
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
void put64(std::vector<uint8_t>& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

std::string list(const std::vector<uint64_t>& v) {
    std::string s;
    for (uint64_t a : v) {
        char b[24];
        std::snprintf(b, sizeof b, "%s%#llx", s.empty() ? "" : " ", static_cast<unsigned long long>(a));
        s += b;
    }
    return "{" + s + "}";
}

// The function f (0x00-0x44, one FDE) and "mid", a symbol inside it (0x0c): the functions [0x00, 0x44) and
// [0x0c, 0x44) both hold the br at 0x28, whose table's case 0 (0x04) lies before mid. Cut to mid's bounds, that
// case is lost; ACNHRecomp issue #58: it fell to the switch's default, a call into the middle of f.
rc::Module overlapping_jump_table() {
    static const uint32_t code[] = {
        0x14000004,  // 0x00 f:      b    disp
        0x52800140,  // 0x04 case0:  mov  w0, #10
        0xd65f03c0,  // 0x08         ret
        0xd503201f,  // 0x0c mid:    nop
        0x7100081f,  // 0x10 disp:   cmp  w0, #2
        0x54000148,  // 0x14         b.hi dflt
        0x10000741,  // 0x18         adr  x1, 0x100 (the table)
        0x38604822,  // 0x1c         ldrb w2, [x1, w0, uxtw]
        0x10ffff23,  // 0x20         adr  x3, case0
        0x8b020863,  // 0x24         add  x3, x3, x2, lsl #2
        0xd61f0060,  // 0x28         br   x3
        0x52800160,  // 0x2c case1:  mov  w0, #11
        0xd65f03c0,  // 0x30         ret
        0x52800180,  // 0x34 case2:  mov  w0, #12
        0xd65f03c0,  // 0x38         ret
        0x52800000,  // 0x3c dflt:   mov  w0, #0
        0xd65f03c0,  // 0x40         ret
    };
    const uint64_t text_size = sizeof code;
    rc::Module m;
    m.name = "test";
    m.image.assign(0x400, 0);
    for (size_t i = 0; i < sizeof code / 4; i++) put32(m.image, 4 * i, code[i]);
    m.text[0] = 0, m.text[1] = text_size;
    m.ro[0] = 0x100, m.ro[1] = 0x300;
    // the table: (case - case0) / 4
    m.image[0x100] = 0, m.image[0x101] = (0x2c - 0x04) / 4, m.image[0x102] = (0x34 - 0x04) / 4;
    // .eh_frame: a CIE without augmentation (absolute pointers) and f's FDE
    put32(m.image, 0x200, 9);  // CIE: length, id 0, version 1, "", code align 4, data align -8, return register 30
    m.image[0x208] = 1, m.image[0x209] = 0, m.image[0x20a] = 4, m.image[0x20b] = 0x78, m.image[0x20c] = 30;
    put32(m.image, 0x220, 20);    // FDE: length, CIE pointer, start, size
    put32(m.image, 0x224, 0x24);  // 0x224 - 0x200
    put64(m.image, 0x228, 0);
    put64(m.image, 0x230, text_size);
    // .eh_frame_hdr: version 1, no eh_frame pointer, a udata4 count, udata4 (initial location, FDE) pairs
    m.image[0x300] = 1, m.image[0x301] = 0xff, m.image[0x302] = 0x03, m.image[0x303] = 0x03;
    put32(m.image, 0x304, 1);
    put32(m.image, 0x308, 0);
    put32(m.image, 0x30c, 0x220);
    m.eh_frame_hdr[0] = 0x300, m.eh_frame_hdr[1] = 0x10;
    m.symbols.resize(2);
    m.symbols[1].name = "mid", m.symbols[1].value = 0x0c, m.symbols[1].shndx = 1;
    return m;
}

}  // namespace

int main() {
    rc::Module m = overlapping_jump_table();
    rc::Analysis an;
    rc::analyze(m, an, [](const std::string&) {});
    auto jt = an.jump_tables.find(0x28);
    check(jt != an.jump_tables.end(), "the br's jump table is found");
    if (jt != an.jump_tables.end())
        check(jt->second == std::vector<uint64_t>{0x04, 0x2c, 0x34},
              "a function overlapping the br keeps every target: " + list(jt->second));
    bool case0_entry = false;
    for (const rc::Function& f : an.functions) case0_entry |= f.start == 0x04;
    check(case0_entry, "the target before mid is an entry (tail-called from mid's copy of the switch)");
    check(an.unresolved_br.empty(), "no unresolved br");
    if (failures) {
        std::printf("%d failed\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
