#pragma once

namespace nx::ui {

// Settings tab (settings.cpp): load a virtual Amiibo for nn::nfp.
void settings_page_amiibo(float scale);
void amiibo_importer_poll();  // main thread: file-picker results (call while settings are open)

}  // namespace nx::ui
