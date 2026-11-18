#include "app/offline_render.h"

#include <algorithm>
#include <memory>

#include "nodes/adsr.h"

namespace wax::app {

std::vector<float> renderOffline(const NodeType& type, std::vector<float> values, int note, double vel, double ms, double sr, double offAt, const Json* data) {
  NodeContext context;
  context.sampleRate = sr;
  context.data = data;
  std::unique_ptr<Node> node = type.create(context);
  std::vector<int> slots(values.size());
  for (std::size_t i = 0; i < slots.size(); i++) slots[i] = static_cast<int>(i);
  const int total = std::max(1, static_cast<int>(sr * ms / 1000)), offSample = offAt >= 0 ? static_cast<int>(sr * offAt / 1000) : -1;
  std::vector<float> out(static_cast<std::size_t>(total)), right(512);
  const float* noInputs[8] = {};   // nothing patched in: a node that looks at an input finds it empty
  NoteEvent on;
  on.type = NoteType::On; on.note = note; on.vel = vel;
  node->note(on);
  for (int at = 0; at < total;) {
    int n = std::min(512, total - at);
    if (offSample > at && offSample < at + n) n = offSample - at;
    if (at == offSample) { NoteEvent off; off.type = NoteType::Off; off.note = note; node->note(off); }
    float* outs[2] = {out.data() + at, right.data()};
    node->process(noInputs, outs, n, values.data(), slots.data());
    at += n;
  }
  return out;
}

std::vector<float> adsrCurve(double a, double d, double s, double r, int hold, int total) {   // one value a millisecond
  Adsr env(1000);
  env.set(a, d, s, r); env.on();
  std::vector<float> out(static_cast<std::size_t>(total));
  for (int i = 0; i < total; i++) { if (i == hold) env.off(); out[static_cast<std::size_t>(i)] = static_cast<float>(env.next()); }
  return out;
}

}  // namespace wax::app
