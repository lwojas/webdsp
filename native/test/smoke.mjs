// Exercises the compiled WASM ABI directly under Node — no browser required. This is the
// one non-native-toolchain test that actually loads the real compiled artifact (everything
// else in native/test/ is a pure C++ unit test, and everything in /test is TS logic that
// never touches WASM); it exists to catch ABI-level regressions (a renamed export, a wrong
// argument order) that neither of those layers would ever see.
import assert from "node:assert/strict";
import createEngineModule from "../build/engine.node.mjs";

const SAMPLE_RATE = 48000;

function makeSine(freqHz, seconds, sampleRate) {
  const n = Math.floor(seconds * sampleRate);
  const data = new Float32Array(n);
  for (let i = 0; i < n; i++) data[i] = Math.sin((2 * Math.PI * freqHz * i) / sampleRate) * 0.5;
  return data;
}

function loadSample(Module, sampleId, channels) {
  const length = channels[0].length;
  const channelPtrs = channels.map((data) => {
    const ptr = Module._webdsp_alloc_channel_buffer(length);
    Module.HEAPF32.set(data, ptr >> 2);
    return ptr;
  });
  const tablePtr = Module._webdsp_alloc_ptr_table(channelPtrs.length);
  Module.HEAP32.set(channelPtrs, tablePtr >> 2);
  Module._webdsp_commit_sample(sampleId, channelPtrs.length, length, SAMPLE_RATE, tablePtr);
}

function readOutputRMS(Module, numFrames, channel) {
  const ptr = Module._webdsp_output_channel_ptr(channel);
  const view = Module.HEAPF32.subarray(ptr >> 2, (ptr >> 2) + numFrames);
  let sumSq = 0;
  for (const v of view) sumSq += v * v;
  return Math.sqrt(sumSq / numFrames);
}

