# Twin filter (machine spec, not built yet)

Two multimode filters with series/parallel routing, a harmonic lock between them, and a drive stage; modulation is split into separate blocks.
Built for both apps with the same parameters and the same sound (the web app gets the same node; only the UI differs). Name and labels stay
clear of any product name. Full machine plus building blocks, as for the other synths.

## Signal path
input -> drive and tilt -> filter 1 -> (series - parallel morph) -> filter 2 -> level -> dry/wet -> out.
Noise and feedback join at the input.

## Parameters
| Section | Parameter | Range | Note |
|---|---|---|---|
| input | drive | 0 to 24 dB | saturation before the filters |
| input | tilt, tilt corner | -12 to +12 dB, 200 Hz to 8 kHz | the two switch positions (high boost / cut) as a real shelf |
| input | trigger | -60 to 0 dB | threshold for the envelopes; separate from drive |
| input | level | -24 to +12 dB | |
| filter 1, 2 | freq | 20 Hz to 20 kHz | 12 dB/oct state-variable |
| filter 1, 2 | resonance | 0 to 1 | stays stable when driven; up to self-oscillation |
| filter 1, 2 | low, band, high | -1 to 1 each | separate levels (the hardware has one morph knob); negative gives notch-like shapes |
| filter 1, 2 | env depth | -1 to 1 | |
| filter 1, 2 | env | env 1 or env 2 | each filter picks its envelope |
| routing | series - parallel | 0 to 1 | 0 = filter 1 into filter 2 (24 dB when tuned alike), 1 = side by side, between = mixed |
| routing | lock | free, ratio, follow | filter 2 free, or at a ratio to filter 1 |
| routing | ratio | 0.25 to 4 | a number, with the usual intervals (octave, fifth, ...) as snap points |
| routing | detune | -100 to +100 ct | |
| routing | track, transpose | off or on, -24 to +24 st | pitch follower tunes filter 2 (last to build) |
| modulation | env 1, env 2 | Bezier envelopes | the shared editor |
| modulation | follower attack, release, sens | 1 to 200 ms, 10 ms to 2 s, 0 to 1 | the follower is its own source |
| modulation | lfo shape, rate, to f1, to f2 | shapes, 0.02 to 40 Hz or tempo steps, -1 to 1 | free depth per filter |
| output | noise, feedback, source | 0 to 1, 0 to 1, out 1 or 2 | feedback taken from the chosen point |
| output | dry - wet, level | 0 to 1, -24 to +12 dB | |

## Order of work
1. Filter pair: the multimode filter and the series/parallel morph.
2. Harmonic lock with free ratios.
3. Modulation blocks: envelopes, follower, LFO.
4. Drive and tilt.
5. Pitch tracker.

## Panel
Sketch agreed first (flat, thin lines, text and dots, sliders; sections side by side in one row that scrolls sideways): input, filter 1,
routing, filter 2, modulation, output. A small response curve over each filter and over the routing; Bezier envelope pictures in modulation.
