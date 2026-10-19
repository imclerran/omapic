// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDate>
#include <QDateTime>
#include <QList>
#include <QLocale>
#include <QSet>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "photo.h"
#include "photofiltermodel.h"
#include "photomodel.h"

// PhotoFilterModel is the live gallery filter + sort: tag match-all/any, folded
// text search over names and tags, tagged/untagged restriction, duplicate
// hiding, and natural-order sorting. It runs headless over a plain PhotoModel.
class TestPhotoFilterModel : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void matchAllRequiresEveryTag();
    void matchAnyAcceptsOneTag();
    void searchMatchesNameAndTagFolded();
    void tagPresenceFilter();
    void dedupeHidesDuplicates();
    void naturalSortOrder();
    void containsIdTracksVisibility();

    // Folder-view directory gate
    void folderModeRootShowsOrphansOnly();
    void folderModeInsideShowsImmediateChildren();
    void folderModeSearchFlattensSubtree();

    // Timeline (date sort + month sections)
    void sortByDateNewestFirstUndatedLast();
    void monthGroupsSectionsOverDateOrder();

    // Album view
    void albumModeInsideFiltersByAlbumQuery();
    void albumModeListLevelFlattenedSearch();

    // Misc ordering / combinations / bounds
    void folderFirstGroupsByFolder();
    void combinedTagAndSearchFilters();
    void accessorsHandleOutOfRange();

    // setPhotos no-op guard (keeps launch reloads from blinking the thumbnails)
    void setPhotosSkipsResetWhenUnchanged();

private:
    static Photo makePhoto(int id, const QString &path, const QString &name,
                           const QSet<int> &tagIds = {}, const QStringList &tagNames = {});
    // A photo with a capture time, named after its path's last component.
    static Photo datedPhoto(int id, const QString &path, qint64 takenAt);
    // Current proxy order as a list of photo ids.
    QList<int> proxyIds() const;

    PhotoModel *m_source = nullptr;
    PhotoFilterModel *m_proxy = nullptr;
};

Photo TestPhotoFilterModel::makePhoto(int id, const QString &path, const QString &name,
                                      const QSet<int> &tagIds, const QStringList &tagNames)
{
    Photo p;
    p.id = id;
    p.path = path;
    p.fileName = name;
    p.tagIds = tagIds;
    p.tagNames = tagNames;
    return p;
}

Photo TestPhotoFilterModel::datedPhoto(int id, const QString &path, qint64 takenAt)
{
    Photo p = makePhoto(id, path, path.mid(path.lastIndexOf(QLatin1Char('/')) + 1));
    p.takenAt = takenAt;
    return p;
}

QList<int> TestPhotoFilterModel::proxyIds() const
{
    QList<int> ids;
    for (int row = 0; row < m_proxy->rowCount(); ++row)
        ids.append(m_proxy->idAt(row));
    return ids;
}

void TestPhotoFilterModel::init()
{
    m_source = new PhotoModel;
    m_proxy = new PhotoFilterModel;
    m_proxy->setSourceModel(m_source);
    m_proxy->sort(0); // activate lessThan ordering
}

void TestPhotoFilterModel::cleanup()
{
    delete m_proxy;
    m_proxy = nullptr;
    delete m_source;
    m_source = nullptr;
}

void TestPhotoFilterModel::matchAllRequiresEveryTag()
{
    const int dog = 1, park = 2;
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {dog}),
        makePhoto(11, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg"), {dog, park}),
        makePhoto(12, QStringLiteral("/p/c.jpg"), QStringLiteral("c.jpg"), {park}),
    };
    m_source->setPhotos(photos);

    m_proxy->setMatchAll(true);
    m_proxy->setSelectedTagIds({dog, park});
    QCOMPARE(m_proxy->count(), 1);
    QCOMPARE(proxyIds(), QList<int>{11}); // only the photo carrying both
}

void TestPhotoFilterModel::matchAnyAcceptsOneTag()
{
    const int dog = 1, park = 2;
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {dog}),
        makePhoto(11, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg"), {dog, park}),
        makePhoto(12, QStringLiteral("/p/c.jpg"), QStringLiteral("c.jpg"), {park}),
        makePhoto(13, QStringLiteral("/p/d.jpg"), QStringLiteral("d.jpg"), {}),
    };
    m_source->setPhotos(photos);

    m_proxy->setMatchAll(false);
    m_proxy->setSelectedTagIds({dog, park});
    QCOMPARE(m_proxy->count(), 3); // a, b, c but not the untagged d
    QCOMPARE(proxyIds(), (QList<int>{10, 11, 12}));
}

