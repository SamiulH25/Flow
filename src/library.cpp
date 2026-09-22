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
    const Row &row = m_rows.at(index.row());
    if (role == Slot)
        return row.slot;
    return TrackRoles::value(m_library->tracks().at(row.track), role);
}

QHash<int, QByteArray> TrackListModel::roleNames() const
{
    QHash<int, QByteArray> roles = TrackRoles::names();
    roles.insert(Slot, "slot");
    return roles;
}

void TrackListModel::setRows(QList<Row> rows)
{
    beginResetModel();
    m_rows = std::move(rows);
    endResetModel();
}

void TrackListModel::trackChanged(int trackIndex)
{
    // A mix may list the same track more than once.
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).track == trackIndex)
            emit dataChanged(index(i), index(i));
    }
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

// ---------------------------------------------------------------- MixListModel

MixListModel::MixListModel(Library *library)
    : QAbstractListModel(library)
{
}

int MixListModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_mixes.size());
}

QVariant MixListModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const LibraryDb::Playlist &mix = m_mixes.at(index.row());
    switch (role) {
    case MixId: return mix.id;
    case Name: return mix.name;
    case TrackCount: return int(mix.trackIds.size());
    }
    return {};
}

QHash<int, QByteArray> MixListModel::roleNames() const
{
    return {{MixId, "mixId"}, {Name, "name"}, {TrackCount, "count"}};
}

void MixListModel::reset(QList<LibraryDb::Playlist> mixes)
{
    beginResetModel();
    m_mixes = std::move(mixes);
    endResetModel();
}

void MixListModel::changed(int row)
{
    emit dataChanged(index(row), index(row));
}

int MixListModel::rowOf(qint64 id) const
{
    for (int i = 0; i < m_mixes.size(); ++i) {
        if (m_mixes.at(i).id == id)
            return i;
    }
    return -1;
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
    , m_mixModel(this)
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
    rebuildAlbums();
    reloadMixes();   // pruned tracks have left their mixes (FK cascade)
    // Fall back to everything when the viewed album or mix is gone.
    if ((m_viewKind == u"album" && !viewedAlbum())
        || (m_viewKind == u"mix" && m_mixModel.rowOf(m_viewMixId) < 0))
        setView(QStringLiteral("all"), {}, -1);
    applyFilter();
    updateStorage();
    emit reloaded();
    emit viewChanged();
}

