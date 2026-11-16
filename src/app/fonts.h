// The app's typeface: ProggyVector (scalable Proggy, MIT; third_party/imgui/ProggyVector_Licence.txt), drawn through FreeType when the
// build has it, so the letters are hinted onto the pixel grid and stay sharp at every size.
#pragma once

namespace wax::app {

// Call once, after the ImGui context is made and before the first frame.
void loadFonts();

}  // namespace wax::app
