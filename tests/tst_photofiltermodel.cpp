// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QList>
#include <QSet>
#include <QStringList>
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

private:
    static Photo makePhoto(int id, const QString &path, const QString &name,
                           const QSet<int> &tagIds = {}, const QStringList &tagNames = {});
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

QTEST_GUILESS_MAIN(TestPhotoFilterModel)
#include "tst_photofiltermodel.moc"
