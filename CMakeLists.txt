cmake_minimum_required(VERSION 3.21)
project(wax-core VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive-)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)   # the standard sscanf is fine here
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
endif()

# The only outside code so far: a JSON reader for .wax files (header only, fetched once at configure time).
include(FetchContent)
FetchContent_Declare(json
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  URL_HASH SHA256=d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d)
FetchContent_MakeAvailable(json)

# The engine: everything that makes sound, with no window and no sound card.
add_library(waxcore
  third_party/v8-ieee754/ieee754.cc
  src/core/params.cpp
  src/core/node_registry.cpp
  src/core/random.cpp
  src/project/project.cpp
  src/project/master.cpp
  src/graph/compile.cpp
  src/seq/cells.cpp
  src/seq/chordname.cpp
  src/seq/columns.cpp
  src/seq/names.cpp
  src/app/column_info.cpp
  src/seq/automation.cpp src/seq/curve_edit.cpp
  src/seq/lane.cpp
  src/engine/runtime.cpp
  src/engine/task_pool.cpp
  src/engine/sleep.cpp
  src/engine/limiter.cpp
  src/engine/sequencer.cpp
  src/engine/engine.cpp
  src/engine/offline.cpp
  src/io/wav.cpp
  src/nodeseq/nodeseq.cpp
  src/nodeseq/seq_parse.cpp
  src/nodeseq/types.cpp
  src/nodeseq/types_time.cpp
  src/nodeseq/types_values.cpp
  src/nodeseq/types_chords.cpp
  src/nodeseq/types_output.cpp
  src/nodeseq/types_logic.cpp
  src/nodes/all_nodes.cpp
  src/nodes/waves.cpp
  src/nodes/plumbing.cpp
  src/nodes/out.cpp
  src/nodes/busin.cpp
  src/nodes/tempo.cpp
  src/nodes/voices.cpp
  src/nodes/canyon.cpp
  src/nodes/chord.cpp
  src/nodes/chorus.cpp
  src/nodes/clap.cpp
  src/nodes/combperc.cpp
  src/nodes/control_nodes.cpp
  src/nodes/delay.cpp
  src/nodes/duette.cpp
  src/nodes/env.cpp
  src/nodes/fft.cpp
  src/nodes/gain.cpp
  src/nodes/hall.cpp
  src/nodes/hat808.cpp
  src/nodes/lfo.cpp
  src/nodes/filter.cpp
  src/nodes/noise.cpp
  src/nodes/crusher.cpp
  src/nodes/gater.cpp
  src/nodes/compressor.cpp
  src/nodes/flanger.cpp
  src/nodes/filterpro.cpp
  src/nodes/twinfilter.cpp
  src/nodes/oberheim.cpp
  src/nodes/fourpole.cpp
  src/nodes/sampler.cpp
  src/nodes/glsl.cpp
  src/project/samples.cpp src/project/sample_edit.cpp src/project/spectrogram.cpp src/project/onsets.cpp src/project/backups.cpp
  src/nodes/mix.cpp
  src/nodes/dcblock.cpp
  src/nodes/declick.cpp
  src/nodes/distortion.cpp
  src/nodes/multitap.cpp
  src/nodes/freqshift.cpp
  src/nodes/phaser.cpp
  src/nodes/shimmer.cpp
  src/nodes/fmsynth.cpp
  src/nodes/piano.cpp
  src/nodes/wave_table_data.cpp
  src/nodes/tape.cpp
  src/nodes/circuit.cpp
  src/nodes/string.cpp
  src/nodes/wavetable.cpp
  src/nodes/addsynth.cpp
  src/nodes/padcore.cpp
  src/nodes/padsynth.cpp
  src/nodes/spreadosc.cpp
  src/nodes/arp.cpp
  src/nodes/specpan.cpp
  src/nodes/blur.cpp
  src/nodes/bezier.cpp
  src/seq/loop.cpp
  src/app/units.cpp
  src/app/machines.cpp
  src/app/module_util.cpp
  src/app/xm_import.cpp
  src/app/it_import.cpp
  src/nodes/env2.cpp
  src/nodes/haas.cpp
  src/nodes/hat2.cpp
  src/nodes/drumsynth2.cpp
  src/nodes/clang.cpp
  src/nodes/urugraverb.cpp
  src/nodes/spacecore.cpp
  src/nodes/sympathy.cpp
  src/nodes/kalcore.cpp
  src/nodes/kaleidoscope.cpp
  src/nodes/pogue.cpp
  src/nodes/lone.cpp
  src/nodes/islands.cpp
  src/nodes/gate.cpp
  src/nodes/ladder.cpp
  src/nodes/eq.cpp
  src/nodes/pulsar.cpp
  src/nodes/ensemble.cpp
  src/nodes/reflect.cpp
  src/nodes/split.cpp
  src/nodes/mbsat.cpp
  src/nodes/pan.cpp
  src/nodes/headpan.cpp
  src/nodes/follower.cpp
  src/nodes/range.cpp
  src/nodes/envfx.cpp
  src/nodes/ctrl.cpp
  src/nodes/notepan.cpp
  src/nodes/hpss.cpp
  src/nodes/bodyblur.cpp
  src/nodes/mbcomp.cpp
  src/nodes/specdyn.cpp
  src/nodes/xsynth.cpp
  src/nodes/rotary.cpp
  src/nodes/jawharp.cpp
  src/nodes/khufu.cpp
  src/nodes/granular.cpp
  src/nodes/groove.cpp
  src/nodes/snarl.cpp
  src/nodes/dvii.cpp
  src/nodes/specsynth.cpp
  src/nodes/modchan.cpp
  src/nodes/xmchan.cpp
  src/nodes/itchan.cpp
  src/nodes/waveseq.cpp
  src/nodes/vecmix.cpp
  src/nodes/vecpath.cpp
  src/nodes/compass.cpp
  src/nodes/convolver.cpp
  src/nodes/add.cpp
  src/nodes/route.cpp
  src/nodes/amp.cpp
  src/nodes/genexpr.cpp
  src/nodes/tap.cpp
  src/engine/takes.cpp
  src/edit/document.cpp
  src/edit/presets.cpp
  src/project/save.cpp
  src/edit/spline_ops.cpp
  src/nodes/drawings.cpp
  src/edit/sync.cpp
  src/edit/cell_editor.cpp
  src/edit/sequence_ops.cpp
  src/nodes/sweep.cpp
  src/nodes/exciter.cpp
  src/nodes/cardinal.cpp
  src/nodes/membrane.cpp
  src/nodes/bongo.cpp
  src/nodes/drumsynth.cpp
  src/nodes/saturator.cpp
  src/nodes/waveshaper.cpp
  src/nodes/transient.cpp
  src/nodes/ringmod.cpp
  src/nodes/comb.cpp
  src/nodes/shiva.cpp
  src/nodes/woodblock.cpp
  src/nodes/mosh.cpp
  src/nodes/osc.cpp
  src/nodes/plate.cpp
  src/nodes/pluckgate.cpp
  src/nodes/sidechain.cpp
  src/nodes/squelch.cpp
  src/nodes/stft.cpp
  src/nodes/subsynth.cpp
  src/nodes/vocoder.cpp
  src/nodes/wavefolder.cpp
  src/nodes/westcoast.cpp)
