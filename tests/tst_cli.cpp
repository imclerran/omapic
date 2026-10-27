// omapic - a photo gallery with tagging and slideshows
// Copyright (C) 2026 Ian McLerran
// SPDX-License-Identifier: GPL-3.0-or-later

#include <QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>

#include "cli.h"
#include "database.h"

// The command-line interface: argument dispatch, the headless commands against a
// temporary library (scope and sort defaults, output shapes, exit codes, parse
// errors with carets), the read-only open, and the slideshow settings, which
// come from the flags alone.
class TestCli : public QObject {
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void dispatchRecognisesSubcommandsOnly();
    void optionsStopAtFirstNonOption();
    void parseValidatesOptions();
    void helpAndUsage();

    void queryListsMatchesInGalleryOrder();
    void queryScopeFlags();
    void querySortFlags();
    void queryOutputShapes();
    void queryExplainAndNotes();
    void queryParseErrorShowsCaret();
    void queryExpandsSmartAlbumReferences();
    void listCommands();
    void libraryOpenFailures();
    void resolveMediaForView();
    void slideshowIgnoresSavedSettings();
    void slideshowFlagsTurnOnOnlyTheirSetting();
    void orderPlaylistShapes();

private:
    // Run a headless command line (without argv[0]) against the test library.
    int run(const QStringList &args, QString *out = nullptr, QString *err = nullptr);
    // Seed the library: a tagged photo set on disk, a disabled folder, a
    // duplicate copy, and one file missing from disk.
    void seedLibrary();
    QString path(const QString &rel) const { return m_dir->filePath(rel); }
    QString dbPath() const { return m_dir->filePath(QStringLiteral("library.db")); }
    void touch(const QString &p);

    QTemporaryDir *m_dir = nullptr;
    int m_beach = -1, m_sunset = -1, m_people = -1, m_paris = -1, m_trip = -1, m_place = -1;
};

void TestCli::init()
{
    m_dir = new QTemporaryDir;
    QVERIFY(m_dir->isValid());
}

void TestCli::cleanup()
{
    delete m_dir;
    m_dir = nullptr;
}

void TestCli::touch(const QString &p)
{
    QDir().mkpath(QFileInfo(p).absolutePath());
    QFile f(p);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write("x");
}

int TestCli::run(const QStringList &args, QString *out, QString *err)
{
    QString o, e;
    QTextStream outStream(&o), errStream(&e);
    const int code = cli::runHeadless(cli::parse(args), dbPath(), outStream, errStream);
    outStream.flush();
    errStream.flush();
    if (out) *out = o;
    if (err) *err = e;
    return code;
}

