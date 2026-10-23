// Host files: opening and saving a file through the host's own file dialogs, for when the page cannot. A plugin's
// embedded WebView (macOS WKWebView) has no file input and ignores downloads, so an application that exports or
// imports a project file asks the host to do it. In a plain browser there is no host: `getHostFiles()` is null and the
// application uses an <input type=file> and a download link as usual. See docs/host-files.md.

export interface HostFileSaveOptions {
  /** The name offered in the dialog, e.g. "My Song.webseq.json". */
  suggestedName: string;
  /** The file's content: bytes, or text (saved as UTF-8). */
  data: ArrayBuffer | string;
}

export interface HostFileOpenOptions {
  /** File extensions to offer, without the dot (e.g. ["json"]); omit for any file. */
  extensions?: string[];
}

export interface HostFilesBridge {
  /** Shows the host's save dialog and writes the file. Resolves with the name it was saved under, or null if the user cancelled. Rejects if the write failed. */
  saveFile(options: HostFileSaveOptions): Promise<{ name: string } | null>;
  /** Shows the host's open dialog and reads the file. Resolves with its name and bytes, or null if the user cancelled. Rejects if the read failed. */
  openFile(options?: HostFileOpenOptions): Promise<{ name: string; data: ArrayBuffer } | null>;
}

/** The host's file bridge, or null when there is none (a plain browser). The host installs it on
 * `globalThis.__webdspHost.files` before the page's scripts run. */
export function getHostFiles(): HostFilesBridge | null {
  const host = (globalThis as { __webdspHost?: { files?: HostFilesBridge } }).__webdspHost;
  return host?.files ?? null;
}
