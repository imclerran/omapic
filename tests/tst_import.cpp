// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

// exiv2 first (clashes with Qt's signals/slots/emit macros); used to author
// embedded metadata and sidecar fixtures the way other apps would.
#include <exiv2/exiv2.hpp>

#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QTemporaryDir>

#include "database.h"
#include "libraryworker.h"
#include "photo.h"
#include "photometadata.h"
#include "sidecar.h"

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
    void flushWritesTaggedPhotoSidecar();
    void flushImportsNewerSidecarFirst();
    void flushRemovesOmapicSidecarWhenUntagged();
    void flushSkipsDisabledFolderUntilReenabled();
    void rescanPrunesOldRemovedTags();
    void rescanReportsWhetherAnythingChanged();
    void migrationReportsWhetherAnythingChanged();

private:
    QString pics() const { return m_dir->filePath(QStringLiteral("pics")); }
    QString dbFile() const { return m_dir->filePath(QStringLiteral("library.db")); }
    static void writeFile(const QString &path, const QByteArray &bytes);
    // Import `path`, returning its photo id (via a verify connection).
    int importAndId(const QString &path);
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

int TestImport::importAndId(const QString &path)
{
    m_worker->importDirectory(pics());
    int id = -1;
    {
        Database db;
        if (db.open(dbFile(), QStringLiteral("idlookup"))) {
            for (const Photo &p : db.loadPhotos())
                if (p.path == path)
                    id = p.id;
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("idlookup"));
    return id;
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
    m_worker->rescan(/*retentionDays=*/0);

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

    m_worker->rescan(/*retentionDays=*/0);
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

void TestImport::flushWritesTaggedPhotoSidecar()
{
    const QString photo = pics() + QStringLiteral("/tagged.jpg");
    writeFile(photo, "bytes");
    const int id = importAndId(photo);
    QVERIFY(id > 0);

    // Tag it and queue it the way a Library user action would, then flush.
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mut")));
        const int tag = db.ensureTag(QStringLiteral("beach"));
        db.linkTagToContent(id, tag);
        db.rememberContentTag(db.photoHash(id), tag);
        db.enqueueSidecarDirty({id});
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mut"));

    m_worker->flushSidecars();

    QVERIFY(QFile::exists(photo + QStringLiteral(".xmp")));
    const auto back = sidecar::readSidecar(photo);
    QVERIFY(back.has_value());
    QVERIFY(back->flat.contains(QStringLiteral("beach")));
    QVERIFY(!back->contentHash.isEmpty()); // omapic stamps the content hash

    // The queue is drained.
    Database db;
    QVERIFY(db.open(dbFile(), QStringLiteral("q")));
    QVERIFY(db.sidecarDirtyIds().isEmpty());
    QSqlDatabase::removeDatabase(QStringLiteral("q"));
}

void TestImport::flushImportsNewerSidecarFirst()
{
    const QString photo = pics() + QStringLiteral("/both.jpg");
    writeFile(photo, "bytes");
    const int id = importAndId(photo);
    QVERIFY(id > 0);

    // omapic has one tag queued; meanwhile another app wrote a sidecar with a
    // different keyword. The flush must import theirs before rewriting, so both end
    // up in the file (and the library) rather than ours clobbering theirs.
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mut2")));
        const int tag = db.ensureTag(QStringLiteral("ours"));
        db.linkTagToContent(id, tag);
        db.rememberContentTag(db.photoHash(id), tag);
        db.enqueueSidecarDirty({id});
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mut2"));

    writeFile(photo + QStringLiteral(".xmp"),
        "<?xpacket begin='' id='W5M0MpCehiHzreSzNTczkc9d'?>\n"
        "<x:xmpmeta xmlns:x='adobe:ns:meta/'>\n"
        " <rdf:RDF xmlns:rdf='http://www.w3.org/1999/02/22-rdf-syntax-ns#'>\n"
        "  <rdf:Description rdf:about='' xmlns:dc='http://purl.org/dc/elements/1.1/'>\n"
        "   <dc:subject><rdf:Bag><rdf:li>theirs</rdf:li></rdf:Bag></dc:subject>\n"
        "  </rdf:Description>\n"
        " </rdf:RDF>\n"
        "</x:xmpmeta>\n"
        "<?xpacket end='w'?>\n");

    m_worker->flushSidecars();

    const auto back = sidecar::readSidecar(photo);
    QVERIFY(back.has_value());
    QVERIFY2(back->flat.contains(QStringLiteral("ours")), qPrintable(back->flat.join(',')));
    QVERIFY2(back->flat.contains(QStringLiteral("theirs")), qPrintable(back->flat.join(',')));
    QVERIFY(tagsOf(photo).contains(QStringLiteral("theirs"))); // imported into the library too
}

void TestImport::flushRemovesOmapicSidecarWhenUntagged()
{
    const QString photo = pics() + QStringLiteral("/gone.jpg");
    writeFile(photo, "bytes");
    const int id = importAndId(photo);
    QVERIFY(id > 0);
    const QString side = photo + QStringLiteral(".xmp");

    int tag = -1;
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mut3")));
        tag = db.ensureTag(QStringLiteral("temp"));
        db.linkTagToContent(id, tag);
        db.rememberContentTag(db.photoHash(id), tag);
        db.enqueueSidecarDirty({id});
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mut3"));
    m_worker->flushSidecars();
    QVERIFY(QFile::exists(side)); // omapic created it

    // Remove the only tag and flush again: the omapic-only sidecar is deleted.
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mut4")));
        db.unlinkTagFromContent(id, tag);
        db.forgetContentTag(db.photoHash(id), tag);
        db.enqueueSidecarDirty({id});
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mut4"));
    m_worker->flushSidecars();
    QVERIFY(!QFile::exists(side));
}

void TestImport::flushSkipsDisabledFolderUntilReenabled()
{
    const QString photo = pics() + QStringLiteral("/hidden.jpg");
    writeFile(photo, "bytes");
    const int id = importAndId(photo); // importDirectory registers pics() as a folder
    QVERIFY(id > 0);
    const QString side = photo + QStringLiteral(".xmp");

    int folderId = -1;
    const auto setPicsEnabled = [&](bool on) {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("fld")));
        for (const FolderInfo &f : db.loadFolders())
            if (f.path == pics())
                folderId = f.id;
        if (folderId > 0)
            db.setFolderEnabled(folderId, on);
        QSqlDatabase::removeDatabase(QStringLiteral("fld"));
    };

    // Tag + queue the photo, then disable its folder before flushing.
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mut5")));
        const int tag = db.ensureTag(QStringLiteral("hiddentag"));
        db.linkTagToContent(id, tag);
        db.rememberContentTag(db.photoHash(id), tag);
        db.enqueueSidecarDirty({id});
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mut5"));
    setPicsEnabled(false);

    m_worker->flushSidecars();
    QVERIFY2(!QFile::exists(side), "a disabled folder must never be written");

    // The entry stays queued; re-enabling and flushing writes it.
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("q5")));
        QVERIFY(db.sidecarDirtyIds().contains(id)); // still pending, not dropped
        QSqlDatabase::removeDatabase(QStringLiteral("q5"));
    }
    setPicsEnabled(true);
    m_worker->flushSidecars();
    QVERIFY(QFile::exists(side));
    QVERIFY(sidecar::readSidecar(photo)->flat.contains(QStringLiteral("hiddentag")));
}

