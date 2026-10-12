#include "library.h"
#include "enricher.h"

#include "libraryscanner.h"
#include "m3u.h"

#include <QCollator>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QSaveFile>
#include <QJSEngine>
#include <QLocale>
#include <QSet>
#include <QSettings>
#include <QStorageInfo>

#include <algorithm>
#include <utility>

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

    // One collator for every sort: ICU setup is not free and the settings must
    // not drift between the cached keys and the live compares.
    m_collator.setCaseSensitivity(Qt::CaseInsensitive);
    m_collator.setNumericMode(true);

    QSettings settings;
    m_navidrome.url = settings.value(QStringLiteral("navidrome/url")).toString();
    m_navidrome.user = settings.value(QStringLiteral("navidrome/user")).toString();
    m_navidrome.password = settings.value(QStringLiteral("navidrome/password")).toString();

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
    const auto it = m_indexByPath.constFind(path);
    return it == m_indexByPath.cend() ? nullptr : &m_tracks.at(*it);
}

QByteArray Library::cachedPeaks(qint64 id)
{
    return m_db.isOpen() ? m_db.peaks(id) : QByteArray();
}

void Library::storePeaks(qint64 id, const QByteArray &peaks)
{
    if (m_db.isOpen() && m_indexById.contains(id) && !m_db.setPeaks(id, peaks))
        qWarning().noquote() << "flow: could not cache waveform:" << m_db.lastError();
}

namespace {

// Dominant color of an image as a vivid "#rrggbb"; "" for grey or empty images.
QString accentOf(const QImage &source)
{
    const QImage img = source.scaled(32, 32, Qt::IgnoreAspectRatio, Qt::FastTransformation)
                           .convertToFormat(QImage::Format_RGB32);
    double wr = 0, wg = 0, wb = 0, wsum = 0;   // saturation-weighted sums of kept pixels
    qint64 ar = 0, ag = 0, ab = 0;             // plain sums of every pixel (fallback)
    qint64 an = 0;
    int kept = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb *row = reinterpret_cast<const QRgb *>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x) {
            const int r = qRed(row[x]), g = qGreen(row[x]), b = qBlue(row[x]);
            ar += r; ag += g; ab += b; ++an;
            const int mx = std::max({r, g, b}), mn = std::min({r, g, b});
            if (mx < 40 || mn > 215)            // near-black or near-white
                continue;
            const double sat = double(mx - mn) / mx;
            if (sat < 0.15)                      // too grey to carry a hue
                continue;
            wr += r * sat; wg += g * sat; wb += b * sat; wsum += sat;
            ++kept;
        }
    }
    if (an == 0)
        return {};

    double r, g, b;
    if (kept >= 16) {
        r = wr / wsum; g = wg / wsum; b = wb / wsum;
    } else {
        r = double(ar) / an; g = double(ag) / an; b = double(ab) / an;
    }
    float h, s, v;   // QColor::getHsvF takes float*
    QColor::fromRgbF(r / 255.0, g / 255.0, b / 255.0).getHsvF(&h, &s, &v);
    if (h < 0)                                   // achromatic: no hue to show
        return {};
    return QColor::fromHsvF(h, std::max<qreal>(s, 0.55), std::max<qreal>(v, 0.85))
        .name(QColor::HexRgb);
}

} // namespace

