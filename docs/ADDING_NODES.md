# Adding a node to the native engine

The web app (github.com/zeffii/webtracker) is where a node is designed; its `docs/ADDING_NODES.md` explains the node, the
instrument, the house rules (no brand names, no factory presets, a `quality` setting for heavy nodes) and the tests.
This page is what the native side needs once the web node exists. The native engine renders **the same samples**; the
test is that the two renders differ by nothing (-300 dB), not by a little.

Open `src/nodes/gain.cpp` (the smallest node) and `src/nodes/piano2.cpp` (a large one, a bit-exact port) beside this.

## The shape of a node

```cpp
// Tremolo: the level swells and falls at `rate`, as deep as `depth`.
// Port of webtracker src/audio/nodes/trem.js.
#include "core/js_math.h"
#include "nodes/all_nodes.h"
#include "nodes/shared_dsp.h"   // kPi and the shared helpers

namespace wax {
namespace {

enum Param { kRate = 0, kDepth };          // the order of the web node's `params`
enum Input { kIn = 0 };

class Trem final : public Node {
public:
  explicit Trem(const NodeContext& context) : sampleRate_(context.sampleRate) {}

  void process(const float* const* inputs, float* const* outputs, int n, const float* params, const int* slots) override {
    const float* in = inputs[kIn];
    float* out = outputs[0];
    const double step = params[slots[kRate]] / sampleRate_, depth = params[slots[kDepth]];
    for (int i = 0; i < n; i++) {
      phase_ += step; if (phase_ >= 1) phase_ -= 1;
      out[i] = static_cast<float>(in[i] * (1 - depth + depth * (0.5 + 0.5 * js::sin(2 * kPi * phase_))));
    }
  }

private:
  double sampleRate_;
  double phase_ = 0;   // JS numbers are doubles, so state is double
};

}  // namespace

void registerTrem(NodeRegistry& registry) {
  NodeType type;
  type.type = "trem";
  type.inputs = {"in"};
  type.outputs = {"out"};
  type.params = {{"rate", 0.1, 20, 5}, {"depth", 0, 1, 0.5}};   // name, min, max, default (and `true` last for a whole-number parameter)
  type.dual = true;
  type.create = [](const NodeContext& context) { return std::make_unique<Trem>(context); };
  registry.add(std::move(type));
}

}  // namespace wax
```

The example above compiles against the headers as they are; when you adapt it, check `gain.cpp` and `piano2.cpp` for the current names.

## Matching the web, to the bit

- Port the algorithm line for line, same order of operations: floating-point addition is not associative.
- Wherever the web calls `Math.pow`, `Math.sin`, `Math.cos`, `Math.exp`, `Math.log2`, `Math.atan2`, `Math.tanh` or
  `Math.hypot`, call the `js::` version (`core/js_math.h`) or `jsHypot`; `Math.round` is `jsRound`. These reproduce
  the JavaScript engine's results, which `std::` does not promise to.
- Keep state in `double`; the output is `float`, as the web's `Float32Array`.
- Random numbers: the same seeded xorshift as the web node, so a note is the same every time.
- Do not "improve" the node here. A faster or cleaner version belongs in a later, deliberate change to both sides
  (heavy nodes may diverge once parity is proven, and then only behind a setting).

## Registering it

1. `src/nodes/all_nodes.h`: `void registerTrem(NodeRegistry& registry);`
2. `src/nodes/all_nodes.cpp`: call `registerTrem(r);` in the list.
3. `CMakeLists.txt`: add `src/nodes/trem.cpp` to the sources.
4. A generator that should sleep when it is silent goes in the `kGated` list in `src/engine/sleep.cpp`.
5. Regenerate what the app reads from the web repository (run from this repo, pointing at a checkout of the web one):

   ```
   node tools/gen-machines.mjs     /path/to/webtracker > src/app/machine_table.inc
   node tools/gen-node-help.mjs    /path/to/webtracker > src/app/node_help_table.inc
   node tools/gen-node-looks.mjs   /path/to/webtracker > src/app/node_looks_table.inc
   node tools/gen-insert-menu.mjs  /path/to/webtracker > src/app/insert_menu_table.inc
   ```

   `src/app/units_table.inc` has no generator: add the unit rows for the new node's parameters by hand
   (`{"trem", "rate", "Hz"}`).

## The proof

1. A short song that uses the node, in `tests/songs/trem.wax`. Give it parameters away from the defaults, so each
   path through the node runs; a few notes are enough.
2. The web side renders its reference: in the webtracker checkout,
   `node tools/render-ref.mjs /path/to/wax-core/tests/songs/trem.wax out.wav`, and copy the resulting `trem.ref.json`
   next to the song. It records the frames, the peak and a checksum.
3. Add `"trem"` to the `render-synths` (or the group that fits) list in `tests/render_tests.cpp`.
4. Build and run `./build/wax-tests render-synths/trem`, then render the song natively
   (`./build/wax-render tests/songs/trem.wax native.wav`) and compare:
   `./build/wax-compare web.wav native.wav -120`. It should say `-300.0 dB`. Anything else is a difference to find.
5. `./build/wax-tests --quick` passes, then commit.

Tests are described in `docs/TESTING.md`; a test registers itself with `WAX_TEST("group", name)` and needs no list.
A node with behaviour of its own (an edit that must reach the engine, a native-only addition) gets a test of its
own beside the others.
