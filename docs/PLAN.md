# The plan

Agreed on 2026-11-14. The web app (webtracker) is the reference and stays as it is; this repo is the native engine,
built engine first, with the window last. Tick a step when it is done.

- [x] **1. Reference renderer** (in webtracker, build 772). `node tools/render-ref.mjs song.wax` writes a repeatable
      32-bit float render and a `.json` of facts; every later step is checked against it.
- [x] **2. Skeleton.** CMake, C++20, the `.wax` reader, the parameter block, the node interface, the first node (gain),
      `wax-info`, `wax-compare`, tests, and a build on Windows and Linux for every push.
- [ ] **3. Sequencer and graph runtime**, on one thread: the graph compiler (`graph/compile.js`), the runtime
      (`audio/runtime.js`: voices, sleep, delay compensation), the pattern reader (`seq/lane.js`, `seq/cells.js`) and
      the sequencer (`audio/sequencer.js`). Also the master machine every project gets when it is loaded
      (`ensureMaster`: the `bus-in` and `tempo` nodes).
- [ ] **4. First song: `concrete.wax`.** Its nodes: osc, env, gain, clap, combperc, hat808, out (and bus-in, tempo
      for the master). A new `wax-render song.wax out.wav`, compared with the reference.
- [ ] **5. The other demos, smallest first**: acid, progression, chords, westcoast, then nodes and voicing (which bring
      the Node Sequencer), mosh, vocoder, weights, original, phase, suite, bugsong, interstellar-nodes.
- [ ] **6. Threads.** A pool of workers renders the instruments of a block side by side. Sidechain, listen and meta
      machines make one instrument wait for another, so those are put in order and the rest run together. The check:
      the render is the same as on one thread.
- [ ] **7. Live sound and MIDI**: miniaudio and RtMidi, a player without a window, keyjazz from a controller.
- [ ] **8. The window**: Dear ImGui on SDL3. The pattern editor first, then the sequence view, the mixer, parameter
      panels, and the drawn views (Bezier envelopes, the DX loader) last.
- [ ] **9. The long tail**: the nodes no demo uses, the genexpr node (its compiler has to be ported) and the glsl node.

## What counts as ported

A node is ported when the songs that use it render within -90 dB of the reference (`wax-compare`). The renders cannot
be identical bit for bit: the JS and C++ maths libraries differ in the last digits of sin, exp and tan. Reverbs and the
nodes that roll dice are compared more loosely. The dice themselves are the same on both sides (`core/random.h`).

## Rules for a ported node

- One file in `src/nodes/`, named after the JS file, with the JS file named in its first comment.
- The same ports and the same parameters in the same order as the JS node, so `.wax` files mean the same thing.
- A translation, not a rewrite: same variables, same order of operations. JS numbers are doubles, so state that is a
  plain JS number is a `double` here, and what the JS keeps in a `Float32Array` is a `float`.
- All memory is taken when the node is made; `process()` never allocates.
- No brand or model names, in node names, labels or comments.
