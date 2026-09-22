#include "librarydb.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>

namespace {
constexpr int kSchemaVersion = 1;
QAtomicInt s_connectionCounter;

// A null QString binds as SQL NULL; the text columns are NOT NULL.
QString text(const QString &s)
{
    return s.isNull() ? QStringLiteral("") : s;
}
} // namespace

LibraryDb::LibraryDb()
    : m_name(QStringLiteral("flow-library-%1").arg(s_connectionCounter.fetchAndAddRelaxed(1)))
{
}

LibraryDb::~LibraryDb()
{
    if (!QSqlDatabase::contains(m_name))
        return;
    {
        QSqlDatabase d = QSqlDatabase::database(m_name, false);
        d.close();
    } // every QSqlDatabase handle must be gone before removeDatabase()
    QSqlDatabase::removeDatabase(m_name);
}

QSqlDatabase LibraryDb::db() const
{
    return QSqlDatabase::database(m_name, false);
}

bool LibraryDb::isOpen() const
{
    return QSqlDatabase::contains(m_name) && db().isOpen();
}

bool LibraryDb::open(const QString &path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSqlDatabase d = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
    d.setDatabaseName(path);
    d.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=5000"));
    if (!d.open()) {
        m_error = d.lastError().text();
        return false;
    }
    // WAL lets the GUI read (and toggle likes) while the scanner writes.
    return exec(QStringLiteral("PRAGMA journal_mode=WAL"))
        && exec(QStringLiteral("PRAGMA synchronous=NORMAL"))
        && migrate();
}

