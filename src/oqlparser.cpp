// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// The OQL parser: a recursive-descent parser whose "lexer" is folded into it,
// because where a bare term ends depends on the vocabulary (longest match), not
// on the characters alone. See oql.h for the language.

#include "oql.h"
#include "oqlsyntax.h"

#include <QChar>
#include <QVector>

#include <algorithm>

namespace oql {
namespace {

constexpr int kMaxSuggestions = 5;

// Levenshtein distance, for "did you mean". Only called on short strings.
int editDistance(const QString &a, const QString &b)
{
    const int n = a.size(), m = b.size();
    QVector<int> prev(m + 1), cur(m + 1);
    for (int j = 0; j <= m; ++j)
        prev[j] = j;
    for (int i = 1; i <= n; ++i) {
        cur[0] = i;
        for (int j = 1; j <= m; ++j) {
            const int sub = prev[j - 1] + (a.at(i - 1) == b.at(j - 1) ? 0 : 1);
            cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, sub});
        }
        std::swap(prev, cur);
    }
    return prev[m];
}

QString quoted(const QString &s)
{
    return QLatin1Char('"') + s + QLatin1Char('"');
}

class Parser {
public:
    Parser(const QString &text, const Vocabulary &vocab) : m_text(text), m_vocab(vocab) {}

    ParseResult run()
    {
        ParseResult result;
        if (m_text.trimmed().isEmpty()) {
            result.error = {{0, 0}, QStringLiteral("Empty query"), {}};
            return result;
        }
        Node root;
        if (parseAnd(root)) {
            skipSpace();
            if (!atEnd()) {
                // parseAnd only stops early at ')' — one with no matching '('.
                fail({m_pos, 1}, QStringLiteral("Unmatched \")\""));
            }
        }
        result.error = m_error;
        result.notes = m_notes;
        if (result.ok())
            result.root = root;
        return result;
    }

private:
    // --- scanning helpers ---
    bool atEnd() const { return m_pos >= int(m_text.size()); }
    QChar peek() const { return m_text.at(m_pos); }
    void skipSpace()
    {
        while (!atEnd() && peek().isSpace())
            ++m_pos;
    }
    // The end of the bare word starting at `from`: the first boundary at or
    // after it. Used for error spans and predicate values.
    int wordEnd(int from) const
    {
        int p = from;
        while (p < m_text.size() && !syntax::isBoundary(m_text.at(p)))
            ++p;
        return p;
    }
    bool fail(Span span, const QString &message, QStringList suggestions = {})
    {
        if (m_error.isNull())
            m_error = {span, message, std::move(suggestions)};
        return false;
    }

    // --- longest match ---
    // The longest known name of `kind` starting at `start` and ending at a
    // boundary. On success sets `end` (exclusive, trailing whitespace excluded)
    // and `id`, and returns true. A '"' always ends a term, so nothing past the
    // next quote is considered.
    bool longestMatch(NameKind kind, int start, int &end, int &id) const
    {
        int limit = m_text.indexOf(QLatin1Char('"'), start);
        if (limit < 0)
            limit = m_text.size();
        for (int p = limit; p > start; --p) {
            if (p != m_text.size() && !syntax::isBoundary(m_text.at(p)))
                continue;
            if (m_text.at(p - 1).isSpace())
                continue; // same candidate as the shorter one before the space
            const int found = m_vocab.resolve(kind, foldName(m_text.mid(start, p - start)));
            if (found >= 0) {
                end = p;
                id = found;
                return true;
            }
        }
        return false;
    }

