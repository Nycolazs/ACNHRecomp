#include "ui/amiibo_importer.h"

#include <string>

#include "imgui.h"

#include <nx/runtime.h>

#include "amiibo_decode.h"
#include "nfp_host.h"
#include "platform.h"
#include "ui/i18n.h"
#include "ui/settings.h"

namespace nx::ui {

namespace {

enum class Pick { None, Tag, Keys };

Pick g_pick = Pick::None;
std::string g_status;
std::string g_keys_feedback;  // result of the last key import attempt (not overwritten while keys still missing)

void refresh_tag_status() {
    if (nfp_host::has_tag()) g_status = trf("amiibo.loaded", nfp_host::tag_path());
}

}  // namespace

void amiibo_importer_poll() {
    if (g_pick == Pick::None) return;
    std::string picked;
    if (!plat::pick_result(picked)) return;
    const Pick what = g_pick;
    g_pick = Pick::None;
    if (what == Pick::Tag) {
        if (picked.empty()) {
            g_status = tr("amiibo.cancelled");
        } else {
            std::string err;
            if (nfp_host::load_tag_file(picked, &err)) {
                g_status = trf("amiibo.loaded", picked);
                nfp_host::signal_activate_if_pending();
            } else {
                g_status = err.empty() ? tr("amiibo.load_fail") : err;
            }
        }
        refresh_tag_status();
    } else if (what == Pick::Keys) {
        if (picked.empty()) {
            g_keys_feedback = tr("amiibo.keys_cancelled");
        } else {
            std::string err;
            if (amiibo::install_master_keys_file(picked, &err)) {
                g_keys_feedback = tr("amiibo.keys_installed");
            } else {
                g_keys_feedback = err.empty() ? tr("amiibo.keys_install_fail") : err;
            }
        }
    }
}

void settings_page_amiibo(float scale) {
    (void)scale;
    ImGui::TextWrapped("%s", tr("amiibo.desc"));
    ImGui::Spacing();

    const bool have_keys = amiibo::master_keys_available();

    ImGui::SeparatorText(tr("amiibo.keys_section"));
    ImGui::TextWrapped("%s", tr("amiibo.keys_hint"));
    if (have_keys) {
        ImGui::TextWrapped("%s", tr("amiibo.keys_saved"));
    } else {
        ImGui::TextWrapped("%s", tr("amiibo.keys_missing"));
        if (!g_keys_feedback.empty()) ImGui::TextWrapped("%s", g_keys_feedback.c_str());
    }
    ImGui::Spacing();

    float bw = std::min(ImGui::GetContentRegionAvail().x, 320.0f * scale);
    ImGui::BeginDisabled(have_keys);
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("amiibo.keys_choose"), ImVec2(bw, 0))) {
        g_pick = Pick::Keys;
        plat::pick_file(tr("amiibo.keys_pick_title"), "bin");
    }
    ImGui::PopStyleColor();
    settings_nav_item();
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::SeparatorText(tr("amiibo.tag_section"));
    ImGui::TextWrapped("%s", tr("amiibo.tag_hint"));
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("amiibo.choose"), ImVec2(bw, 0))) {
        g_pick = Pick::Tag;
        plat::pick_file(tr("amiibo.pick_title"), "bin");
    }
    ImGui::PopStyleColor();
    settings_nav_item();
    ImGui::SameLine();
    ImGui::BeginDisabled(!nfp_host::has_tag());
    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("amiibo.clear"), ImVec2(bw, 0))) {
        nfp_host::clear_tag();
        g_status = tr("amiibo.cleared");
    }
    ImGui::PopStyleColor();
    settings_nav_item();
    ImGui::EndDisabled();

    refresh_tag_status();
    if (!g_status.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", g_status.c_str());
    }
}

}  // namespace nx::ui
