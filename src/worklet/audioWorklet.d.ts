// Minimal ambient declarations for the AudioWorkletGlobalScope APIs used here. TypeScript's
// bundled lib files don't ship these (they conflict with DOM's `self`/`postMessage`, which
// is why this file — not the main tsconfig's `lib` array — is where they live). Kept
// deliberately small: only what src/worklet/engine-processor.ts actually touches.
export {};

declare global {
  const sampleRate: number;
  const currentFrame: number;
  const currentTime: number;

  interface AudioWorkletProcessor {
    readonly port: MessagePort;
    process(
      inputs: Float32Array[][],
      outputs: Float32Array[][],
      parameters: Record<string, Float32Array>,
    ): boolean;
  }

  const AudioWorkletProcessor: {
    prototype: AudioWorkletProcessor;
    new (options?: AudioWorkletNodeOptions): AudioWorkletProcessor;
  };

  interface AudioWorkletNodeOptions {
    processorOptions?: Record<string, unknown>;
  }

  function registerProcessor(
    name: string,
    processorCtor: new (options?: AudioWorkletNodeOptions) => AudioWorkletProcessor,
  ): void;
}
