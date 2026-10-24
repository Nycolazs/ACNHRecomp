// The shared Animal Crossing look (theme.h): style, fonts and icons.
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <vector>

#include "ui/i18n.h"

namespace nx::ui {

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

// A font with Cyrillic and the rest of the extended ranges, merged under the menu font: rounded fonts such as
// Quicksand have no Cyrillic, which then showed as "?" in Russian. Glyphs the menu font has keep its look.
std::string find_cover_font(const std::string& latin) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* p :
         {"/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf", "/usr/share/fonts/noto/NotoSans-Bold.ttf",
          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
          "/usr/share/fonts/truetype/ubuntu/Ubuntu-B.ttf", "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
          "C:\\Windows\\Fonts\\segoeuib.ttf", "C:\\Windows\\Fonts\\arialbd.ttf"})
        if (latin != p && fs::is_regular_file(fs::u8path(p), ec)) return p;
    return "";
}

}  // namespace

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
    std::string latin = find_font(false, user_font), extra = find_font(true, ""), cover = find_cover_font(latin);
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
    if (cjk)
        cb.AddRanges(lang == "ja"      ? io.Fonts->GetGlyphRangesJapanese()
                     : lang == "ko"    ? io.Fonts->GetGlyphRangesKorean()
                     : lang == "zh-TW" ? io.Fonts->GetGlyphRangesChineseFull()
                                       : io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    for (const UiLanguage& l : ui_languages()) cb.AddText(l.native_name);
    cb.AddText("简体中文繁體中文日本語한국어");  // the game's language list (settings.cpp)
    cb.BuildRanges(&cjk_ranges);
    auto add = [&](float px) -> ImFont* {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        ImFont* f = latin.empty() ? nullptr : io.Fonts->AddFontFromFileTTF(latin.c_str(), px, &cfg, ranges.Data);
        if (!f) {
            ImFontConfig d;
            d.SizePixels = px;
            f = io.Fonts->AddFontDefault(&d);
        }
        if (f && !latin.empty() && !cover.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 2;
            io.Fonts->AddFontFromFileTTF(cover.c_str(), px, &m, ranges.Data);
        }
        if (!extra.empty()) {
            ImFontConfig m;
            m.MergeMode = true;
            m.OversampleH = 1;
            io.Fonts->AddFontFromFileTTF(extra.c_str(), px, &m, cjk_ranges.Data);
        }
        return f;
    };
    ThemeFonts fonts;
    fonts.body = add(std::round(20.0f * scale));
    fonts.title = add(std::round(30.0f * scale));
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
    s.FrameBorderSize = 0;
    s.PopupBorderSize = 0;
    s.ScaleAllSizes(scale);
    // Cream paper, brown ink, mint and yellow accents: the game's menus and NookPhone.
    auto rgb = [](int r, int g, int b, float a = 1.0f) { return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a); };
    ImVec4* c = s.Colors;
    c[ImGuiCol_Text] = rgb(107, 79, 54);
    c[ImGuiCol_TextDisabled] = rgb(170, 150, 120);
    c[ImGuiCol_WindowBg] = rgb(255, 249, 232);
    c[ImGuiCol_ChildBg] = rgb(248, 239, 214);
    c[ImGuiCol_PopupBg] = rgb(255, 251, 240);
    c[ImGuiCol_Border] = rgb(230, 214, 178);
    c[ImGuiCol_FrameBg] = rgb(240, 228, 196);
    c[ImGuiCol_FrameBgHovered] = rgb(248, 226, 150);
    c[ImGuiCol_FrameBgActive] = rgb(255, 217, 102);
    c[ImGuiCol_TitleBg] = rgb(124, 194, 103);
    c[ImGuiCol_TitleBgActive] = rgb(124, 194, 103);
    c[ImGuiCol_ScrollbarBg] = rgb(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = rgb(214, 196, 158);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(196, 176, 136);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(170, 150, 110);
    c[ImGuiCol_CheckMark] = rgb(255, 255, 255);
    c[ImGuiCol_SliderGrab] = rgb(78, 186, 160);
    c[ImGuiCol_SliderGrabActive] = rgb(56, 160, 136);
    c[ImGuiCol_Button] = rgb(118, 200, 170);
    c[ImGuiCol_ButtonHovered] = rgb(255, 217, 102);
    c[ImGuiCol_ButtonActive] = rgb(246, 196, 64);
    c[ImGuiCol_Header] = rgb(255, 232, 160);
    c[ImGuiCol_HeaderHovered] = rgb(255, 222, 128);
    c[ImGuiCol_HeaderActive] = rgb(255, 210, 90);
    c[ImGuiCol_Separator] = rgb(230, 214, 178);
    c[ImGuiCol_Tab] = rgb(240, 228, 196);
    c[ImGuiCol_TabHovered] = rgb(255, 222, 128);
    c[ImGuiCol_TabSelected] = rgb(124, 194, 103);
    c[ImGuiCol_PlotHistogram] = rgb(124, 194, 103);
    c[ImGuiCol_NavCursor] = rgb(246, 170, 40);
    c[ImGuiCol_ModalWindowDimBg] = rgb(60, 40, 20, 0.45f);
}

void theme_icon(ImDrawList* dl, int icon, ImVec2 c, float r, ImU32 col) {
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
