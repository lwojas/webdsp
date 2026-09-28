// Decoding is delegated entirely to the browser's own audio decoder (per architecture
// guidance: don't reimplement container/codec parsing in WASM). This runs once per sample
// load, on the main thread, never on the realtime path.
//
// Note on sample rate: `decodeAudioData` decodes (and resamples, if needed) into an
// AudioBuffer whose sampleRate equals the OfflineAudioContext's configured rate — the
// browser does not expose the source file's original sample rate through this API. We
// decode at the engine's own sample rate so no further resampling is needed to play the
// sample back at unity rate, and record that as the sample's sampleRate. Per-voice
// playback rate/pitch changes still go through the runtime's own interpolating resampler
// (native/src/resampler.h), so nothing here assumes samples share a rate or duration — see
// ARCHITECTURE.md, "Samples", for the full rationale.

export interface DecodedAudio {
  channelData: Float32Array[];
  channels: number;
  sampleRate: number;
  length: number;
}

export async function decodeAudioFile(
  data: ArrayBuffer,
  engineSampleRate: number,
): Promise<DecodedAudio> {
  // A throwaway OfflineAudioContext is the standard way to decode off the realtime graph;
  // it is never connected to output.
  const probe = new OfflineAudioContext(1, 1, engineSampleRate);
  const buffer = await probe.decodeAudioData(data);

  const channelData: Float32Array[] = [];
  for (let ch = 0; ch < buffer.numberOfChannels; ch++) {
    // Copy out of the AudioBuffer's internal storage so we own a plain, transferable
    // ArrayBuffer (getChannelData's view cannot be handed to postMessage's transfer list).
    channelData.push(Float32Array.from(buffer.getChannelData(ch)));
  }

  return {
    channelData,
    channels: buffer.numberOfChannels,
    sampleRate: buffer.sampleRate,
    length: buffer.length,
  };
}
