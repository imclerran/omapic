// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include "oql.h"

#include "oqlsyntax.h"
#include "textfold.h"

#include <QChar>

namespace oql {

QString foldName(const QString &s)
{
    return foldText(s).simplified();
}

// --- MapVocabulary -------------------------------------------------------------

void MapVocabulary::add(NameKind kind, int id, const QString &name)
{
    Table &t = m_tables[int(kind)];
    t.byFolded.insert(foldName(name), id);
    t.byId.insert(id, name);
    t.allNames.append(name);
}

void MapVocabulary::addAlias(const QString &alias, int tagId)
{
    Table &t = m_tables[int(NameKind::Tag)];
    t.byFolded.insert(foldName(alias), tagId);
    t.allNames.append(alias);
}

int MapVocabulary::resolve(NameKind kind, const QString &folded) const
{
    return table(kind).byFolded.value(folded, -1);
}

QString MapVocabulary::name(NameKind kind, int id) const
{
    return table(kind).byId.value(id);
}

QStringList MapVocabulary::names(NameKind kind) const
{
    return table(kind).allNames;
}

// --- Node ------------------------------------------------------------------------

bool Node::operator==(const Node &o) const
{
    if (kind != o.kind || id != o.id || name != o.name || children != o.children)
        return false;
    return kind != NodeKind::Missing || was == o.was;
}

Node Node::tag(int id, const QString &name)
{
    Node n;
    n.kind = NodeKind::Tag;
    n.id = id;
    n.name = name;
    return n;
}

Node Node::album(int id, const QString &name)
{
    Node n;
    n.kind = NodeKind::Album;
    n.id = id;
    n.name = name;
    return n;
}

Node Node::smartAlbum(int id, const QString &name)
{
    Node n;
    n.kind = NodeKind::SmartAlbum;
    n.id = id;
    n.name = name;
    return n;
}

Node Node::category(int id, const QString &name)
{
    Node n;
    n.kind = NodeKind::Category;
    n.id = id;
    n.name = name;
    return n;
}

Node Node::favorite()
{
    Node n;
    n.kind = NodeKind::Favorite;
    return n;
}

Node Node::untagged()
{
    Node n;
    n.kind = NodeKind::Untagged;
    return n;
}

Node Node::missing(const QString &name, NameKind was)
{
    Node n;
    n.kind = NodeKind::Missing;
    n.name = name;
    n.was = was;
    return n;
}

Node Node::notOf(Node child)
{
    Node n;
    n.kind = NodeKind::Not;
    n.children.append(std::move(child));
    return n;
}

Node Node::andOf(QVector<Node> children)
{
    Node n;
    n.kind = NodeKind::And;
    n.children = std::move(children);
    return n;
}

Node Node::orOf(QVector<Node> children)
{
    Node n;
    n.kind = NodeKind::Or;
    n.children = std::move(children);
    return n;
}

// --- Formatting -----------------------------------------------------------------

namespace {

// The name kind a leaf's text is resolved against when it is read back.
NameKind leafNameKind(const Node &n)
{
    switch (n.kind) {
    case NodeKind::Album: return NameKind::Album;
    case NodeKind::SmartAlbum: return NameKind::SmartAlbum;
    case NodeKind::Category: return NameKind::Category;
    case NodeKind::Missing: return n.was;
    default: return NameKind::Tag;
    }
}

// The display name to print for a leaf: the vocabulary's current name for the
// id when it has one, else the name recorded in the node.
QString leafName(const Node &n, const Vocabulary &vocab)
{
    if (n.kind == NodeKind::Missing || n.id < 0)
        return n.name;
    const QString current = vocab.name(leafNameKind(n), n.id);
    return current.isEmpty() ? n.name : current;
}

// A formatted piece of output: either literal syntax or a name that may still
// need quoting. Names are decided in a right-to-left pass once the text that
// follows each is known (quoting a later name changes what an earlier one is
// followed by, since '"' is a boundary).
struct Piece {
    QString text;
    bool isName = false;
    NameKind kind = NameKind::Tag;
    bool prefixed = false; // after album:/cat:, where tag-start syntax doesn't apply
};

// Whether a name must be quoted on its own account, regardless of what follows
// it: because it starts with the syntax the parser honours at a term start,
// looks like a predicate, or contains a character that would end the term.
bool needsQuoteAlone(const QString &name, bool prefixed)
{
    if (name.isEmpty())
        return true;
    const QChar first = name.at(0);
    if (first == QLatin1Char('-') || first == QLatin1Char('(') || first == QLatin1Char('"'))
        return true;
    if (name.contains(QLatin1Char('|')) || name.contains(QLatin1Char('"')))
        return true; // '|' always ends a term; '"' can't be quoted at all
    if (name.trimmed() != name || name.contains(QLatin1Char('\n')))
        return true;
    if (!prefixed && syntax::predicateKeyAt(name, 0) >= 0)
        return true;
    return false;
}

// True when, read bare and followed by `rest`, a longer known name of `kind`
// would be chosen in place of `name`: some candidate ending at a boundary inside
// `rest` resolves. Only the text up to the first '"' in rest can be swallowed
// (a quote always ends a term).
bool longerMatchFollows(const QString &name, NameKind kind, const QString &rest,
                        const Vocabulary &vocab)
{
    const int quote = rest.indexOf(QLatin1Char('"'));
    const QString reach = quote < 0 ? rest : rest.left(quote);
    for (int i = 1; i <= reach.size(); ++i) {
        const bool boundary = i == reach.size() || syntax::isBoundary(reach.at(i));
        if (!boundary)
            continue;
        const QString candidate = foldName(name + reach.left(i));
        if (candidate != foldName(name) && vocab.resolve(kind, candidate) >= 0)
            return true;
    }
    return false;
}

QString quoted(const QString &name)
{
    return QLatin1Char('"') + name + QLatin1Char('"');
}

// The precedence rank of a node kind, loosest first: AND < OR < NOT < leaf.
int rank(NodeKind k)
{
    switch (k) {
    case NodeKind::And: return 0;
    case NodeKind::Or: return 1;
    case NodeKind::Not: return 2;
    default: return 3;
    }
}

void emitNode(const Node &n, QVector<Piece> &out, const Vocabulary &vocab);

void emitChild(const Node &child, NodeKind parent, QVector<Piece> &out, const Vocabulary &vocab)
{
    // Parenthesize a child that binds no tighter than its parent (so `(a b) c`
    // keeps its grouping and an AND under OR/NOT reads correctly), plus an OR
    // under AND for readability — `beach (sunset | sunrise)` says what the
    // precedence rule only implies.
    const bool parens = (rank(child.kind) <= rank(parent) && parent != NodeKind::Not)
        || (parent == NodeKind::Not && rank(child.kind) < rank(NodeKind::Not))
        || (parent == NodeKind::And && child.kind == NodeKind::Or);
    if (parens)
        out.append({QStringLiteral("("), false});
    emitNode(child, out, vocab);
    if (parens)
        out.append({QStringLiteral(")"), false});
}

void emitNode(const Node &n, QVector<Piece> &out, const Vocabulary &vocab)
{
    switch (n.kind) {
    case NodeKind::Tag:
        out.append({leafName(n, vocab), true, NameKind::Tag});
        break;
    case NodeKind::Album:
        out.append({syntax::kAlbumKey + QLatin1Char(':'), false});
        out.append({leafName(n, vocab), true, NameKind::Album, true});
        break;
    case NodeKind::SmartAlbum:
        out.append({syntax::kAlbumKey + QLatin1Char(':'), false});
        out.append({leafName(n, vocab), true, NameKind::SmartAlbum, true});
        break;
    case NodeKind::Category:
        out.append({syntax::kCatKey + QLatin1Char(':'), false});
        out.append({leafName(n, vocab), true, NameKind::Category, true});
        break;
    case NodeKind::Favorite:
        out.append({syntax::kIsKey + QLatin1Char(':') + syntax::kFavValue, false});
        break;
    case NodeKind::Untagged:
        out.append({syntax::kIsKey + QLatin1Char(':') + syntax::kUntaggedValue, false});
        break;
    case NodeKind::Missing:
        // The name is unknown to the vocabulary by definition, so it can only be
        // written as a literal; it reads back as an unknown-name error, which is
        // the point — a missing term is surfaced, never silently dropped.
        if (n.was == NameKind::Album || n.was == NameKind::SmartAlbum)
            out.append({syntax::kAlbumKey + QLatin1Char(':'), false});
        else if (n.was == NameKind::Category)
            out.append({syntax::kCatKey + QLatin1Char(':'), false});
        out.append({quoted(n.name), false});
        break;
    case NodeKind::Not:
        out.append({QStringLiteral("-"), false});
        if (!n.children.isEmpty())
            emitChild(n.children.first(), NodeKind::Not, out, vocab);
        break;
    case NodeKind::And:
    case NodeKind::Or:
        for (int i = 0; i < n.children.size(); ++i) {
            if (i > 0)
                out.append({n.kind == NodeKind::And ? QStringLiteral(" ") : QStringLiteral(" | "), false});
            emitChild(n.children.at(i), n.kind, out, vocab);
        }
        break;
    }
}

} // namespace

QString format(const Node &root, const Vocabulary &vocab)
{
    QVector<Piece> pieces;
    emitNode(root, pieces, vocab);

    // Decide quoting right to left: each name sees the final text that follows it.
    QString rest;
    for (int i = pieces.size() - 1; i >= 0; --i) {
        Piece &p = pieces[i];
        if (p.isName) {
            const bool quote = needsQuoteAlone(p.text, p.prefixed)
                || longerMatchFollows(p.text, p.kind, rest, vocab);
            if (quote)
                p.text = quoted(p.text);
        }
        rest.prepend(p.text);
    }
    return rest;
}

// --- Evaluation -----------------------------------------------------------------

bool matches(const Node &n, const QSet<int> &photoTags, const EvalContext &ctx)
{
    switch (n.kind) {
    case NodeKind::Tag:
    case NodeKind::Album:
        return photoTags.contains(n.id);
    case NodeKind::SmartAlbum:
    case NodeKind::Missing:
        return false;
    case NodeKind::Category:
        for (int t : photoTags)
            if (ctx.tagCategory.value(t, -1) == n.id)
                return true;
        return false;
    case NodeKind::Favorite:
        return ctx.favoritesTagId >= 0 && photoTags.contains(ctx.favoritesTagId);
    case NodeKind::Untagged:
        for (int t : photoTags)
            if (!ctx.albumTagIds.contains(t))
                return false;
        return true;
    case NodeKind::Not:
        return !n.children.isEmpty() && !matches(n.children.first(), photoTags, ctx);
    case NodeKind::And:
        for (const Node &c : n.children)
            if (!matches(c, photoTags, ctx))
                return false;
        return !n.children.isEmpty();
    case NodeKind::Or:
        for (const Node &c : n.children)
            if (matches(c, photoTags, ctx))
                return true;
        return false;
    }
    return false;
}

} // namespace oql