QString Library::accentFor(const QString &cover)
{
    if (cover.isEmpty())
        return {};
    const auto hit = m_accentCache.constFind(cover);
    if (hit != m_accentCache.cend())
        return *hit;

    // Cover role is a file URL (track.cpp); accept plain local paths too.
    const QUrl url(cover);
    const QString path = url.isLocalFile() ? url.toLocalFile() : cover;
    const QString accent = accentOf(QImage(path));
    m_accentCache.insert(cover, accent);
    return accent;
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

void Library::setMusicDir(const QString &dir)
{
    // Accept both paths and file:// URLs (FolderDialog hands out URLs).
    const QUrl url(dir);
    const QString path = url.isLocalFile() ? url.toLocalFile() : dir;
    if (path.trimmed().isEmpty())
        return;
    const QString clean = QDir::cleanPath(QDir(path).absolutePath());
    if (clean == m_musicDir)
        return;
    m_musicDir = clean;
    QSettings().setValue(QStringLiteral("library/dir"), m_musicDir);
    emit musicDirChanged();

    reload();   // cached tracks under the new folder show up right away
    if (m_scanThread) {
        m_cancelScan = true;   // the running scan is for the old folder
        m_rescanPending = true;
    } else {
        rescan();
    }
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

void Library::dropKeys()
{
    m_keys.foldValid = m_keys.albumValid = m_keys.titleValid = m_keys.artistValid = false;
    m_keys.albumFold.clear();
    m_keys.album.clear();
    m_keys.title.clear();
    m_keys.artist.clear();
}

void Library::ensureFoldKeys()
{
    if (m_keys.foldValid && m_keys.albumFold.size() == m_tracks.size())
        return;
    m_keys.albumFold.resize(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_keys.albumFold[i] = TrackFormat::albumKey(m_tracks.at(i));
    m_keys.foldValid = true;
}

void Library::ensureAlbumKeys()
{
    if (m_keys.albumValid && m_keys.album.size() == m_tracks.size())
        return;
    // QCollatorSortKey has no default constructor, so the list is built with
    // append() (constructed keys), never resize(n) / operator[].
    m_keys.album.clear();
    m_keys.album.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_keys.album.append(m_collator.sortKey(m_tracks.at(i).album));
    m_keys.albumValid = true;
}

void Library::ensureTitleKeys()
{
    if (m_keys.titleValid && m_keys.title.size() == m_tracks.size())
        return;
    m_keys.title.clear();
    m_keys.title.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_keys.title.append(m_collator.sortKey(m_tracks.at(i).title));
    m_keys.titleValid = true;
}

void Library::ensureArtistKeys()
{
    if (m_keys.artistValid && m_keys.artist.size() == m_tracks.size())
        return;
    m_keys.artist.clear();
    m_keys.artist.reserve(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_keys.artist.append(m_collator.sortKey(m_tracks.at(i).artist));
    m_keys.artistValid = true;
}

void Library::refreshKeys(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= m_tracks.size())
        return;
    const Track &t = m_tracks.at(trackIndex);
    if (m_keys.foldValid && trackIndex < m_keys.albumFold.size())
        m_keys.albumFold[trackIndex] = TrackFormat::albumKey(t);
    if (m_keys.albumValid && trackIndex < m_keys.album.size())
        m_keys.album[trackIndex] = m_collator.sortKey(t.album);
    if (m_keys.titleValid && trackIndex < m_keys.title.size())
        m_keys.title[trackIndex] = m_collator.sortKey(t.title);
    if (m_keys.artistValid && trackIndex < m_keys.artist.size())
        m_keys.artist[trackIndex] = m_collator.sortKey(t.artist);
}

void Library::reload()
{
    const QString prefix = m_musicDir + u'/';
    m_tracks.clear();
    m_indexById.clear();
    m_indexByPath.clear();
    m_likedCount = 0;
    dropKeys();
    if (m_db.isOpen()) {
        // The index may still hold tracks of a previously used music folder;
        // they are kept (with their likes) but not shown.
        QList<Track> all = m_db.loadAll();
        m_tracks.reserve(all.size());
        // Remote tracks of the configured server show too; everything else stays hidden.
        const QString remotePrefix =
            m_navidrome.isValid() ? Navidrome::pathPrefix(m_navidrome) : QString();
        for (Track &t : all) {
            const bool local = t.path.startsWith(prefix);
            const bool remote = !remotePrefix.isEmpty() && t.path.startsWith(remotePrefix);
            if (!local && !remote)
                continue;
            const int index = int(m_tracks.size());
            m_indexById.insert(t.id, index);
            m_indexByPath.insert(t.path, index);
            if (t.liked)
                ++m_likedCount;
            m_tracks.append(std::move(t));   // no deep copy of a dozen QString fields
        }
    }
    // "Recently added" orders by album, so an album scanned across a second
    // boundary is not split: every track takes its album's newest timestamp.
    // The album key is folded once per track and reused for both the mapping
    // and the per-track expansion (it used to be rebuilt twice per track).
    ensureFoldKeys();
    QHash<QString, qint64> albumAdded;
    albumAdded.reserve(int(m_tracks.size() / 8) + 1);
    for (int i = 0; i < m_tracks.size(); ++i) {
        qint64 &latest = albumAdded[m_keys.albumFold.at(i)];
        latest = qMax(latest, m_tracks.at(i).addedAt);
    }
    m_albumAddedAt.resize(m_tracks.size());
    for (int i = 0; i < m_tracks.size(); ++i)
        m_albumAddedAt[i] = albumAdded.value(m_keys.albumFold.at(i));
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

    // Albums and mixes have their own natural order for "def".
    if (m_sortMode == u"def" && m_viewKind != u"all") {
        m_trackModel.setRows(std::move(rows));
        emit countsChanged();
        return;
    }

    // Comparator keys: folding and collation are far too expensive to redo per
    // comparison (a keystroke re-sorts the whole library), so they are built
    // once here and cached across calls until the tracks change.
    const bool byTitle = m_sortMode == u"title";
    const bool byArtist = m_sortMode == u"artist";
    const bool byAlbum = byTitle || byArtist || m_sortMode == u"def";
    ensureFoldKeys();                       // albumKey, used by albumOrder
    if (byAlbum) {
        ensureAlbumKeys();
        ensureTitleKeys();                  // albumOrder's final tie-break
    }
    if (byTitle)
        ensureArtistKeys();                 // title sort's tie-break

    const QList<QString> &fold = m_keys.albumFold;
    const QList<QCollatorSortKey> &albumKey = m_keys.album;
    const QList<QCollatorSortKey> &titleKey = m_keys.title;
    const QList<QCollatorSortKey> &artistKey = m_keys.artist;

    // Same album title, different artist: keep the albums apart: same disc and
    // track numbers, then the title.
    const auto albumOrder = [&](int ia, int ib) {
        if (albumKey.at(ia) < albumKey.at(ib))
            return true;
        if (albumKey.at(ib) < albumKey.at(ia))
            return false;
        if (fold.at(ia) != fold.at(ib))
            return fold.at(ia) < fold.at(ib);
        const Track &a = m_tracks.at(ia);
        const Track &b = m_tracks.at(ib);
        if (a.discNo != b.discNo)
            return a.discNo < b.discNo;
        if (a.trackNo != b.trackNo)
            return a.trackNo < b.trackNo;
        return titleKey.at(ia) < titleKey.at(ib);
    };
    std::stable_sort(rows.begin(), rows.end(), [&](const TrackListModel::Row &ra,
                                                   const TrackListModel::Row &rb) {
        const int ia = ra.track;
        const int ib = rb.track;
        if (m_sortMode == u"title") {
            if (titleKey.at(ia) < titleKey.at(ib))
                return true;
            if (titleKey.at(ib) < titleKey.at(ia))
                return false;
            return artistKey.at(ia) < artistKey.at(ib);
        }
        if (m_sortMode == u"artist") {
            if (artistKey.at(ia) < artistKey.at(ib))
                return true;
            if (artistKey.at(ib) < artistKey.at(ia))
                return false;
            return albumOrder(ia, ib);
        }
        if (m_sortMode == u"dur")
            return m_tracks.at(ia).durationMs > m_tracks.at(ib).durationMs;
        // "def": recently added albums first, each in track order
        if (m_albumAddedAt.at(ia) != m_albumAddedAt.at(ib))
            return m_albumAddedAt.at(ia) > m_albumAddedAt.at(ib);
        return albumOrder(ia, ib);
    });

    m_trackModel.setRows(std::move(rows));
    emit countsChanged();
}

void Library::rebuildAlbums()
{
    ensureFoldKeys();   // one albumKey per track, shared with the sorting paths
    QHash<QString, int> byKey;
    QList<AlbumListModel::Album> albums;
    QList<qint64> albumsAddedAt;   // newest added_at per album, parallel to albums
    for (int i = 0; i < m_tracks.size(); ++i) {
        const QString &key = m_keys.albumFold.at(i);
        auto it = byKey.find(key);
        if (it == byKey.end()) {
            it = byKey.insert(key, int(albums.size()));
            albums.append(AlbumListModel::Album{key, {}});
            albumsAddedAt.append(0);
        }
        const int row = *it;
        albums[row].tracks.append(i);
        albumsAddedAt[row] = qMax(albumsAddedAt.at(row), m_tracks.at(i).addedAt);
    }

    const auto &all = m_tracks;
    for (auto &album : albums) {
        std::stable_sort(album.tracks.begin(), album.tracks.end(), [&](int ia, int ib) {
            const Track &a = all.at(ia);
            const Track &b = all.at(ib);
            if (a.discNo != b.discNo)
                return a.discNo < b.discNo;
            if (a.trackNo != b.trackNo)
                return a.trackNo < b.trackNo;
            return m_collator.compare(a.title, b.title) < 0;
        });
    }
    // Sort album indices, not the albums themselves: the "added_at" used by the
    // comparator is precomputed above instead of re-scanned for every compare.
    QList<int> order;
    order.reserve(albums.size());
    for (int i = 0; i < albums.size(); ++i)
        order.append(i);
    std::stable_sort(order.begin(), order.end(), [&](int ia, int ib) {
        const qint64 aa = albumsAddedAt.at(ia);
        const qint64 ab = albumsAddedAt.at(ib);
        if (aa != ab)
            return aa > ab;
        return m_collator.compare(all.at(albums.at(ia).tracks.first()).album,
                                  all.at(albums.at(ib).tracks.first()).album)
                < 0;
    });
    QList<AlbumListModel::Album> sorted;
    sorted.reserve(albums.size());
    for (int i : order)
        sorted.append(std::move(albums[i]));
    m_albumModel.setAlbums(std::move(sorted));
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
        // Pending mixes wait for the scan that will see their files.
        m_rescanPending = false;
        rescan();
    } else if (ok) {
        createPendingMixes();
        createPendingEnrichments();
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

// ---------------------------------------------------------------- M3U

QVariantMap Library::importMix(const QUrl &file)
{
    const auto fail = [](const QString &error) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    };
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return fail(QStringLiteral("Could not open %1").arg(QFileInfo(path).fileName()));
    const QStringList entries = M3u::parse(M3u::decode(f.readAll()), QFileInfo(path).absolutePath());

    QList<qint64> ids;
    int skipped = 0;
    for (const QString &entry : entries) {
        // Symlinked folders: fall back to the canonical path.
        const Track *t = trackByPath(entry);
        if (!t)
            t = trackByPath(QFileInfo(entry).canonicalFilePath());
        if (t)
            ids.append(t->id);
        else
            ++skipped;
    }
    if (ids.isEmpty())
        return fail(entries.isEmpty() ? QStringLiteral("The playlist is empty")
                                      : QStringLiteral("None of its %1 tracks are in your library").arg(entries.size()));

    const qint64 mixId = createMix(QFileInfo(path).completeBaseName());
    if (mixId < 0)
        return fail(m_error);
    const int row = m_mixModel.rowOf(mixId);
    m_mixModel.mixes()[row].trackIds = ids;
    if (!saveMix(row))
        return fail(m_error);
    return {{QStringLiteral("ok"), true}, {QStringLiteral("mixId"), mixId},
            {QStringLiteral("added"), int(ids.size())}, {QStringLiteral("skipped"), skipped}};
}

void Library::addMixAfterScan(const QString &name, const QStringList &paths)
{
    m_pendingMixes.append(PendingMix{name, paths});
    rescan();
}

void Library::createPendingMixes()
{
    const QList<PendingMix> pending = std::exchange(m_pendingMixes, {});
    for (const PendingMix &p : pending) {
        QList<qint64> ids;
        for (const QString &path : p.paths) {
            const Track *t = trackByPath(QDir::cleanPath(path));
            if (!t)
                t = trackByPath(QFileInfo(path).canonicalFilePath());
            if (t)
                ids.append(t->id);
        }
        const int missing = int(p.paths.size() - ids.size());
        const qint64 mixId = ids.isEmpty() ? -1 : createMix(p.name);
        const int row = m_mixModel.rowOf(mixId);
        bool saved = row >= 0;
        if (saved) {
            m_mixModel.mixes()[row].trackIds = ids;
            saved = saveMix(row);
        }
        if (saved)
            emit mixAdded(mixId, p.name, int(ids.size()), missing);
        else
            emit mixAdded(-1, p.name, 0, int(p.paths.size()));
    }
}

bool Library::applyEnrichment(qint64 id, const Track::Enrichment &e)
{
    const auto it = m_indexById.constFind(id);
    if (it == m_indexById.cend())
        return false;
    if (!m_db.setEnrichment(id, e)) {
        setError(QStringLiteral("Could not save the lookup: ") + m_db.lastError());
        return false;
    }
    Track &t = m_tracks[*it];
    t.enriched = e;
    // Gaps only: what the file itself says always wins.
    if (t.albumArtist.isEmpty())
        t.albumArtist = e.albumArtist;
    if (t.year == 0)
        t.year = e.year;
    if (t.trackNo == 0)
        t.trackNo = e.trackNo;
    if (t.discNo == 0)
        t.discNo = e.discNo;
    if (!e.coverPath.isEmpty())
        t.coverPath = e.coverPath;
    refreshKeys(*it);   // an album artist from the lookup moves the album key
    m_trackModel.trackChanged(*it);
    if (!e.coverPath.isEmpty() || !e.albumArtist.isEmpty())
        rebuildAlbums();   // album art and keys may have moved
    emit trackUpdated(id);
    return true;
}

void Library::enrichFiles(const QStringList &paths)
{
    if (!m_enricher || paths.isEmpty())
        return;
    // Files that are already indexed can be looked up right away; the rest
    // wait for the rescan that the watcher will trigger anyway.
    QList<qint64> now;
    for (const QString &path : paths) {
        const Track *t = trackByPath(QDir::cleanPath(path));
        if (!t)
            t = trackByPath(QFileInfo(path).canonicalFilePath());
        if (t)
            now.append(t->id);
        else
            m_pendingEnrich.append(path);
    }
    if (!now.isEmpty())
        m_enricher->enrichAll(now);
    if (!m_pendingEnrich.isEmpty())
        rescan();
}

void Library::createPendingEnrichments()
{
    const QStringList pending = std::exchange(m_pendingEnrich, {});
    if (!m_enricher || pending.isEmpty())
        return;
    QList<qint64> ids;
    for (const QString &path : pending) {
        const Track *t = trackByPath(QDir::cleanPath(path));
        if (!t)
            t = trackByPath(QFileInfo(path).canonicalFilePath());
        if (t)
            ids.append(t->id);
    }
    m_enricher->enrichAll(ids);
}

void Library::enrichAlbum(int albumRow)
{
    if (!m_enricher)
        return;
    const QVariantList ids = albumTrackIds(albumRow);
    QList<qint64> list;
    list.reserve(ids.size());
    for (const QVariant &v : ids)
        list.append(v.toLongLong());
    m_enricher->enrichAll(list);
}

void Library::enrichShown()
{
    if (!m_enricher)
        return;
    const QVariantList ids = visibleTrackIds();
    QList<qint64> list;
    list.reserve(ids.size());
    for (const QVariant &v : ids)
        list.append(v.toLongLong());
    m_enricher->enrichAll(list);
}

void Library::storeLoudness(qint64 id, float rmsDb, float peakDb)
{
    const auto it = m_indexById.constFind(id);
    if (it == m_indexById.cend())
        return;
    Track &t = m_tracks[*it];
    if (qFuzzyCompare(1.0 + t.rmsDb, 1.0 + rmsDb) && qFuzzyCompare(1.0 + t.peakDb, 1.0 + peakDb))
        return;
    t.rmsDb = rmsDb;
    t.peakDb = peakDb;
    if (!m_db.setLoudness(id, rmsDb, peakDb))
        return;   // the numbers stay in memory; the next scan will try again
    emit trackUpdated(id);
}

QVariantMap Library::exportMix(qint64 mixId, const QUrl &file)
{
    const auto fail = [](const QString &error) {
        return QVariantMap{{QStringLiteral("ok"), false}, {QStringLiteral("error"), error}};
    };
    const int row = m_mixModel.rowOf(mixId);
    if (row < 0)
        return fail(QStringLiteral("No such mix"));
    QList<Track> tracks;
    int skipped = 0;   // remote tracks have no file another player could open
    for (qint64 id : m_mixModel.mixes().at(row).trackIds) {
        if (const Track *t = trackById(id)) {
            if (Navidrome::isRemotePath(t->path))
                ++skipped;
            else
                tracks.append(*t);
        }
    }
    const QString path = file.isLocalFile() ? file.toLocalFile() : file.toString();
    // QSaveFile: an interrupted write never leaves a truncated playlist behind.
    QSaveFile out(path);
    if (!out.open(QIODevice::WriteOnly) || out.write(M3u::write(tracks)) < 0 || !out.commit())
        return fail(QStringLiteral("Could not write %1").arg(QFileInfo(path).fileName()));
    return {{QStringLiteral("ok"), true}, {QStringLiteral("added"), int(tracks.size())},
            {QStringLiteral("skipped"), skipped}};
}

// ---------------------------------------------------------------- Navidrome

void Library::setNavidrome(const QString &url, const QString &user, const QString &password)
{
    m_navidrome = NavidromeServer{url.trimmed(), user.trimmed(), password};
    QSettings settings;
    settings.setValue(QStringLiteral("navidrome/url"), m_navidrome.url);
    settings.setValue(QStringLiteral("navidrome/user"), m_navidrome.user);
    settings.setValue(QStringLiteral("navidrome/password"), m_navidrome.password);
    settings.sync();
    // The password is a credential: the settings file is readable by this user only.
    QFile::setPermissions(settings.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    reload();   // tracks of a previously configured server stop showing
    navidromeSync();
}

void Library::navidromeSync()
{
    cancelNavidromeSync();
    if (!m_navidrome.isValid()) {
        m_navidromeStatus = QStringLiteral("Enter the server address, user name and password.");
        emit navidromeChanged();
        return;
    }
    m_navidromeSync = new NavidromeSync(m_navidrome, m_coverDir, {}, this);
    connect(m_navidromeSync, &NavidromeSync::finished, this, &Library::onNavidromeFinished);
    m_navidromeStatus = QStringLiteral("Syncing…");
    emit navidromeChanged();
    m_navidromeSync->start();
}

void Library::navidromeForget()
{
    cancelNavidromeSync();
    if (m_db.isOpen()) {
        const bool ok = m_db.transaction()
            && removeRemoteTracks(Navidrome::pathPrefix(m_navidrome), {}) && m_db.commit();
        if (!ok) {
            m_db.rollback();
            setError(QStringLiteral("Could not remove Navidrome tracks: ") + m_db.lastError());
        }
    }
    QSettings settings;
    settings.remove(QStringLiteral("navidrome"));
    m_navidrome = {};
    m_navidromeStatus.clear();
    reload();
    emit navidromeChanged();
}

QUrl Library::sourceUrl(const Track &t) const
{
    if (!Navidrome::isRemotePath(t.path))
        return QUrl::fromLocalFile(t.path);
    if (!m_navidrome.isValid() || !t.path.startsWith(Navidrome::pathPrefix(m_navidrome)))
        return {};   // a server that is no longer configured
    return Navidrome::streamUrl(m_navidrome, Navidrome::songId(t.path));
}

void Library::onNavidromeFinished(NavidromeSyncResult result)
{
    if (m_navidromeSync) {
        m_navidromeSync->deleteLater();
        m_navidromeSync = nullptr;
    }
    if (!result.ok) {
        m_navidromeStatus = QStringLiteral("Sync failed: ") + result.error;
        emit navidromeChanged();
        return;
    }

    // One transaction: a failure leaves the index as it was before the sync.
    bool ok = m_db.isOpen() && m_db.transaction();
    QString error = ok ? QString() : (m_db.isOpen() ? m_db.lastError()
                                                    : QStringLiteral("library database is not open"));
    QSet<QString> seen;
    seen.reserve(result.tracks.size());
    QSet<QString> albums;
    for (const Track &t : std::as_const(result.tracks)) {
        if (!ok)
            break;
        seen.insert(t.path);
        albums.insert(TrackFormat::albumKey(t));
        if (!m_db.upsert(t)) {
            ok = false;
            error = m_db.lastError();
        }
    }
    if (ok && !removeRemoteTracks(Navidrome::pathPrefix(m_navidrome), seen)) {
        ok = false;
        error = m_db.lastError();
    }
    if (ok && !m_db.commit()) {
        ok = false;
        error = m_db.lastError();
    }
    if (!ok) {
        m_db.rollback();
        m_navidromeStatus = QStringLiteral("Sync failed: ") + error;
        emit navidromeChanged();
        return;
    }

    reload();
    m_navidromeStatus = QStringLiteral("Synced %1 tracks in %2 albums")
                            .arg(result.tracks.size())
                            .arg(albums.size());
    emit navidromeChanged();
}

bool Library::removeRemoteTracks(const QString &prefix, const QSet<QString> &keep)
{
    const QHash<QString, LibraryDb::Stamp> known = m_db.stamps();
    for (auto it = known.cbegin(); it != known.cend(); ++it) {
        if (!it.key().startsWith(prefix) || keep.contains(it.key()))
            continue;
        if (!m_db.remove(it.key()))
            return false;
    }
    return true;
}

void Library::cancelNavidromeSync()
{
    if (!m_navidromeSync)
        return;
    m_navidromeSync->disconnect(this);   // a cancelled sync must not reach onNavidromeFinished
    m_navidromeSync->cancel();
    m_navidromeSync->deleteLater();
    m_navidromeSync = nullptr;
}
