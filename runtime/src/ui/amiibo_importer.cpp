#include "ui/amiibo_importer.h"

#include <string>

#include "imgui.h"

#include "nfp_host.h"
#include "platform.h"
#include "ui/i18n.h"
#include "ui/settings.h"

namespace nx::ui {

namespace {

bool g_tag_pick_pending = false;
std::string g_status;

void refresh_tag_status() {
    if (nfp_host::has_tag()) g_status = trf("amiibo.loaded", nfp_host::tag_path());
}

}  // namespace

void amiibo_importer_poll() {
    if (!g_tag_pick_pending) return;
    std::string picked;
    if (!plat::pick_result(picked)) return;
    g_tag_pick_pending = false;
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
}

void settings_page_amiibo(float scale) {
    ImGui::TextWrapped("%s", tr("amiibo.desc"));
    ImGui::Spacing();

    float bw = std::min(ImGui::GetContentRegionAvail().x, 320.0f * scale);
    ImGui::SeparatorText(tr("amiibo.tag_section"));
    ImGui::TextWrapped("%s", tr("amiibo.tag_hint"));
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0));
    if (ImGui::Button(tr("amiibo.choose"), ImVec2(bw, 0))) {
        g_tag_pick_pending = true;
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
