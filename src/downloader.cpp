#include "downloader.h"

#include "library.h"
#include "ytdlp.h"

#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <csignal>
#include <unistd.h>

Downloader::Downloader(Library *library, const QString &tempRoot, const QString &program,
                       QObject *parent)
    : QAbstractListModel(parent)
    , m_library(library)
    , m_tempRoot(tempRoot)
    , m_program(program)
{
    if (m_program.isEmpty()) {
        m_program = QStandardPaths::findExecutable(QStringLiteral("yt-dlp"));
        // yt-dlp needs ffmpeg to turn the downloaded stream into an MP3.
        if (m_program.isEmpty())
            m_missing = QStringLiteral("yt-dlp");
        else if (QStandardPaths::findExecutable(QStringLiteral("ffmpeg")).isEmpty())
            m_missing = QStringLiteral("ffmpeg");
    }
    connect(m_library, &Library::musicDirChanged, this, &Downloader::destinationChanged);
    connect(m_library, &Library::mixAdded, this,
            [this](qint64 mixId, const QString &name, int added, int) {
                if (mixId >= 0)
                    emit notice(QStringLiteral("Mix “%1” · %2 %3").arg(name).arg(added)
                                    .arg(added == 1 ? QStringLiteral("song") : QStringLiteral("songs")));
            });
}

Downloader::~Downloader()
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    stopProcess();
    if (!m_process->waitForFinished(3000)) {
        ::kill(-pid_t(m_process->processId()), SIGKILL);
        m_process->waitForFinished(1000);
    }
    delete m_process;
    QDir(m_runningTemp).removeRecursively();
}

Downloader *Downloader::create(QQmlEngine *, QJSEngine *jsEngine)
{
    Q_ASSERT(s_instance);
    Q_ASSERT(jsEngine->thread() == s_instance->thread());
    QJSEngine::setObjectOwnership(s_instance, QJSEngine::CppOwnership);
    return s_instance;
}

// ---------------------------------------------------------------- model

int Downloader::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_jobs.size());
}

QVariant Downloader::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Job &job = m_jobs.at(index.row());
    switch (role) {
    case JobId: return job.id;
    case Url: return job.url;
    case Title:
        if (!job.list.isEmpty())
            return YtDlp::listName(job.list);
        if (!job.item.isEmpty())
            return job.item;
        return QString(job.url).remove(QRegularExpression(QStringLiteral("^https?://(www\\.)?")));
    case State: return job.state;
    case Progress:
        if (job.state == u"done")
            return 1.0;
        return job.count > 0 ? (job.index - 1 + job.itemProgress) / job.count : 0.0;
    case Detail:
        if (job.state == u"queued")
            return QStringLiteral("Waiting");
        if (job.state != u"running")
            return job.outcome;
        if (job.index == 0)
            return QStringLiteral("Starting…");
        if (job.count > 1)
            return QStringLiteral("%1 / %2 · %3").arg(job.index).arg(job.count).arg(job.item);
        return job.itemProgress >= 1 ? QStringLiteral("Converting…") : job.item;
    case IsPlaylist: return job.playlist;
    }
    return {};
}

QHash<int, QByteArray> Downloader::roleNames() const
{
    return {{JobId, "jobId"}, {Url, "url"}, {Title, "title"}, {State, "status"},
            {Progress, "progress"}, {Detail, "detail"}, {IsPlaylist, "isPlaylist"}};
}

int Downloader::rowOf(int jobId) const
{
    for (int i = 0; i < m_jobs.size(); ++i) {
        if (m_jobs.at(i).id == jobId)
            return i;
    }
    return -1;
}

void Downloader::changed(int row)
{
    if (row >= 0)
        emit dataChanged(index(row), index(row));
}

int Downloader::active() const
{
    return int(std::count_if(m_jobs.cbegin(), m_jobs.cend(), [](const Job &job) {
        return job.state == u"queued" || job.state == u"running";
    }));
}

QString Downloader::destination() const
{
    return m_library->musicDir() + QStringLiteral("/YouTube");
}

bool Downloader::isSupportedUrl(const QString &url)
{
    return YtDlp::isSupportedUrl(url);
}

