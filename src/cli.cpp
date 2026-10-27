// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "cli.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QSet>
#include <QStandardPaths>
#include <QVector>

#include <algorithm>

#include "database.h"
#include "oql.h"
#include "photo.h"
#include "photosort.h"

namespace cli {
namespace {

// --- The command and option tables -------------------------------------------

struct CommandName {
    const char *name;
    Command command;
};
const CommandName kCommands[] = {
    {"query", Command::Query},
    {"tags", Command::Tags},
    {"albums", Command::Albums},
    {"categories", Command::Categories},
    {"view", Command::View},
    {"slideshow", Command::Slideshow},
};

// Which commands an option belongs to.
enum Scope : unsigned {
    All = 0xff,
    Query_ = 1u << int(Command::Query),
    View_ = 1u << int(Command::View),
    Slideshow_ = 1u << int(Command::Slideshow),
    WithQuery = Query_ | View_ | Slideshow_,
    Gui = View_ | Slideshow_,
};

struct OptionSpec {
    const char *name;   // long name
    char shortName;     // 0 for none
    bool takesValue;
    unsigned scope;
    const char *valueName;
    const char *help;
};
const OptionSpec kOptions[] = {
    {"help", 'h', false, All, "", "Show this help."},
    {"library", 0, true, All, "path", "The library to read (default: $OMAPIC_LIBRARY, else the gallery's)."},
    {"count", 0, false, Query_, "", "Print only the number of matches."},
    {"json", 0, false, Query_, "", "Print matches as JSON objects."},
    {"0", '0', false, Query_, "", "Separate paths with NUL instead of newline (for xargs -0)."},
    {"explain", 0, false, Query_, "", "Print how the query was read instead of matches (exit 0 when it parses)."},
    {"strict", 0, false, WithQuery, "", "Refuse a reading that depends on your tag names or a broken smart album."},
    {"sort", 0, true, WithQuery, "name|date|tags", "Order, as the gallery sorts: name (default), date, tags."},
    {"reverse", 0, false, WithQuery, "", "Reverse the order."},
    {"all-copies", 0, false, WithQuery, "", "Keep duplicate copies of the same content."},
    {"include-disabled", 0, false, WithQuery, "", "Include photos under disabled folders."},
    {"fullscreen", 0, false, Gui, "", "Open full screen."},
    {"interval", 0, true, Slideshow_, "S", "Seconds per photo (default 4)."},
    {"shuffle", 0, false, Slideshow_, "", "Shuffle the photos."},
    {"shuffle-folders", 0, false, Slideshow_, "", "Visit folders in random order (photos stay grouped)."},
    {"crossfade", 0, false, Slideshow_, "", "Crossfade between photos instead of cutting."},
    {"start-random", 0, false, Slideshow_, "", "Start on a random photo."},
    {"loop-videos", 0, false, Slideshow_, "", "Repeat a video until you move on."},
};

const OptionSpec *findOption(const QString &name, Command c)
{
    for (const OptionSpec &o : kOptions) {
        const bool match = name == QLatin1String(o.name)
            || (o.shortName && name.size() == 1 && name.at(0) == QLatin1Char(o.shortName));
        if (match && (o.scope & (1u << int(c))))
            return &o;
    }
    return nullptr;
}

bool takesQuery(Command c)
{
    return c == Command::Query || c == Command::View || c == Command::Slideshow;
}

QString commandName(Command c)
{
    for (const CommandName &n : kCommands)
        if (n.command == c)
            return QLatin1String(n.name);
    return {};
}

// --- Library access -------------------------------------------------------------

const char kLibraryEnv[] = "OMAPIC_LIBRARY";

// Open the library libraryPath(opts) names, read-only, explaining on `err`
// (naming the path) when that isn't possible. Returns kExitMatches when open.
int openLibrary(Database &db, const Options &opts, QTextStream &err)
{
    const QString dbPath = libraryPath(opts);
    // "Launch omapic once to create it" only helps for the gallery's own
    // library; a path someone chose is simply missing.
    const bool chosen = !opts.library.isEmpty() || !qEnvironmentVariableIsEmpty(kLibraryEnv);
    static int seq = 0;
    const QString conn = QStringLiteral("cli_%1").arg(++seq);
    switch (db.openReadOnly(dbPath, conn)) {
    case Database::OpenStatus::Ok:
        return kExitMatches;
    case Database::OpenStatus::CannotOpen:
        if (!QFileInfo::exists(dbPath)) {
            if (chosen)
                err << "omapic: no library exists at " << dbPath << '\n';
            else
                err << "omapic: cannot open the library at " << dbPath
                    << " (launch omapic once to create it)\n";
        } else {
            err << "omapic: cannot open the library at " << dbPath << '\n';
        }
        return kExitLibrary;
    case Database::OpenStatus::SchemaOlder:
        err << "omapic: the library at " << dbPath
            << " needs upgrading; launch omapic once to upgrade it\n";
        return kExitLibrary;
    case Database::OpenStatus::SchemaNewer:
        err << "omapic: the library at " << dbPath
            << " was written by a newer Omapic; update Omapic to use it\n";
        return kExitLibrary;
    }
    return kExitLibrary;
}

// Print a parse error the way compilers do: the message, the query with a
// caret line under the offending span, and any suggestions.
void printParseError(const oql::Error &e, const QString &query, QTextStream &err)
{
    err << "omapic: error: " << e.message << '\n';
    if (!query.isEmpty()) {
        err << "  " << query << '\n';
        err << "  " << QString(e.span.start, QLatin1Char(' '))
            << QString(std::max(1, e.span.length), QLatin1Char('^')) << '\n';
    }
    if (!e.suggestions.isEmpty())
        err << "  did you mean: " << e.suggestions.join(QStringLiteral(", ")) << '\n';
}

// Under --strict, a reading that depended on the vocabulary is an error in the
// same shape as a parse error, with a hint on how to pin the names down.
void printStrictNote(const oql::Note &n, const QString &query, QTextStream &err)
{
    oql::Error e;
    e.span = n.span;
    e.message = n.message;
    printParseError(e, query, err);
    err << "  hint: quote the names you mean, e.g. \"dark forest\" for one tag or\n"
           "  \"dark\" \"forest\" for two (--strict refuses to guess)\n";
}

// Every Missing term in a tree (a tag, album or category a smart album still
// mentions but that no longer exists), described for an error message.
void collectMissing(const oql::Node &n, QStringList &out)
{
    if (n.kind == oql::NodeKind::Missing) {
        const char *what = n.was == oql::NameKind::Category ? "category"
            : n.was == oql::NameKind::Tag ? "tag" : "album";
        out.append(QStringLiteral("A smart album refers to the %1 \"%2\", which no longer exists")
                       .arg(QLatin1String(what), n.name));
    }
    for (const oql::Node &c : n.children)
        collectMissing(c, out);
}

// The query pipeline shared by `query`, `view` and `slideshow`, in two steps.
// First the text is parsed against the library and smart-album references are
// expanded (all --explain needs); then every photo in the flat view's scope is
// evaluated, files missing from disk are dropped, and the rest are ordered as
// the grid would.
struct Parsed {
    QString canonical;
    oql::Node tree; // expanded: no SmartAlbum references left
};

int parseQuery(const Options &opts, Database &db, QTextStream &err, Parsed &out)
{
    oql::MapVocabulary vocab;
    db.fillVocabulary(vocab);
    const oql::ParseResult parsed = oql::parse(opts.query, vocab);
    if (!parsed.ok()) {
        printParseError(parsed.error, opts.query, err);
        return kExitUsage;
    }
    // Readings that depended on the vocabulary are worth a warning: `dark forest`
    // was split only because no tag has that name. Under --strict they are errors.
    if (opts.strict && !parsed.notes.isEmpty()) {
        for (const oql::Note &n : parsed.notes)
            printStrictNote(n, opts.query, err);
        return kExitUsage;
    }
    for (const oql::Note &n : parsed.notes)
        err << "omapic: note: " << n.message << '\n';
    out.canonical = oql::format(parsed.root, vocab);

    // Smart-album references expand to the referenced album's current tree.
    const QHash<int, oql::Node> stored = db.smartAlbumQueries();
    QStringList problems;
    out.tree = oql::expand(parsed.root, [&stored](int id) -> const oql::Node * {
        const auto it = stored.constFind(id);
        return it == stored.constEnd() ? nullptr : &it.value();
    }, &problems);
    if (opts.strict) {
        collectMissing(out.tree, problems);
        if (!problems.isEmpty()) {
            for (const QString &p : problems) {
                oql::Error e;
                e.span = {0, int(opts.query.size())};
                e.message = p;
                printParseError(e, opts.query, err);
            }
            return kExitUsage;
        }
    }
    for (const QString &p : problems)
        err << "omapic: warning: " << p << '\n';
    return kExitMatches;
}

struct Resolved {
    QVector<Photo> photos;
    int missing = 0;
};

photosort::Mode sortMode(const QString &key)
{
    if (key == QStringLiteral("date"))
        return photosort::Mode::Date;
    if (key == QStringLiteral("tags"))
        return photosort::Mode::TagCount;
    return photosort::Mode::Name;
}

int resolveQuery(const Options &opts, Database &db, const oql::Node &tree, QTextStream &err, Resolved &out)
{
    const oql::EvalContext ctx = db.evalContext();
    for (const Photo &p : db.loadPhotos(/*activeOnly=*/!opts.includeDisabled)) {
        if (p.isDuplicate && !opts.allCopies)
            continue; // the flat view collapses copies by content hash
        if (!oql::matches(tree, p.tagIds, ctx))
            continue;
        if (!QFileInfo::exists(p.path)) {
            ++out.missing;
            continue;
        }
        out.photos.append(p);
    }
    if (out.missing > 0)
        err << "omapic: " << out.missing << (out.missing == 1 ? " matching file is" : " matching files are")
            << " missing from disk and skipped\n";

    const photosort::Mode mode = sortMode(opts.sort);
    const bool ascending = photosort::defaultAscending(mode) != opts.reverse;
    const QCollator collator = photosort::collator();
    std::stable_sort(out.photos.begin(), out.photos.end(), [&](const Photo &a, const Photo &b) {
        photosort::Keys ka, kb;
        ka.path = a.path; ka.fileName = a.fileName; ka.takenAt = a.takenAt; ka.tagCount = int(a.tagNames.size());
        kb.path = b.path; kb.fileName = b.fileName; kb.takenAt = b.takenAt; kb.tagCount = int(b.tagNames.size());
        return photosort::lessThan(ka, kb, mode, ascending, /*folderFirst=*/false, collator);
    });
    return out.photos.isEmpty() ? kExitNoMatches : kExitMatches;
}

QJsonObject photoJson(const Photo &p)
{
    QJsonObject o;
    o.insert(QStringLiteral("id"), p.id);
    o.insert(QStringLiteral("path"), p.path);
    o.insert(QStringLiteral("name"), p.fileName);
    o.insert(QStringLiteral("video"), p.isVideo);
    o.insert(QStringLiteral("favorite"), p.favorite);
    o.insert(QStringLiteral("takenAt"),
             p.takenAt > 0 ? QJsonValue(QDateTime::fromSecsSinceEpoch(p.takenAt).toString(Qt::ISODate))
                           : QJsonValue());
    o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(p.tagNames));
    return o;
}

// Names of one kind, sorted for people (case-insensitive), one per line.
void printNames(QStringList names, QTextStream &out)
{
    std::sort(names.begin(), names.end(),
              [](const QString &a, const QString &b) { return a.compare(b, Qt::CaseInsensitive) < 0; });
    names.removeDuplicates();
    for (const QString &n : names)
        out << n << '\n';
}

} // namespace

