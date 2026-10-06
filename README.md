# omapic

A photo gallery with tagging, tag filtering, and slideshows — built with C++ and Qt 6 Quick.

## Features

- **Import** a folder (recursively) of images into a local library.
- **Tag** photos; tags are stored in SQLite and persist across runs.
- **Filter** the gallery by selecting tags in the sidebar, with a *Match all* / *Match any* toggle.
- **Slideshow** the currently filtered set in fullscreen (`←/→` navigate, `Space` pause, `Esc` exit).

## Architecture

| Component | Role |
|-----------|------|
| `Database` | SQLite wrapper: `photos`, `tags`, and a `photo_tags` join table |
| `PhotoModel` | `QAbstractListModel`, the in-memory source of truth |
| `PhotoFilterModel` | `QSortFilterProxyModel` doing the tag filtering |
| `TagModel` | tag list with counts + selection state (drives the filter) |
| `Library` | the single object QML talks to; owns the above, routes all mutations through the DB |

The library database lives at `~/.local/share/omapic/library.db`.

## Build

Requires Qt 6.5+, CMake, and a C++17 compiler.

```sh
cmake -B build -G Ninja
cmake --build build
./build/omapic
```
