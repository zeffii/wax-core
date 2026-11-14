# wax-core

The native engine of [Wax](https://github.com/zeffii/webtracker), the tracker: the same songs (`.wax` files), the same
machines, in C++ for speed and for rendering instruments on several processor cores at once. The web app stays as it is
and is the reference: a machine counts as ported when this engine's render of a song matches the web app's.

The order of work and where it stands is in [docs/PLAN.md](docs/PLAN.md).

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
| `src/nodes/` | the machines' DSP, one file per node, each a translation of `webtracker/src/audio/nodes/<name>.js` |
| `src/project/` | reading `.wax` files |
| `src/io/` | sound files, and comparing two renders |
| `tools/` | the command-line programs |
| `tests/` | the tests (`ctest`), and the songs they use |

## The tools

    wax-info song.wax                        what a song is made of, and which of its node types are ported
    wax-compare reference.wav native.wav     how far two renders are apart (passes under -90 dB)

A reference render comes from the web app's repo: `node tools/render-ref.mjs song.wax`.