// --- Dispatch and parsing -------------------------------------------------------

bool isSubcommand(const QString &arg)
{
    return commandFor(arg) != Command::None;
}

bool isHeadless(Command c)
{
    return c == Command::Query || c == Command::Tags || c == Command::Albums || c == Command::Categories;
}

Command commandFor(const QString &arg)
{
    for (const CommandName &c : kCommands)
        if (arg == QLatin1String(c.name))
            return c.command;
    return Command::None;
}

Options parse(const QStringList &args)
{
    Options o;
    if (args.isEmpty())
        return o;
    o.command = commandFor(args.first());
    if (o.command == Command::None)
        return o;

    // Long options are recognised anywhere before `--`, even after the query
    // has started, so `query beach --count` works; an unknown `--long` argument
    // is a usage error rather than silently part of the query. Short options
    // (-0, -h) count only before the query, because `-0` is valid OQL (NOT the
    // tag "0") and `-people` starts a query. After `--` everything is query text.
    QStringList queryParts;
    bool literal = false;
    int shapes = 0; // --count / --json / -0 given
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        if (literal) {
            queryParts.append(a);
            continue;
        }
        if (a == QStringLiteral("--")) {
            literal = true;
            continue;
        }
        const bool isLong = a.startsWith(QStringLiteral("--")) && a.size() > 2;
        const bool isShort = !isLong && queryParts.isEmpty() && a.startsWith(QLatin1Char('-')) && a.size() == 2;
        QString name, value;
        bool hasValue = false;
        if (isLong) {
            name = a.mid(2);
            const int eq = name.indexOf(QLatin1Char('='));
            if (eq >= 0) {
                value = name.mid(eq + 1);
                name = name.left(eq);
                hasValue = true;
            }
        } else if (isShort) {
            name = a.mid(1);
        }
        const OptionSpec *spec = name.isEmpty() ? nullptr : findOption(name, o.command);
        if (!spec) {
            if (isLong) {
                o.error = QStringLiteral("Unknown option %1 (put -- before the query if it is query text)").arg(a);
                return o;
            }
            queryParts.append(a);
            continue;
        }
        if (spec->takesValue && !hasValue) {
            if (i + 1 >= args.size()) {
                o.error = QStringLiteral("Option --%1 needs a value").arg(QLatin1String(spec->name));
                return o;
            }
            value = args.at(++i);
        } else if (!spec->takesValue && hasValue) {
            o.error = QStringLiteral("Option --%1 takes no value").arg(QLatin1String(spec->name));
            return o;
        }

        const QLatin1String n(spec->name);
        if (n == QLatin1String("help")) {
            o.help = true;
        } else if (n == QLatin1String("library")) {
            if (value.isEmpty()) {
                o.error = QStringLiteral("--library needs a path");
                return o;
            }
            o.library = value;
        } else if (n == QLatin1String("count")) {
            o.output = Output::Count;
            ++shapes;
        } else if (n == QLatin1String("json")) {
            o.output = Output::Json;
            ++shapes;
        } else if (n == QLatin1String("0")) {
            o.output = Output::Nul;
            ++shapes;
        } else if (n == QLatin1String("explain")) {
            o.explain = true;
        } else if (n == QLatin1String("strict")) {
            o.strict = true;
        } else if (n == QLatin1String("sort")) {
            if (value != QStringLiteral("name") && value != QStringLiteral("date") && value != QStringLiteral("tags")) {
                o.error = QStringLiteral("--sort takes name, date or tags, not \"%1\"").arg(value);
                return o;
            }
            o.sort = value;
        } else if (n == QLatin1String("reverse")) {
            o.reverse = true;
        } else if (n == QLatin1String("all-copies")) {
            o.allCopies = true;
        } else if (n == QLatin1String("include-disabled")) {
            o.includeDisabled = true;
        } else if (n == QLatin1String("fullscreen")) {
            o.fullscreen = true;
        } else if (n == QLatin1String("interval")) {
            bool ok = false;
            const double secs = value.toDouble(&ok);
            if (!ok || secs <= 0) {
                o.error = QStringLiteral("--interval needs a number of seconds greater than 0, not \"%1\"").arg(value);
                return o;
            }
            o.intervalSecs = secs;
        } else if (n == QLatin1String("shuffle")) {
            o.shuffle = true;
        } else if (n == QLatin1String("shuffle-folders")) {
            o.shuffleFolders = true;
        } else if (n == QLatin1String("crossfade")) {
            o.crossfade = true;
        } else if (n == QLatin1String("start-random")) {
            o.startRandom = true;
        } else if (n == QLatin1String("loop-videos")) {
            o.loopVideos = true;
        }
    }
    if (o.help)
        return o;

    // Only one output shape makes sense at a time.
    if (shapes > 1) {
        o.error = QStringLiteral("--count, --json and -0 are mutually exclusive");
        return o;
    }

    // An unquoted query the shell split arrives as several arguments.
    o.query = queryParts.join(QLatin1Char(' ')).trimmed();
    if (takesQuery(o.command) && o.query.isEmpty())
        o.error = QStringLiteral("%1 needs a query").arg(commandName(o.command));
    else if (!takesQuery(o.command) && !o.query.isEmpty())
        o.error = QStringLiteral("%1 takes no arguments").arg(commandName(o.command));
    return o;
}

