// The shared Animal Crossing look (theme.h): style, fonts and icons.
#include "ui/theme.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <vector>

// stb_truetype, the rasterizer Dear ImGui already uses. A second static copy so a system font can be rejected
// before it is merged: a face ImGui cannot parse fails the whole atlas.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"

#if __has_include(<nx/runtime.h>) && !defined(NX_LAUNCHER_UI)
#include <nx/runtime.h>
#define THEME_LOG(...) nx::log(__VA_ARGS__)
#else  // the launcher: no runtime, stderr
#include <cstdio>
#define THEME_LOG(fmt, ...) std::fprintf(stderr, fmt "\n", __VA_ARGS__)
#endif

#include "ui/i18n.h"
#include "imgui_internal.h"  // the last item's flags and focus (theme_button, theme_nav_focused)
#include "ui/IconsLucide.h"  // IconFontCppHeaders: the codepoints of assets/fonts/Lucide-launcher.ttf

namespace nx::ui {

// Subset of Noto Sans (SIL OFL 1.1): the menu's CJK glyphs when the system face is missing or unreadable.
extern const unsigned char kUiCjkTtf[];
extern const int kUiCjkTtfSize;
extern const unsigned char kUiHangulTtf[];
extern const int kUiHangulTtfSize;

namespace {

// A rounded sans font from the system for the menus, plus a CJK one when the menu language needs it. Searched in
// the usual places of each OS; ImGui's built-in font when none is found.
std::string find_font(bool cjk, const std::string& user_font) {
    namespace fs = std::filesystem;
    std::vector<std::string> c;
    if (cjk) {
        c = {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc", "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
             "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
             "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc", "/System/Library/Fonts/PingFang.ttc",
             "/System/Library/Fonts/Hiragino Sans GB.ttc", "C:\\Windows\\Fonts\\msyhbd.ttc",
             "C:\\Windows\\Fonts\\YuGothB.ttc", "C:\\Windows\\Fonts\\malgunbd.ttf",
             "/system/fonts/NotoSansCJK-Regular.ttc"};
    } else {
        if (!user_font.empty()) c.push_back(user_font);  // a font of the player's choice
        for (const char* p :
             {"/usr/share/fonts/truetype/nunito/Nunito-Bold.ttf", "/usr/share/fonts/truetype/quicksand/Quicksand-Bold.ttf",
              "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
              "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
              "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf", "/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf",
              "/System/Library/Fonts/Helvetica.ttc", "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf",
              "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf"})
            c.push_back(p);
    }
    std::error_code ec;
    for (const auto& p : c)
        if (fs::is_regular_file(fs::u8path(p), ec)) return p;
    return "";
}

// True when stb can draw `cp` from face 0. CFF2 variable fonts (some Android Noto Sans builds) fail this and must
// not be merged: one unreadable face fails the whole atlas, and the menu then falls back to "?" again.
bool file_draws(const std::string& path, int cp) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0 || n > 40 * 1024 * 1024) return false;
    in.seekg(0);
    std::vector<unsigned char> data(static_cast<size_t>(n));
    if (!in.read(reinterpret_cast<char*>(data.data()), n)) return false;
    const int off = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (off < 0) return false;
    stbtt_fontinfo info;
    return stbtt_InitFont(&info, data.data(), off) && stbtt_FindGlyphIndex(&info, cp) != 0;
}

// A font with Cyrillic and the rest of the extended ranges, merged under the menu font: rounded fonts such as
// Quicksand have no Cyrillic, which then showed as "?" in Russian. Glyphs the menu font has keep its look.
std::string find_cover_font(const std::string& latin) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* p :
         {"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
          "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf",
          "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/NotoSans-VF.ttf",
          "/system/fonts/RobotoStatic-Regular.ttf", "/system/fonts/Roboto-Regular.ttf"})
        if (latin != p && fs::is_regular_file(fs::u8path(p), ec) && file_draws(p, 0x0416)) return p;  // Ж
    return "";
}

// Dear ImGui 1.91 draws with stb_truetype, which reads TrueType and CFF but not CFF2. Android 15+ replaced
// /system/fonts/NotoSansCJK-Regular.ttc with a CFF2 variable collection, so merging it failed the atlas and the
// menu kept only a Latin font. Every missing glyph is then "?".
int preferred_cjk_face(const std::string& lang) {
    if (lang == "zh-CN") return 2;  // Noto CJK collection: JP, KR, SC, TC, HK
    if (lang == "zh-TW") return 3;
    if (lang == "ko") return 1;
    return 0;
}

