# web-assets

CDN-served payloads for the on-device web UI. jsDelivr serves these
straight from GitHub, pinned to the release tag:

    https://cdn.jsdelivr.net/gh/imshentastic/CrumBLE@<tag>/web-assets/<file>

`crumble-prebake.wasm` is the EPUB optimizer engine. As of 4.7.0 it is no
longer embedded in firmware flash (it was the single largest blob, 1.3 MB
gzipped); the optimizer JS fetches it from here, caches the bytes in the
browser's IndexedDB for offline reuse, and only falls back to the device
route on older/debug firmware built with CRUMBLE_EMBED_WASM=1.

Release checklist: whenever tools/crumble-prebake is rebuilt, re-copy
build-wasm/crumble-prebake.wasm here in the same commit — the firmware at
a tag and the wasm at that tag must agree on the prebake cache format.
