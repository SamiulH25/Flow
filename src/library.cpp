#include "library.h"

#include "libraryscanner.h"

#include <QCollator>
#include <QDir>
#include <QJSEngine>
#include <QLocale>
#include <QSet>
#include <QStorageInfo>

#include <algorithm>

// ---------------------------------------------------------------- TrackListModel

TrackListModel::TrackListModel(Library *library)
    : QAbstractListModel(library)
    , m_library(library)
{
}

int TrackListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QVariant TrackListModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    return TrackRoles::value(m_library->tracks().at(m_rows.at(index.row())), role);
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    return TrackRoles::names();
}

void TrackListModel::setRows(QList<int> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

void TrackListModel::trackChanged(int trackIndex)
{
    const qsizetype row = m_rows.indexOf(trackIndex);
    if (row >= 0)
        emit dataChanged(index(int(row)), index(int(row)));
}

// ---------------------------------------------------------------- AlbumListModel

AlbumListModel::AlbumListModel(Library *library)
    : QAbstractListModel(library)
    , m_library(library)
{
}

int AlbumListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_albums.size());
}

QVariant AlbumListModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Album &album = m_albums.at(index.row());
    if (role == TrackCount)
        return int(album.tracks.size());
    if (role == AlbumIndex)
        return index.row();
    // Represent the album by its first track that has art (else the first).
    const QList<Track> &all = m_library->tracks();
    int rep = album.tracks.first();
    for (int i : album.tracks) {
        if (!all.at(i).coverPath.isEmpty()) {
            rep = i;
            break;
        }
    }
    const Track &t = all.at(rep);
    if (role == TrackRoles::Artist && !t.albumArtist.isEmpty())
        return t.albumArtist;
    return TrackRoles::value(t, role);
}

QHash<int, QByteArray> AlbumListModel::roleNames() const
{
    QHash<int, QByteArray> roles = TrackRoles::names();
    roles.insert(TrackCount, "count");
    roles.insert(AlbumIndex, "albumIndex");
    return roles;
}

void AlbumListModel::setAlbums(QList<Album> albums)
{
    beginResetModel();
    m_albums = std::move(albums);
    endResetModel();
}

// ---------------------------------------------------------------- Library

Library::Library(const QString &musicDir, const QString &dbPath, const QString &coverDir,
                 QObject *parent)
    : QObject(parent)
    , m_musicDir(QDir::cleanPath(QDir(musicDir).absolutePath()))
    , m_dbPath(dbPath)
    , m_coverDir(coverDir)
    , m_trackModel(this)
    , m_albumModel(this)
{
    m_rescanDebounce.setSingleShot(true);
    m_rescanDebounce.setInterval(2000);   // coalesce bursts (copying an album in)
    connect(&m_rescanDebounce, &QTimer::timeout, this, &Library::rescan);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this,
            [this] { m_rescanDebounce.start(); });

    if (!m_db.open(m_dbPath))
        setError(QStringLiteral("Could not open library database: ") + m_db.lastError());
    reload();    // show the cached index immediately
    rescan();    // then catch up with the disk in the background
}

Library::~Library()
{
    if (m_scanThread) {
        m_cancelScan = true;
        m_scanThread->wait();
        delete m_scanThread;
    }
}