int cjk_face_index(const unsigned char* data, int faces, int prefer) {
    auto draws_han = [&](int i) {
        if (i < 0 || i >= faces) return false;
        const int off = stbtt_GetFontOffsetForIndex(data, i);
        if (off < 0) return false;
        stbtt_fontinfo info;
        if (!stbtt_InitFont(&info, data, off)) return false;
        return stbtt_FindGlyphIndex(&info, 0x4E2D) != 0;  // 中
    };
    if (draws_han(prefer)) return prefer;
    for (int i = 0; i < faces; i++)
        if (draws_han(i)) return i;
    return -1;
}

bool usable_cjk_file(const std::string& path, int prefer, int* index) {
    namespace fs = std::filesystem;
    std::ifstream in(fs::u8path(path), std::ios::binary);
    if (!in) return false;
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0 || n > 80 * 1024 * 1024) return false;
    in.seekg(0);
    std::vector<unsigned char> data(static_cast<size_t>(n));
    if (!in.read(reinterpret_cast<char*>(data.data()), n)) return false;
    int faces = stbtt_GetNumberOfFonts(data.data());
    if (faces <= 0) faces = 1;
    const int face = cjk_face_index(data.data(), faces, prefer);
    if (face < 0) return false;
    *index = face;
    return true;
}

#if defined(__ANDROID__)
bool name_looks_cjk(std::string name) {
    for (char& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
    for (const char* k : {"cjk", "fallback", "misans", "harmony", "sourcehan", "notosanssc", "notosanstc", "notoserif",
                          "droidsans", "hans", "hant", "chinese"})
        if (name.find(k) != std::string::npos) return true;
    return false;
}
#endif

// The first face stb can draw 中 from. `index` is its number inside a collection.
std::string find_cjk_font(const std::string& lang, const std::string& user_font, int* index) {
    namespace fs = std::filesystem;
    const int prefer = preferred_cjk_face(lang);
    std::vector<std::string> c;
    if (!user_font.empty()) c.push_back(user_font);
    for (const char* p : {"/usr/share/fonts/opentype/noto/NotoSansCJK-Bold.ttc",
                          "/usr/share/fonts/noto-cjk/NotoSansCJK-Bold.ttc",
                          "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Bold.ttc",
                          "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                          "/System/Library/Fonts/PingFang.ttc", "/System/Library/Fonts/Hiragino Sans GB.ttc",
                          "C:\\Windows\\Fonts\\msyhbd.ttc", "C:\\Windows\\Fonts\\YuGothB.ttc",
                          "C:\\Windows\\Fonts\\malgunbd.ttf", "/system/fonts/NotoSansCJK-Regular.ttc",
                          "/system/fonts/NotoSansCJK-Regular.otf", "/product/fonts/NotoSansCJK-Regular.ttc",
                          "/system/fonts/NotoSansSC-Regular.otf", "/system/fonts/NotoSansHans-Regular.otf",
                          "/system/fonts/DroidSansFallbackFull.ttf", "/system/fonts/DroidSansFallback.ttf",
                          "/system/fonts/NotoSerifCJK-Regular.ttc", "/product/fonts/NotoSerifCJK-Regular.ttc"})
        c.push_back(p);
    std::error_code ec;
#if defined(__ANDROID__)
    for (const char* dir : {"/system/fonts", "/product/fonts", "/system_ext/fonts"}) {
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
        for (int n = 0; !ec && it != end && n < 400; it.increment(ec), n++) {
            if (!it->is_regular_file(ec)) continue;
            const std::string name = it->path().filename().string();
            if (!name_looks_cjk(name)) continue;
            c.push_back(it->path().string());
        }
        ec.clear();
    }
#endif
    for (const std::string& p : c) {
        if (!fs::is_regular_file(fs::u8path(p), ec)) continue;
        if (usable_cjk_file(p, prefer, index)) return p;
    }
    return "";
}

struct Embedded {
    const void* data = nullptr;
    int size = 0;
};
Embedded g_embedded_body, g_embedded_title, g_embedded_icons;
ImFont* g_icon_font = nullptr;  // the last theme_build_fonts' ThemeFonts::icons (theme_icon)

}  // namespace

void theme_set_embedded_fonts(const void* body, int body_size, const void* title, int title_size) {
    g_embedded_body = {body, body_size};
    g_embedded_title = {title, title_size};
}