    // --- suggestions ---
    // Known names of `kinds` resembling `typed`, best first: a prefix or
    // substring match outranks a small edit distance.
    QStringList suggest(const QVector<NameKind> &kinds, const QString &typed) const
    {
        const QString f = foldName(typed);
        if (f.isEmpty())
            return {};
        struct Scored { int score; QString name; };
        QVector<Scored> scored;
        for (NameKind kind : kinds) {
            for (const QString &name : m_vocab.names(kind)) {
                const QString nf = foldName(name);
                int score = -1;
                if (nf.startsWith(f) || f.startsWith(nf))
                    score = 0;
                else if (nf.contains(f))
                    score = 1;
                else {
                    const int budget = std::max(1, int(f.size()) / 4);
                    if (std::abs(nf.size() - f.size()) <= budget && editDistance(nf, f) <= budget)
                        score = 2;
                }
                if (score >= 0)
                    scored.append({score, name});
            }
        }
        std::stable_sort(scored.begin(), scored.end(), [](const Scored &a, const Scored &b) {
            if (a.score != b.score)
                return a.score < b.score;
            if (a.name.size() != b.name.size())
                return a.name.size() < b.name.size();
            return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
        });
        QStringList out;
        for (const Scored &s : scored) {
            if (!out.contains(s.name))
                out.append(s.name);
            if (out.size() >= kMaxSuggestions)
                break;
        }
        return out;
    }

    // When the text from `termStart` (or from the run of '-' / '(' syntax just
    // before it) would resolve as a tag if quoted, that quoted form — so a user
    // who typed `-20 °C` or `is:it` meaning the tag is told how to say so.
    QStringList quotedTagSuggestion(int termStart) const
    {
        int s = termStart;
        while (s > 0 && (m_text.at(s - 1) == QLatin1Char('-') || m_text.at(s - 1) == QLatin1Char('(')))
            --s;
        int end = 0, id = -1;
        if (longestMatch(NameKind::Tag, s, end, id))
            return {quoted(m_vocab.name(NameKind::Tag, id))};
        return {};
    }

    QStringList unknownNameSuggestions(const QVector<NameKind> &kinds, int termStart,
                                       const QString &typed) const
    {
        QStringList out = quotedTagSuggestion(termStart);
        for (const QString &s : suggest(kinds, typed))
            if (!out.contains(s))
                out.append(s);
        return out;
    }

    // --- grammar ---
    // What an AND operand contributed to the bare-run bookkeeping: the span of
    // its bare tag term when it is one (possibly negated), else length 0.
    struct Operand {
        Node node;
        Span bare;
    };

    // and := or (or)*
    bool parseAnd(Node &out)
    {
        QVector<Operand> operands;
        const int start = m_pos;
        for (;;) {
            skipSpace();
            if (atEnd() || peek() == QLatin1Char(')'))
                break;
            Operand op;
            if (!parseOr(op))
                return false;
            operands.append(op);
        }
        if (operands.isEmpty())
            return fail({start, 0}, QStringLiteral("Expected a term"));
        noteBareRuns(operands);
        if (operands.size() == 1) {
            out = operands.first().node;
            return true;
        }
        QVector<Node> children;
        for (const Operand &op : operands)
            children.append(op.node);
        out = Node::andOf(children);
        out.span = {children.first().span.start, children.last().span.end() - children.first().span.start};
        return true;
    }

    // Adjacent bare terms separated only by whitespace are where the reading
    // depended on the vocabulary: `dark forest` is two tags only because no tag
    // is named `dark forest`. Record a note per such run.
    void noteBareRuns(const QVector<Operand> &operands)
    {
        int i = 0;
        while (i < operands.size()) {
            if (operands.at(i).bare.length == 0) {
                ++i;
                continue;
            }
            int j = i;
            while (j + 1 < operands.size() && operands.at(j + 1).bare.length > 0)
                ++j;
            if (j > i) {
                const int s = operands.at(i).node.span.start;
                const int e = operands.at(j).node.span.end();
                QStringList parts;
                for (int k = i; k <= j; ++k) {
                    const Span sp = operands.at(k).node.span;
                    parts.append(quoted(m_text.mid(sp.start, sp.length)));
                }
                m_notes.append({{s, e - s},
                                QStringLiteral("%1 is read as %2 separate terms: %3")
                                    .arg(quoted(m_text.mid(s, e - s)))
                                    .arg(j - i + 1)
                                    .arg(parts.join(QStringLiteral(", ")))});
            }
            i = j + 1;
        }
    }

