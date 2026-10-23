// Host state: lets an application keep its project in whatever is hosting it, when the host saves sessions of its
// own (a DAW plugin: the project lives in the DAW session, not in the page's storage).
//
// The runtime knows nothing about what a project is. The application hands over an opaque snapshot (its own JSON plus
// any binary payloads, such as sample files, that the JSON refers to by key) and gets the same snapshot back when the
// host restores a session. In a plain browser there is no host: `getHostState()` is null and an application keeps using
// its own storage. See docs/host-state.md.

export interface HostStateSnapshot {
  /** Which application wrote this ("webseq", "webtracker"): a host refuses to hand it to another. */
  app: string;
  /** The application's own format version for `data`. */
  format: number;
  /** The application's project as text (JSON). Kept small: binary payloads go in `blobs`. */
  data: string;
  /** Binary payloads `data` refers to, by key. A buffer that is the same object as in the previous
   * publish is not sent again, so keep stable buffers across publishes. */
  blobs: Record<string, ArrayBuffer>;
}

export interface HostStateBridge {
  /** Hands the host the application's current state, replacing what it held. */
  publish(snapshot: HostStateSnapshot): void;
  /** Tells the host the application is starting from nothing (New project): drop what it held. */
  clear(): void;
  /** Subscribes to a session restore. The host answers once with the state it loaded for this session, or with
   * null when it has none (a new session), so an application can tell "nothing saved" from "not heard yet". A
   * subscriber that arrives after the answer gets it at once, and a later load (the DAW reverting the session)
   * calls it again. Returns an unsubscribe function. */
  onRestore(fn: (snapshot: HostStateSnapshot | null) => void): () => void;
}

/** The host's state bridge, or null when there is none (a plain browser). The host installs it on
 * `globalThis.__webdspHost.state` before the page's scripts run. */
export function getHostState(): HostStateBridge | null {
  const host = (globalThis as { __webdspHost?: { state?: HostStateBridge } }).__webdspHost;
  return host?.state ?? null;
}
