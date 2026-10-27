# Omapic

A photo and video gallery for [Omarchy](https://omarchy.org), built around tagging.

## Features

- **Tags that stick** — tags follow the file's content, not its path, so they survive moves, renames, and even deleting and re-importing a folder.
- **Color-coded tag categories** — group tags (people, places, events, …), with colors pulled from your active Omarchy theme.
- **Tag rules** — "if *beach*, then also *vacation*." Set it once and it applies across your whole library, and to everything you tag from then on.
- **Tag aliases** — teach a second name to mean an existing tag ("forests" → *forest*). Typing the alias anywhere — tagging, search, import — applies the canonical tag, and aliasing a name that's already a tag merges the two.
- **Albums, Favorites, and Smart Albums** — star favorites, build albums by hand, or save a query as a smart album that keeps itself up to date — pick a set of tags, or write anything the [query language](#query-language-oql) can say.
- **Drill into tags by folder** — pick a tag and the folder view narrows to just the folders that contain it.
- **Forgiving search** — find tags without worrying about case or accents ("cafe" finds "café").
- **Slideshows** — from any filter, selection, or album, with folder-aware shuffle and intervals down to 0.1 s.
- **Default image viewer** — `omapic photo.jpg` or `omapic <dir>` opens a lightweight viewer without loading your library.
- **Command line** — `omapic query 'beach -people'` lists matching photos for scripts, and `omapic view` / `omapic slideshow` open a query in the viewer. See [Command line](#command-line).
- **Omarchy integration** — follows your theme live. Add any photo to your wallpaper rotation, or generate a whole theme from it with [Aether](https://github.com/bjarneo/aether).
- **Works on other Linux desktops too** — with reduced integration: set any photo as the wallpaper (via the desktop portal, or a supported wallpaper tool) and open files in your file manager. Omarchy- and Aether-specific actions simply hide when those aren't installed.
- **HEIC and AVIF support**, plus video.

## Install

A PKGBUILD is in [`packaging/aur/`](packaging/aur/).

## Build

Requires Qt 6.5+, exiv2, CMake, and a C++17 compiler. At runtime, `ffmpegthumbnailer` provides video thumbnails, `ffprobe` (from `ffmpeg`) reads video capture dates and metadata, and `kimageformats` (with `libheif` and `libavif`) provides HEIC/AVIF support. Aether is optional.

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

## Query language (OQL)

Smart albums are saved queries in **OQL**, the Omapic Query Language, and the
filter sidebar's **Advanced…** button opens the same query builder on the
current tag selection (see [Advanced queries](#advanced-queries) below).

| Form | Meaning |
|---|---|
| `a b` | AND — both |
| `a \| b` | OR — either |
| `-a` | NOT. `-` is syntax only at the start of a term, so `t-shirt` is just a tag. |
| `( … )` | grouping. `(` at the start of a term always opens a group. |
| `"…"` | a literal name, never syntax |
| `is:fav` | in Favorites |
| `is:untagged` | has no tags (album and Favorites membership don't count) |
| `album:<name>` | in an album or a smart album, by name |
| `cat:<name>` | has any tag in that category; `-cat:<name>` means none |

NOT binds tightest, then OR, then AND: `beach sunset | sunrise` means
`beach (sunset | sunrise)`. There are no word operators (`AND`, `OR`, `NOT`),
because they could be tag names.

Names are matched by **longest match**, ignoring case and accents, so names
with spaces and parentheses need no quoting: `dinner (2019)` is one tag if such a
tag exists, `Star Wars: Episode IV` is a tag (a colon only starts a predicate
after `is`, `album` or `cat`), and `dark forest` is the tag of that name when
there is one, else the two tags `dark` and `forest`. Aliases work too and
resolve to their tag. Syntax wins at the start of a term, so a name that starts
with `-` or `(` must be quoted: `"-20 °C"`, `"(untitled)"`. An unknown name is
an error with suggestions, never a silent fallback to something else.

A smart album can refer to another with `album:<name>`; the reference is
expanded when photos are evaluated, so editing the inner album changes every
album that uses it. Deleting a tag, album or category a query mentions leaves a
named "missing" term in its place (shown as `"name"`) that matches nothing —
the query is never silently rewritten. A `"` can't appear in a name (it is the
one character a query can't quote), so Omapic turns it into `“`/`”`.

## Advanced queries

**Advanced…** at the bottom of the tag sidebar opens the query builder,
pre-filled with the current selection (`a b` or `a | b`). The smart-album
dialog is the same builder with a name field. A query is a row of chips — tags,
`is:` / `album:` / `cat:` predicates, parentheses and `|` bars, any of them
negated — with the result count updating live underneath. **Apply** (or Enter)
narrows the gallery to the matches on top of the sidebar selection and the
search, shown as a strip above the photos with Edit… and ✕ to lift it, and the
tag sidebar narrows to the tags on the matching photos, as it does inside a
folder; **Save as smart album…** keeps the query instead.

- Type a tag name and press **Tab** or **Enter** (or click a suggestion) to
  commit it as a chip. Space never commits, because names can contain spaces.
  Suggestions show tags with their category dots, aliases as `forests → forest`,
  predicates, and the `( )` and `NOT…` rows when you type `(` or `-`; after
  `album:` or `cat:` only albums or categories are offered. Enter on text that
  isn't a tag offers corrections, or a split into the tags it does contain.
- Typing `(` and choosing `( )` inserts both parentheses with the cursor between
  them; typing `)` steps over the one that was inserted for you; Backspace on an
  empty `()` removes both; an unmatched parenthesis is underlined.
- **←/→** move between chips; **Backspace** selects the previous chip and, again,
  turns it back into text; **Shift+arrows** select a range; **-** on a selected
  chip toggles NOT; **|** inserts an OR; **Ctrl+C** copies the selection as text
  and **Ctrl+V** pastes text as chips, highlighting anything it couldn't resolve.
- **Text** switches to the raw query for editing by hand; **Chips** switches back.

## Command line

The gallery's library can be queried from a terminal, read-only, with the same
[query language](#query-language-oql) smart albums use. It is safe to run while
the gallery is open.

```sh
omapic query 'beach (sunset | sunrise) -people'   # one path per line
omapic query --count is:untagged                   # just the number
omapic query --json --sort date 'album:Trip'       # id, path, name, tags, takenAt, favorite, video
omapic query --jsonl 'is:fav'                      # the same, one object per line
omapic query -0 'cat:Place' | xargs -0 cp -t ~/places/
omapic query --explain 'dark forest'               # how the query was read (exit 0 if it parses)
omapic query --strict 'dark forest'                # error out instead of guessing at names
omapic tags | albums | categories                  # names, one per line (for completion)
omapic tags --counts                               # name, tab, number of photos
omapic albums --json                               # name, smart, count, query
omapic view 'is:fav'                               # open the matches in the viewer
omapic slideshow --interval 3 --shuffle 'beach'    # play them as a slideshow
omapic tags --library ~/backup/library.db          # read another library
omapic completion zsh                              # a completion script (see below)
```

`--json` prints an indented array and `--jsonl` one compact object per line,
with the same fields; `takenAt` is UTC with a `Z` suffix
(`2024-06-01T18:30:00Z`), or null when unknown. `tags`, `albums` and
`categories` print one name per line, or with `--counts` each name followed by
a tab and its number of photos, or with `--json` / `--jsonl` objects: tags
`{name, category, count, aliases}` (category null when it has none), albums
`{name, smart, count, query}` (the canonical query text of a smart album, else
null), categories `{name, count}`; `--tsv` is the tab-separated form the
completion scripts read (name, count, then a tag's category or an album's
kind). Counts use the same scope as `query`'s defaults, and these commands
take `--include-disabled` and `--all-copies` to widen it.

Every subcommand reads the gallery's own library
(`~/.local/share/omapic/library.db`, or wherever `$XDG_DATA_HOME` points)
unless `--library <path>` names another one, or, failing that, the
`OMAPIC_LIBRARY` environment variable does. The library is only ever opened
read-only; an error names the path that couldn't be opened.

`query`, `view` and `slideshow` share the scope and sort flags: by default the
results are what the gallery's flat view shows — one copy per file content
(`--all-copies` keeps duplicates), enabled folders only (`--include-disabled`
adds the rest), files that exist on disk (missing ones are counted on stderr),
in the grid's natural name order (`--sort name|date|tags`, `--reverse`; dates
and tag counts read newest / most-tagged first). `slideshow` shows each photo
for 4 seconds, in order, with cuts between them, unless a flag says otherwise:
`--interval S`, `--shuffle`, `--shuffle-folders`, `--crossfade`,
`--start-random` and `--loop-videos`; `--fullscreen` opens `view` full screen.

The command line depends only on its arguments, its environment variables and
the library. It never reads the gallery's saved settings, so a command behaves
the same whatever was last clicked in the app; the one piece of library state
it honours is which folders are enabled, because that is part of the library
itself.

Long options go anywhere before `--`, before or after the query
(`omapic query beach --count`); an unknown `--option` is an error wherever it
appears. Short options (`-0`, `-h`) count only before the query, because
`-0` is also a valid query term (NOT the tag `0`) — so `omapic query -people`
is the query `-people`. `--` ends the options, and everything after it is query
text (`omapic query --count -- -people`). An unquoted query the shell splits
into words is joined back with single spaces, and a word you shell-quoted
(`omapic query is:fav "dark forest"`) is quoted in the query too, since the
shell strips its quotes; a single argument is taken as the whole query as
typed, so `omapic query 'beach "dark forest"'` works as well. Subcommands win over file
arguments: open a file literally named `query` with `omapic ./query` or
`omapic -- query`.

Parse errors are shown with a caret under the offending span and "did you
mean" suggestions, and a reading that depended on your tags (`dark forest`
split into two tags because no tag has that name) is noted on stderr. With
`--strict`, such a reading — and a smart album whose query mentions a deleted
name or refers back to itself — is an error instead (exit 2), so a script
never silently runs a different query than it meant; quote the names
(`"dark forest"`, or `"dark" "forest"`) to say which reading you want. Exit
codes: 0 something matched (for `--explain`: the query parses), 1 nothing
matched, 2 a usage or query error, 3 the library couldn't be opened. The tool
never migrates the library: if it is older than the installed Omapic, launch
the app once to upgrade it.

### Shell completion

`omapic completion zsh|bash|fish` prints a completion script for the shell.
It completes the subcommands, each one's options (and the values of `--sort`),
and query words: tag names by default, album names after `album:`, category
names after `cat:`, and the `is:` values — fetched from the library as you
type, so `OMAPIC_LIBRARY` and a `--library` already on the line are honoured.
zsh and fish show each tag's category and photo count next to it. Names with
spaces or shell characters are inserted quoted. Install it once:

```sh
# zsh: a file named _omapic in a directory on $fpath, before compinit
mkdir -p ~/.zsh/completions && omapic completion zsh > ~/.zsh/completions/_omapic
# then in ~/.zshrc, before `compinit`:  fpath=(~/.zsh/completions $fpath)

# bash: where bash-completion loads scripts on demand…
mkdir -p ~/.local/share/bash-completion/completions
omapic completion bash > ~/.local/share/bash-completion/completions/omapic
# …or, without bash-completion, in ~/.bashrc:  eval "$(omapic completion bash)"

# fish
omapic completion fish > ~/.config/fish/completions/omapic.fish
```

Like the other subcommands, `completion` wins over a file of that name:
`omapic ./completion` opens the file.

## Tag retention

When a file leaves your library — a folder removed, a photo deleted, or a file
that vanished from disk — its tags are kept, keyed by the file's content, so they
return if the same file is ever added back. By default they're remembered
forever. If you'd rather not accumulate tags for files you'll never see again,
**Settings ▸ Library ▸ Remember tags for removed files** can age them out after a
year, six months, or 30 days (pruned on the next rescan), and **Forget now…**
clears them all immediately. Recovery only works when the re-added file is
byte-identical, so most re-downloads from services that re-encode won't match.

## Tests

The logic layer (the SQLite store, text folding, the query engine, the command
line, media scanning, and the gallery filter) and the query builder (driven with
key events against a mock library, offscreen) have a [Qt Test](https://doc.qt.io/qt-6/qttest-index.html) suite under
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