Library *Library::create(QQmlEngine *, QJSEngine *jsEngine)
{
    Q_ASSERT(s_instance);
    Q_ASSERT(jsEngine->thread() == s_instance->thread());
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

const Track *Library::trackById(qint64 id) const
{
    const auto it = m_indexById.constFind(id);
    return it == m_indexById.cend() ? nullptr : &m_tracks.at(*it);
}

const Track *Library::trackByPath(const QString &path) const
{
    for (const Track &t : m_tracks) {
        if (t.path == path)
            return &t;
    }
    return nullptr;
}

void Library::setError(const QString &error)
{
    if (m_error == error)
        return;
    m_error = error;
    if (!error.isEmpty())
        qWarning().noquote() << "flow:" << error;
    emit errorChanged();
}

void Library::setQuery(const QString &query)
{
    if (m_query == query)
        return;
    m_query = query;
    emit queryChanged();
    applyFilter();
}

void Library::setFormat(const QString &format)
{
    if (m_format == format)
        return;
    m_format = format;
    emit formatChanged();
    applyFilter();
}

void Library::setSortMode(const QString &mode)
{
    if (m_sortMode == mode)
        return;
    m_sortMode = mode;
    emit sortModeChanged();
    applyFilter();
}

void Library::reload()
{
    const QString prefix = m_musicDir + u'/';
    m_tracks.clear();
    m_indexById.clear();
    m_likedCount = 0;
    if (m_db.isOpen()) {
        // The index may still hold tracks of a previously used music folder;
        // they are kept (with their likes) but not shown.
        const QList<Track> all = m_db.loadAll();
        for (const Track &t : all) {
            if (!t.path.startsWith(prefix))
                continue;
            m_indexById.insert(t.id, int(m_tracks.size()));
            m_tracks.append(t);
            if (t.liked)
                ++m_likedCount;
        }
    }
    // "Recently added" orders by album, so an album scanned across a second
    // boundary is not split: every track takes its album's newest timestamp.
    QHash<QString, qint64> albumAdded;
    for (const Track &t : std::as_const(m_tracks)) {
        qint64 &latest = albumAdded[TrackFormat::albumKey(t)];
        latest = qMax(latest, t.addedAt);
    }
    m_albumAddedAt.clear();
    m_albumAddedAt.reserve(m_tracks.size());
    for (const Track &t : std::as_const(m_tracks))
        m_albumAddedAt.append(albumAdded.value(TrackFormat::albumKey(t)));
    applyFilter();
    rebuildAlbums();
    updateStorage();
    emit reloaded();
}

void Library::applyFilter()
{
    const QStringList words = m_query.split(u' ', Qt::SkipEmptyParts);
    QList<int> rows;
    rows.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i) {
        const Track &t = m_tracks.at(i);
        if (m_format == u"Liked") {
            if (!t.liked)
                continue;
        } else if (m_format != u"All" && t.format != m_format) {
            continue;
        }
        bool match = true;
        for (const QString &w : words) {
            if (!t.title.contains(w, Qt::CaseInsensitive)
                && !t.artist.contains(w, Qt::CaseInsensitive)
                && !t.album.contains(w, Qt::CaseInsensitive)) {
                match = false;
                break;
            }
        }
        if (match)
            rows.append(i);
    }

    QCollator coll;
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    coll.setNumericMode(true);
    const auto albumOrder = [&](const Track &a, const Track &b) {
        if (const int c = coll.compare(a.album, b.album))
            return c < 0;
        const QString ka = TrackFormat::albumKey(a);
        const QString kb = TrackFormat::albumKey(b);
        if (ka != kb)
            return ka < kb;   // same title, different artist: keep albums apart
        if (a.discNo != b.discNo)
            return a.discNo < b.discNo;
        if (a.trackNo != b.trackNo)
            return a.trackNo < b.trackNo;
        return coll.compare(a.title, b.title) < 0;
    };
    const auto &all = m_tracks;
    std::stable_sort(rows.begin(), rows.end(), [&](int ia, int ib) {
        const Track &a = all.at(ia);
        const Track &b = all.at(ib);
        if (m_sortMode == u"title") {
            if (const int c = coll.compare(a.title, b.title))
                return c < 0;
            return coll.compare(a.artist, b.artist) < 0;
        }
        if (m_sortMode == u"artist") {
            if (const int c = coll.compare(a.artist, b.artist))
                return c < 0;
            return albumOrder(a, b);
        }
        if (m_sortMode == u"dur")
            return a.durationMs > b.durationMs;
        // "def": recently added albums first, each in track order
        if (m_albumAddedAt.at(ia) != m_albumAddedAt.at(ib))
            return m_albumAddedAt.at(ia) > m_albumAddedAt.at(ib);
        return albumOrder(a, b);
    });

    m_trackModel.setRows(std::move(rows));
    emit countsChanged();
}

void Library::rebuildAlbums()
{
    QHash<QString, int> byKey;
    QList<AlbumListModel::Album> albums;
    for (int i = 0; i < m_tracks.size(); ++i) {
        const QString key = TrackFormat::albumKey(m_tracks.at(i));
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            it = byKey.insert(key, int(albums.size()));
            albums.append(AlbumListModel::Album{});
        }
        albums[*it].tracks.append(i);
    }

    QCollator coll;
    coll.setCaseSensitivity(Qt::CaseInsensitive);
    coll.setNumericMode(true);
    const auto &all = m_tracks;
    for (auto &album : albums) {
        std::stable_sort(album.tracks.begin(), album.tracks.end(), [&](int ia, int ib) {
            const Track &a = all.at(ia);
            const Track &b = all.at(ib);
            if (a.discNo != b.discNo)
                return a.discNo < b.discNo;
            if (a.trackNo != b.trackNo)
                return a.trackNo < b.trackNo;
            return coll.compare(a.title, b.title) < 0;
        });
    }
    const auto addedAt = [&](const AlbumListModel::Album &album) {
        qint64 latest = 0;
        for (int i : album.tracks)
            latest = qMax(latest, all.at(i).addedAt);
        return latest;
    };
    std::stable_sort(albums.begin(), albums.end(), [&](const auto &a, const auto &b) {
        const qint64 aa = addedAt(a);
        const qint64 ab = addedAt(b);
        if (aa != ab)
            return aa > ab;
        return coll.compare(all.at(a.tracks.first()).album, all.at(b.tracks.first()).album) < 0;
    });
    m_albumModel.setAlbums(std::move(albums));
}