bool LibraryDb::exec(const QString &sql)
{
    QSqlQuery q(db());
    if (!q.exec(sql)) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDb::migrate()
{
    QSqlQuery q(db());
    if (!q.exec(QStringLiteral("PRAGMA user_version")) || !q.next()) {
        m_error = q.lastError().text();
        return false;
    }
    const int version = q.value(0).toInt();
    if (version == kSchemaVersion)
        return true;
    if (version > kSchemaVersion) {
        m_error = QStringLiteral("library database is from a newer Flow (schema %1)").arg(version);
        return false;
    }
    if (!transaction())
        return false;
    const bool ok = exec(QStringLiteral(
               "CREATE TABLE IF NOT EXISTS tracks ("
               " id INTEGER PRIMARY KEY,"
               " path TEXT NOT NULL UNIQUE,"
               " mtime INTEGER NOT NULL DEFAULT 0,"
               " size INTEGER NOT NULL DEFAULT 0,"
               " title TEXT NOT NULL DEFAULT '',"
               " artist TEXT NOT NULL DEFAULT '',"
               " album TEXT NOT NULL DEFAULT '',"
               " album_artist TEXT NOT NULL DEFAULT '',"
               " track_no INTEGER NOT NULL DEFAULT 0,"
               " disc_no INTEGER NOT NULL DEFAULT 0,"
               " year INTEGER NOT NULL DEFAULT 0,"
               " duration_ms INTEGER NOT NULL DEFAULT 0,"
               " format TEXT NOT NULL DEFAULT '',"
               " sample_rate INTEGER NOT NULL DEFAULT 0,"
               " bits INTEGER NOT NULL DEFAULT 0,"
               " bitrate INTEGER NOT NULL DEFAULT 0,"
               " channels INTEGER NOT NULL DEFAULT 0,"
               " cover TEXT NOT NULL DEFAULT '',"
               " liked INTEGER NOT NULL DEFAULT 0,"
               " added_at INTEGER NOT NULL DEFAULT 0)"))
        && exec(QStringLiteral("PRAGMA user_version=%1").arg(kSchemaVersion))
        && commit();
    if (!ok)
        db().rollback();
    return ok;
}

QList<Track> LibraryDb::loadAll()
{
    QList<Track> out;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral(
            "SELECT id, path, mtime, size, title, artist, album, album_artist, track_no,"
            " disc_no, year, duration_ms, format, sample_rate, bits, bitrate, channels,"
            " cover, liked, added_at FROM tracks"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next()) {
        Track t;
        t.id = q.value(0).toLongLong();
        t.path = q.value(1).toString();
        t.mtime = q.value(2).toLongLong();
        t.size = q.value(3).toLongLong();
        t.title = q.value(4).toString();
        t.artist = q.value(5).toString();
        t.album = q.value(6).toString();
        t.albumArtist = q.value(7).toString();
        t.trackNo = q.value(8).toInt();
        t.discNo = q.value(9).toInt();
        t.year = q.value(10).toInt();
        t.durationMs = q.value(11).toLongLong();
        t.format = q.value(12).toString();
        t.sampleRate = q.value(13).toInt();
        t.bitsPerSample = q.value(14).toInt();
        t.bitrate = q.value(15).toInt();
        t.channels = q.value(16).toInt();
        t.coverPath = q.value(17).toString();
        t.liked = q.value(18).toBool();
        t.addedAt = q.value(19).toLongLong();
        out.append(t);
    }
    return out;
}

QHash<QString, LibraryDb::Stamp> LibraryDb::stamps()
{
    QHash<QString, Stamp> out;
    QSqlQuery q(db());
    q.setForwardOnly(true);
    if (!q.exec(QStringLiteral("SELECT path, mtime, size FROM tracks"))) {
        m_error = q.lastError().text();
        return out;
    }
    while (q.next())
        out.insert(q.value(0).toString(), {q.value(1).toLongLong(), q.value(2).toLongLong()});
    return out;
}

bool LibraryDb::upsert(const Track &t)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral(
        "INSERT INTO tracks (path, mtime, size, title, artist, album, album_artist, track_no,"
        " disc_no, year, duration_ms, format, sample_rate, bits, bitrate, channels, cover,"
        " added_at)"
        " VALUES (:path, :mtime, :size, :title, :artist, :album, :album_artist, :track_no,"
        " :disc_no, :year, :duration_ms, :format, :sample_rate, :bits, :bitrate, :channels,"
        " :cover, :added_at)"
        " ON CONFLICT(path) DO UPDATE SET mtime=excluded.mtime, size=excluded.size,"
        " title=excluded.title, artist=excluded.artist, album=excluded.album,"
        " album_artist=excluded.album_artist, track_no=excluded.track_no,"
        " disc_no=excluded.disc_no, year=excluded.year, duration_ms=excluded.duration_ms,"
        " format=excluded.format, sample_rate=excluded.sample_rate, bits=excluded.bits,"
        " bitrate=excluded.bitrate, channels=excluded.channels, cover=excluded.cover"));
    q.bindValue(QStringLiteral(":path"), text(t.path));
    q.bindValue(QStringLiteral(":mtime"), t.mtime);
    q.bindValue(QStringLiteral(":size"), t.size);
    q.bindValue(QStringLiteral(":title"), text(t.title));
    q.bindValue(QStringLiteral(":artist"), text(t.artist));
    q.bindValue(QStringLiteral(":album"), text(t.album));
    q.bindValue(QStringLiteral(":album_artist"), text(t.albumArtist));
    q.bindValue(QStringLiteral(":track_no"), t.trackNo);
    q.bindValue(QStringLiteral(":disc_no"), t.discNo);
    q.bindValue(QStringLiteral(":year"), t.year);
    q.bindValue(QStringLiteral(":duration_ms"), t.durationMs);
    q.bindValue(QStringLiteral(":format"), text(t.format));
    q.bindValue(QStringLiteral(":sample_rate"), t.sampleRate);
    q.bindValue(QStringLiteral(":bits"), t.bitsPerSample);
    q.bindValue(QStringLiteral(":bitrate"), t.bitrate);
    q.bindValue(QStringLiteral(":channels"), t.channels);
    q.bindValue(QStringLiteral(":cover"), text(t.coverPath));
    q.bindValue(QStringLiteral(":added_at"),
                t.addedAt > 0 ? t.addedAt : QDateTime::currentSecsSinceEpoch());
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDb::remove(const QString &path)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("DELETE FROM tracks WHERE path = ?"));
    q.addBindValue(path);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDb::setLiked(qint64 id, bool liked)
{
    QSqlQuery q(db());
    q.prepare(QStringLiteral("UPDATE tracks SET liked = ? WHERE id = ?"));
    q.addBindValue(liked ? 1 : 0);
    q.addBindValue(id);
    if (!q.exec()) {
        m_error = q.lastError().text();
        return false;
    }
    return true;
}

bool LibraryDb::transaction()
{
    if (!db().transaction()) {
        m_error = db().lastError().text();
        return false;
    }
    return true;
}

bool LibraryDb::commit()
{
    if (!db().commit()) {
        m_error = db().lastError().text();
        return false;
    }
    return true;
}
