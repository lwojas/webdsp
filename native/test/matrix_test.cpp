#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <set>
#include <string>
#include <vector>
#include "../src/engine.h"
#include "../src/params.h"
#include "test_main.h"

// Covers Matrix (instruments/matrix.h, dsp/matrix_filter.h): registration and the 109-row catalogue
// (21 module controls + 88 matrix cells, 8 outs x 11 ins), that every control and every cell reaches the
// DSP, lifecycle and determinism, that the default patch is the ordinary voice and that every cable of it
// (the audio path included) can be removed or redirected, what each kind of cable does (envelope and LFO
// sweeps, LFO rates, audio-rate cables, noise through the filter), that an amount of 0 is exactly no cable,
// feedback (bounded, never non-finite, even at the extremes), the filter modes, parameter smoothing, bus
// routing and allocation. DSP checks are sanity measurements (zero-crossing pitch, band energy,
// brightness), never exact samples.
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
constexpr int32_t kMx = static_cast<int32_t>(InstrumentKind::Matrix);
constexpr double kPi = 3.14159265358979323846;

using Buf = std::vector<float>;
using P = MatrixParam;
using O = MatrixOut;
using I = MatrixIn;
int32_t id(P p) { return static_cast<int32_t>(p); }
int32_t cell(O o, I i) { return matrixAmountParam(o, i); }
size_t at(double s) { return static_cast<size_t>(s * kRate); }

void run(Engine& e, int64_t& frame, double seconds, Buf* o) {
  const int32_t frames = static_cast<int32_t>(std::ceil(seconds * kRate / kBlock)) * kBlock;
  for (int32_t d = 0; d < frames; d += kBlock) {
    e.process(frame, kBlock);
    frame += kBlock;
    o->insert(o->end(), e.outputChannel(0), e.outputChannel(0) + kBlock);
  }
}

struct Rig {
  Engine e;
  int64_t f = 0;
  Buf o;
  explicit Rig(int32_t bus = 0) { e.init(kRate, 2, 8); e.createInstrument(1, kMx, bus); }
  void set(P p, float v) { e.setInstrumentParam(1, id(p), v); }
  void patch(O o, I i, float a) { e.setInstrumentParam(1, cell(o, i), a); }
  void go(double s) { run(e, f, s, &o); }
};

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
// First-difference rms over rms: a cheap brightness measure (grows with high-frequency content).
double bright(const Buf& v, size_t a, size_t b) {
  double d = 0, s = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) { const double x = v[i] - v[i - 1]; d += x * x; s += double(v[i]) * v[i]; }
  return std::sqrt(d / std::fmax(s, 1e-12));
}
bool finite(const Buf& v) {
  for (float x : v) if (!std::isfinite(x)) return false;
  return true;
}
double crossingHz(const Buf& v, size_t a, size_t b) {
  int n = 0;
  b = std::min(b, v.size());
  for (size_t i = a + 1; i < b; i++) if (v[i - 1] < 0.0f && v[i] >= 0.0f) n++;
  return n / (double(b - a) / kRate);
}

// Reference patch for measurements: a held note (sustain 1), filter wide open, no patches except
// ENV 1 -> level so the note sounds. Each test changes only what it examines.
using Params = std::vector<std::pair<int32_t, float>>;
const Params kHeld = {{id(P::Tune), 110.0f}, {id(P::Cutoff), 16000.0f}, {id(P::Resonance), 0.0f},
                      {id(P::Keytrack), 0.0f}, {id(P::Env1Attack), 0.001f}, {id(P::Env1Decay), 1.0f},
                      {id(P::Env1Sustain), 1.0f}, {id(P::Env1Release), 0.05f},
                      {cell(O::Env2, I::Cutoff), 0.0f}};

Buf render(const Params& params, double seconds = 1.0, float pitch = 0.0f, float velocity = 1.0f,
           const Params& base = kHeld) {
  Rig r;
  for (auto& [p, v] : base) r.e.setInstrumentParam(1, p, v);
  for (auto& [p, v] : params) r.e.setInstrumentParam(1, p, v);
  r.e.instrumentNoteOn(1, velocity, 0.0f, pitch);
  r.go(seconds);
  return r.o;
}

// ---- registration and catalogue ---------------------------------------------------------------

