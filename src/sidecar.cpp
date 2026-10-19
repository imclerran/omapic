// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// exiv2 first: its headers quarrel with Qt's signals/slots/emit macros.
#include <exiv2/exiv2.hpp>

#include "sidecar.h"

#include <QFileInfo>

namespace sidecar {
namespace {

// The URI omapic claims for its own XMP properties. Registering the "omapic"
// prefix before reading maps this URI to that prefix, so our keys resolve as
// "Xmp.omapic.*" rather than an exiv2-generated prefix (ns1, …).
const char *kOmapicNs = "https://github.com/imclerran/omapic/xmp/1.0/";

void ensureNamespace()
{
    static const bool once = [] {
        try {
            Exiv2::XmpProperties::registerNs(kOmapicNs, "omapic");
        } catch (...) {
            // Already registered, or exiv2 refused — reads still work for the
            // standard namespaces, which is the common case.
        }
        return true;
    }();
    Q_UNUSED(once);
}

// Read an XMP property as a list of strings. Arrays (bag/seq/alt) yield one
// entry per item; a plain text property yields a single entry. Empty when the
// key is absent. (count() is the item count for arrays but the character count
// for text, so the type — not count — decides how to read it.)
QStringList readValues(const Exiv2::XmpData &xmp, const char *key)
{
    QStringList out;
    try {
        const auto it = xmp.findKey(Exiv2::XmpKey(key));
        if (it == xmp.end())
            return out;
        const Exiv2::TypeId t = it->typeId();
        if (t == Exiv2::xmpBag || t == Exiv2::xmpSeq || t == Exiv2::xmpAlt) {
            for (long i = 0; i < it->count(); ++i) {
                const QString s = QString::fromStdString(it->toString(i)).trimmed();
                if (!s.isEmpty())
                    out << s;
            }
        } else {
            const QString s = QString::fromStdString(it->toString()).trimmed();
            if (!s.isEmpty())
                out << s;
        }
    } catch (...) {
        // Unknown key prefix or malformed value — treat as absent.
    }
    return out;
}

} // namespace

CategorizedTag splitHierarchical(const QString &keyword)
{
    const QString sep = keyword.contains(QLatin1Char('|'))
                            ? QStringLiteral("|")
                            : QStringLiteral("/");
    const QStringList parts = keyword.split(sep, Qt::SkipEmptyParts);
    if (parts.isEmpty())
        return {QString(), keyword.trimmed()};
    if (parts.size() == 1)
        return {QString(), parts.first().trimmed()};
    // First component is the category; the rest (rejoined with '/') is the tag,
    // flattening hierarchies deeper than two levels.
    const QString category = parts.first().trimmed();
    QStringList rest = parts.mid(1);
    for (QString &s : rest)
        s = s.trimmed();
    return {category, rest.join(QLatin1Char('/'))};
}

QString sidecarPath(const QString &photoPath)
{
    const QString primary = photoPath + QStringLiteral(".xmp"); // photo.jpg.xmp
    if (QFileInfo::exists(primary))
        return primary;
    // Adobe convention: strip the extension (photo.xmp).
    const QFileInfo fi(photoPath);
    const QString adobe = fi.path() + QLatin1Char('/') + fi.completeBaseName()
                          + QStringLiteral(".xmp");
    if (adobe != primary && QFileInfo::exists(adobe))
        return adobe;
    return {};
}

std::optional<ParsedTags> readSidecar(const QString &photoPath)
{
    const QString path = sidecarPath(photoPath);
    if (path.isEmpty())
        return std::nullopt;

    ensureNamespace();
    try {
        auto img = Exiv2::ImageFactory::open(path.toStdString());
        if (!img.get())
            return std::nullopt;
        img->readMetadata();
        const Exiv2::XmpData &xmp = img->xmpData();

        ParsedTags p;
        p.flat = readValues(xmp, "Xmp.dc.subject");
        const auto addHierarchical = [&](const char *key) {
            for (const QString &kw : readValues(xmp, key))
                p.categorized.append(splitHierarchical(kw));
        };
        addHierarchical("Xmp.lr.hierarchicalSubject");
        addHierarchical("Xmp.digiKam.TagsList");
        addHierarchical("Xmp.MicrosoftPhoto.LastKeywordXMP");

        const QStringList ch = readValues(xmp, "Xmp.omapic.contentHash");
        if (!ch.isEmpty()) {
            p.contentHash = ch.first();
            p.hasOmapicData = true;
        }
        const QStringList albums = readValues(xmp, "Xmp.omapic.albums");
        if (!albums.isEmpty()) {
            p.albums = albums;
            p.hasOmapicData = true;
        }
        const QStringList fav = readValues(xmp, "Xmp.omapic.favorite");
        if (!fav.isEmpty()) {
            const QString v = fav.first();
            p.favorite = v.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0
                         || v == QLatin1String("1");
            p.hasOmapicData = true;
        }

        if (p.flat.isEmpty() && p.categorized.isEmpty() && !p.hasOmapicData)
            return std::nullopt; // sidecar had nothing we understand
        return p;
    } catch (...) {
        return std::nullopt;
    }
}

} // namespace sidecar
