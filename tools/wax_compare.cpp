// wax-compare reference.wav native.wav [limit in dB, default -90]
// Says how far two renders are apart and exits with 0 when the largest difference is under the limit.
// The reference comes from webtracker (node tools/render-ref.mjs song.wax); the other one from this engine.
#include <cstdio>
#include <cstdlib>
#include <exception>

#include "io/wav.h"

int main(int argc, char** argv) {
  if (argc < 3) {
    std::printf("usage: wax-compare reference.wav native.wav [limit in dB, default -90]\n");
    return 1;
  }
  try {
    const double limitDb = argc > 3 ? std::atof(argv[3]) : -90.0;
    const wax::StereoAudio reference = wax::readWav(argv[1]);
    const wax::StereoAudio native = wax::readWav(argv[2]);
    if (reference.sampleRate != native.sampleRate) {
      std::printf("FAIL  the sample rates differ (%d and %d)\n", reference.sampleRate, native.sampleRate);
      return 3;
    }
    const wax::AudioDifference d = wax::compareAudio(reference, native);
    const bool pass = d.peakDb <= limitDb && d.lengthGap == 0;
    std::printf("%s  largest difference %.1f dB (at %.3f s), overall %.1f dB, limit %.1f dB\n",
                pass ? "PASS" : "FAIL", d.peakDb, static_cast<double>(d.worstFrame) / reference.sampleRate, d.rmsDb, limitDb);
    if (d.lengthGap) std::printf("      the lengths differ by %zu frames\n", d.lengthGap);
    return pass ? 0 : 3;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "wax-compare: %s\n", e.what());
    return 2;
  }
}