void testRegistrationAndCatalogue() {
  const InstrumentKindMeta* m = instrumentKindMeta(kMx);
  CHECK(m && std::strcmp(m->id, "matrix") == 0 && std::strcmp(m->name, "Matrix") == 0 &&
        std::strcmp(m->category, "synth") == 0);
  CHECK(kMx == 18 && kMatrixParamCount == 109 && instrumentParamCount(kMx) == kMatrixParamCount + 1);
  CHECK(kMatrixOutCount == 8 && kMatrixInCount == 11 && kMatrixAmountCount == 88);
  CHECK(instrumentParamCount(kMx) <= kMaxModParams);
  auto in = makeInstrument(kMx);
  CHECK(in && in->kind() == InstrumentKind::Matrix && !in->isActive());
  std::set<std::string> names;
  for (int32_t i = 0; i < kMatrixParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kMx, i);
    CHECK(p && p->id == i && p->def >= p->min && p->def <= p->max && p->min < p->max);
    CHECK(names.insert(p->name).second);  // names are unique, so a client can key on them
  }
  CHECK(instrumentParamInfo(kMx, kMatrixParamCount)->id == kInstrumentGainParam &&
        instrumentParamInfo(kMx, kMatrixParamCount + 1) == nullptr);

  // The matrix: id = 21 + out * 11 + in, signed amount, not an LFO target, named "<out>_<in>".
  CHECK(cell(O::Vco, I::Pitch) == 21 && cell(O::Vca, I::Out) == 108 && cell(O::Env1, I::Level) == 21 + 4 * 11 + 7);
  const char* outName[] = {"vco", "noise", "lfo1", "lfo2", "env1", "env2", "vcf", "vca"};
  const char* inName[] = {"pitch", "pwm", "vcfin", "cutoff", "resonance", "drive", "vcain", "level", "lfo1rate", "lfo2rate", "out"};
  std::vector<std::string> patched;
  for (int o = 0; o < kMatrixOutCount; o++) {
    for (int i = 0; i < kMatrixInCount; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kMx, cell(static_cast<O>(o), static_cast<I>(i)));
      CHECK(p && p->min == -1.0f && p->max == 1.0f);
      CHECK(std::string(p->name) == std::string(outName[o]) + "_" + inName[i]);
      CHECK(resolveModDomain(*p) == ModDomain::None);
      if (p->def != 0.0f) patched.push_back(p->name);
    }
  }
  // The default patch is the ordinary voice, wired the way a Mother-32 is normalled.
  CHECK((patched == std::vector<std::string>{"vco_vcfin", "env1_level", "env2_cutoff", "vcf_vcain", "vca_out"}));
  CHECK_NEAR(instrumentParamInfo(kMx, cell(O::Env2, I::Cutoff))->def, 0.4, 1e-6);

  auto find = [](const char* n) { for (int32_t i = 0; i < kMatrixParamCount; i++) if (!std::strcmp(kMatrixParams[i].name, n)) return &kMatrixParams[i]; return (const InstrumentParamInfo*)nullptr; };
  CHECK(find("env1sustain")->def == 0.0f);                       // a one-shot out of the box
  CHECK(find("noise") == nullptr);                               // noise is a cable now, not a knob
  CHECK(resolveModDomain(*find("cutoff")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("lfo1rate")) == ModDomain::Log);
  CHECK(resolveModDomain(*find("wave")) == ModDomain::None && resolveModDomain(*find("mode")) == ModDomain::None);
  CHECK(resolveModDomain(*find("resonance")) == ModDomain::Linear);
  Engine e; e.init(kRate, 2, 8);
  CHECK(e.createInstrument(1, kMx, 0));
}

// The LFO layer reaches every control, the base of any parameter reads back even at a high index, and
// the matrix cells are not modulation targets.
void testModulationLayerCoversTheTable() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kMx, 0);
  e.setInstrumentParam(1, id(P::Env2Release), 1.5f);
  CHECK_NEAR(e.instrumentBaseParam(1, id(P::Env2Release)), 1.5, 1e-6);
  e.setInstrumentParam(1, cell(O::Vca, I::Out), 0.7f);   // the last cell
  CHECK_NEAR(e.instrumentBaseParam(1, cell(O::Vca, I::Out)), 0.7, 1e-6);
  CHECK_NEAR(e.instrumentBaseParam(1, kInstrumentGainParam), 0.0, 1e-6);
  LfoConfig c;
  c.enabled = true;
  c.target = id(P::Cutoff);
  c.amount = 0.5f;
  CHECK(e.setInstrumentLfo(1, 0, c));
  c.target = cell(O::Lfo1, I::Cutoff);                    // a matrix cell is not a target
  CHECK(!e.setInstrumentLfo(1, 1, c));
}

// Every control and every cell reaches the DSP: moving it off its default changes the rendered note.
// The reference patch gives each one something to act on: a pulse wave for PWM, every module in use.
void testEveryParameterPropagates() {
  const Params patch = {{id(P::Wave), 1.0f}, {id(P::Resonance), 0.3f}, {id(P::Cutoff), 2500.0f},
                        {id(P::Drive), 0.3f}, {id(P::Level), 0.3f}, {id(P::Env1Sustain), 0.5f},
                        {id(P::Env2Sustain), 0.5f}, {id(P::Lfo1Rate), 6.0f}, {id(P::Lfo2Rate), 3.0f},
                        {id(P::Lfo1Wave), 2.0f}, {id(P::Lfo2Wave), 4.0f},
                        {cell(O::Lfo1, I::Cutoff), 0.2f}, {cell(O::Lfo2, I::Pwm), 0.3f},
                        {cell(O::Env2, I::Cutoff), 0.3f}, {cell(O::Lfo2, I::Pitch), 0.05f}};
  auto make = [&](int32_t override, float value) {
    Rig r;
    for (auto& [p, v] : patch) r.e.setInstrumentParam(1, p, v);
    if (override >= 0) r.e.setInstrumentParam(1, override, value);
    r.e.instrumentNoteOn(1, 0.8f, 0.0f, 5.0f, static_cast<int32_t>(0.3 * kRate));  // off the root, so keytrack acts
    r.go(0.6);
    return r.o;
  };
  const Buf ref = make(-1, 0.0f);
  for (int32_t i = 0; i < kMatrixParamCount; i++) {
    const InstrumentParamInfo* p = instrumentParamInfo(kMx, i);
    float moved = p->def;
    for (const auto& [pp, vv] : patch) if (pp == i) moved = vv;
    const float target = (std::fabs(moved - p->min) > std::fabs(moved - p->max)) ? p->min : p->max;
    const Buf out = make(i, target);
    CHECK(finite(out));
    if (out == ref) std::fprintf(stderr, "param %s has no effect\n", p->name);
    CHECK(out != ref);
  }
}

