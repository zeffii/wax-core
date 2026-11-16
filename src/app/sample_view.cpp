#include "app/sample_view.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

#include "app/icons.h"
#include "app/theme.h"
#include "app/widgets.h"
#include "imgui.h"
#include "project/samples.h"
#include "project/spectrogram.h"

namespace wax::app {

namespace {

constexpr float kGrab = 6;   // pixels either side of a marker that grab it
constexpr int kRootNote = 60;
const ImU32 kLoopColour = IM_COL32(90, 150, 255, 255), kSusColour = IM_COL32(255, 170, 40, 255), kSliceColour = IM_COL32(235, 80, 70, 255);

// The computer keyboard as a piano, the same keys as the pattern screen: two rows, the lower from C of the octave.
const ImGuiKey kKeys[] = {
    ImGuiKey_Z, ImGuiKey_S, ImGuiKey_X, ImGuiKey_D, ImGuiKey_C, ImGuiKey_V, ImGuiKey_G, ImGuiKey_B, ImGuiKey_H, ImGuiKey_N,
    ImGuiKey_J, ImGuiKey_M, ImGuiKey_Comma,
    ImGuiKey_Q, ImGuiKey_2, ImGuiKey_W, ImGuiKey_3, ImGuiKey_E, ImGuiKey_R, ImGuiKey_5, ImGuiKey_T, ImGuiKey_6, ImGuiKey_Y,
    ImGuiKey_7, ImGuiKey_U, ImGuiKey_I};

std::vector<std::string> sampleIds(const Project& project) {
  std::vector<std::string> ids;
  if (project.samples.is_object()) for (const auto& [id, sample] : project.samples.items()) if (!isPoolSample(id)) ids.push_back(id);
  return ids;
}

// The view: the frames shown. A zoom of 0, 0 shows the whole sample.
struct Span { double from, to; };
Span spanOf(const SampleView& v, int frames) {
  const double to = v.viewTo > 0 ? std::min<double>(v.viewTo, frames) : frames;
  const double from = std::min<double>(v.viewFrom, std::max(0.0, to - 1));
  return {from, std::max(to, from + 1)};
}

void setView(SampleView& v, double from, double to, int frames) {
  const double span = std::min<double>(frames, std::max(16.0, std::round(to - from)));
  from = std::min(std::max(0.0, std::round(from)), frames - span);
  if (span >= frames) { v.viewFrom = 0; v.viewTo = 0; } else { v.viewFrom = from; v.viewTo = from + span; }
}

// Min and max of the sample for each pixel column.
std::vector<float> computePeaks(const std::vector<float>& data, int width, double from, double to) {
  std::vector<float> peaks(static_cast<std::size_t>(width) * 2, 0.0f);
  const double per = (to - from) / width;
  const int n = static_cast<int>(data.size());
  for (int x = 0; x < width; x++) {
    const int a = std::min(n, static_cast<int>(from + std::floor(x * per)));
    const int b = std::min(n, std::max(a + 1, static_cast<int>(from + std::floor((x + 1) * per))));
    float lo = 1e9f, hi = -1e9f;
    for (int i = a; i < b; i++) { lo = std::min(lo, data[static_cast<std::size_t>(i)]); hi = std::max(hi, data[static_cast<std::size_t>(i)]); }
    peaks[static_cast<std::size_t>(2 * x)] = lo > 1e8f ? 0 : lo;
    peaks[static_cast<std::size_t>(2 * x + 1)] = hi < -1e8f ? 0 : hi;
  }
  return peaks;
}

std::string hex4(int frame) {
  char text[16];
  std::snprintf(text, sizeof text, "%04X", frame & 0xffff);
  return text;
}

// What a marker shows for its place, as an Off cell would need it: 0000..FFFF through the sample.
std::string sliceHex(int frame, int frames) {
  return hex4(static_cast<int>(std::lround(static_cast<double>(frame) / std::max(1, frames - 1) * 65535)));
}

// Applies `change` to a decoded copy of the sample and commits it as one undoable edit. A refusal (the tool throws) becomes the status.
template <class Change>
bool commit(SampleView& v, Document& document, const std::string& label, bool fold, Change change) {
  try {
    EditableSample s = readSample(document.project().samples[v.selected]);
    change(s);
    document.apply(std::make_unique<SetSample>(v.selected, writeSample(s, document.project().samples[v.selected]), label, fold), fold);
    v.status.clear();
    return true;
  } catch (const std::runtime_error& e) {
    v.status = e.what();
    return false;
  }
}

std::optional<SampleRange> selectionOf(const SampleView& v) {
  if (v.selTo > v.selFrom) return SampleRange{v.selFrom, v.selTo};
  return std::nullopt;
}

void select(SampleView& v, const std::string& id) {
  if (id == v.selected) return;
  v.selected = id;
  v.selFrom = v.selTo = v.head = 0;
  v.activeSlice = -1;
  v.viewFrom = v.viewTo = 0;
}

// The edits: each tool by name, applied to the selection (or all of the sample), then the selection is tidied.
bool runTool(SampleView& v, Document& document, const std::string& tool, const char* label) {
  const std::optional<SampleRange> range = selectionOf(v);
  if (!commit(v, document, label, false, [&](EditableSample& s) { applySampleTool(s, tool, range); })) return false;
  if (tool == "crop" || tool == "cut") v.selFrom = v.selTo = 0;
  v.head = std::min(v.head, readSample(document.project().samples[v.selected]).frames());
  return true;
}

void copySelection(SampleView& v, Document& document, bool cut) {
  if (!(v.selTo > v.selFrom)) { v.status = "Select a range first (drag on the waveform), then copy it."; return; }
  try {
    v.clip = copyRange(readSample(document.project().samples[v.selected]), SampleRange{v.selFrom, v.selTo});
    v.status = "Copied " + std::to_string(v.selTo - v.selFrom) + " frames.";
  } catch (const std::runtime_error& e) { v.status = e.what(); return; }
  if (cut) runTool(v, document, "cut", "Cut");
}

void pasteClipAtHead(SampleView& v, Document& document, PasteMode mode) {
  if (v.clip.left.empty()) { v.status = "Nothing copied yet: select a range and press Ctrl+C."; return; }
  const int at = v.head, len = static_cast<int>(v.clip.left.size());
  if (!commit(v, document, mode == PasteMode::Insert ? "Paste" : "Paste over", false, [&](EditableSample& s) { pasteClip(s, at, v.clip, mode); })) return;
  v.selFrom = at; v.selTo = at + len; v.head = at + len;
}

// Plays the sample through the preview, pitched by `note` (the root note plays it at its own rate), from frame `from`.
void audition(SampleView& v, Preview& preview, int note, int from) {
  const EditableSample& s = v.cache;
  if (s.frames() < 2 || from >= s.frames() - 1) return;
  auto clip = std::make_shared<PreviewClip>();
  clip->left.assign(s.left.begin() + from, s.left.end());
  clip->right = s.stereo() ? std::vector<float>(s.right.begin() + from, s.right.end()) : clip->left;
  const double volume = s.number("volume", 1) * s.number("globalVolume", 1), pan = s.number("pan", 0);
  const float gl = static_cast<float>(volume * (pan > 0 ? 1 - pan : 1)), gr = static_cast<float>(volume * (pan < 0 ? 1 + pan : 1));
  for (float& f : clip->left) f *= gl;
  for (float& f : clip->right) f *= gr;
  clip->rate = s.rate * std::pow(2.0, (note - kRootNote) / 12.0);
  preview.start(std::move(clip));
}

// A frame count as a spin box: the arrows step by 1, Shift 100, Ctrl 1000. Returns the new value, or `value` when it did not move.
int spinFrames(const char* id, int value, float width) {
  char text[24];
  std::snprintf(text, sizeof text, "%d", value);
  const int dir = spinBox(id, text, width);
  if (!dir) return value;
  const ImGuiIO& io = ImGui::GetIO();
  return value + dir * (io.KeyCtrl ? 1000 : io.KeyShift ? 100 : 1);
}

}  // namespace

bool drawSampleScreen(SampleView& v, Document& document, Preview& preview, float scale) {
  bool changed = false;
  const std::vector<std::string> ids = sampleIds(document.project());
  if (ids.empty()) {
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kDimText), "No samples yet. Load a sound file in the browser's Samples tab.");
    return false;
  }
  if (std::find(ids.begin(), ids.end(), v.selected) == ids.end()) select(v, ids.front());
  if (v.cacheId != v.selected || v.cacheRevision != document.revision()) {   // decode once per change, not once per frame
    v.cache = readSample(document.project().samples[v.selected]);
    v.cacheId = v.selected;
    v.cacheRevision = document.revision();
    v.peaksKey.clear();
    const int n = v.cache.frames();
    v.selTo = std::min(v.selTo, n);
    v.selFrom = std::min(v.selFrom, v.selTo);
    v.head = std::min(v.head, n);
  }
  EditableSample& s = v.cache;
  const int frames = s.frames();
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec4 dim = ImGui::ColorConvertU32ToFloat4(kDimText);

  // ---- the list ----
  ImGui::BeginChild("samplelist", ImVec2(170 * scale, 0), false);
  for (const std::string& id : ids) {
    ImGui::PushID(id.c_str());
    if (textButton(id.c_str(), id == v.selected)) select(v, id);
    ImGui::PopID();
  }
  ImGui::EndChild();
  ImGui::SameLine();
  ImGui::BeginGroup();

  // ---- the settings, side by side ----
  const bool stereo = s.stereo();
  {   // the name (Enter renames it; the instruments that play it follow), then what it is, and the remove button
    char name[64];
    std::snprintf(name, sizeof name, "%s", v.selected.c_str());
    ImGui::SetNextItemWidth(150 * scale);
    if (ImGui::InputText("##samplename", name, sizeof name, ImGuiInputTextFlags_EnterReturnsTrue) && name[0] && v.selected != name) {
      const std::string wanted = sampleIdFromName(name);
      const std::string next = document.project().samples.contains(wanted) && wanted != v.selected ? uniqueSampleId(document.project().samples, wanted) : wanted;
      document.apply(std::make_unique<RenameSample>(v.selected, next));
      v.selected = v.cacheId = next;
      changed = true;
    }
    ImGui::SameLine();
    ImGui::TextColored(dim, "%d frames   %.2f s   %s", frames, s.rate > 0 ? frames / s.rate : 0.0, stereo ? "stereo" : "mono");
    ImGui::SameLine(0, 18 * scale);
    if (textButton(v.spectrogram ? "Waveform" : "Spectrogram")) v.spectrogram = !v.spectrogram;
    ImGui::SameLine(0, 18 * scale);
    if (textButton("Remove")) {
      const std::vector<std::string> users = instrumentsPlaying(document.project(), v.selected);
      if (!users.empty()) v.status = "\"" + v.selected + "\" is used by " + users.front() + (users.size() > 1 ? " and others." : ".");
      else {
        const std::string gone = v.selected;
        const auto at = std::find(ids.begin(), ids.end(), gone) - ids.begin();
        document.apply(std::make_unique<RemoveSample>(gone));
        changed = true;
        const std::vector<std::string> rest = sampleIds(document.project());
        v.selected.clear();
        if (!rest.empty()) select(v, rest[std::min<std::size_t>(static_cast<std::size_t>(at), rest.size() - 1)]);
        ImGui::EndGroup();
        return changed;   // the screen is drawn again next frame for the neighbour
      }
    }
  }
  const auto loopColumn = [&](bool sustain, const char* title, ImU32 colour) {
    ImGui::BeginGroup();
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    const char* on = sustain ? "susOn" : "loopOn";
    const bool has = s.number(sustain ? "susEnd" : "loopEnd") > 0;
    int start = static_cast<int>(s.number(sustain ? "susStart" : "loopStart")), end = static_cast<int>(s.number(sustain ? "susEnd" : "loopEnd")), xfade = static_cast<int>(s.number(sustain ? "susXfade" : "loopXfade"));
    ImGui::PushID(on);
    const bool pressed = textButton(loopActive(s, sustain) ? "on" : "off", loopActive(s, sustain));
    ImGui::PopID();
    if (pressed) {
      const bool turnOn = !loopActive(s, sustain);
      changed |= commit(v, document, sustain ? "Sustain loop" : "Loop", false, [&](EditableSample& e) {
        if (!has) setLoopPoints(e, 0, e.frames(), sustain);   // no points yet: loop the whole sample
        e.props[on] = turnOn;
      });
    }
    ImGui::TextColored(dim, "start"); ImGui::SameLine(46 * scale);
    const int newStart = spinFrames(sustain ? "##susStart" : "##loopStart", start, 90 * scale);
    if (newStart != start) changed |= commit(v, document, "Move loop start", false, [&](EditableSample& e) { const int e2 = has ? end : e.frames(); setLoopPoints(e, std::clamp(newStart, 0, e2 - 2), e2, sustain); });
    ImGui::TextColored(dim, "end"); ImGui::SameLine(46 * scale);
    const int newEnd = spinFrames(sustain ? "##susEnd" : "##loopEnd", end, 90 * scale);
    if (newEnd != end) changed |= commit(v, document, "Move loop end", false, [&](EditableSample& e) { if (newEnd < 1) setLoopPoints(e, 0, 0, sustain); else setLoopPoints(e, has ? start : 0, std::clamp(newEnd, (has ? start : 0) + 2, e.frames()), sustain); });
    ImGui::TextColored(dim, "xfade"); ImGui::SameLine(46 * scale);
    const int newFade = spinFrames(sustain ? "##susXfade" : "##loopXfade", xfade, 90 * scale);
    if (newFade != xfade) changed |= commit(v, document, "Loop crossfade", false, [&](EditableSample& e) {
      const char* key = sustain ? "susXfade" : "loopXfade";
      if (newFade <= 0) e.props.erase(key); else e.props[key] = std::min(newFade, e.frames());
    });
    ImGui::EndGroup();
  };
  loopColumn(false, "Loop", kLoopColour);
  ImGui::SameLine(0, 16 * scale);
  loopColumn(true, "Sustain loop", kSusColour);
  ImGui::SameLine(0, 16 * scale);
  ImGui::BeginGroup();
  ImGui::TextUnformatted("Selection");
  ImGui::TextColored(dim, "from"); ImGui::SameLine(46 * scale);
  const int newFrom = spinFrames("##selFrom", v.selFrom, 90 * scale);
  if (newFrom != v.selFrom) { v.selFrom = std::clamp(newFrom, 0, frames - 1); v.selTo = v.selTo > v.selFrom ? v.selTo : frames; }
  ImGui::TextColored(dim, "to"); ImGui::SameLine(46 * scale);
  const int newTo = spinFrames("##selTo", v.selTo, 90 * scale);
  if (newTo != v.selTo) { if (newTo < 1) v.selFrom = v.selTo = 0; else { v.selTo = std::clamp(newTo, 1, frames); v.selFrom = std::min(v.selFrom, v.selTo - 1); } }
  ImGui::TextColored(dim, "rate"); ImGui::SameLine(46 * scale);
  const int rate = static_cast<int>(s.rate), newRate = spinFrames("##rate", rate, 90 * scale);
  if (newRate != rate) changed |= commit(v, document, "Sample rate", false, [&](EditableSample& e) { e.rate = std::clamp(newRate, 1000, 384000); });
  ImGui::EndGroup();
  ImGui::SameLine(0, 16 * scale);
  ImGui::BeginGroup();
  const struct { const char* key; const char* label; double min, max, def; } levels[] = {{"volume", "Volume", 0, 1, 1}, {"globalVolume", "Global volume", 0, 1, 1}, {"pan", "Pan", -1, 1, 0}};
  for (const auto& level : levels) {
    double value = s.number(level.key, level.def);
    SliderSpec spec;
    spec.min = level.min; spec.max = level.max; spec.def = level.def; spec.width = 150;
    const SliderResult result = slider((std::string("##") + level.key).c_str(), level.label, value, spec);
    if (result.changed) changed |= commit(v, document, std::string("Sample ") + level.key, true, [&](EditableSample& e) { setSampleLevel(e, level.key, std::round(value * 100) / 100); });
  }
  ImGui::EndGroup();

  // ---- the edit tools ----
  const struct { const char* label; const char* tool; const char* tip; } tools[] = {
      {"Crop", "crop", "Keep the selection (or the loop region) (Shift+C)"}, {"Cut", "cut", "Remove the selection (Shift+X)"},
      {"Silence", "silence", "Shift+S"}, {"Reverse", "reverse", "Shift+R"}, {"Normalise", "normalise", "Shift+N"},
      {"Fade in", "fadeIn", "Shift+I"}, {"Fade out", "fadeOut", "Shift+O"}, {"Remove DC", "removeDc", "Shift+D"}, {"Bake crossfade", "bakeCrossfade", "Write the loop crossfades into the audio (Shift+B)"}};
  for (std::size_t i = 0; i < sizeof tools / sizeof tools[0]; i++) {
    if (i) ImGui::SameLine(0, 10 * scale);
    if (textButton(tools[i].label)) changed |= runTool(v, document, tools[i].tool, tools[i].label);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tools[i].tip);
  }
  ImGui::SameLine(0, 18 * scale);
  if (textButton("Copy")) copySelection(v, document, false);
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Paste")) pasteClipAtHead(v, document, PasteMode::Insert);
  ImGui::SameLine(0, 10 * scale);
  if (textButton("Whole")) setView(v, 0, frames, frames);

  // ---- the waveform ----
  const float statusH = ImGui::GetTextLineHeightWithSpacing();
  const ImVec2 size(std::max(40.0f, ImGui::GetContentRegionAvail().x), std::max(60.0f, ImGui::GetContentRegionAvail().y - statusH));
  const ImVec2 origin = ImGui::GetCursorScreenPos();
  ImGui::InvisibleButton("wave", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
  const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
  const Span span = spanOf(v, frames);
  const auto frameToX = [&](double f) { return origin.x + static_cast<float>((f - span.from) / (span.to - span.from)) * size.x; };
  const auto xToFrame = [&](float x) { return static_cast<int>(std::lround(span.from + std::clamp((x - origin.x) / size.x, 0.0f, 1.0f) * (span.to - span.from))); };
  ImDrawList* draw = ImGui::GetWindowDrawList();
  draw->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);
  const float mid = origin.y + size.y / 2;
  draw->AddLine(ImVec2(origin.x, mid), ImVec2(origin.x + size.x, mid), dimAlpha(60));

  // the loops, as the drag shows them while it goes on
  struct Region { int start, end; ImU32 colour; bool sustain; };
  Region regions[2];
  for (int k = 0; k < 2; k++) {
    const bool sustain = k == 1;
    regions[k] = {static_cast<int>(s.number(sustain ? "susStart" : "loopStart")), static_cast<int>(s.number(sustain ? "susEnd" : "loopEnd")), sustain ? kSusColour : kLoopColour, sustain};
    if (v.drag != SampleView::Drag::None && v.drag != SampleView::Drag::Select && v.drag != SampleView::Drag::Slice && v.dragSustain == sustain && (v.dragMoved || v.drag != SampleView::Drag::Region)) {
      regions[k].start = v.dragStart;
      regions[k].end = v.dragEnd;
    }
  }
  for (const Region& r : regions) {
    if (r.end <= r.start) continue;
    const float x0 = frameToX(r.start), x1 = frameToX(r.end);
    const bool on = r.sustain ? loopActive(s, true) : loopActive(s, false);
    draw->AddRectFilled(ImVec2(x0, origin.y), ImVec2(x1, origin.y + size.y), (r.colour & 0x00ffffff) | (static_cast<ImU32>(on ? 45 : 18) << 24));
  }
  if (v.selTo > v.selFrom) draw->AddRectFilled(ImVec2(frameToX(v.selFrom), origin.y), ImVec2(frameToX(v.selTo), origin.y + size.y), IM_COL32(255, 255, 255, 40));
  if (v.drag == SampleView::Drag::Select && v.dragMoved) draw->AddRectFilled(ImVec2(frameToX(v.dragStart), origin.y), ImVec2(frameToX(v.dragEnd), origin.y + size.y), IM_COL32(255, 255, 255, 40));

  // the waveform itself, or the spectrogram (one cell per pixel, made again when the view or the size changes)
  const int width = std::max(1, static_cast<int>(size.x));
  const std::string key = v.selected + ":" + std::to_string(v.cacheRevision) + ":" + std::to_string(width) + ":" + std::to_string(static_cast<long long>(span.from)) + ":" + std::to_string(static_cast<long long>(span.to));
  const float half = size.y / 2 - 2;
  if (v.spectrogram) {
    const int height = std::max(1, static_cast<int>(size.y));
    const std::string spectrumKey = key + ":" + std::to_string(height);
    if (v.spectrumKey != spectrumKey || !pixelTexture("spectrogram", nullptr, width, height)) {
      const std::vector<std::uint8_t> cells = computeSpectrogram(s.left, s.right, static_cast<int>(span.from), static_cast<int>(span.to), width, height);
      std::vector<unsigned char> pixels(cells.size() * 4);
      for (std::size_t i = 0; i < cells.size(); i++) { const auto c = spectrogramHeat(cells[i]); std::copy(c.begin(), c.end(), pixels.begin() + static_cast<std::ptrdiff_t>(i * 4)); }
      pixelTexture("spectrogram", pixels.data(), width, height);
      v.spectrumKey = spectrumKey;
    }
    const ImTextureID texture = pixelTexture("spectrogram", nullptr, width, height);
    if (texture) draw->AddImage(texture, origin, ImVec2(origin.x + size.x, origin.y + size.y));
  } else {
    if (v.peaksKey != key) { v.peaks = computePeaks(s.left, width, span.from, span.to); v.peaksKey = key; }
    for (int x = 0; x < width; x++) {
      const float lo = v.peaks[static_cast<std::size_t>(2 * x)], hi = v.peaks[static_cast<std::size_t>(2 * x + 1)];
      const float px = origin.x + x + 0.5f;
      draw->AddLine(ImVec2(px, mid - hi * half), ImVec2(px, std::max(mid - hi * half + 1, mid - lo * half)), accentAlpha(200));
    }
  }
  for (const Region& r : regions) {
    if (r.end <= r.start) continue;
    for (const int f : {r.start, r.end}) draw->AddLine(ImVec2(frameToX(f), origin.y), ImVec2(frameToX(f), origin.y + size.y), r.colour, 1.5f);
  }
  const std::vector<int> savedSlices = sliceMarkers(s);
  const std::vector<int>& slices = v.drag == SampleView::Drag::Slice ? v.dragSlices : savedSlices;
  for (std::size_t i = 0; i < slices.size(); i++) {
    const float x = frameToX(slices[i]);
    draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + size.y), kSliceColour, static_cast<int>(i) == v.activeSlice ? 2.5f : 1.0f);
    char label[32];
    std::snprintf(label, sizeof label, "%02X %s", static_cast<int>(i), sliceHex(slices[i], frames).c_str());
    draw->AddText(ImVec2(x + 3, origin.y + 1), kSliceColour, label);
  }
  draw->AddLine(ImVec2(frameToX(v.head), origin.y), ImVec2(frameToX(v.head), origin.y + size.y), textAlpha(150));
  draw->PopClipRect();

  // ---- the mouse on the waveform ----
  const auto nearestSlice = [&](float x, float within) {
    int best = -1;
    float distance = within + 1;
    for (std::size_t i = 0; i < savedSlices.size(); i++) if (std::abs(frameToX(savedSlices[i]) - x) < distance) { distance = std::abs(frameToX(savedSlices[i]) - x); best = static_cast<int>(i); }
    return best;
  };
  const auto commitSlices = [&](std::vector<int> next, int focus) {
    if (!commit(v, document, "Slice markers", false, [&](EditableSample& e) { setSliceMarkers(e, next); })) return false;
    const std::vector<int> now = sliceMarkers(readSample(document.project().samples[v.selected]));
    v.activeSlice = focus < 0 ? -1 : static_cast<int>(std::find(now.begin(), now.end(), focus) - now.begin());
    return true;
  };
  const ImVec2 mouse = io.MousePos;
  if (hovered && io.KeyCtrl && io.MouseWheel != 0) {   // Ctrl+wheel zooms about the mouse
    const double at = xToFrame(mouse.x), k = io.MouseWheel > 0 ? 0.5 : 2.0, from = at - (at - span.from) * k;
    setView(v, from, from + (span.to - span.from) * k, frames);
  } else if (hovered && !io.KeyCtrl && io.MouseWheel != 0 && v.viewTo > 0) {   // the wheel moves along a zoomed view
    const double d = -io.MouseWheel * (span.to - span.from) * 0.2;
    setView(v, span.from + d, span.to + d, frames);
  }
  if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0) && v.viewTo > 0) {
    const double d = -io.MouseDelta.x / size.x * (span.to - span.from);
    setView(v, span.from + d, span.to + d, frames);
  }
  if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && frames > 0) {
    const int f = xToFrame(mouse.x);
    v.dragMoved = false;
    v.dragAnchor = f;
    const auto grabRegion = [&](bool sustain) {
      v.dragSustain = sustain;
      const int start = static_cast<int>(s.number(sustain ? "susStart" : "loopStart")), end = static_cast<int>(s.number(sustain ? "susEnd" : "loopEnd"));
      v.dragStart = start; v.dragEnd = end;
      if (end > start && std::abs(mouse.x - frameToX(start)) <= kGrab * scale) v.drag = SampleView::Drag::LoopStart;
      else if (end > start && std::abs(mouse.x - frameToX(end)) <= kGrab * scale) v.drag = SampleView::Drag::LoopEnd;
      else v.drag = SampleView::Drag::Region;
    };
    if (io.KeyAlt) {   // a slice marker when one is under the mouse (Alt+click alone makes one), else the loop
      const int at = nearestSlice(mouse.x, kGrab * scale);
      if (at >= 0) { v.drag = SampleView::Drag::Slice; v.dragSlice = at; v.dragSlices = savedSlices; v.activeSlice = at; }
      else { grabRegion(false); v.dragSlice = -1; }
    } else if (io.KeyCtrl) grabRegion(true);
    else { v.drag = SampleView::Drag::Select; v.dragStart = v.dragEnd = f; }
  }
  if (v.drag != SampleView::Drag::None && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
    const int f = xToFrame(mouse.x);
    switch (v.drag) {
      case SampleView::Drag::Slice: v.dragSlices[static_cast<std::size_t>(v.dragSlice)] = std::min(f, frames - 1); v.dragMoved = true; break;
      case SampleView::Drag::LoopStart: v.dragStart = std::min(f, v.dragEnd - 2); v.dragMoved = true; break;
      case SampleView::Drag::LoopEnd: v.dragEnd = std::max(f, v.dragStart + 2); v.dragMoved = true; break;
      case SampleView::Drag::Select: if (std::abs(f - v.dragAnchor) >= 1) { v.dragStart = std::min(f, v.dragAnchor); v.dragEnd = std::max(f, v.dragAnchor); v.dragMoved = true; } break;
      case SampleView::Drag::Region: if (std::abs(f - v.dragAnchor) >= 2) { v.dragStart = std::min(f, v.dragAnchor); v.dragEnd = std::max(f, v.dragAnchor); v.dragMoved = true; } break;
      default: break;
    }
  }
  if (v.drag != SampleView::Drag::None && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
    const SampleView::Drag kind = v.drag;
    v.drag = SampleView::Drag::None;
    if (kind == SampleView::Drag::Slice) {
      changed |= commitSlices(v.dragSlices, v.dragSlices[static_cast<std::size_t>(v.dragSlice)]);
    } else if (kind == SampleView::Drag::Select) {
      if (v.dragMoved) { v.selFrom = v.dragStart; v.selTo = v.dragEnd; } else v.head = v.dragAnchor;   // a click without a drag places the playhead
    } else if (v.dragMoved) {
      changed |= commit(v, document, v.dragSustain ? "Sustain loop" : "Loop", false, [&](EditableSample& e) { setLoopPoints(e, std::max(0, v.dragStart), v.dragEnd, v.dragSustain); });
    } else if (kind == SampleView::Drag::Region && v.dragSlice == -1) {   // Alt+click without a drag: a slice marker
      const int at = std::min(v.dragAnchor, frames - 1);
      std::vector<int> next = savedSlices;
      next.push_back(at);
      changed |= commitSlices(next, at);
    }
  }
  if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {   // double-click: no selection; with Alt, take a marker or the loop away; with Ctrl, the sustain loop
    if (io.KeyAlt) {
      const int at = nearestSlice(mouse.x, kGrab * 2 * scale);
      if (at >= 0) { std::vector<int> next = savedSlices; next.erase(next.begin() + at); changed |= commitSlices(next, -1); }
      else changed |= commit(v, document, "Loop", false, [&](EditableSample& e) { setLoopPoints(e, 0, 0, false); });
    } else if (io.KeyCtrl) changed |= commit(v, document, "Sustain loop", false, [&](EditableSample& e) { setLoopPoints(e, 0, 0, true); });
    else v.selFrom = v.selTo = 0;
  }
  if (hovered) {
    bool onMarker = false;
    for (const Region& r : regions) if (r.end > r.start && (std::abs(mouse.x - frameToX(r.start)) <= kGrab * scale || std::abs(mouse.x - frameToX(r.end)) <= kGrab * scale)) onMarker = true;
    if (onMarker || (io.KeyAlt && nearestSlice(mouse.x, kGrab * scale) >= 0)) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
  }

  // ---- the keys (the text boxes have the keyboard when one is active) ----
  if (!io.WantTextInput) {
    const auto tool = [&](ImGuiKey key, const char* name, const char* label) { if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(key, false)) changed |= runTool(v, document, name, label); };
    tool(ImGuiKey_C, "crop", "Crop"); tool(ImGuiKey_T, "crop", "Crop"); tool(ImGuiKey_X, "cut", "Cut"); tool(ImGuiKey_S, "silence", "Silence");
    tool(ImGuiKey_R, "reverse", "Reverse"); tool(ImGuiKey_N, "normalise", "Normalise"); tool(ImGuiKey_I, "fadeIn", "Fade in"); tool(ImGuiKey_O, "fadeOut", "Fade out");
    tool(ImGuiKey_D, "removeDc", "Remove DC"); tool(ImGuiKey_B, "bakeCrossfade", "Bake crossfade");
    if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_M, false)) {   // a slice marker at the start of the selection
      std::vector<int> next = savedSlices;
      const int at = std::min(v.selFrom, frames - 1);
      next.push_back(at);
      changed |= commitSlices(next, at);
    }
    if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_U, false) && !savedSlices.empty()) {   // the marker nearest the selection goes
      const int at = std::min(v.selFrom, frames - 1);
      std::vector<int> next = savedSlices;
      next.erase(std::min_element(next.begin(), next.end(), [&](int a, int b) { return std::abs(a - at) < std::abs(b - at); }));
      changed |= commitSlices(next, -1);
    }
    if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false) && v.selTo > v.selFrom) setView(v, v.selFrom, v.selTo, frames);
    if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A, false)) setView(v, span.from - (span.to - span.from) / 2, span.to + (span.to - span.from) / 2, frames);
    if (io.KeyShift && !io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_W, false)) setView(v, 0, frames, frames);
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_C, false)) copySelection(v, document, false);
    if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_X, false)) { copySelection(v, document, true); changed = true; }
    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) { pasteClipAtHead(v, document, io.KeyShift ? PasteMode::Overwrite : PasteMode::Insert); changed = true; }
    if (!io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
      if (ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false)) { v.octave = std::max(0, v.octave - 1); v.status = "Octave " + std::to_string(v.octave); }
      if (ImGui::IsKeyPressed(ImGuiKey_RightBracket, false)) { v.octave = std::min(9, v.octave + 1); v.status = "Octave " + std::to_string(v.octave); }
      for (const int dir : {-1, 1}) {   // - and + move the current slice marker (Shift: 100 frames, Ctrl: 1000)
        if (!ImGui::IsKeyPressed(dir < 0 ? ImGuiKey_Minus : ImGuiKey_Equal, true) || savedSlices.empty()) continue;
        const std::size_t i = static_cast<std::size_t>(std::clamp(v.activeSlice, 0, static_cast<int>(savedSlices.size()) - 1));
        const int to = std::clamp(savedSlices[i] + dir, 0, frames - 1);
        if (to == savedSlices[i]) continue;
        if (std::find(savedSlices.begin(), savedSlices.end(), to) != savedSlices.end()) { v.status = "Another marker is already there."; continue; }
        std::vector<int> next = savedSlices;
        next[i] = to;
        changed |= commitSlices(next, to);
      }
      if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) { audition(v, preview, kRootNote, v.head); v.heldKey = ImGuiKey_Space; }   // plays at its root note, from the playhead
      for (int i = 0; i < static_cast<int>(sizeof kKeys / sizeof kKeys[0]); i++) {
        if (!ImGui::IsKeyPressed(kKeys[i], false)) continue;
        const int note = std::clamp(12 * (v.octave + 1) + i, 0, 127);
        audition(v, preview, note, 0);
        v.heldKey = kKeys[i];
      }
    }
  }
  if (v.heldKey >= 0 && (!ImGui::IsKeyDown(static_cast<ImGuiKey>(v.heldKey)) || io.WantTextInput)) { preview.stop(); v.heldKey = -1; }

  if (!v.status.empty()) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(kWarm), "%s", v.status.c_str());
  else ImGui::TextColored(dim, "drag: select   Alt+drag: loop   Ctrl+drag: sustain   Alt+click: slice   Ctrl+wheel: zoom");
  ImGui::EndGroup();
  return changed;
}

}  // namespace wax::app
