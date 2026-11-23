# Tests

One program, `wax-tests`, no framework. Build it (`cmake --build build --target wax-tests`) and run it from the repo root.

```
wax-tests                 everything (a few minutes: most of it is song renders)
wax-tests --quick         everything except the song renders (about 2 seconds)
wax-tests --list          the groups and their tests, nothing run
wax-tests render-effects  one group
wax-tests piano           every test with "piano" in its group or name
wax-tests core graph      several at once
wax-tests --times         how long each test took, slowest last
wax-tests --stop          stop at the first test that fails
```

The old `--it`, `--xm` and `--samples` still work. The exit code is the number of failed checks; the last lines name the tests they were in, so ctest and CI go red the same way. A test that throws is reported and the run goes on.

## Groups

| group | file | what |
| --- | --- | --- |
| core | tests/core_tests.cpp | parameters, dice, WAV files, units, levels, the project file |
| document | tests/document_tests.cpp | editing a song: cells, patterns, blocks, loop, save, backups, sequence |
| curves | tests/curves_tests.cpp | splines, curve and drawing editing |
| graph | tests/graph_tests.cpp | editing an instrument: nodes, wires, exposure, presets, new machines |
| samples | tests/samples_tests.cpp | sample editing and loading, spectrogram, onsets, the live sampler, tap |
| import | tests/import_tests.cpp | opening .xm and .it files |
| nodes | tests/nodes_tests.cpp | things inside single nodes (the built-in rooms) and native-only additions |
| render-* | tests/render_tests.cpp | each song in tests/songs rendered and compared with what the web app made of it; grouped by what it exercises (synths, effects, spectral, reverb, control, genexpr, samplers, channels, songs) |
| threads | tests/render_tests.cpp | the same song on one thread and on four |

## Adding a test

A test registers itself; there is no list to edit.

```cpp
WAX_TEST("graph", testMyThing) {
  check(1 + 1 == 2, "what is being checked");
}
```

Put it in the file of its group (`using tests::check;` is at the top). The name shown is the function name in words: `testMyThing` is `graph/my-thing`. A new group is a new `tests/<name>_tests.cpp` and one line in the `add_executable` of `CMakeLists.txt`.

A new song: put `<name>.wax` and `<name>.ref.json` (made by the web app's render-ref) in tests/songs and add its name to the list of its family in `tests/render_tests.cpp`.

Shared pieces (`check`, `throws`, `renderSong`) are in tests/support.h.