QString usage(Command command)
{
    QString s;
    QTextStream ts(&s);
    if (command == Command::None) {
        ts << "Usage: omapic [files...]                    the gallery, or a viewer when given files\n"
              "       omapic query [options] '<query>'     list the photos matching an OQL query\n"
              "       omapic tags | albums | categories    list names, one per line\n"
              "       omapic view [options] '<query>'      open the matches in the viewer\n"
              "       omapic slideshow [options] '<query>' play the matches as a slideshow\n"
              "\n"
              "Subcommands win over file arguments: open a file named \"query\" with\n"
              "`omapic ./query` or `omapic -- query`. Long options go anywhere before `--`\n"
              "(`omapic query beach --count`); short ones (-0, -h) only before the query, so\n"
              "`omapic query -people` is a query. Everything after `--` is query text. Run\n"
              "`omapic <subcommand> --help` for its options. Exit codes: 0 matches, 1 no\n"
              "matches, 2 usage or query error, 3 the library could not be opened.\n";
        return s;
    }
    const QString name = commandName(command);
    ts << "Usage: omapic " << name;
    bool any = false;
    for (const OptionSpec &o : kOptions)
        if ((o.scope & (1u << int(command))) && QLatin1String(o.name) != QLatin1String("help"))
            any = true;
    if (any)
        ts << " [options]";
    if (takesQuery(command))
        ts << " '<query>'";
    ts << "\n";
    switch (command) {
    case Command::Query:
        ts << "List the photos matching an OQL query, one path per line, in the gallery's\n"
              "flat-view order. Duplicate copies, disabled folders and files missing from\n"
              "disk are left out unless asked for.\n";
        break;
    case Command::Tags: ts << "List every tag name, for discovery and completion.\n"; break;
    case Command::Albums: ts << "List every album and smart album name.\n"; break;
    case Command::Categories: ts << "List every tag category name.\n"; break;
    case Command::View: ts << "Open the matching photos in the viewer.\n"; break;
    case Command::Slideshow:
        ts << "Play the matching photos as a slideshow: 4 seconds per photo, in order, with\n"
              "cuts between them, unless a flag says otherwise. The gallery's saved slideshow\n"
              "settings are not consulted.\n";
        break;
    case Command::None: break;
    }
    if (any) {
        ts << "\nOptions:\n";
        for (const OptionSpec &o : kOptions) {
            if (!(o.scope & (1u << int(command))))
                continue;
            // `-0`, `-h, --help`, or `--name`.
            QString flag = QStringLiteral("  ");
            const QLatin1String longName(o.name);
            if (o.shortName && longName.size() == 1)
                flag += QStringLiteral("-%1").arg(QLatin1Char(o.shortName));
            else if (o.shortName)
                flag += QStringLiteral("-%1, --%2").arg(QLatin1Char(o.shortName)).arg(longName);
            else
                flag += QStringLiteral("--%1").arg(longName);
            if (o.takesValue)
                flag += QStringLiteral(" <%1>").arg(QLatin1String(o.valueName));
            ts << flag.leftJustified(28) << QLatin1String(o.help) << '\n';
        }
    }
    if (takesQuery(command))
        ts << "\nThe query language: `a b` AND, `a | b` OR, `-a` NOT, `( )` groups, \"...\" quotes a\n"
              "name, is:fav, is:untagged, album:<name>, cat:<name>. NOT binds tightest, then\n"
              "OR, then AND. See the README for the full description.\n";
    return s;
}