void theme_set_embedded_icons(const void* data, int size) { g_embedded_icons = {data, size}; }

float theme_scale(float width, float height) {
    if (height > width) return std::clamp(width / 720.0f, 0.6f, 4.0f);
    return std::clamp(std::min(width / 1280.0f, height / 720.0f), 0.6f, 4.0f);
}

ImU32 theme_rgb(int r, int g, int b, int a) { return IM_COL32(r, g, b, a); }

ThemeFonts theme_build_fonts(float scale, const std::string& user_font) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const std::string& lang = ui_language();
    bool cjk = lang == "ja" || lang == "ko" || lang == "zh-CN" || lang == "zh-TW";
    // The CJK font is merged in every language: in full for ja/ko/zh, else only for the languages' own names in the
    // language lists (日本語, 한국어, 中文).
    int cjk_index = 0;
    std::string latin = find_font(false, user_font), cover = find_cover_font(latin);
    std::string extra = find_cjk_font(lang, user_font, &cjk_index);
    bool embed = extra.empty();  // no system face stb can draw; the Noto subset compiled into the app
    static ImVector<ImWchar> ranges;  // Latin + Latin-1/Extended + Cyrillic + general punctuation + arrows/dashes
    ranges.clear();
    ImFontGlyphRangesBuilder rb;
    rb.AddRanges(io.Fonts->GetGlyphRangesDefault());
    rb.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
    static const ImWchar more[] = {0x0100, 0x024F, 0x2000, 0x206F, 0x2190, 0x21FF, 0x2212, 0x2212, 0x25A0, 0x25FF, 0};
    rb.AddRanges(more);
    rb.BuildRanges(&ranges);
    static ImVector<ImWchar> cjk_ranges;
    cjk_ranges.clear();
    ImFontGlyphRangesBuilder cb;
    if (cjk) {
        // ChineseFull is every ideograph and GetGlyphRangesKorean is every Hangul syllable. Either one, from a
        // complete system face, makes an atlas a phone will not build. The failure path then kept only the Latin
        // font, so Korean (and a traditional menu) became "?" even after simplified Chinese worked. The menu's own
        // characters are added from its strings below; simplified common and the Japanese list stay, they are the
        // size that already fits.
#if defined(__ANDROID__)
        if (lang == "ja") cb.AddRanges(io.Fonts->GetGlyphRangesJapanese());
        else if (lang != "ko") cb.AddRanges(io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
#else
        cb.AddRanges(lang == "ja" ? io.Fonts->GetGlyphRangesJapanese()
                     : lang == "ko" ? io.Fonts->GetGlyphRangesKorean()
                     : lang == "zh-TW" ? io.Fonts->GetGlyphRangesChineseFull()
                                       : io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
#endif
        struct Visit {
            static void fn(const char* s, void* p) { static_cast<ImFontGlyphRangesBuilder*>(p)->AddText(s); }
        };
        ui_visit_texts(lang.c_str(), Visit::fn, &cb);
    }
    for (const UiLanguage& l : ui_languages()) cb.AddText(l.native_name);
    cb.AddText("简体中文繁體中文日本語한국어");  // the game's language list (settings.cpp)
    cb.BuildRanges(&cjk_ranges);
    auto add = [&](float px, const Embedded& own) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = nullptr;
        if (own.data) {
            cfg.FontDataOwnedByAtlas = false;  // static data: the atlas only reads it
            f = io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(own.data), own.size, px, &cfg, ranges.Data);
        } else if (!latin.empty()) {
            f = io.Fonts->AddFontFromFileTTF(latin.c_str(), px, &cfg, ranges.Data);
        }
        if (!f) {
            ImFontConfig d;
            d.SizePixels = px;
            f = io.Fonts->AddFontDefault(&d);
        }
        // Under an embedded font the system's covers what it lacks (Fredoka has no Cyrillic).
        std::string under = own.data && !latin.empty() ? latin : cover;
        if (f && (own.data || !latin.empty()) && !under.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 2;
            io.Fonts->AddFontFromFileTTF(under.c_str(), px, &m, ranges.Data);
        }
        auto merge_embedded = [&] {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 1;
            m.FontDataOwnedByAtlas = false;
            io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiCjkTtf), kUiCjkTtfSize, px, &m, cjk_ranges.Data);
            if (kUiHangulTtfSize > 0) {
                ImFontConfig h;
                h.MergeMode = true;
                h.OversampleH = 1;
                h.FontDataOwnedByAtlas = false;
                io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(kUiHangulTtf), kUiHangulTtfSize, px, &h,
                                               cjk_ranges.Data);
            }
        };
        if (!extra.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 1;
            m.FontNo = cjk_index;
            io.Fonts->AddFontFromFileTTF(extra.c_str(), px, &m, cjk_ranges.Data);
        }
        // A phone's CJK face is often simplified Chinese only (Noto Sans SC, MiSans, HarmonyOS Sans). It draws 中,
        // so it used to be the only face merged, and Hangul, kana and the traditional characters it lacks stayed
        // "?". The embedded subset fills those holes. MergeMode keeps the system glyph where both have one.
        merge_embedded();
        return f;
    };
    // The icons: their own font, so each is drawn at the size it needs.
    auto add_icons = [&]() -> ImFont* {
        if (!g_embedded_icons.data) return nullptr;
        static const ImWchar pua[] = {0xe000, 0xf8ff, 0};
        ImFontConfig cfg;
        cfg.FontDataOwnedByAtlas = false;
        cfg.OversampleH = 2;
        return io.Fonts->AddFontFromMemoryTTF(const_cast<void*>(g_embedded_icons.data), g_embedded_icons.size,
                                              std::round(56.0f * scale), &cfg, pua);
    };
    ThemeFonts fonts;
    fonts.body = add(std::round(20.0f * scale), g_embedded_body);
    fonts.title = add(std::round(30.0f * scale), g_embedded_title);
    fonts.icons = add_icons();
    // A font file the atlas cannot use makes the whole build fail and leaves it empty (0x0): on Android the backend then
    // made a zero-sized texture and the driver crashed. Then: the built-in font alone.
    unsigned char* px = nullptr;
    int w = 0, h = 0;
    auto built = [&] { return io.Fonts->Build() && (io.Fonts->GetTexDataAsRGBA32(&px, &w, &h), w > 0 && h > 0); };
    if (!built() && !extra.empty()) {  // the system face parsed and still would not pack: the embedded subset
        THEME_LOG("ui: the fonts did not build with %s: the embedded CJK font", extra.c_str());
        io.Fonts->Clear();
        extra.clear();
        embed = true;
        fonts.body = add(std::round(20.0f * scale), g_embedded_body);
        fonts.title = add(std::round(30.0f * scale), g_embedded_title);
        fonts.icons = add_icons();
    } else if (embed) {
        THEME_LOG("ui: no system CJK font stb can draw: the embedded Noto subset (%d bytes)", kUiCjkTtfSize);
    }
    if (!built()) {
        THEME_LOG("ui: the fonts did not build (%s, %s, %s): the built-in font instead", latin.c_str(), cover.c_str(),
            extra.c_str());
        io.Fonts->Clear();
        ImFontConfig d;
        d.SizePixels = std::round(20.0f * scale);
        fonts.body = io.Fonts->AddFontDefault(&d);
        ImFontConfig t;
        t.SizePixels = std::round(30.0f * scale);
        fonts.icons = nullptr;
        fonts.title = io.Fonts->AddFontDefault(&t);
        io.Fonts->Build();
    }
    g_icon_font = fonts.icons;
    return fonts;
}

