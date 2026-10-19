// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// exiv2 first (it quarrels with Qt's signals/slots/emit macros), used here only
// to author sidecar fixtures the way darktable/digiKam would.
#include <exiv2/exiv2.hpp>

#include <QtTest>

#include <QFile>
#include <QTemporaryDir>

#include "sidecar.h"

// The sidecar reader turns other apps' XMP keyword fields into omapic tags.
class TestSidecar : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void splitHierarchicalCases();
    void readsDarktableStyleSidecar();
    void readsDigikamAndMicrosoftFields();
    void readsAdobeBaseNameFallback();
    void absentSidecarReturnsNullopt();

private:
    static void writeRawXmp(const QString &path, const QByteArray &xml);
};

void TestSidecar::initTestCase()
{
    // Authoring omapic:* fields below needs the prefix registered first. (The
    // reader registers lazily on its own; this is only for the test's writes.)
    try {
        Exiv2::XmpProperties::registerNs(
            "https://github.com/imclerran/omapic/xmp/1.0/", "omapic");
    } catch (...) {
    }
}

void TestSidecar::writeRawXmp(const QString &path, const QByteArray &xml)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(xml);
    f.close();
}

void TestSidecar::splitHierarchicalCases()
{
    // Lightroom uses '|'; digiKam/Microsoft use '/'. First component is the
    // category, the rest (rejoined with '/') is the tag; a lone component is an
    // uncategorised tag.
    QCOMPARE(sidecar::splitHierarchical(QStringLiteral("Places|Spain")),
             sidecar::CategorizedTag(QStringLiteral("Places"), QStringLiteral("Spain")));
    QCOMPARE(sidecar::splitHierarchical(QStringLiteral("People/Family/Mum")),
             sidecar::CategorizedTag(QStringLiteral("People"), QStringLiteral("Family/Mum")));
    QCOMPARE(sidecar::splitHierarchical(QStringLiteral("Beach")),
             sidecar::CategorizedTag(QString(), QStringLiteral("Beach")));
}

void TestSidecar::readsDarktableStyleSidecar()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.path() + QStringLiteral("/photo.jpg");
    const QString side = photo + QStringLiteral(".xmp"); // photo.jpg.xmp

    Exiv2::XmpData xmp;
    xmp["Xmp.dc.subject"] = "beach";
    xmp["Xmp.dc.subject"] = "sunset";
    xmp["Xmp.lr.hierarchicalSubject"] = "Places|Spain";
    xmp["Xmp.omapic.contentHash"] = "sha256:abc123";
    std::string packet;
    QCOMPARE(Exiv2::XmpParser::encode(packet, xmp), 0);
    auto img = Exiv2::ImageFactory::create(Exiv2::ImageType::xmp, side.toStdString());
    img->setXmpPacket(packet);
    img->writeMetadata();

    const auto parsed = sidecar::readSidecar(photo);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->flat.size(), 2);
    QVERIFY(parsed->flat.contains(QStringLiteral("beach")));
    QVERIFY(parsed->flat.contains(QStringLiteral("sunset")));
    QCOMPARE(parsed->categorized.size(), 1);
    QCOMPARE(parsed->categorized.first(),
             sidecar::CategorizedTag(QStringLiteral("Places"), QStringLiteral("Spain")));
    QCOMPARE(parsed->contentHash, QStringLiteral("sha256:abc123"));
    QVERIFY(parsed->hasOmapicData);
}

void TestSidecar::readsDigikamAndMicrosoftFields()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.path() + QStringLiteral("/clip.png");
    const QString side = photo + QStringLiteral(".xmp");

    // A real-world sidecar from digiKam + a Microsoft tool, written verbatim.
    const QByteArray xml =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about=''\n"
        "    xmlns:digiKam='http://www.digikam.org/ns/1.0/'\n"
        "    xmlns:MicrosoftPhoto='http://ns.microsoft.com/photo/1.0/'>\n"
        "   <digiKam:TagsList><rdf:Seq>\n"
        "    <rdf:li>People/Family/Mum</rdf:li>\n"
        "    <rdf:li>Beach</rdf:li>\n"
        "   </rdf:Seq></digiKam:TagsList>\n"
        "   <MicrosoftPhoto:LastKeywordXMP><rdf:Bag>\n"
        "    <rdf:li>Places/Spain</rdf:li>\n"
        "   </rdf:Bag></MicrosoftPhoto:LastKeywordXMP>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>\n";
    writeRawXmp(side, xml);

    const auto parsed = sidecar::readSidecar(photo);
    QVERIFY(parsed.has_value());
    QVERIFY(parsed->flat.isEmpty());
    QCOMPARE(parsed->categorized.size(), 3);
    QVERIFY(parsed->categorized.contains(
        sidecar::CategorizedTag(QStringLiteral("People"), QStringLiteral("Family/Mum"))));
    QVERIFY(parsed->categorized.contains(
        sidecar::CategorizedTag(QString(), QStringLiteral("Beach"))));
    QVERIFY(parsed->categorized.contains(
        sidecar::CategorizedTag(QStringLiteral("Places"), QStringLiteral("Spain"))));
    QVERIFY(!parsed->hasOmapicData); // no omapic:* fields in a third-party sidecar
}

void TestSidecar::readsAdobeBaseNameFallback()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.path() + QStringLiteral("/holiday.jpg");
    // Adobe convention: holiday.xmp (base name), not holiday.jpg.xmp.
    const QString side = dir.path() + QStringLiteral("/holiday.xmp");

    const QByteArray xml =
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about=''\n"
        "    xmlns:dc='http://purl.org/dc/elements/1.1/'>\n"
        "   <dc:subject><rdf:Bag><rdf:li>holiday</rdf:li></rdf:Bag></dc:subject>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>\n";
    writeRawXmp(side, xml);

    QCOMPARE(sidecar::sidecarPath(photo), side);
    const auto parsed = sidecar::readSidecar(photo);
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->flat, QStringList{QStringLiteral("holiday")});
}

void TestSidecar::absentSidecarReturnsNullopt()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.path() + QStringLiteral("/lonely.jpg");
    QVERIFY(sidecar::sidecarPath(photo).isEmpty());
    QVERIFY(!sidecar::readSidecar(photo).has_value());
}

QTEST_GUILESS_MAIN(TestSidecar)
#include "tst_sidecar.moc"