    // or := not ('|' not)*
    bool parseOr(Operand &out)
    {
        if (!parseNot(out))
            return false;
        QVector<Node> children;
        for (;;) {
            skipSpace();
            if (atEnd() || peek() != QLatin1Char('|'))
                break;
            const int bar = m_pos;
            ++m_pos;
            skipSpace();
            if (atEnd() || peek() == QLatin1Char(')') || peek() == QLatin1Char('|'))
                return fail({bar, 1}, QStringLiteral("Expected a term after \"|\""));
            if (children.isEmpty())
                children.append(out.node);
            Operand next;
            if (!parseNot(next))
                return false;
            children.append(next.node);
        }
        if (!children.isEmpty()) {
            Node n = Node::orOf(children);
            n.span = {children.first().span.start, children.last().span.end() - children.first().span.start};
            out = {n, {}};
        }
        return true;
    }

    // not := '-' not | primary
    bool parseNot(Operand &out)
    {
        skipSpace();
        if (!atEnd() && peek() == QLatin1Char('-')) {
            const int start = m_pos;
            ++m_pos;
            skipSpace();
            if (atEnd() || peek() == QLatin1Char(')') || peek() == QLatin1Char('|'))
                return fail({start, 1}, QStringLiteral("Expected a term after \"-\""));
            Operand inner;
            if (!parseNot(inner))
                return false;
            Node n = Node::notOf(inner.node);
            n.span = {start, inner.node.span.end() - start};
            out = {n, inner.bare};
            return true;
        }
        return parsePrimary(out);
    }

    // primary := '(' and ')' | '"' name '"' | predicate | name
    bool parsePrimary(Operand &out)
    {
        skipSpace();
        const int start = m_pos;
        if (atEnd())
            return fail({start, 0}, QStringLiteral("Expected a term"));
        const QChar c = peek();
        if (c == QLatin1Char('(')) {
            ++m_pos;
            skipSpace();
            if (!atEnd() && peek() == QLatin1Char(')'))
                return fail({start, m_pos + 1 - start}, QStringLiteral("Empty group \"()\""));
            Node inner;
            if (!parseAnd(inner))
                return false;
            skipSpace();
            if (atEnd() || peek() != QLatin1Char(')'))
                return fail({start, 1}, QStringLiteral("Missing closing \")\""));
            ++m_pos;
            inner.span = {start, m_pos - start};
            out = {inner, {}};
            return true;
        }
        if (c == QLatin1Char(')'))
            return fail({start, 1}, QStringLiteral("Unmatched \")\""));
        if (c == QLatin1Char('|'))
            return fail({start, 1}, QStringLiteral("Expected a term before \"|\""));
        if (c == QLatin1Char('"'))
            return parseQuoted(out);
        const int key = syntax::predicateKeyAt(m_text, m_pos);
        if (key >= 0)
            return parsePredicate(key, out);
        return parseBareTag(out);
    }

    // A quoted literal: always a tag name, whatever it contains.
    bool parseQuoted(Operand &out)
    {
        const int start = m_pos;
        const int close = m_text.indexOf(QLatin1Char('"'), start + 1);
        if (close < 0)
            return fail({start, int(m_text.size()) - start}, QStringLiteral("Missing closing quote"));
        const QString raw = m_text.mid(start + 1, close - start - 1);
        m_pos = close + 1;
        const Span span{start, m_pos - start};
        const int id = m_vocab.resolve(NameKind::Tag, foldName(raw));
        if (id < 0)
            return fail(span, QStringLiteral("Unknown tag %1").arg(quoted(raw)),
                        suggest({NameKind::Tag}, raw));
        Node n = Node::tag(id, m_vocab.name(NameKind::Tag, id));
        n.span = span;
        out = {n, {}};
        return true;
    }

