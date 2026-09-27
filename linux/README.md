# CadThumb for Linux

Thumbnails for STEP/3MF/STL files in file managers that support the freedesktop.org "Thumbnailer
Entry" spec: GNOME Files/Nautilus, Nemo, Caja, PCManFM, and Thunar (via Tumbler).

There is no tray icon and no background service here on purpose — unlike Windows, the file manager
itself already caches thumbnails, runs extraction out-of-process and enforces its own timeout. All
that's left is a plain one-shot renderer (`cadthumb-thumbnailer <file> <output.png> <size>`), sharing
the STEP/3MF/STL loaders and the software rasterizer with the Windows build (`render/Rasterizer.cpp`
is copied byte-for-byte). **Settings, status and (un)install all live in one terminal command,
`cadthumb`**, in place of the tray icon's menu.

## Install

```bash
git clone https://github.com/GNUGiminot/CadThumb.git
cd CadThumb/linux
sudo apt install cmake g++ pkg-config libpugixml-dev libocct-data-exchange-dev  # Debian/Ubuntu; see below for other distros
./install.sh            # current user, STEP + 3MF
./install.sh --stl      # + .stl
./install.sh --system   # for all users (asks for sudo)
```

The first run builds `cadthumb-thumbnailer` (a couple of minutes), then registers it. Open a folder
with a `.step`/`.3mf` file in your file manager — reopen it or press F5 if it was already open, file
managers only pick up a new thumbnailer on the next folder read.

Other distros: Fedora — `sudo dnf install cmake gcc-c++ pkgconfig pugixml-devel opencascade-devel`;
Arch — `opencascade` and `pugixml` are both in the `extra` repo.

## The `cadthumb` command

```
cadthumb                    status + quick hints (default)
cadthumb status             binary, registration, settings -- all in one place
cadthumb settings           print/open the settings file (~/.config/cadthumb/settings.ini)
cadthumb install [--system] [--stl]
cadthumb uninstall [--system] [--purge]
cadthumb refresh [file]     forget cached thumbnails so they regenerate (all, or just one file)
cadthumb test <file> [size] render one thumbnail directly, for debugging
```

`settings.ini` uses the same key names as the Windows build's `[Render]` section (quality, up-axis,
view angles, colors, background, `MaxFileSizeMB`, `Prefer3mfEmbedded`, ...) — see
`src/common/Settings.cpp` for the full, commented list of defaults it writes on first run.

## Uninstall

```bash
./uninstall.sh            # or, once installed: cadthumb uninstall
./uninstall.sh --purge    # also deletes settings.ini
./uninstall.sh --system   # matches an --system install
```

## Status

Verified: builds against OpenCASCADE 7.6 (Ubuntu 24.04's `libocct-*-dev`) and pugixml; the `cadthumb`
install/uninstall/status/settings/test flow was exercised end-to-end with a stand-in binary. **Not
yet verified**: an actual rendered thumbnail has not yet been visually confirmed inside a real file
manager window (no GUI desktop was available while writing this). See `tools/STATUS.md` for a
build-environment quirk hit while cross-checking the real binary link step (specific to a rootless
verification sandbox, not expected on a normal `sudo apt install`) and exact next steps.

## Building manually

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/cadthumb-thumbnailer model.step out.png 256
```

## Layout

| Path | Purpose |
|---|---|
| `bin/cadthumb` | The management command: install/uninstall/status/settings/refresh/test. |
| `install.sh`, `uninstall.sh` | Thin bootstrap wrappers around `bin/cadthumb install`/`uninstall` that build first if needed. |
| `packaging/cadthumb.thumbnailer` | Reference copy of the freedesktop registration file (`cadthumb install` generates its own with resolved paths). |
| `packaging/cadthumb-mime.xml` | MIME-type associations for systems whose shared-mime-info doesn't already know STEP/3MF/STL. |
| `src/main.cpp` | `cadthumb-thumbnailer`: freedesktop `%u %o %s` argument handling, a safety-timeout watchdog thread. |
| `src/render/` | Same STEP (OpenCASCADE)/3MF (miniz+pugixml)/STL loaders and rasterizer as Windows, on `std::string` paths. |
| `src/common/` | Settings, POSIX paths, PNG I/O via stb. |
| `third_party/` | Vendored miniz 3.1.2 and stb_image/stb_image_write (public domain / MIT, single-header). |
| `tools/` | Scripts used to verify the build without root access (rootless `.deb` extraction into a sysroot) — not part of the normal build. |
