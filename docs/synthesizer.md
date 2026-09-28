# The synthesizer

Telic has a polyphonic phase-modulation synthesizer, a sequencer, and an
effects chain, all computed in C at 48 kHz in stereo. The same engine runs in
two modes. Offline, `render-audio` advances it by a number of seconds and
answers the output as an n×2 matrix, so a sound can be measured with the
Fourier words, compared, or written out; this mode runs on native and wasm.
Live, after `audio-on`, the engine runs on the output device's thread and the
words that change it take effect at the device's next buffer. The engine is in
one mode at a time: `render-audio` errors while the device is open.

This document describes the engine from the top down: the signal path, then
each stage in the order a sample passes through it, then the sequencer, the
effects, the live device, and how to design a patch. The reference tables
(docs/reference.md, "Audio synthesizer") list every word and key with its
range; this document explains what the numbers do. Every example below renders
offline and prints a measurement.

## The signal path

A sample is computed in this order:

1. The sequencer fires every scheduled note-on and note-off whose sample has
   come.
2. Each of the 32 voices that is sounding computes one sample: its
   oscillators, the phase-modulation matrix between them, the sum of its
   carriers, its unison copies, its ladder filter, and its pan.
3. The voices and every sample matrix given to `play-samples` are summed.
4. The sum passes through the effects chain: chorus, delay, reverb, master
   level.

Sixteen **instruments**, numbered 0–15, each hold a **patch**: a frame that
describes a sound. A note names an instrument; the voice that plays it copies
the instrument's patch when it starts, so a patch given later with
`instrument-patch!` changes only later notes. `instrument!` and `oscillator!`
change one parameter both in the patch and in every voice already sounding on
that instrument.

## Pitch

A pitch is a MIDI note number in [0, 127], fractional values allowed, or a
symbol: a letter a–g, then an optional `#` or `b`, then an optional `+` (a
quarter tone up) or `d` (a quarter tone down), then an octave from −1 to 9.
The note number of a symbol is 12 × (octave + 1) + the letter's semitone +
the accidentals, so `:c4` is 60 and `:a4` is 69. The frequency is
440 × 2^((n − 69)/12).

```forth pitch
:a4 pitch>hz . :c4 pitch>midi . :c+4 pitch>midi . :ebd4 pitch>midi . cr
```
```output
440 60 60.5 62.5
```

## Oscillators

A patch has one to eight oscillators, numbered from 0 in the order of its
`:oscillators` array. Each has a wave, a frequency, a level and an envelope.

- **Waves.** `:sine`, `:triangle`, `:saw`, `:pulse`, and three noises. The saw
  and the pulse are band-limited with PolyBLEP: a two-sample polynomial
  correction at each jump, which moves the strongest alias of a 5 kHz saw 36
  dB below the fundamental where a naive saw leaves it 17 dB below. `:width`
  sets the pulse's duty cycle; 0.5 is a square. `:white` is uniform noise
  from a 64-bit generator, `:pink` is white noise through three one-pole
  filters (about −3 dB per octave), and `:brown` is white noise through one
  leaky integrator (about −6 dB per octave). Each voice seeds its noise from
  the RNG at note-on, so `seed` makes a render repeatable.
- **Frequency.** An oscillator runs at the note's frequency × `:ratio`, bent
  by `:detune` cents. `:fixed-hz`, when above 0, replaces that with a fixed
  frequency that ignores the note, the ratio, glide, and the LFO's pitch.
- **Level.** `:level` scales the oscillator's output, whether the output is
  heard or modulates another oscillator.

An oscillator whose index is listed in `:carriers` is heard: the carriers'
outputs are summed into the voice. The default is `[ 0 ]`. Every other
oscillator is heard only through what it does to others.

## Phase modulation

`:modulation` is an array of `[ from to depth ]` routes. Each sample,
oscillator `from`'s output (after its level and envelope) × depth is added to
oscillator `to`'s phase, in radians. A route's depth is the modulation index
when the modulator runs at full level: a sine carrier at 1 kHz modulated by a
sine at 100 Hz with index β has sidebands at 1000 ± 100k Hz with amplitudes
|Jk(β)|, the Bessel functions of the first kind.

