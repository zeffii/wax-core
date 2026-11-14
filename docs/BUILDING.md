# Building Wax on Windows with VS Code

## Once

1. Install **Visual Studio Build Tools 2022** (or Visual Studio Community) with the workload *Desktop development with C++*.
   That brings the MSVC compiler, CMake and the Windows SDK. (`winget install Microsoft.VisualStudio.2022.BuildTools`
   then add the workload in the installer.)
2. Install **Git** and **CMake** if they are not there yet (`winget install Git.Git Kitware.CMake`).
3. In VS Code install the two extensions it suggests when the folder opens: **CMake Tools** and **C/C++**.

## Every time

1. `git clone https://github.com/zeffii/wax-core` and open the folder in VS Code (*File > Open Folder*).
2. The first time, CMake Tools asks for a **kit**: pick *Visual Studio Build Tools 2022 Release - amd64*
   (or the same for Community). It then configures by itself. This step downloads SDL3 and the JSON library, so it
   needs the internet once.
3. **Build:** `F7`, or the *Build* button in the blue status bar. The first build takes a few minutes (SDL3 is
   built too); later ones only rebuild what changed.
4. **Run:** pick the target in the status bar (the play-triangle button: `wax-app`), then `Ctrl+F5`.
   To open another song, drop a `.wax` file on the window. Or use *Run and Debug* (`F5`) and choose
   *wax-app: concrete demo*; that runs under the debugger.
5. **Tests:** `wax-tests` as the target, then `Ctrl+F5`; it should end with `all tests passed`.

Buttons in the status bar: the build type (Release or Debug; Release is the one to listen with, Debug is slow),
the kit, the target to build, and the target to run.

## Without VS Code

    cmake -S . -B build
    cmake --build build --config Release --parallel 3
    build\Release\wax-app.exe tests\songs\concrete.wax

## Options

- `-DWAX_LIVE=OFF`: no sound or MIDI (just the offline tools).
- `-DWAX_GUI=OFF`: no window (keeps `wax-play`).

## In the window

`Space` plays and stops. Click a row number to play from there. `Z S X D C V G B H N J M` and `Q 2 W 3 E R 5 T 6 Y 7 U I`
play the chosen machine like a piano; `[` and `]` change the octave. A MIDI controller plays it too
(`--midi n` picks the port; `wax-play --list-midi` lists them).