void theme_apply_style(float scale) {
    ImGuiStyle& s = ImGui::GetStyle();
    s = ImGuiStyle();
    s.WindowRounding = 28.0f;
    s.ChildRounding = 22.0f;
    s.FrameRounding = 14.0f;
    s.PopupRounding = 16.0f;
    s.GrabRounding = 14.0f;
    s.TabRounding = 14.0f;
    s.ScrollbarRounding = 12.0f;
    s.WindowPadding = ImVec2(26, 22);
    s.FramePadding = ImVec2(14, 8);
    s.ItemSpacing = ImVec2(12, 12);
    s.ItemInnerSpacing = ImVec2(10, 8);
    s.ScrollbarSize = 14.0f;
    s.GrabMinSize = 22.0f;
    s.WindowBorderSize = 0;
    s.ChildBorderSize = 0;
    s.FrameBorderSize = 1.5f;  // dropdowns, sliders and fields: paper edged in deep sand, apart from the sand
    s.PopupBorderSize = 0;
    s.ScaleAllSizes(scale);
    s.FrameRounding *= 2;  // pills (clamped to half the height)
    s.PopupRounding *= 1.4f;
    s.WindowRounding *= 30.0f / 28.0f;
    // ACNHRecomp-Importer's island notice board: sand, a paper board, bark ink, lagoon for focus and sliders, sun
    // yellow for the chosen.
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    auto u = [](ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = u(kThemeInk);
    c[ImGuiCol_TextDisabled] = rgb(176, 158, 136);
    c[ImGuiCol_WindowBg] = u(kThemeBoard);
    c[ImGuiCol_ChildBg] = u(kThemeSand);
    c[ImGuiCol_PopupBg] = u(kThemeBoard);
    c[ImGuiCol_Border] = u(kThemeSandDeep);
    c[ImGuiCol_FrameBg] = u(kThemeBoard);
    c[ImGuiCol_FrameBgHovered] = rgb(255, 250, 230);
    c[ImGuiCol_FrameBgActive] = u(kThemeSun);
    c[ImGuiCol_TitleBg] = u(kThemeLeaf);
    c[ImGuiCol_TitleBgActive] = u(kThemeLeaf);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(226, 212, 174);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(208, 192, 150);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(190, 172, 128);
    c[ImGuiCol_CheckMark] = u(kThemeLeafDeep);
    c[ImGuiCol_SliderGrab] = u(kThemeLagoon);
    c[ImGuiCol_SliderGrabActive] = u(kThemeLagoonDeep);
    c[ImGuiCol_Button] = u(kThemeBoard);  // also a dropdown's arrow box: the same as its frame
    c[ImGuiCol_ButtonHovered] = rgb(255, 250, 230);
    c[ImGuiCol_ButtonActive] = u(kThemeSandDeep);
    c[ImGuiCol_Header] = rgb(250, 236, 168);
    c[ImGuiCol_HeaderHovered] = rgb(249, 226, 128);
    c[ImGuiCol_HeaderActive] = u(kThemeSun);
    c[ImGuiCol_Separator] = u(kThemeSandDeep);
    c[ImGuiCol_Tab] = u(kThemeSand);
    c[ImGuiCol_TabHovered] = rgb(249, 226, 128);
    c[ImGuiCol_TabSelected] = u(kThemeLeaf);
    c[ImGuiCol_PlotHistogram] = u(kThemeLeaf);
    c[ImGuiCol_NavCursor] = u(kThemeLagoonDeep);
    c[ImGuiCol_ModalWindowDimBg] = rgb(107, 84, 68, 0.4f);
}

ImU32 theme_shade(ImU32 c, float k) {
    auto ch = [&](int s) {
        float v = float((c >> s) & 0xFF);
        v = k < 1 ? v * k : v + (255 - v) * (k - 1);
        return int(std::clamp(v, 0.0f, 255.0f));
    };
    return IM_COL32(ch(IM_COL32_R_SHIFT), ch(IM_COL32_G_SHIFT), ch(IM_COL32_B_SHIFT), (c >> IM_COL32_A_SHIFT) & 0xFF);
}

ImU32 theme_mix(ImU32 a, ImU32 b, float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    ImVec4 x = ImGui::ColorConvertU32ToFloat4(a), y = ImGui::ColorConvertU32ToFloat4(b);
    return ImGui::ColorConvertFloat4ToU32(ImVec4(x.x + (y.x - x.x) * t, x.y + (y.y - x.y) * t, x.z + (y.z - x.z) * t,
                                                 x.w + (y.w - x.w) * t));
}

ImU32 theme_ink_on(ImU32 c) {
    float l = (0.299f * float(c & 0xFF) + 0.587f * float((c >> 8) & 0xFF) + 0.114f * float((c >> 16) & 0xFF)) / 255;
    return l > 0.62f ? kThemeInk : kThemeBoard;
}

void theme_leaf(ImDrawList* dl, ImVec2 c, float size, float turn) {
    float cs = std::cos(turn), sn = std::sin(turn), w = size * 0.42f, h = size * 0.5f;
    auto P = [&](float x, float y) { return ImVec2(c.x + x * cs - y * sn, c.y + x * sn + y * cs); };
    dl->PathLineTo(P(0, h));
    dl->PathBezierCubicCurveTo(P(-w * 1.25f, h * 0.35f), P(-w * 0.9f, -h * 0.75f), P(0, -h));
    dl->PathBezierCubicCurveTo(P(w * 0.9f, -h * 0.75f), P(w * 1.25f, h * 0.35f), P(0, h));
    dl->PathFillConvex(kThemeLeaf);
    dl->AddLine(P(0, h * 1.12f), P(0, -h * 0.62f), kThemeLeafDeep, size * 0.055f);
    for (float t : {0.35f, -0.1f}) {
        dl->AddLine(P(0, h * t), P(-w * 0.5f, h * (t - 0.32f)), kThemeLeafDeep, size * 0.04f);
        dl->AddLine(P(0, h * t), P(w * 0.5f, h * (t - 0.32f)), kThemeLeafDeep, size * 0.04f);
    }
}

void theme_stitch(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, float scale) {
    dl->PathRect(a, b, rounding);
    ImVector<ImVec2> pts;
    pts.swap(dl->_Path);
    dl->PathClear();
    if (pts.Size < 2) return;
    pts.push_back(pts[0]);
    const float on = 6 * scale, period = 12 * scale, thick = 2 * scale;
    float d = 0;
    for (int k = 1; k < pts.Size; k++) {
        ImVec2 p = pts[k - 1], q = pts[k];
        float len = std::sqrt((q.x - p.x) * (q.x - p.x) + (q.y - p.y) * (q.y - p.y));
        for (float s = 0; s < len;) {
            float phase = std::fmod(d + s, period), step = std::min(len - s, phase < on ? on - phase : period - phase);
            if (phase < on) {
                float t0 = s / len, t1 = (s + step) / len;
                dl->AddLine(ImVec2(p.x + (q.x - p.x) * t0, p.y + (q.y - p.y) * t0),
                            ImVec2(p.x + (q.x - p.x) * t1, p.y + (q.y - p.y) * t1), kThemeSandDeep, thick);
            }
            s += std::max(step, 0.01f);
        }
        d += len;
    }
}

void theme_dashes(ImDrawList* dl, ImVec2 p, float w, float scale) {
    const float h = 1.25f * scale;
    for (float x = p.x; x + 7 * scale <= p.x + w; x += 14 * scale)
        dl->AddRectFilled(ImVec2(x, p.y), ImVec2(x + 7 * scale, p.y + 2 * h), kThemeSandDeep, h);
}

void theme_lip(ImDrawList* dl, ImVec2 p, ImVec2 size, float lip, float rounding, ImU32 face, ImU32 lip_col, ImU32 border,
               float border_w, bool down, float lift) {
    if (down) lift = 0;
    if (!down)
        dl->AddRectFilled(ImVec2(p.x, p.y + lip - lift), ImVec2(p.x + size.x, p.y + size.y + lip), lip_col, rounding);
    ImVec2 a(p.x, p.y + (down ? lip : -lift)), b(a.x + size.x, a.y + size.y);
    dl->AddRectFilled(a, b, face, rounding);
    if (border_w > 0) dl->AddRect(a, b, border, rounding, 0, border_w);
}

float theme_hover(bool clickable) {
    bool hov = clickable && ImGui::IsItemHovered();
    if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    float* v = ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID() ^ 0x5eedu, 0.0f);
    float target = hov ? 1.0f : 0.0f;
    *v += (target - *v) * std::min(1.0f, ImGui::GetIO().DeltaTime * 18.0f);
    if (std::fabs(target - *v) < 0.01f) *v = target;
    return *v;
}

