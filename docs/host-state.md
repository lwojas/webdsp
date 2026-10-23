# Host state (saving the project in the host's session)

A plugin host (a DAW) saves its own sessions, so the project should live there, not in the page's storage (which is
per-WebView profile, and shared by every instance of the plugin). `getHostState()` returns the host's bridge, or null in a
plain browser, where an application keeps using its own storage.

```ts
const hostState = getHostState();                  // null in a browser
hostState?.publish({ app: "webseq", format: 1, data: json, blobs: { "3": sampleBytes } });
hostState?.clear();                                // New project: drop what the host held
const off = hostState?.onRestore((s) => { /* s: the saved snapshot, or null for a new session */ });
```

- **Opaque.** `data` is the application's own JSON (no sample bytes); `blobs` are the binary payloads it refers to by key
  (sample files). The runtime and the host never look inside. `app` and `format` stamp it, so a host refuses to give
  webseq's state to webtracker, and an application can refuse a format it does not know.
- **Publish cheaply.** Call it on the same debounce as a local recovery write. A blob that is the same `ArrayBuffer`
  object as in the previous publish is not sent again, so keep the buffers you loaded stable.
- **Restore.** The host answers `onRestore` once (also for a late subscriber): with the saved snapshot, or with `null`
  when this session has none. `null` is a definite "nothing saved", so an application that seeds a new session (a demo
  song) does so on `null` and not before; a host that never answers (an older plugin) leaves the callback uncalled, so
  such an application should give up waiting after a moment. Publishing before the restore arrives would overwrite the
  saved session, so publish only edits (not a pristine initial project), as webseq and webtracker do for their recovery
  writes.
- **When hosted, skip local recovery.** The host's session is the source of truth; a shared local recovery slot would be
  shared by every instance.
- The plugin keeps the latest snapshot in memory, so saving the session does not wait on the page, and a session saves
  correctly with the editor closed.
