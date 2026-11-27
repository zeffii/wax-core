# wax-core

The native engine of [Wax](https://github.com/zeffii/webtracker), the tracker: the same songs (`.wax` files), the same
machines, in C++ for speed and for rendering instruments on several processor cores at once. The web app stays as it is
and is the reference: a machine counts as ported when this engine's render of a song matches the web app's.

The order of work and where it stands is in [docs/PLAN.md](docs/PLAN.md). Porting a node: [docs/ADDING_NODES.md](docs/ADDING_NODES.md).

## Building (Windows 11)

You need [Visual Studio 2022](https://visualstudio.microsoft.com/) (the free Community edition or just the Build Tools)
with the "Desktop development with C++" workload, which brings CMake with it. Then, in a "Developer PowerShell for VS 2022":

    cmake -S . -B build
    cmake --build build --config Release
    ctest --test-dir build -C Release

The programs land in `build\Release\`. Or open the folder in Visual Studio (File > Open > Folder), which reads
`CMakeLists.txt` by itself. On Linux and macOS the same three commands work with GCC or Clang.

## What is here

| path | what it is |
| --- | --- |
| `src/core/` | the parameter block, what a node is, the list of node types, the dice |
| `src/graph/` | the graph compiler: an instrument's nodes and wires become a list of nodes to run and the buffers between them |
| `src/engine/` | the runtime (runs the graphs, voices, sleep), the sequencer (plays the patterns), the limiter, offline rendering |
| `src/nodeseq/` | the Node Sequencer: a graph of event and control wires that plays beside the patterns |
| `src/seq/` | pattern cells and columns, and laying a lane's patterns out along the song |
| `src/nodes/` | the machines' DSP, one file per node, each a translation of `webtracker/src/audio/nodes/<name>.js` |
| `src/project/` | reading `.wax` files, and the master machine every project gets |
| `src/io/` | sound files, and comparing two renders |
| `tools/` | the command-line programs |
| `tests/` | the tests (`ctest`), and the songs they use |

## The tools

    wax-info song.wax                        what a song is made of, and which of its node types are ported
    wax-render song.wax out.wav              renders a song offline to a 32-bit float WAV
    wax-play song.wax [--loop] [--midi n] [--instrument id] [--null] [--dry out.wav]
                                             plays through the sound card, no window; a MIDI keyboard plays the
                                             instrument (keyjazz); terminal: p play, s stop, n 60 / f 60 notes, q quit
    wax-play --list-midi                     lists the MIDI input ports
    wax-compare reference.wav native.wav     how far two renders are apart (passes under -90 dB)

A reference render comes from the web app's repo: `node tools/render-ref.mjs song.wax`.
