// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QUrl>

// The command-line interface: `omapic query|tags|albums|categories` (headless,
// read-only) and `omapic view|slideshow` (resolve a query, then open the
// viewer). main.cpp dispatches on the subcommand before creating any
// application object; everything here runs on omapic-core only, so the tests
// drive it against a temporary library.
//
//   omapic query [--count | --json | -0] [--explain] [--sort name|date|tags]
//                [--reverse] [--all-copies] [--include-disabled] '<query>'
//   omapic tags | albums | categories
//   omapic view [scope/sort flags] [--fullscreen] '<query>'
//   omapic slideshow [scope/sort flags] [--interval S] [--shuffle]
//                    [--shuffle-folders] [--crossfade] [--start-random]
//                    [--loop-videos] [--fullscreen] '<query>'
//
// What the tool does depends only on its arguments and the library: it never
// reads the gallery's saved settings (QSettings), which are incidental UI state.
// The one piece of library state it honours is which folders are enabled,
// because that lives in the database (see --include-disabled).
//
// Options come before the query. Option parsing stops at the first argument
// that isn't a known option (so `omapic query -people` is a query), or at `--`.
// Several remaining arguments (an unquoted query the shell split) are joined
// with single spaces.
//
// Exit codes: 0 = matches, 1 = no matches, 2 = parse or usage error, 3 = the
// library could not be opened (missing, needs the app to upgrade it, or newer
// than this build).
namespace cli {

enum class Command { None, Query, Tags, Albums, Categories, View, Slideshow };
enum class Output { Paths, Count, Json, Nul };

constexpr int kExitMatches = 0;
constexpr int kExitNoMatches = 1;
constexpr int kExitUsage = 2;
constexpr int kExitLibrary = 3;

struct Options {
    Command command = Command::None;
    bool help = false;
    QString error;   // non-empty: a usage error (exit 2) with this message
    QString query;   // the joined query text
    Output output = Output::Paths;
    bool explain = false;
    QString sort = QStringLiteral("name"); // name | date | tags
    bool reverse = false;
    bool allCopies = false;       // keep duplicate copies (by content hash)
    bool includeDisabled = false; // include photos under disabled folders
    bool fullscreen = false;
    // Slideshow flags. Every behaviour is off unless its flag is given.
    double intervalSecs = 4.0;
    bool shuffle = false;
    bool shuffleFolders = false;
    bool crossfade = false;
    bool startRandom = false;
    bool loopVideos = false;
};

// True when `arg` names a subcommand. A file literally named "query" is opened
// with `omapic ./query` or `omapic -- query`.
bool isSubcommand(const QString &arg);
Command commandFor(const QString &arg); // Command::None when it isn't one
// Headless commands run on QCoreApplication; the others need a GUI.
bool isHeadless(Command c);

// Parse the arguments after argv[0]. Never prints; the caller shows `error`
// or the usage text.
Options parse(const QStringList &args);
// The usage text, for --help and usage errors. With a command, that command's
// text; otherwise the overview.
QString usage(Command command = Command::None);

// The gallery's library file (~/.local/share/omapic/library.db).
QString defaultDatabasePath();

// Run a headless command against the library at `dbPath`, writing to the
// streams. Returns the exit code.
int runHeadless(const Options &opts, const QString &dbPath, QTextStream &out, QTextStream &err);

// Resolve a view/slideshow query to file URLs in gallery order, applying the
// scope flags and skipping files missing from disk (reported on `err`).
// Returns the exit code; `urls` is filled only on kExitMatches.
int resolveMedia(const Options &opts, const QString &dbPath, QList<QUrl> &urls, QTextStream &err);

// The slideshow's settings, from the command-line flags alone: 4 s per photo
// and everything else off unless a flag turns it on. The gallery's saved
// slideshow settings are never consulted.
struct SlideshowSettings {
    int intervalMs = 4000;
    bool shuffle = false;
    bool shuffleFolders = false;
    bool fade = false;
    bool randomStart = false;
    bool loop = false; // loop videos
};
SlideshowSettings slideshowSettings(const Options &opts);

// Apply the shuffle / folder-shuffle / random-start settings to a playlist as
// the gallery does, returning the index to start at. `seed` fixes the
// randomness for tests; 0 draws from the global generator.
int orderPlaylist(QList<QUrl> &urls, const SlideshowSettings &s, quint32 seed = 0);

} // namespace cli
