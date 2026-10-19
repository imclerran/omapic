// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// exiv2 first (clashes with Qt's signals/slots/emit macros); used to author
// embedded metadata and sidecar fixtures the way other apps would.
#include <exiv2/exiv2.hpp>

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QStringList>
#include <QTemporaryDir>

#include "database.h"
#include "libraryworker.h"
#include "photo.h"
#include "photometadata.h"

// End-to-end coverage of the import/rescan orchestration in LibraryWorker. In
// the app the worker runs on a background thread via queued signals, but its
// slots are ordinary functions, so here we call them synchronously on a
// file-backed temp library and read the result back through a second Database
// connection — the asynchrony is just the app's wiring, not the logic.
class TestImport : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void importAppliesSidecarTags();
    void inPlaceEditKeepsTagsViaRekey();
    void externalSidecarChangeIsPickedUp();
    void embeddedKeywordsReadInOnePass();

private:
    QString pics() const { return m_dir->filePath(QStringLiteral("pics")); }
    QString dbFile() const { return m_dir->filePath(QStringLiteral("library.db")); }
    static void writeFile(const QString &path, const QByteArray &bytes);
    // Tag names on the photo at `path`, read through a fresh verify connection.
    QStringList tagsOf(const QString &path);
    // Names of the user categories in the library.
    QStringList categoryNames();

    QTemporaryDir *m_dir = nullptr;
    LibraryWorker *m_worker = nullptr;
};

void TestImport::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
    QVERIFY(QDir().mkpath(pics()));
    m_worker = new LibraryWorker(dbFile());
    m_worker->initialize(); // opens the worker's DB connection in this thread
}

void TestImport::cleanup()
{
    delete m_worker; // releases its "omapic_worker" connection's handle
    m_worker = nullptr;
    QSqlDatabase::removeDatabase(QStringLiteral("omapic_worker"));
    delete m_dir;
    m_dir = nullptr;
}

void TestImport::writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(bytes);
    f.close();
}

QStringList TestImport::tagsOf(const QString &path)
{
    QStringList names;
    {
        Database db;
        if (db.open(dbFile(), QStringLiteral("verify"))) {
            for (const Photo &p : db.loadPhotos()) {
                if (p.path == path) {
                    names = p.tagNames;
                    break;
                }
            }
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("verify"));
    return names;
}

QStringList TestImport::categoryNames()
{
    QStringList names;
    {
        Database db;
        if (db.open(dbFile(), QStringLiteral("verify_cat"))) {
            for (const CategoryInfo &c : db.loadCategories())
                names << c.name;
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("verify_cat"));
    return names;
}

void TestImport::importAppliesSidecarTags()
{
    const QString photo = pics() + QStringLiteral("/one.jpg");
    writeFile(photo, "not-a-real-jpeg-but-hashes-fine");
    // A darktable/digiKam-style sidecar: a flat keyword and a hierarchical one.
    writeFile(photo + QStringLiteral(".xmp"),
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about=''\n"
        "    xmlns:dc='http://purl.org/dc/elements/1.1/'\n"
        "    xmlns:lr='http://ns.adobe.com/lightroom/1.0/'>\n"
        "   <dc:subject><rdf:Bag><rdf:li>sunset</rdf:li></rdf:Bag></dc:subject>\n"
        "   <lr:hierarchicalSubject><rdf:Bag>\n"
        "    <rdf:li>Places|Spain</rdf:li>\n"
        "   </rdf:Bag></lr:hierarchicalSubject>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>\n");

    m_worker->importDirectory(pics());

    const QStringList tags = tagsOf(photo);
    QVERIFY2(tags.contains(QStringLiteral("sunset")), qPrintable(tags.join(',')));
    QVERIFY2(tags.contains(QStringLiteral("Spain")), qPrintable(tags.join(',')));
    QVERIFY(categoryNames().contains(QStringLiteral("Places"))); // category created on import
}

void TestImport::inPlaceEditKeepsTagsViaRekey()
{
    const QString photo = pics() + QStringLiteral("/edit.jpg");
    writeFile(photo, "original-bytes");
    m_worker->importDirectory(pics());

    // Tag the imported photo (archives the tag against its current content hash).
    int photoId = -1;
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("find")));
        for (const Photo &p : db.loadPhotos())
            if (p.path == photo)
                photoId = p.id;
    }
    QSqlDatabase::removeDatabase(QStringLiteral("find"));
    QVERIFY(photoId > 0);
    m_worker->addTagToPhotos({photoId}, QStringLiteral("keepme"));
    QVERIFY(tagsOf(photo).contains(QStringLiteral("keepme")));

    // Edit the file in place: new bytes → new content hash and a different size.
    writeFile(photo, "edited-bytes-that-are-longer-than-before");
    m_worker->rescan();

    // The tag survived the edit: the archive was rekeyed from the old hash to
    // the new one, and the still-present row picked it back up.
    QVERIFY2(tagsOf(photo).contains(QStringLiteral("keepme")),
             "an in-place edit must not drop the photo's tags");
}

void TestImport::externalSidecarChangeIsPickedUp()
{
    const QString photo = pics() + QStringLiteral("/later.jpg");
    writeFile(photo, "bytes");
    m_worker->importDirectory(pics()); // no sidecar yet → no tags
    QVERIFY(tagsOf(photo).isEmpty());

    // Another app writes a sidecar after the photo is already in the library.
    writeFile(photo + QStringLiteral(".xmp"),
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/'>\n"
        "   <dc:subject><rdf:Bag><rdf:li>addedlater</rdf:li></rdf:Bag></dc:subject>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>\n");

    m_worker->rescan();
    QVERIFY2(tagsOf(photo).contains(QStringLiteral("addedlater")),
             "a sidecar written after import must be picked up on rescan");
}

void TestImport::embeddedKeywordsReadInOnePass()
{
    // A real 1×1 PNG, so exiv2 can embed an XMP keyword into a valid image.
    const QString photo = pics() + QStringLiteral("/embedded.png");
    {
        QImage img(1, 1, QImage::Format_RGBA8888);
        img.fill(Qt::black);
        QVERIFY(img.save(photo, "PNG"));
    }

    // Embed an XMP keyword directly in the image (no sidecar).
    try {
        auto img = Exiv2::ImageFactory::open(photo.toStdString());
        QVERIFY(img.get());
        Exiv2::XmpData xmp;
        xmp["Xmp.dc.subject"] = "embeddedtag";
        img->setXmpData(xmp);
        img->writeMetadata();
    } catch (const std::exception &e) {
        QFAIL(qPrintable(QStringLiteral("exiv2 could not embed XMP: ") + QString::fromUtf8(e.what())));
    }

    const ImportMetadata meta = readImportMetadata(photo, /*isVideo=*/false);
    QVERIFY2(meta.flat.contains(QStringLiteral("embeddedtag")), qPrintable(meta.flat.join(',')));

    // And it flows through the worker's import into the library.
    m_worker->importDirectory(pics());
    QVERIFY(tagsOf(photo).contains(QStringLiteral("embeddedtag")));
}

QTEST_GUILESS_MAIN(TestImport)
#include "tst_import.moc"