QString defaultDatabasePath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)
        + QStringLiteral("/omapic/library.db");
}

QString libraryPath(const Options &opts)
{
    if (!opts.library.isEmpty())
        return opts.library;
    const QString env = qEnvironmentVariable(kLibraryEnv);
    if (!env.isEmpty())
        return env;
    return defaultDatabasePath();
}

// --- Commands -------------------------------------------------------------------

int runHeadless(const Options &opts, QTextStream &out, QTextStream &err)
{
    if (!opts.error.isEmpty()) {
        err << "omapic: " << opts.error << '\n' << usage(opts.command);
        return kExitUsage;
    }
    if (opts.help) {
        out << usage(opts.command);
        return kExitMatches;
    }
    Database db;
    const int opened = openLibrary(db, opts, err);
    if (opened != kExitMatches)
        return opened;

    if (opts.command != Command::Query) {
        oql::MapVocabulary vocab;
        db.fillVocabulary(vocab);
        QStringList names;
        switch (opts.command) {
        case Command::Tags:
            // Canonical names only: an alias is listed by the tag it stands for.
            for (const QString &n : vocab.names(oql::NameKind::Tag)) {
                const int id = vocab.resolve(oql::NameKind::Tag, oql::foldName(n));
                if (id >= 0 && vocab.name(oql::NameKind::Tag, id) == n)
                    names.append(n);
            }
            break;
        case Command::Albums:
            names = vocab.names(oql::NameKind::Album) + vocab.names(oql::NameKind::SmartAlbum);
            break;
        case Command::Categories:
            names = vocab.names(oql::NameKind::Category);
            break;
        default:
            break;
        }
        printNames(names, out);
        return names.isEmpty() ? kExitNoMatches : kExitMatches;
    }

    Parsed q;
    if (parseQuery(opts, db, err, q) != kExitMatches)
        return kExitUsage;
    if (opts.explain) {
        // How the query was read; whether anything matches is beside the point.
        out << q.canonical << '\n';
        return kExitMatches;
    }
    Resolved r;
    const int code = resolveQuery(opts, db, q.tree, err, r);
    switch (opts.output) {
    case Output::Count:
        out << r.photos.size() << '\n';
        break;
    case Output::Json: {
        QJsonArray arr;
        for (const Photo &p : r.photos)
            arr.append(photoJson(p));
        out << QJsonDocument(arr).toJson(QJsonDocument::Indented);
        break;
    }
    case Output::Nul:
        for (const Photo &p : r.photos)
            out << p.path << QChar(u'\0');
        break;
    case Output::Paths:
        for (const Photo &p : r.photos)
            out << p.path << '\n';
        break;
    }
    return code;
}

