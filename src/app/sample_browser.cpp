#include "app/sample_browser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <system_error>

#include "app/theme.h"
#include "app/widgets.h"
#include "imgui.h"
#include "io/wav.h"

namespace wax::app {

namespace fs = std::filesystem;

bool isSoundFile(const fs::path& file) {
  std::string ext = file.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return ext == ".wav";
}

namespace {

void list(SampleBrowserState& state) {
  state.entries.clear();
  state.selected = -1;
  state.clip.reset();
  state.peaks.clear();
  state.error.clear();
  state.tooBig = false;
  std::error_code ec;
  for (const fs::directory_entry& e : fs::directory_iterator(state.folder, fs::directory_options::skip_permission_denied, ec)) {
    std::error_code inner;
    const bool folder = e.is_directory(inner);
    if (e.path().filename().string().rfind('.', 0) == 0) continue;   // hidden
    if (!folder && !isSoundFile(e.path())) continue;
    state.entries.push_back({e.path().filename().string(), folder, folder ? 0 : e.file_size(inner)});
  }
  if (ec) state.error = "Cannot read this folder: " + ec.message();
  std::sort(state.entries.begin(), state.entries.end(), [](const SampleBrowserState::Entry& a, const SampleBrowserState::Entry& b) {
    if (a.folder != b.folder) return a.folder;
    std::string x = a.name, y = b.name;
    std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return x < y;
  });
}

// Reads the picked file for hearing and drawing.
void read(SampleBrowserState& state, Preview& preview, const fs::path& file) {
  state.clip.reset();
  state.peaks.clear();
  state.error.clear();
  try {
    const StereoAudio audio = readWav(file.string());
    auto clip = std::make_shared<PreviewClip>();
    clip->left = audio.left;
    clip->right = audio.right;
    clip->rate = audio.sampleRate;
    state.rate = audio.sampleRate;
    state.stereo = audio.left != audio.right;
    const int columns = 400;
    const std::size_t frames = audio.left.size();
    for (int c = 0; c < columns; c++) {
      const std::size_t from = frames * static_cast<std::size_t>(c) / columns, to = std::max(from + 1, frames * static_cast<std::size_t>(c + 1) / columns);
      float lo = 0, hi = 0;
      for (std::size_t i = from; i < to && i < frames; i++) { lo = std::min(lo, audio.left[i]); hi = std::max(hi, audio.left[i]); }
      state.peaks.push_back(lo);
      state.peaks.push_back(hi);
    }
    state.clip = clip;
    preview.start(clip);
  } catch (const std::exception& e) {
    state.error = e.what();
  }
}

std::string sizeText(std::uintmax_t bytes) {
  char text[32];
  if (bytes >= 1024 * 1024) std::snprintf(text, sizeof text, "%.1f MB", static_cast<double>(bytes) / (1024 * 1024));
  else std::snprintf(text, sizeof text, "%.0f KB", static_cast<double>(bytes) / 1024);
  return text;
}

}  // namespace

SampleBrowserResult drawSampleBrowser(SampleBrowserState& state, Preview& preview, const fs::path& start, double limitMB, const std::string& replaceId) {
  SampleBrowserResult result;
  const float scale = ImGui::GetStyle().FontScaleDpi;
  if (state.folder.empty()) {
    std::error_code ec;
    state.folder = fs::is_directory(start, ec) ? start : fs::current_path(ec);
    list(state);
  }
  const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(kDimText);

  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * 0.85f);
  ImGui::TextColored(dim, "%s", state.folder.string().c_str());
  ImGui::PopFont();
  if (state.folder.has_parent_path() && state.folder.parent_path() != state.folder && textButton("Up")) {
    preview.stop();
    state.folder = state.folder.parent_path();
    list(state);
  }

  const float listHeight = std::max(60.0f * scale, ImGui::GetContentRegionAvail().y - 150 * scale);
  ImGui::BeginChild("samplelist", ImVec2(0, listHeight), false, ImGuiWindowFlags_NoNav);
  for (int i = 0; i < static_cast<int>(state.entries.size()); i++) {
    const SampleBrowserState::Entry& e = state.entries[static_cast<std::size_t>(i)];
    const std::string label = (e.folder ? "[" + e.name + "]" : e.name) + "##e" + std::to_string(i);
    if (ImGui::Selectable(label.c_str(), i == state.selected, ImGuiSelectableFlags_AllowDoubleClick)) {
      if (e.folder && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        preview.stop();
        state.folder /= e.name;
        list(state);
        break;
      }
      state.selected = i;
      state.error.clear();
      preview.stop();
      if (!e.folder) {
        state.tooBig = static_cast<double>(e.size) > limitMB * 1024 * 1024;
        state.clip.reset();
        state.peaks.clear();
        if (!state.tooBig) read(state, preview, state.folder / e.name);
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && state.clip) { result.load = true; result.file = state.folder / e.name; }
      }
    }
  }
  ImGui::EndChild();

  const SampleBrowserState::Entry* picked = state.selected >= 0 && state.selected < static_cast<int>(state.entries.size()) ? &state.entries[static_cast<std::size_t>(state.selected)] : nullptr;
  if (picked && !picked->folder) {
    ImGui::TextUnformatted(picked->name.c_str());
    if (state.tooBig) {
      ImGui::TextColored(dim, "%s is over the size limit (%.0f MB).", sizeText(picked->size).c_str(), limitMB);
      if (textButton("Preview anyway")) { state.tooBig = false; read(state, preview, state.folder / picked->name); }
    } else if (state.clip) {
      const double seconds = static_cast<double>(state.clip->left.size()) / std::max(1.0, state.rate);
      ImGui::TextColored(dim, "%.2f s  %.0f Hz  %s  %s", seconds, state.rate, state.stereo ? "stereo" : "mono", sizeText(picked->size).c_str());
      const ImVec2 at = ImGui::GetCursorScreenPos();
      const float width = ImGui::GetContentRegionAvail().x, height = 56 * scale;
      ImDrawList* draw = ImGui::GetWindowDrawList();
      draw->AddRectFilled(at, ImVec2(at.x + width, at.y + height), lineAlpha(30));
      const std::size_t columns = state.peaks.size() / 2;
      for (std::size_t c = 0; c < columns; c++) {
        const float x = at.x + width * static_cast<float>(c) / static_cast<float>(columns);
        const float mid = at.y + height * 0.5f;
        draw->AddLine(ImVec2(x, mid - state.peaks[c * 2 + 1] * height * 0.5f), ImVec2(x, mid - state.peaks[c * 2] * height * 0.5f - 1), kAccent);
      }
      if (preview.playing()) { const float x = at.x + width * static_cast<float>(preview.progress()); draw->AddLine(ImVec2(x, at.y), ImVec2(x, at.y + height), kText); }
      ImGui::InvisibleButton("##wave", ImVec2(width, height));
      if (ImGui::IsItemClicked()) preview.start(state.clip);   // a click on the waveform plays it again
      if (textButton(preview.playing() ? "Stop" : "Play")) { if (preview.playing()) preview.stop(); else preview.start(state.clip); }
      ImGui::SameLine(0, 12 * scale);
      if (textButton("Load as a new sample")) { result.load = true; result.file = state.folder / picked->name; }
      if (!replaceId.empty()) {   // (on its own line: the panel is narrow)
        if (textButton(("Replace " + replaceId).c_str())) { result.load = true; result.replace = true; result.file = state.folder / picked->name; }
      }
    }
  }
  if (!state.error.empty()) ImGui::TextColored(ImVec4(0.9f, 0.4f, 0.4f, 1), "%s", state.error.c_str());
  return result;
}

}  // namespace wax::app