void TestPhotoFilterModel::searchMatchesNameAndTagFolded()
{
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/sunset.jpg"), QStringLiteral("sunset.jpg")),
        makePhoto(11, QStringLiteral("/p/IMG_1.jpg"), QStringLiteral("IMG_1.jpg"),
                  {1}, {QStringLiteral("Café")}),
        makePhoto(12, QStringLiteral("/p/IMG_2.jpg"), QStringLiteral("IMG_2.jpg"),
                  {2}, {QStringLiteral("mountain")}),
    };
    m_source->setPhotos(photos);

    // Matches a file name.
    m_proxy->setSearchText(QStringLiteral("sunset"));
    QCOMPARE(proxyIds(), QList<int>{10});

    // Matches a tag name, accent- and case-insensitively ("cafe" finds "Café").
    m_proxy->setSearchText(QStringLiteral("cafe"));
    QCOMPARE(proxyIds(), QList<int>{11});

    // No match.
    m_proxy->setSearchText(QStringLiteral("zzz"));
    QCOMPARE(m_proxy->count(), 0);

    // Clearing the search shows everything again.
    m_proxy->setSearchText(QString());
    QCOMPARE(m_proxy->count(), 3);
}

void TestPhotoFilterModel::tagPresenceFilter()
{
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {1}, {QStringLiteral("x")}),
        makePhoto(11, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg")),
        makePhoto(12, QStringLiteral("/p/c.jpg"), QStringLiteral("c.jpg"), {2}, {QStringLiteral("y")}),
    };
    m_source->setPhotos(photos);

    m_proxy->setTagPresence(PhotoFilterModel::TaggedOnly);
    QCOMPARE(proxyIds(), (QList<int>{10, 12}));

    m_proxy->setTagPresence(PhotoFilterModel::UntaggedOnly);
    QCOMPARE(proxyIds(), QList<int>{11});

    m_proxy->setTagPresence(PhotoFilterModel::AnyTags);
    QCOMPARE(m_proxy->count(), 3);
}

void TestPhotoFilterModel::dedupeHidesDuplicates()
{
    Photo original = makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"));
    Photo dup = makePhoto(11, QStringLiteral("/p/copy.jpg"), QStringLiteral("copy.jpg"));
    dup.isDuplicate = true;
    m_source->setPhotos({original, dup});

    // Dedupe is on by default (ungrouped view): the duplicate is hidden.
    QVERIFY(m_proxy->dedupe());
    QCOMPARE(proxyIds(), QList<int>{10});

    // Folder/timeline views turn it off and show every copy.
    m_proxy->setDedupe(false);
    QCOMPARE(m_proxy->count(), 2);
}

void TestPhotoFilterModel::naturalSortOrder()
{
    // Inserted out of order and with numbers that sort wrong lexically.
    QVector<Photo> photos{
        makePhoto(1, QStringLiteral("/p/IMG_10.jpg"), QStringLiteral("IMG_10.jpg")),
        makePhoto(2, QStringLiteral("/p/IMG_2.jpg"), QStringLiteral("IMG_2.jpg")),
        makePhoto(3, QStringLiteral("/p/IMG_1.jpg"), QStringLiteral("IMG_1.jpg")),
        makePhoto(4, QStringLiteral("/p/IMG_100.jpg"), QStringLiteral("IMG_100.jpg")),
    };
    m_source->setPhotos(photos);

    // Numeric-aware: 1 < 2 < 10 < 100, not the lexical 1,10,100,2.
    QCOMPARE(proxyIds(), (QList<int>{3, 2, 1, 4}));
}

void TestPhotoFilterModel::containsIdTracksVisibility()
{
    const int dog = 1;
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {dog}),
        makePhoto(11, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg")),
    };
    m_source->setPhotos(photos);

    m_proxy->setSelectedTagIds({dog});
    QVERIFY(m_proxy->containsId(10));   // passes the filter
    QVERIFY(!m_proxy->containsId(11));  // filtered out
    QVERIFY(!m_proxy->containsId(999)); // not in the source at all
}

