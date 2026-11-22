// Head pan: places a sound around the head. The far ear hears it later, quieter and duller (the head shadows it), and a sound from
// behind loses its top in both ears. Mono in (the sides are summed), a pair of ears out. Port of webtracker src/audio/nodes/headpan.js.
#include <algorithm>
#include <cmath>
#include <vector>

#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"
#include "nodes/spacecore.h"

namespace wax {
namespace {

enum Param { kAngle = 0, kDistance, kHead, kShadow, kMix };
constexpr double kMaxSeconds = 0.002;

class HeadPan final : public Node {
public:
  explicit HeadPan(const NodeContext& context)
      : sampleRate_(context.sampleRate), size_(static_cast<int>(std::ceil(context.sampleRate * kMaxSeconds)) + 4), buf_(size_, 0.0f),
        slew_(1 - js::exp(-1 / (context.sampleRate * 0.04))) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    auto p = [&](int i) { return static_cast<double>(params[slots[i]]); };
    const space::Head h = space::headOf(p(kAngle), p(kDistance), p(kHead), p(kShadow));
    const double mix = p(kMix);
    const bool farL = h.farLeft;
    const double tdl = farL ? h.itd * sampleRate_ : 0, tdr = farL ? 0 : h.itd * sampleRate_;
    const double tgl = h.gain * (farL ? h.farGain : 1), tgr = h.gain * (farL ? 1 : h.farGain);
    const double tcl = coef(farL ? h.farCut : 20000), tcr = coef(farL ? 20000 : h.farCut), trear = coef(h.rearCut);
    if (!init_) { dl_ = tdl; dr_ = tdr; gl_ = tgl; gr_ = tgr; cl_ = tcl; cr_ = tcr; rear_ = trear; init_ = true; }
    for (int i = 0; i < n; i++) {
      dl_ += (tdl - dl_) * slew_; dr_ += (tdr - dr_) * slew_; gl_ += (tgl - gl_) * slew_; gr_ += (tgr - gr_) * slew_;
      cl_ += (tcl - cl_) * slew_; cr_ += (tcr - cr_) * slew_; rear_ += (trear - rear_) * slew_;
      const double inL = inputs[0][i], inR = inputs[1][i];
      const double x = (inL + inR) * 0.5;
      buf_[w_] = static_cast<float>(std::abs(x) < 1e-20 ? 0 : x);
      l_ += (at(dl_) - l_) * std::min(cl_, rear_);
      r_ += (at(dr_) - r_) * std::min(cr_, rear_);
      outputs[0][i] = static_cast<float>(inL + (l_ * gl_ - inL) * mix);
      outputs[1][i] = static_cast<float>(inR + (r_ * gr_ - inR) * mix);
      if (++w_ == size_) w_ = 0;
    }
  }

private:
  double coef(double hz) const { return 1 - js::exp(-2 * kPi * std::min(hz, sampleRate_ * 0.45) / sampleRate_); }
  double at(double d) const {
    double p = w_ - d;
    if (p < 0) p += size_;
    const int j = static_cast<int>(p);
    const double f = p - j;
    const double a = buf_[j], b = buf_[j + 1 == size_ ? 0 : j + 1];
    return a + (b - a) * f;
  }

  double sampleRate_;
  int size_;
  std::vector<float> buf_;
  double slew_;
  int w_ = 0;
  double l_ = 0, r_ = 0;
  bool init_ = false;
  double dl_ = 0, dr_ = 0, gl_ = 1, gr_ = 1, cl_ = 1, cr_ = 1, rear_ = 1;
};

}  // namespace

void registerHeadPan(NodeRegistry& registry) {
  NodeType type;
  type.type = "headpan";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.kinds = {{"in", PortKind::Stereo}, {"out", PortKind::Stereo}};
  type.isEffect = true;
  type.params = {{"angle", -180, 180, 40}, {"distance", 0.5, 20, 1.5}, {"head", 14, 22, 17.5}, {"shadow", 0, 1, 1}, {"mix", 0, 1, 1}};
  type.create = [](const NodeContext& context) { return std::make_unique<HeadPan>(context); };
  registry.add(std::move(type));
}

}  // namespace wax