void Library::applyFilter()
{
    QList<TrackListModel::Row> source;
    if (m_viewKind == u"album") {
        if (const AlbumListModel::Album *album = viewedAlbum()) {
            for (int i : album->tracks)
                source.append(TrackListModel::Row{i});
        }
    } else if (m_viewKind == u"mix") {
        const int row = m_mixModel.rowOf(m_viewMixId);
        const QList<qint64> ids = row >= 0 ? m_mixModel.mixes().at(row).trackIds : QList<qint64>();
        for (int slot = 0; slot < ids.size(); ++slot) {
            const auto it = m_indexById.constFind(ids.at(slot));
            if (it != m_indexById.cend())
                source.append(TrackListModel::Row{*it, slot});
        }
    } else {
        source.reserve(m_tracks.size());
        for (int i = 0; i < m_tracks.size(); ++i)
            source.append(TrackListModel::Row{i});
    }

    const QStringList words = m_query.split(u' ', Qt::SkipEmptyParts);
    QList<TrackListModel::Row> rows;
    rows.reserve(source.size());
    for (const TrackListModel::Row &r : std::as_const(source)) {
        const Track &t = m_tracks.at(r.track);
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
            rows.append(r);
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
    // Albums and mixes have their own natural order for "def".
    if (m_sortMode == u"def" && m_viewKind != u"all") {
        m_trackModel.setRows(std::move(rows));
        emit countsChanged();
        return;
    }
    const auto &all = m_tracks;
    std::stable_sort(rows.begin(), rows.end(), [&](const TrackListModel::Row &ra,
                                                   const TrackListModel::Row &rb) {
        const int ia = ra.track;
        const int ib = rb.track;
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
        albums[*it].key = key;
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
    for (const TrackListModel::Row &r : m_trackModel.rows())
        ids.append(m_tracks.at(r.track).id);
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

// ---------------------------------------------------------------- views

const AlbumListModel::Album *Library::viewedAlbum() const
{
    for (const auto &album : m_albumModel.albums()) {
        if (album.key == m_viewAlbumKey)
            return &album;
    }
    return nullptr;
}

void Library::setView(const QString &kind, const QString &albumKey, qint64 mixId)
{
    m_viewKind = kind;
    m_viewAlbumKey = albumKey;
    m_viewMixId = mixId;
}

QString Library::viewTitle() const
{
    if (m_viewKind == u"album") {
        if (const auto *album = viewedAlbum())
            return m_tracks.at(album->tracks.first()).album;
    } else if (m_viewKind == u"mix") {
        const int row = m_mixModel.rowOf(m_viewMixId);
        if (row >= 0)
            return m_mixModel.mixes().at(row).name;
    }
    return QStringLiteral("Collection");
}

QString Library::viewSubtitle() const
{
    if (m_viewKind == u"album") {
        if (const auto *album = viewedAlbum()) {
            const Track &t = m_tracks.at(album->tracks.first());
            const QString artist = t.albumArtist.isEmpty() ? t.artist : t.albumArtist;
            return t.year > 0 ? artist + QStringLiteral(" · ") + QString::number(t.year) : artist;
        }
    } else if (m_viewKind == u"mix") {
        return QStringLiteral("Mix");
    }
    return {};
}

void Library::showAll()
{
    setView(QStringLiteral("all"), {}, -1);
    applyFilter();
    emit viewChanged();
}

void Library::showAlbum(int albumRow)
{
    const auto &albums = m_albumModel.albums();
    if (albumRow < 0 || albumRow >= albums.size())
        return;
    setView(QStringLiteral("album"), albums.at(albumRow).key, -1);
    applyFilter();
    emit viewChanged();
}

void Library::showMix(qint64 mixId)
{
    if (m_mixModel.rowOf(mixId) < 0)
        return;
    setView(QStringLiteral("mix"), {}, mixId);
    applyFilter();
    emit viewChanged();
}

// ---------------------------------------------------------------- mixes

void Library::reloadMixes()
{
    m_mixModel.reset(m_db.isOpen() ? m_db.playlists() : QList<LibraryDb::Playlist>());
}

bool Library::saveMix(int row)
{
    const LibraryDb::Playlist &mix = m_mixModel.mixes().at(row);
    if (!m_db.setPlaylistTracks(mix.id, mix.trackIds)) {
        setError(QStringLiteral("Could not save mix: ") + m_db.lastError());
        reloadMixes();   // back to what the database has
        applyFilter();
        return false;
    }
    m_mixModel.changed(row);
    if (m_viewKind == u"mix" && m_viewMixId == mix.id)
        applyFilter();
    return true;
}

qint64 Library::createMix(const QString &name)
{
    const QString clean = name.trimmed().isEmpty() ? QStringLiteral("New mix") : name.trimmed();
    const qint64 id = m_db.createPlaylist(clean);
    if (id < 0) {
        setError(QStringLiteral("Could not create mix: ") + m_db.lastError());
        return -1;
    }
    reloadMixes();
    return id;
}

void Library::renameMix(qint64 mixId, const QString &name)
{
    const int row = m_mixModel.rowOf(mixId);
    const QString clean = name.trimmed();
    if (row < 0 || clean.isEmpty())
        return;
    if (!m_db.renamePlaylist(mixId, clean)) {
        setError(QStringLiteral("Could not rename mix: ") + m_db.lastError());
        return;
    }
    m_mixModel.mixes()[row].name = clean;
    m_mixModel.changed(row);
    if (m_viewKind == u"mix" && m_viewMixId == mixId)
        emit viewChanged();
}

void Library::deleteMix(qint64 mixId)
{
    if (m_mixModel.rowOf(mixId) < 0)
        return;
    if (!m_db.deletePlaylist(mixId)) {
        setError(QStringLiteral("Could not delete mix: ") + m_db.lastError());
        return;
    }
    reloadMixes();
    if (m_viewKind == u"mix" && m_viewMixId == mixId)
        showAll();
}

void Library::addToMix(qint64 mixId, qint64 trackId)
{
    const int row = m_mixModel.rowOf(mixId);
    if (row < 0 || !m_indexById.contains(trackId))
        return;
    m_mixModel.mixes()[row].trackIds.append(trackId);
    saveMix(row);
}

void Library::removeFromMix(qint64 mixId, int slot)
{
    const int row = m_mixModel.rowOf(mixId);
    if (row < 0)
        return;
    QList<qint64> &ids = m_mixModel.mixes()[row].trackIds;
    if (slot < 0 || slot >= ids.size())
        return;
    ids.removeAt(slot);
    saveMix(row);
}

void Library::moveInMix(qint64 mixId, int fromSlot, int toSlot)
{
    const int row = m_mixModel.rowOf(mixId);
    if (row < 0)
        return;
    QList<qint64> &ids = m_mixModel.mixes()[row].trackIds;
    if (fromSlot < 0 || fromSlot >= ids.size() || toSlot < 0 || toSlot >= ids.size()
        || fromSlot == toSlot)
        return;
    ids.move(fromSlot, toSlot);
    saveMix(row);
}

QVariantList Library::mixTrackIds(qint64 mixId) const
{
    QVariantList out;
    const int row = m_mixModel.rowOf(mixId);
    if (row < 0)
        return out;
    for (qint64 id : m_mixModel.mixes().at(row).trackIds) {
        if (m_indexById.contains(id))
            out.append(id);
    }
    return out;
}
