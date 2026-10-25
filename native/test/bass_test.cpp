#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers the synthesized Bass (instruments/bass.h) through the Engine's instrument API,
// including the engine-side timed note-off. Set BASS_WAV_DIR=<dir> to dump cases as 16-bit WAVs
// for listening (see docs/bass-instrument.md).
using namespace webdsp;

static long g_allocs = 0;
void* operator new(std::size_t n) {
  g_allocs++;
  if (void* p = std::malloc(n ? n : 1)) return p;
  throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kBass = static_cast<int32_t>(InstrumentKind::Bass);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
int32_t id(BassParam p) { return static_cast<int32_t>(p); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

struct Setup {
  // -1 = leave at default.
  float tune = -1, body = -1, bite = -1, cutoff = -1, res = -1, attack = -1, decay = -1,
        sustain = -1, release = -1;
  float velocity = 1.0f, pitch = 0.0f;
  double duration = -1;  // seconds; < 0 = held
};

void apply(Engine& e, const Setup& s) {
  auto set = [&](BassParam p, float v) { if (v >= 0) e.setInstrumentParam(1, id(p), v); };
  set(BassParam::Tune, s.tune); set(BassParam::Body, s.body); set(BassParam::Bite, s.bite);
  set(BassParam::Cutoff, s.cutoff); set(BassParam::Resonance, s.res);
  set(BassParam::Attack, s.attack); set(BassParam::Decay, s.decay);
  set(BassParam::Sustain, s.sustain); set(BassParam::Release, s.release);
}

void run(Engine& e, int64_t& frame, double seconds, Buf* l) {
  const int32_t frames = static_cast<int32_t>(seconds * kRate / kBlock) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    l->insert(l->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

void writeWav(const char* name, const Buf& b) {
  const char* dir = std::getenv("BASS_WAV_DIR");
  if (!dir) return;
  std::string path = std::string(dir) + "/" + name + ".wav";
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return;
  const uint32_t n = static_cast<uint32_t>(b.size()), bytes = n * 2, rate = 48000;
  const uint16_t pcm = 1, ch = 1, bits = 16, align = 2;
  const uint32_t riff = 36 + bytes, fmt = 16, byteRate = rate * 2;
  std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
  std::fwrite(&fmt, 4, 1, f); std::fwrite(&pcm, 2, 1, f); std::fwrite(&ch, 2, 1, f);
  std::fwrite(&rate, 4, 1, f); std::fwrite(&byteRate, 4, 1, f); std::fwrite(&align, 2, 1, f);
  std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&bytes, 4, 1, f);
  for (uint32_t i = 0; i < n; i++) {
    const int16_t s = static_cast<int16_t>(std::fmax(-1.f, std::fmin(1.f, b[i])) * 32767);
    std::fwrite(&s, 2, 1, f);
  }
  std::fclose(f);
}

// One note on instrument 1 (master bus), `seconds` rendered. A duration >= 0 uses the timed
// note-off; otherwise the note is held for the whole render.
Buf play(const Setup& s, double seconds, const char* wav = nullptr) {
  Engine e;
  e.init(kRate, 2, 8);
  e.createInstrument(1, kBass, 0);
  apply(e, s);
  const int32_t dur = s.duration < 0 ? -1 : static_cast<int32_t>(std::lround(s.duration * kRate));
  e.instrumentNoteOn(1, s.velocity, 0.0f, s.pitch, dur);
  int64_t f = 0;
  Buf out;
  run(e, f, seconds, &out);
  if (wav) writeWav(wav, out);
  return out;
}

float peak(const Buf& v, size_t a = 0, size_t b = SIZE_MAX) {
  float p = 0;
  for (size_t i = a; i < v.size() && i < b; i++) p = std::fmax(p, std::fabs(v[i]));
  return p;
}
double rms(const Buf& v, size_t a, size_t b) {
  double s = 0;
  b = std::min(b, v.size());
  for (size_t i = a; i < b; i++) s += double(v[i]) * v[i];
  return std::sqrt(s / double(b - a));
}
// Magnitude of the DFT at `hz` over [a, b) with a Hann window.
double bin(const Buf& v, size_t a, size_t b, double hz) {
  double re = 0, im = 0;
  b = std::min(b, v.size());
  const double n = double(b - a);
  for (size_t i = a; i < b; i++) {
    const double w = 0.5 - 0.5 * std::cos(2 * kPi * double(i - a) / n);
    const double ph = 2 * kPi * hz * double(i) / kRate;
    re += w * v[i] * std::cos(ph);
    im -= w * v[i] * std::sin(ph);
  }
  return std::sqrt(re * re + im * im) * 2.0 / n;
}
// Spectral centroid over harmonics of f0 (1..40), amplitude weighted.
double centroid(const Buf& v, size_t a, size_t b, double f0) {
  double num = 0, den = 0;
  for (int h = 1; h <= 40; h++) {
    const double m = bin(v, a, b, f0 * h);
    num += f0 * h * m;
    den += m;
  }
  return den > 0 ? num / den : 0;
}
// Fundamental: strongest DFT bin within +-8% of `nominal` (0.1 Hz steps; the sub at f/2 and
// the harmonics at 2f are outside the window).
double peakFreq(const Buf& v, size_t a, size_t b, double nominal) {
  double best = 0, bestHz = 0;
  for (double hz = nominal * 0.92; hz <= nominal * 1.08; hz += 0.1) {
    const double m = bin(v, a, b, hz);
    if (m > best) { best = m; bestHz = hz; }
  }
  return bestHz;
}
bool finiteAll(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}

// A clean reference voice: pure sine core, no drive, filter wide open.
Setup clean() {
  Setup s;
  s.body = 0; s.bite = 0; s.cutoff = 4000; s.res = 0; s.attack = 0.001f;
  s.decay = 0.03f; s.sustain = 1; s.release = 0.05f;
  return s;
}

// --- tests -----------------------------------------------------------------------------------

void testProducesFiniteBoundedAudio() {
  Buf o = play({}, 1.0, "default");
  CHECK(peak(o) > 0.2f);
  CHECK(peak(o) <= 1.0f);
  CHECK(finiteAll(o));
  Setup hot; hot.body = 1; hot.bite = 1; hot.res = 1; hot.cutoff = 4000; hot.velocity = 4.0f;
  hot.tune = 110; hot.sustain = 1;
  Buf h = play(hot, 1.0, "extreme");
  CHECK(peak(h) <= 1.0f);
  CHECK(finiteAll(h));
}

void testDeterministic() {
  Buf a = play({}, 0.5), b = play({}, 0.5);
  CHECK(a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

void testPitchAccuracy() {
  for (double tune : {41.2, 55.0, 110.0}) {
    Setup s = clean(); s.tune = float(tune); s.body = 0;
    Buf o = play(s, 0.6);
    CHECK_NEAR(peakFreq(o, at(0.1), at(0.55), tune) / tune, 1.0, 0.003);
  }
  Setup s = clean(); s.pitch = 12;  // +12 semitones on tune
  Buf o = play(s, 0.6);
  CHECK_NEAR(peakFreq(o, at(0.1), at(0.55), 110.0) / 110.0, 1.0, 0.003);
  Setup r = clean(); r.body = 1;  // saw core keeps the same period
  CHECK_NEAR(peakFreq(play(r, 0.6), at(0.1), at(0.55), 55.0) / 55.0, 1.0, 0.003);
}

void testEnvelopeTiming() {
  Setup s = clean();
  s.attack = 0.05f; s.decay = 0.2f; s.sustain = 0.5f; s.release = 0.2f; s.duration = 1.0;
  Buf o = play(s, 1.6);
  // (peak-based: sine + sub beat over two cycles, which makes short-window RMS ripple.)
  const double pk = peak(o, at(0.04), at(0.12));       // top of the attack
  const double sus = peak(o, at(0.6), at(0.9));        // held
  CHECK_NEAR(sus / pk, 0.5, 0.05);                       // public sustain is the real level
  CHECK(peak(o, at(0.005), at(0.015)) < pk * 0.3);       // still climbing during attack
  CHECK(rms(o, at(1.3), at(1.6)) < 1e-4);               // released and silent
  // The release starts exactly at the duration frame, not a block later.
  const size_t off = at(1.0);
  CHECK(peak(o, off - 1500, off) > sus * 0.8);
  CHECK(peak(o, off + at(0.18), off + at(0.2)) < sus * 0.2);
}

void testFilterClosesAndTracksKey() {
  Setup open = clean(); open.body = 1; open.tune = 55; open.cutoff = 4000;
  Setup shut = open; shut.cutoff = 150;
  Buf a = play(open, 0.5), b = play(shut, 0.5);
  CHECK(centroid(a, at(0.2), at(0.4), 55) > centroid(b, at(0.2), at(0.4), 55) * 2.0);
  // Slope: a harmonic two octaves above the cutoff is ~48 dB down on a 24 dB filter, relative
  // to the unfiltered level at that harmonic. Check a generous >30 dB.
  const double h8 = bin(b, at(0.2), at(0.4), 55 * 8) / bin(a, at(0.2), at(0.4), 55 * 8);
  CHECK(h8 < 0.03);
  // Keytracking: +12 st moves the effective cutoff by ~sqrt(2), so brightness *relative to the
  // note's own harmonics* stays near-constant (centroid / f0 within 40%).
  Setup hi = shut; hi.pitch = 12;
  const double rel0 = centroid(b, at(0.2), at(0.4), 55) / 55;
  Buf c = play(hi, 0.5);
  const double rel1 = centroid(c, at(0.2), at(0.4), 110) / 110;
  CHECK(rel1 > rel0 * 0.5 && rel1 < rel0 * 1.5);
}

void testResonanceStableAndBounded() {
  Setup s; s.body = 1; s.bite = 1; s.res = 1; s.cutoff = 40; s.sustain = 1;
  CHECK(peak(play(s, 0.5)) <= 1.0f);
  // Fuzz: extreme combinations never produce NaN/Inf or exceed full scale.
  const float vals[] = {0.0f, 0.5f, 1.0f};
  for (float b : vals) for (float r : vals) for (float bt : vals) for (float c : {40.0f, 4000.0f}) {
    Setup f; f.body = b; f.res = r; f.bite = bt; f.cutoff = c; f.velocity = 2.0f;
    Buf o = play(f, 0.2);
    CHECK(finiteAll(o));
    CHECK(peak(o) <= 1.0f);
  }
  // Live parameter slamming mid-note.
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.instrumentNoteOn(1, 1.0f);
  int64_t fr = 0; Buf o;
  for (int i = 0; i < 100; i++) {
    e.setInstrumentParam(1, id(BassParam::Cutoff), (i & 1) ? 40.0f : 4000.0f);
    e.setInstrumentParam(1, id(BassParam::Resonance), (i & 2) ? 1.0f : 0.0f);
    run(e, fr, kBlock / kRate, &o);
  }
  CHECK(finiteAll(o) && peak(o) <= 1.0f);
}

void testParamValidation() {
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.setInstrumentParam(1, id(BassParam::Cutoff), 1e9f);   // clamped
  e.setInstrumentParam(1, id(BassParam::Tune), NAN);       // ignored
  e.setInstrumentParam(1, 999, 1.0f);                      // unknown id: no-op
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o; run(e, f, 0.2, &o);
  CHECK(finiteAll(o) && peak(o) > 0.05f);
  CHECK(instrumentParamCount(kBass) == 11 + 1);  // + the shared source gain
  for (int32_t i = 0; i < 11; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kBass, i);
    CHECK(p && p->id == i && p->min <= p->def && p->def <= p->max);
  }
  CHECK(std::strcmp(instrumentKindMeta(kBass)->id, "bass") == 0);
}

void testVelocity() {
  // bite 0: velocity is a pure level (spectrum identical up to scale).
  Setup q = clean(); q.body = 1; q.cutoff = 600;
  Setup l = q; l.velocity = 0.25f;
  Buf a = play(q, 0.5), b = play(l, 0.5);
  CHECK_NEAR(rms(b, at(0.1), at(0.4)) / rms(a, at(0.1), at(0.4)), 0.25, 0.02);
  CHECK_NEAR(centroid(b, at(0.1), at(0.4), 55) / centroid(a, at(0.1), at(0.4), 55), 1.0, 0.03);
  // bite 1: harder playing is brighter at equal normalised level, and not a scaled copy.
  Setup t = q; t.bite = 1; t.cutoff = 300; t.sustain = 1; t.decay = 0.5f;
  Setup soft = t; soft.velocity = 0.3f;
  Buf hard = play(t, 0.5), sf = play(soft, 0.5);
  CHECK(centroid(hard, at(0.01), at(0.12), 55) > centroid(sf, at(0.01), at(0.12), 55) * 1.15);
  CHECK(rms(hard, at(0.1), at(0.4)) > rms(sf, at(0.1), at(0.4)));
}

void testRetriggerIsContinuous() {
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.setInstrumentParam(1, id(BassParam::Tune), 55.0f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o;
  run(e, f, 0.15, &o);
  const size_t seam = o.size();
  e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f);  // same pitch: pure envelope/timbre retrigger
  run(e, f, 0.2, &o);
  float worst = 0;
  for (size_t i = seam - 2; i < seam + 600; i++) worst = std::fmax(worst, std::fabs(o[i + 1] - o[i]));
  CHECK(worst < 0.12f);
  CHECK(e.activeInstrumentCount() == 1);
  // Retrigger during release re-attacks from the current level (no restart-from-zero click).
  e.instrumentNoteOff(1);
  run(e, f, 0.03, &o);
  const size_t seam2 = o.size();
  e.instrumentNoteOn(1, 1.0f);
  run(e, f, 0.1, &o);
  worst = 0;
  for (size_t i = seam2 - 2; i < seam2 + 600; i++) worst = std::fmax(worst, std::fabs(o[i + 1] - o[i]));
  CHECK(worst < 0.12f);
}

void testScheduledRetriggerKeepsOldNoteSoundingUntilItsFrame() {
  // A note ending exactly where the next begins (a looped full-pattern note) lands mid-block.
  // The old note must keep sounding up to that frame: no silent gap before the seam.
  for (int64_t seamOffset : {0, 37, 64, 100}) {
    Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
    const int64_t start = 1000, dur = 48000 + seamOffset;
    e.scheduleInstrumentNote(start, 1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(dur));
    e.scheduleInstrumentNote(start + dur, 1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(dur));
    int64_t f = 0; Buf o; run(e, f, 1.2, &o);
    const size_t seam = size_t(start + dur);
    int run0 = 0, longest = 0;
    for (size_t i = seam - 300; i < seam + 300; i++) {
      run0 = (o[i] == 0.0f) ? run0 + 1 : 0;
      longest = std::max(longest, run0);
    }
    CHECK(longest < 8);
    float worst = 0;
    for (size_t i = seam - 300; i < seam; i++) worst = std::fmax(worst, std::fabs(o[i + 1] - o[i]));
    CHECK(worst < 0.05f);
  }
}

void testReleaseAndCleanup() {
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.setInstrumentParam(1, id(BassParam::Release), 0.2f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o;
  run(e, f, 0.3, &o);
  CHECK(e.activeInstrumentCount() == 1);       // held: sustains indefinitely
  e.instrumentNoteOff(1);
  e.instrumentNoteOff(1);                      // repeated note-off is a no-op
  run(e, f, 0.1, &o);
  CHECK(e.activeInstrumentCount() == 1);       // still in its tail
  run(e, f, 0.2, &o);
  CHECK(e.activeInstrumentCount() == 0);
  CHECK(peak(o, o.size() - 480) == 0.0f);
  // Removal mid-note and with a note still queued is safe and drops the queued note.
  e.instrumentNoteOn(1, 1.0f);
  e.scheduleInstrumentNote(f + 48000, 1, 1.0f);
  e.removeInstrument(1);
  Buf r; run(e, f, 1.2, &r);
  CHECK(peak(r) == 0.0f);
  CHECK(e.instrumentCount() == 0);
}

void testScheduledDurationIsSampleAccurate() {
  // Note scheduled mid-block with a duration that ends mid-block: release lands on the frame.
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.setInstrumentParam(1, id(BassParam::Release), 0.01f);
  e.setInstrumentParam(1, id(BassParam::Cutoff), 4000.0f);
  const int64_t start = 4000 + 37, dur = 6000 + 53;
  e.scheduleInstrumentNote(start, 1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(dur));
  int64_t f = 0; Buf o; run(e, f, 0.4, &o);
  CHECK(peak(o, 0, size_t(start) - 1) == 0.0f);          // silent before the start frame
  CHECK(peak(o, size_t(start) + 200, size_t(start + dur)) > 0.1f);
  const size_t end = size_t(start + dur);
  CHECK(rms(o, end - 100, end) > 0.05);                    // held right up to the release frame
  CHECK(peak(o, end + 520) == 0.0f);                       // 10 ms release done, exact silence
  CHECK(e.activeInstrumentCount() == 0);
}

void testImmediateDuration() {
  Setup s = clean(); s.release = 0.02f; s.duration = 0.1;
  Buf o = play(s, 0.4);
  CHECK(rms(o, at(0.05), at(0.09)) > 0.1);
  CHECK(peak(o, at(0.1) + at(0.03)) == 0.0f);
}

void testRetriggerWithoutDurationCancelsPendingRelease() {
  Engine e; e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  e.instrumentNoteOn(1, 1.0f, 0, 0, 4800);   // would release at 0.1 s
  int64_t f = 0; Buf o; run(e, f, 0.05, &o);
  e.instrumentNoteOn(1, 1.0f);               // new, held note supersedes it
  run(e, f, 0.4, &o);
  CHECK(rms(o, at(0.3), at(0.45)) > 0.05);
  CHECK(e.activeInstrumentCount() == 1);
}

// --- legato / slide ----------------------------------------------------------------------------

struct Note { double at; float pitch; double dur; float vel; };

// Renders scheduled bass notes on a clean sustained voice (legato/slide as given) in blocks of
// `block` frames. Note frames are exact, so different block sizes see identical note timing.
Buf playNotes(const std::vector<Note>& notes, double seconds, bool legato, float slide,
              int32_t block = kBlock, float sustain = 1.0f, Engine* keep = nullptr) {
  Engine local; Engine& e = keep ? *keep : local;
  e.init(kRate, 2, 8); e.createInstrument(1, kBass, 0);
  Setup s = clean(); s.sustain = sustain; s.tune = 55;
  apply(e, s);
  e.setInstrumentParam(1, id(BassParam::Legato), legato ? 1.0f : 0.0f);
  e.setInstrumentParam(1, id(BassParam::Slide), slide);
  for (const Note& n : notes) {
    e.scheduleInstrumentNote(static_cast<int64_t>(n.at * kRate), 1, n.vel, 0.0f, n.pitch,
                             n.dur < 0 ? -1 : static_cast<int32_t>(n.dur * kRate));
  }
  Buf o; int64_t f = 0;
  const int32_t frames = static_cast<int32_t>(seconds * kRate / block) * block;
  for (int32_t d = 0; d < frames; d += block) {
    e.process(f, block);
    f += block;
    o.insert(o.end(), e.outputChannel(0), e.outputChannel(0) + block);
  }
  return o;
}
double hzOf(double tune, double semis) { return tune * std::exp2(semis / 12.0); }
double worstStep(const Buf& o, size_t a, size_t b) {
  double w = 0;
  for (size_t i = a; i < b; i++) w = std::fmax(w, std::fabs(o[i + 1] - o[i]));
  return w;
}

void testLegatoContinuesVoiceWithoutReattack() {
  // sustain 0.3: a re-attack would jump the level back toward 1; legato must stay at the plateau.
  const std::vector<Note> ab = {{0.0, 0, 1.0, 1.0f}, {0.5, 7, 1.0, 1.0f}};
  Buf off = playNotes(ab, 0.9, false, 0.0f, kBlock, 0.3f);
  Buf on = playNotes(ab, 0.9, true, 0.0f, kBlock, 0.3f);
  const double before = rms(on, at(0.4), at(0.49));
  const double after = rms(on, at(0.55), at(0.64));
  CHECK_NEAR(after / before, 1.0, 0.04);
  CHECK(rms(off, at(0.5025), at(0.52)) / rms(off, at(0.4), at(0.49)) > 1.4);  // control: it does re-attack
  // Pitch is B straight away (slide 0) and A before.
  CHECK_NEAR(peakFreq(on, at(0.4), at(0.49), 55.0) / 55.0, 1.0, 0.003);
  CHECK_NEAR(peakFreq(on, at(0.55), at(0.9), hzOf(55, 7)) / hzOf(55, 7), 1.0, 0.003);
  // No click at the seam.
  const size_t seam = at(0.5);
  CHECK(worstStep(on, seam - 200, seam + 400) < 0.12);
}

void testLegatoKeepsHeldVelocity() {
  // The new note's velocity would be a level step; legato keeps the held note's level.
  Buf o = playNotes({{0.0, 0, 1.0, 1.0f}, {0.4, 5, 1.0, 0.2f}}, 0.8, true, 0.0f);
  CHECK_NEAR(rms(o, at(0.5), at(0.7)) / rms(o, at(0.25), at(0.38)), 1.0, 0.04);
}

void testNonLegatoIsOrdinaryRetrigger() {
  // Note A released before B: a normal new note even with legato on, and slide never applies.
  Buf o = playNotes({{0.0, 0, 0.2, 1.0f}, {0.3, 12, 0.5, 1.0f}}, 0.8, true, 0.3f);
  CHECK_NEAR(peakFreq(o, at(0.4), at(0.7), 110.0) / 110.0, 1.0, 0.003);
  // Released-but-still-ringing predecessor (release 0.05 s): retrigger, no glide either.
  Buf r = playNotes({{0.0, 0, 0.2, 1.0f}, {0.22, 12, 0.5, 1.0f}}, 0.8, true, 0.3f);
  CHECK_NEAR(peakFreq(r, at(0.3), at(0.6), 110.0) / 110.0, 1.0, 0.003);
  // Legato off: abutting notes also just retrigger and jump.
  Buf j = playNotes({{0.0, 0, 0.3, 1.0f}, {0.3, 12, 0.5, 1.0f}}, 0.8, false, 0.3f);
  CHECK_NEAR(peakFreq(j, at(0.4), at(0.7), 110.0) / 110.0, 1.0, 0.003);
}

void testSlideGlidesAndLandsOnTarget() {
  const double tA = 0.3, slide = 0.2;
  Buf o = playNotes({{0.0, 0, 1.0, 1.0f}, {tA, 12, 1.0, 1.0f}}, 1.0, true, float(slide));
  CHECK_NEAR(peakFreq(o, at(0.2), at(0.29), 55.0) / 55.0, 1.0, 0.003);
  // Mid-glide the pitch is strictly between the two (linear in pitch: +6 st at the halfway point).
  const double mid = peakFreq(o, at(tA + 0.06), at(tA + 0.14), hzOf(55, 6));
  CHECK(mid > 55.0 * 1.1 && mid < 110.0 * 0.92);
  CHECK_NEAR(mid / hzOf(55, 6), 1.0, 0.08);
  CHECK_NEAR(peakFreq(o, at(tA + 0.25), at(tA + 0.6), 110.0) / 110.0, 1.0, 0.003);
  CHECK(worstStep(o, at(tA) - 200, at(tA + 0.5)) < 0.12);
}

void testSlideAcrossLargeIntervalsBothDirections() {
  // C -> C two octaves up and back down: same time, exact arrival (not a fixed number of Hz).
  Buf o = playNotes({{0.0, 0, 0.5, 1.0f}, {0.5, 24, 0.5, 1.0f}, {1.0, 0, 1.0, 1.0f}}, 2.0, true, 0.1f);
  CHECK_NEAR(peakFreq(o, at(0.7), at(0.98), 220.0) / 220.0, 1.0, 0.003);
  CHECK_NEAR(peakFreq(o, at(1.2), at(1.9), 55.0) / 55.0, 1.0, 0.003);
  CHECK(finiteAll(o) && peak(o) <= 1.0f);
}

void testSlideIsBlockSizeIndependent() {
  const std::vector<Note> n = {{0.0, 0, 1.0, 1.0f}, {0.32, 9, 1.0, 1.0f}};
  Buf a = playNotes(n, 0.96, true, 0.15f, 64);
  Buf b = playNotes(n, 0.96, true, 0.15f, 128);
  Buf c = playNotes(n, 0.96, true, 0.15f, 256);
  CHECK(a.size() == b.size() && b.size() == c.size());
  double d1 = 0, d2 = 0;
  for (size_t i = 0; i < a.size(); i++) {
    d1 = std::fmax(d1, std::fabs(a[i] - b[i]));
    d2 = std::fmax(d2, std::fabs(a[i] - c[i]));
  }
  CHECK(d1 < 1e-4 && d2 < 1e-4);
}

void testRepeatedLegatoAndMidGlideRetarget() {
  // A -> B -> C held throughout (abutting durations); each transition glides from where it was.
  Buf o = playNotes({{0.0, 0, 0.4, 1.0f}, {0.4, 7, 0.4, 1.0f}, {0.8, 3, 0.3, 1.0f}}, 1.6, true, 0.0f);
  CHECK_NEAR(peakFreq(o, at(0.25), at(0.38), 55.0) / 55.0, 1.0, 0.003);
  CHECK_NEAR(peakFreq(o, at(0.5), at(0.78), hzOf(55, 7)) / hzOf(55, 7), 1.0, 0.003);
  CHECK_NEAR(peakFreq(o, at(0.9), at(1.09), hzOf(55, 3)) / hzOf(55, 3), 1.0, 0.003);
  CHECK(worstStep(o, at(0.4) - 100, at(0.4) + 300) < 0.12);
  CHECK(worstStep(o, at(0.8) - 100, at(0.8) + 300) < 0.12);
  // C arrives 0.05 s into a 0.2 s glide to B: it retargets from the current pitch, lands on C.
  Buf m = playNotes({{0.0, 0, 1.0, 1.0f}, {0.3, 12, 1.0, 1.0f}, {0.35, 5, 1.0, 1.0f}}, 1.2, true, 0.2f);
  CHECK_NEAR(peakFreq(m, at(0.7), at(1.1), hzOf(55, 5)) / hzOf(55, 5), 1.0, 0.003);
  CHECK(worstStep(m, at(0.28), at(0.7)) < 0.12);
}

void testLegatoReleaseOwnership() {
  // Last note owns the voice: B's duration replaces A's release, and the voice ends after B only.
  Engine e;
  Buf o = playNotes({{0.0, 0, 0.5, 1.0f}, {0.2, 7, 0.3, 1.0f}}, 0.4, true, 0.0f, kBlock, 1.0f, &e);
  CHECK(e.activeInstrumentCount() == 1);        // one voice for both notes, still sounding
  CHECK(rms(o, at(0.3), at(0.39)) > 0.1);       // A's 0.5 s release did not fire at 0.5, nor at 0.2
  int64_t f = int64_t(o.size()); Buf t;
  run(e, f, 0.5, &t);                            // B's note-off at 0.5 s, then release (0.05 s)
  CHECK(rms(t, at(0.05), at(0.09)) > 0.1);       // still held before 0.5 s
  CHECK(rms(t, at(0.3), at(0.5)) < 1e-4);
  CHECK(e.activeInstrumentCount() == 0);
  // Slide = 0 and legato on with an unheld (idle) voice is simply a fresh note.
  Buf fresh = playNotes({{0.0, 7, 0.3, 1.0f}}, 0.4, true, 0.2f);
  CHECK_NEAR(peakFreq(fresh, at(0.1), at(0.28), hzOf(55, 7)) / hzOf(55, 7), 1.0, 0.003);
}

void testRoutesThroughBus() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kBass, /*busId=*/1);
  e.setBusParam(1, static_cast<int32_t>(NodeParam::BusGain), 0.5f);
  e.instrumentNoteOn(1, 1.0f);
  int64_t f = 0; Buf o; run(e, f, 0.3, &o);
  Buf direct = play({}, 0.3);
  CHECK(peak(o) > 0.0f);
  CHECK_NEAR(peak(o), peak(direct) * 0.5, 1e-3);
}

// Guards: the instrument must not collapse to "naive saw -> LP -> ADSR".
void testIsNotATrivialOscillatorFilter() {
  // 1. Timbre moves independently of level: with sustain 1 the *level* is flat, yet the
  //    normalised spectrum darkens as the internal timbre envelope closes.
  Setup s; s.body = 1; s.bite = 1; s.cutoff = 150; s.res = 0.2f; s.sustain = 1; s.decay = 0.5f;
  s.attack = 0.001f;
  Buf o = play(s, 0.9, "bite");
  const double early = centroid(o, at(0.01), at(0.09), 55);
  const double late = centroid(o, at(0.55), at(0.85), 55);
  const double levelRatio = rms(o, at(0.55), at(0.85)) / rms(o, at(0.01), at(0.09));
  CHECK(early > late * 1.3);
  CHECK(levelRatio > 0.5 && levelRatio < 1.5);  // level essentially flat (it is the timbre moving)

  // 2. Even harmonics: the asymmetric shaper puts energy at 2*f0 that a symmetric/linear
  //    path does not; it rises with bite and is absent at bite 0 on a pure sine core.
  Setup c = clean(); c.cutoff = 4000;
  Setup d = c; d.bite = 1; d.body = 0; d.sustain = 1; d.decay = 2.0f;
  const double h2clean = bin(play(c, 0.6), at(0.1), at(0.5), 110) / bin(play(c, 0.6), at(0.1), at(0.5), 55);
  const double h2bite = bin(play(d, 0.6), at(0.1), at(0.5), 110) / bin(play(d, 0.6), at(0.1), at(0.5), 55);
  CHECK(h2clean < 0.05);
  CHECK(h2bite > h2clean * 3.0);

  // 3. Fundamental retention: the cutoff is floored at the note's fundamental, so even the
  //    lowest cutoff leaves the pitch within 6 dB of the open-filter level.
  Setup open = clean(); open.body = 0.5f; open.tune = 110; open.cutoff = 4000;
  Setup shut = open; shut.cutoff = 40;
  const double fo = bin(play(open, 0.5), at(0.1), at(0.4), 110);
  const double fs = bin(play(shut, 0.5), at(0.1), at(0.4), 110);
  CHECK(fs > fo * 0.5);

  // 4. Nonlinear in velocity at bite > 0: half velocity is not a half-amplitude copy.
  Setup full = s; full.sustain = 0.8f;
  Setup half = full; half.velocity = 0.5f;
  Buf a = play(full, 0.4), b = play(half, 0.4);
  double diff = 0, ref = 0;
  for (size_t i = at(0.05); i < at(0.35); i++) {
    const double d2 = double(a[i]) * 0.5 - double(b[i]);
    diff += d2 * d2; ref += double(a[i]) * 0.5 * double(a[i]) * 0.5;
  }
  CHECK(std::sqrt(diff / ref) > 0.1);

  // 5. Distance from a naive saw -> one-pole LP -> ADSR reference at the same level/pitch.
  Buf ref2(a.size());
  double ph = 0, lp = 0;
  const double coef = 1.0 - std::exp(-2 * kPi * 150.0 / kRate);
  for (size_t i = 0; i < ref2.size(); i++) {
    ph += 55.0 / kRate; ph -= std::floor(ph);
    lp += coef * ((2 * ph - 1) - lp);
    ref2[i] = float(lp);
  }
  // Compare normalised magnitude spectra at the first 12 harmonics (onset window).
  double dist = 0;
  const double ra = bin(a, at(0.01), at(0.09), 55), rb = bin(ref2, at(0.01), at(0.09), 55);
  for (int h = 2; h <= 12; h++) {
    const double x = bin(a, at(0.01), at(0.09), 55 * h) / ra;
    const double y = bin(ref2, at(0.01), at(0.09), 55 * h) / rb;
    dist += std::fabs(x - y);
  }
  CHECK(dist > 0.5);
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kBass, 1);
  e.setInstrumentParam(1, id(BassParam::Bite), 0.9f);
  e.setInstrumentParam(1, id(BassParam::Resonance), 0.8f);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 600; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);   // timed note: exercises the split
    if (i == 300) e.instrumentNoteOff(1);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testProducesFiniteBoundedAudio();
  testDeterministic();
  testPitchAccuracy();
  testEnvelopeTiming();
  testFilterClosesAndTracksKey();
  testResonanceStableAndBounded();
  testParamValidation();
  testVelocity();
  testRetriggerIsContinuous();
  testScheduledRetriggerKeepsOldNoteSoundingUntilItsFrame();
  testReleaseAndCleanup();
  testScheduledDurationIsSampleAccurate();
  testImmediateDuration();
  testRetriggerWithoutDurationCancelsPendingRelease();
  testLegatoContinuesVoiceWithoutReattack();
  testLegatoKeepsHeldVelocity();
  testNonLegatoIsOrdinaryRetrigger();
  testSlideGlidesAndLandsOnTarget();
  testSlideAcrossLargeIntervalsBothDirections();
  testSlideIsBlockSizeIndependent();
  testRepeatedLegatoAndMidGlideRetarget();
  testLegatoReleaseOwnership();
  testRoutesThroughBus();
  testIsNotATrivialOscillatorFilter();
  testRenderDoesNotAllocate();
  std::printf("bass_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
