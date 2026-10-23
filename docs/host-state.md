# Host state (saving the project in the host's session)

A plugin host (a DAW) saves its own sessions, so the project should live there, not in the page's storage (which is
per-WebView profile, and shared by every instance of the plugin). `getHostState()` returns the host's bridge, or null in a
plain browser, where an application keeps using its own storage.

```ts
const hostState = getHostState();                  // null in a browser
hostState?.publish({ app: "webseq", format: 1, data: json, blobs: { "3": sampleBytes } });
hostState?.clear();                                // New project: drop what the host held
const off = hostState?.onRestore((s) => { /* install s.data and s.blobs */ });
```

- **Opaque.** `data` is the application's own JSON (no sample bytes); `blobs` are the binary payloads it refers to by key
  (sample files). The runtime and the host never look inside. `app` and `format` stamp it, so a host refuses to give
  webseq's state to webtracker, and an application can refuse a format it does not know.
- **Publish cheaply.** Call it on the same debounce as a local recovery write. A blob that is the same `ArrayBuffer`
  object as in the previous publish is not sent again, so keep the buffers you loaded stable.
- **Restore.** If the host loaded state for this application it calls `onRestore` once (also for a late subscriber),
  with the same shape. No state means the callback never runs, so the application starts normally. Publishing before
  the restore arrives would overwrite the saved session, so an application should publish only edits (not its pristine
  initial project), as webseq and webtracker already do for their recovery writes.
- **When hosted, skip local recovery.** The host's session is the source of truth; a shared local recovery slot would be
  shared by every instance.
- The plugin keeps the latest snapshot in memory, so saving the session does not wait on the page, and a session saves
  correctly with the editor closed.