    bool parseBareTag(Operand &out)
    {
        const int start = m_pos;
        int end = 0, id = -1;
        if (!longestMatch(NameKind::Tag, start, end, id)) {
            const int we = wordEnd(start);
            const QString word = m_text.mid(start, we - start);
            return fail({start, we - start}, QStringLiteral("Unknown tag %1").arg(quoted(word)),
                        unknownNameSuggestions({NameKind::Tag}, start, word));
        }
        m_pos = end;
        Node n = Node::tag(id, m_vocab.name(NameKind::Tag, id));
        n.span = {start, end - start};
        const QString raw = m_text.mid(start, end - start);
        if (raw.contains(QLatin1Char(' ')) || raw.contains(QLatin1Char('\t')))
            m_notes.append({n.span, QStringLiteral("%1 is read as one tag").arg(quoted(raw))});
        out = {n, n.span};
        return true;
    }

    // `key:` has been recognised at m_pos. The value is read according to the
    // key: a closed word for is:, a name (bare longest match or quoted) for
    // album: and cat:.
    bool parsePredicate(int keyIndex, Operand &out)
    {
        const syntax::PredicateSpec &spec = syntax::kPredicates[keyIndex];
        const int start = m_pos;
        const int valueStart = start + int(qstrlen(spec.key)) + 1;
        m_pos = valueStart;
        switch (spec.pred) {
        case syntax::Predicate::Is: {
            const int we = wordEnd(valueStart);
            const QString value = foldName(m_text.mid(valueStart, we - valueStart));
            Node n;
            if (value == syntax::kFavValue)
                n = Node::favorite();
            else if (value == syntax::kUntaggedValue)
                n = Node::untagged();
            else {
                QStringList sugg = quotedTagSuggestion(start);
                sugg << syntax::kIsKey + QLatin1Char(':') + syntax::kFavValue
                     << syntax::kIsKey + QLatin1Char(':') + syntax::kUntaggedValue;
                const QString typed = m_text.mid(start, we - start);
                return fail({start, we - start},
                            value.isEmpty()
                                ? QStringLiteral("Expected fav or untagged after \"is:\"")
                                : QStringLiteral("Unknown is: value %1").arg(quoted(typed)),
                            sugg);
            }
            m_pos = we;
            n.span = {start, we - start};
            out = {n, {}};
            return true;
        }
        case syntax::Predicate::Album:
            return parseNamePredicate(start, valueStart, {NameKind::Album, NameKind::SmartAlbum},
                                      QStringLiteral("album"), out);
        case syntax::Predicate::Cat:
            return parseNamePredicate(start, valueStart, {NameKind::Category},
                                      QStringLiteral("category"), out);
        }
        return false;
    }

