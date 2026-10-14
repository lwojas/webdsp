#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "engine.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

// The engine's entire ABI. A flat extern "C" surface rather than embind: fewer moving
// parts, a smaller generated glue file, and a boundary explicit enough to read top to
// bottom as the contract it is. See ARCHITECTURE.md, "Where WASM is used".
//
// Single global Engine instance: this module is instantiated once per AudioWorkletProcessor
// instance (one engine per audio graph), so there is nothing to gain from supporting
// multiple engines per WASM instance. All calls below run on the single audio-rendering
// thread — see ARCHITECTURE.md, "Threading model" — so no synchronization is needed.
namespace {
webdsp::Engine g_engine;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE
void webdsp_init(double sampleRate, int32_t outputChannels, int32_t maxVoices) {
  g_engine.init(sampleRate, outputChannels, maxVoices);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_configure_voices(int32_t maxVoices) { g_engine.reconfigureVoices(maxVoices); }

// --- sample memory ---

EMSCRIPTEN_KEEPALIVE
float* webdsp_alloc_channel_buffer(int32_t numFrames) {
  return static_cast<float*>(std::malloc(static_cast<size_t>(numFrames) * sizeof(float)));
}

EMSCRIPTEN_KEEPALIVE
int32_t* webdsp_alloc_ptr_table(int32_t count) {
  return static_cast<int32_t*>(std::malloc(static_cast<size_t>(count) * sizeof(int32_t)));
}

EMSCRIPTEN_KEEPALIVE
void webdsp_free(void* ptr) { std::free(ptr); }

EMSCRIPTEN_KEEPALIVE
void webdsp_commit_sample(int32_t sampleId, int32_t numChannels, int32_t length,
                           int32_t sampleRate, int32_t* channelPtrTable) {
  g_engine.commitSample(sampleId, numChannels, length, sampleRate, channelPtrTable);
  std::free(channelPtrTable);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_remove_sample(int32_t sampleId) { g_engine.removeSample(sampleId); }

EMSCRIPTEN_KEEPALIVE
void webdsp_set_sample_param(int32_t sampleId, int32_t param, float value) {
  g_engine.setSampleParam(sampleId, param, value);
}

// --- voice control ---

EMSCRIPTEN_KEEPALIVE
void webdsp_trigger(int32_t voiceId, int32_t sampleId, int32_t busId, float gain, float rate,
                     int32_t startFrame, int32_t endFrame, int32_t loop, int32_t reverse,
                     int32_t durationFrames, float pan) {
  g_engine.trigger(voiceId, sampleId, busId, gain, rate, startFrame, endFrame, loop != 0,
                    reverse != 0, durationFrames, pan);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_release(int32_t voiceId) { g_engine.release(voiceId); }

EMSCRIPTEN_KEEPALIVE
void webdsp_stop(int32_t voiceId) { g_engine.stop(voiceId); }

EMSCRIPTEN_KEEPALIVE
void webdsp_set_voice_param(int32_t voiceId, int32_t param, float value) {
  g_engine.setVoiceParam(voiceId, param, value);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_set_bus_param(int32_t busId, int32_t param, float value) {
  g_engine.setBusParam(busId, param, value);
}

// --- FX chains (see ARCHITECTURE.md, "FX chains") ---
// Effect ids are chosen by the caller (> 0). Mutators return 1 on success, 0 if rejected.

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_add(int32_t busId, int32_t effectId, int32_t kind, int32_t index) {
  return g_engine.addEffect(busId, effectId, kind, index) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_remove(int32_t busId, int32_t effectId) {
  return g_engine.removeEffect(busId, effectId) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_move(int32_t busId, int32_t effectId, int32_t index) {
  return g_engine.moveEffect(busId, effectId, index) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_set_enabled(int32_t busId, int32_t effectId, int32_t enabled) {
  return g_engine.setEffectEnabled(busId, effectId, enabled != 0) ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_set_param(int32_t busId, int32_t effectId, int32_t param, float value) {
  return g_engine.setEffectParam(busId, effectId, param, value) ? 1 : 0;
}

// Inspection: chain length, and the id / kind / enabled flag at each position (0 / -1 / 0 when
// out of range).
EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_count(int32_t busId) {
  const webdsp::FxChain* c = g_engine.effectChain(busId);
  return c ? c->count() : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_id_at(int32_t busId, int32_t index) {
  const webdsp::FxChain* c = g_engine.effectChain(busId);
  return c ? c->idAt(index) : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_kind_at(int32_t busId, int32_t index) {
  const webdsp::FxChain* c = g_engine.effectChain(busId);
  return c ? c->kindAt(index) : -1;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_fx_enabled_at(int32_t busId, int32_t index) {
  const webdsp::FxChain* c = g_engine.effectChain(busId);
  return c && c->enabledAt(index) ? 1 : 0;
}

// --- scheduling ---

EMSCRIPTEN_KEEPALIVE
void webdsp_schedule_event(double atFrame, int32_t voiceId, int32_t sampleId, int32_t busId,
                            float gain, float rate, int32_t startFrame, int32_t endFrame,
                            int32_t loop, int32_t reverse, int32_t durationFrames, float pan) {
  g_engine.scheduleEvent(static_cast<int64_t>(atFrame), voiceId, sampleId, busId, gain, rate,
                          startFrame, endFrame, loop != 0, reverse != 0, durationFrames, pan);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_cancel_scheduled(double fromFrame) {
  g_engine.cancelScheduled(static_cast<int64_t>(fromFrame));
}

// --- capture / resample ---

EMSCRIPTEN_KEEPALIVE
void webdsp_start_capture(int32_t captureId, int32_t busId) {
  g_engine.startCapture(captureId, busId);
}

EMSCRIPTEN_KEEPALIVE
void webdsp_stop_capture(int32_t captureId) { g_engine.stopCapture(captureId); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_arm_capture(int32_t captureId, int32_t busId, double startFrame, double stopFrame) {
  return g_engine.armCapture(captureId, busId, static_cast<int64_t>(startFrame),
                              static_cast<int64_t>(stopFrame))
             ? 1
             : 0;
}

EMSCRIPTEN_KEEPALIVE
double webdsp_max_capture_seconds() { return webdsp::Engine::maxCaptureSeconds(); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_finished_capture_count() {
  return static_cast<int32_t>(g_engine.finishedCaptures().size());
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_finished_capture_id(int32_t index) { return g_engine.finishedCaptures()[index]; }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_capture_length(int32_t captureId) {
  const webdsp::Capture* c = g_engine.capture(captureId);
  return c ? c->length() : 0;
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_capture_channels(int32_t captureId) {
  const webdsp::Capture* c = g_engine.capture(captureId);
  return c ? c->channels() : 0;
}

EMSCRIPTEN_KEEPALIVE
const float* webdsp_capture_channel_ptr(int32_t captureId, int32_t channel) {
  const webdsp::Capture* c = g_engine.capture(captureId);
  return c ? c->channelData(channel) : nullptr;
}

EMSCRIPTEN_KEEPALIVE
void webdsp_discard_capture(int32_t captureId) { g_engine.discardCapture(captureId); }

// --- realtime render (called once per AudioWorklet render quantum) ---

EMSCRIPTEN_KEEPALIVE
void webdsp_process(double blockStartFrame, int32_t numFrames) {
  g_engine.process(static_cast<int64_t>(blockStartFrame), numFrames);
}

EMSCRIPTEN_KEEPALIVE
float* webdsp_output_channel_ptr(int32_t channel) { return g_engine.outputChannel(channel); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_ended_voice_count() {
  return static_cast<int32_t>(g_engine.endedVoices().size());
}

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_ended_voice_id(int32_t index) { return g_engine.endedVoices()[index]; }

// --- diagnostics / capabilities ---

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_active_voice_count() { return g_engine.activeVoiceCount(); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_loaded_sample_count() { return g_engine.loadedSampleCount(); }

EMSCRIPTEN_KEEPALIVE
double webdsp_sample_memory_bytes() { return static_cast<double>(g_engine.sampleMemoryBytes()); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_max_voices() { return g_engine.maxVoices(); }

EMSCRIPTEN_KEEPALIVE
int32_t webdsp_output_channels() { return g_engine.outputChannels(); }

}  // extern "C"
