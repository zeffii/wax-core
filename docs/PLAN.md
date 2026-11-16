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
      computer keyboard and MIDI, drop a .wax on the window to open it. See docs/BUILDING.md.
      Editing cells: `edit/` has the Document (every change is an undoable Edit), the cell editor (cursor, notes, hex digits, clear,
      insert and delete row) and the sync that sends an edited lane to the playing engine (checked: an edit made while playing sounds
      exactly like the edited song from the start). In the window, Enter turns editing on; arrows, PageUp/PageDown, piano keys or
      hex digits, = for note off, ` for note cut, Delete/Backspace, Insert and Shift+Delete for rows, Ctrl+Z/Ctrl+Y.
      `wax-app --press "enter,pagedown,z,ctrl+z"` drives the window from a script for screenshots. 
      Saving: `project/save.cpp` writes the file as it was read with the lanes as edited (nothing else in it changes); Ctrl+S saves,
      Ctrl+Shift+S saves as, Ctrl+O opens, and the title shows * while there are unsaved changes. Next: panels.
- [x] **9. The long tail**: the nodes no demo uses, the genexpr node (its compiler has to be ported) and the glsl node.
      So far: cloves.wax (a user's song: 136 bpm, 1552 ticks, 7 instruments) is sample for sample identical to the web render. It
      needed 12 more nodes: haas, env2, hat2, drumsynth2, clang, urugraVerb, sympathy, kaleidoscope, pogue and lone (shared code:
      `bezier`, `spacecore`, `kalcore`). Pogue's onset detector and step pattern, and the lone's sync, fold, sub-sine and
      slide, are ported but not yet exercised by a song. Then islands, gate, ladder, eq, comb, ringmod, transient, waveshaper, saturator, drumsynth, bongo, membrane, cardinal, exciter, sweep, pulsar, pulsetrain, ensemble, reflect, split, mbsat, headpan, pan, follower, range, envfx, notetrig, ctrl, notepan, hpss, bodyblur, mbcomp, specdyn, xsynth, rotary, jawharp, khufu, granular, groove, snarl, dvii, specsynth, modchan, waveseq, vecmix, vecpath, compass, convolver, amp, route, add, genexpr and tap (tests/songs). Every node type is ported. The tap node sends its takes out through NodeContext::emit and GraphRuntime::drainTakes(); engine/takes.h assembles them (faded, into tap01 to tap16 and an optional copy), and the app layer still has to put the result in the project and call sendSample for the samplers that play it (
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

## UI: the parameter panel is a test panel for now

The current panel (flat node names, a row of sliders) exists to try sounds. The target is the web app's parameter view
(webtracker `src/graph/paramview.js`, styles `.pv-*` in index.html), to be ported when the UI is tweaked:
- a docked vertical list at the right, width dragged by a grip on its left edge (default 210, 90 to 520);
- an inverted title strip with the node name (and a pin that keeps a node's rows when another is selected);
- one 16-px row per parameter: name (9 characters, wider panels show more), then a bar with the value written inside it,
  a faint fill to the value, one thin accent mark; exposed parameters get a dot and their hex cell value at the bar's right;
  a parameter driven by a wire is dimmed; time parameters show song ticks and snap to tick lines (Alt drags freely);
- keys: Up/Down choose a row, Left/Right change it (Shift more, Ctrl less), Home/End, PageUp/PageDown, Enter opens the
  row menu (type a value, reset, expose as pattern column with 1/2/4 hex width, add a port); the keyjazz keys keep playing;
- values shown in real units (ms/s, Hz/kHz, %, st, dB, on/off, named choices).

## UI: envelopes

Drawn envelopes (env2, drumsynth2, hat2, compass, khufu, pulsar) open from "draw envelopes" next to the node names, in a floating window:
the canvas of webtracker's bezier editor (anchors, handles, point types, insert on curve, snap, log time, sustain knot) and its tool row.
Edits are `SetDrawing` (a drag is one undo) and reach a playing song through `Node::data` (queued, taken at the start of a block).
Single letters are not used on the canvas (they are piano keys): types and the knot are buttons; Delete and the arrows work while the
canvas has been clicked. Still to do: drawings of the other nodes (see drawings.cpp), a larger/undocked layout, the web's per-node views.
Test helpers: `wax-app song.wax --null --envelope inst.node --clicks "x,y;x,y" --press "..." --screenshot out.bmp`.

- DX loader window (`app/dx_window`): flat tabs voice / operator / scaling / lfo and pitch, opened from "edit voice" on a dvii node (`--dx inst.node` headless). Edits repack the voice into data.bank (SetBank, live). Still to do: bank tab (load/save .syx, names, search, swap, compare), voice names.

- Graph view (`app/graph_view`): tiles + wires of the lane's instrument above the param panel; click picks the node, drag moves it (MoveNode, saved as x/y). Wiring: pull from a tile's right dot to another tile to connect (first free ports), click the dot on a wire to cut it, right-click a tile to delete it; all undoable and sent live (Connect/Disconnect/DeleteNode, `Engine::sendGraph`: the instrument is recompiled on the window thread and swapped in at a block start; notes sounding in it are cut). Add node: right-click empty space in the graph for a filtered list of types (AddNode, live: its parameter slots are allocated when the instrument is recompiled). Still to do: choosing ports on multi-port nodes, tile shapes by role from the web, zoom/pan.

- Parameter view (`app/param_panel`, `app/units`): the web's docked list at the right of the lanes: inverted title strip, a row per parameter (name, bar with the value inside in real units from the generated units table), grip to resize (double-click resets), drag/wheel/double-click-reset, right-click menu to type a value (`2.5k`, `40%`) or reset; wired parameters are dimmed. Now a floating window over the graph (small font), with expose as pattern column (+ width) and add a port in the row menu, saved. Still to do: keys, pin, tick snapping for time parameters, tooltips.

## Web app parity map (read from webtracker master, build 774, 2026-11-15)

The web app is built as screens behind a left rail, a header, and docks. The native window follows the same shape. State of each:

- **Rail** (icons in the web, words here): Song F1, Pattern F2, Instrument F3, Sample F4, Script, Shader, DSP, Node Sequencer, Info F10, Config F11, plus Shortcuts, Manual, About.
  Native: Song, Pattern, Instrument, Info (placeholder), Config (placeholder) with the F keys. Missing: Sample (piano roll/spectrogram/slicer), Script, Shader, DSP view, Node Sequencer, Manual/About.
- **Header**: File menu (Open, Open from link, New, New keeping instruments, Save, Export WAV/MP4, Backups, save folder), play, pause, rec, follow, loop (Ctrl+B/E/L), then the LCD: len, bpm, swing, ticks per beat, beats per bar, time; project name.
  Native: Play/Stop, Follow, File menu (Open, Save, Save as), the LCD's bpm / swing / tpb / bar as spin boxes (undoable, live, saved with the song; len and time read out), loop button, Ctrl+O/S. Missing: pause, rec, editable len, project name, New, export, backups.
- **Song screen** (arrangement): vertical sequence with a pattern list at the side (width grip), lane rename/move buttons, pattern options. Native: the vertical sequence, and the pattern list at the side (the lane's patterns with rows and use count, a width grip, click outlines one, double-click / Place / Space while editing puts it at the cursor row and moves on to its end, New makes a blank one, Ctrl+Up/Down step the list; placing is one undo step). Missing: lane rename/move/duplicate/remove, pattern options (rename, colour, clone), insert silence, loop markers.
- **Pattern screen**: the grid with the shortcut sheet beside it (move, select, copy/paste, loop, Space writes the live value, etc.). Native: lane tabs and grid, typing and keyjazz; Shift+arrows selects a block, Ctrl+C/X/V copy, cut and paste (one undo step; the system clipboard carries tab/newline text), Ctrl+A selects the lane, Esc drops it. Ctrl+B / Ctrl+E / Ctrl+L set the loop start, end and switch (live in the engine, undoable, saved as song.loop, marked beside the row numbers; the header loop icon toggles it). The shortcut sheet sits beside the grid (status-bar keys toggle; lists only the keys the native app has). Space, editing, on a parameter column writes the parameter's live value (the engine's current value) into the cell and moves down; Shift+Space writes every parameter column of the row, one undo step. Missing: loop markers on the Song screen.
- **Instrument screen**: instrument list on the left (voices, tracks), the graph in the middle, parameter dock at the right, node help (Shift+F1), exposed-parameter dock. Native: list, graph (tiles, wires, add/cut/delete live), param dock. Missing: voices/tracks fields, node help, exposed dock, instrument add/remove, meta machines.
- **Mixer dock**: native mixer under Song and Pattern.
- **Sample screen**: sample browser, roll bar, slicer. Not started.
- **Docks and floats** to match later: scope rail, meters, perf meter, command bar (`|`), bounce dialog, console.

What the user will most likely ask for next, in order: (1) the header's File menu and LCD fields, (2) the pattern screen's side shortcut sheet and selection/copy/paste, (3) the Song screen's pattern list at the side, (4) instrument list fields and node help, (5) the Sample screen, (6) look and feel tweaks towards the web (spacing, thin lines, flat text, no frames).

- Icons (`app/icons`): the web's rail/header icons live in `art/icons/*.svg` (white, extracted from index.html), rasterised at run time with nanosvg (`third_party/nanosvg`) at the pixel size shown and cached per size, drawn tinted (dim / lit on hover or `on`), no frames: `iconButton(name, tip, on, size)`. The rail uses them. Still to do: use them in the header (play, pause, rec, follow, loop), the Instrument screen and docks.

- Themes and status bar: the web's eight colour themes (generated into `app/themes_table.inc` from themes.css) plus the native `slate`, chosen from the status bar's dropdown (kept in the user's preferences folder; `--theme name` for headless). Views read runtime colour variables (`kText`, `kAccent`, `textAlpha(n)`, ...). Status bar (bottom): edit state, lane and tick, octave, undo/redo, last message; at the right cpu, mixer toggle, limiter, panic, theme. Still to do from the web footer: copy load line, streams, live picture, latency, meters, cut, build number, the parameter float; the web's per-theme details (glow, radius, pattern hue) are not carried over.

### Graph tiles (as the web's compact look)
Shapes (rounded sources, boxes, circle for voices, speaker for out, route dot), sizes (compactLayout in src/app/node_look.cpp, a port of
tilelayout.js), glyphs and per-type shapes come from the web app: `node tools/gen-node-looks.mjs /path/to/webtracker > src/app/node_looks_table.inc`
records each glyph by running the web's own drawing code. Ports sit on the tile edge (the real port, so wiring picks the port; parameter
ports hang in a strip of dots under the tile), wires are coloured by kind (dashed for control/param), nodes snap to the 24px dot grid
(Ctrl for free). Not yet: node faces (the arpeggiator's step mask), hover tips on ports, wire selection, pull-out/inline insert.

### Add-node menu
The graph's right-click menu is the web's cascading insert menu (src/app/insert_menu.cpp, a port of graph/menu.js): categories, groups, nodes in
columns, Up/Down/Left/Right/Enter, underlined hotkeys, "/" or Tab for a flat search (best match first). The grouping table is generated:
`node tools/gen-insert-menu.mjs /path/to/webtracker > src/app/insert_menu_table.inc`. Not yet: replacing a node or inserting into a wire from the
menu, and the Shift+I machine tree for adding an instrument with its own lane. `--clicks "r:x,y"` right-clicks in the headless harness.

### Adding an instrument (machine tree)
Shift+I, or "+ add" above the Instrument screen's list, opens the web's machine tree (src/app/machine_tree.cpp: groups fold open and shut, thin
guide lines, arrows/Enter/letters). A pick adds a fresh instrument of that kind and a blank lane of its own, live: AddInstrument (one undo
step), then the machine compiles into the running engine and its lane follows. The kinds and their fresh instruments are generated from the web app:
`node tools/gen-machines.mjs /path/to/webtracker > src/app/machine_table.inc`. A machine whose nodes the native engine lacks, or that needs a
sample, is greyed. Undo replaces the lane in the engine by an empty one (the idle instrument stays loaded). Samplers are offered once the song has a sample (see the Samples tab).

- Song screen lane tools: rename, move earlier/later, duplicate, remove, remove with machine (undoable, saved). Still missing: pattern colour (done), (loop markers now drawn on the Song screen).
- Song screen: an overview strip at the right edge when the song is taller than the view (patterns in their colours, loop band, the part on show, cursor and playhead; click or drag to scroll). The lane's shown curve is drawn over it and edited with the mouse (src/app/curve_view.cpp): drag an anchor (snaps to pattern edges, which pins it, and to rows; Alt is free), double-click the line to add one, right-click for delete/pin/segment kind (curve, straight, gap)/extend over the next pattern/skip a pattern, right-click the lane to add a curve for a parameter, show another, switch off, delete. Mouse only (the web's E/B/[/] keys are not ported: the sequence never takes the keyboard). The editing maths is src/seq/curve_edit.cpp (a port of automation.js's editing half, tested); a drag is one undo step (SetCurves folds); pinned anchors follow their pattern when an edit moves it (Document::apply adds the follow-up as part of the same undo step, FollowedEdit). Not yet: the curve's rest value, section add/remove from the UI, the value box beside the cursor.
- Song screen: insert silence (whole song, patterns crossing the row stay whole; the web's 'cut' option not ported).

- Pattern screen: "Pattern view" options in the browser (also Hide empty columns, Follow the playing row, Note preview, Text size; still missing: Pattern window, Pattern roll): all lanes side by side (lane names over their columns, click a cell to move the cursor into any lane) or one lane per tab; patterns tinted by colour.
- Pattern columns: headers are the short names (Cut -> ct, as the web), and a small line under the grid says what the caret column is, its range and the cell in real units (describeColumn)..
- Samples tab (browser panel): the samples in the song, then a folder browser of the computer (src/app/sample_browser.cpp, a port of the web's sample browser): click a .wav to hear it (src/live/preview.h mixes it over the song; waveform with a playhead), double-click a folder to open it, "Load as a new sample" or double-click a file adds it to the project (AddSample, undoable; id as the web names it; kept as 16-bit PCM in the file, rate as the file's, not resampled). Machines that play samples (Sampler, Granular, Spectral, MOD channel) are offered in Shift+I once the song has a sample and are given it (first one, or all as slots) as in the web's addSynthInstrument; the engine sends a new machine's samples live (GraphRuntime::queueSample). Only .wav (16/24/32-bit float) is read: no decoder for flac/mp3/ogg; no files over 10 MB without asking is wired but the limit is not in Config yet. Not ported: the Sample screen (roll, spectrogram, slicer, loop points), Record, replace, rename/delete sample, keys in the file list, remembered folder.
- Browser: a panel down the left of every screen (Alt+B or the status bar toggle), tabs Patterns / Machines / Samples (samples not ported), width draggable (all the way left closes it), and under the lists the options of the screen shown (the Song screen's lane tools and insert silence), split by a drag bar. Still missing: the options of the Instrument (voices, presets), Pattern and Sample screens.
- Header: pause (Pause key), editable len (a row per click, undoable, trims placements), project name box (saved). Rec (MIDI record into patterns) not ported.
- File menu: New (an empty song: the master lane only; Save asks for a file), Export WAV (offline render on a thread, status line says when done). Not ported: Open from link, New From Existing, Backups, Save folder, Reload app. Splash: the Wax logo and a loader bar at start (--no-splash / --splash).
- Info screen: the song's notes (saved as song.info, one undo step each time the box is left). Config screen: theme, follow, playhead style (marker/line/band), wire colours and dashed control wires, reset; kept in config.json in the preferences folder. Web settings not ported: most of the rest (sample, resynthesis, console, MIDI, backup, export, meters, script, visual, font size, tint, scope...).
- Graph wiring now as the web: drag output to input or free input to output; a port with wires picks up a wire (drop on a port to move, elsewhere unplugs, Esc restores, Shift+drag a new wire); click a wire to select, Delete cuts; loops refused (message under the graph); hover text for ports and wires. Not yet: Tab to pick among several wires, insert-into-wire and pull-out by dragging a node.
- Instrument screen browser options: voices, tracks (a spin box, 1 to 8), sleep when quiet, move earlier/later (undoable, saved). The browser switches to Machines on the Instrument screen and Patterns on Song/Pattern. Also rename (a box), duplicate and remove (refused while a lane plays it or a sidechain/meta machine refers to it). Presets: a list under the machine options (click recalls, Save/Overwrite/Rename/Delete; ids stable for command 00; a preset also keeps the nodes' drawings (node.data): saved with it, put back on recall, one undo step). Missing: new instrument name/kind box, library. Layout rule from the user: the native UI may be cruder at first, but things must sit in roughly the same places as the web app.
