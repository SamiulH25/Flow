// Library views (all / album / mix) and mix editing on a real scanned folder.
#include "library.h"
#include "librarydb.h"

#include <QFile>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <taglib/fileref.h>
#include <taglib/tag.h>

namespace {

bool writeTaggedWav(const QString &path, const char *title, const char *album, unsigned track)
{
    const quint32 rate = 8000, blockAlign = 2, dataSize = rate * blockAlign;
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataSize);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(rate);
    u32(rate * blockAlign);
    u16(quint16(blockAlign));
    u16(16);
    h.append("data");
    u32(dataSize);
    {
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return false;
        f.write(h);
        f.write(QByteArray(dataSize, '\0'));
    }
    TagLib::FileRef f(QFile::encodeName(path).constData());
    if (f.isNull() || !f.tag())
        return false;
    f.tag()->setTitle(title);
    f.tag()->setArtist("Artist");
    f.tag()->setAlbum(album);
    f.tag()->setTrack(track);
    return f.save();
}

QStringList titles(Library &lib)
{
    QStringList out;
    for (const QVariant &id : lib.visibleTrackIds())
        out.append(lib.trackById(id.toLongLong())->title);
    return out;
}

qint64 idOf(Library &lib, const QString &title)
{
    for (const Track &t : lib.tracks()) {
        if (t.title == title)
            return t.id;
    }
    return -1;
}

int albumRow(Library &lib, const QString &album)
{
    QAbstractItemModel *m = lib.albumModel();
    const int role = m->roleNames().key("al");
    for (int r = 0; r < m->rowCount(); ++r) {
        if (m->index(r, 0).data(role).toString() == album)
            return r;
    }
    return -1;
}

} // namespace

