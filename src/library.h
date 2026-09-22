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
#include <QtQml/qqmlregistration.h>

#include <atomic>

class QJSEngine;
class QQmlEngine;
class Library;

// Filtered + sorted view over the library, one row per track.
class TrackListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit TrackListModel(Library *library);
    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    void setRows(QList<int> rows);                // indices into Library::tracks()
    const QList<int> &rows() const { return m_rows; }
    void trackChanged(int trackIndex);

private:
    Library *m_library;
    QList<int> m_rows;
};

// One row per album, carrying the roles of its first track plus `count`.
class AlbumListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    enum Role { TrackCount = TrackRoles::Last + 1, AlbumIndex };

    struct Album {
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

class Library : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel *tracks READ trackModel CONSTANT)
    Q_PROPERTY(QAbstractItemModel *albums READ albumModel CONSTANT)
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(QString format READ format WRITE setFormat NOTIFY formatChanged)
    Q_PROPERTY(QString sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY countsChanged)
    Q_PROPERTY(int shownCount READ shownCount NOTIFY countsChanged)
    Q_PROPERTY(int likedCount READ likedCount NOTIFY countsChanged)
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
    Q_PROPERTY(int scanProgress READ scanProgress NOTIFY scanProgressChanged)
    Q_PROPERTY(QString musicDir READ musicDir CONSTANT)
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
    QString error() const { return m_error; }
    QString storageText() const { return m_storageText; }
    qreal storageUsed() const { return m_storageUsed; }

    const QList<Track> &tracks() const { return m_tracks; }
    const Track *trackById(qint64 id) const;
    const Track *trackByPath(const QString &path) const;

    Q_INVOKABLE void rescan();
    Q_INVOKABLE void toggleLike(qint64 id);
    Q_INVOKABLE QVariantList visibleTrackIds() const;       // current filter/sort order
    Q_INVOKABLE QVariantList albumTrackIds(int albumRow) const;

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

private:
    void reload();
    void applyFilter();
    void rebuildAlbums();
    void updateStorage();
    void setError(const QString &error);
    void onScanFinished(bool ok, const QString &error, const QStringList &dirs, bool changed);

    static inline Library *s_instance = nullptr;

    QString m_musicDir;
    QString m_dbPath;
    QString m_coverDir;
    LibraryDb m_db;

    QList<Track> m_tracks;
    QHash<qint64, int> m_indexById;
    QList<qint64> m_albumAddedAt;   // per track: newest added_at of its album
    int m_likedCount = 0;
    TrackListModel m_trackModel;
    AlbumListModel m_albumModel;

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
