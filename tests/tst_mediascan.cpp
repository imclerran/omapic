// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDir>
#include <QSet>
#include <QTemporaryDir>

#include "mediascan.h"

// mediascan is the single source of truth for "what counts as media" and the
// .nomedia exclusion, shared by the library scanner and the standalone viewer.
class TestMediaScan : public QObject {
    Q_OBJECT

private slots:
    void isMediaFileRecognizesImagesAndVideos();
    void isMediaFileCaseInsensitive();
    void isMediaFileRejectsNonMedia();
    void collectFindsMediaRecursively();
    void collectMatchesUppercaseExtensions();
    void collectHonorsNomedia();
    void collectIgnoresEmptyAndMissingRoot();
    void collectSortedIsCaseInsensitiveOrder();

private:
    // Create an empty file at dir/relPath, making parent directories as needed.
    static void touch(const QString &base, const QString &relPath);
};

void TestMediaScan::touch(const QString &base, const QString &relPath)
{
    const QString full = base + QLatin1Char('/') + relPath;
    QVERIFY(QDir().mkpath(QFileInfo(full).path()));
    QFile f(full);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.close();
}

void TestMediaScan::isMediaFileRecognizesImagesAndVideos()
{
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.jpg")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.jpeg")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.png")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.heic")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.avif")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.mp4")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.mkv")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/a.mov")));
}

void TestMediaScan::isMediaFileCaseInsensitive()
{
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/A.JPG")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/clip.MP4")));
    QVERIFY(mediascan::isMediaFile(QStringLiteral("/photos/img.Jpeg")));
}

void TestMediaScan::isMediaFileRejectsNonMedia()
{
    QVERIFY(!mediascan::isMediaFile(QStringLiteral("/photos/notes.txt")));
    QVERIFY(!mediascan::isMediaFile(QStringLiteral("/photos/raw.cr2")));
    QVERIFY(!mediascan::isMediaFile(QStringLiteral("/photos/noext")));
    QVERIFY(!mediascan::isMediaFile(QStringLiteral("/photos/.hidden")));
    QVERIFY(!mediascan::isMediaFile(QString()));
}

void TestMediaScan::collectFindsMediaRecursively()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();

    touch(root, QStringLiteral("top.jpg"));
    touch(root, QStringLiteral("sub/nested.png"));
    touch(root, QStringLiteral("sub/deep/clip.mp4"));
    touch(root, QStringLiteral("sub/readme.txt"));   // not media
    touch(root, QStringLiteral("sub/archive.zip"));  // not media

    QSet<QString> out;
    mediascan::collect(root, out);

    QCOMPARE(out.size(), 3);
    QVERIFY(out.contains(root + QStringLiteral("/top.jpg")));
    QVERIFY(out.contains(root + QStringLiteral("/sub/nested.png")));
    QVERIFY(out.contains(root + QStringLiteral("/sub/deep/clip.mp4")));
}

void TestMediaScan::collectMatchesUppercaseExtensions()
{
    // isMediaFile() lowercases the suffix, so it is case-insensitive. collect()
    // must agree, or a file like PHOTO.JPG would be reported as media yet never
    // picked up by a scan. This pins that the two stay consistent.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();

    touch(root, QStringLiteral("PHOTO.JPG"));
    touch(root, QStringLiteral("Clip.MOV"));
    touch(root, QStringLiteral("mixed.JpEg"));

    for (const QString &rel : {QStringLiteral("PHOTO.JPG"), QStringLiteral("Clip.MOV"),
                               QStringLiteral("mixed.JpEg")}) {
        QVERIFY2(mediascan::isMediaFile(root + QLatin1Char('/') + rel),
                 qPrintable(QStringLiteral("isMediaFile should accept ") + rel));
    }

    QSet<QString> out;
    mediascan::collect(root, out);
    QCOMPARE(out.size(), 3);
    QVERIFY(out.contains(root + QStringLiteral("/PHOTO.JPG")));
    QVERIFY(out.contains(root + QStringLiteral("/Clip.MOV")));
    QVERIFY(out.contains(root + QStringLiteral("/mixed.JpEg")));
}

void TestMediaScan::collectIgnoresEmptyAndMissingRoot()
{
    // An empty root string returns early; a non-existent path yields nothing.
    // Neither must throw, and both leave any pre-existing results untouched.
    QSet<QString> out;
    mediascan::collect(QString(), out);
    QVERIFY(out.isEmpty());
    mediascan::collect(QStringLiteral("/no/such/directory/omapic-test"), out);
    QVERIFY(out.isEmpty());

    QCOMPARE(mediascan::collectSorted(QString()), QStringList());
}

void TestMediaScan::collectHonorsNomedia()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();

    touch(root, QStringLiteral("keep.jpg"));
    touch(root, QStringLiteral("private/secret.jpg"));
    touch(root, QStringLiteral("private/.nomedia"));
    // A subtree below an excluded folder stays excluded (ancestor walk).
    touch(root, QStringLiteral("private/more/also_secret.png"));
    touch(root, QStringLiteral("shared/ok.png"));

    QSet<QString> out;
    mediascan::collect(root, out);

    QCOMPARE(out.size(), 2);
    QVERIFY(out.contains(root + QStringLiteral("/keep.jpg")));
    QVERIFY(out.contains(root + QStringLiteral("/shared/ok.png")));
    QVERIFY(!out.contains(root + QStringLiteral("/private/secret.jpg")));
    QVERIFY(!out.contains(root + QStringLiteral("/private/more/also_secret.png")));
}

void TestMediaScan::collectSortedIsCaseInsensitiveOrder()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString root = dir.path();

    touch(root, QStringLiteral("Banana.jpg"));
    touch(root, QStringLiteral("apple.jpg"));
    touch(root, QStringLiteral("cherry.png"));

    const QStringList sorted = mediascan::collectSorted(root);
    QCOMPARE(sorted.size(), 3);
    // Case-insensitive ordering: apple, Banana, cherry (not ASCII order where
    // capital 'B' would precede lowercase 'a').
    QVERIFY(sorted.at(0).endsWith(QStringLiteral("apple.jpg")));
    QVERIFY(sorted.at(1).endsWith(QStringLiteral("Banana.jpg")));
    QVERIFY(sorted.at(2).endsWith(QStringLiteral("cherry.png")));
}

QTEST_GUILESS_MAIN(TestMediaScan)
#include "tst_mediascan.moc"
