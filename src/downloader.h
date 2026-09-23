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
// yt-dlp into <music folder>/YouTube, one job at a time. Finished files are
// picked up by a library rescan; a playlist also becomes a mix (albums
// already show up as albums). The object is the job list model itself.
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
    bool busy() const { return m_process != nullptr; }
    int count() const { return int(m_jobs.size()); }
    int active() const;
    QString destination() const;

    Q_INVOKABLE static bool isSupportedUrl(const QString &url);
    Q_INVOKABLE static bool isPlaylistUrl(const QString &url);

    // Queues a download; `playlist` false takes only the song of a watch link.
    // Returns "" or why the link was not queued.
    Q_INVOKABLE QString add(const QString &url, bool playlist);
    Q_INVOKABLE void cancel(int jobId);
    Q_INVOKABLE void clearFinished();

signals:
    void busyChanged();
    void countChanged();
    void destinationChanged();
    // One line for a toast, when a job ends (and when its mix is made).
    void notice(const QString &text);

private:
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
    };

    int rowOf(int jobId) const;
    void changed(int row);
    void startNext();
    void readOutput(QProcess::ProcessChannel channel);
    void handleLine(const QByteArray &line);
    void onFinished(int exitCode, QProcess::ExitStatus status);
    void stopProcess();

    static inline Downloader *s_instance = nullptr;

    Library *m_library;
    QString m_tempRoot;
    QString m_program;
    QString m_missing;
    QList<Job> m_jobs;
    int m_nextId = 1;
    QProcess *m_process = nullptr;
    int m_runningId = 0;
    QString m_runningTemp;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
};
