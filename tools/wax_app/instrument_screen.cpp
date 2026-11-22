// The Instrument screen.
#include "wax_app/app_state.h"

namespace wax::tool {

// The Instrument screen: the graph of the machine picked, its node strip and the parameter dock.
void drawInstrumentScreen(FrameContext& f) {
  [[maybe_unused]] Session* session = f.session;
  [[maybe_unused]] Player& player = f.player;
  [[maybe_unused]] Document& doc = f.doc;
  [[maybe_unused]] CellEditor& editor = f.editor;
  [[maybe_unused]] const Song& song = f.song;
  [[maybe_unused]] std::string& selectedLane = f.selectedLane;
  [[maybe_unused]] std::string& laneInstrument = f.laneInstrument;
  [[maybe_unused]] const bool playing = f.playing;
  [[maybe_unused]] const int tick = f.tick;
  [[maybe_unused]] bool& follow = f.follow;
  [[maybe_unused]] int& playFromRequest = f.playFromRequest;
  [[maybe_unused]] const float scale = f.scale;
  [[maybe_unused]] const Options& options = f.options;
  [[maybe_unused]] std::map<ImGuiKey, int>& held = f.held;
  [[maybe_unused]] int& octave = f.octave;
  [[maybe_unused]] const int perBeat = f.perBeat;
  [[maybe_unused]] const int perBar = f.perBar;
  [[maybe_unused]] const float statusH = f.statusH;
  [[maybe_unused]] const bool mixerShown = f.mixerShown;
  // the instruments as flat text on the left, the graph in the middle, the parameter dock at the right
  if (session->instrument.empty()) session->instrument = !laneInstrument.empty() ? laneInstrument : session->playable.empty() ? std::string(kMaster) : session->playable.front();
  laneInstrument = session->instrument;
  ImGui::BeginChild("instmain", ImVec2(0, 0), false);
  if (session->panel.instrument != laneInstrument) { session->panel.instrument = laneInstrument; session->panel.node.clear(); }
  session->graphView.bottomInset = app::nodeStripHeight(doc, laneInstrument, ImGui::GetContentRegionAvail().y);
  session->graphView.dockMin = session->panel.dock.min; session->graphView.dockMax = session->panel.dock.max;
  session->graphView.probeOf = [&](const std::string& instrument, const std::string& node) { return session->player->engine().runtime().probe(instrument, node); };
  app::drawGraphView(session->graphView, doc, laneInstrument, session->panel.node, 0);
  if (session->graphView.tileDoubleClicked) session->panel.open = true;   // a double-click on a node shows its parameters (a single click only selects it)
  if (!session->graphView.openDrawings.empty()) session->panel.drawNode = std::exchange(session->graphView.openDrawings, std::string());
  const ImVec2 graphMin = ImGui::GetWindowPos();
  ImVec2 graphMax(graphMin.x + ImGui::GetWindowWidth(), graphMin.y + ImGui::GetWindowHeight());
  const app::NodeTrailState& trail = session->graphView.trail;
  if (trail.bankOpen) graphMax.x -= trail.bankMax.x - trail.bankMin.x;   // the parameters sit left of the open bank
  ImGui::EndChild();
  const bool panelChanged = app::drawParamPanel(session->panel, doc, player.engine(), laneInstrument, graphMin, graphMax);   // the parameters float over the graph
  const bool dockChanged = app::drawExposedDock(session->panel, doc, player.engine(), laneInstrument, graphMin, graphMax);   // the exposed ones at its left
  const app::NodeStripResult stripResult = app::drawNodeStrip(session->strip, session->panel, doc, player.engine(), laneInstrument, graphMin, graphMax, session->graphView.probeOf);   // the node UIs along its bottom
  if (stripResult.dataChanged) syncData(doc, player.engine());
  const bool pictureChanged = std::exchange(session->graphView.paramEdited, false);   // a dot dragged in a response picture
  if (panelChanged || dockChanged || pictureChanged || stripResult.paramsChanged) {
    syncParams(doc, player.engine());
    const std::vector<std::string> graphProblems = syncGraphs(doc, player.engine()), laneProblems = syncLanes(doc, player.engine());
    session->editStatus = !graphProblems.empty() ? graphProblems.front() : !laneProblems.empty() ? laneProblems.front() : std::string();
  }
}

}  // namespace wax::tool
