// Clicking with the mouse (the touch screen) in ACNH's UI. Two UI systems answer it:
//  - guiTouchManager (main+31c3b50): the game's own touch support, buttons registered in a touch tree (the software
//    keyboard, confirmation windows). runtime/src/hle_hid.cpp feeds it touches as the console does; one hook here
//    reaches the window buttons the game leaves out of reach.
//  - the focus managers of the layout framework (main+3c48c60): every list with a cursor (dialogue choices, menus).
//    They know no touch; a click on one of their items moves the cursor there and presses A, as a player would, and
//    the mouse moving over one moves the cursor there.
//  - the message window: a click while it waits for the next page presses A.
// docs/ACNH.md ("Touch / mouse") has the overview.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

#include <nx/runtime.h>

#include "version.h"

namespace nx {

namespace {

// ---- guiTouchManager ------------------------------------------------------------------------------------------

// The touch tree holds nodes: +0 parent, +8 first child, +0x18 next sibling, +0x20 the pane giving the rectangle
// (+0x30..+0x3c), +0x50 flags (bit 0: hit-tested along with what is under it). Its second layer (manager+0x2a8)
// holds the windows; a window's buttons hang under its content node (window+0x4d0), which has no pane of its own.
// Opening and activating a window (main+3195360, main+31975e0) clear that node's bit 0 and only a scrolling content
// pane (main+319b8d0) sets it again, so in the confirmation windows ("Are you OK with ...?": Rethink It / OK) the
// buttons were out of reach and a click did nothing. In that layer, a node without a pane under an enabled parent is
// enabled, below the windows' own nodes (the layer's children): a window that is not up keeps its node off, and with
// it everything under it.
void enable_groups(uint64_t node, int depth) {
    for (; node && depth < 16; node = ld<uint64_t>(node + 0x18)) {
        uint16_t fl = ld<uint16_t>(node + 0x50);
        if (!(fl & 1)) {
            uint64_t parent = ld<uint64_t>(node);
            if (!depth || ld<uint64_t>(node + 0x20) || !parent || !(ld<uint16_t>(parent + 0x50) & 1)) continue;
            st<uint16_t>(node + 0x50, uint16_t(fl | 1));
        }
        enable_groups(ld<uint64_t>(node + 8), depth + 1);
    }
}

// The frame on which a touch ended on a button of the touch tree (it pressed that button): the focus managers leave
// that click alone. A slot (manager+8 + 8 * k, manager+4 of them) has +7 set on the frame its touch ended and +0x30
// the button it grabbed.
std::atomic<uint64_t> g_gui_click_frame{0};

// guiTouchManager::update, right before its hit tests (main+31c3f98, x24 = the manager).
void gui_touch_update(CPU& c) {
    uint64_t m = c.x[24];
    if (!m) return;
    enable_groups(ld<uint64_t>(m + 0x2a8 + 8), 0);
    int n = ld<int32_t>(m + 4);
    for (int k = 0; k < n && k < 16; k++) {
        uint64_t s = ld<uint64_t>(m + 8 + 8 * k);
        if (s && ld<uint8_t>(s + 7) && ld<uint64_t>(s + 0x30)) g_gui_click_frame = g_frame.load();
    }
}

// ---- focus managers -------------------------------------------------------------------------------------------

// A focus manager (main+3c48c60 runs it each frame with x0 = it, x1 = the input device): +8 the list of its targets
// (a node at target + [manager+0x14], next at node+8, the list closes on the manager), +0x18 / +0x20 the players' cursor
// entries (+0x10 the focused target, +4 / +8 the cursor's position). A target (one per item, item+0x218): +0x20 its
// delegates (0x20 each: [1] leaves the focus), +0x14a0 the centre and +0x14a8 the size of its pane's rectangle
// (main+3c49f4c: the pane's global matrix applied to its local rectangle) in layout units (1920x1080, origin at the
// centre, y up), +0x14b0 the players it is enabled for. Setting the focus as the game does (main+31017d0): the old
// target is told it leaves, main+3c48b68 (manager, player, target, 0) moves it, the new target's vtable[0x18] runs and
// the entry takes its position.
uint64_t set_focus_addr() { return acnh::ofs(0x3c48b68, 0x3c48b78); }  // main+3c48b68 above (3.0.3: +3c48b78)
constexpr float kLayoutToScreen = 1280.0f / 1920.0f;
// Screen pixels around an item that still count as on it: an item's rectangle is often its text alone (the
// "Confirm." button: 13 high); a click near no item takes the nearest one this close.
constexpr float kPad = 6.0f, kNear = 24.0f;

struct Click {
    bool pending = false;
    uint32_t x = 0, y = 0;
    uint64_t frame = 0;
    uint64_t until = 0;  // the last frame it waits for a taker
    bool woke = false;
};
Click g_click;

// The mouse moved to (x, y) (button up) on `frame`: the item under it takes the cursor, without A.
struct Hover {
    bool pending = false;
    uint32_t x = 0, y = 0;
    uint64_t frame = 0;
};
Hover g_hover;
uint64_t g_hover_polled = ~0ull;

bool tracing() {
    static const bool on = std::getenv("NX_TRACE_TOUCH") != nullptr;
    return on;
}

// Calls guest code at main+offset with these arguments on a copy of the context; the stack below the caller's.
uint64_t guest(CPU& c, uint64_t offset, uint64_t a0, uint64_t a1 = 0, uint64_t a2 = 0, uint64_t a3 = 0) {
    CPU saved = c;
    c.x[0] = a0, c.x[1] = a1, c.x[2] = a2, c.x[3] = a3;
    c.x[31] = (saved.x[31] - 0x200) & ~0xfull;
    call(c, offset < kModuleBase ? kModuleBase + offset : offset);
    uint64_t r = c.x[0];
    c = saved;
    return r;
}

void set_focus(CPU& c, uint64_t mgr, uint64_t entry, uint64_t target) {
    uint64_t cur = ld<uint64_t>(entry + 0x10);
    if (cur == target) return;
    if (cur) {
        uint64_t ev = (c.x[31] - 0x40) & ~0xfull;  // {target, player, 0, 0}
        st<uint64_t>(ev, cur);
        st<uint64_t>(ev + 8, 0);
        st<uint64_t>(ev + 0x10, 0);
        uint64_t d = cur + 0x40;
        guest(c, ld<uint64_t>(ld<uint64_t>(d)), d, 1, ev);
        st<uint64_t>(entry + 0x10, 0);
    }
    guest(c, set_focus_addr(), mgr, 0, target, 0);
    guest(c, ld<uint64_t>(ld<uint64_t>(target) + 0x18), target);
    st<float>(entry + 4, ld<float>(target + 0x14a0));
    st<float>(entry + 8, ld<float>(target + 0x14a4));
}

// The enabled target of this manager under (x, y) on the screen (the smallest one if they overlap; else the nearest
// within kNear, unless `nearest` is false: hovering), or 0. With `disabled`, the disabled ones only.
uint64_t target_at(uint64_t mgr, float x, float y, bool disabled = false, bool nearest = true) {
    int32_t off = ld<int32_t>(mgr + 0x14);
    uint64_t best = 0, near = 0;
    float best_area = INFINITY, near_dist = kNear;
    uint64_t node = ld<uint64_t>(mgr + 8);
    for (int k = 0; node && node != mgr && k < 256; k++, node = ld<uint64_t>(node + 8)) {
        uint64_t t = node - off;
        if (bool(ld<uint32_t>(t + 0x14b0) & 1) == disabled) continue;
        float cx = 640.0f + ld<float>(t + 0x14a0) * kLayoutToScreen, cy = 360.0f - ld<float>(t + 0x14a4) * kLayoutToScreen;
        float hw = ld<float>(t + 0x14a8) * kLayoutToScreen * 0.5f, hh = ld<float>(t + 0x14ac) * kLayoutToScreen * 0.5f;
        if (!(hw > 0 && hh > 0 && hw < 2000 && hh < 2000)) continue;
        float dx = std::fabs(x - cx) - hw, dy = std::fabs(y - cy) - hh;  // outside the rectangle by
        if (dx <= kPad && dy <= kPad) {
            if (hw * hh < best_area) best = t, best_area = hw * hh;
        } else {
            float d = std::hypot(std::max(dx, 0.0f), std::max(dy, 0.0f));
            if (d < near_dist) near = t, near_dist = d;
        }
    }
    return best ? best : nearest ? near : 0;
}

bool has_enabled(uint64_t mgr) {
    int32_t off = ld<int32_t>(mgr + 0x14);
    uint64_t node = ld<uint64_t>(mgr + 8);
    for (int k = 0; node && node != mgr && k < 256; k++, node = ld<uint64_t>(node + 8))
        if (ld<uint32_t>(node - off + 0x14b0) & 1) return true;
    return false;
}

// NX_TRACE_TOUCH: a click that missed lists the manager's enabled items (screen rectangles).
void log_targets(uint64_t mgr) {
    int32_t off = ld<int32_t>(mgr + 0x14);
    uint64_t node = ld<uint64_t>(mgr + 8);
    for (int k = 0; node && node != mgr && k < 256; k++, node = ld<uint64_t>(node + 8)) {
        uint64_t t = node - off;
        if (!(ld<uint32_t>(t + 0x14b0) & 1)) continue;
        float cx = 640.0f + ld<float>(t + 0x14a0) * kLayoutToScreen, cy = 360.0f - ld<float>(t + 0x14a4) * kLayoutToScreen;
        float w = ld<float>(t + 0x14a8) * kLayoutToScreen, h = ld<float>(t + 0x14ac) * kLayoutToScreen;
        log("acnh: click missed manager %llx: item %llx at %.0f x %.0f, %.0f x %.0f", (unsigned long long)mgr,
            (unsigned long long)ld<uint64_t>(t + 0x28), cx, cy, w, h);
    }
}

// Hovering: the item under a mouse that moved takes the cursor (the managers of this frame and the next get it).
void focus_hover(CPU& c, uint64_t f) {
    if (g_hover_polled != f) {
        g_hover_polled = f;
        uint32_t x, y;
        if (mouse_take_hover(x, y)) g_hover = {true, x, y, f};
        else if (g_hover.pending && f > g_hover.frame + 1) g_hover.pending = false;
    }
    uint64_t mgr = c.x[0];
    if (!g_hover.pending || !mgr || (c.x[3] & 1) || ld<uint32_t>(mgr + 0x18) == 0) return;
    uint64_t entry = ld<uint64_t>(ld<uint64_t>(mgr + 0x20));
    if (!entry || !ld<uint64_t>(entry + 0x10)) return;  // a list that shows no cursor is left asleep
    uint64_t t = target_at(mgr, float(g_hover.x), float(g_hover.y), false, false);
    if (!t) return;
    g_hover.pending = false;
    if (t == ld<uint64_t>(entry + 0x10)) return;
    if (tracing())
        log("acnh: mouse over %u x %u: item %llx takes the cursor", g_hover.x, g_hover.y,
            (unsigned long long)ld<uint64_t>(t + 0x28));
    set_focus(c, mgr, entry, t);
}

// main+3c48c60 (x0 = the manager, w3 bit 0: input blocked), before it reads this frame's input.
void focus_input(CPU& c) {
    uint64_t f = g_frame.load();
    if (!g_click.pending) focus_hover(c, f);
    if (!g_click.pending) {
        uint32_t x, y;
        uint64_t at;
        if (!touch_take_tap(x, y, at)) return;
        g_click = {true, x, y, at, at + 6, false};
    }
    // A frame later, once guiTouchManager has had its turn with the same click; a few frames to find a taker.
    if (f <= g_click.frame) return;
    if (f > g_click.until || g_gui_click_frame >= g_click.frame) {
        g_click.pending = false;
        return;
    }
    uint64_t mgr = c.x[0];
    if (!mgr || (c.x[3] & 1) || ld<uint32_t>(mgr + 0x18) == 0) return;
    uint64_t entry = ld<uint64_t>(ld<uint64_t>(mgr + 0x20));
    if (!entry) return;
    uint64_t t = target_at(mgr, float(g_click.x), float(g_click.y));
    // A list that shows no cursor yet (all its items disabled) waits for a first button press, as on the console
    // (the style choice of the intro): one A wakes it, then the click is tried again for a while.
    if (!t && !g_click.woke && !ld<uint64_t>(entry + 0x10) && !has_enabled(mgr) &&
        target_at(mgr, float(g_click.x), float(g_click.y), true)) {
        g_click.woke = true;
        g_click.until = f + 45;
        if (tracing()) log("acnh: click at %u x %u on a list without a cursor: A to wake it", g_click.x, g_click.y);
        hid_hold_buttons(1 /* A */, 3);
        return;
    }
    if (!t) {
        if (tracing()) log_targets(mgr);
        return;
    }
    g_click.pending = false;
    if (tracing())
        log("acnh: click at %u x %u: item %llx (target %llx, manager %llx), focus and A", g_click.x, g_click.y,
            (unsigned long long)ld<uint64_t>(t + 0x28), (unsigned long long)t, (unsigned long long)mgr);
    set_focus(c, mgr, entry, t);
    hid_hold_buttons(1 /* A */, 3);
}

// ---- messages ---------------------------------------------------------------------------------------------------

// The layout framework's button handlers (main+3c4b870 runs them after the page's focus manager, main+3c4b1fc each,
// x0 = it, w3 bit 0: input blocked) watch one button each: +0x14 which, +0xbc on, +0x18 their delegates (+8 the
// object they call). The message window's page turner (class main+0x49fc8f0, its handlers on the buttons A and B
// advance the text) is on only while a page is fully shown and waits, the ▼ under it. A click then (one no list item
// took in two frames, anywhere on the screen) presses A; with no message waiting, a click in the world does nothing.
constexpr uint64_t kPageTurner = 0x49fc8f0;

void button_input(CPU& c) {
    uint64_t h = c.x[0];
    if (!h || (c.x[3] & 1) || !ld<uint8_t>(h + 0xbc)) return;
    uint64_t owner = ld<uint64_t>(h + 0x18 + 8);
    if (!owner || ld<uint64_t>(owner) != kModuleBase + kPageTurner) return;
    uint64_t f = g_frame.load();
    if (!g_click.pending) {
        uint32_t x, y;
        uint64_t at;
        if (!touch_take_tap(x, y, at)) return;
        g_click = {true, x, y, at, at + 6, false};
    }
    if (f < g_click.frame + 2) return;
    if (f > g_click.until || g_gui_click_frame >= g_click.frame) {
        g_click.pending = false;
        return;
    }
    g_click.pending = false;
    if (tracing()) log("acnh: click at %u x %u while a message waits: A", g_click.x, g_click.y);
    hid_hold_buttons(1 /* A */, 3);
}

}  // namespace

NX_HOOK_VERSIONS(0x31c3f98, 0x31c3fa8, gui_touch_update);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x3c48c60, 0x3c48c70, focus_input);  // 3.0.2, 3.0.3
NX_HOOK_VERSIONS(0x3c4b1fc, 0x3c4b20c, button_input);  // 3.0.2, 3.0.3

}  // namespace nx
