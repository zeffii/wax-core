// The Sample screen (F4): the project's samples in a list, the settings of the chosen one (loops, levels, rate, selection) as small
// controls, and its waveform below with the loop, sustain loop, selection and slice markers drawn on it and edited with the mouse.
// Port of webtracker src/project/samplescreen.js and waveform.js. Every change is one undoable SetSample edit.
#pragma once

#include <string>
#include <vector>

#include "edit/document.h"
#include "live/preview.h"
#include "project/sample_edit.h"

namespace wax::app {

struct SampleView {
  std::string selected;                // the chosen sample's id
  int selFrom = 0, selTo = 0;          // the selection (edits act on it; empty: the whole sample)
  int head = 0;                        // the playhead: where a paste goes, and where Space plays from
  int activeSlice = -1;                // the slice marker the nudge keys move
  int octave = 5;
  double viewFrom = 0, viewTo = 0;     // the part of the sample shown, in frames (0, 0: all of it)
  std::string status;                  // the last thing that went wrong (or was done), shown under the waveform
  SampleClip clip;                     // the clipboard: stays when another sample is chosen

  // what the mouse is dragging on the waveform
  enum class Drag { None, Select, Region, LoopStart, LoopEnd, Slice } drag = Drag::None;
  bool dragSustain = false;
  int dragAnchor = 0, dragStart = 0, dragEnd = 0, dragSlice = 0;
  bool dragMoved = false;
  std::vector<int> dragSlices;

  // the chosen sample decoded for drawing, and the waveform's peaks for the size it is drawn at
  EditableSample cache;
  std::string cacheId;
  int cacheRevision = -1;
  std::vector<float> peaks;
  std::string peaksKey;

  int heldKey = -1;                    // the piano key playing the sample (ImGuiKey), -1 for none
};

// Draws the screen filling the space it is called in. True when the project changed (the caller then runs syncSamples).
bool drawSampleScreen(SampleView& view, Document& document, Preview& preview, float scale);

}  // namespace wax::app
