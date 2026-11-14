# The plan

Agreed on 2026-11-14. The web app (webtracker) is the reference and stays as it is; this repo is the native engine,
built engine first, with the window last. Tick a step when it is done.

- [x] **1. Reference renderer** (in webtracker, build 772). `node tools/render-ref.mjs song.wax` writes a repeatable
      32-bit float render and a `.json` of facts; every later step is checked against it.
- [x] **2. Skeleton.** CMake, C++20, the `.wax` reader, the parameter block, the node interface, the first node (gain),
      `wax-info`, `wax-compare`, tests, and a build on Windows and Linux for every push.
- [x] **3. Sequencer and graph runtime**, on one thread: the graph compiler (`graph/compile.cpp`), the runtime
      (`engine/runtime.cpp`: voices, sleep, kill, delay compensation), the pattern reader (`seq/`), the sequencer
      (`engine/sequencer.cpp`), the limiter, and the master machine every project gets when it is loaded.
- [x] **4. First song: `concrete.wax`.** Its nodes: osc, env, gain, clap, combperc, hat808, out, bus-in, tempo and
      the hidden sum, fold and delay nodes. `wax-render song.wax out.wav` renders it, and the result is the same as
      the reference to the last bit (on Linux with GCC; other compilers' maths may differ in the last digits).
- [x] **5. The other demos, smallest first.** (all 15 done)
      Done, each within -140 dB of the reference or closer: acid, progression, chords, westcoast (which brought squelch,
      subsynth, duette, chord, chorus, hall, plate, pluckgate, wavefolder, westcoast, random, times).
      nodes and voicing, which brought the Node Sequencer (`src/nodeseq/`), are identical to the reference.
      weights and vocoder followed (prismacanyon, lfo, delay, sidechain, vocoder), within -140 dB and identical.
      mosh.wax is now sample for sample the same as well (see "Where the maths libraries matter").
      original, phase, suite and interstellar-nodes followed (19 more nodes: filter, noise, crusher, gater, multitap,
      freqshift, phaser, shimmer, shiva, woodblock, specpan, blur, addsynth, arp, tape, string, wavetable, fmsynth, piano),
      all identical to the reference. bugsong, the last, needed lane automation curves (`seq/automation.cpp`), samples sent to
      nodes (`project/samples.cpp`, `GraphRuntime::sendSample`) and nine more nodes: distortion, dcblock, mix,
      compressor, flanger, filterpro, oberheim, fourpole and the sampler. It is identical to the reference too, and so
      are variants of it that switch on the sampler's repeats, grains, filters, vibrato and slides.
- [x] **6. Threads.** A pool of workers renders the instruments of a block side by side. Sidechain, listen and meta
      machines make one instrument wait for another, so those are put in order and the rest run together. The check:
      the render is the same as on one thread.
      Done: `GraphRuntime::setThreads(n)` / `wax-render --threads n` (`engine/task_pool.cpp`). Each block, every instrument
      but the master is a task that waits for the instruments it hears through sidechains (and for a meta machine's
      machines); the mix is added up afterwards in the order of the file, and the master runs last on the sum. Byte for
      byte the same output on 1, 2, 4 and 8 threads (tests/tests.cpp, testThreads). A sidechain on an instrument that
      runs later hears nothing, as on one thread. The one thing the order of threads could still touch is a loop of
      sidechains between meta machines (the sound of a meta machine is made once a block, by whoever asks first).
- [x] **7. Live sound and MIDI**: miniaudio and RtMidi (vendored in third_party), `wax-play`: a player without a window, keyjazz from a MIDI controller. The set-up shared with the offline renderer is now `engine::Engine`. Tested here on miniaudio's null device and with `--dry` (the audio callback run by hand in odd block sizes: bit-identical to `wax-render`); real sound and real MIDI are for a Windows ear. `-DWAX_LIVE=OFF` skips it.
- [ ] **8. The window**: Dear ImGui on SDL3. The pattern editor first, then the sequence view, the mixer, parameter
      panels, and the drawn views (Bezier envelopes, the DX loader) last.
      First slice done (`wax-app`): transport, a pattern table per lane that follows the playhead, level meters, keyjazz from the
      computer keyboard and MIDI, drop a .wax on the window to open it. See docs/BUILDING.md. Next: editing cells.
- [ ] **9. The long tail**: the nodes no demo uses, the genexpr node (its compiler has to be ported) and the glsl node.
      So far: cloves.wax (a user's song: 136 bpm, 1552 ticks, 7 instruments) is sample for sample identical to the web render. It
      needed 12 more nodes: haas, env2, hat2, drumsynth2, clang, urugraVerb, sympathy, kaleidoscope, pogue and lone (shared code:
      `bezier`, `spacecore`, `kalcore`). Pogue's onset detector and step pattern, and the lone's sync, fold, sub-sine and
      slide, are ported but not yet exercised by a song. Then islands, gate, ladder, eq, comb, ringmod, transient, waveshaper, saturator, drumsynth, bongo, membrane, cardinal, exciter, sweep, pulsar, pulsetrain, ensemble, reflect, split, mbsat, headpan, pan, follower, range, envfx, notetrig, ctrl, notepan, hpss, bodyblur, mbcomp, specdyn, xsynth, rotary, jawharp, khufu, granular, groove, snarl, dvii, specsynth, modchan, waveseq, vecmix, vecpath, compass, convolver, amp, route and add (tests/songs). Still unported: 2 node types (tap, genexpr) (
      genexpr ...); `wax-info song.wax` lists what a song is missing.

## Parts of the engine still to come

Each is named in a comment where it will go, and `wax-render` says so when a song needs one of them:

- the Node Sequencer's `script` node (it runs JavaScript typed into the song; no demo uses it)
- lane curves, the automation (`seq/automation.js`, `Sequencer.applyCurves`), with `bugsong.wax`
- node messages: samples, drawn waves and curves, and the data a preset sends (`runtime.message`)
- the mixer's carve stage (`audio/carve.js`)
- the arp's note input and the sidechain's tap (the hooks are in `core/node.h` and the run order already honours sidechains)

## What counts as ported

A node is ported when the songs that use it render within -90 dB of the reference (`wax-compare`). Most of the demos
render byte for byte the same as the web app. Reverbs and the nodes that roll dice are the ones to watch if one drifts.
The dice themselves are the same on both sides (`core/random.h`).

## Where the maths libraries matter

The system maths library gives a last digit that differs from the JS engine's in 1 to 40 of every 100 calls (measured:
sin 3%, exp 10%, atan2 18%, cbrt 40%). Most nodes stay 140 dB below the sound anyway, but mosh and the other spectral
nodes make yes-or-no decisions from those numbers, so one decision falls the other way and the renders part company.

So every node calls `js::sin`, `js::pow` and the rest (`core/js_math.h`), which run a copy of the JS engine's own maths
(V8's fdlibm port, `third_party/v8-ieee754`, with its licence). It is the version in Node 22, the engine that makes the
reference renders. It checks out bit for bit against Node on 400,000 random inputs for each of 18 functions, and it makes
Windows and Linux agree exactly. Never call `std::sin`, `std::pow` and friends in a node. The build turns off fused
multiply-add (`-ffp-contract=off`) so a compiler can't change a last digit either.

A browser's own V8 may differ from Node 22's in some functions (newer V8 moved some maths to a different library), so
the browser and a render can part by a last digit in rare cases; `.wax` files are unaffected.

## Rules for a ported node

- One file in `src/nodes/`, named after the JS file, with the JS file named in its first comment.
- The same ports and the same parameters in the same order as the JS node, so `.wax` files mean the same thing.
- A translation, not a rewrite: same variables, same order of operations. JS numbers are doubles, so state that is a
  plain JS number is a `double` here, and what the JS keeps in a `Float32Array` is a `float`.
- All memory is taken when the node is made; `process()` never allocates.
- No brand or model names, in node names, labels or comments.
