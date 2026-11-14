// Band splitter core, shared by `split` and `mbsat`: four bands from three Linkwitz-Riley (24 dB/oct) crossovers in series,
// phase-aligned so the bands add back to the original. Port of the filters in webtracker src/audio/nodes/split.js.
#pragma once

#include <algorithm>
#include <cmath>

#include "core/js_math.h"
#include "nodes/shared_dsp.h"

namespace wax::splitcore {

inline constexpr double kK = 1.4142135623730951;   // Butterworth damping (1 / Q): Math.SQRT2

struct Svf {
  double ic1 = 0, ic2 = 0, v1 = 0, v2 = 0, a1 = 0, a2 = 0, a3 = 0;
  void set(double g) { a1 = 1 / (1 + g * (g + kK)); a2 = g * a1; a3 = g * a2; }
  double run(double x) {
    const double v3 = x - ic2;
    const double w1 = a1 * ic1 + a2 * v3;
    const double w2 = ic2 + a2 * ic1 + a3 * v3;
    ic1 = 2 * w1 - ic1;
    ic2 = 2 * w2 - ic2;
    v1 = w1; v2 = w2;
    return w2;
  }
  void flush() { if (std::abs(ic1) < 1e-20) ic1 = 0; if (std::abs(ic2) < 1e-20) ic2 = 0; }
};

struct Channel {
  Svf a1, a2, b1, b2, c1, c2, p2, p3, q3;

  void set(double g1, double g2, double g3) {
    a1.set(g1); a2.set(g1);
    b1.set(g2); b2.set(g2); p2.set(g2);
    c1.set(g3); c2.set(g3); p3.set(g3); q3.set(g3);
  }

  // x in; the four bands go to out[0..3][i]
  void run(double x, float* const* out, int i) {
    const double lp1 = a2.run(a1.run(x));
    const double r1 = x - 2 * kK * a1.v1 - lp1;
    const double lp2 = b2.run(b1.run(r1));
    const double r2 = r1 - 2 * kK * b1.v1 - lp2;
    const double lp3 = c2.run(c1.run(r2));
    const double hp3 = r2 - 2 * kK * c1.v1 - lp3;
    p2.run(lp1);
    const double low = lp1 - 2 * kK * p2.v1;
    p3.run(low);
    out[0][i] = static_cast<float>(low - 2 * kK * p3.v1);
    q3.run(lp2);
    out[1][i] = static_cast<float>(lp2 - 2 * kK * q3.v1);
    out[2][i] = static_cast<float>(lp3);
    out[3][i] = static_cast<float>(hp3);
  }

  void flush() { for (Svf* s : {&a1, &a2, &b1, &b2, &c1, &c2, &p2, &p3, &q3}) s->flush(); }
};

// The crossovers kept in order and slewed, as `g` coefficients (tan of the half-angle), the way both nodes do it.
struct Crossovers {
  double f[3] = {0, 0, 0};
  bool started = false;
  void update(double f1, double f2in, double f3in, int n, double sampleRate, double g[3]) {
    const double f2 = std::max(f2in, f1 * 1.1), f3 = std::max(f3in, f2 * 1.1);
    if (!started) { f[0] = f1; f[1] = f2; f[2] = f3; started = true; }
    const double slew = 1 - js::exp(-n / (sampleRate * 0.02));
    f[0] += (f1 - f[0]) * slew; f[1] += (f2 - f[1]) * slew; f[2] += (f3 - f[2]) * slew;
    for (int k = 0; k < 3; k++) g[k] = js::tan(kPi * std::min(std::max(f[k], 10.0), sampleRate * 0.45) / sampleRate);
  }
};

}  // namespace wax::splitcore