bool Downloader::isPlaylistUrl(const QString &url)
{
    return YtDlp::isPlaylistUrl(url);
}

// ---------------------------------------------------------------- jobs

QString Downloader::add(const QString &url, bool playlist)
{
    if (!m_missing.isEmpty())
        return m_missing + QStringLiteral(" is not installed");
    if (!YtDlp::isSupportedUrl(url))
        return QStringLiteral("Paste a YouTube or YouTube Music link");
    const QString link = YtDlp::normalizedUrl(url);
    for (const Job &job : std::as_const(m_jobs)) {
        if (job.url == link && job.playlist == playlist
            && (job.state == u"queued" || job.state == u"running"))
            return QStringLiteral("Already downloading");
    }

    Job job;
    job.id = m_nextId++;
    job.url = link;
    job.playlist = playlist;
    job.album = playlist && YtDlp::isAlbumUrl(link);
    // Newest first, so a fresh link shows up where it was pasted.
    beginInsertRows({}, 0, 0);
    m_jobs.prepend(job);
    endInsertRows();
    emit countChanged();
    startNext();
    return {};
}

void Downloader::cancel(int jobId)
{
    const int row = rowOf(jobId);
    if (row < 0)
        return;
    Job &job = m_jobs[row];
    if (job.state == u"queued") {
        job.state = QStringLiteral("canceled");
        job.outcome = QStringLiteral("Canceled");
        changed(row);
        emit countChanged();
    } else if (job.state == u"running" && !job.canceled) {
        job.canceled = true;
        stopProcess();   // onFinished wraps up
    }
}

void Downloader::clearFinished()
{
    for (int i = int(m_jobs.size()) - 1; i >= 0; --i) {
        const QString &state = m_jobs.at(i).state;
        if (state != u"queued" && state != u"running") {
            beginRemoveRows({}, i, i);
            m_jobs.removeAt(i);
            endRemoveRows();
        }
    }
    emit countChanged();
}

void Downloader::startNext()
{
    if (m_process)
        return;
    int row = -1;
    for (int i = int(m_jobs.size()) - 1; i >= 0; --i) {   // oldest first
        if (m_jobs.at(i).state == u"queued") {
            row = i;
            break;
        }
    }
    if (row < 0)
        return;

    Job &job = m_jobs[row];
    job.state = QStringLiteral("running");
    m_runningId = job.id;
    m_runningTemp = m_tempRoot + QStringLiteral("/job-") + QString::number(job.id);
    QDir(m_runningTemp).removeRecursively();
    QDir().mkpath(m_runningTemp);
    QDir().mkpath(destination());

    YtDlp::Request request;
    request.url = job.url;
    request.outDir = destination();
    request.tempDir = m_runningTemp;
    request.playlist = job.playlist;
    request.album = job.album;

    m_stdoutBuffer.clear();
    m_stderrBuffer.clear();
    m_process = new QProcess(this);
    m_process->setProgram(m_program);
    m_process->setArguments(YtDlp::arguments(request));
    // Own process group: cancelling must also stop the ffmpeg yt-dlp runs.
    m_process->setChildProcessModifier([] { ::setpgid(0, 0); });
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            [this] { readOutput(QProcess::StandardOutput); });
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this] { readOutput(QProcess::StandardError); });
    connect(m_process, &QProcess::finished, this, &Downloader::onFinished);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            onFinished(-1, QProcess::CrashExit);
    });
    changed(row);
    emit busyChanged();
    m_process->start();
}

void Downloader::stopProcess()
{
    if (m_process && m_process->processId() > 0)
        ::kill(-pid_t(m_process->processId()), SIGTERM);
}

void Downloader::readOutput(QProcess::ProcessChannel channel)
{
    QByteArray &buffer = channel == QProcess::StandardOutput ? m_stdoutBuffer : m_stderrBuffer;
    m_process->setReadChannel(channel);
    buffer += m_process->readAll();
    qsizetype start = 0;
    for (qsizetype i = 0; i < buffer.size(); ++i) {
        if (buffer.at(i) == '\n' || buffer.at(i) == '\r') {
            if (i > start)
                handleLine(buffer.mid(start, i - start));
            start = i + 1;
        }
    }
    buffer.remove(0, start);
}

