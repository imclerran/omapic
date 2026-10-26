// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>
#include <initializer_list>

// OQL — the Omapic Query Language. A small boolean language over a photo's tags
// (and, later, its metadata) that backs smart albums, the command-line `query`
// tool and the GUI's query builder. Every consumer goes through this one engine
// so a query reads the same everywhere.
//
// Syntax
//   a b          AND (juxtaposition)
//   a | b        OR
//   -a           NOT ('-' is syntax only at the start of a term: t-shirt is a tag)
//   ( … )        grouping ('(' at the start of a term always opens a group)
//   "…"          a literal name, never syntax
//   is:fav       in Favorites
//   is:untagged  no user-visible tags (album / Favorites membership don't count)
//   album:<name> in a manual album or a smart album, by name
//   cat:<name>   carries any tag of that category
// Precedence, tightest first: NOT, OR, AND — so `beach sunset | sunrise` means
// `beach (sunset | sunrise)`. There are no word operators (AND/OR/NOT collide
// with real tag names).
//
// Names are resolved by longest match against the vocabulary: at each term
// position the longest known tag/alias (or album/category after the predicate
// key) whose text starts there and ends at a boundary (end of input, whitespace,
// '|', ')' or '"') wins, so names with spaces and parentheses need no quoting.
// Syntax wins at the start of a term — a leading '-' is NOT and a leading '('
// opens a group whatever tags exist — so a name starting with either must be
// quoted ("-20 °C", "(untitled)"). Inside a match, '(' and '-' are ordinary.
// Aliases resolve to their canonical tag at parse time. An unknown name is an
// error (with "did you mean" suggestions), never a silent fallback.
namespace oql {

// Which name table a name belongs to.
enum class NameKind { Tag, Album, SmartAlbum, Category };

// The folding every name goes through before lookup, on both the vocabulary
// and the query side: accent-/case-folded (textfold.h) with whitespace runs
// collapsed to one space and the ends trimmed. Tag names are stored that
// tidy already (see Database's sanitizeName), so folding a stored name is a
// no-op apart from the case/accent folding.
QString foldName(const QString &s);

// The name tables a query is parsed and formatted against. Abstract so the
// engine can be driven from a database snapshot, a hand-built map in tests, or
// anything else; the parser never touches a database itself.
class Vocabulary {
public:
    virtual ~Vocabulary() = default;
    // The id a folded name (see foldName) resolves to, or -1 when unknown. For
    // Tag, alias names resolve to the canonical tag's id.
    virtual int resolve(NameKind kind, const QString &folded) const = 0;
    // The current display name of an id, empty when unknown.
    virtual QString name(NameKind kind, int id) const = 0;
    // Every display name of a kind (for Tag, alias names included). Used for
    // "did you mean" suggestions and for deciding when the formatter must quote.
    virtual QStringList names(NameKind kind) const = 0;
};

// A Vocabulary over plain in-memory tables. The app fills one from the database
// (tags, aliases, albums, smart albums, categories); tests fill it by hand.
class MapVocabulary : public Vocabulary {
public:
    void addTag(int id, const QString &name) { add(NameKind::Tag, id, name); }
    void addAlias(const QString &alias, int tagId);
    void addAlbum(int id, const QString &name) { add(NameKind::Album, id, name); }
    void addSmartAlbum(int id, const QString &name) { add(NameKind::SmartAlbum, id, name); }
    void addCategory(int id, const QString &name) { add(NameKind::Category, id, name); }
    void add(NameKind kind, int id, const QString &name);

    int resolve(NameKind kind, const QString &folded) const override;
    QString name(NameKind kind, int id) const override;
    QStringList names(NameKind kind) const override;

private:
    struct Table {
        QHash<QString, int> byFolded; // folded name (incl. aliases) -> id
        QHash<int, QString> byId;     // id -> display name (canonical only)
        QStringList allNames;         // display names, canonical then aliases
    };
    const Table &table(NameKind kind) const { return m_tables[int(kind)]; }
    Table m_tables[4];
};

// --- The AST ----------------------------------------------------------------

enum class NodeKind {
    Tag,        // id = tag id
    Album,      // id = album tag id (a manual album)
    SmartAlbum, // id = smart album id; a reference, expanded at evaluation time
    Category,   // id = category id
    Favorite,   // is:fav
    Untagged,   // is:untagged
    Missing,    // a name whose tag/album/category no longer exists; never matches
    Not,        // one child
    And,        // two or more children
    Or,         // two or more children
};

// A character range in the query text (for error carets and highlighting).
struct Span {
    int start = 0;
    int length = 0;
    int end() const { return start + length; }
};

struct Node {
    NodeKind kind = NodeKind::Tag;
    int id = -1;
    // The display name the term was resolved to at parse time (the canonical
    // tag, never the alias). For Missing it is the last known name. The
    // formatter prefers the vocabulary's current name for the id when it has one.
    QString name;
    // For Missing: what the name used to denote (so it formats as album:"…",
    // cat:"…" or a plain quoted tag).
    NameKind was = NameKind::Tag;
    QVector<Node> children;
    // Where the term (or the whole sub-expression) sits in the parsed text.
    // Zero for a node built by hand. Not part of equality.
    Span span;

    bool isLeaf() const { return children.isEmpty(); }

    // Structural equality: kind, id, name, children (and `was` for Missing).
    // Spans are ignored, so a re-parsed query compares equal to the original.
    bool operator==(const Node &o) const;
    bool operator!=(const Node &o) const { return !(*this == o); }