bool theme_nav_focused() { return ImGui::IsItemFocused() && ImGui::GetCurrentContext()->NavCursorVisible; }

bool theme_button(const char* label, ImVec2 size, ImU32 face) {
    const float h = ImGui::GetFrameHeight(), lip = h * 0.1f;
    const char* end = ImGui::FindRenderedTextEnd(label);
    const ImVec2 ts = ImGui::CalcTextSize(label, end);
    float w = size.x > 0 ? size.x : size.x < 0 ? ImGui::GetContentRegionAvail().x : ts.x + ImGui::GetStyle().FramePadding.x * 2;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(w, h + lip), ImGuiButtonFlags_EnableNav);
    ImGui::PopStyleColor();
    const bool enabled = !(ImGui::GetCurrentContext()->LastItemData.ItemFlags & ImGuiItemFlags_Disabled);
    const bool quiet = face == kThemeBoard;
    ImU32 lip_col = face == kThemeSun ? kThemeSunDeep : quiet ? kThemeSandDeep : theme_shade(face, 0.8f);
    ImU32 ink = theme_ink_on(face), border = lip_col;
    const float t = enabled ? theme_hover() : 0.0f;
    const bool down = enabled && ImGui::IsItemActive(), focus = enabled && theme_nav_focused();
    if (!enabled) face = kThemeSandDeep, lip_col = IM_COL32(224, 211, 172, 255), border = lip_col, ink = kThemeInkSoft;
    else face = theme_mix(face, quiet ? kThemeBoard : theme_shade(face, 1.15f), t), border = quiet ? theme_mix(lip_col, kThemeLagoon, t) : lip_col;
    const float lift = down ? 0 : t * lip * 0.6f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float bw = ImGui::GetStyle().FrameBorderSize;
    theme_lip(dl, p, ImVec2(w, h), lip, h * 0.5f, face, lip_col, focus ? kThemeLagoonDeep : border, focus ? bw * 2 : bw,
              down, lift);
    dl->AddText(ImVec2(p.x + (w - ts.x) * 0.5f, p.y + (down ? lip : -lift) + (h - ts.y) * 0.5f), ink, label, end);
    return pressed && enabled;
}