void testParamValidation() {
  auto in = makeInstrument(kMx);
  in->configure(kRate);
  in->setParam(id(P::Cutoff), 1e9f);              // clamped, not crashed
  in->setParam(id(P::Cutoff), std::nanf(""));     // ignored
  in->setParam(cell(O::Vcf, I::Cutoff), 50.0f);   // clamped to 1
  in->setParam(id(P::Mode), -5.0f);
  in->setParam(9999, 1.0f);                       // unknown id: no-op
  in->noteOn(1.0f, 0, 0.0f, 0.0f);
  float a[kBlock] = {}, b[kBlock] = {}, s0[kBlock] = {}, s1[kBlock] = {};
  float* bus[2] = {a, b};
  float* scratch[2] = {s0, s1};
  CHECK(in->render(bus, 2, kBlock, scratch));
  for (int i = 0; i < kBlock; i++) CHECK(std::isfinite(a[i]) && std::fabs(a[i]) <= 1.0f);
}

// ---- lifecycle --------------------------------------------------------------------------------

void testLifecycle() {
  // A pluck (sustain 0) ends on its own once ENV 1 has finished.
  {
    Rig r;
    r.set(P::Env1Decay, 0.1f);
    r.set(P::Env1Release, 0.05f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(0.05);
    CHECK(peak(r.o) > 0.05f);
    r.go(0.6);
    CHECK(peak(r.o, at(0.5), r.o.size()) < 1e-6f);
  }
  // A held note rings until note-off, then releases to silence.
  {
    Rig r;
    for (auto& [p, v] : kHeld) r.e.setInstrumentParam(1, p, v);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(0.5);
    CHECK(rms(r.o, at(0.3), at(0.5)) > 0.05);
    r.e.instrumentNoteOff(1);
    r.go(0.3);
    CHECK(rms(r.o, at(0.7), at(0.8)) < 1e-4);
  }
  // A note with a duration releases by itself.
  {
    Rig r;
    for (auto& [p, v] : kHeld) r.e.setInstrumentParam(1, p, v);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, 0.0f, static_cast<int32_t>(0.2 * kRate));
    r.go(0.6);
    CHECK(rms(r.o, at(0.1), at(0.2)) > 0.05);
    CHECK(rms(r.o, at(0.4), at(0.6)) < 1e-4);
  }
  // ENV 1 is the note's life whatever it is patched to: with the amp held open by its base level and
  // ENV 1 taken off it, the note still ends with ENV 1.
  {
    Rig r;
    r.set(P::Level, 0.8f);
    r.patch(O::Env1, I::Level, 0.0f);
    r.set(P::Env1Decay, 0.1f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(0.05);
    CHECK(rms(r.o, 0, at(0.05)) > 0.05);   // a gate: constant level while ENV 1 runs
    r.go(0.6);
    CHECK(peak(r.o, at(0.5), r.o.size()) < 1e-6f);
  }
}

void testDeterministicAndRetrigger() {
  const Params noisy = {{id(P::Lfo1Wave), 4.0f}, {cell(O::Lfo1, I::Cutoff), 0.5f}, {cell(O::Noise, I::VcfIn), 0.4f},
                        {cell(O::Vcf, I::Cutoff), 0.3f}, {id(P::Resonance), 0.7f}};
  CHECK(render(noisy) == render(noisy));            // identical inputs, identical samples
  // A retrigger on a sounding note is continuous: no jump beyond what the signal itself does.
  Rig r;
  for (auto& [p, v] : kHeld) r.e.setInstrumentParam(1, p, v);
  r.set(P::Cutoff, 1500.0f);
  r.e.instrumentNoteOn(1, 0.9f);
  r.go(0.3);
  float own = 0;
  for (size_t i = at(0.2) + 1; i < r.o.size(); i++) own = std::fmax(own, std::fabs(r.o[i] - r.o[i - 1]));
  const size_t mark = r.o.size();
  r.e.instrumentNoteOn(1, 0.9f, 0.0f, 7.0f);
  r.go(0.1);
  float jump = 0;
  for (size_t i = mark; i < r.o.size(); i++) jump = std::fmax(jump, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(jump < own * 3.0f + 0.1f);
  CHECK(finite(r.o));
}

void testVelocityAndPitch() {
  const Buf loud = render({}, 0.5, 0.0f, 1.0f), soft = render({}, 0.5, 0.0f, 0.3f);
  CHECK(rms(loud, at(0.1), at(0.4)) > 2.0 * rms(soft, at(0.1), at(0.4)));
  // The note's pitch is semitones on top of Tune (saw, filter open: the zero-crossing rate is f0).
  const Buf a = render({}, 0.6), b = render({}, 0.6, 12.0f), c = render({{id(P::Tune), 220.0f}}, 0.6);
  CHECK_NEAR(crossingHz(a, at(0.1), at(0.5)), 110.0, 3.0);
  CHECK_NEAR(crossingHz(b, at(0.1), at(0.5)), 220.0, 5.0);
  CHECK_NEAR(crossingHz(c, at(0.1), at(0.5)), 220.0, 5.0);
}

// ---- the voice --------------------------------------------------------------------------------

void testWaves() {
  const Buf saw = render({{id(P::Wave), 0.0f}}), pulse = render({{id(P::Wave), 1.0f}}), tri = render({{id(P::Wave), 2.0f}});
  CHECK(finite(saw) && finite(pulse) && finite(tri));
  // A triangle has far less top end than a saw; a narrow pulse is brighter than a square.
  CHECK(bright(tri, at(0.2), at(0.8)) < 0.4 * bright(saw, at(0.2), at(0.8)));
  const Buf square = render({{id(P::Wave), 1.0f}, {id(P::Pwm), 0.5f}}), narrow = render({{id(P::Wave), 1.0f}, {id(P::Pwm), 0.1f}});
  CHECK(bright(narrow, at(0.2), at(0.8)) > bright(square, at(0.2), at(0.8)));
  // Pulse width does not leave a DC offset behind.
  double m = 0;
  for (size_t i = at(0.2); i < at(0.8); i++) m += narrow[i];
  CHECK(std::fabs(m / double(at(0.6))) < 0.12);
  for (const Buf* b : {&saw, &pulse, &tri}) CHECK(peak(*b) <= 1.0f && rms(*b, at(0.2), at(0.8)) > 0.05);
}

void testFilterModes() {
  // Direct measurement: a sine through the filter at 100 Hz and at 8 kHz, cutoff 1 kHz.
  auto gain = [](int32_t mode, double hz, float res) {
    MatrixFilter f;
    f.configure(kRate);
    double acc = 0;
    const int n = 24000;
    for (int i = 0; i < n; i++) {
      const float y = f.process(0.5f * static_cast<float>(std::sin(2 * kPi * hz * i / kRate)), 1000.0f, res, 0.0f, mode);
      if (i >= n / 2) acc += double(y) * y;
    }
    return std::sqrt(acc / (n / 2)) / (0.5 / std::sqrt(2.0));
  };
  CHECK(gain(0, 100, 0.0f) > 0.85 && gain(0, 8000, 0.0f) < 0.01);      // LP 24
  CHECK(gain(1, 100, 0.0f) > 0.85 && gain(1, 8000, 0.0f) < 0.05);      // LP 12
  CHECK(gain(0, 8000, 0.0f) < gain(1, 8000, 0.0f));                    // 24 dB is steeper than 12
  CHECK(gain(3, 100, 0.0f) < 0.05 && gain(3, 8000, 0.0f) > 0.85);      // HP
  CHECK(gain(2, 1000, 0.0f) > 0.4 && gain(2, 100, 0.0f) < 0.3 && gain(2, 8000, 0.0f) < 0.3);   // BP peaks at the cutoff
  CHECK(gain(0, 1000, 0.9f) > gain(0, 1000, 0.0f));                    // resonance lifts the corner
  CHECK(gain(1, 1000, 0.9f) > gain(1, 1000, 0.0f));
  // The ladder rings on its own at full resonance, and a silent input stays silent.
  MatrixFilter f;
  f.configure(kRate);
  float y = f.process(1.0f, 1000.0f, 1.0f, 0.0f, 0);
  double tail = 0;
  bool ok = std::isfinite(y);
  for (int i = 1; i < 24000; i++) {
    y = f.process(0.0f, 1000.0f, 1.0f, 0.0f, 0);
    ok = ok && std::isfinite(y) && std::fabs(y) <= 1.0f;
    if (i > 12000) tail += double(y) * y;
  }
  CHECK(ok && tail > 1e-3);
  MatrixFilter z;
  z.configure(kRate);
  for (int i = 0; i < 1000; i++) CHECK(z.process(0.0f, 5000.0f, 0.5f, 0.5f, i % 4) == 0.0f);
}

void testVoiceFilter() {
  const Buf open = render({{id(P::Cutoff), 12000.0f}}), closed = render({{id(P::Cutoff), 300.0f}});
  CHECK(bright(closed, at(0.2), at(0.8)) < 0.3 * bright(open, at(0.2), at(0.8)));
  const Buf hp = render({{id(P::Mode), 3.0f}, {id(P::Cutoff), 4000.0f}});
  CHECK(rms(hp, at(0.2), at(0.8)) < rms(open, at(0.2), at(0.8)));
  // Keytrack: an octave up with full keytrack opens the filter, with none it does not.
  const Buf lowNote = render({{id(P::Cutoff), 600.0f}, {id(P::Keytrack), 1.0f}}, 0.6, 0.0f);
  const Buf highNote = render({{id(P::Cutoff), 600.0f}, {id(P::Keytrack), 1.0f}}, 0.6, 24.0f);
  const Buf highFixed = render({{id(P::Cutoff), 600.0f}, {id(P::Keytrack), 0.0f}}, 0.6, 24.0f);
  CHECK(rms(highNote, at(0.1), at(0.5)) > 1.5 * rms(highFixed, at(0.1), at(0.5)));
  CHECK(finite(lowNote));
}

// ---- the patch bay: the audio path is cables too --------------------------------------------------

void testDefaultPatchIsTheOrdinaryVoice() {
  // Writing zero to every cell that is not part of the default patch is exactly the default voice.
  Params zeros;
  for (int o = 0; o < kMatrixOutCount; o++) {
    for (int i = 0; i < kMatrixInCount; i++) {
      const int32_t c = cell(static_cast<O>(o), static_cast<I>(i));
      if (instrumentParamInfo(kMx, c)->def == 0.0f) zeros.push_back({c, 0.0f});
    }
  }
  CHECK(render({}) == render(zeros));
  // It sounds on its own: VCO into the filter into the amp into the output, ENV 1 on the amp.
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.5);
  CHECK(peak(r.o) > 0.1f && finite(r.o));
}

void testEveryCableOfTheDefaultPatchCanBeRemoved() {
  auto silent = [](Params p) {
    Rig r;
    for (auto& [id_, v] : p) r.e.setInstrumentParam(1, id_, v);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(0.3);
    return peak(r.o) == 0.0f;
  };
  CHECK(silent({{cell(O::Vca, I::Out), 0.0f}}));        // nothing reaches the output
  CHECK(silent({{cell(O::Vco, I::VcfIn), 0.0f}}));      // nothing reaches the filter
  CHECK(silent({{cell(O::Vcf, I::VcaIn), 0.0f}}));      // nothing reaches the amp
  CHECK(silent({{cell(O::Env1, I::Level), 0.0f}}));     // the amp never opens
}

void testAudioPathCanBeRerouted() {
  // The oscillator straight into the amp skips the filter: with a closed filter the default chain is
  // dull and the bypass is bright.
  const Buf chain = render({{id(P::Cutoff), 300.0f}});
  const Buf bypass = render({{id(P::Cutoff), 300.0f}, {cell(O::Vcf, I::VcaIn), 0.0f}, {cell(O::Vco, I::VcaIn), 1.0f}});
  CHECK(bright(bypass, at(0.2), at(0.8)) > 3.0 * bright(chain, at(0.2), at(0.8)));
  // The oscillator straight to the output skips the amp as well (no envelope: a constant tone).
  const Buf direct = render({{cell(O::Vca, I::Out), 0.0f}, {cell(O::Vco, I::Out), 0.8f}});
  CHECK(rms(direct, at(0.2), at(0.8)) > 0.2);
  CHECK_NEAR(crossingHz(direct, at(0.2), at(0.8)), 110.0, 3.0);
  // Noise through the filter instead of the oscillator: a noise voice, not a tone.
  const Buf noise = render({{cell(O::Vco, I::VcfIn), 0.0f}, {cell(O::Noise, I::VcfIn), 1.0f}, {id(P::Cutoff), 2000.0f}});
  CHECK(finite(noise) && rms(noise, at(0.2), at(0.8)) > 0.02);
  // Both into the filter: a cable is a mixer channel, the in sums them.
  const Buf both = render({{cell(O::Noise, I::VcfIn), 0.5f}, {id(P::Cutoff), 6000.0f}});
  CHECK(bright(both, at(0.2), at(0.8)) > bright(render({{id(P::Cutoff), 6000.0f}}), at(0.2), at(0.8)));
  // An amount is a level: halving the cable into the output halves the signal (below the soft clip).
  const Buf full = render({}, 0.6, 0.0f, 0.3f), half = render({{cell(O::Vca, I::Out), 0.5f}}, 0.6, 0.0f, 0.3f);
  CHECK_NEAR(rms(full, at(0.1), at(0.5)) / rms(half, at(0.1), at(0.5)), 2.0, 0.1);
  // Cables into one in add: the oscillator and the VCA each into the output at half level is the full voice
  // plus half of the raw oscillator (a mix).
  const Buf mix = render({{cell(O::Vco, I::Out), 0.5f}}, 0.6, 0.0f, 0.3f);
  CHECK(mix != full && rms(mix, at(0.1), at(0.5)) > rms(full, at(0.1), at(0.5)));
}

void testLfoAsAnOscillatorAndRateCables() {
  // An LFO patched to the output is an oscillator at the LFO's rate, and a rate cable moves it.
  Params lfoOnly = {{cell(O::Vca, I::Out), 0.0f}, {cell(O::Lfo1, I::Out), 0.8f}, {id(P::Lfo1Rate), 40.0f},
                    {id(P::Env1Sustain), 1.0f}, {id(P::Env1Decay), 1.0f}};
  const Buf slow = render(lfoOnly, 1.0);
  CHECK_NEAR(crossingHz(slow, at(0.1), at(0.9)), 40.0, 2.0);
  Params faster = lfoOnly;
  faster.push_back({cell(O::Env1, I::Lfo1Rate), 0.667f});   // ENV 1 (held at 1) +2 octaves: x4
  const Buf fast = render(faster, 1.0);
  CHECK_NEAR(crossingHz(fast, at(0.1), at(0.9)), 160.0, 12.0);
  // LFO 2 can drive LFO 1's rate (a slow sweep of a fast wobble): finite and different.
  const Buf swept = render({{cell(O::Lfo2, I::Lfo1Rate), 0.5f}, {cell(O::Lfo1, I::Cutoff), 0.5f}});
  CHECK(finite(swept) && swept != render({{cell(O::Lfo1, I::Cutoff), 0.5f}}));
}

// ---- cables -----------------------------------------------------------------------------------

void testEnvelopeSweepsTheFilter() {
  // ENV 2 -> cutoff: positive opens then closes (bright early), negative does the opposite.
  const Params base = {{id(P::Cutoff), 400.0f}, {id(P::Env2Decay), 0.3f}, {id(P::Env2Sustain), 0.0f},
                       {id(P::Env1Decay), 1.5f}, {id(P::Env1Sustain), 1.0f}};
  Params up = base, down = base;
  up.push_back({cell(O::Env2, I::Cutoff), 0.8f});
  down.push_back({cell(O::Env2, I::Cutoff), -0.4f});
  const Buf a = render(up, 1.0), b = render(down, 1.0);
  CHECK(bright(a, at(0.02), at(0.12)) > 2.0 * bright(a, at(0.7), at(0.9)));
  CHECK(bright(b, at(0.02), at(0.12)) < 0.95 * bright(b, at(0.7), at(0.9)));
}

void testLfoCables() {
  // LFO 1 -> pitch: vibrato moves the zero-crossing rate over time; a slow LFO -> cutoff makes the
  // brightness swing.
  const Params vib = {{id(P::Lfo1Rate), 4.0f}, {id(P::Lfo1Wave), 0.0f}, {cell(O::Lfo1, I::Pitch), 0.5f}};
  const Buf v = render(vib, 1.0), flat = render({}, 1.0);
  double lo = 1e9, hi = 0;
  for (double t = 0.1; t < 0.9; t += 0.05) {
    const double hz = crossingHz(v, at(t), at(t + 0.05));
    lo = std::fmin(lo, hz);
    hi = std::fmax(hi, hz);
  }
  CHECK(hi > 1.25 * lo);
  const double f0 = crossingHz(flat, at(0.1), at(0.5));
  CHECK(std::fabs(crossingHz(flat, at(0.5), at(0.9)) - f0) < 4.0);

  const Params wob = {{id(P::Lfo1Rate), 2.0f}, {id(P::Lfo1Wave), 0.0f}, {id(P::Cutoff), 700.0f},
                      {cell(O::Lfo1, I::Cutoff), 0.7f}};
  const Buf w = render(wob, 1.0);
  double bl = 1e9, bh = 0;
  for (double t = 0.1; t < 0.9; t += 0.05) {
    const double b = bright(w, at(t), at(t + 0.05));
    bl = std::fmin(bl, b);
    bh = std::fmax(bh, b);
  }
  CHECK(bh > 2.0 * bl);
  // Every LFO wave, at a fast rate, patched hard, stays in range.
  for (int32_t wave = 0; wave <= 4; wave++) {
    CHECK(finite(render({{id(P::Lfo1Wave), float(wave)}, {id(P::Lfo1Rate), 15.0f}, {cell(O::Lfo1, I::Cutoff), 1.0f}, {cell(O::Lfo1, I::Pitch), 1.0f}})));
  }
}

void testAudioRateCables() {
  // VCO -> cutoff and noise -> cutoff change the sound at audio rate (an effect no envelope or LFO has).
  const Params base = {{id(P::Cutoff), 900.0f}};
  const Buf plain = render(base), fm = render({{id(P::Cutoff), 900.0f}, {cell(O::Vco, I::Cutoff), 0.6f}}),
            rough = render({{id(P::Cutoff), 900.0f}, {cell(O::Noise, I::Cutoff), 0.5f}});
  CHECK(finite(fm) && finite(rough));
  CHECK(fm != plain && rough != plain);
  CHECK(std::fabs(bright(fm, at(0.2), at(0.8)) - bright(plain, at(0.2), at(0.8))) > 0.2 * bright(plain, at(0.2), at(0.8)));  // a different tone
  // VCO -> level is amplitude modulation by the oscillator itself (ring-like).
  const Buf am = render({{id(P::Level), 0.3f}, {cell(O::Vco, I::Level), 0.8f}});
  CHECK(finite(am) && am != plain && rms(am, at(0.2), at(0.8)) > 0.01);
  // Noise patched to pitch is a random walk of the pitch: still bounded and finite.
  CHECK(finite(render({{cell(O::Noise, I::Pitch), 0.2f}})));
}

void testFeedback() {
  // Every way to take a signal back upstream stays finite and in range at its strongest, filter ringing.
  struct Case { O o; I i; float a; };
  const Case cases[] = {{O::Vcf, I::Cutoff, 1.0f}, {O::Vcf, I::Cutoff, -1.0f}, {O::Vcf, I::Pitch, 1.0f},
                        {O::Vcf, I::Pitch, -0.3f}, {O::Vco, I::Pitch, 1.0f}, {O::Vcf, I::Resonance, 1.0f},
                        {O::Vcf, I::Drive, 1.0f}, {O::Vco, I::Cutoff, 1.0f}, {O::Vcf, I::Pwm, 1.0f},
                        {O::Vcf, I::VcfIn, 1.0f}, {O::Vcf, I::VcfIn, -1.0f}, {O::Vca, I::VcfIn, 1.0f},
                        {O::Vca, I::Pitch, 1.0f}, {O::Vca, I::Cutoff, -1.0f}, {O::Vcf, I::Lfo1Rate, 1.0f},
                        {O::Vca, I::Lfo2Rate, 1.0f}, {O::Vcf, I::Level, 1.0f}, {O::Vca, I::VcaIn, 1.0f}};
  for (const Case& c : cases) {
    Params p = {{id(P::Cutoff), 1200.0f}, {id(P::Wave), 1.0f}, {id(P::Drive), 1.0f}, {id(P::Resonance), 1.0f},
                {cell(O::Lfo1, I::Cutoff), 0.5f}, {cell(c.o, c.i), c.a}};
    const Buf b = render(p, 1.0);
    CHECK(finite(b) && peak(b) <= 1.0f);
  }
  // The classic: VCF out -> cutoff. Positive feedback adds level and changes the waveshape.
  const Params plain = {{id(P::Cutoff), 1200.0f}, {id(P::Resonance), 0.6f}};
  const Buf ref = render(plain);
  const Buf growl = render({{id(P::Cutoff), 1200.0f}, {id(P::Resonance), 0.6f}, {cell(O::Vcf, I::Cutoff), 0.5f}});
  CHECK(growl != ref && finite(growl));
  CHECK(rms(growl, at(0.2), at(0.8)) > 1.1 * rms(ref, at(0.2), at(0.8)));
  CHECK(std::fabs(bright(growl, at(0.2), at(0.8)) - bright(ref, at(0.2), at(0.8))) > 0.03 * bright(ref, at(0.2), at(0.8)));
  // The filter feeding its own input is a different sound from the plain filter.
  const Buf self = render({{id(P::Cutoff), 1200.0f}, {id(P::Resonance), 0.6f}, {cell(O::Vcf, I::VcfIn), 0.5f}});
  CHECK(self != ref && finite(self));
}

void testExtremesStayFiniteAndBounded() {
  uint32_t seed = 0x1234567u;
  auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return (seed >> 8) / float(1 << 24); };
  for (int trial = 0; trial < 24; trial++) {
    Rig r;
    for (int32_t i = 0; i < kMatrixParamCount; i++) {
      const InstrumentParamInfo* p = instrumentParamInfo(kMx, i);
      float v = rnd();
      v = v < 0.25f ? p->min : v > 0.75f ? p->max : p->min + (p->max - p->min) * rnd();  // hit the edges often
      r.e.setInstrumentParam(1, i, v);
    }
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, std::floor(rnd() * 48.0f) - 12.0f);
    r.go(0.5);
    r.e.instrumentNoteOn(1, 1.0f, 0.0f, 31.0f);
    r.go(0.3);
    CHECK(finite(r.o));
    CHECK(peak(r.o) <= 1.0f);
  }
  // Every cable at +1 and at -1 at once, filter and drive at the limit.
  for (float sign : {1.0f, -1.0f}) {
    Rig r;
    for (int32_t i = id(P::AmountBase); i < kMatrixParamCount; i++) r.e.setInstrumentParam(1, i, sign);
    r.set(P::Resonance, 1.0f);
    r.set(P::Drive, 1.0f);
    r.set(P::Level, 1.0f);
    r.set(P::Env1Sustain, 1.0f);
    r.e.instrumentNoteOn(1, 1.0f);
    r.go(1.0);
    CHECK(finite(r.o) && peak(r.o) <= 1.0f);
  }
}

