// The nodes the graph compiler inserts by itself. Nobody places them by hand.
// Ports of webtracker src/audio/nodes/sum.js, sumst.js, fold.js and compdelay.js.
#include <algorithm>

#include "core/js_math.h"
#include "nodes/all_nodes.h"

namespace wax {
namespace {

// Where several wires feed one input: adds them together.
class Sum final : public Node {
public:
  explicit Sum(const NodeContext& context) : feeds_(static_cast<int>(context.connected.size())) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    float* out = outputs[0];
    std::copy(inputs[0], inputs[0] + n, out);
    for (int k = 1; k < feeds_; k++) {
      const float* x = inputs[k];
      for (int i = 0; i < n; i++) out[i] += x[i];
    }
  }

private:
  int feeds_;
};

// Where several wires feed one stereo input: adds them, channel by channel. Each feed is a left/right pair.
class SumStereo final : public Node {
public:
  explicit SumStereo(const NodeContext& context) : feeds_(static_cast<int>(context.connected.size())) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    float* outLeft = outputs[0];
    float* outRight = outputs[1];
    std::copy(inputs[0], inputs[0] + n, outLeft);
    std::copy(inputs[1], inputs[1] + n, outRight);
    for (int k = 1; k < feeds_; k++) {
      const float* l = inputs[2 * k];
      const float* r = inputs[2 * k + 1];
      for (int i = 0; i < n; i++) { outLeft[i] += l[i]; outRight[i] += r[i]; }
    }
  }

private:
  int feeds_;
};

// Where a stereo wire feeds a mono input: the average of left and right.
class Fold final : public Node {
public:
  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    const float* l = inputs[0];
    const float* r = inputs[1];
    float* out = outputs[0];
    for (int i = 0; i < n; i++) out[i] = static_cast<float>((static_cast<double>(l[i]) + r[i]) * 0.5);
  }
};

// Where two audio paths meet and one is quicker than the other (delay compensation): it delays the quicker one by
// whole samples, so both arrive together. Mono or stereo.
class CompDelay final : public Node {
public:
  explicit CompDelay(const NodeContext& context) : stereo_(context.stereo) {
    const double samples = context.data ? context.data->value("samples", 1.0) : 1.0;
    length_ = std::max(1, jsRoundInt(samples));
    lines_[0].assign(static_cast<std::size_t>(length_), 0.0f);
    lines_[1].assign(static_cast<std::size_t>(length_), 0.0f);
  }

  void process(const float* const* inputs, float* const* outputs, int n, const float*, const int*) override {
    const int channels = stereo_ ? 2 : 1;
    for (int c = 0; c < channels; c++) {
      const float* x = inputs[c];
      float* y = outputs[c];
      std::vector<float>& line = lines_[c];
      int w = at_;
      for (int i = 0; i < n; i++) {
        y[i] = line[static_cast<std::size_t>(w)];
        line[static_cast<std::size_t>(w)] = x[i];
        if (++w == length_) w = 0;
      }
    }
    at_ = (at_ + n) % length_;
  }

private:
  bool stereo_;
  int length_ = 1;
  std::vector<float> lines_[2];
  int at_ = 0;
};

NodeType hidden(const char* name) {
  NodeType type;
  type.type = name;
  type.outputs = {"out"};
  return type;
}

}  // namespace

void registerPlumbing(NodeRegistry& registry) {
  NodeType sum = hidden("sum");
  sum.create = [](const NodeContext& context) { return std::make_unique<Sum>(context); };
  registry.add(std::move(sum));

  NodeType sumStereo = hidden("sumst");
  sumStereo.create = [](const NodeContext& context) { return std::make_unique<SumStereo>(context); };
  registry.add(std::move(sumStereo));

  NodeType fold = hidden("fold");
  fold.create = [](const NodeContext&) { return std::make_unique<Fold>(); };
  registry.add(std::move(fold));

  NodeType delay = hidden("compdelay");
  delay.create = [](const NodeContext& context) { return std::make_unique<CompDelay>(context); };
  registry.add(std::move(delay));
}

}  // namespace wax
