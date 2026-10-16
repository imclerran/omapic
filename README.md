# omapic

A photo and video gallery for [Omarchy](https://omarchy.org), built around tagging —
C++ and Qt 6 Quick.

## Features

- **Tagging** — tag photos individually or in bulk; filter by tag with *Match all* /
  *Match any*, or by tagged / untagged.
- **Tag categories** — group tags (place, event, artist, …); each category takes a
  color from the active Omarchy theme, and tag chips are color-coded throughout.
- **Tags follow content, not paths** — files are content-hashed, so tags survive
  moves and renames, and are restored if a photo or folder is removed and later
  re-imported.
- **Library folders** — add multiple root folders, hide or show each one, and
  rescan to pick up new, moved, and deleted files. A `.nomedia` file excludes a
  folder and its subtree.
- **Three views** — a timeline grouped by month taken, a folder browser with
  mosaic previews, or a flat grid with duplicate files collapsed.
- **Slideshows** — of the current filter or selection, including whole selected
  folders. Options include *shuffle folders* (folders in random order, each one's
  photos kept together) and intervals down to 0.1 s.
- **Standalone viewer** — `omapic photo.jpg …` or `omapic <dir>` opens a lightweight
  viewer without loading the library, so omapic can be the default image and
  video handler.
- **Omarchy integration** — follows the active theme live. From any photo: add it
  to the current theme's wallpaper rotation, or generate a theme from it with
  [Aether](https://github.com/bjarneo/aether).

## Install

A PKGBUILD is in [`packaging/aur/`](packaging/aur/).

## Build

Requires Qt 6.5+ (Quick, QuickControls2, Sql, Multimedia, Concurrent), exiv2,
CMake, and a C++17 compiler. Runtime: `ffmpegthumbnailer` for video thumbnails;
a Qt HEIF image plugin (e.g. `qt6-heif-image-plugin`, built on `libheif`) for
`.heic`/`.heif` photos — without it HEIC files are listed but show a placeholder;
Aether is optional.

```sh
cmake -B build -G Ninja
cmake --build build
./build/omapic
```

The library database lives at `~/.local/share/omapic/library.db`; settings at
`~/.config/omapic/`.

## License

Copyright (C) 2026 Ian McLerran

omapic is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [LICENSE](LICENSE) for the full text.
