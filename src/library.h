#pragma once

#include "librarydb.h"
#include "track.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QHash>
#include <QList>
#include <QObject>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <QtQml/qqmlregistration.h>

#include <atomic>

class QJSEngine;
class QQmlEngine;
class Library;

// Filtered + sorted view over the current source (all / album / mix).
class TrackListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { Slot = TrackRoles::Last + 1 };

    struct Row {
        int track;      // index into Library::tracks()
        int slot = -1;  // position in the mix when viewing one, else -1
    };

    explicit TrackListModel(Library *library);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(QList<Row> rows);
    const QList<Row> &rows() const { return m_rows; }
    void trackChanged(int trackIndex);

private:
    Library *m_library;
    QList<Row> m_rows;
};

// One row per album, carrying the roles of its first track plus `count`.
class AlbumListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { TrackCount = TrackRoles::Last + 1, AlbumIndex };

    struct Album {
        QString key;         // TrackFormat::albumKey
        QList<int> tracks;   // indices into Library::tracks(), in play order
    };

    explicit AlbumListModel(Library *library);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setAlbums(QList<Album> albums);
    const QList<Album> &albums() const { return m_albums; }

private:
    Library *m_library;
    QList<Album> m_albums;
};

// The user's mixes (playlists), in creation order.
class MixListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { MixId = Qt::UserRole + 1, Name, TrackCount };

    explicit MixListModel(Library *library);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void reset(QList<LibraryDb::Playlist> mixes);
    void changed(int row);
    QList<LibraryDb::Playlist> &mixes() { return m_mixes; }
    const QList<LibraryDb::Playlist> &mixes() const { return m_mixes; }
    int rowOf(qint64 id) const;

private:
    QList<LibraryDb::Playlist> m_mixes;
};

class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel *tracks READ trackModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *albums READ albumModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *mixes READ mixModel CONSTANT)
    Q_PROPERTY(QString viewKind READ viewKind NOTIFY viewChanged)      // all | album | mix
    Q_PROPERTY(QString viewTitle READ viewTitle NOTIFY viewChanged)
    Q_PROPERTY(QString viewSubtitle READ viewSubtitle NOTIFY viewChanged)
    Q_PROPERTY(qint64 viewMixId READ viewMixId NOTIFY viewChanged)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY formatChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countsChanged)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY countsChanged)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY countsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int scanProgress READ scanProgress NOTIFY scanProgressChanged)
    Q_PROPERTY(QString musicDir READ musicDir WRITE setMusicDir NOTIFY musicDirChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QString storageText READ storageText NOTIFY countsChanged)
    Q_PROPERTY(qreal storageUsed READ storageUsed NOTIFY countsChanged)