void theme_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col) {
    // Lucide's glyph, about the line icons' 1.4 r wide.
    if (g_icon_font && icon != kIconPlay) {
        const char* g = nullptr;
        switch (icon) {
            case kIconLeaf: g = ICON_LC_LEAF; break;
            case kIconScreen: g = ICON_LC_MONITOR; break;
            case kIconNote: g = ICON_LC_MUSIC; break;
            case kIconController: g = ICON_LC_GAMEPAD_2; break;
            case kIconHouse: g = ICON_LC_HOUSE; break;
            case kIconFolder: g = ICON_LC_FOLDER_OPEN; break;
            case kIconKey: g = ICON_LC_KEY; break;
            case kIconBox: g = ICON_LC_PACKAGE; break;
            case kIconPlus: g = ICON_LC_PLUS; break;
            case kIconCheck: g = ICON_LC_CHECK; break;
            case kIconCross: g = ICON_LC_X; break;
            case kIconGear: g = ICON_LC_SETTINGS; break;
            case kIconGlobe: g = ICON_LC_GLOBE; break;
            case kIconWarning: g = ICON_LC_TRIANGLE_ALERT; break;
            case kIconText: g = ICON_LC_FILE_TEXT; break;
            default: g = ICON_LC_INFO; break;
        }
        const float fs = r * 1.7f;
        const ImVec2 ts = g_icon_font->CalcTextSizeA(fs, FLT_MAX, 0, g);
        dl->AddText(g_icon_font, fs, ImVec2(std::round(c.x - ts.x * 0.5f), std::round(c.y - ts.y * 0.5f)), col, g);
        return;
    }
    float t = std::max(1.5f, r * 0.14f);
    auto P = [&](float x, float y) { return ImVec2(c.x + r * x, c.y + r * y); };
    switch (icon) {
        case kIconLeaf:
            dl->AddBezierQuadratic(P(-0.6f, 0.6f), P(-0.7f, -0.8f), P(0.6f, -0.6f), col, t);
            dl->AddBezierQuadratic(P(-0.6f, 0.6f), P(0.8f, 0.7f), P(0.6f, -0.6f), col, t);
            dl->AddLine(P(-0.6f, 0.6f), P(0.2f, -0.2f), col, t);
            break;
        case kIconScreen:
            dl->AddRect(P(-0.7f, -0.5f), P(0.7f, 0.35f), col, r * 0.12f, 0, t);
            dl->AddLine(P(-0.3f, 0.65f), P(0.3f, 0.65f), col, t);
            break;
        case kIconNote:
            dl->AddCircleFilled(P(-0.3f, 0.45f), r * 0.25f, col);
            dl->AddLine(P(-0.07f, 0.45f), P(-0.07f, -0.6f), col, t);
            dl->AddLine(P(-0.07f, -0.6f), P(0.5f, -0.35f), col, t);
            break;
        case kIconController:
            dl->AddRect(P(-0.75f, -0.35f), P(0.75f, 0.4f), col, r * 0.35f, 0, t);
            dl->AddLine(P(-0.45f, 0), P(-0.15f, 0), col, t);
            dl->AddLine(P(-0.3f, -0.15f), P(-0.3f, 0.15f), col, t);
            dl->AddCircleFilled(P(0.35f, 0), r * 0.1f, col);
            break;
        case kIconHouse:
            dl->AddTriangle(P(0, -0.7f), P(-0.75f, -0.05f), P(0.75f, -0.05f), col, t);
            dl->AddRect(P(-0.5f, -0.05f), P(0.5f, 0.6f), col, 0, 0, t);
            break;
        case kIconFolder:
            dl->AddRect(P(-0.75f, -0.4f), P(0.75f, 0.55f), col, r * 0.12f, 0, t);
            dl->AddLine(P(-0.75f, -0.4f), P(-0.7f, -0.6f), col, t);
            dl->AddLine(P(-0.7f, -0.6f), P(-0.15f, -0.6f), col, t);
            dl->AddLine(P(-0.15f, -0.6f), P(-0.05f, -0.4f), col, t);
            break;
        case kIconKey:
            dl->AddCircle(P(-0.35f, 0), r * 0.32f, col, 0, t);
            dl->AddLine(P(-0.03f, 0), P(0.75f, 0), col, t);
            dl->AddLine(P(0.55f, 0), P(0.55f, 0.3f), col, t);
            dl->AddLine(P(0.75f, 0), P(0.75f, 0.25f), col, t);
            break;
        case kIconBox:
            dl->AddRect(P(-0.65f, -0.3f), P(0.65f, 0.6f), col, r * 0.08f, 0, t);
            dl->AddLine(P(-0.75f, -0.3f), P(0.75f, -0.3f), col, t);
            dl->AddLine(P(0, -0.3f), P(0, 0.6f), col, t);
            dl->AddBezierQuadratic(P(0, -0.3f), P(-0.5f, -0.9f), P(-0.15f, -0.3f), col, t);
            dl->AddBezierQuadratic(P(0, -0.3f), P(0.5f, -0.9f), P(0.15f, -0.3f), col, t);
            break;
        case kIconPlus:
            dl->AddLine(P(-0.55f, 0), P(0.55f, 0), col, t * 1.3f);
            dl->AddLine(P(0, -0.55f), P(0, 0.55f), col, t * 1.3f);
            break;
        case kIconCheck:
            dl->AddLine(P(-0.5f, 0.02f), P(-0.12f, 0.4f), col, t * 1.4f);
            dl->AddLine(P(-0.12f, 0.4f), P(0.55f, -0.38f), col, t * 1.4f);
            break;
        case kIconCross:
            dl->AddLine(P(-0.4f, -0.4f), P(0.4f, 0.4f), col, t * 1.4f);
            dl->AddLine(P(-0.4f, 0.4f), P(0.4f, -0.4f), col, t * 1.4f);
            break;
        case kIconPlay:
            dl->AddTriangleFilled(P(-0.35f, -0.55f), P(-0.35f, 0.55f), P(0.6f, 0), col);
            break;
        case kIconGear:
            for (int k = 0; k < 8; k++) {
                float a = float(k) * 3.14159265f / 4;
                dl->AddLine(P(std::cos(a) * 0.45f, std::sin(a) * 0.45f), P(std::cos(a) * 0.72f, std::sin(a) * 0.72f),
                            col, t * 1.4f);
            }
            dl->AddCircle(c, r * 0.45f, col, 0, t);
            dl->AddCircle(c, r * 0.16f, col, 0, t);
            break;
        case kIconGlobe:
            dl->AddCircle(c, r * 0.7f, col, 0, t);
            dl->AddEllipse(c, ImVec2(r * 0.3f, r * 0.7f), col, 0, 0, t);
            dl->AddLine(P(-0.7f, 0), P(0.7f, 0), col, t);
            break;
        case kIconWarning:
            dl->AddTriangle(P(0, -0.7f), P(-0.75f, 0.6f), P(0.75f, 0.6f), col, t);
            dl->AddLine(P(0, -0.25f), P(0, 0.2f), col, t);
            dl->AddCircleFilled(P(0, 0.4f), t * 0.7f, col);
            break;
        case kIconText:
            dl->AddRect(P(-0.6f, -0.7f), P(0.6f, 0.7f), col, r * 0.1f, 0, t);
            for (float y : {-0.35f, 0.0f, 0.35f}) dl->AddLine(P(-0.35f, y), P(0.35f, y), col, t);
            break;
        default:  // info
            dl->AddCircle(c, r * 0.7f, col, 0, t);
            dl->AddLine(P(0, -0.05f), P(0, 0.4f), col, t);
            dl->AddCircleFilled(P(0, -0.32f), t * 0.8f, col);
            break;
    }
}

}  // namespace nx::ui