int resolveMedia(const Options &opts, QList<QUrl> &urls, QTextStream &err)
{
    if (!opts.error.isEmpty()) {
        err << "omapic: " << opts.error << '\n' << usage(opts.command);
        return kExitUsage;
    }
    Database db;
    const int opened = openLibrary(db, opts, err);
    if (opened != kExitMatches)
        return opened;
    Parsed q;
    if (parseQuery(opts, db, err, q) != kExitMatches)
        return kExitUsage;
    Resolved r;
    const int code = resolveQuery(opts, db, q.tree, err, r);
    if (code != kExitMatches) {
        if (code == kExitNoMatches)
            err << "omapic: nothing matches " << q.canonical << '\n';
        return code;
    }
    for (const Photo &p : r.photos)
        urls.append(QUrl::fromLocalFile(p.path));
    return kExitMatches;
}

SlideshowSettings slideshowSettings(const Options &opts)
{
    SlideshowSettings s;
    s.intervalMs = std::max(1, int(opts.intervalSecs * 1000.0 + 0.5));
    s.shuffle = opts.shuffle;
    s.shuffleFolders = opts.shuffleFolders;
    s.fade = opts.crossfade;
    s.randomStart = opts.startRandom;
    s.loop = opts.loopVideos;
    return s;
}