```forth phase-modulation
{ :oscillators [ { :fixed-hz 1000 } { :fixed-hz 100 } ] :modulation [ [ 1 0 1 ] ] } 0 instrument-patch!
silence-audio :a4 1 0 note-on 1 render-audio 4800 48000 0 1 submatrix to sidebands
[ 1000 1100 1200 ] ( sidebands 48000 rot amplitude-at 0.70710678 / 1000 * round 1000 / . ) each cr silence-audio
```
```output
0.765 0.44 0.115
```

J0(1) = 0.765, J1(1) = 0.440, J2(1) = 0.115. (The measurement divides by
0.7071 because a centered voice puts cos(π/4) of its signal on each side.)

The oscillators are computed from the highest index down, once per sample. A
route from a higher index to a lower one therefore carries the modulator's
output from this sample; any other route, including `[ i i d ]`, carries the
output from the previous sample. `[ i i d ]` is feedback: an oscillator
modulating its own phase turns a sine toward a saw as d rises. Routes stack:
a modulator can modulate another modulator, and several routes into one
target add.

```forth feedback
{ :oscillators [ { :fixed-hz 1000 } ] :modulation [ [ 0 0 1 ] ] } 0 instrument-patch!
silence-audio :a4 1 0 note-on 1 render-audio 4800 48000 0 1 submatrix to fed
[ 1000 2000 3000 ] ( fed 48000 rot amplitude-at 0.70710678 / 100 * round 100 / . ) each cr silence-audio
```
```output
0.85 0.32 0.17
```

Because an oscillator's level and envelope scale its output before it enters
a route, a modulator's envelope shapes the timbre over the note: a modulator
that decays faster than its carrier gives a bright onset that mellows; one that
rises more slowly than its carrier gives a tone that brightens after the onset,
the pattern of a brass instrument.

## Envelopes

Every oscillator has its own envelope, and the filter has one more. An
envelope has five stages:

1. **Attack.** A linear rise from the level the envelope is at to 1 in
   `:attack` seconds.
2. **Decay.** An exponential approach to `:sustain` with time constant
   `:decay`: after `:decay` seconds the level has covered 1 − 1/e (63%) of
   the distance.
3. **Sustain.** The level holds at `:sustain` while the note is held, or,
   when `:sustain-decay` is above 0, falls toward 0 with that time constant.
4. **Release.** From note-off, the level falls toward 0 with time constant
   `:release`, from wherever it had reached. A note released during its attack
   releases from the attack's level, not from 1.
5. **Idle.** The release ends when the level falls below 10⁻⁴ (−80 dB). A
   voice stops when all of its carriers' envelopes are idle.

A time constant τ means the level falls 8.7 dB each τ seconds, so a release of
0.3 s reaches −80 dB after 2.8 s. A note-on for a pitch already sounding on
the same instrument retriggers that voice: every envelope restarts its attack
from its current level, so a repeated note swells again without a click.

```forth envelope
{ :oscillators [ { :fixed-hz 1000 :attack 0.1 :decay 0.2 :sustain 0.5 } ] } 0 instrument-patch!
silence-audio :a4 1 0 note-on 1 render-audio to held
held 0.3 48000 * round dup 480 - swap 480 + 0 1 submatrix 48000 1000 amplitude-at 0.70710678 / 1000 * round 1000 / .
0.5 0.5 1 exp / + 1000 * round 1000 / . cr silence-audio
```
```output
0.684 0.684
```

At 0.3 s, one decay time constant after the attack's peak, the level is
0.5 + 0.5/e.

## Voices and voice allocation

A note takes one of 32 voices. A note-on first looks for a voice already
sounding the same pitch on the same instrument and retriggers it. Otherwise it
takes a free voice; when none is free, it takes the voice released longest
ago; when none is released, the voice started longest ago. A taken voice stops
at once and starts the new note.

## Unison

`:unison` n (1–7) plays n copies of every oscillator in each voice. The copies
are detuned evenly across `:unison-detune` cents — the whole span, so 20
places three copies at −10, 0 and +10 — and panned evenly across
±`:unison-spread` around the patch's `:pan`. The copies share the voice's
envelopes, and their sum is scaled by 1/√n, which keeps the level of n
uncorrelated copies near that of one.

```forth unison
{ :oscillators [ { :fixed-hz 1000 } ] :unison 3 :unison-detune 20 } 0 instrument-patch!
silence-audio :a4 1 0 note-on 2 render-audio to copies
[ -10 0 10 ] ( copies 0 96000 0 1 submatrix 48000 rot 1000 swap 1200 / 2 swap ^ * amplitude-at 0.70710678 / 1000 * round 1000 / . ) each cr silence-audio
```
```output
0.577 0.577 0.577
```