    // album:<name> / cat:<name>. Tries each kind in order and keeps the longest
    // match (ties go to the first kind: a manual album over a smart album).
    bool parseNamePredicate(int start, int valueStart, const QVector<NameKind> &kinds,
                            const QString &what, Operand &out)
    {
        int bestEnd = -1, bestId = -1;
        NameKind bestKind = kinds.first();
        QString raw;
        if (valueStart < m_text.size() && m_text.at(valueStart) == QLatin1Char('"')) {
            const int close = m_text.indexOf(QLatin1Char('"'), valueStart + 1);
            if (close < 0)
                return fail({valueStart, int(m_text.size()) - valueStart},
                            QStringLiteral("Missing closing quote"));
            raw = m_text.mid(valueStart + 1, close - valueStart - 1);
            for (NameKind kind : kinds) {
                const int id = m_vocab.resolve(kind, foldName(raw));
                if (id >= 0) {
                    bestEnd = close + 1;
                    bestId = id;
                    bestKind = kind;
                    break;
                }
            }
            if (bestEnd < 0)
                return fail({start, close + 1 - start},
                            QStringLiteral("Unknown %1 %2").arg(what, quoted(raw)),
                            suggest(kinds, raw));
        } else {
            for (NameKind kind : kinds) {
                int end = 0, id = -1;
                if (longestMatch(kind, valueStart, end, id) && end > bestEnd) {
                    bestEnd = end;
                    bestId = id;
                    bestKind = kind;
                }
            }
            if (bestEnd < 0) {
                const int we = wordEnd(valueStart);
                const QString word = m_text.mid(valueStart, we - valueStart);
                if (word.isEmpty())
                    return fail({start, we - start},
                                QStringLiteral("Expected a %1 name after \"%2:\"")
                                    .arg(what, m_text.mid(start, valueStart - start - 1)),
                                quotedTagSuggestion(start));
                return fail({start, we - start},
                            QStringLiteral("Unknown %1 %2").arg(what, quoted(word)),
                            unknownNameSuggestions(kinds, start, word));
            }
            raw = m_text.mid(valueStart, bestEnd - valueStart);
        }
        m_pos = bestEnd;
        Node n;
        const QString name = m_vocab.name(bestKind, bestId);
        switch (bestKind) {
        case NameKind::Album: n = Node::album(bestId, name); break;
        case NameKind::SmartAlbum: n = Node::smartAlbum(bestId, name); break;
        case NameKind::Category: n = Node::category(bestId, name); break;
        case NameKind::Tag: n = Node::tag(bestId, name); break;
        }
        n.span = {start, bestEnd - start};
        out = {n, {}};
        return true;
    }

    const QString m_text;
    const Vocabulary &m_vocab;
    int m_pos = 0;
    Error m_error;
    QVector<Note> m_notes;
};

} // namespace

ParseResult parse(const QString &text, const Vocabulary &vocab)
{
    return Parser(text, vocab).run();
}

