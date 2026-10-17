// Typed surface for the compiled WASM module (native/src/api.cpp's extern "C" exports).
// The .js this describes is a build artifact (native/build.sh; not checked in — run
// `npm run build:wasm` first) so this ambient declaration is what gives it a type here.
export interface EngineModule {
  HEAPF32: Float32Array;
  HEAP32: Int32Array;
  HEAPU8: Uint8Array;

  _webdsp_init(sampleRate: number, outputChannels: number, maxVoices: number): void;
  _webdsp_configure_voices(maxVoices: number): void;

  _webdsp_alloc_channel_buffer(numFrames: number): number;
  _webdsp_alloc_ptr_table(count: number): number;
  _webdsp_free(ptr: number): void;
  _webdsp_commit_sample(
    sampleId: number,
    numChannels: number,
    length: number,
    sampleRate: number,
    channelPtrTable: number,
  ): void;
  _webdsp_remove_sample(sampleId: number): void;
  _webdsp_set_sample_param(sampleId: number, param: number, value: number): void;

  _webdsp_trigger(
    voiceId: number,
    sampleId: number,
    busId: number,
    gain: number,
    rate: number,
    startFrame: number,
    endFrame: number,
    loop: number,
    reverse: number,
    durationFrames: number,
    pan: number,
  ): void;
  _webdsp_release(voiceId: number): void;
  _webdsp_stop(voiceId: number): void;
  _webdsp_set_voice_param(voiceId: number, param: number, value: number): void;
  _webdsp_set_bus_param(busId: number, param: number, value: number): void;

  // FX chains: mutators return 1 on success, 0 if rejected.
  _webdsp_fx_add(busId: number, effectId: number, kind: number, index: number): number;
  _webdsp_fx_remove(busId: number, effectId: number): number;
  _webdsp_fx_move(busId: number, effectId: number, index: number): number;
  _webdsp_fx_set_enabled(busId: number, effectId: number, enabled: number): number;
  _webdsp_fx_set_param(busId: number, effectId: number, param: number, value: number): number;
  _webdsp_fx_count(busId: number): number;
  _webdsp_fx_id_at(busId: number, index: number): number;
  _webdsp_fx_kind_at(busId: number, index: number): number;
  _webdsp_fx_enabled_at(busId: number, index: number): number;

  // Instruments: create/remove return 1 on success, 0 if rejected. Metadata is per kind.
  _webdsp_instrument_create(instrumentId: number, kind: number, busId: number): number;
  _webdsp_instrument_remove(instrumentId: number): number;
  _webdsp_instrument_note_on(instrumentId: number, velocity: number, pan: number, pitch: number, durationFrames: number): void;
  _webdsp_instrument_note_off(instrumentId: number): void;
  _webdsp_instrument_set_param(instrumentId: number, param: number, value: number): void;
  _webdsp_schedule_instrument_note(atFrame: number, instrumentId: number, velocity: number, pan: number, pitch: number, durationFrames: number): void;
  _webdsp_set_transport(atFrame: number, anchorFrame: number, bpm: number): void;
  _webdsp_instrument_kind_id(kind: number): number;
  _webdsp_instrument_kind_name(kind: number): number;
  _webdsp_instrument_kind_category(kind: number): number;
  _webdsp_instrument_param_count(kind: number): number;
  _webdsp_instrument_param_id(kind: number, index: number): number;
  _webdsp_instrument_param_min(kind: number, index: number): number;
  _webdsp_instrument_param_max(kind: number, index: number): number;
  _webdsp_instrument_param_default(kind: number, index: number): number;
  _webdsp_instrument_param_name(kind: number, index: number): number;
  _webdsp_instrument_param_unit(kind: number, index: number): number;
  _webdsp_instrument_param_mod(kind: number, index: number): number;
  _webdsp_instrument_set_lfo(instrumentId: number, slot: number, enabled: number, waveform: number, sync: number, rateHz: number, division: number, phase: number, amount: number, target: number, retrigger: number): number;
  _webdsp_instrument_base_param(instrumentId: number, param: number): number;
  _webdsp_instrument_effective_param(instrumentId: number, param: number): number;
  _webdsp_lfo_shape(waveform: number, phase: number): number;
  _webdsp_lfo_max(): number;
  _webdsp_lfo_waveform_count(): number;
  _webdsp_lfo_waveform_name(index: number): number;
  _webdsp_lfo_division_count(): number;
  _webdsp_lfo_division_name(index: number): number;
  _webdsp_lfo_division_beats(index: number): number;
  _webdsp_lfo_min_rate(): number;
  _webdsp_lfo_max_rate(): number;
  _webdsp_active_instrument_count(): number;

  _webdsp_schedule_event(
    atFrame: number,
    voiceId: number,
    sampleId: number,
    busId: number,
    gain: number,
    rate: number,
    startFrame: number,
    endFrame: number,
    loop: number,
    reverse: number,
    durationFrames: number,
    pan: number,
  ): void;
  _webdsp_cancel_scheduled(fromFrame: number): void;

  _webdsp_start_capture(captureId: number, busId: number): void;
  _webdsp_stop_capture(captureId: number): void;
  _webdsp_arm_capture(captureId: number, busId: number, startFrame: number, stopFrame: number): number;
  _webdsp_max_capture_seconds(): number;
  _webdsp_finished_capture_count(): number;
  _webdsp_finished_capture_id(index: number): number;
  _webdsp_capture_length(captureId: number): number;
  _webdsp_capture_channels(captureId: number): number;
  _webdsp_capture_channel_ptr(captureId: number, channel: number): number;
  _webdsp_discard_capture(captureId: number): void;

  _webdsp_process(blockStartFrame: number, numFrames: number): void;
  _webdsp_output_channel_ptr(channel: number): number;
  _webdsp_ended_voice_count(): number;
  _webdsp_ended_voice_id(index: number): number;

  _webdsp_active_voice_count(): number;
  _webdsp_loaded_sample_count(): number;
  _webdsp_sample_memory_bytes(): number;
  _webdsp_max_voices(): number;
  _webdsp_output_channels(): number;
}

declare module "*/generated/engine.js" {
  export default function createEngineModule(): Promise<EngineModule>;
}