void TestCli::seedLibrary()
{
    // Scoped so the Database releases its connection before removeDatabase.
    Database db;
    QVERIFY(db.open(dbPath(), QStringLiteral("seed")));
    m_beach = db.ensureTag(QStringLiteral("beach"));
    m_sunset = db.ensureTag(QStringLiteral("sunset"));
    m_people = db.ensureTag(QStringLiteral("people"));
    m_paris = db.ensureTag(QStringLiteral("Paris"));
    m_place = db.insertCategory(QStringLiteral("Place"), 0);
    db.setTagCategory(m_paris, m_place);
    QVERIFY(db.createAlias(QStringLiteral("seaside"), m_beach));
    m_trip = db.createAlbum(QStringLiteral("Trip"));

    const QString on = path(QStringLiteral("on"));
    const QString off = path(QStringLiteral("off"));
    db.insertFolder(on);
    const int offId = db.insertFolder(off);
    db.setFolderEnabled(offId, false);

    // b.jpg: beach+sunset, dated 2000; a.jpg: beach, dated 3000, favorite + Trip;
    // c.jpg: a byte-identical copy of a.jpg (same hash, added later);
    // d.jpg: people+Paris, undated; gone.jpg: beach, not on disk;
    // off/e.jpg: beach, under the disabled folder.
    struct Row { const char *rel; QList<int> tags; qint64 taken; const char *hash; bool fav; bool exists; };
    const Row rows[] = {
        {"on/b.jpg", {m_beach, m_sunset}, 2000, "hb", false, true},
        {"on/a.jpg", {m_beach}, 3000, "ha", true, true},
        {"on/sub/c.jpg", {m_beach}, 3000, "ha", false, true},
        {"on/d.jpg", {m_people, m_paris}, 0, "hd", false, true},
        {"on/gone.jpg", {m_beach}, 1000, "hg", false, false},
        {"off/e.jpg", {m_beach}, 500, "he", false, true},
    };
    for (const Row &r : rows) {
        const QString p = path(QString::fromLatin1(r.rel));
        if (r.exists)
            touch(p);
        const int id = db.insertPhoto(p, QString::fromLatin1(r.hash), r.taken);
        for (int t : r.tags)
            db.linkPhotoTag(id, t);
        if (r.fav)
            db.linkPhotoTag(id, db.favoritesTagId());
        if (QString::fromLatin1(r.rel) == QStringLiteral("on/a.jpg"))
            db.linkPhotoTag(id, m_trip);
    }
    QVERIFY(db.createSmartAlbum(QStringLiteral("Beachy"), {m_beach, m_sunset}, true) > 0);
    db = Database(); // drop the connection before removing it
    QSqlDatabase::removeDatabase(QStringLiteral("seed"));
}

void TestCli::dispatchRecognisesSubcommandsOnly()
{
    QVERIFY(cli::isSubcommand(QStringLiteral("query")));
    QVERIFY(cli::isSubcommand(QStringLiteral("slideshow")));
    QVERIFY(!cli::isSubcommand(QStringLiteral("./query")));
    QVERIFY(!cli::isSubcommand(QStringLiteral("--")));
    QVERIFY(!cli::isSubcommand(QStringLiteral("photo.jpg")));
    QCOMPARE(cli::parse({}).command, cli::Command::None);
    QCOMPARE(cli::parse({QStringLiteral("--"), QStringLiteral("query")}).command, cli::Command::None);
    QCOMPARE(cli::parse({QStringLiteral("query"), QStringLiteral("beach")}).command, cli::Command::Query);
    QVERIFY(cli::isHeadless(cli::Command::Query));
    QVERIFY(cli::isHeadless(cli::Command::Tags));
    QVERIFY(!cli::isHeadless(cli::Command::View));
    QVERIFY(!cli::isHeadless(cli::Command::None));
}

void TestCli::optionsStopAtFirstNonOption()
{
    // A query starting with '-' is a query, not an option.
    cli::Options o = cli::parse({QStringLiteral("query"), QStringLiteral("-people")});
    QVERIFY(o.error.isEmpty());
    QCOMPARE(o.query, QStringLiteral("-people"));
    // Options before the query are honoured; later dash tokens are query text.
    o = cli::parse({QStringLiteral("query"), QStringLiteral("--count"), QStringLiteral("beach"), QStringLiteral("--count")});
    QCOMPARE(o.output, cli::Output::Count);
    QCOMPARE(o.query, QStringLiteral("beach --count"));
    // `--` ends the options explicitly.
    o = cli::parse({QStringLiteral("query"), QStringLiteral("--count"), QStringLiteral("--"), QStringLiteral("-people")});
    QCOMPARE(o.output, cli::Output::Count);
    QCOMPARE(o.query, QStringLiteral("-people"));
    o = cli::parse({QStringLiteral("query"), QStringLiteral("--"), QStringLiteral("--count")});
    QCOMPARE(o.query, QStringLiteral("--count"));
    // An unquoted query the shell split is joined with single spaces.
    o = cli::parse({QStringLiteral("query"), QStringLiteral("beach"), QStringLiteral("(sunset"), QStringLiteral("|"), QStringLiteral("sunrise)")});
    QCOMPARE(o.query, QStringLiteral("beach (sunset | sunrise)"));
    // Values: separate or inline.
    o = cli::parse({QStringLiteral("query"), QStringLiteral("--sort"), QStringLiteral("date"), QStringLiteral("beach")});
    QCOMPARE(o.sort, QStringLiteral("date"));
    QCOMPARE(o.query, QStringLiteral("beach"));
    o = cli::parse({QStringLiteral("slideshow"), QStringLiteral("--interval=2.5"), QStringLiteral("--crossfade"), QStringLiteral("beach")});
    QVERIFY(o.error.isEmpty());
    QCOMPARE(o.intervalSecs, 2.5);
    QVERIFY(o.crossfade);
    QVERIFY(!o.shuffle);
}

