import type { HostCommand, WorkletEvent } from "./commandProtocol";

// Thin transport wrapper around the AudioWorkletNode's message port. Owns nothing about
// samples, voices, or ids — that's AudioRuntime's job (src/runtime/index.ts). This class
// only knows how to get commands to the worklet (queueing them if it isn't ready yet, since
// the WASM module loads asynchronously) and fan events back out.
export interface ReadyInfo {
  sampleRate: number;
  outputChannels: number;
  maxVoices: number;
  renderQuantumFrames: number;
  maxCaptureSeconds: number;
}

export class WorkletBridge {
  readonly node: AudioWorkletNode;
  readonly ready: Promise<ReadyInfo>;

  private queued: Array<{ cmd: HostCommand; transfer: Transferable[] }> = [];
  private isReady = false;
  private listeners = new Set<(event: WorkletEvent) => void>();

  private constructor(node: AudioWorkletNode) {
    this.node = node;
    this.ready = new Promise<ReadyInfo>((resolve) => {
      this.node.port.onmessage = (ev: MessageEvent<WorkletEvent>) => {
        const data = ev.data;
        if (!this.isReady && data.type === "ready") {
          this.isReady = true;
          resolve(data);
          for (const { cmd, transfer } of this.queued.splice(0)) {
            this.node.port.postMessage(cmd, transfer);
          }
        }
        for (const listener of this.listeners) listener(data);
      };
    });
  }

  static async create(
    audioContext: AudioContext,
    workletModuleUrl: string,
    maxVoices: number,
    outputChannels: number,
  ): Promise<WorkletBridge> {
    await audioContext.audioWorklet.addModule(workletModuleUrl);
    const node = new AudioWorkletNode(audioContext, "webdsp-engine", {
      numberOfInputs: 0,
      numberOfOutputs: 1,
      outputChannelCount: [outputChannels],
      processorOptions: { maxVoices, outputChannels },
    });
    node.connect(audioContext.destination);
    return new WorkletBridge(node);
  }

  send(cmd: HostCommand, transfer: Transferable[] = []): void {
    if (this.isReady) {
      this.node.port.postMessage(cmd, transfer);
    } else {
      this.queued.push({ cmd, transfer });
    }
  }

  onEvent(fn: (event: WorkletEvent) => void): () => void {
    this.listeners.add(fn);
    return () => this.listeners.delete(fn);
  }

  disconnect(): void {
    this.node.disconnect();
  }
}