## The filter

A patch with `:cutoff` runs every voice through a four-pole transistor ladder
filter: D'Angelo and Välimäki's model of the Moog ladder, as ported in promini,
computed at twice the sample rate. Each of its four stages is a one-pole
low-pass with a tanh saturation, and `:resonance` feeds the fourth stage's
output back to the input. Above the cutoff the response falls at more than 24
dB per octave; `:resonance` near 4 rings at the cutoff, and `:drive` above 1
saturates the stages, adding odd harmonics. A voice whose copies are spread
in stereo runs two ladders, one per side. Without `:cutoff` there is no
filter.

```forth ladder
{ :oscillators [ { :wave :saw } ] } 0 instrument-patch! silence-audio 45 1 0 note-on 1 render-audio 9600 48000 0 1 submatrix to dry
{ :oscillators [ { :wave :saw } ] :cutoff 440 } 0 instrument-patch! silence-audio 45 1 0 note-on 1 render-audio 9600 48000 0 1 submatrix to wet
[ 880 1760 ] ( dup wet 48000 rot amplitude-at swap dry 48000 rot amplitude-at / log10 20 * round . ) each cr silence-audio
```
```output
-28 -65
```

The filter envelope moves the cutoff to `:cutoff` × 2^(amount × level), where
level is the filter envelope's level and `:amount` is in octaves, so a
positive amount opens the filter on each note and closes it as the envelope
decays.

## The LFO

Each voice has one low-frequency oscillator. Its value l in [−1, 1] follows
`:shape` (`:sine`, `:triangle`, `:saw`, `:square`, or `:sample-and-hold`,
which draws a new random value at each cycle) at `:rate` Hz. With `:key-sync`
1 (the default) it starts at phase 0 on each new voice; with 0 it follows a
clock that runs from the engine's first sample, so every voice is in the same
phase. It acts only after `:delay` seconds, then grows to full depth over
`:fade` seconds, which is how a vibrato that enters after the onset is made.
Its destinations are:

- pitch: ± `:pitch-depth` semitones;
- cutoff: ± `:cutoff-depth` octaves;
- pan: + `:pan-depth` × l;
- an oscillator's level, by that oscillator's `:lfo-level` d: the output ×
  (1 − d(1 − l)/2), a tremolo when the oscillator is a carrier and a moving
  modulation index when it is a modulator;
- a pulse's width, by that oscillator's `:lfo-width`: the width + d × l.

```forth vibrato
{ :oscillators [ { :fixed-hz 0 } ] :lfo { :shape :square :rate 0.5 :pitch-depth 1 } } 0 instrument-patch!
silence-audio :a4 1 0 note-on 2 render-audio to swung
swung 4800 43200 0 1 submatrix 48000 440 2 1 12 / ^ * amplitude-at 0.70710678 / 100 * round 100 / . cr silence-audio
```
```output
1
```

A square LFO holds +1 for its first half cycle, so the first second sounds a
semitone above A4.

## Glide

`:glide` seconds makes a new voice's pitch start at the instrument's previous
note and slide to its own, linearly in semitones. The first note after
`silence-audio` does not glide, and a retriggered voice keeps its pitch.

## Velocity

A note's velocity v in [0, 1] scales the voice's gain and its filter envelope's
amount by 1 − s + s × v, where s is the patch's `:velocity-sensitivity`: at 1
velocity scales fully, at 0 it is ignored.

## Parameter changes on sounding notes

`instrument!` changes `:level`, `:pan`, `:cutoff`, `:resonance` or `:drive`,
and `oscillator!` changes one oscillator's `:level`, `:ratio` or `:width`. Each
writes the value into the instrument's patch, for later notes, and moves it in
every sounding voice of that instrument linearly over 64 frames (1.3 ms), so a
change is heard at once without a step in the waveform. `:cutoff` on a voice
without a filter turns its filter on, ramping down from 20 kHz.

## The sequencer

The sequencer places notes at exact positions in time. `sequence-note ( pitch
length -- )`, `sequence-chord ( pitches length -- )` and `sequence-rest
( length -- )` act on the current instrument, set by `sequence-instrument`.
Each instrument has a position; a note or rest starts there, or at the present
when the position is past, and advances the position by its length. So a line
of notes on one instrument plays in order, and two instruments play at once.