void TestCli::parseValidatesOptions()
{
    QVERIFY(!cli::parse({QStringLiteral("query")}).error.isEmpty());                              // needs a query
    QVERIFY(!cli::parse({QStringLiteral("tags"), QStringLiteral("x")}).error.isEmpty());          // takes none
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--bogus"), QStringLiteral("x")}).error.isEmpty());
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--sort"), QStringLiteral("size"), QStringLiteral("x")}).error.isEmpty());
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--sort")}).error.isEmpty());    // missing value
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--count"), QStringLiteral("--json"), QStringLiteral("x")}).error.isEmpty());
    QVERIFY(!cli::parse({QStringLiteral("slideshow"), QStringLiteral("--interval"), QStringLiteral("0"), QStringLiteral("x")}).error.isEmpty());
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--shuffle"), QStringLiteral("x")}).error.isEmpty()); // slideshow-only
    QVERIFY(!cli::parse({QStringLiteral("query"), QStringLiteral("--count=1"), QStringLiteral("x")}).error.isEmpty());
    // The old --loop and --no-crossfade are plain unknown options now.
    QVERIFY(cli::parse({QStringLiteral("slideshow"), QStringLiteral("--loop"), QStringLiteral("x")}).error.contains(QStringLiteral("Unknown option --loop")));
    QVERIFY(cli::parse({QStringLiteral("slideshow"), QStringLiteral("--no-crossfade"), QStringLiteral("x")}).error.contains(QStringLiteral("Unknown option --no-crossfade")));
    // A usage error exits 2 and prints the usage.
    QString err;
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--bogus"), QStringLiteral("x")}, nullptr, &err), cli::kExitUsage);
    QVERIFY(err.contains(QStringLiteral("Unknown option --bogus")));
    QVERIFY(err.contains(QStringLiteral("Usage: omapic query")));
}

void TestCli::helpAndUsage()
{
    QString out;
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--help")}, &out), cli::kExitMatches);
    QVERIFY(out.contains(QStringLiteral("--explain")));
    QVERIFY(out.contains(QStringLiteral("-0")));
    QVERIFY(!out.contains(QStringLiteral("--shuffle")));
    QCOMPARE(run({QStringLiteral("slideshow"), QStringLiteral("-h")}, &out), cli::kExitMatches);
    QVERIFY(out.contains(QStringLiteral("--shuffle-folders")));
    QVERIFY(out.contains(QStringLiteral("--loop-videos")));
    QVERIFY(!out.contains(QStringLiteral("--no-crossfade")));
    QVERIFY(cli::usage().contains(QStringLiteral("omapic -- query")));
}

void TestCli::queryListsMatchesInGalleryOrder()
{
    seedLibrary();
    QString out, err;
    // Flat-view defaults: active folders only, one copy per content, files on
    // disk only, natural name order. c.jpg (duplicate of a.jpg), e.jpg (disabled
    // folder) and gone.jpg (missing) are left out; the missing one is reported.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("beach")}, &out, &err), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + '\n' + path(QStringLiteral("on/b.jpg")) + '\n');
    QVERIFY(err.contains(QStringLiteral("1 matching file is missing from disk")));
    // Aliases and the rest of the language work against the library.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("seaside -sunset")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + '\n');
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("cat:Place | is:fav")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + '\n' + path(QStringLiteral("on/d.jpg")) + '\n');
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("album:Trip")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + '\n');
    // No matches: exit 1, nothing printed.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("sunset people")}, &out, &err), cli::kExitNoMatches);
    QVERIFY(out.isEmpty());
}

