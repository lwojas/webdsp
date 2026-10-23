# Host files (open and save through the host's dialogs)

A plugin's embedded WebView cannot always do what a browser does for files: macOS's WKWebView has no file input
(`<input type=file>` shows nothing) and ignores `<a download>`. `getHostFiles()` returns the host's file bridge, or null in a
plain browser, where an application keeps using the input and the download link.

```ts
const files = getHostFiles();                       // null in a browser
const saved = await files?.saveFile({ suggestedName: "My Song.webseq.json", data: text });   // { name } | null (cancelled)
const picked = await files?.openFile({ extensions: ["json"] });                              // { name, data } | null (cancelled)
```

- Both calls show the host's own dialog and resolve `null` if the user cancels; they reject if the file could not be
  written or read.
- `data` for saving is bytes or text (UTF-8); opening returns bytes. Wrap them if a consumer expects a `File`:
  `new File([picked.data], picked.name)`.
- The host installs the bridge on `globalThis.__webdspHost.files` before the page's scripts run.
- Prefer this only when `getHostFiles()` is non-null; otherwise use the browser's own picker and download.
