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
#include "oqlsyntax.h"
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
    {"completion", Command::Completion},
};

// Which commands an option belongs to.
enum Scope : unsigned {
    All = 0xff,
    Query_ = 1u << int(Command::Query),
    View_ = 1u << int(Command::View),
    Slideshow_ = 1u << int(Command::Slideshow),
    WithQuery = Query_ | View_ | Slideshow_,
    Gui = View_ | Slideshow_,
    Names = (1u << int(Command::Tags)) | (1u << int(Command::Albums)) | (1u << int(Command::Categories)),
    WithLibrary = WithQuery | Names, // every command that reads the library
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
    {"library", 0, true, WithLibrary, "path", "The library to read (default: $OMAPIC_LIBRARY, else the gallery's)."},
    {"count", 0, false, Query_, "", "Print only the number of matches."},
    {"counts", 0, false, Names, "", "Print each name, a tab, and its number of photos."},
    {"tsv", 0, false, Names, "", "Tab-separated: name, photos, then a tag's category or an album's kind."},
    {"json", 0, false, Query_ | Names, "", "Print an indented JSON array of objects."},
    {"jsonl", 0, false, Query_ | Names, "", "Print one compact JSON object per line."},
    {"0", '0', false, Query_, "", "Separate paths with NUL instead of newline (for xargs -0)."},
    {"explain", 0, false, Query_, "", "Print how the query was read instead of matches (exit 0 when it parses)."},
    {"strict", 0, false, WithQuery, "", "Refuse a reading that depends on your tag names or a broken smart album."},
    {"sort", 0, true, WithQuery, "name|date|tags", "Order, as the gallery sorts: name (default), date, tags."},
    {"reverse", 0, false, WithQuery, "", "Reverse the order."},
    {"all-copies", 0, false, WithQuery | Names, "", "Keep duplicate copies of the same content."},
    {"include-disabled", 0, false, WithQuery | Names, "", "Include photos under disabled folders."},
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

// Where the name starts in a query word: after a leading NOT run and an
// album:/cat: key (an is: value never holds a name). The key's index in
// kPredicates is returned through `key`, -1 when there is none.
int nameStart(const QString &word, int *key = nullptr)
{
    int start = 0;
    while (start < word.size() && word.at(start) == QLatin1Char('-'))
        ++start;
    const int k = oql::syntax::predicateKeyAt(word, start);
    if (k >= 0 && oql::syntax::kPredicates[k].pred != oql::syntax::Predicate::Is)
        start += int(qstrlen(oql::syntax::kPredicates[k].key)) + 1;
    if (key)
        *key = k;
    return start;
}

// A lone query argument arrives as typed, minus the quotes the shell ate:
// `omapic query "dark forest"` reaches us as `dark forest`, a bare run of
// words the parser would read by longest match and note as a guess. When the
// whole argument — after a leading NOT and a predicate key — is exactly one
// known name, it was that quoted name, so it is quoted in OQL before parsing.
// Anything else (several words, syntax, text already quoted) is still the
// whole query as typed.
QString quoteLoneName(const QString &query, const oql::Vocabulary &vocab)
{
    const QString trimmed = query.trimmed();
    if (trimmed.contains(QLatin1Char('"')))
        return query;
    int key = -1;
    const int start = nameStart(trimmed, &key);
    const QString name = trimmed.mid(start).trimmed();
    bool space = false;
    for (const QChar c : name)
        if (c.isSpace()) { space = true; break; }
    if (!space)
        return query;
    QVector<oql::NameKind> kinds{oql::NameKind::Tag};
    if (key >= 0)
        kinds = oql::syntax::kPredicates[key].pred == oql::syntax::Predicate::Album
                    ? QVector<oql::NameKind>{oql::NameKind::Album, oql::NameKind::SmartAlbum}
                    : QVector<oql::NameKind>{oql::NameKind::Category};
    const QString folded = oql::foldName(name);
    for (const oql::NameKind kind : kinds)
        if (vocab.resolve(kind, folded) >= 0)
            return trimmed.left(start) + QLatin1Char('"') + name + QLatin1Char('"');
    return query;
}

int parseQuery(const Options &opts, Database &db, QTextStream &err, Parsed &out)
{
    oql::MapVocabulary vocab;
    db.fillVocabulary(vocab);
    const QString query = quoteLoneName(opts.query, vocab);
    const oql::ParseResult parsed = oql::parse(query, vocab);
    if (!parsed.ok()) {
        printParseError(parsed.error, query, err);
        return kExitUsage;
    }
    // Readings that depended on the vocabulary are worth a warning: `dark forest`
    // was split only because no tag has that name. Under --strict they are errors.
    if (opts.strict && !parsed.notes.isEmpty()) {
        for (const oql::Note &n : parsed.notes)
            printStrictNote(n, query, err);
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
                e.span = {0, int(query.size())};
                e.message = p;
                printParseError(e, query, err);
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
    // In UTC with a Z suffix, so the value means the same wherever it is read.
    o.insert(QStringLiteral("takenAt"),
             p.takenAt > 0 ? QJsonValue(QDateTime::fromSecsSinceEpoch(p.takenAt).toUTC().toString(Qt::ISODate))
                           : QJsonValue());
    o.insert(QStringLiteral("tags"), QJsonArray::fromStringList(p.tagNames));
    return o;
}

// The indented array (--json) or one compact object per line (--jsonl).
void printJson(const QVector<QJsonObject> &objects, Output format, QTextStream &out)
{
    if (format == Output::Jsonl) {
        for (const QJsonObject &o : objects)
            out << QJsonDocument(o).toJson(QJsonDocument::Compact) << '\n';
        return;
    }
    QJsonArray arr;
    for (const QJsonObject &o : objects)
        arr.append(o);
    out << QJsonDocument(arr).toJson(QJsonDocument::Indented);
}

// --- The name lists ---------------------------------------------------------------

// One line of `tags`, `albums` or `categories`: the name, the number of photos
// in scope that carry it, and the object --json / --jsonl print.
struct NameRow {
    QString name;
    int count = 0;
    QString extra; // --tsv's third field: a tag's category, an album's kind
    QJsonObject json;
};

// The photos a name list counts: query's default scope (one copy per content,
// enabled folders, files on disk), widened by the same flags.
QVector<Photo> scopedPhotos(const Options &opts, Database &db)
{
    QVector<Photo> out;
    for (const Photo &p : db.loadPhotos(/*activeOnly=*/!opts.includeDisabled)) {
        if (p.isDuplicate && !opts.allCopies)
            continue;
        if (!QFileInfo::exists(p.path))
            continue;
        out.append(p);
    }
    return out;
}

QVector<NameRow> nameRows(const Options &opts, Database &db)
{
    oql::MapVocabulary vocab;
    db.fillVocabulary(vocab);
    // Counting walks every photo; the plain list doesn't need to.
    const bool counting = opts.output != Output::Plain;
    QVector<Photo> photos;
    oql::EvalContext ctx;
    QHash<int, int> byTag;      // tag id (album tags included) -> photos carrying it
    QHash<int, int> byCategory; // category id -> photos with any tag in it
    if (counting) {
        photos = scopedPhotos(opts, db);
        ctx = db.evalContext();
        for (const Photo &p : photos) {
            QSet<int> categories;
            for (int t : p.tagIds) {
                ++byTag[t];
                const int c = ctx.tagCategory.value(t, -1);
                if (c >= 0)
                    categories.insert(c);
            }
            for (int c : categories)
                ++byCategory[c];
        }
    }
    auto nullable = [](const QString &s) { return s.isEmpty() ? QJsonValue() : QJsonValue(s); };

    QVector<NameRow> rows;
    switch (opts.command) {
    case Command::Tags: {
        QHash<int, QStringList> aliases; // by tag id, in alias-name order
        for (const AliasInfo &a : db.loadAliases())
            aliases[a.tagId].append(a.aliasName);
        // Canonical names only: an alias is listed by the tag it stands for.
        for (const QString &n : vocab.names(oql::NameKind::Tag)) {
            const int id = vocab.resolve(oql::NameKind::Tag, oql::foldName(n));
            if (id < 0 || vocab.name(oql::NameKind::Tag, id) != n)
                continue;
            NameRow r;
            r.name = n;
            r.count = byTag.value(id);
            const int category = ctx.tagCategory.value(id, -1);
            if (category >= 0)
                r.extra = vocab.name(oql::NameKind::Category, category);
            r.json = {
                {QStringLiteral("name"), n},
                {QStringLiteral("category"), nullable(category >= 0 ? vocab.name(oql::NameKind::Category, category) : QString())},
                {QStringLiteral("count"), r.count},
                {QStringLiteral("aliases"), QJsonArray::fromStringList(aliases.value(id))},
            };
            rows.append(r);
        }
        break;
    }
    case Command::Albums: {
        for (const QString &n : vocab.names(oql::NameKind::Album)) {
            NameRow r;
            r.name = n;
            r.count = byTag.value(vocab.resolve(oql::NameKind::Album, oql::foldName(n)));
            r.extra = QStringLiteral("album");
            r.json = {
                {QStringLiteral("name"), n},
                {QStringLiteral("smart"), false},
                {QStringLiteral("count"), r.count},
                {QStringLiteral("query"), QJsonValue()},
            };
            rows.append(r);
        }
        // A smart album's count is its query evaluated over the same photos.
        const QHash<int, oql::Node> stored = db.smartAlbumQueries();
        const auto lookup = [&stored](int id) -> const oql::Node * {
            const auto it = stored.constFind(id);
            return it == stored.constEnd() ? nullptr : &it.value();
        };
        for (const SmartAlbumInfo &sa : db.smartAlbums()) {
            NameRow r;
            r.name = sa.name;
            r.extra = QStringLiteral("smart album");
            if (counting) {
                const oql::Node tree = oql::expand(sa.query, lookup, nullptr, sa.id);
                for (const Photo &p : photos)
                    if (oql::matches(tree, p.tagIds, ctx))
                        ++r.count;
            }
            r.json = {
                {QStringLiteral("name"), sa.name},
                {QStringLiteral("smart"), true},
                {QStringLiteral("count"), r.count},
                {QStringLiteral("query"), oql::format(sa.query, vocab)},
            };
            rows.append(r);
        }
        break;
    }
    case Command::Categories:
        for (const QString &n : vocab.names(oql::NameKind::Category)) {
            NameRow r;
            r.name = n;
            r.count = byCategory.value(vocab.resolve(oql::NameKind::Category, oql::foldName(n)));
            r.json = {{QStringLiteral("name"), n}, {QStringLiteral("count"), r.count}};
            rows.append(r);
        }
        break;
    default:
        break;
    }
    // Sorted for people (case-insensitive).
    std::stable_sort(rows.begin(), rows.end(), [](const NameRow &a, const NameRow &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return rows;
}

int runNameList(const Options &opts, Database &db, QTextStream &out)
{
    const QVector<NameRow> rows = nameRows(opts, db);
    switch (opts.output) {
    case Output::Json:
    case Output::Jsonl: {
        QVector<QJsonObject> objects;
        for (const NameRow &r : rows)
            objects.append(r.json);
        printJson(objects, opts.output, out);
        break;
    }
    case Output::Counts:
        for (const NameRow &r : rows)
            out << r.name << '\t' << r.count << '\n';
        break;
    case Output::Tsv:
        for (const NameRow &r : rows) {
            out << r.name << '\t' << r.count;
            if (opts.command != Command::Categories)
                out << '\t' << r.extra;
            out << '\n';
        }
        break;
    default: {
        QString last;
        for (const NameRow &r : rows) {
            if (r.name == last)
                continue;
            out << r.name << '\n';
            last = r.name;
        }
        break;
    }
    }
    return rows.isEmpty() ? kExitNoMatches : kExitMatches;
}

} // namespace

// --- Dispatch and parsing -------------------------------------------------------

// One shell argument of a multi-argument query, with OQL quotes added when it
// holds whitespace and none of its own: `-dark forest` → `-"dark forest"`,
// `album:Best of 2019` → `album:"Best of 2019"`. Anything else passes through.
QString quoteShellWord(const QString &part)
{
    const QString trimmed = part.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('"')))
        return part;
    bool space = false;
    for (const QChar c : trimmed)
        if (c.isSpace()) { space = true; break; }
    if (!space)
        return part;
    // Keep a leading NOT and a predicate key outside the quotes.
    const int start = nameStart(trimmed);
    const QString name = trimmed.mid(start).trimmed();
    if (name.isEmpty())
        return part;
    return trimmed.left(start) + QLatin1Char('"') + name + QLatin1Char('"');
}

bool isSubcommand(const QString &arg)
{
    return commandFor(arg) != Command::None;
}

bool isHeadless(Command c)
{
    return c == Command::Query || c == Command::Tags || c == Command::Albums || c == Command::Categories
        || c == Command::Completion;
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
    int shapes = 0; // output shapes given (--count, --json, --jsonl, -0, --counts)
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
        } else if (n == QLatin1String("counts")) {
            o.output = Output::Counts;
            ++shapes;
        } else if (n == QLatin1String("tsv")) {
            o.output = Output::Tsv;
            ++shapes;
        } else if (n == QLatin1String("json")) {
            o.output = Output::Json;
            ++shapes;
        } else if (n == QLatin1String("jsonl")) {
            o.output = Output::Jsonl;
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
        o.error = o.command == Command::Query
            ? QStringLiteral("--count, --json, --jsonl and -0 are mutually exclusive")
            : QStringLiteral("--counts, --tsv, --json and --jsonl are mutually exclusive");
        return o;
    }