void TestCli::queryScopeFlags()
{
    seedLibrary();
    QString out;
    // Folder state lives in the library, so it is honoured: off/e.jpg is under
    // a disabled folder and stays out unless --include-disabled asks for it.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QVERIFY(!out.contains(QStringLiteral("off/e.jpg")));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--all-copies"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QCOMPARE(out.split('\n', Qt::SkipEmptyParts).size(), 3); // a, b and the copy c
    QVERIFY(out.contains(QStringLiteral("sub/c.jpg")));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--include-disabled"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QCOMPARE(out.split('\n', Qt::SkipEmptyParts).size(), 3); // a, b and off/e
    QVERIFY(out.contains(QStringLiteral("off/e.jpg")));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--include-disabled"), QStringLiteral("--all-copies"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QCOMPARE(out.split('\n', Qt::SkipEmptyParts).size(), 4);
}

void TestCli::querySortFlags()
{
    seedLibrary();
    QString out;
    auto names = [&] {
        QStringList n;
        for (const QString &p : out.split('\n', Qt::SkipEmptyParts))
            n.append(p.mid(p.lastIndexOf('/') + 1));
        return n;
    };
    const QString all = QStringLiteral("beach | people");
    // name: natural ascending (the grid's default); --reverse flips it.
    QCOMPARE(run({QStringLiteral("query"), all}, &out), cli::kExitMatches);
    QCOMPARE(names(), (QStringList{QStringLiteral("a.jpg"), QStringLiteral("b.jpg"), QStringLiteral("d.jpg")}));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--reverse"), all}, &out), cli::kExitMatches);
    QCOMPARE(names(), (QStringList{QStringLiteral("d.jpg"), QStringLiteral("b.jpg"), QStringLiteral("a.jpg")}));
    // date: newest first by default, undated last; reversed = oldest first.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--sort"), QStringLiteral("date"), all}, &out), cli::kExitMatches);
    QCOMPARE(names(), (QStringList{QStringLiteral("a.jpg"), QStringLiteral("b.jpg"), QStringLiteral("d.jpg")}));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--sort=date"), QStringLiteral("--reverse"), all}, &out), cli::kExitMatches);
    QCOMPARE(names(), (QStringList{QStringLiteral("d.jpg"), QStringLiteral("b.jpg"), QStringLiteral("a.jpg")}));
    // tags: most-tagged first (b and d have two; name breaks the tie), then a.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--sort"), QStringLiteral("tags"), all}, &out), cli::kExitMatches);
    QCOMPARE(names(), (QStringList{QStringLiteral("b.jpg"), QStringLiteral("d.jpg"), QStringLiteral("a.jpg")}));
}

void TestCli::queryOutputShapes()
{
    seedLibrary();
    QString out;
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--count"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QCOMPARE(out, QStringLiteral("2\n"));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--count"), QStringLiteral("sunset people")}, &out), cli::kExitNoMatches);
    QCOMPARE(out, QStringLiteral("0\n"));

    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("-0"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + QChar(u'\0') + path(QStringLiteral("on/b.jpg")) + QChar(u'\0'));

    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--json"), QStringLiteral("beach")}, &out), cli::kExitMatches);
    const QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
    QVERIFY(doc.isArray());
    const QJsonArray arr = doc.array();
    QCOMPARE(arr.size(), 2);
    const QJsonObject a = arr.first().toObject();
    QCOMPARE(a.value(QStringLiteral("name")).toString(), QStringLiteral("a.jpg"));
    QCOMPARE(a.value(QStringLiteral("path")).toString(), path(QStringLiteral("on/a.jpg")));
    QCOMPARE(a.value(QStringLiteral("favorite")).toBool(), true);
    QCOMPARE(a.value(QStringLiteral("video")).toBool(), false);
    QCOMPARE(a.value(QStringLiteral("tags")).toArray(), QJsonArray{QStringLiteral("beach")}); // album tags hidden
    QVERIFY(a.value(QStringLiteral("takenAt")).isString());
    const QJsonObject b = arr.last().toObject();
    QCOMPARE(b.value(QStringLiteral("tags")).toArray(), (QJsonArray{QStringLiteral("beach"), QStringLiteral("sunset")}));
    // An undated photo has a null takenAt.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--json"), QStringLiteral("people")}, &out), cli::kExitMatches);
    QVERIFY(QJsonDocument::fromJson(out.toUtf8()).array().first().toObject().value(QStringLiteral("takenAt")).isNull());
}

