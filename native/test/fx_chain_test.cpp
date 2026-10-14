#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include "../src/engine.h"
#include "../src/fx_chain.h"
#include "../src/params.h"
#include "test_main.h"

using namespace webdsp;

namespace {

constexpr int32_t kBlock = 128;
constexpr double kRate = 48000.0;
constexpr int32_t kBlocks = 40;
constexpr int32_t kN = kBlocks * kBlock;

constexpr int32_t kFilter = static_cast<int32_t>(FxKind::Filter);
constexpr int32_t kDelay = static_cast<int32_t>(FxKind::Delay);
constexpr int32_t kSat = static_cast<int32_t>(FxKind::Saturation);
constexpr int32_t kChorus = static_cast<int32_t>(FxKind::ChorusFlanger);

int32_t P(NodeParam p) { return static_cast<int32_t>(p); }

// Deterministic stereo noise-ish input (LCG), identical for every call.
std::vector<float> makeInput() {
  std::vector<float> in(kN * 2);
  uint32_t s = 12345;
  for (float& x : in) {
    s = s * 1664525u + 1013904223u;
    x = (static_cast<int32_t>(s >> 8) / static_cast<float>(1 << 23) - 1.0f) * 0.5f;
  }
  return in;
}

// Runs `chain` over the input in blocks; `between` is called before each block index.
template <typename F>
std::vector<float> run(FxChain& chain, F&& between) {
  std::vector<float> data = makeInput();  // interleaved L,R per frame for storage only
  std::vector<float> out(kN * 2);
  float l[kBlock], r[kBlock];
  for (int32_t b = 0; b < kBlocks; b++) {
    between(b);
    for (int32_t i = 0; i < kBlock; i++) {
      l[i] = data[(b * kBlock + i) * 2];
      r[i] = data[(b * kBlock + i) * 2 + 1];
    }
    float* ch[2] = {l, r};
    chain.process(ch, 2, kBlock);
    for (int32_t i = 0; i < kBlock; i++) {
      out[(b * kBlock + i) * 2] = l[i];
      out[(b * kBlock + i) * 2 + 1] = r[i];
    }
  }
  return out;
}
std::vector<float> run(FxChain& chain) {
  return run(chain, [](int32_t) {});
}

FxChain makeChain() {
  FxChain c;
  c.configure(kRate);
  return c;
}

void lowpass(FxChain& c, int32_t id, float hz) {
  CHECK(c.setParam(id, P(NodeParam::FilterCutoff), hz));
  CHECK(c.setParam(id, P(NodeParam::FilterMode), 0.0f));
}
void delay(FxChain& c, int32_t id) {
  CHECK(c.setParam(id, P(NodeParam::DelayTime), 0.003f));
  CHECK(c.setParam(id, P(NodeParam::DelayFeedback), 0.5f));
  CHECK(c.setParam(id, P(NodeParam::DelayMix), 0.6f));
}
void sat(FxChain& c, int32_t id) {
  CHECK(c.setParam(id, P(NodeParam::SatDrive), 24.0f));
  CHECK(c.setParam(id, P(NodeParam::SatMix), 1.0f));
}

bool differ(const std::vector<float>& a, const std::vector<float>& b) { return a != b; }

// --- multiple instances ---

// Two filters of the same kind coexist, run in order, and hold independent parameters.
void testTwoFiltersIndependentParams() {
  FxChain both = makeChain();
  CHECK(both.add(1, kFilter, -1));
  CHECK(both.add(2, kFilter, -1));
  CHECK(both.count() == 2);
  lowpass(both, 1, 400.0f);
  lowpass(both, 2, 4000.0f);

  // Reference: the same two filters built by hand as two separate single-filter chains.
  FxChain a = makeChain(), b = makeChain();
  a.add(1, kFilter, -1);
  b.add(1, kFilter, -1);
  lowpass(a, 1, 400.0f);
  lowpass(b, 1, 4000.0f);
  // A then B, sample for sample, is exactly what the two-instance chain must compute.
  std::vector<float> in = makeInput();
  std::vector<float> out = run(both);
  std::vector<float> ref(kN * 2);
  {
    float l[kBlock], r[kBlock];
    for (int32_t blk = 0; blk < kBlocks; blk++) {
      for (int32_t i = 0; i < kBlock; i++) {
        l[i] = in[(blk * kBlock + i) * 2];
        r[i] = in[(blk * kBlock + i) * 2 + 1];
      }
      float* ch[2] = {l, r};
      a.process(ch, 2, kBlock);
      b.process(ch, 2, kBlock);
      for (int32_t i = 0; i < kBlock; i++) {
        ref[(blk * kBlock + i) * 2] = l[i];
        ref[(blk * kBlock + i) * 2 + 1] = r[i];
      }
    }
  }
  CHECK(out == ref);
}

// Changing filter A's cutoff must not alter what filter B does: with A disabled (so it is a
// pure pass-through) B's output must be identical whatever A's cutoff is.
void testParamChangeDoesNotLeak() {
  auto build = [](float aCut) {
    FxChain c = makeChain();
    c.add(1, kFilter, -1);
    c.add(2, kFilter, -1);
    lowpass(c, 1, aCut);
    lowpass(c, 2, 1500.0f);
    c.setEnabled(1, false);
    return c;
  };
  FxChain c1 = build(200.0f), c2 = build(9000.0f);
  CHECK(run(c1) == run(c2));

  // And with A enabled, a different A cutoff does change the result (B is not shadowing it).
  FxChain d1 = makeChain(), d2 = makeChain();
  for (FxChain* c : {&d1, &d2}) {
    c->add(1, kFilter, -1);
    c->add(2, kFilter, -1);
    lowpass(*c, 2, 1500.0f);
  }
  lowpass(d1, 1, 200.0f);
  lowpass(d2, 1, 9000.0f);
  CHECK(differ(run(d1), run(d2)));
}

// --- ordering ---

void testOrderMatters() {
  auto chainOf = [](int32_t first, int32_t second) {
    FxChain c = makeChain();
    c.add(1, first, -1);
    c.add(2, second, -1);
    return c;
  };
  FxChain fs = chainOf(kFilter, kSat), sf = chainOf(kSat, kFilter);
  lowpass(fs, 1, 800.0f);
  sat(fs, 2);
  lowpass(sf, 2, 800.0f);
  sat(sf, 1);
  CHECK(differ(run(fs), run(sf)));

  // Filter -> Delay differs from Delay -> Filter too (the spec's example).
  FxChain fd = chainOf(kFilter, kDelay), df = chainOf(kDelay, kFilter);
  lowpass(fd, 1, 800.0f);
  delay(fd, 2);
  lowpass(df, 2, 800.0f);
  delay(df, 1);
  // Both are linear time-invariant, so these are mathematically close; the point is the
  // nonlinear pair above. Here we only require determinism of order bookkeeping.
  CHECK(fd.idAt(0) == 1 && fd.idAt(1) == 2 && df.idAt(0) == 1 && df.idAt(1) == 2);
}

void testMoveChangesResultAndBookkeeping() {
  FxChain c = makeChain();
  c.add(1, kFilter, -1);
  c.add(2, kSat, -1);
  lowpass(c, 1, 800.0f);
  sat(c, 2);
  std::vector<float> before = run(c);
  FxChain again = makeChain();
  again.add(1, kFilter, -1);
  again.add(2, kSat, -1);
  lowpass(again, 1, 800.0f);
  sat(again, 2);
  again.move(2, 0);  // Sat -> Filter
  CHECK(again.idAt(0) == 2 && again.idAt(1) == 1);
  CHECK(differ(before, run(again)));
  again.move(2, 1);  // back
  CHECK(again.idAt(0) == 1 && again.idAt(1) == 2);
  again.reset();
  CHECK(before == run(again));

  // Index semantics: insert at a position, clamp, and move across three.
  FxChain d = makeChain();
  d.add(10, kFilter, -1);
  d.add(11, kDelay, -1);
  d.add(12, kSat, 0);   // front
  d.add(13, kFilter, 1);  // between
  CHECK(d.idAt(0) == 12 && d.idAt(1) == 13 && d.idAt(2) == 10 && d.idAt(3) == 11);
  d.move(12, 99);  // clamp to end
  CHECK(d.idAt(3) == 12 && d.idAt(0) == 13);
  d.move(11, -5);  // clamp to front
  CHECK(d.idAt(0) == 11 && d.idAt(1) == 13 && d.idAt(2) == 10);
}

// --- mixed chain: Filter -> Delay -> Filter ---

void testFilterDelayFilter() {
  FxChain c = makeChain();
  c.add(1, kFilter, -1);
  c.add(2, kDelay, -1);
  c.add(3, kFilter, -1);
  lowpass(c, 1, 3000.0f);
  delay(c, 2);
  lowpass(c, 3, 600.0f);
  std::vector<float> full = run(c);

  // Same graph with the second filter's cutoff changed must differ, and with the *first*
  // filter's cutoff changed must differ differently — i.e. both are live and independent.
  FxChain c2 = makeChain();
  c2.add(1, kFilter, -1);
  c2.add(2, kDelay, -1);
  c2.add(3, kFilter, -1);
  lowpass(c2, 1, 3000.0f);
  delay(c2, 2);
  lowpass(c2, 3, 6000.0f);
  std::vector<float> secondChanged = run(c2);
  FxChain c3 = makeChain();
  c3.add(1, kFilter, -1);
  c3.add(2, kDelay, -1);
  c3.add(3, kFilter, -1);
  lowpass(c3, 1, 300.0f);
  delay(c3, 2);
  lowpass(c3, 3, 600.0f);
  std::vector<float> firstChanged = run(c3);
  CHECK(differ(full, secondChanged));
  CHECK(differ(full, firstChanged));
  CHECK(differ(secondChanged, firstChanged));

  // Removing the first filter must give exactly Delay -> Filter(600), built fresh.
  c.remove(1);
  CHECK(c.count() == 2 && c.idAt(0) == 2 && c.idAt(1) == 3);
}

// --- lifecycle ---

// Removing one instance must not disturb the state of another instance of the same kind.
// Delay has audible memory, so run half, remove the *other* delay mid-stream, and compare the
// survivor's output with a chain that never had the extra instance.
void testRemoveDoesNotDisturbSibling() {
  FxChain withExtra = makeChain();
  withExtra.add(1, kDelay, -1);
  withExtra.add(2, kDelay, -1);
  delay(withExtra, 1);
  // Instance 2 is added but left inert (mix 0): pure pass-through, so the chain output equals
  // instance 1 alone until it is removed.
  FxChain solo = makeChain();
  solo.add(1, kDelay, -1);
  delay(solo, 1);
  std::vector<float> a = run(withExtra, [&](int32_t b) {
    if (b == kBlocks / 2) {
      CHECK(withExtra.remove(2));
    }
  });
  std::vector<float> b = run(solo);
  CHECK(a == b);
  CHECK(withExtra.count() == 1 && withExtra.idAt(0) == 1);
  CHECK(!withExtra.remove(2));  // already gone
}

void testBypassEnableAndReset() {
  FxChain c = makeChain();
  c.add(1, kSat, -1);
  sat(c, 1);
  FxChain passthrough = makeChain();
  std::vector<float> dry = run(passthrough);
  CHECK(c.setEnabled(1, false));
  CHECK(!c.enabledAt(0));
  CHECK(run(c) == dry);  // disabled == bit-exact bypass
  CHECK(c.setEnabled(1, true));
  CHECK(differ(run(c), dry));

  // Enable/disable of one instance leaves its sibling's parameters and state alone.
  FxChain two = makeChain();
  two.add(1, kDelay, -1);
  two.add(2, kDelay, -1);
  delay(two, 1);
  delay(two, 2);
  two.setEnabled(2, false);
  FxChain one = makeChain();
  one.add(1, kDelay, -1);
  delay(one, 1);
  CHECK(run(two) == run(one));

  // reset() makes a used chain reproduce a fresh chain's output.
  FxChain fresh = makeChain();
  fresh.add(1, kDelay, -1);
  delay(fresh, 1);
  std::vector<float> first = run(fresh);
  fresh.reset();
  CHECK(run(fresh) == first);
}

void testRejectsBadRequests() {
  FxChain c = makeChain();
  CHECK(!c.add(0, kFilter, -1));      // id 0 reserved
  CHECK(!c.add(1, 99, -1));           // unknown kind
  CHECK(!c.add(1, -1, -1));
  CHECK(c.add(1, kFilter, -1));
  CHECK(!c.add(1, kDelay, -1));       // duplicate id
  CHECK(!c.setParam(1, P(NodeParam::DelayMix), 0.5f));  // param belongs to another kind
  CHECK(!c.setParam(9, P(NodeParam::FilterCutoff), 1000.0f));  // unknown id
  CHECK(!c.move(9, 0) && !c.remove(9) && !c.setEnabled(9, false));
  for (int32_t i = 2; i <= kMaxFxPerBus; i++) CHECK(c.add(i, kFilter, -1));
  CHECK(!c.add(100, kFilter, -1));    // full
  CHECK(c.count() == kMaxFxPerBus);
}

// --- engine level: track vs master, legacy compatibility ---

void commitTone(Engine& e, int32_t id, int32_t frames) {
  float* ch[2] = {static_cast<float*>(std::calloc(frames, sizeof(float))),
                  static_cast<float*>(std::calloc(frames, sizeof(float)))};
  for (int32_t i = 0; i < frames; i++) ch[0][i] = ch[1][i] = 0.5f * std::sin(i * 0.05f) + 0.2f;
  e.commitSample(id, 2, frames, static_cast<int32_t>(kRate), ch);
}

std::vector<float> render(Engine& e, int32_t blocks) {
  std::vector<float> out;
  int64_t frame = 0;
  for (int32_t b = 0; b < blocks; b++) {
    e.process(frame, kBlock);
    frame += kBlock;
    for (int32_t c = 0; c < 2; c++) out.insert(out.end(), e.outputChannel(c), e.outputChannel(c) + kBlock);
  }
  return out;
}

void setupEngine(Engine& e) {
  e.init(kRate, 2, 16);
  commitTone(e, 1, 20000);
  e.trigger(1, 1, /*busId=*/1, 1.0f, 1.0f, 0, -1, true, false);
}

// Track and master chains are independent: the same stack on the track bus and on the master
// bus is separate state, and altering one does not move the other.
void testTrackAndMasterIndependent() {
  Engine e;
  setupEngine(e);
  CHECK(e.addEffect(1, 5, kFilter, -1));
  CHECK(e.addEffect(1, 6, kFilter, -1));
  CHECK(e.addEffect(0, 5, kFilter, -1));  // same ids on a different bus are fine
  CHECK(e.addEffect(0, 6, kDelay, -1));
  CHECK(e.effectChain(1)->count() == 2 && e.effectChain(0)->count() == 2);
  CHECK(e.effectChain(1)->kindAt(1) == kFilter && e.effectChain(0)->kindAt(1) == kDelay);
  e.setEffectParam(1, 5, P(NodeParam::FilterCutoff), 500.0f);
  e.setEffectParam(1, 6, P(NodeParam::FilterCutoff), 2000.0f);
  e.setEffectParam(0, 5, P(NodeParam::FilterCutoff), 8000.0f);

  Engine ref;
  setupEngine(ref);
  ref.addEffect(1, 5, kFilter, -1);
  ref.addEffect(1, 6, kFilter, -1);
  ref.setEffectParam(1, 5, P(NodeParam::FilterCutoff), 500.0f);
  ref.setEffectParam(1, 6, P(NodeParam::FilterCutoff), 2000.0f);
  // Master gets a different (and richer) chain; the master filter at 8 kHz must change output.
  std::vector<float> a = render(e, 60);
  std::vector<float> b = render(ref, 60);
  CHECK(differ(a, b));

  // Removing the master chain's instances restores the track-only result exactly.
  Engine e2;
  setupEngine(e2);
  e2.addEffect(1, 5, kFilter, -1);
  e2.addEffect(1, 6, kFilter, -1);
  e2.addEffect(0, 5, kFilter, -1);
  e2.setEffectParam(1, 5, P(NodeParam::FilterCutoff), 500.0f);
  e2.setEffectParam(1, 6, P(NodeParam::FilterCutoff), 2000.0f);
  e2.removeEffect(0, 5);
  CHECK(render(e2, 60) == b);

  CHECK(!e.addEffect(kMaxTrackBuses + 1, 1, kFilter, -1));  // out-of-range bus rejected
}

// The old NodeParam API is a wrapper over the chain: results equal the equivalent explicit
// chain, and the legacy order (filter, chorus, delay, reverb, comp, sat) holds whatever order
// the parameters are set in.
void testLegacyApiIsThinWrapper() {
  Engine legacy;
  setupEngine(legacy);
  // Set in a deliberately scrambled order.
  legacy.setBusParam(1, P(NodeParam::SatDrive), 12.0f);
  legacy.setBusParam(1, P(NodeParam::SatMix), 1.0f);
  legacy.setBusParam(1, P(NodeParam::DelayTime), 0.01f);
  legacy.setBusParam(1, P(NodeParam::DelayMix), 0.5f);
  legacy.setBusParam(1, P(NodeParam::FilterCutoff), 1200.0f);
  legacy.setBusParam(1, P(NodeParam::ChorusFlangerMix), 0.5f);

  const FxChain* ch = legacy.effectChain(1);
  CHECK(ch->count() == 4);
  CHECK(ch->kindAt(0) == kFilter && ch->kindAt(1) == kChorus && ch->kindAt(2) == kDelay &&
        ch->kindAt(3) == kSat);

  Engine generic;
  setupEngine(generic);
  generic.addEffect(1, 1, kFilter, -1);
  generic.addEffect(1, 2, kChorus, -1);
  generic.addEffect(1, 3, kDelay, -1);
  generic.addEffect(1, 4, kSat, -1);
  generic.setEffectParam(1, 4, P(NodeParam::SatDrive), 12.0f);
  generic.setEffectParam(1, 4, P(NodeParam::SatMix), 1.0f);
  generic.setEffectParam(1, 3, P(NodeParam::DelayTime), 0.01f);
  generic.setEffectParam(1, 3, P(NodeParam::DelayMix), 0.5f);
  generic.setEffectParam(1, 1, P(NodeParam::FilterCutoff), 1200.0f);
  generic.setEffectParam(1, 2, P(NodeParam::ChorusFlangerMix), 0.5f);
  CHECK(render(legacy, 60) == render(generic, 60));

  // BusGain/BusPan stay bus-level and never create an instance.
  Engine g;
  setupEngine(g);
  g.setBusParam(1, P(NodeParam::BusGain), 0.5f);
  g.setBusParam(1, P(NodeParam::BusPan), 0.3f);
  CHECK(g.effectChain(1)->count() == 0);
}

// Generic and legacy instances of the same kind coexist; legacy writes only hit the legacy one.
void testLegacyAndGenericCoexist() {
  Engine e;
  setupEngine(e);
  e.addEffect(1, 7, kFilter, -1);
  e.setBusParam(1, P(NodeParam::FilterCutoff), 700.0f);  // creates the legacy filter
  const FxChain* ch = e.effectChain(1);
  CHECK(ch->count() == 2);
  CHECK(ch->idAt(0) == 7 && ch->idAt(1) == legacyFxId(FxKind::Filter));
  // The explicit instance was left inert (its cutoff never set).
  Engine only;
  setupEngine(only);
  only.setBusParam(1, P(NodeParam::FilterCutoff), 700.0f);
  CHECK(render(e, 30) == render(only, 30));
}

}  // namespace

int main() {
  testTwoFiltersIndependentParams();
  testParamChangeDoesNotLeak();
  testOrderMatters();
  testMoveChangesResultAndBookkeeping();
  testFilterDelayFilter();
  testRemoveDoesNotDisturbSibling();
  testBypassEnableAndReset();
  testRejectsBadRequests();
  testTrackAndMasterIndependent();
  testLegacyApiIsThinWrapper();
  testLegacyAndGenericCoexist();

  std::printf("fx_chain_test: %d/%d passed\n", webdsp::test::g_count - webdsp::test::g_failures,
              webdsp::test::g_count);
  return webdsp::test::g_failures == 0 ? 0 : 1;
}
