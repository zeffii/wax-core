// Tests: the songs in tests/songs, each rendered and checked against what the web app made of it (the .ref.json beside it). The songs are
// grouped by what they exercise, so `wax-tests render-effects` runs just the effects. A new song is one more name in its list.
#include "tests/support.h"

#include <string>
#include <vector>

#include "engine/offline.h"
#include "project/project.h"

using tests::check;

namespace {

struct Family {
  const char* group;
  std::vector<const char*> songs;
};

const std::vector<Family> kFamilies = {
  {"render-songs", {"concrete", "nodes", "suite", "bugsong", "cloves", "curves"}},
  {"render-synths", {"oberheim", "fourpole", "bongo", "membrane", "cardinal-eco", "cardinal", "drumsynth", "pulsar", "pulsetrain", "jawharp", "jawharpsoft", "khufu", "khufuone", "snarl", "snarlglide", "snarlmod", "dvii", "dviilfo", "dviisolo", "dviigrand", "specsynth", "specsynthroot", "pianofelt", "pianostrike", "piano2", "padsynth", "compass", "compassonce", "waveseq", "waveseqonce", "vecmix", "vecmixonce"}},
  {"render-effects", {"effects", "effects2", "islands", "eqladdergate", "saturator", "circuit", "circuit2", "declick", "exciter-sweep", "ensemble", "reflect", "mbsat", "split", "headpan", "pan", "mbcomp", "combringtransient", "waveshaper", "amp", "ampclean", "rotary", "rotarystop", "groove", "groovesim", "groovewarp"}},
  {"render-spectral", {"mosh", "hpss", "hpssperc", "bodyblur", "bodyblurbig", "specdyn", "specdynbig", "xsynth", "xsynthbig"}},
  {"render-reverb", {"convolver", "convolverlong", "convolverwide"}},
  {"render-control", {"follower", "range", "rangeknob", "envfx", "notetrig", "ctrl", "ctrllog", "notepan", "notepancluster", "notepanrandom", "route", "add"}},
  {"render-genexpr", {"genexpr", "genexprfm", "genexprpluck", "genexprecho", "genexprcrush", "genexprstress", "genexprstressvoice", "genexprdefault", "genexprbad"}},
  {"render-samplers", {"sampler-repeats", "sampler-grains", "granular", "granularshort", "tap"}},
  {"render-channels", {"modchan", "modchan2", "modchannearest", "xmchan", "xmchanamiga", "itchan", "itchanamiga", "itchan3", "itchanproxy", "itchanrate", "itchantrack"}},
};

// The same song on one thread and on four: not one sample may differ.
void sameOnAnyThreads(const std::string& name) {
  const std::string songs = WAX_TEST_SONGS;
  const wax::Project project = wax::loadProject(songs + "/" + name + ".wax");
  wax::RenderOptions options;
  const wax::RenderResult single = wax::renderSong(project, options);
  options.threads = 4;
  const wax::RenderResult several = wax::renderSong(project, options);
  const bool same = single.audio.left == several.audio.left && single.audio.right == several.audio.right;
  check(same, name + ": four threads render the same samples as one");
  std::printf("ok: %s.wax is the same on 1 and 4 threads\n", name.c_str());
}

// Registered while the program starts, like the tests that use WAX_TEST.
const bool kRegistered = [] {
  for (const Family& family : kFamilies)
    for (const char* song : family.songs) tests::add(family.group, song, [song] { tests::renderSong(song); });
  for (const char* song : {"concrete", "nodes", "mosh", "suite", "effects2", "bugsong", "cloves"})
    tests::add("threads", song, [song] { sameOnAnyThreads(song); });
  return true;
}();

}  // namespace
