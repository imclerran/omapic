# Omapic

A photo and video gallery for [Omarchy](https://omarchy.org), built around tagging.

## Features

- **Tags that stick** — tags follow the file's content, not its path, so they survive moves, renames, and even deleting and re-importing a folder.
- **Color-coded tag categories** — group tags (people, places, events, …), with colors pulled from your active Omarchy theme.
- **Tag rules** — "if *beach*, then also *vacation*." Set it once and it applies across your whole library, and to everything you tag from then on.
- **Albums, Favorites, and Smart Albums** — star favorites, build albums by hand, or save a tag search as a smart album that keeps itself up to date.
- **Drill into tags by folder** — pick a tag and the folder view narrows to just the folders that contain it.
- **Forgiving search** — find tags without worrying about case or accents ("cafe" finds "café").
- **Slideshows** — from any filter, selection, or album, with folder-aware shuffle and intervals down to 0.1 s.
- **Default image viewer** — `omapic photo.jpg` or `omapic <dir>` opens a lightweight viewer without loading your library.
- **Omarchy integration** — follows your theme live. Add any photo to your wallpaper rotation, or generate a whole theme from it with [Aether](https://github.com/bjarneo/aether).
- **Works on other Linux desktops too** — with reduced integration: set any photo as the wallpaper (via the desktop portal, or a supported wallpaper tool) and open files in your file manager. Omarchy- and Aether-specific actions simply hide when those aren't installed.
- **HEIC and AVIF support**, plus video.

## Install

A PKGBUILD is in [`packaging/aur/`](packaging/aur/).

## Build

Requires Qt 6.5+, exiv2, CMake, and a C++17 compiler. At runtime, `ffmpegthumbnailer` provides video thumbnails and `kimageformats` (with `libheif` and `libavif`) provides HEIC/AVIF support. Aether is optional.

```sh
cmake -B build -G Ninja
cmake --build build
./build/omapic
```

## Library compatibility

Your library lives in a small SQLite database at
`~/.local/share/omapic/library.db`. When you upgrade Omapic, an older library is
migrated in place on the next launch — no action needed. A library written by a
*newer* Omapic than the one you're running is refused rather than risk damaging
it; update Omapic (or restore a backup) to open it again. Before a non-trivial
(non-additive) upgrade, Omapic writes a backup beside the library at
`~/.local/share/omapic/library.db.bak-v<N>` (the two most recent are kept), so
there's always a pre-upgrade copy to fall back on.

## Tests

The logic layer (the SQLite store, text folding, media scanning, and the gallery
filter) has a [Qt Test](https://doc.qt.io/qt-6/qttest-index.html) suite under
[`tests/`](tests/), run via CTest and exercised in CI on every push and pull
request:

```sh
cmake -B build -G Ninja          # tests build by default
cmake --build build
ctest --test-dir build --output-on-failure
```

Pass `-DOMAPIC_BUILD_TESTS=OFF` to skip them (packaging builds do).

## License

Copyright (C) 2026 Ian McLerran

omapic is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any later version. See [LICENSE](LICENSE) for the full text.