public:
    Library(const QString &musicDir, const QString &dbPath, const QString &coverDir,
            QObject *parent = nullptr);
    ~Library() override;

    // QML singleton hook: hands out the instance created in main().
    static Library *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    static void setInstance(Library *library) { s_instance = library; }

    QAbstractItemModel *trackModel() { return &m_trackModel; }
    QAbstractItemModel *albumModel() { return &m_albumModel; }
    QAbstractItemModel *mixModel() { return &m_mixModel; }
    QString viewKind() const { return m_viewKind; }
    QString viewTitle() const;
    QString viewSubtitle() const;
    qint64 viewMixId() const { return m_viewKind == u"mix" ? m_viewMixId : -1; }
    QString query() const { return m_query; }
    void setQuery(const QString &query);
    QString format() const { return m_format; }
    void setFormat(const QString &format);
    QString sortMode() const { return m_sortMode; }
    void setSortMode(const QString &mode);
    int totalCount() const { return int(m_tracks.size()); }
    int shownCount() const { return int(m_trackModel.rows().size()); }
    int likedCount() const { return m_likedCount; }
    bool scanning() const { return m_scanThread != nullptr; }
    int scanProgress() const { return m_scanProgress; }
    QString musicDir() const { return m_musicDir; }
    // Switches the library folder: shows what is already indexed there, then
    // rescans. Other folders' tracks (and their likes / mixes) stay in the index.
    void setMusicDir(const QString &dir);
    QString error() const { return m_error; }
    QString storageText() const { return m_storageText; }
    qreal storageUsed() const { return m_storageUsed; }

    const QList<Track> &tracks() const { return m_tracks; }
    const Track *trackById(qint64 id) const;
    const Track *trackByPath(const QString &path) const;
    QByteArray cachedPeaks(qint64 id);
    void storePeaks(qint64 id, const QByteArray &peaks);

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void toggleLike(qint64 id);
    Q_INVOKABLE QVariantList visibleTrackIds() const;       // current filter/sort order
    Q_INVOKABLE QVariantList albumTrackIds(int albumRow) const;

    // Which tracks the list shows; filters and sort apply on top.
    Q_INVOKABLE void showAll();
    Q_INVOKABLE void showAlbum(int albumRow);
    Q_INVOKABLE void showMix(qint64 mixId);

    Q_INVOKABLE qint64 createMix(const QString &name);   // -1 on failure
    Q_INVOKABLE void renameMix(qint64 mixId, const QString &name);
    Q_INVOKABLE void deleteMix(qint64 mixId);
    Q_INVOKABLE void addToMix(qint64 mixId, qint64 trackId);
    Q_INVOKABLE void removeFromMix(qint64 mixId, int slot);
    Q_INVOKABLE void moveInMix(qint64 mixId, int fromSlot, int toSlot);
    Q_INVOKABLE QVariantList mixTrackIds(qint64 mixId) const;

    // M3U: import makes a new mix from the entries found in the library;
    // result {ok, mixId, added, skipped, error}. Export writes .m3u8.
    Q_INVOKABLE QVariantMap importMix(const QUrl &file);
    Q_INVOKABLE QVariantMap exportMix(qint64 mixId, const QUrl &file);

signals:
    void queryChanged();
    void formatChanged();
    void sortModeChanged();
    void countsChanged();
    void scanningChanged();
    void scanProgressChanged();
    void errorChanged();
    void trackUpdated(qint64 id);   // metadata or liked changed
    void reloaded();                // whole library reloaded from the database
    void viewChanged();
    void musicDirChanged();

private:
    void reload();
    void applyFilter();
    void rebuildAlbums();
    void updateStorage();
    void setError(const QString &error);
    void reloadMixes();
    bool saveMix(int row);
    const AlbumListModel::Album *viewedAlbum() const;
    void setView(const QString &kind, const QString &albumKey, qint64 mixId);
    void onScanFinished(bool ok, const QString &error, const QStringList &dirs, bool changed);

    static inline Library *s_instance = nullptr;

    QString m_musicDir;
    QString m_dbPath;
    QString m_coverDir;
    LibraryDb m_db;

    QList<Track> m_tracks;
    QHash<qint64, int> m_indexById;
    QHash<QString, int> m_indexByPath;
    QList<qint64> m_albumAddedAt;   // per track: newest added_at of its album
    int m_likedCount = 0;
    TrackListModel m_trackModel;
    AlbumListModel m_albumModel;
    MixListModel m_mixModel;

    QString m_viewKind = QStringLiteral("all");
    QString m_viewAlbumKey;       // stable across rescans, unlike the row
    qint64 m_viewMixId = -1;

    QString m_query;
    QString m_format = QStringLiteral("All");
    QString m_sortMode = QStringLiteral("def");

    QThread *m_scanThread = nullptr;
    std::atomic_bool m_cancelScan = false;
    bool m_rescanPending = false;
    int m_scanProgress = 0;
    QFileSystemWatcher m_watcher;
    QTimer m_rescanDebounce;

    QString m_error;
    QString m_storageText;
    qreal m_storageUsed = 0;
};