void Library::updateStorage()
{
    qint64 libraryBytes = 0;
    for (const Track &t : std::as_const(m_tracks))
        libraryBytes += t.size;

    const QLocale locale;
    const QString library = locale.formattedDataSize(libraryBytes, 1, QLocale::DataSizeSIFormat);
    const QStorageInfo storage(m_musicDir);
    if (storage.isValid() && storage.isReady() && storage.bytesTotal() > 0) {
        const qint64 total = storage.bytesTotal();
        const qint64 free = storage.bytesAvailable();
        m_storageText = library + QStringLiteral(" · ")
                        + locale.formattedDataSize(free, 1, QLocale::DataSizeSIFormat)
                        + QStringLiteral(" free");
        m_storageUsed = qBound(0.0, double(total - free) / double(total), 1.0);
    } else {
        m_storageText = library;
        m_storageUsed = 0;
    }
    emit countsChanged();
}

void Library::rescan()
{
    if (m_scanThread) {
        m_rescanPending = true;   // run again once the current scan is done
        return;
    }
    m_cancelScan = false;
    m_scanProgress = 0;
    emit scanProgressChanged();

    const QString root = m_musicDir;
    const QString dbPath = m_dbPath;
    const QString coverDir = m_coverDir;
    m_scanThread = QThread::create([this, root, dbPath, coverDir] {
        LibraryScanner scanner(dbPath, coverDir);
        const auto result = scanner.scan(root, &m_cancelScan, [this](int n) {
            QMetaObject::invokeMethod(this, [this, n] {
                m_scanProgress = n;
                emit scanProgressChanged();
            }, Qt::QueuedConnection);
        });
        QMetaObject::invokeMethod(this, [this, result] {
            onScanFinished(result.ok, result.error, result.dirs,
                           result.updated > 0 || result.removed > 0);
        }, Qt::QueuedConnection);
    });
    m_scanThread->setObjectName(QStringLiteral("flow-scan"));
    m_scanThread->start(QThread::LowPriority);
    emit scanningChanged();
}

void Library::onScanFinished(bool ok, const QString &error, const QStringList &dirs,
                             bool changed)
{
    m_scanThread->wait();
    delete m_scanThread;
    m_scanThread = nullptr;

    setError(ok ? QString() : error);
    if (ok) {
        // Re-watch exactly the directories that exist now.
        const QStringList watched = m_watcher.directories();
        const QSet<QString> want(dirs.cbegin(), dirs.cend());
        QStringList stale;
        for (const QString &d : watched) {
            if (!want.contains(d))
                stale.append(d);
        }
        if (!stale.isEmpty())
            m_watcher.removePaths(stale);
        const QSet<QString> have(watched.cbegin(), watched.cend());
        QStringList fresh;
        for (const QString &d : dirs) {
            if (!have.contains(d))
                fresh.append(d);
        }
        if (!fresh.isEmpty())
            m_watcher.addPaths(fresh);
    }

    // A no-op rescan must not reset the views (and their scroll positions).
    if (changed)
        reload();
    else
        updateStorage();
    emit scanningChanged();

    if (m_rescanPending) {
        m_rescanPending = false;
        rescan();
    }
}

void Library::toggleLike(qint64 id)
{
    const auto it = m_indexById.constFind(id);
    if (it == m_indexById.cend())
        return;
    Track &t = m_tracks[*it];
    if (!m_db.setLiked(id, !t.liked)) {
        setError(QStringLiteral("Could not save like: ") + m_db.lastError());
        return;
    }
    t.liked = !t.liked;
    m_likedCount += t.liked ? 1 : -1;
    m_trackModel.trackChanged(*it);
    if (m_format == u"Liked")
        applyFilter();
    emit countsChanged();
    emit trackUpdated(id);
}

QVariantList Library::visibleTrackIds() const
{
    QVariantList ids;
    for (int i : m_trackModel.rows())
        ids.append(m_tracks.at(i).id);
    return ids;
}

QVariantList Library::albumTrackIds(int albumRow) const
{
    QVariantList ids;
    const auto &albums = m_albumModel.albums();
    if (albumRow < 0 || albumRow >= albums.size())
        return ids;
    for (int i : albums.at(albumRow).tracks)
        ids.append(m_tracks.at(i).id);
    return ids;
}
