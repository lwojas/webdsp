import { useCallback, useRef, useState } from "react";
import { AudioRuntime } from "../../runtime";
import type { RuntimeCapabilities } from "../../runtime/types";

// This hook is the *only* place in the app that talks to AudioRuntime.create(). Everything
// else (PadGrid, SequencerPanel, ...) receives the already-constructed runtime as a prop —
// demonstrating that the runtime itself has no idea React exists; this hook is just a thin,
// ordinary client of its public API, same as a non-React app's bootstrap code would be.
//
// The worklet URL is resolved here, explicitly, rather than relying on any default from the
// runtime package itself — AudioRuntime.create() intentionally has none (see
// src/runtime/workletUrl.ts). This app happens to be served by Vite, so it can use Vite's
// own dev/build conventions directly; a real consumer of the published package would import
// `defaultWorkletUrl` from "webdsp/worklet-url" instead, or point at wherever it copies
// lib/engine-processor.js as a static asset.
const workletModuleUrl = import.meta.env.DEV ? "/src/worklet/engine-processor.ts" : "/engine-processor.js";

export function useAudioRuntime() {
  const [runtime, setRuntime] = useState<AudioRuntime | null>(null);
  const [capabilities, setCapabilities] = useState<RuntimeCapabilities | null>(null);
  const [error, setError] = useState<string | null>(null);
  const initializing = useRef(false);

  // AudioContext can only start from a user gesture — call this from a click handler.
  const init = useCallback(async () => {
    if (runtime || initializing.current) return;
    initializing.current = true;
    try {
      const rt = await AudioRuntime.create({ workletModuleUrl });
      await rt.resume();
      setCapabilities(rt.getCapabilities());
      setRuntime(rt);
    } catch (err) {
      setError(err instanceof Error ? err.message : String(err));
    } finally {
      initializing.current = false;
    }
  }, [runtime]);

  return { runtime, capabilities, error, init };
}