A length is a fraction of a whole note, and a whole note lasts 240 / tempo
seconds (four beats at `sequence-tempo`). Lengths written as exact rationals —
`1/4`, `3/8`, `1/7` — stay exact: each position is kept as an exact fraction of
a sample (a whole note is 11,520,000,000 / (1000 × tempo) samples), so a sum of
lengths never drifts. A float length is converted with `float>exact`.

```forth exact-positions
silence-audio 120 sequence-tempo 0 sequence-instrument
: sevenths ( -- ) 0 7000 1 do k 1/7 sequence-rest loop ; sevenths
sequence-end . cr silence-audio
```
```output
2000
```

7000 sevenths of a whole note at 120 bpm end at exactly 2000 seconds, although
each seventh is 13714.2857… samples.

A note sounds for `sequence-articulation` × its length (7/8 by default; 1 is
legato) at `sequence-velocity`. Tempo, articulation, velocity and instrument
apply to the notes queued after they are set. The sequencer keeps a sorted
schedule of 8192 note-on and note-off events on the engine's side and fires
each at its exact sample during rendering; a word that would overflow the
schedule errors and queues nothing. Offline, queued notes sound during the
`render-audio` calls that reach them. Live, the present is 2048 samples (43
ms) ahead of the device's clock, which leaves the device time to receive the
events before their samples. `sequence-end` answers the seconds until the
latest instrument's position, and `wait-sequence` blocks until every queued
note has sounded and every voice is silent.

A score is written as data and queued phrase by phrase; to shape time (a
fermata, a ritardando), compute the performed length of each note from its
score position with exact arithmetic and queue that length. The chorale in
examples/befiehl-du-deine-wege.telic does both.

## The effects chain

The summed signal passes through three effects and a master level, in a fixed
order: chorus, delay, reverb, master. `effect! ( value key effect -- )` sets
one key. Each effect mixes dry × its input + wet × its processed signal; every
stage starts at `:wet 0 :dry 1`, and a stage at `:wet 0` is skipped, so an
untouched chain passes the signal unchanged, sample for sample. `:wet`, `:dry`
and `:level` move linearly over 64 frames.

- **Chorus.** A delay per side of `:delay` seconds swept by ± `:depth` at
  `:rate` Hz, the right side 90° behind the left, read with linear
  interpolation. Mixed with the dry signal, the swept copy thickens and widens
  a sound.
- **Delay.** A delay line of up to 2 s per side with `:feedback`. In
  `:ping-pong` mode the input's mono sum echoes on the left after `:time`, on
  the right after 2 × `:time`, and alternates, each pair × `:feedback`; in
  `:straight` mode each side echoes itself. A `:time` change moves the read
  position one sample per sample, which bends the pitch of echoes in flight
  instead of clicking.
- **Reverb.** Jon Dattorro's plate reverb (1997), ported from promini: a
  predelay, a one-pole input filter (`:bandwidth`), four input allpass
  diffusers, and a tank of two cross-coupled halves, each a modulated allpass,
  a delay, a damping filter (`:damping`), a gain (`:decay`), a second allpass
  and a second delay. It is true stereo: each input side has its own plate,
  and `:cross-feed` mixes each side into the other's. The output is the sum of
  fourteen taps across the tank, then `:width`, a 20 Hz DC blocker, and
  `:low-cut` and `:high-cut`. `:size` scales every delay. `:freeze 1` shuts
  the input and raises the tank's gain to 0.9999, holding the tail; the shimmer
  pitch-shifts the tail (`:shimmer1-shift`, `:shimmer2-shift`, in semitones)
  after the tank, or with `:shimmer-in-loop 1` shifts the tank's feedback so
  each pass climbs further.
- **Master.** `:level` scales the output.

```forth straight-delay
{ :oscillators [ { :fixed-hz 1000 :attack 0 :release 0 } ] :pan -1 } 0 instrument-patch! silence-audio
0.1 :time :delay effect! 0.5 :feedback :delay effect! 1 :wet :delay effect! :straight :mode :delay effect!
:a4 1 0 note-on 0.01 render-audio drop :a4 0 note-off 0.4 render-audio to echoes
[ 0.09 0.19 0.29 ] ( echoes swap 48000 * round dup 480 + 0 1 submatrix 48000 1000 amplitude-at 1000 * round 1000 / . ) each cr
0 :wet :delay effect! :ping-pong :mode :delay effect! 0.375 :time :delay effect! 0.01 render-audio drop silence-audio
```
```output
1 0.5 0.25
```