class TestLibrary : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    QString m_music;
    QString m_db;
    std::unique_ptr<Library> m_lib;

    void openLibrary()
    {
        m_lib = std::make_unique<Library>(m_music, m_db, m_tmp.filePath(QStringLiteral("covers")));
        QTRY_VERIFY_WITH_TIMEOUT(!m_lib->scanning(), 10000);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(m_tmp.isValid());
        m_music = m_tmp.filePath(QStringLiteral("music"));
        m_db = m_tmp.filePath(QStringLiteral("library.db"));
        QVERIFY(QDir().mkpath(m_music));
        QVERIFY(writeTaggedWav(m_music + QStringLiteral("/a1.wav"), "One", "Alpha", 1));
        QVERIFY(writeTaggedWav(m_music + QStringLiteral("/a2.wav"), "Two", "Alpha", 2));
        QVERIFY(writeTaggedWav(m_music + QStringLiteral("/b1.wav"), "Three", "Beta", 1));
        openLibrary();
        QCOMPARE(m_lib->totalCount(), 3);
    }

    void albumView()
    {
        Library &lib = *m_lib;
        QCOMPARE(lib.viewKind(), QStringLiteral("all"));
        QCOMPARE(lib.viewTitle(), QStringLiteral("Collection"));
        lib.showAlbum(albumRow(lib, QStringLiteral("Alpha")));
        QCOMPARE(lib.viewKind(), QStringLiteral("album"));
        QCOMPARE(lib.viewTitle(), QStringLiteral("Alpha"));
        QCOMPARE(lib.viewSubtitle(), QStringLiteral("Artist"));
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("One"), QStringLiteral("Two")}));
        lib.setQuery(QStringLiteral("two"));   // filters apply inside the view
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("Two")}));
        lib.setQuery({});
        lib.showAll();
        QCOMPARE(lib.shownCount(), 3);
    }

    void mixEditing()
    {
        Library &lib = *m_lib;
        const qint64 mix = lib.createMix(QStringLiteral("  Late Night  "));
        QVERIFY(mix > 0);
        QCOMPARE(lib.mixModel()->rowCount(), 1);
        lib.addToMix(mix, idOf(lib, QStringLiteral("Three")));
        lib.addToMix(mix, idOf(lib, QStringLiteral("One")));
        lib.addToMix(mix, idOf(lib, QStringLiteral("Three")));   // duplicates allowed
        lib.addToMix(mix, 999999);                               // unknown track ignored

        lib.showMix(mix);
        QCOMPARE(lib.viewKind(), QStringLiteral("mix"));
        QCOMPARE(lib.viewTitle(), QStringLiteral("Late Night"));
        QCOMPARE(lib.viewMixId(), mix);
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("Three"), QStringLiteral("One"),
                                           QStringLiteral("Three")}));
        const int slotRole = lib.trackModel()->roleNames().key("slot");
        QCOMPARE(lib.trackModel()->index(2, 0).data(slotRole).toInt(), 2);

        lib.moveInMix(mix, 2, 0);
        lib.removeFromMix(mix, 1);   // the "Three" that was first
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("Three"), QStringLiteral("One")}));
        lib.renameMix(mix, QStringLiteral("Deep Focus"));
        QCOMPARE(lib.viewTitle(), QStringLiteral("Deep Focus"));
        QCOMPARE(lib.mixTrackIds(mix).size(), 2);

        lib.setSortMode(QStringLiteral("title"));   // other sorts still work in a mix
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("One"), QStringLiteral("Three")}));
        lib.setSortMode(QStringLiteral("def"));
    }

    void mixesPersist()
    {
        const qint64 mixId = m_lib->viewMixId();
        m_lib.reset();
        openLibrary();
        Library &lib = *m_lib;
        QCOMPARE(lib.mixModel()->rowCount(), 1);
        lib.showMix(mixId);
        QCOMPARE(lib.viewTitle(), QStringLiteral("Deep Focus"));
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("Three"), QStringLiteral("One")}));
    }

    void deletedTracksLeaveMixes()
    {
        Library &lib = *m_lib;
        const qint64 mixId = lib.viewMixId();
        QVERIFY(QFile::remove(m_music + QStringLiteral("/b1.wav")));
        lib.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!lib.scanning(), 10000);
        QCOMPARE(lib.totalCount(), 2);
        QCOMPARE(lib.viewKind(), QStringLiteral("mix"));
        QCOMPARE(titles(lib), (QStringList{QStringLiteral("One")}));
        QCOMPARE(lib.mixTrackIds(mixId).size(), 1);
    }

    void viewFallsBackWhenGone()
    {
        Library &lib = *m_lib;
        lib.deleteMix(lib.viewMixId());
        QCOMPARE(lib.mixModel()->rowCount(), 0);
        QCOMPARE(lib.viewKind(), QStringLiteral("all"));

        lib.showAlbum(albumRow(lib, QStringLiteral("Alpha")));
        QVERIFY(QFile::remove(m_music + QStringLiteral("/a1.wav")));
        QVERIFY(QFile::remove(m_music + QStringLiteral("/a2.wav")));
        lib.rescan();
        QTRY_VERIFY_WITH_TIMEOUT(!lib.scanning(), 10000);
        QCOMPARE(lib.viewKind(), QStringLiteral("all"));
        QCOMPARE(lib.totalCount(), 0);
    }

    void migratesVersion1Database()
    {
        // A database written by the first release (schema 1, no playlists).
        const QString path = m_tmp.filePath(QStringLiteral("v1.db"));
        {
            QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("v1"));
            d.setDatabaseName(path);
            QVERIFY(d.open());
            QSqlQuery q(d);
            QVERIFY(q.exec(QStringLiteral("CREATE TABLE tracks (id INTEGER PRIMARY KEY, path TEXT NOT NULL UNIQUE,"
                                          " mtime INTEGER NOT NULL DEFAULT 0, size INTEGER NOT NULL DEFAULT 0,"
                                          " title TEXT NOT NULL DEFAULT '', artist TEXT NOT NULL DEFAULT '',"
                                          " album TEXT NOT NULL DEFAULT '', album_artist TEXT NOT NULL DEFAULT '',"
                                          " track_no INTEGER NOT NULL DEFAULT 0, disc_no INTEGER NOT NULL DEFAULT 0,"
                                          " year INTEGER NOT NULL DEFAULT 0, duration_ms INTEGER NOT NULL DEFAULT 0,"
                                          " format TEXT NOT NULL DEFAULT '', sample_rate INTEGER NOT NULL DEFAULT 0,"
                                          " bits INTEGER NOT NULL DEFAULT 0, bitrate INTEGER NOT NULL DEFAULT 0,"
                                          " channels INTEGER NOT NULL DEFAULT 0, cover TEXT NOT NULL DEFAULT '',"
                                          " liked INTEGER NOT NULL DEFAULT 0, added_at INTEGER NOT NULL DEFAULT 0)")));
            QVERIFY(q.exec(QStringLiteral("INSERT INTO tracks (path, title, liked) VALUES ('/x.flac', 'Kept', 1)")));
            QVERIFY(q.exec(QStringLiteral("PRAGMA user_version=1")));
            d.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("v1"));

        LibraryDb db;
        QVERIFY2(db.open(path), qPrintable(db.lastError()));
        const QList<Track> tracks = db.loadAll();
        QCOMPARE(tracks.size(), 1);
        QVERIFY(tracks.first().liked);
        const qint64 mix = db.createPlaylist(QStringLiteral("m"));
        QVERIFY(mix > 0);
        QVERIFY(db.setPlaylistTracks(mix, {tracks.first().id}));
        QCOMPARE(db.playlists().first().trackIds, QList<qint64>{tracks.first().id});
        QVERIFY(db.remove(QStringLiteral("/x.flac")));
        QVERIFY(db.playlists().first().trackIds.isEmpty());   // cascade
    }
};

QTEST_GUILESS_MAIN(TestLibrary)
#include "tst_library.moc"