void testLiveParameterChanges() {
  // Moving an amount (or a control) while the note sounds glides: the largest sample-to-sample step
  // right after a switch stays close to what the voice already does by itself.
  Rig r;
  for (auto& [p, v] : kHeld) r.e.setInstrumentParam(1, p, v);
  r.set(P::Cutoff, 1500.0f);
  r.e.instrumentNoteOn(1, 1.0f);
  r.go(0.3);
  float own = 0;
  for (size_t i = at(0.1) + 1; i < r.o.size(); i++) own = std::fmax(own, std::fabs(r.o[i] - r.o[i - 1]));
  const size_t mark = r.o.size();
  r.patch(O::Env1, I::Level, 0.6f);        // a hard drop in level (1 -> 0.6)
  r.patch(O::Env1, I::Cutoff, -0.6f);      // and a closing filter
  r.go(0.2);
  float jump = 0;
  for (size_t i = mark; i < r.o.size(); i++) jump = std::fmax(jump, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(finite(r.o) && jump < own * 3.0f + 0.2f);
  r.patch(O::Env1, I::Level, 1.0f);
  r.patch(O::Env1, I::Cutoff, 0.0f);      // removing cables mid-note is also clean
  r.go(0.2);
  CHECK(finite(r.o) && peak(r.o, mark, r.o.size()) <= 1.0f);
  // Pulling the audio path itself out mid-note fades rather than clicks.
  const size_t mark2 = r.o.size();
  r.patch(O::Vca, I::Out, 0.0f);
  r.go(0.1);
  float jump2 = 0;
  for (size_t i = mark2; i < r.o.size(); i++) jump2 = std::fmax(jump2, std::fabs(r.o[i] - r.o[i - 1]));
  CHECK(jump2 < own * 3.0f + 0.2f && peak(r.o, at(0.0) + mark2 + 600, r.o.size()) < 1e-3f);
}

void testScheduledNoteStartsOnItsFrame() {
  Rig r;
  r.e.scheduleInstrumentNote(static_cast<int64_t>(0.1 * kRate), 1, 1.0f, 0.0f, 0.0f);
  r.go(0.3);
  CHECK(peak(r.o, 0, at(0.1) - 1) == 0.0f);
  CHECK(peak(r.o, at(0.1), at(0.15)) > 0.01f);
}

void testRoutesThroughBusAndPan() {
  Rig r;
  r.e.instrumentNoteOn(1, 1.0f, -1.0f);
  int64_t f = 0;
  float l = 0, rr = 0;
  for (int i = 0; i < 40; i++) {
    r.e.process(f, kBlock);
    f += kBlock;
    for (int k = 0; k < kBlock; k++) { l = std::fmax(l, std::fabs(r.e.outputChannel(0)[k])); rr = std::fmax(rr, std::fabs(r.e.outputChannel(1)[k])); }
  }
  CHECK(l > 0.01f && rr < 1e-3f);   // hard-left pan reaches only the left channel
}

void testRenderDoesNotAllocate() {
  Engine e; e.init(kRate, 2, 8);
  e.createInstrument(1, kMx, 1);
  e.instrumentNoteOn(1, 1.0f, 0.3f);
  int64_t f = 0;
  e.process(f, kBlock);
  f += kBlock;
  const long before = g_allocs;
  for (int i = 0; i < 1200; i++) {
    if (i == 100) e.instrumentNoteOn(1, 0.7f, 0, 5, 3000);
    if (i == 150) e.setInstrumentParam(1, cell(O::Vcf, I::Cutoff), 0.6f);
    if (i == 200) e.instrumentNoteOn(1, 0.7f, 0, 9);
    if (i == 250) e.setInstrumentParam(1, id(P::Mode), 2.0f);
    if (i == 300) e.setInstrumentParam(1, cell(O::Lfo1, I::Pitch), 0.4f);
    if (i == 400) e.setInstrumentParam(1, id(P::Resonance), 0.9f);
    if (i == 450) e.setInstrumentParam(1, cell(O::Noise, I::Cutoff), -0.5f);
    if (i == 480) e.setInstrumentParam(1, cell(O::Env1, I::Lfo1Rate), 0.5f);
    if (i == 500) e.instrumentNoteOff(1);
    e.process(f, kBlock);
    f += kBlock;
  }
  CHECK(g_allocs == before);
}

}  // namespace

int main() {
  testRegistrationAndCatalogue();
  testModulationLayerCoversTheTable();
  testEveryParameterPropagates();
  testParamValidation();
  testLifecycle();
  testDeterministicAndRetrigger();
  testVelocityAndPitch();
  testWaves();
  testFilterModes();
  testVoiceFilter();
  testDefaultPatchIsTheOrdinaryVoice();
  testEveryCableOfTheDefaultPatchCanBeRemoved();
  testAudioPathCanBeRerouted();
  testLfoAsAnOscillatorAndRateCables();
  testEnvelopeSweepsTheFilter();
  testLfoCables();
  testAudioRateCables();
  testFeedback();
  testExtremesStayFiniteAndBounded();
  testLiveParameterChanges();
  testScheduledNoteStartsOnItsFrame();
  testRoutesThroughBusAndPan();
  testRenderDoesNotAllocate();
  std::printf("matrix_test: %d checks, %d failures\n", webdsp::test::g_count, webdsp::test::g_failures);
  return webdsp::test::g_failures ? 1 : 0;
}