// --- Folder-view directory gate ---------------------------------------------

void TestPhotoFilterModel::folderModeRootShowsOrphansOnly()
{
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/lib/a.jpg"), QStringLiteral("a.jpg")),
        makePhoto(2, QStringLiteral("/lib/sub/b.jpg"), QStringLiteral("b.jpg")),
        makePhoto(3, QStringLiteral("/orphan/c.jpg"), QStringLiteral("c.jpg")),
    });
    m_proxy->setFolderRoots({QStringLiteral("/lib")});

    // Folder-view root with no search: only orphans (photos under no root) show.
    m_proxy->applyView(/*dedupe=*/false, /*sortByDate=*/false, /*folderMode=*/true,
                       /*folderPath=*/QString());
    QCOMPARE(proxyIds(), QList<int>{3});
}

void TestPhotoFilterModel::folderModeInsideShowsImmediateChildren()
{
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/lib/a.jpg"), QStringLiteral("a.jpg")),     // immediate child
        makePhoto(2, QStringLiteral("/lib/sub/b.jpg"), QStringLiteral("b.jpg")), // nested, excluded
        makePhoto(3, QStringLiteral("/other/c.jpg"), QStringLiteral("c.jpg")),   // elsewhere
    });

    // Inside "/lib": only photos directly in that directory, not nested deeper
    // (those are reached by descending into the subfolder).
    m_proxy->applyView(false, false, /*folderMode=*/true, /*folderPath=*/QStringLiteral("/lib"));
    QCOMPARE(proxyIds(), QList<int>{1});
}

void TestPhotoFilterModel::folderModeSearchFlattensSubtree()
{
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/lib/IMG1.jpg"), QStringLiteral("IMG1.jpg")),
        makePhoto(2, QStringLiteral("/lib/sub/IMG2.jpg"), QStringLiteral("IMG2.jpg")),
        makePhoto(3, QStringLiteral("/outside/IMG3.jpg"), QStringLiteral("IMG3.jpg")),
    });

    m_proxy->applyView(false, false, /*folderMode=*/true, /*folderPath=*/QStringLiteral("/lib"));
    // Searching flattens the folder to its whole subtree: the nested IMG2 now shows
    // alongside the immediate IMG1, but IMG3 outside "/lib" stays out.
    m_proxy->setSearchText(QStringLiteral("img"));
    QCOMPARE(proxyIds(), (QList<int>{1, 2}));
}

// --- Timeline (date sort + month sections) ----------------------------------

void TestPhotoFilterModel::sortByDateNewestFirstUndatedLast()
{
    const qint64 mar20 = QDateTime(QDate(2024, 3, 20), QTime(12, 0)).toSecsSinceEpoch();
    const qint64 mar15 = QDateTime(QDate(2024, 3, 15), QTime(12, 0)).toSecsSinceEpoch();
    const qint64 dec15 = QDateTime(QDate(2023, 12, 15), QTime(12, 0)).toSecsSinceEpoch();

    m_source->setPhotos({
        datedPhoto(1, QStringLiteral("/p/a.jpg"), mar15),
        datedPhoto(2, QStringLiteral("/p/b.jpg"), mar20),
        datedPhoto(3, QStringLiteral("/p/c.jpg"), dec15),
        datedPhoto(4, QStringLiteral("/p/d.jpg"), 0), // undated
    });

    m_proxy->applyView(false, /*sortByDate=*/true, false, QString());
    // Newest first; the undated photo (takenAt 0) sorts last.
    QCOMPARE(proxyIds(), (QList<int>{2, 1, 3, 4}));
}