A 10 ms burst on the left echoes on the left at 0.1, 0.2 and 0.3 s, halving
each time. `silence-audio` empties the delay and reverb buffers along with the
voices; `wait-audio` and `wait-sequence` do not wait for an effect's tail.

## The live device

`audio-on` opens the default output device with miniaudio (48 kHz, stereo,
32-bit float) and runs the engine in the device's callback; `audio-off`
closes it. The callback runs on a thread the interpreter does not control, and
it never takes a lock, allocates, frees, or reads a telic value. The
interpreter reaches it three ways:

- a single-producer, single-consumer ring of 4096 commands (note-on, note-off,
  parameter changes, effect changes, sequencer events, samples to play),
  drained at the start of each device buffer; a word that would overflow it
  errors;
- one pending-patch slot per instrument behind an atomic flag, so
  `instrument-patch!` never copies a patch while the device reads it;
- a second ring on which the device returns the sample buffers of finished
  `play-samples` calls for the interpreter to free.

Every frame the engine renders, live or offline, is also written after the
effects chain into a ring of the last 16384 frames, and the count of frames
written is published when each render finishes. `recent-audio ( frames --
matrix )` copies up to 8192 of the newest into an n×2 matrix, so a program can
watch what is playing without touching the device thread: a
`screen-frame` loop that takes 4096 frames each display frame, windows them
with `hann`, and draws `fft magnitudes` as bars is a spectrum analyzer, as the
chorale example does.

`play-samples` copies an n×1 or n×2 matrix to floats and returns at once;
several can overlap. `wait-audio` blocks until every `play-samples` has ended
and every voice is silent, which is when the last samples have been handed to
the device; the device's own output latency follows. Both waits return at
once when the device is off, and Ctrl-C interrupts them. Offline, every
command applies immediately, which is why `render-audio` and the live device
give the same samples for the same sequence of words.

## Measuring a sound

Offline rendering makes every claim about a sound testable. `amplitude-at
( v rate hz -- a )` is the amplitude of the sinusoid at exactly hz in a signal
sampled at rate, from a Hann-windowed discrete Fourier sum; `amplitude-spectrum`
gives the whole spectrum as a dataset; `fft`, `ifft`, `cfft`, `icfft`,
`magnitudes` and `hann` are the transforms underneath. The examples in this
document are measurements of this kind, and so is the synthesizer's test suite
(tests/12*_audio_*, tests/13*_audio_*), which checks harmonic series, Bessel
sidebands, envelope levels, filter slopes, echo positions and reverb decay.

```forth spectrum-peak
{ :oscillators [ { :wave :saw :fixed-hz 1000 } ] } 0 instrument-patch!
silence-audio :a4 1 0 note-on 1 render-audio 4800 48000 0 1 submatrix 48000 amplitude-spectrum to bins
bins :amplitude @ argmax bins :hz @ swap @e . cr silence-audio
```
```output
1000
```

## Designing a patch

A patch is data, so a sound is built by editing a frame and listening, or by
measuring. Some patterns:

- **A smooth, sustained tone.** Carrier attacks of 50–200 ms, a sustain of
  0.8–0.9 with a `:sustain-decay` of many seconds so a long note fades
  slightly, and a release of 0.3–0.6 s, longer than the gap articulation
  leaves between notes, so each note overlaps the next. Short, percussive
  stages belong on modulators, where they shape the onset's color rather than
  its loudness.
- **Movement.** A modulator a few cents off an integer ratio (3.004 rather
  than 3) makes the spectrum beat slowly; a second unison copy detuned by 5–8
  cents does the same across the stereo field.
- **Air.** A pink-noise carrier at a level near 0.03 with a fast decay adds a
  breath at the onset.
- **Brass-like brightening.** A modulator at ratio 1 whose attack is slower
  than its carrier's, at an index of 1.5–2.
- **A soft saw.** A sine with self-feedback `[ 0 0 d ]`, d near 1, is a saw
  with fewer high harmonics than the band-limited `:saw`.

The four voices of examples/befiehl-du-deine-wege.telic use these patterns,
each voice with a different combination.
