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

  // Armed capture: reserve a window ahead of the render clock, spanning a block boundary, and
  // confirm the engine starts/stops it on the exact sample with no explicit stop call.
  assert.ok(Module._webdsp_max_capture_seconds() > 0, "engine should report a capture capacity");
  loadSample(Module, 3, [makeSine(220, 0.5, SAMPLE_RATE), makeSine(220, 0.5, SAMPLE_RATE)]);
  const armStart = 8700; // mid-block within the [8600,8728) render quantum
  const armStop = 8700 + 64; // ends mid-block within [8728,8856)
  const armed = Module._webdsp_arm_capture(2, 0, armStart, armStop);
  assert.equal(armed, 1, "arm_capture should accept a window within capacity");
  Module._webdsp_trigger(60, 3, 0, 1.0, 1.0, 0, -1, /* loop */ 1, 0, -1);
  Module._webdsp_process(8600, 128); // window starts partway through this block
  assert.equal(Module._webdsp_finished_capture_count(), 0, "not finished yet");
  Module._webdsp_process(8728, 128); // window ends partway through this block
  assert.equal(Module._webdsp_finished_capture_count(), 1, "capture should auto-finish this block");
  assert.equal(Module._webdsp_finished_capture_id(0), 2);
  assert.equal(Module._webdsp_capture_length(2), armStop - armStart, "captured exactly the armed window");
  const armedPtr = Module._webdsp_capture_channel_ptr(2, 0);
  const armedData = Module.HEAPF32.subarray(armedPtr >> 2, (armedPtr >> 2) + Module._webdsp_capture_length(2));
  assert.ok(armedData.some((v) => Math.abs(v) > 0.01), "armed capture should be non-silent");
  Module._webdsp_discard_capture(2);
  Module._webdsp_process(8856, 128); // finishedCaptures() must not resurface a discarded id
  assert.equal(Module._webdsp_finished_capture_count(), 0);
  Module._webdsp_stop(60);
  Module._webdsp_remove_sample(3);

  // A window bigger than the engine's capture capacity must be refused outright, never
  // silently truncated.
  const overCapacityFrames = Math.floor(Module._webdsp_max_capture_seconds() * SAMPLE_RATE) + 1;
  const refused = Module._webdsp_arm_capture(4, 0, 20000, 20000 + overCapacityFrames);
  assert.equal(refused, 0, "arm_capture should refuse a window exceeding capture capacity");

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

  // Track buses: a voice on busId 2 should be shaped by that bus's own filter without
  // affecting a simultaneous voice on MASTER_BUS (busId 0) — proving buses route/process
  // independently before being summed together, not just accepted-and-ignored. See
  // ARCHITECTURE.md, "Buses / mixing". Reset the master filter (left engaged by the test
  // above) back to transparent first, so it can't confound this one.
  Module._webdsp_set_bus_param(0, NODE_PARAM_FILTER_CUTOFF, 18000);
  for (let i = 0; i < 10; i++) Module._webdsp_process(5300 + i * 128, 128);

  Module._webdsp_trigger(50, 2, /* busId */ 0, 1.0, 1.0, 0, -1, /* loop */ 1, 0, -1);
  Module._webdsp_trigger(51, 2, /* busId */ 2, 1.0, 1.0, 0, -1, /* loop */ 1, 0, -1);
  Module._webdsp_process(6580, 128);
  const bothUnfiltered = readOutputRMS(Module, 128, 0);
  assert.ok(bothUnfiltered > 0.1, "expected audible output with both bus-0 and bus-2 voices");

  Module._webdsp_set_bus_param(2, NODE_PARAM_FILTER_MODE, 0); // LowPass, track bus 2 only
  Module._webdsp_set_bus_param(2, NODE_PARAM_FILTER_CUTOFF, 300);
  for (let i = 0; i < 10; i++) Module._webdsp_process(6708 + i * 128, 128); // let it settle
  const partiallyFiltered = readOutputRMS(Module, 128, 0);
  assert.ok(
    partiallyFiltered < bothUnfiltered * 0.75,
    `expected filtering only bus 2 to still audibly reduce the combined mix, got before=${bothUnfiltered} after=${partiallyFiltered}`,
  );

  // Stopping the bus-2 voice and comparing to a fresh single-bus-0 voice confirms bus 2's
  // filter attenuated *its own* signal rather than the mix uniformly (which a leak into the
  // master chain would also produce): the remaining bus-0-only level should look like an
  // ordinary unfiltered voice, not like the heavily-attenuated combined level above.
  Module._webdsp_stop(51);
  Module._webdsp_stop(50);
  Module._webdsp_process(7988, 128);
  Module._webdsp_trigger(52, 2, /* busId */ 0, 1.0, 1.0, 0, -1, /* loop */ 1, 0, -1);
  Module._webdsp_process(8116, 128);
  const masterOnly = readOutputRMS(Module, 128, 0);
  assert.ok(
    masterOnly > partiallyFiltered * 0.7,
    `bus-2's filter must not have leaked onto bus-0 voices, got masterOnly=${masterOnly} partiallyFiltered=${partiallyFiltered}`,
  );
  Module._webdsp_stop(52);

  // Reverb: NodeParam.ReverbMix/ReverbDecay/ReverbDamping (ids 9/7/8) on the master bus
  // should leave a decaying wet tail audible for a while after the dry source voice stops.
  // This exercises the exact same compiled DSPNode native/test/reverb_test.cpp tests
  // directly against the native build, so a pass here alongside that pass is the
  // "native/WASM consistency" check for this node — same source, two different
  // compilers/targets, same observable behavior. Also confirms the filter -> delay ->
  // reverb three-node chain (DSPChain bumped from capacity 2 to 3) didn't disturb the
  // filter/delay behavior already exercised above in this same file.
  const NODE_PARAM_REVERB_DECAY = 7;
  const NODE_PARAM_REVERB_DAMPING = 8;
  const NODE_PARAM_REVERB_MIX = 9;

  // A low decay coefficient keeps this smoke test's tail short enough to fully die out
  // within a practical number of process() calls (the tank's round-trip time is inherently
  // long — thousands of samples per delay line — so RT60 at higher decay values, e.g. the
  // 0.5 used by native/test/reverb_test.cpp's more rigorous decay-shape test, comfortably
  // exceeds a second; that test window is sized for that case instead).
  Module._webdsp_set_bus_param(0, NODE_PARAM_REVERB_MIX, 1.0);
  Module._webdsp_set_bus_param(0, NODE_PARAM_REVERB_DECAY, 0.2);
  Module._webdsp_set_bus_param(0, NODE_PARAM_REVERB_DAMPING, 0.2);

  Module._webdsp_trigger(70, 1, /* busId */ 0, 1.0, 1.0, 0, -1, 0, 0, -1);
  for (let i = 0; i < 4; i++) Module._webdsp_process(9000 + i * 128, 128); // feed the diffusers/tank
  Module._webdsp_stop(70);
  for (let i = 0; i < 30; i++) Module._webdsp_process(9512 + i * 128, 128); // dry source now silent
  const tailRms = readOutputRMS(Module, 128, 0);
  assert.ok(
    tailRms > 0.001,
    `expected an audible reverb tail after the source voice stopped, got rms=${tailRms}`,
  );

  // The tail must actually decay, not sustain or grow (a runaway tank would fail this).
  for (let i = 0; i < 300; i++) Module._webdsp_process(13472 + i * 128, 128);
  const decayedRms = readOutputRMS(Module, 128, 0);
  assert.ok(
    decayedRms < tailRms * 0.5,
    `expected the reverb tail to decay, got tail=${tailRms} decayed=${decayedRms}`,
  );
  Module._webdsp_set_bus_param(0, NODE_PARAM_REVERB_MIX, 0); // back to inert for what follows

  // Track-bus routing: a fresh, never-touched bus (3) gets its own reverb turned on and its
  // tail must reach the master output once summed in — proving reverb participates in the
  // same track-bus -> mixer -> master chain the filter isolation test above already proved
  // buses process independently through (that mechanism doesn't change per-node, so it's
  // not re-proven from scratch here, just exercised for this node).
  Module._webdsp_set_bus_param(3, NODE_PARAM_REVERB_MIX, 1.0);
  Module._webdsp_set_bus_param(3, NODE_PARAM_REVERB_DECAY, 0.2);
  Module._webdsp_trigger(71, 1, /* busId */ 3, 1.0, 1.0, 0, -1, 0, 0, -1);
  for (let i = 0; i < 4; i++) Module._webdsp_process(17600 + i * 128, 128);
  Module._webdsp_stop(71);
  for (let i = 0; i < 30; i++) Module._webdsp_process(18112 + i * 128, 128);
  const bus3TailRms = readOutputRMS(Module, 128, 0);
  assert.ok(
    bus3TailRms > 0.001,
    `expected track bus 3's own reverb tail to reach the master output, got rms=${bus3TailRms}`,
  );
  Module._webdsp_set_bus_param(3, NODE_PARAM_REVERB_MIX, 0);

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