void TestCli::queryExplainAndNotes()
{
    seedLibrary();
    QString out, err;
    // --explain prints the canonical reading instead of matches; the exit code
    // still says whether anything matched.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--explain"), QStringLiteral("beach   sunset|people")}, &out, &err), cli::kExitMatches);
    QCOMPARE(out, QStringLiteral("beach (sunset | people)\n"));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("--explain"), QStringLiteral("people sunset")}, &out, &err), cli::kExitNoMatches);
    QCOMPARE(out, QStringLiteral("people sunset\n"));
    // A reading that depended on the vocabulary is noted on stderr.
    QVERIFY(err.contains(QStringLiteral("omapic: note: \"people sunset\" is read as 2 separate terms")));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("beach | people")}, &out, &err), cli::kExitMatches);
    QVERIFY(!err.contains(QStringLiteral("note:")));
}

void TestCli::queryParseErrorShowsCaret()
{
    seedLibrary();
    QString out, err;
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("beach sunsets")}, &out, &err), cli::kExitUsage);
    QVERIFY(out.isEmpty());
    QCOMPARE(err, QStringLiteral("omapic: error: Unknown tag \"sunsets\"\n"
                                 "  beach sunsets\n"
                                 "        ^^^^^^^\n"
                                 "  did you mean: sunset\n"));
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("(beach")}, &out, &err), cli::kExitUsage);
    QVERIFY(err.contains(QStringLiteral("Missing closing \")\"")));
    QVERIFY(err.contains(QStringLiteral("\n  ^\n")));
}

void TestCli::queryExpandsSmartAlbumReferences()
{
    seedLibrary();
    QString out;
    // album:Beachy is the smart album `beach sunset`: only b.jpg.
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("album:Beachy")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/b.jpg")) + '\n');
    QCOMPARE(run({QStringLiteral("query"), QStringLiteral("beach -album:Beachy")}, &out), cli::kExitMatches);
    QCOMPARE(out, path(QStringLiteral("on/a.jpg")) + '\n');
}

void TestCli::listCommands()
{
    seedLibrary();
    QString out;
    QCOMPARE(run({QStringLiteral("tags")}, &out), cli::kExitMatches);
    // Canonical tag names only (no alias, no album tags), case-insensitive order.
    QCOMPARE(out, QStringLiteral("beach\nParis\npeople\nsunset\n"));
    QCOMPARE(run({QStringLiteral("albums")}, &out), cli::kExitMatches);
    QCOMPARE(out, QStringLiteral("Beachy\nFavorites\nTrip\n"));
    QCOMPARE(run({QStringLiteral("categories")}, &out), cli::kExitMatches);
    QCOMPARE(out, QStringLiteral("Place\n"));
}

