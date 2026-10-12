#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QtQml/qqmlregistration.h>

class QJSEngine;
class QQmlEngine;
class Library;

// Downloads songs, playlists and albums from YouTube / YouTube Music with
// yt-dlp into <music folder>/YouTube, up to maxConcurrent jobs at once.
// Finished files are picked up by a library rescan; a playlist also becomes
// a mix (albums already show up as albums). The object is the job list model
// itself.
class Downloader : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QAbstractItemModel *jobs READ jobs CONSTANT)
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString missing READ missing CONSTANT)   // "yt-dlp", "ffmpeg" or ""
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int active READ active NOTIFY countChanged)   // queued + running
    Q_PROPERTY(int runningCount READ runningCount NOTIFY countChanged)
    Q_PROPERTY(int maxConcurrent READ maxConcurrent WRITE setMaxConcurrent NOTIFY maxConcurrentChanged)
    Q_PROPERTY(QString destination READ destination NOTIFY destinationChanged)

public:
    enum Role {
        JobId = Qt::UserRole + 1,
        Url,
        Title,      // playlist title, else the song, else the link
        State,      // queued | running | done | failed | canceled
        Progress,   // 0..1 over the whole job
        Detail,     // "3 / 12 · Song" while running, the outcome after
        IsPlaylist,
        Items,      // QVariantList of {index, title, artist, state, progress}
        ItemCount,
        Eta,        // seconds remaining over the whole job, -1 when unknown
    };

    // `program` overrides the yt-dlp found on PATH (tests use a fake one);
    // without an override ffmpeg is required too.
    Downloader(Library *library, const QString &tempRoot, const QString &program = {},
               QObject *parent = nullptr);
    ~Downloader() override;

    static Downloader *create(QQmlEngine *qmlEngine, QJSEngine *jsEngine);
    static void setInstance(Downloader *downloader) { s_instance = downloader; }

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QAbstractItemModel *jobs() { return this; }
    bool available() const { return m_missing.isEmpty(); }
    QString missing() const { return m_missing; }
    bool busy() const { return !m_running.isEmpty(); }
    int count() const { return int(m_jobs.size()); }
    int active() const;
    int runningCount() const { return int(m_running.size()); }
    int maxConcurrent() const { return m_maxConcurrent; }
    void setMaxConcurrent(int n);
    QString destination() const;

    Q_INVOKABLE static bool isSupportedUrl(const QString &url);
    Q_INVOKABLE static bool isPlaylistUrl(const QString &url);

    // Queues a download; `playlist` false takes only the song of a watch link.
    // Returns "" or why the link was not queued.
    Q_INVOKABLE QString add(const QString &url, bool playlist);
    Q_INVOKABLE void cancel(int jobId);
    Q_INVOKABLE void clearFinished();
    Q_INVOKABLE QVariantList jobItems(int jobId) const;

signals:
    void busyChanged();
    void countChanged();
    void destinationChanged();
    void maxConcurrentChanged();
    // One line for a toast, when a job ends (and when its mix is made).
    void notice(const QString &text);

private:
    struct ItemStatus {
        int index = 0;
        QString title;
        QString artist;
        QString state = QStringLiteral("waiting");   // waiting | downloading | done | failed
        qreal progress = 0;
        QString file;
    };
    struct Sample {
        qint64 ms = 0;
        qreal progress = 0;
    };
    struct Job {
        int id = 0;
        QString url;
        bool playlist = false;
        bool album = false;
        QString state = QStringLiteral("queued");
        QString list;        // playlist title, from yt-dlp
        QString item;        // current song
        int index = 0;       // current song, 1-based
        int count = 0;
        qreal itemProgress = 0;
        QStringList files;   // finished, in playlist order
        QStringList errors;
        bool canceled = false;
        QString outcome;
        QList<ItemStatus> items;   // per-track status, in index order
        QList<Sample> samples;   // (time, overall progress) history, for eta
    };
    struct Running {
        QProcess *process = nullptr;
        QString temp;
        QByteArray outBuf;
        QByteArray errBuf;
    };

    int rowOf(int jobId) const;
    void changed(int row);
    static qreal overallProgress(const Job &job);
    static int etaSeconds(const Job &job);
    static void recordSample(Job &job);
    static QVariantList itemsVariant(const Job &job);
    ItemStatus *findItem(Job &job, int index);
    void ensureItems(Job &job, int count);
    void startNext();
    void startJob(int row);
    void readOutput(int jobId, QProcess::ProcessChannel channel);
    void handleLine(int jobId, const QByteArray &line);
    void onFinished(int jobId, int exitCode, QProcess::ExitStatus status);
    void stopProcess(int jobId);

    static inline Downloader *s_instance = nullptr;

    Library *m_library;
    QString m_tempRoot;
    QString m_program;
    QString m_missing;
    QList<Job> m_jobs;
    int m_nextId = 1;
    int m_maxConcurrent = 3;
    QHash<int, Running> m_running;   // jobId -> live yt-dlp process
};