int orderPlaylist(QList<QUrl> &urls, const SlideshowSettings &s, quint32 seed)
{
    if (urls.isEmpty())
        return 0;
    QRandomGenerator local(seed);
    QRandomGenerator *rng = seed ? &local : QRandomGenerator::global();
    auto shuffleRange = [&](QList<QUrl> &list) {
        for (int i = list.size() - 1; i > 0; --i)
            list.swapItemsAt(i, int(rng->bounded(i + 1)));
    };
    if (s.shuffleFolders) {
        // Group by containing folder (keeping each folder's order, or shuffling
        // within it when shuffle is on too) and visit the folders in random order.
        QStringList folders;
        QHash<QString, QList<QUrl>> byFolder;
        for (const QUrl &u : urls) {
            const QString p = u.toLocalFile();
            const QString dir = p.left(p.lastIndexOf(QLatin1Char('/')));
            if (!byFolder.contains(dir))
                folders.append(dir);
            byFolder[dir].append(u);
        }
        for (int i = folders.size() - 1; i > 0; --i)
            folders.swapItemsAt(i, int(rng->bounded(i + 1)));
        urls.clear();
        for (const QString &dir : folders) {
            QList<QUrl> &group = byFolder[dir];
            if (s.shuffle)
                shuffleRange(group);
            urls += group;
        }
        return 0;
    }
    if (s.shuffle) {
        shuffleRange(urls);
        return 0;
    }
    if (s.randomStart)
        return int(rng->bounded(urls.size()));
    return 0;
}

} // namespace cli