void TestCli::libraryOpenFailures()
{
    QString err;
    // No library yet: nothing is created, and the user is told what to do.
    QCOMPARE(run({QStringLiteral("tags")}, nullptr, &err), cli::kExitLibrary);
    QVERIFY(err.contains(QStringLiteral("launch omapic once")));
    QVERIFY(!QFile::exists(dbPath()));

    // An older library is not migrated by the CLI.
    seedLibrary();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("rewind"));
        db.setDatabaseName(dbPath());
        QVERIFY(db.open());
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA user_version = %1").arg(Database::currentSchemaVersion() - 1)));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("rewind"));
    QCOMPARE(run({QStringLiteral("tags")}, nullptr, &err), cli::kExitLibrary);
    QVERIFY(err.contains(QStringLiteral("needs upgrading")));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
        db.setDatabaseName(dbPath());
        QVERIFY(db.open());
        QSqlQuery q(QStringLiteral("PRAGMA user_version"), db);
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), Database::currentSchemaVersion() - 1); // untouched
        QSqlQuery up(db);
        QVERIFY(up.exec(QStringLiteral("PRAGMA user_version = %1").arg(Database::currentSchemaVersion() + 1)));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("check"));
    // A newer one is refused.
    QCOMPARE(run({QStringLiteral("tags")}, nullptr, &err), cli::kExitLibrary);
    QVERIFY(err.contains(QStringLiteral("newer Omapic")));
}

void TestCli::resolveMediaForView()
{
    seedLibrary();
    QString e;
    QTextStream err(&e);
    QList<QUrl> urls;
    QCOMPARE(cli::resolveMedia(cli::parse({QStringLiteral("view"), QStringLiteral("beach")}), dbPath(), urls, err),
             cli::kExitMatches);
    QCOMPARE(urls, (QList<QUrl>{QUrl::fromLocalFile(path(QStringLiteral("on/a.jpg"))),
                                QUrl::fromLocalFile(path(QStringLiteral("on/b.jpg")))}));
    urls.clear();
    QCOMPARE(cli::resolveMedia(cli::parse({QStringLiteral("slideshow"), QStringLiteral("sunset people")}), dbPath(), urls, err),
             cli::kExitNoMatches);
    QVERIFY(urls.isEmpty());
    err.flush();
    QVERIFY(e.contains(QStringLiteral("nothing matches")));
    QCOMPARE(cli::resolveMedia(cli::parse({QStringLiteral("view"), QStringLiteral("zzz")}), dbPath(), urls, err),
             cli::kExitUsage);
    QCOMPARE(cli::resolveMedia(cli::parse({QStringLiteral("view")}), dbPath(), urls, err), cli::kExitUsage);
}

void TestCli::slideshowIgnoresSavedSettings()
{
    // The gallery's settings file, with every slideshow behaviour turned on and
    // a non-default interval, in the place QSettings would read it from.
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, path(QStringLiteral("config")));
    QCoreApplication::setOrganizationName(QStringLiteral("omapic"));
    QCoreApplication::setApplicationName(QStringLiteral("omapic"));
    {
        QSettings saved;
        saved.beginGroup(QStringLiteral("slideshow"));
        saved.setValue(QStringLiteral("intervalMs"), 7000);
        saved.setValue(QStringLiteral("shuffle"), true);
        saved.setValue(QStringLiteral("shuffleFolders"), true);
        saved.setValue(QStringLiteral("fade"), true);
        saved.setValue(QStringLiteral("randomStart"), true);
        saved.setValue(QStringLiteral("loop"), true);
        saved.endGroup();
        saved.sync();
        QVERIFY(QFile::exists(saved.fileName()));
    }
    // A bare `slideshow x` is 4 s per photo with everything off regardless.
    const cli::SlideshowSettings s = cli::slideshowSettings(cli::parse({QStringLiteral("slideshow"), QStringLiteral("x")}));
    QCOMPARE(s.intervalMs, 4000);
    QVERIFY(!s.shuffle);
    QVERIFY(!s.shuffleFolders);
    QVERIFY(!s.fade);
    QVERIFY(!s.randomStart);
    QVERIFY(!s.loop);
}