    // Convenience constructors for hand-built trees (tests, smart-album
    // conversion). `name` is optional for the id-based kinds.
    static Node tag(int id, const QString &name = {});
    static Node album(int id, const QString &name = {});
    static Node smartAlbum(int id, const QString &name = {});
    static Node category(int id, const QString &name = {});
    static Node favorite();
    static Node untagged();
    static Node missing(const QString &name, NameKind was = NameKind::Tag);
    static Node notOf(Node child);
    static Node andOf(QVector<Node> children);
    static Node orOf(QVector<Node> children);
};

// --- Parsing ----------------------------------------------------------------

struct Error {
    Span span;
    QString message;
    QStringList suggestions; // "did you mean" candidates, best first (may be empty)
    bool isNull() const { return message.isEmpty(); }
};

// An explanation of a reading that depended on the vocabulary rather than on
// syntax alone: a run of words read as one multi-word tag, or a run split into
// several tags because no longer name matched. The CLI prints these as
// warnings so a user can see when `dark forest` meant two tags.
struct Note {
    Span span;
    QString message;
};

struct ParseResult {
    Node root;           // meaningful only when ok()
    Error error;         // set when parsing failed
    QVector<Note> notes; // vocabulary-dependent readings, in text order
    bool ok() const { return error.isNull(); }
};

// Parse `text` against `vocab`. Never throws; an empty or blank query is an
// error ("empty query") since no consumer has a use for one.
ParseResult parse(const QString &text, const Vocabulary &vocab);

// --- Formatting -------------------------------------------------------------

// The canonical text of a tree: terms by their current names, one space
// between AND operands, ` | ` between OR operands, parentheses only where the
// structure needs them (and around an OR inside an AND, for readability), and
// a name quoted only where the parser would otherwise read it differently —
// because it starts with '-' or '(', looks like a predicate, or because a
// longer known name would swallow the words that follow it. Everything else is
// left bare, so `dinner (2019)` stays `dinner (2019)`. parse(format(t)) == t
// for any tree whose names are in the vocabulary.
QString format(const Node &root, const Vocabulary &vocab);

// --- Evaluation -------------------------------------------------------------

// The facts about the library that evaluation needs beyond a photo's own tag
// ids. Built once per filter pass, not per photo.
struct EvalContext {
    int favoritesTagId = -1;
    // Every album tag id (Favorites included): the hidden tags that don't count
    // as user-visible for is:untagged.
    QSet<int> albumTagIds;
    // tag id -> category id, for categorized tags only (cat:<name>).
    QHash<int, int> tagCategory;
};

// True when a photo carrying exactly `photoTags` satisfies `root`. A Missing
// node never matches; a SmartAlbum reference is treated as Missing here — it
// must be expanded (replaced by the referenced album's tree) first.
bool matches(const Node &root, const QSet<int> &photoTags, const EvalContext &ctx);

// --- Tag sets ---------------------------------------------------------------

// The tree for a plain tag set: `a b c` (matchAll) or `a | b | c`; a single tag
// is just that tag; an empty set is an empty AND, which matches nothing. Names
// are filled from `vocab` when one is given. This is what the sidebar's all/any
// selection and the legacy smart-album form become.
Node fromTagSet(const QList<int> &tagIds, bool matchAll, const Vocabulary *vocab = nullptr);
// The inverse, when it exists: true when `root` is a single tag or a flat
// AND/OR of tags, filling `tagIds` and `matchAll`. Anything else (negation,
// predicates, nesting, a missing name) returns false. Lets the simple tag-set
// editor show the queries it can represent and hand the rest to the text form.
bool asTagSet(const Node &root, QList<int> &tagIds, bool &matchAll);

// --- Persistence ------------------------------------------------------------

// The stored form of a tree: JSON of ids ({"k":"tag","id":5,"n":"beach"},
// {"k":"and","c":[…]} …). Names are recorded so a deleted id can become a
// Missing node that still says what it was; they are never authoritative.
QJsonObject toJson(const Node &root);
// Parses the stored form; false (and `out` untouched) on anything malformed.
bool fromJson(const QJsonObject &json, Node &out);
QByteArray toJsonText(const Node &root);                // compact UTF-8
bool fromJsonText(const QByteArray &text, Node &out);

// --- Maintenance ------------------------------------------------------------

// The ids of every leaf of the given kinds (Tag and Album leaves both hold tag
// ids; SmartAlbum and Category leaves hold their own ids).
QSet<int> referencedIds(const Node &root, std::initializer_list<NodeKind> kinds);
// Point every Tag/Album leaf with id `from` at `to` (a tag merge), then drop
// the exact duplicates that can leave in an AND/OR. True when anything changed.
bool replaceTagId(Node &root, int from, int to);
// Turn every leaf of `kind` with id `id` into a Missing node keeping `name` (a
// deleted tag, album, category or smart album). True when anything changed.
bool markMissing(Node &root, NodeKind kind, int id, const QString &name);

// --- Smart-album references -------------------------------------------------

// Looks up a smart album's stored (unexpanded) tree by id; nullptr if unknown.
using SmartAlbumLookup = std::function<const Node *(int)>;

// Replace every SmartAlbum reference with the referenced album's current tree,
// recursively, so editing a nested album changes every album that uses it.
// A reference to an unknown album becomes Missing. A cycle (A → B → A) is an
// error: the reference that closes it becomes Missing and a message is added to
// `errors` (when given). `selfId` is the id of the album `root` belongs to, so
// a chain leading back to it counts as a cycle too (-1 for a free query).
// Expand once per filter pass and evaluate photos against the result; never
// per photo.
Node expand(const Node &root, const SmartAlbumLookup &lookup, QStringList *errors = nullptr,
            int selfId = -1);
// True when storing `query` as smart album `albumId` would let it reach itself
// through any chain of references (including directly).
bool wouldCycle(int albumId, const Node &query, const SmartAlbumLookup &lookup);

} // namespace oql
