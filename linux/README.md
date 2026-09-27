# CadThumb for Linux (in progress)

Thumbnails for STEP/3MF/STL files in file managers that support the freedesktop.org "Thumbnailer
Entry" spec: GNOME Files/Nautilus, Nemo, Caja, PCManFM, and Thunar (via Tumbler).

## Status: builds, not yet packaged

Unlike Windows, there is no background service here on purpose: the file manager itself already
caches thumbnails, runs extraction out-of-process and enforces its own timeout, so this side is just
a plain one-shot CLI, `cadthumb-thumbnailer <file> <output.png> <size>`, sharing the STEP/3MF/STL
loaders and the software rasterizer with the Windows build (`render/Rasterizer.cpp` is copied
byte-for-byte).

* **Compiles** against OpenCASCADE 7.6 (Ubuntu 24.04's `libocct-*-dev` packages) and pugixml.
* **Not yet verified end-to-end** (no thumbnail has actually been opened and looked at yet) and
  **not yet packaged**: `install.sh`, `uninstall.sh` and `packaging/cadthumb.thumbnailer` (the
  file-manager registration) don't exist yet. See `tools/STATUS.md` for exactly where verification
  was left off and what's left to finish it.

## Building

```bash
sudo apt install cmake g++ pkg-config libpugixml-dev libocct-data-exchange-dev
cmake -S . -B build
cmake --build build -j"$(nproc)"
./build/cadthumb-thumbnailer model.step out.png 256
```

Fedora: `sudo dnf install cmake gcc-c++ pkgconfig pugixml-devel opencascade-devel`.
Arch: `opencascade` and `pugixml` are both in the `extra` repo.

## Layout

| Path | Purpose |
|---|---|
| `src/main.cpp` | CLI entry point: freedesktop `%u %o %s` argument handling, a safety-timeout watchdog thread. |
| `src/render/` | Same STEP (OpenCASCADE)/3MF (miniz+pugixml)/STL loaders and rasterizer as Windows, on `std::string` paths. |
| `src/common/` | Settings (`~/.config/cadthumb/settings.ini`, same key names as Windows), POSIX paths, PNG I/O via stb. |
| `third_party/` | Vendored miniz 3.1.2 and stb_image/stb_image_write (both public domain / MIT, single-header). |
| `tools/` | Scripts used to verify the build without root access (rootless `.deb` extraction into a sysroot) — not part of the normal build, see `tools/STATUS.md`. |