target_include_directories(waxcore PUBLIC src third_party)
# Results must be the same on every machine: no fused multiply-add, no fast-math.
if(NOT MSVC)
  target_compile_options(waxcore PUBLIC -ffp-contract=off)
endif()
target_link_libraries(waxcore PUBLIC nlohmann_json::nlohmann_json)

# Command-line tools.
add_executable(wax-info tools/wax_info.cpp)
target_link_libraries(wax-info PRIVATE waxcore)

add_executable(wax-render tools/wax_render.cpp)
target_link_libraries(wax-render PRIVATE waxcore)

add_executable(wax-compare tools/wax_compare.cpp)
target_link_libraries(wax-compare PRIVATE waxcore)

# Tests: one plain program, run by `ctest`.
enable_testing()
add_executable(wax-tests tests/tests.cpp)
target_link_libraries(wax-tests PRIVATE waxcore)
target_compile_definitions(wax-tests PRIVATE WAX_TEST_SONGS="${CMAKE_SOURCE_DIR}/tests/songs")
add_test(NAME wax-tests COMMAND wax-tests)

# The live player: sound through miniaudio, MIDI through RtMidi (both vendored in third_party). Off with -DWAX_LIVE=OFF.
option(WAX_LIVE "Build wax-play, the live player" ON)
if(WAX_LIVE)
  find_package(Threads REQUIRED)
  add_library(waxlive STATIC tools/miniaudio_impl.cpp third_party/rtmidi/RtMidi.cpp)
  target_include_directories(waxlive PUBLIC third_party third_party/rtmidi)
  target_link_libraries(waxlive PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  if(WIN32)
    target_compile_definitions(waxlive PUBLIC __WINDOWS_MM__)
    target_link_libraries(waxlive PUBLIC winmm)
  elseif(APPLE)
    target_compile_definitions(waxlive PUBLIC __MACOSX_CORE__)
    target_link_libraries(waxlive PUBLIC "-framework CoreMIDI" "-framework CoreAudio" "-framework CoreFoundation")
  else()
    find_package(ALSA)
    if(ALSA_FOUND)
      target_compile_definitions(waxlive PUBLIC __LINUX_ALSA__)
      target_link_libraries(waxlive PUBLIC ALSA::ALSA)
    else()
      target_compile_definitions(waxlive PUBLIC __RTMIDI_DUMMY__)
    endif()
    target_link_libraries(waxlive PUBLIC m)
  endif()
  # Other people's code: no warnings from it.
  set_source_files_properties(tools/miniaudio_impl.cpp third_party/rtmidi/RtMidi.cpp PROPERTIES COMPILE_OPTIONS
    "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
  target_sources(waxlive PRIVATE src/live/audio_out.cpp)
  target_link_libraries(waxlive PUBLIC waxcore)
  add_executable(wax-play tools/wax_play.cpp)
  target_link_libraries(wax-play PRIVATE waxcore waxlive)

  # The window: SDL3 (fetched at configure time, built static, video only) and Dear ImGui (vendored). Off with -DWAX_GUI=OFF.
  option(WAX_GUI "Build wax-app, the window" ON)
  if(WAX_GUI)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    set(SDL_AUDIO OFF CACHE BOOL "" FORCE)        # sound is miniaudio's
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_JOYSTICK OFF CACHE BOOL "" FORCE)
    set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
    set(SDL_HIDAPI OFF CACHE BOOL "" FORCE)
    set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
    set(SDL_POWER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    if(UNIX AND NOT APPLE)
      # SDL3 needs the X11 development packages to open a real window. Without them (a CI machine, a server) it is
      # built with its dummy video driver: the app runs and draws, nowhere, which is enough to test it.
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
        pkg_check_modules(WAX_XCURSOR QUIET xcursor xext xi xrandr xfixes xscrnsaver xkbcommon)
      endif()
      if(NOT WAX_XCURSOR_FOUND)
        message(STATUS "X11 development files not found: wax-app is built with SDL's dummy video driver only")
        set(SDL_X11 OFF CACHE BOOL "" FORCE)
        set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
        set(SDL_KMSDRM OFF CACHE BOOL "" FORCE)
        set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
      endif()
    endif()
    FetchContent_Declare(sdl3 GIT_REPOSITORY https://github.com/libsdl-org/SDL.git GIT_TAG release-3.4.8 GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(sdl3)
    # Text is drawn through FreeType (hinted, so it stays sharp); without it ImGui falls back to its own rasteriser, which is softer.
    find_package(Freetype QUIET)
    if(FREETYPE_FOUND)
      set(WAX_FREETYPE Freetype::Freetype)
    else()   # none installed (a plain Windows setup): build it here, with none of its optional libraries
      foreach(off ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${off} ON CACHE BOOL "" FORCE)
      endforeach()
      FetchContent_Declare(freetype GIT_REPOSITORY https://github.com/freetype/freetype.git GIT_TAG VER-2-13-3 GIT_SHALLOW TRUE)
      FetchContent_MakeAvailable(freetype)
      set(WAX_FREETYPE freetype)
      target_compile_options(freetype PRIVATE "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")   # (its own warnings are not ours to fix)
    endif()
    add_library(waximgui STATIC
      third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_impl_sdl3.cpp third_party/imgui/imgui_impl_sdlrenderer3.cpp)
    target_include_directories(waximgui PUBLIC third_party/imgui third_party)
    target_link_libraries(waximgui PUBLIC SDL3::SDL3)
    target_sources(waximgui PRIVATE third_party/imgui/misc/freetype/imgui_freetype.cpp)
    target_compile_definitions(waximgui PUBLIC IMGUI_ENABLE_FREETYPE)
    target_link_libraries(waximgui PUBLIC ${WAX_FREETYPE})
    set_source_files_properties(third_party/imgui/misc/freetype/imgui_freetype.cpp PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
    set_source_files_properties(
      third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
      third_party/imgui/imgui_widgets.cpp third_party/imgui/imgui_impl_sdl3.cpp third_party/imgui/imgui_impl_sdlrenderer3.cpp
      PROPERTIES COMPILE_OPTIONS "$<IF:$<CXX_COMPILER_ID:MSVC>,/W0,-w>")
    add_executable(wax-app tools/wax_app.cpp tools/wax_app/header.cpp tools/wax_app/keyboard.cpp tools/wax_app/browser.cpp tools/wax_app/song_screen.cpp tools/wax_app/pattern_screen.cpp tools/wax_app/instrument_screen.cpp tools/wax_app/info_screen.cpp tools/wax_app/config_screen.cpp src/app/theme.cpp src/app/fonts.cpp src/app/pattern_view.cpp src/app/widgets.cpp src/app/param_panel.cpp src/app/sequence_view.cpp src/app/minimap.cpp src/app/vector_window.cpp src/app/genexpr_window.cpp src/app/song_keys.cpp src/app/mixer_view.cpp src/app/envelope_editor.cpp src/app/envelope_window.cpp
  src/app/dx_window.cpp src/app/gate_window.cpp src/app/chord_window.cpp src/app/wavetable_window.cpp src/app/shape_window.cpp
  src/app/graph_view.cpp src/app/graph_toolbar.cpp src/app/toolbar.cpp src/app/pattern_toolbar.cpp src/app/sequence_toolbar.cpp
  src/app/node_look.cpp src/app/node_trail.cpp src/app/shortcut_sheet.cpp src/app/pattern_list.cpp src/app/curve_view.cpp src/app/sample_browser.cpp src/app/sample_view.cpp src/app/pattern_colour.cpp src/app/config.cpp src/app/file_association.cpp src/app/insert_menu.cpp src/app/machine_tree.cpp src/app/icons.cpp src/app/node_help.cpp src/app/node_envelope.cpp src/app/node_pictures.cpp src/app/node_response.cpp src/app/node_strip.cpp src/app/pogue_picture.cpp src/app/offline_render.cpp src/app/cardinal_picture.cpp src/app/space_pictures.cpp)
    target_include_directories(wax-app PRIVATE third_party/imgui third_party tools)
    target_compile_definitions(wax-app PRIVATE WAX_ICON_DIR="${CMAKE_SOURCE_DIR}/art/icons")
    target_link_libraries(wax-app PRIVATE waxcore waxlive waximgui)
    if(WIN32)
      # The program icon in Explorer and the taskbar (art/wax.ico, made by art/make_ico.py).
      enable_language(RC)
      target_sources(wax-app PRIVATE art/wax.rc)
    endif()
    if(MSVC)
      # No console window behind the app, but keep main().
      set_target_properties(wax-app PROPERTIES WIN32_EXECUTABLE ON)
      target_link_options(wax-app PRIVATE /ENTRY:mainCRTStartup)
    endif()
  endif()
endif()