async function main() {
  const Module = await createEngineModule();

  Module._webdsp_init(SAMPLE_RATE, 2, 16);
  assert.equal(Module._webdsp_max_voices(), 16, "maxVoices should match init");
  assert.equal(Module._webdsp_output_channels(), 2);
  assert.equal(Module._webdsp_loaded_sample_count(), 0);

  const tone = makeSine(440, 0.5, SAMPLE_RATE);
  loadSample(Module, 1, [tone, tone]);
  assert.equal(Module._webdsp_loaded_sample_count(), 1);
  assert.ok(Module._webdsp_sample_memory_bytes() > 0);

  // Silence before any trigger.
  Module._webdsp_process(0, 128);
  assert.equal(readOutputRMS(Module, 128, 0), 0, "output must be silent with no active voices");

  // Trigger a voice and confirm it produces sound and is tracked as active.
  Module._webdsp_trigger(/* voiceId */ 1, /* sampleId */ 1, /* busId */ 0, /* gain */ 1.0,
    /* rate */ 1.0, /* start */ 0, /* end */ -1, /* loop */ 0, /* reverse */ 0, /* durationFrames */ -1);
  assert.equal(Module._webdsp_active_voice_count(), 1);
  Module._webdsp_process(128, 128);
  const rms = readOutputRMS(Module, 128, 0);
  assert.ok(rms > 0.1, `expected audible output, got rms=${rms}`);

  // Polyphony: trigger several more voices simultaneously, all distinct handles.
  for (let v = 2; v <= 5; v++) {
    Module._webdsp_trigger(v, 1, 0, 0.8, 1.0, 0, -1, 0, 0, -1);
  }
  assert.equal(Module._webdsp_active_voice_count(), 5);
  Module._webdsp_process(256, 128);

  // Sample-accurate mid-block scheduling: schedule an event partway through the next block
  // and confirm it does not sound during the silent lead-in.
  Module._webdsp_stop(1);
  for (let v = 2; v <= 5; v++) Module._webdsp_stop(v);
  Module._webdsp_process(384, 128);
  assert.equal(Module._webdsp_active_voice_count(), 0);

  Module._webdsp_schedule_event(/* atFrame */ 512 + 64, /* voiceId */ 10, 1, 0, 1.0, 1.0, 0, -1, 0, 0, -1);
  Module._webdsp_process(512, 128);
  const leadIn = Module.HEAPF32.subarray(
    Module._webdsp_output_channel_ptr(0) >> 2,
    (Module._webdsp_output_channel_ptr(0) >> 2) + 64,
  );
  for (const v of leadIn) assert.equal(v, 0, "no sound before the scheduled frame");
  const tail = Module.HEAPF32.subarray(
    (Module._webdsp_output_channel_ptr(0) >> 2) + 64,
    (Module._webdsp_output_channel_ptr(0) >> 2) + 128,
  );
  const tailHasSignal = tail.some((v) => Math.abs(v) > 0.01);
  assert.ok(tailHasSignal, "expected sound starting exactly at the scheduled frame");

  // Voice release/stop and reclaiming.
  Module._webdsp_release(10);
  for (let i = 0; i < 20; i++) Module._webdsp_process(640 + i * 128, 128);
  assert.equal(Module._webdsp_active_voice_count(), 0, "released voice should fade out and free itself");

  // Capture / resample proof of concept.
  Module._webdsp_start_capture(1, 0);
  Module._webdsp_trigger(20, 1, 0, 1.0, 1.0, 0, -1, 0, 0, -1);
  for (let i = 0; i < 10; i++) Module._webdsp_process(3200 + i * 128, 128);
  Module._webdsp_stop_capture(1);
  const capLen = Module._webdsp_capture_length(1);
  assert.ok(capLen > 0, "capture should have recorded frames");
  const capPtr = Module._webdsp_capture_channel_ptr(1, 0);
  const capData = Module.HEAPF32.subarray(capPtr >> 2, (capPtr >> 2) + capLen);
  assert.ok(capData.some((v) => Math.abs(v) > 0.01), "captured audio should be non-silent");
  Module._webdsp_discard_capture(1);
  Module._webdsp_stop(20); // capture's own trigger voice, otherwise still playing below
  Module._webdsp_process(4472, 128);
  assert.equal(Module._webdsp_active_voice_count(), 0);

  // Note duration: a voice triggered with durationFrames should auto-release (short linear
  // fade-out) that many frames after playback starts, rather than playing indefinitely.
  // kReleaseSeconds is 0.01s (480 frames @ 48kHz), so give it a full block of runway.
  Module._webdsp_trigger(/* voiceId */ 30, 1, 0, 1.0, 1.0, 0, -1, 0, 0, /* durationFrames */ 64);
  assert.equal(Module._webdsp_active_voice_count(), 1);
  Module._webdsp_process(4600, 128); // frames 64..127 should be releasing/silent by block end
  Module._webdsp_process(4728, 512);
  assert.equal(
    Module._webdsp_active_voice_count(),
    0,
    "voice with durationFrames should auto-release and free itself without an explicit release()/stop()",
  );

  // Master filter: NodeParam::FilterCutoff/FilterMode (ids 4/6) on the master bus (busId 0)
  // should audibly attenuate a high-frequency tone once switched to a low cutoff lowpass,
  // demonstrating a public, generic master-processing node (not per-voice DSP).
  const highTone = makeSine(8000, 0.2, SAMPLE_RATE);
  loadSample(Module, 2, [highTone, highTone]);
  Module._webdsp_trigger(40, 2, 0, 1.0, 1.0, 0, -1, /* loop */ 1, 0, -1);
  Module._webdsp_process(5300, 128);
  const unfiltered = readOutputRMS(Module, 128, 0);
  assert.ok(unfiltered > 0.1, "expected audible high-frequency tone before filtering");

  const NODE_PARAM_FILTER_CUTOFF = 4;
  const NODE_PARAM_FILTER_MODE = 6;
  Module._webdsp_set_bus_param(0, NODE_PARAM_FILTER_MODE, 0); // LowPass
  Module._webdsp_set_bus_param(0, NODE_PARAM_FILTER_CUTOFF, 300); // well below the 8kHz tone
  for (let i = 0; i < 10; i++) Module._webdsp_process(5428 + i * 128, 128); // let the filter settle
  const filtered = readOutputRMS(Module, 128, 0);
  assert.ok(
    filtered < unfiltered * 0.5,
    `expected the master lowpass to attenuate an 8kHz tone, got unfiltered=${unfiltered} filtered=${filtered}`,
  );
  Module._webdsp_stop(40);

  // Unload frees memory bookkeeping.
  Module._webdsp_remove_sample(1);
  Module._webdsp_remove_sample(2);
  assert.equal(Module._webdsp_loaded_sample_count(), 0);

  console.log("native ABI smoke test passed");
}

main().catch((err) => {
  console.error(err);
  process.exit(1);
});