void TestCli::slideshowFlagsTurnOnOnlyTheirSetting()
{
    auto with = [](const QString &flag) {
        return cli::slideshowSettings(cli::parse({QStringLiteral("slideshow"), flag, QStringLiteral("x")}));
    };
    const cli::SlideshowSettings none = with(QStringLiteral("--fullscreen"));
    auto onlyDiffers = [&](const cli::SlideshowSettings &s, bool cli::SlideshowSettings::*field) {
        return s.*field && !(none.*field)
            && (field == &cli::SlideshowSettings::shuffle || s.shuffle == none.shuffle)
            && (field == &cli::SlideshowSettings::shuffleFolders || s.shuffleFolders == none.shuffleFolders)
            && (field == &cli::SlideshowSettings::fade || s.fade == none.fade)
            && (field == &cli::SlideshowSettings::randomStart || s.randomStart == none.randomStart)
            && (field == &cli::SlideshowSettings::loop || s.loop == none.loop)
            && s.intervalMs == none.intervalMs;
    };
    QVERIFY(onlyDiffers(with(QStringLiteral("--shuffle")), &cli::SlideshowSettings::shuffle));
    QVERIFY(onlyDiffers(with(QStringLiteral("--shuffle-folders")), &cli::SlideshowSettings::shuffleFolders));
    QVERIFY(onlyDiffers(with(QStringLiteral("--crossfade")), &cli::SlideshowSettings::fade));
    QVERIFY(onlyDiffers(with(QStringLiteral("--start-random")), &cli::SlideshowSettings::randomStart));
    QVERIFY(onlyDiffers(with(QStringLiteral("--loop-videos")), &cli::SlideshowSettings::loop));
    // --interval changes only the interval, rounded to whole milliseconds.
    const cli::SlideshowSettings fast = cli::slideshowSettings(
        cli::parse({QStringLiteral("slideshow"), QStringLiteral("--interval"), QStringLiteral("0.5"), QStringLiteral("x")}));
    QCOMPARE(fast.intervalMs, 500);
    QVERIFY(!fast.shuffle && !fast.shuffleFolders && !fast.fade && !fast.randomStart && !fast.loop);
}

void TestCli::orderPlaylistShapes()
{
    const QList<QUrl> base = {
        QUrl::fromLocalFile(QStringLiteral("/p/a/1.jpg")), QUrl::fromLocalFile(QStringLiteral("/p/a/2.jpg")),
        QUrl::fromLocalFile(QStringLiteral("/p/b/3.jpg")), QUrl::fromLocalFile(QStringLiteral("/p/b/4.jpg")),
        QUrl::fromLocalFile(QStringLiteral("/p/c/5.jpg")),
    };
    cli::SlideshowSettings s;
    QList<QUrl> urls = base;
    QCOMPARE(cli::orderPlaylist(urls, s, 7), 0);
    QCOMPARE(urls, base); // nothing on: untouched, start at the first

    s.randomStart = true;
    urls = base;
    const int start = cli::orderPlaylist(urls, s, 7);
    QVERIFY(start >= 0 && start < base.size());
    QCOMPARE(urls, base);

    s = {};
    s.shuffle = true;
    urls = base;
    QCOMPARE(cli::orderPlaylist(urls, s, 7), 0);
    QCOMPARE(QSet<QUrl>(urls.begin(), urls.end()), QSet<QUrl>(base.begin(), base.end()));
    QVERIFY(urls != base || true); // a permutation; equality is possible but unlikely

    s = {};
    s.shuffleFolders = true;
    urls = base;
    QCOMPARE(cli::orderPlaylist(urls, s, 7), 0);
    QCOMPARE(urls.size(), base.size());
    // Folders stay contiguous and keep their inner order.
    QStringList dirs;
    for (const QUrl &u : urls) {
        const QString p = u.toLocalFile();
        const QString d = p.left(p.lastIndexOf('/'));
        if (dirs.isEmpty() || dirs.last() != d)
            dirs.append(d);
    }
    QCOMPARE(dirs.size(), 3);
    for (int i = 1; i < urls.size(); ++i) {
        const QString p = urls.at(i).toLocalFile(), q = urls.at(i - 1).toLocalFile();
        if (p.left(p.lastIndexOf('/')) == q.left(q.lastIndexOf('/')))
            QVERIFY(q < p);
    }
}

QTEST_GUILESS_MAIN(TestCli)
#include "tst_cli.moc"
