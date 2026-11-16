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
- **Sample screen** (F4): done in part, see "Sample screen" below. Still to do: piano roll, chord detection, record.
- **Docks and floats** to match later: scope rail, meters, perf meter, command bar (`|`), bounce dialog, console.

What the user will most likely ask for next, in order: (1) the header's File menu and LCD fields, (2) the pattern screen's side shortcut sheet and selection/copy/paste, (3) the Song screen's pattern list at the side, (4) instrument list fields and node help, (5) the Sample screen, (6) look and feel tweaks towards the web (spacing, thin lines, flat text, no frames).

- Icons (`app/icons`): the web's rail/header icons live in `art/icons/*.svg` (white, extracted from index.html), rasterised at run time with nanosvg (`third_party/nanosvg`) at the pixel size shown and cached per size, drawn tinted (dim / lit on hover or `on`), no frames: `iconButton(name, tip, on, size)`. The rail uses them. Still to do: use them in the header (play, pause, rec, follow, loop), the Instrument screen and docks.

- Themes and status bar: the web's eight colour themes (generated into `app/themes_table.inc` from themes.css) plus the native `slate`, chosen from the status bar's dropdown (kept in the user's preferences folder; `--theme name` for headless). Views read runtime colour variables (`kText`, `kAccent`, `textAlpha(n)`, ...). Status bar (bottom): edit state, lane and tick, octave, undo/redo, last message; at the right cpu, mixer toggle, limiter, panic, theme. Still to do from the web footer: copy load line, streams, live picture, latency, meters, cut, build number, the parameter float; the web's per-theme details (glow, radius, pattern hue) are not carried over.

### Graph tiles (as the web's compact look)
Shapes (rounded sources, boxes, circle for voices, speaker for out, route dot), sizes (compactLayout in src/app/node_look.cpp, a port of
tilelayout.js), glyphs and per-type shapes come from the web app: `node tools/gen-node-looks.mjs /path/to/webtracker > src/app/node_looks_table.inc`
records each glyph by running the web's own drawing code. Ports sit on the tile edge (the real port, so wiring picks the port; parameter
ports hang in a strip of dots under the tile), wires are coloured by kind (dashed for control/param), nodes snap to the 24px dot grid
(Ctrl for free). Not yet: node faces (the arpeggiator's step mask).

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
- Samples tab (browser panel): the samples in the song, then a folder browser of the computer (src/app/sample_browser.cpp, a port of the web's sample browser): click a .wav to hear it (src/live/preview.h mixes it over the song; waveform with a playhead), double-click a folder to open it, "Load as a new sample" or double-click a file adds it to the project (AddSample, undoable; id as the web names it; kept as 16-bit PCM in the file, rate as the file's, not resampled). Machines that play samples (Sampler, Granular, Spectral, MOD channel) are offered in Shift+I once the song has a sample and are given it (first one, or all as slots) as in the web's addSynthInstrument; the engine sends a new machine's samples live (GraphRuntime::queueSample). Only .wav (16/24/32-bit float) is read: no decoder for flac/mp3/ogg; no files over 10 MB without asking is wired but the limit is not in Config yet. Not ported: Record, replace, rename/delete sample, keys in the file list, remembered folder.
- Browser: a panel down the left of every screen (Alt+B or the status bar toggle), tabs Patterns / Machines / Samples (samples not ported), width draggable (all the way left closes it), and under the lists the options of the screen shown (the Song screen's lane tools and insert silence), split by a drag bar. Still missing: the options of the Instrument (voices, presets), Pattern and Sample screens.
- Header: pause (Pause key), editable len (a row per click, undoable, trims placements), project name box (saved). Rec (MIDI record into patterns) not ported.
- File menu: New (an empty song: the master lane only; Save asks for a file), Export WAV (offline render on a thread, status line says when done). Backups: a snapshot every few minutes while the song changes (src/project/backups.cpp; Config > Backup), File > Backups lists them and opens one as an untitled song. Not ported: Open from link, New From Existing, the backup download/folder options, Save folder, Reload app. Splash: the Wax logo and a loader bar at start (--no-splash / --splash).
- Info screen: the song's notes (saved as song.info, one undo step each time the box is left). Config screen: theme, follow, wire colours and dashed control wires, reset; kept in config.json in the preferences folder. Web settings not ported: most of the rest (sample, resynthesis, console, MIDI, backup, export, meters, script, visual, font size, tint, scope...).
- Graph wiring now as the web: drag output to input or free input to output; a port with wires picks up a wire (drop on a port to move, elsewhere unplugs, Esc restores, Shift+drag a new wire); click a wire to select, Delete cuts; loops refused (message under the graph); hover text for ports and wires. Dragging a tile onto an audio wire puts the node into it (InsertOnWire: first input and first output; the wire lights up, the status line says so; one undo step apart from the move); Config > "Graph: dragging a node far out of its chain takes it out" (off by default, as the web) or Shift+drag takes it out, its neighbours joined (PullOut, a dashed line shows the join); Esc during the drag moves the node alone. Tab on a port with several wires lights the next one, and a press on the port picks that one up (Shift+Tab goes back).
- Instrument screen browser options: voices, tracks (a spin box, 1 to 8), sleep when quiet, move earlier/later (undoable, saved). The browser switches to Machines on the Instrument screen and Patterns on Song/Pattern. Also rename (a box), duplicate and remove (refused while a lane plays it or a sidechain/meta machine refers to it). Presets: a list under the machine options (click recalls, Save/Overwrite/Rename/Delete; ids stable for command 00; a preset also keeps the nodes' drawings (node.data): saved with it, put back on recall, one undo step). Missing: new instrument name/kind box, library. Layout rule from the user: the native UI may be cruder at first, but things must sit in roughly the same places as the web app.

- Sample screen (F4, src/app/sample_view.cpp; the audio tools are src/project/sample_edit.cpp, a port of the web's sampleedit.js and the sample parts of ops.js): the samples in a list, the loop and sustain loop (on/off, start, end, crossfade), selection, rate and the three levels as small controls, the edit tools (crop, cut, silence, reverse, normalise, fade in/out, remove DC, bake crossfade, copy, paste) as words and as Shift+C/X/S/R/N/I/O/D/B, Ctrl+C/X/V (Shift+V overwrites), and the waveform with the loops, selection, slice markers (with their Off-cell hex) and playhead: drag selects, Alt+drag makes or moves the loop, Ctrl+drag the sustain loop, a loop edge grabs when the mouse is on it, Alt+click makes a slice marker (Alt+drag moves, Alt+double-click removes), click puts the playhead, double-click clears the selection, Ctrl+wheel zooms, wheel and middle drag move along, Shift+Z/A/W zoom to selection/out/whole, Shift+M/U mark and remove a slice at the selection. Every change is one undoable SetSample edit (the sample as the file keeps it, so it is saved as 16-bit PCM); a slider drag folds into one step; Ctrl+Z is the same global undo as everywhere (the web keeps a separate history per sample). Document::takeChangedSamples + syncSamples give every node that plays the sample the new version live (Engine::sendSample, via the queueSample queue). Piano keys and Space audition the sample through the preview mixer, pitched by resampling (root note C-5), without loops, from the start (Space: from the playhead); the web plays it through the sampler with its loops. Not ported: spectral sharpening of the spectrogram, piano roll (Shift+P), chord detection (Shift+H), hide-switches for the loop/selection display, typing a value in a number field (the arrows and the wheel step it).
- Sample screen, name field (Enter renames; instruments that play it follow: RenameSample), Remove (refused while an instrument plays it: RemoveSample, undoable), and - / + nudge the current slice marker by one frame.
- Replace: with the Sample screen shown, the Samples tab offers "Replace <sample>" for the picked file (new audio, levels kept, loops/markers dropped; one SetSample edit).
- Spectrogram (src/project/spectrogram.cpp, a port of spectrogram.js, checked against its numbers): the Spectrogram / Waveform word on the Sample screen switches the display; one cell per pixel over the part shown, drawn as a texture (pixelTexture in icons.cpp), with the loops, selection and markers over it.
- Auto-slice (Shift+E or the word beside Spectrogram; src/project/onsets.cpp, a port of onsets.js checked against its frames): a dialog with sensitivity, shortest gap, "replace the markers in range" and "whole sample", a live count of the hits, and faint ghost markers on the waveform; Slice writes the markers (one undo step). The settings live in the screen, not in config.json yet.
- Config: new groups Editing (Up/Down wrap rows, Left/Right wrap lanes) and Sample (auto-slice settings, hide switches for the loop/selection/sustain regions, slice hex mode, ask-before-reading size). In the Pattern screen's All lanes view Left/Right now carry the cursor into the next lane at the first/last column (laneWrap: round the ends), as the web app does. The auto-slice dialog edits the same settings (saved in config.json).

- App start: with no file given the app opens a new empty song (the master lane only; blankProject() in tools/wax_app.cpp) instead of asking for a dropped file. The header begins with the small Wax mark and the File menu, to the right of the rail's thin dividing line.
- Config > Engine: render threads (1 to the cores the machine reports; saved as `threads`, `--threads` still overrides at start). Shows cores available and in use, with the hint that using all cores may not be in your interest. Changing it stops the sound card callback, rebuilds the engine's thread pool and restarts the audio (the sound is the same on any number).
- Graph: right-click a tile > replace... opens the add menu for the type it becomes (ReplaceNode, a port of edit.js replaceNode: wires follow by port name or position, same-range parameters keep their values, a node named for its type gets the new type's name, exposed columns/commands/sample choices follow or go; undo restores the machine exactly). Pattern view (all lanes): lane names are drawn over the cells, not as cell text, so a long name no longer stretches its first column.
- Graph: right-click an audio wire opens the add menu (only types with an audio input and output); the pick is added at the wire and wired into it as one undo step (FollowedEdit of AddNode + InsertOnWire).
- Graph keys and groups (as the web's patcher): drag on empty space to lasso nodes (Shift+click adds or drops one); a group moves together (MoveNodes, one undo), Ctrl+D duplicates the picked node or group with its wires between them (DuplicateNodes), Delete / Backspace / Ctrl+X removes (one undo for a group), Alt+R removes and joins the wires round (JoinAround + DeleteNode), Tab / Shift+Tab step through the nodes left to right, Alt+arrows go to the nearest node that way. Keys act while the mouse is over the graph and no text box has the keyboard. Not yet: Alt+V (keep pictures), arrows into the param panel.
- Node help: Shift+F1 on the Instrument screen opens the picked node's help in a floating window (src/app/node_help.cpp; the written pages of help.js and every parameter hint of hints.js are generated by `node tools/gen-node-help.mjs /path/to/webtracker > src/app/node_help_table.inc`): summary, ways to get the most out of it, limits, links (open in the browser), sections, then ports and each parameter with its hint and range. Nodes without a written page show the ports and parameters. Hovering a parameter's name in the panel shows its hint and range.
- Build note: always check the build's exit status; an earlier run grepped the log for the wrong pattern and tests ran on a stale binary. Fixed: document.cpp helper order, graph_view.cpp include, the node_help.cpp target.

- Pattern editor column widths are fixed from every row (heading or widest cell), so they no longer change while scrolling. A song without a master machine gets BPM/Swg/TPB/Vol columns only; the limiter's and carve stage's columns appear when exposed from the parameter panel (saved masters keep what they saved).

- Exposed-parameter dock (`drawExposedDock`, param_panel.cpp): a window at the graph's left with a row per exposed column of the machine (named by its label, same bars/menus as the parameter panel via the shared `drawRow`); shown only when something is exposed. Still to do: keys into the docks, pin/float copies for other machines.

- Parameter views: a panel wider than ~330 px shows each row's buttons (e: pattern column on/off, p: port on/off, and the column's width 1h/2h/4h, "--" when not a column), as the web does; narrower, only the right-click menu. The parameter panel and the exposed dock are both dragged by their title and resized by their edge; their height follows the rows (what last frame's content needed), at most 2/3 of the graph area, then a scrollbar.

- Equaliser response picture (`app/node_response`): the picked (or Alt+V kept) eq node shows its combined response, each band's thin curve, numbered dots and cut triangles beside it; dragging a dot sets the band's frequency (sideways) and gain (up/down, the dot sits on the combined curve), a triangle sets a cut; one undo step per drag (SetParam merged). Not yet: arrows into the param panel, nudging the graph clear of the exposed dock on load.

- Nudge: the first time an instrument's graph is shown with the exposed dock over some of its nodes, the whole graph moves clear of the dock (down when that fits the view, else right), as one MoveNodes edit (the web's `nudgeClear`).

- Parameter view keys (both panels): while the mouse is over a panel and no text box has the keyboard, Up/Down choose a row (a thin accent mark at its left), PageUp/PageDown jump eight, Left/Right change it by 1% of its range (Shift 5x, Ctrl 0.2x; whole steps for integers), Home/End set the minimum/maximum, Enter opens the row menu. Alt+arrows stay the graph's. Not yet: a pin/float copy of a panel for other machines.

- Floating parameter windows on the Song and Pattern screens (`drawFloatPanels`): the status bar's "params" button shows a window with the exposed sliders of the cursor lane's machine (follows the lane); its "pin" keeps a window for that machine open (several can be pinned; X or "unpin" puts one away). Same rows, keys, buttons and dragging as the exposed dock. Pins are not saved yet; pop-out windows not ported.

- Fix: the floating panels (parameter panel, exposed dock, follow/pinned windows) went behind the main window as soon as the graph was clicked (it took the focus and came to the front); the main window now has NoBringToFrontOnFocus.
- Step levels window (`app/gate_window`, SetNodeData edit): "edit steps" in the parameter panel of a trance gate or arpeggiator opens bars for the node's data.gate (click/drag to draw, presets trance/16ths/8ths/offbeat/gallop/swell/random/open); the arpeggiator reads the first 16 of the 64 levels. First of the web's per-node views; the others (taps, spectrum, canyon, drum, kaleidoscope, ...) are still to do.

- Spectrum bars for the additive synth ("draw spectrum", 32 partials, presets saw/square/triangle/organ/hollow/formant/sine) share the window below.
- Wave and curve drawing window (`app/shape_window`): "draw wave" on an oscillator (one cycle of 128 points, data.wave) and "draw curve" on a waveshaper (129 points, data.curve, optional symmetric): drag on the pad, presets, and for the wave smooth/flip/reverse/mirror/random; one SetNodeData per change, a drag is one undo. Not yet: the FM synth's per-operator waves, westcoast's drawn wave, spectrum (addsynth), taps (delay), chord names, wavetable, genexpr code, and the web's other per-node views.

- Tap view for the delay ("edit taps", `drawTaps` in shape_window.cpp): level stems and pan dots, click empty space to add (up to 16), drag to move, double-click to remove/centre, presets even/ping-pong/accel/random/default; the comb response curve of the web's view is not drawn.

- FM synth: "draw wave" has op 1-4 tabs (data.waves, one 128-point wave per operator), in the same window as the oscillator's wave.

- Chord node: "edit chords" opens the 256-slot window (pages 0x-Fx, a box per slot with the notes it makes, unreadable names marked); data.chords, one undo step per slot.

- Westcoast: "draw wave" shares the oscillator's wave window (data.wave).

- Wavetable: "edit table" opens a window with the table stack (bundled set or cut from a sample, per-frame size), waves A and B cut from any sample (frame box, ±16, snap, click the strip) and the blend at the current mix; data.table/pair as base64, data.meta remembers sources, one undo step each. `SetNodeData::also` sets a second key in the same step; a null value erases a key.

- Parameter-only node pictures (`app/node_pictures`, shown beside the picked node or with Alt+V like the envelopes): prismacanyon (echo bars per delay line against the song's ticks, pan and cutoff per line), ensemble (the three delays' length over 4 s), haas (early/late pulses and the mono comb). Display only: the sliders move them (the web's haas pulse drag is not ported). Still to do: the pictures that need the audio thread to report (notepan, pogue, filterpro, ...), and the other parameter-only ones (granular, membrane, kaleidoscope, drum, sympathy, compass pad and wave sequences, pulsar; reflect, rotary and khufu are done).

- Pictures: reflect (the room from above: drag the sound, the listener and the room's corner; the echoes in time) and rotary (horn and drum turning at the speeds the node's rule gives them, the stop/slow/fast switch is clickable). Picture handles use params starting with '@' (`dragPictureHandle`).
- Pattern view: the gap between two columns is exactly one character (cell padding is half a character each side).

- Pattern view text size: Ctrl + mouse wheel over the pattern grows or shrinks its text (50% to 300%, kept while the app runs; not yet saved in the preferences).

- Khufu picture: two cycles of the low/mid/high registers, each faint and the mix as it is added up.

- Membrane picture: the head with the pattern of the picked mode (filled dot up, hollow down, a cross where it is struck) and every mode as a stem along its pitch; click a stem to see its pattern (kept for the session). Uses `membraneModes` from the node itself so it agrees with the sound.

- Showing or hiding a parameter column now moves the cells of the patterns with it (`SetExposed`: the cell is taken out of, or a blank put into, every row of every lane that plays the instrument; undo restores the rows). Before, the rows kept their old cells and everything after the column slid into the wrong column. Meta lanes are not remapped yet. Ctrl + wheel over the pattern changes the saved "Text size" setting; `--all-lanes` shows every lane side by side in screenshots.

- Granular picture: the sample (waveform, position, the spray band, a scan arrow), the grains (shape, density, overlap) and the cloud (pitch, reverse, spread). A node with a picture and envelopes (granular) shows the envelopes under the picture. Still to do among the parameter-only views: kaleidoscope, the drum machines, hat2, sympathy (live), compass pad and wave sequences, pulsar.
- Double-click on empty graph space opens the add-node menu (as the web app).
- File > Recent: the last 10 songs (opened or saved), kept in the settings; one that has moved shows dim and opening it only warns in the status bar. Mixer dB scale: a space before the 0 so first digits align.
- Kaleidoscope picture (band with draggable ends and body; output tiles/overtones; no live spectra). File > Recent is now a submenu.
- Drum synth picture: envelopes, click, composed root note and first 25 ms, rendered by the node's own code offline (a click from a sample is not drawn). drum2/hat2 (bezier drawings) still to do.
- Drum synth 2 and hi-hat 2 pictures: the drawings with their lengths and the hit(s) rendered by the node's own code (the drawings are edited in the envelope window).
- Song screen: keyboard cursor (arrows row/lane, Tab, PgUp/PgDn 16 rows, Home/End), Enter opens the pattern under the cursor (creates one on empty space), Esc ends editing, H / Config toggles pattern names (hex number stays; default off as the web), click a tick number plays from there, shortcut sheet for the Song screen. Still to port from the web sequence view: remove, clone, slice, resize, move, push/pull, mute, colour, glue, selection.
- Song screen edits: remove, clone, slice, move, resize, push/pull, mute and note-off marks, colour, insert-over (src/edit/sequence_ops, one undo step each via SetLaneSequence; unit-tested). Pattern editor: thin lines where patterns start and end, the patterns other than the one under the cursor dimmed, status bar says which placement (2 of 8). Still to port: block selection and glue, waypoints.
- Song screen block selection (Shift+arrows, tinted), Delete clears the patterns starting in it, G glues it into one new pattern per lane (a prompt when some lanes play nothing there), W / Shift+W waypoint (restore is one undo step). Multi-lane edits go through SetLaneSequences (unit-tested). Still open: block copy/paste/duplicate (Ctrl+C/X/V/D), loop around pattern (Shift+L).
- Alt+S solo / Alt+M silence the cursor lane (Song and Pattern screens), S button beside M in the mixer; solo is transient (Lane::solo/soloMuted, Document::toggleSolo, mask value 3).
- Song screen blocks: Ctrl+C/X/V/D (capture/placeClip in sequence_ops: same lane = same pattern, other lane with equal columns = copy; duplicate copies below and moves the selection). Fixed MSVC C4996 (u8path).
- Tidy: the Song screen keyboard moved out of tools/wax_app.cpp into src/app/song_keys.{h,cpp} (SongEdit state, songCursorKeys, songBlockKeys, laneSoloKeys, glueBlock); wax_app.cpp is down by ~270 lines. New keyboard features go there.
- Graph view: middle-button drag pans the view (GraphViewState::pan; the tiles stay put). Parameter panel: the active-row marker stands half a character clear of the name, and the pattern-column tag (short name + hex) is no longer drawn, as in the web app.
- Notes: a "note" graph node (data { text, open }), no ports and no sound; Control > note in the add menu, folds with the marker at its left, press the text to type (one undo step on leaving, Esc leaves). Web app needs the same node type to show them.
- Playhead: one thin red line gliding through the playing row (no tint, no marker, no Config choice). The display follows the song as it is HEARD: `AudibleClock` (tools/wax_app.cpp) looks the audio thread's tick up as it was one device latency ago (`AudioOut::latencySeconds`), so the line and the follow scroll no longer run ahead of the sound.
- Notes (graph): the text is UI size (12px), the note sits on an amber tint, the whole strip down its left edge is the drag handle, and the fold marker and a tint tickbox (`data.fill`, on unless false; changes only the background) sit at the top right. Same layout in the web app (build 777).
- Pattern follow: the scroll and the playhead line move by the MEASURED row pitch (rows drawn last frame), not the line height: a table row is 17px here where the line height said 20, which made the scroll run ahead of the playhead. The note node is renamed `msg` (files saved with `note` load as `msg`; matches web build 778).
