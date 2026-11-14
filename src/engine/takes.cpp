#include "engine/takes.h"

#include <algorithm>
#include <cstdio>

#include "core/js_math.h"

namespace wax {
namespace {

constexpr double kPi = 3.141592653589793;

// A raised-cosine fade over the first msIn and the last msOut milliseconds.
void fade(std::vector<float>& a, double rate, double msIn, double msOut) {
  const int n = static_cast<int>(a.size());
  const int fi = std::min(jsRoundInt(msIn * rate / 1000), n >> 1);
  const int fo = std::min(jsRoundInt(msOut * rate / 1000), n >> 1);
  for (int i = 0; i < fi; i++) a[static_cast<std::size_t>(i)] = static_cast<float>(a[static_cast<std::size_t>(i)] * (0.5 - 0.5 * js::cos(kPi * i / fi)));
  for (int i = 0; i < fo; i++) a[static_cast<std::size_t>(n - 1 - i)] = static_cast<float>(a[static_cast<std::size_t>(n - 1 - i)] * (0.5 - 0.5 * js::cos(kPi * i / fo)));
}

}  // namespace

std::string tapPoolId(int smp) {
  char name[16];
  std::snprintf(name, sizeof name, "tap%02d", smp);
  return name;
}

std::shared_ptr<SampleMessage> takeSample(const FinishedTake& take) {
  auto s = std::make_shared<SampleMessage>();
  s->rate = take.rate;
  s->data = take.left;
  s->right = take.right;
  s->loopStart = 0;
  s->loopEnd = static_cast<double>(take.left.size());
  return s;
}

std::optional<FinishedTake> TakeAssembler::feed(const TakeEvent& event) {
  const std::string key = event.instrument + '\0' + event.node;
  const TakeMessage& m = event.message;
  if (m.phase == TakeMessage::Phase::Start) {
    open_[key] = Open{m, {}, {}};
    return std::nullopt;
  }
  auto it = open_.find(key);
  if (it == open_.end() || it->second.start.take != m.take) return std::nullopt;
  Open& o = it->second;
  if (m.phase == TakeMessage::Phase::Chunk) {
    o.l.insert(o.l.end(), m.l.begin(), m.l.end());
    o.r.insert(o.r.end(), m.r.begin(), m.r.end());
    return std::nullopt;
  }
  FinishedTake out;
  if (!o.l.empty()) {
    out.instrument = event.instrument;
    out.node = event.node;
    out.smp = o.start.smp;
    out.copy = o.start.copy;
    out.rate = o.start.rate;
    out.left = std::move(o.l);
    out.right = std::move(o.r);
    fade(out.left, out.rate, o.start.fadeIn, o.start.fade);
    if (!out.right.empty()) fade(out.right, out.rate, o.start.fadeIn, o.start.fade);
  }
  const bool any = !out.left.empty();
  open_.erase(it);
  if (!any) return std::nullopt;
  return out;
}

}  // namespace wax