void TestPhotoFilterModel::monthGroupsSectionsOverDateOrder()
{
    const qint64 mar20 = QDateTime(QDate(2024, 3, 20), QTime(12, 0)).toSecsSinceEpoch();
    const qint64 mar15 = QDateTime(QDate(2024, 3, 15), QTime(12, 0)).toSecsSinceEpoch();
    const qint64 dec15 = QDateTime(QDate(2023, 12, 15), QTime(12, 0)).toSecsSinceEpoch();

    m_source->setPhotos({
        datedPhoto(1, QStringLiteral("/p/a.jpg"), mar15),
        datedPhoto(2, QStringLiteral("/p/b.jpg"), mar20),
        datedPhoto(3, QStringLiteral("/p/c.jpg"), dec15),
        datedPhoto(4, QStringLiteral("/p/d.jpg"), 0),
    });
    m_proxy->applyView(false, /*sortByDate=*/true, false, QString());

    const QVariantList groups = m_proxy->monthGroups();
    QCOMPARE(groups.size(), 3); // 2024-03, 2023-12, then undated

    // Month keys are computed the same way the model does (local time), so derive
    // the expected keys rather than hard-coding them.
    const QString marKey = QDateTime::fromSecsSinceEpoch(mar20).toString(QStringLiteral("yyyy-MM"));
    const QString decKey = QDateTime::fromSecsSinceEpoch(dec15).toString(QStringLiteral("yyyy-MM"));

    const QVariantMap g0 = groups.at(0).toMap();
    QCOMPARE(g0.value(QStringLiteral("key")).toString(), marKey);
    QCOMPARE(g0.value(QStringLiteral("firstRow")).toInt(), 0);
    QCOMPARE(g0.value(QStringLiteral("count")).toInt(), 2); // the two March photos

    const QVariantMap g1 = groups.at(1).toMap();
    QCOMPARE(g1.value(QStringLiteral("key")).toString(), decKey);
    QCOMPARE(g1.value(QStringLiteral("firstRow")).toInt(), 2);
    QCOMPARE(g1.value(QStringLiteral("count")).toInt(), 1);

    const QVariantMap g2 = groups.at(2).toMap();
    QCOMPARE(g2.value(QStringLiteral("key")).toString(), QStringLiteral("0000-00"));
    QCOMPARE(g2.value(QStringLiteral("label")).toString(), QStringLiteral("Undated"));
    QCOMPARE(g2.value(QStringLiteral("firstRow")).toInt(), 3);
    QCOMPARE(g2.value(QStringLiteral("count")).toInt(), 1);
}

// --- Album view -------------------------------------------------------------

void TestPhotoFilterModel::albumModeInsideFiltersByAlbumQuery()
{
    const int albumA = 100, albumB = 200;
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {albumA}),
        makePhoto(2, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg"), {albumB}),
        makePhoto(3, QStringLiteral("/p/c.jpg"), QStringLiteral("c.jpg"), {albumA, albumB}),
        makePhoto(4, QStringLiteral("/p/d.jpg"), QStringLiteral("d.jpg"), {}),
    });

    // A regular album is a single-tag query: show only its members.
    m_proxy->applyView(false, false, false, QString(), /*albumMode=*/true,
                       /*albumTags=*/QVariantList{albumA}, /*albumMatchAll=*/true);
    QCOMPARE(proxyIds(), (QList<int>{1, 3}));

    // A smart album in OR mode: carry any of its tags.
    m_proxy->applyView(false, false, false, QString(), true,
                       QVariantList{albumA, albumB}, /*albumMatchAll=*/false);
    QCOMPARE(proxyIds(), (QList<int>{1, 2, 3}));

    // A smart album in AND mode: carry every tag.
    m_proxy->applyView(false, false, false, QString(), true,
                       QVariantList{albumA, albumB}, /*albumMatchAll=*/true);
    QCOMPARE(proxyIds(), QList<int>{3});
}

void TestPhotoFilterModel::albumModeListLevelFlattenedSearch()
{
    const int albumA = 100, albumB = 200;
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/p/a.jpg"), QStringLiteral("beach.jpg"), {albumA}),
        makePhoto(2, QStringLiteral("/p/b.jpg"), QStringLiteral("beach2.jpg"), {}),      // in no album
        makePhoto(3, QStringLiteral("/p/c.jpg"), QStringLiteral("mountain.jpg"), {albumB}),
    });
    m_proxy->setAlbumTagIds({albumA, albumB});

    // Album-list level (no album selected): a grid of tiles, so no photos show...
    m_proxy->applyView(false, false, false, QString(), /*albumMode=*/true,
                       /*albumTags=*/QVariantList{}, true);
    QCOMPARE(m_proxy->count(), 0);

    // ...until a search flattens to "photos in any album matching the text". The
    // photo in no album (beach2) is excluded despite matching the search term.
    m_proxy->setSearchText(QStringLiteral("beach"));
    QCOMPARE(proxyIds(), QList<int>{1});
}

// --- Misc ordering / combinations / bounds ----------------------------------