void Downloader::handleLine(const QByteArray &line)
{
    const int row = rowOf(m_runningId);
    if (row < 0)
        return;
    Job &job = m_jobs[row];
    const YtDlp::Event e = YtDlp::parseLine(line);
    switch (e.kind) {
    case YtDlp::Event::Progress:
        job.itemProgress = e.total > 0 ? qBound(0.0, double(e.done) / double(e.total), 1.0) : 0.0;
        break;
    case YtDlp::Event::Item:
        job.index = e.index;
        job.count = e.count;
        job.item = e.title;
        job.itemProgress = 0;
        if (!e.list.isEmpty())
            job.list = e.list;
        break;
    case YtDlp::Event::File:
        job.files.append(QDir::cleanPath(e.path));
        job.itemProgress = 1;
        // Songs show up as they finish. Not left to the file watcher: it misses
        // folders created before it watches their parent (a first download).
        m_library->rescanSoon();
        break;
    case YtDlp::Event::Error:
        job.errors.append(e.message);
        break;
    case YtDlp::Event::None:
        return;
    }
    changed(row);
}

void Downloader::onFinished(int exitCode, QProcess::ExitStatus status)
{
    if (!m_process)
        return;
    // Whatever was left without a newline.
    for (QByteArray *buffer : {&m_stdoutBuffer, &m_stderrBuffer}) {
        if (!buffer->isEmpty())
            handleLine(*buffer);
        buffer->clear();
    }
    const bool failedToStart = m_process->error() == QProcess::FailedToStart;
    m_process->disconnect(this);
    m_process->deleteLater();
    m_process = nullptr;
    QDir(m_runningTemp).removeRecursively();

    const int row = rowOf(m_runningId);
    m_runningId = 0;
    if (row >= 0) {
        Job &job = m_jobs[row];
        const int n = int(job.files.size());
        const QString songs = QString::number(n)
                              + (n == 1 ? QStringLiteral(" song") : QStringLiteral(" songs"));
        const QString title = data(index(row), Title).toString();
        bool mixPending = false;
        if (job.canceled) {
            job.state = QStringLiteral("canceled");
            job.outcome = n > 0 ? QStringLiteral("Canceled · kept ") + songs : QStringLiteral("Canceled");
        } else if (n > 0) {
            job.state = QStringLiteral("done");
            job.outcome = songs;
            if (!job.errors.isEmpty())
                job.outcome += QStringLiteral(" · %1 unavailable").arg(job.errors.size());
        } else {
            job.state = QStringLiteral("failed");
            if (failedToStart)
                job.outcome = QStringLiteral("Could not start ") + QFileInfo(m_program).fileName();
            else if (!job.errors.isEmpty())
                job.outcome = job.errors.last();
            else if (status == QProcess::CrashExit)
                job.outcome = QStringLiteral("yt-dlp stopped unexpectedly");
            else
                job.outcome = exitCode == 0 ? QStringLiteral("Nothing to download")
                                            : QStringLiteral("yt-dlp failed (exit code %1)").arg(exitCode);
        }
        job.itemProgress = job.state == u"done" ? 1 : job.itemProgress;
        changed(row);

        if (n > 0) {
            // Albums show up as albums on their own; a playlist becomes a mix.
            if (job.playlist && !job.album && !job.canceled) {
                m_library->addMixAfterScan(job.list.isEmpty() ? QStringLiteral("YouTube playlist")
                                                              : YtDlp::listName(job.list),
                                           job.files);
                mixPending = true;
            } else {
                m_library->rescan();
            }
        }
        if (job.state == u"failed")
            emit notice(QStringLiteral("Download failed: ") + job.outcome);
        else if (job.state == u"done" && !mixPending)
            emit notice(QStringLiteral("Downloaded ") + (n == 1 ? title : songs + QStringLiteral(" · ") + title));
    }

    startNext();
    if (!m_process)
        emit busyChanged();
    emit countChanged();
}