QVector<Token> tokenize(const QString &text, const Vocabulary &vocab)
{
    QVector<Token> out;
    const int n = int(text.size());
    int pos = 0;
    bool negate = false;
    int negateStart = -1;
    auto wordEnd = [&](int from) {
        int p = from;
        while (p < n && !syntax::isBoundary(text.at(p)))
            ++p;
        return p;
    };
    // Longest known name of `kind` from `start`, ending at a boundary (the
    // parser's rule; see Parser::longestMatch).
    auto longest = [&](NameKind kind, int start, int &end) -> int {
        int limit = int(text.indexOf(QLatin1Char('"'), start));
        if (limit < 0)
            limit = n;
        for (int p = limit; p > start; --p) {
            if (p != n && !syntax::isBoundary(text.at(p)))
                continue;
            if (text.at(p - 1).isSpace())
                continue;
            const int id = vocab.resolve(kind, foldName(text.mid(start, p - start)));
            if (id >= 0) {
                end = p;
                return id;
            }
        }
        return -1;
    };
    auto push = [&](Token t) {
        if (negate && (t.kind == Token::Tag || t.kind == Token::Predicate || t.kind == Token::Open
                       || t.kind == Token::Unknown)) {
            t.negated = true;
            t.span = {negateStart, t.span.end() - negateStart};
        } else if (negate) {
            // A `-` with nothing to negate (before `)`, `|` or the end).
            Token stray;
            stray.kind = Token::Unknown;
            stray.span = {negateStart, 1};
            stray.name = QStringLiteral("-");
            out.append(stray);
        }
        negate = false;
        out.append(t);
    };

    while (pos < n) {
        const QChar c = text.at(pos);
        if (c.isSpace()) {
            ++pos;
            continue;
        }
        Token t;
        if (c == QLatin1Char('(') || c == QLatin1Char(')') || c == QLatin1Char('|')) {
            t.kind = c == QLatin1Char('(') ? Token::Open : c == QLatin1Char(')') ? Token::Close : Token::Or;
            t.span = {pos, 1};
            t.name = QString(c);
            ++pos;
            push(t);
            continue;
        }
        if (c == QLatin1Char('-')) {
            if (negate) { // `--a`: the second `-` is its own (stray) token
                Token stray;
                stray.kind = Token::Unknown;
                stray.span = {negateStart, 1};
                stray.name = QStringLiteral("-");
                out.append(stray);
            }
            negate = true;
            negateStart = pos;
            ++pos;
            continue;
        }
        if (c == QLatin1Char('"')) {
            const int close = int(text.indexOf(QLatin1Char('"'), pos + 1));
            const int end = close < 0 ? n : close + 1;
            const QString raw = text.mid(pos + 1, (close < 0 ? n : close) - pos - 1);
            const int id = vocab.resolve(NameKind::Tag, foldName(raw));
            t.kind = id >= 0 && close >= 0 ? Token::Tag : Token::Unknown;
            t.span = {pos, end - pos};
            t.id = id;
            t.name = t.kind == Token::Tag ? vocab.name(NameKind::Tag, id) : text.mid(pos, end - pos);
            pos = end;
            push(t);
            continue;
        }
        const int key = syntax::predicateKeyAt(text, pos);
        if (key >= 0) {
            const syntax::PredicateSpec &spec = syntax::kPredicates[key];
            const int valueStart = pos + int(qstrlen(spec.key)) + 1;
            t.kind = Token::Predicate;
            t.name = QLatin1String(spec.key);
            int end = valueStart;
            bool ok = false;
            if (spec.pred == syntax::Predicate::Is) {
                end = wordEnd(valueStart);
                t.value = foldName(text.mid(valueStart, end - valueStart));
                ok = t.value == syntax::kFavValue || t.value == syntax::kUntaggedValue;
            } else {
                const QVector<NameKind> kinds = spec.pred == syntax::Predicate::Album
                    ? QVector<NameKind>{NameKind::Album, NameKind::SmartAlbum}
                    : QVector<NameKind>{NameKind::Category};
                if (valueStart < n && text.at(valueStart) == QLatin1Char('"')) {
                    const int close = int(text.indexOf(QLatin1Char('"'), valueStart + 1));
                    end = close < 0 ? n : close + 1;
                    const QString raw = text.mid(valueStart + 1, (close < 0 ? n : close) - valueStart - 1);
                    for (NameKind k : kinds) {
                        const int id = vocab.resolve(k, foldName(raw));
                        if (id >= 0 && close >= 0) { t.id = id; t.nameKind = k; ok = true; break; }
                    }
                } else {
                    int best = -1;
                    for (NameKind k : kinds) {
                        int e = 0;
                        const int id = longest(k, valueStart, e);
                        if (id >= 0 && e > best) { best = e; t.id = id; t.nameKind = k; ok = true; }
                    }
                    end = ok ? best : wordEnd(valueStart);
                }
                if (ok)
                    t.value = vocab.name(t.nameKind, t.id);
            }
            t.span = {pos, end - pos};
            if (!ok) {
                t.kind = Token::Unknown;
                t.name = text.mid(pos, end - pos);
                t.value.clear();
            }
            pos = end;
            push(t);
            continue;
        }
        int end = 0;
        const int id = longest(NameKind::Tag, pos, end);
        if (id >= 0) {
            t.kind = Token::Tag;
            t.id = id;
            t.name = vocab.name(NameKind::Tag, id);
        } else {
            end = wordEnd(pos);
            t.kind = Token::Unknown;
            t.name = text.mid(pos, end - pos);
        }
        t.span = {pos, end - pos};
        pos = end;
        push(t);
    }
    if (negate) { // a trailing `-`
        Token stray;
        stray.kind = Token::Unknown;
        stray.span = {negateStart, 1};
        stray.name = QStringLiteral("-");
        out.append(stray);
    }
    return out;
}

} // namespace oql
