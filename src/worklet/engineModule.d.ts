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
  ): void;
  _webdsp_release(voiceId: number): void;
  _webdsp_stop(voiceId: number): void;
  _webdsp_set_voice_param(voiceId: number, param: number, value: number): void;
  _webdsp_set_bus_param(busId: number, param: number, value: number): void;

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
  ): void;
  _webdsp_cancel_scheduled(fromFrame: number): void;

  _webdsp_start_capture(captureId: number, busId: number): void;
  _webdsp_stop_capture(captureId: number): void;
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
