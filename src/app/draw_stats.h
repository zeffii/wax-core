// What a frame costs to draw, for noticing a screen that has become expensive (a picture re-sent as thousands of rectangles every frame, say).
// Off unless asked for (F12, or --stats): then a line of numbers sits at the top right of the window. Nothing is measured while it is off.
#pragma once

struct ImDrawData;

namespace wax::app {

void drawStatsSet(bool on);
bool drawStatsOn();

// Call just before a frame is begun and just after it has been drawn (the draw data is the frame's).
void drawStatsFrameStart();
void drawStatsFrameEnd(const ImDrawData* data);

// Draws the numbers of the last frame over the window (the current frame's are not known yet while it is being built).
void drawStatsOverlay();

// "draw commands 287, vertices 10278, indices 18939, cpu 2.1 ms": the last frame, for the log.
const char* drawStatsLine();

}  // namespace wax::app