void TestPhotoFilterModel::folderFirstGroupsByFolder()
{
    m_source->setPhotos({
        makePhoto(1, QStringLiteral("/b/2.jpg"), QStringLiteral("2.jpg")),
        makePhoto(2, QStringLiteral("/a/9.jpg"), QStringLiteral("9.jpg")),
        makePhoto(3, QStringLiteral("/a/1.jpg"), QStringLiteral("1.jpg")),
        makePhoto(4, QStringLiteral("/b/1.jpg"), QStringLiteral("1.jpg")),
    });

    m_proxy->setFolderFirst(true);
    // Grouped by containing folder (/a before /b), then by natural file name
    // within each folder: /a/1, /a/9, /b/1, /b/2.
    QCOMPARE(proxyIds(), (QList<int>{3, 2, 4, 1}));
}

void TestPhotoFilterModel::combinedTagAndSearchFilters()
{
    const int dog = 1, cat = 2;
    m_source->setPhotos({
        makePhoto(10, QStringLiteral("/p/park1.jpg"), QStringLiteral("park.jpg"), {dog}),
        makePhoto(11, QStringLiteral("/p/beach.jpg"), QStringLiteral("beach.jpg"), {dog}),
        makePhoto(12, QStringLiteral("/p/park2.jpg"), QStringLiteral("park.jpg"), {cat}),
    });

    // Tag filter and text search stack: must carry the tag AND match the text.
    m_proxy->setSelectedTagIds({dog});
    m_proxy->setSearchText(QStringLiteral("park"));
    QCOMPARE(proxyIds(), QList<int>{10}); // dog + "park" name; 11 fails text, 12 fails tag

    // Adding an untagged-only restriction now excludes everything (all are tagged).
    m_proxy->setTagPresence(PhotoFilterModel::UntaggedOnly);
    QCOMPARE(m_proxy->count(), 0);
}

void TestPhotoFilterModel::accessorsHandleOutOfRange()
{
    m_source->setPhotos({
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg")),
    });

    // Valid row.
    QCOMPARE(m_proxy->idAt(0), 10);
    QVERIFY(!m_proxy->sourceAt(0).isEmpty());
    QVERIFY(!m_proxy->photoAt(0).isEmpty());

    // Out-of-range rows return safe empties rather than crashing or asserting.
    QCOMPARE(m_proxy->idAt(-1), -1);
    QCOMPARE(m_proxy->idAt(999), -1);
    QVERIFY(m_proxy->sourceAt(-1).isEmpty());
    QVERIFY(m_proxy->sourceAt(999).isEmpty());
    QVERIFY(m_proxy->photoAt(-1).isEmpty());
    QVERIFY(m_proxy->photoAt(999).isEmpty());
}

void TestPhotoFilterModel::setPhotosSkipsResetWhenUnchanged()
{
    const int dog = 1;
    QVector<Photo> photos{
        makePhoto(10, QStringLiteral("/p/a.jpg"), QStringLiteral("a.jpg"), {dog}),
        makePhoto(11, QStringLiteral("/p/b.jpg"), QStringLiteral("b.jpg")),
    };
    m_source->setPhotos(photos);

    QSignalSpy resets(m_source, &QAbstractItemModel::modelReset);

    // Re-setting an identical set must not reset the model — that reset is what
    // tears down every delegate and blinks the thumbnails on a no-op launch reload.
    m_source->setPhotos(photos);
    QCOMPARE(resets.count(), 0);

    // A real change (a favorite flips) still resets.
    photos[1].favorite = true;
    m_source->setPhotos(photos);
    QCOMPARE(resets.count(), 1);
}

// Custom entry point instead of QTEST_GUILESS_MAIN: PhotoFilterModel sorts with a
// QCollator in numeric mode, and ICU only applies numeric collation under a real
// locale — the C/POSIX locale (common in minimal CI containers) silently degrades
// to lexical order. Pin a known locale so the natural-sort assertion is
// deterministic regardless of the host's configured locale. The app itself keeps
// following the user's locale; this only steers the test process.
int main(int argc, char *argv[])
{
    QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates));
    QCoreApplication app(argc, argv);
    QTEST_SET_MAIN_SOURCE_PATH
    TestPhotoFilterModel tc;
    return QTest::qExec(&tc, argc, argv);
}
#include "tst_photofiltermodel.moc"