    // `completion` takes the shell's name instead of a query.
    if (o.command == Command::Completion) {
        if (queryParts.size() != 1 || completionScript(queryParts.first()).isEmpty())
            o.error = QStringLiteral("completion takes one of zsh, bash or fish");
        else
            o.shell = queryParts.first();
        return o;
    }
    // An unquoted query the shell split arrives as several arguments. Among
    // several, an argument that still contains whitespace was quoted in the shell
    // ("dark forest"), and the shell has already eaten those quotes — so quote
    // it in OQL, or the parser would see a bare run of words and guess. A lone
    // argument is the whole query as typed.
    if (queryParts.size() > 1)
        for (QString &part : queryParts)
            part = quoteShellWord(part);
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
              "       omapic completion zsh|bash|fish      print a shell completion script\n"
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
    else if (command == Command::Completion)
        ts << " zsh|bash|fish";
    ts << "\n";
    switch (command) {
    case Command::Query:
        ts << "List the photos matching an OQL query, one path per line, in the gallery's\n"
              "flat-view order. Duplicate copies, disabled folders and files missing from\n"
              "disk are left out unless asked for.\n";
        break;
    case Command::Tags:
    case Command::Albums:
    case Command::Categories:
        ts << (command == Command::Tags ? "List every tag name, one per line, for discovery and completion.\n"
               : command == Command::Albums ? "List every album and smart album name, one per line.\n"
                                            : "List every tag category name, one per line.\n")
           << "Counts (--counts, --json, --jsonl) use query's default scope: one copy per\n"
              "content, enabled folders, files on disk.\n";
        break;
    case Command::View: ts << "Open the matching photos in the viewer.\n"; break;
    case Command::Slideshow:
        ts << "Play the matching photos as a slideshow: 4 seconds per photo, in order, with\n"
              "cuts between them, unless a flag says otherwise. The gallery's saved slideshow\n"
              "settings are not consulted.\n";
        break;
    case Command::Completion:
        ts << "Print a completion script for the shell to stdout; see the README for where\n"
              "to install it. The script completes subcommands, options and query words\n"
              "(tags, album: and cat: names, is: values), fetching the names from omapic\n"
              "itself as you type.\n";
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

QVector<OptionInfo> optionsFor(Command command)
{
    QVector<OptionInfo> out;
    for (const OptionSpec &o : kOptions) {
        if (!(o.scope & (1u << int(command))))
            continue;
        OptionInfo info;
        info.name = QLatin1String(o.name);
        info.shortName = o.shortName ? QLatin1Char(o.shortName) : QChar();
        info.takesValue = o.takesValue;
        info.valueName = QLatin1String(o.valueName);
        info.help = QLatin1String(o.help);
        out.append(info);
    }
    return out;
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
    if (opts.command == Command::Completion) {
        out << completionScript(opts.shell);
        return kExitMatches;
    }
    Database db;
    const int opened = openLibrary(db, opts, err);
    if (opened != kExitMatches)
        return opened;

    if (opts.command != Command::Query)
        return runNameList(opts, db, out);

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
    case Output::Json:
    case Output::Jsonl: {
        QVector<QJsonObject> objects;
        for (const Photo &p : r.photos)
            objects.append(photoJson(p));
        printJson(objects, opts.output, out);
        break;
    }
    case Output::Nul:
        for (const Photo &p : r.photos)
            out << p.path << QChar(u'\0');
        break;
    default:
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