void TestImport::rescanPrunesOldRemovedTags()
{
    const QString photo = pics() + QStringLiteral("/temp.jpg");
    writeFile(photo, "bytes");
    const int id = importAndId(photo);
    QVERIFY(id > 0);

    QString hash;
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("ret")));
        const int tag = db.ensureTag(QStringLiteral("doomed"));
        db.linkTagToContent(id, tag);
        hash = db.photoHash(id);
        db.rememberContentTag(hash, tag);
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ret"));

    // The file disappears from disk; a rescan drops the row and orphans its hash.
    QVERIFY(QFile::remove(photo));
    m_worker->rescan(/*retentionDays=*/0); // forever: nothing pruned yet
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("ret2")));
        QCOMPARE(db.rememberedRemovedCount(), 1);
        // Backdate the orphan to 100 days ago.
        QSqlQuery u(QSqlDatabase::database(QStringLiteral("ret2")));
        u.prepare(QStringLiteral("UPDATE orphaned_hashes SET orphaned_at = ? WHERE hash = ?"));
        u.addBindValue(QDateTime::currentSecsSinceEpoch() - qint64(100) * 24 * 60 * 60);
        u.addBindValue(hash);
        QVERIFY(u.exec());
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ret2"));

    // A rescan under a 30-day policy forgets it.
    m_worker->rescan(/*retentionDays=*/30);
    Database db;
    QVERIFY(db.open(dbFile(), QStringLiteral("ret3")));
    QCOMPARE(db.rememberedRemovedCount(), 0);
    QSqlDatabase::removeDatabase(QStringLiteral("ret3"));
}

void TestImport::rescanReportsWhetherAnythingChanged()
{
    const QString photo = pics() + QStringLiteral("/stable.jpg");
    writeFile(photo, "bytes");
    m_worker->importDirectory(pics());

    QSignalSpy spy(m_worker, &LibraryWorker::rescanFinished);

    // Nothing on disk moved since the import: the rescan reports no change, so the
    // GUI can skip the model reload that would otherwise blink every thumbnail.
    m_worker->rescan(/*retentionDays=*/0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);

    // A new file appears → a row is added → changed.
    writeFile(pics() + QStringLiteral("/fresh.jpg"), "more-bytes");
    m_worker->rescan(/*retentionDays=*/0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);

    // A file disappears → a row is dropped → changed.
    QVERIFY(QFile::remove(photo));
    m_worker->rescan(/*retentionDays=*/0);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
}

void TestImport::migrationReportsWhetherAnythingChanged()
{
    const QString photo = pics() + QStringLiteral("/m.jpg");
    writeFile(photo, "bytes");
    m_worker->importDirectory(pics()); // import already computes hash + taken_at

    QSignalSpy spy(m_worker, &LibraryWorker::migrationFinished);

    // Nothing to backfill: the migration is a no-op and reports no change, so the
    // launch reload that would blink the thumbnails is skipped.
    m_worker->runMigration();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), false);

    // A row with no content hash gives the hash backfill real work → changed.
    const QString photo2 = pics() + QStringLiteral("/n.jpg");
    writeFile(photo2, "other-bytes");
    {
        Database db;
        QVERIFY(db.open(dbFile(), QStringLiteral("mig")));
        QVERIFY(db.insertPhoto(photo2, QString()) > 0); // empty hash
    }
    QSqlDatabase::removeDatabase(QStringLiteral("mig"));
    m_worker->runMigration();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.takeFirst().at(0).toBool(), true);
}

QTEST_GUILESS_MAIN(TestImport)
#include "tst_import.moc"
