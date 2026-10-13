#include "downloader.h"

#include "library.h"
#include "ytdlp.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJSEngine>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <limits>
#include "processtree.h"

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
    for (auto it = m_running.begin(); it != m_running.end(); ++it)
        it.value().process->disconnect(this);
    for (auto &r : m_running)
        ProcessTree::stop(r.process);
    for (auto &r : m_running) {
        if (!r.process->waitForFinished(2000)) {
            ProcessTree::stop(r.process, true);
            r.process->waitForFinished(1000);
        }
        QDir(r.temp).removeRecursively();
        delete r.process;
    }
    m_running.clear();
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
    case Progress: return overallProgress(job);
    case Detail:
        if (job.state == u"queued") {
            const int waiting = runningCount() >= m_maxConcurrent ? m_maxConcurrent : 0;
            Q_UNUSED(waiting);
            return QStringLiteral("Waiting");
        }
        if (job.state != u"running")
            return job.outcome;
        if (job.index == 0)
            return QStringLiteral("Starting…");
        if (job.count > 1)
            return QStringLiteral("%1 / %2 · %3").arg(job.index).arg(job.count).arg(job.item);
        return job.itemProgress >= 1 ? QStringLiteral("Converting…") : job.item;
    case IsPlaylist: return job.playlist;
    case Items: return itemsVariant(job);
    case ItemCount: return int(job.items.size());
    case Eta: return etaSeconds(job);
    }
    return {};
}

QHash<int, QByteArray> Downloader::roleNames() const
{
    return {{JobId, "jobId"}, {Url, "url"}, {Title, "title"}, {State, "status"},
            {Progress, "progress"}, {Detail, "detail"}, {IsPlaylist, "isPlaylist"},
            {Items, "items"}, {ItemCount, "itemCount"}, {Eta, "eta"}};
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

qreal Downloader::overallProgress(const Job &job)
{
    if (job.state == u"done")
        return 1.0;
    if (!job.items.isEmpty()) {
        double done = 0;
        for (const ItemStatus &it : job.items) {
            if (it.state == u"done")
                done += 1;
            else if (it.state == u"downloading")
                done += qBound(0.0, it.progress, 1.0);
        }
        return done / double(job.items.size());
    }
    return job.count > 0 ? (job.index - 1 + job.itemProgress) / job.count : 0.0;
}

// Keeps samples for the last ~10 s, plus one anchor at or before the window start.
void Downloader::recordSample(Job &job)
{
    constexpr qint64 windowMs = 10000;
    constexpr qsizetype maxSamples = 64;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qreal progress = overallProgress(job);
    if (job.samples.isEmpty() || job.samples.constLast().progress != progress)
        job.samples.append({now, progress});
    while (job.samples.size() > 2
           && (job.samples.at(1).ms <= now - windowMs || job.samples.size() > maxSamples))
        job.samples.removeFirst();
}

// Seconds until the job reaches 100%, or -1 when there is no usable rate yet.
int Downloader::etaSeconds(const Job &job)
{
    if (job.state != u"running")
        return -1;
    const qreal progress = overallProgress(job);
    if (progress < 0.02 || progress >= 1 || job.samples.size() < 2)
        return -1;
    const Sample &first = job.samples.constFirst();
    const Sample &last = job.samples.constLast();
    const qint64 dtMs = last.ms - first.ms;
    const qreal dp = last.progress - first.progress;
    if (dtMs <= 0 || dp <= 0)
        return -1;
    const qreal rate = dp / qreal(dtMs);   // progress per ms
    const qreal seconds = (1 - progress) / rate / 1000;
    return int(std::clamp(seconds, 0.0, qreal(std::numeric_limits<int>::max())));
}

QVariantList Downloader::itemsVariant(const Job &job)
{
    QVariantList out;
    out.reserve(job.items.size());
    for (const ItemStatus &it : job.items) {
        out.append(QVariantMap{
            {QStringLiteral("index"), it.index},
            {QStringLiteral("title"), it.title},
            {QStringLiteral("artist"), it.artist},
            {QStringLiteral("state"), it.state},
            {QStringLiteral("progress"), it.progress},
            {QStringLiteral("file"), it.file},
        });
    }
    return out;
}

Downloader::ItemStatus *Downloader::findItem(Job &job, int index)
{
    for (ItemStatus &it : job.items) {
        if (it.index == index)
            return &it;
    }
    return nullptr;
}

void Downloader::ensureItems(Job &job, int count)
{
    if (count <= 0)
        return;
    for (int i = 1; i <= count; ++i) {
        if (!findItem(job, i)) {
            ItemStatus it;
            it.index = i;
            it.title = QStringLiteral("Track %1").arg(i);
            it.state = QStringLiteral("waiting");
            job.items.append(it);
        }
    }
    std::sort(job.items.begin(), job.items.end(),
              [](const ItemStatus &a, const ItemStatus &b) { return a.index < b.index; });
}

int Downloader::active() const
{
    return int(std::count_if(m_jobs.cbegin(), m_jobs.cend(), [](const Job &job) {
        return job.state == u"queued" || job.state == u"running";
    }));
}

void Downloader::setMaxConcurrent(int n)
{
    n = qBound(1, n, 8);
    if (n == m_maxConcurrent)
        return;
    m_maxConcurrent = n;
    emit maxConcurrentChanged();
    emit countChanged();
    startNext();
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
        for (ItemStatus &it : job.items) {
            if (it.state == u"waiting" || it.state == u"downloading")
                it.state = QStringLiteral("failed");
        }
        changed(row);
        emit countChanged();
    } else if (job.state == u"running" && !job.canceled) {
        job.canceled = true;
        stopProcess(jobId);   // onFinished wraps up
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

QVariantList Downloader::jobItems(int jobId) const
{
    const int row = rowOf(jobId);
    if (row < 0)
        return {};
    return itemsVariant(m_jobs.at(row));
}

void Downloader::startNext()
{
    while (int(m_running.size()) < m_maxConcurrent) {
        int row = -1;
        for (int i = int(m_jobs.size()) - 1; i >= 0; --i) {   // oldest first
            if (m_jobs.at(i).state == u"queued") {
                row = i;
                break;
            }
        }
        if (row < 0)
            return;
        startJob(row);
    }
}

void Downloader::startJob(int row)
{
    Job &job = m_jobs[row];
    const int jobId = job.id;
    job.state = QStringLiteral("running");
    const QString temp = m_tempRoot + QStringLiteral("/job-") + QString::number(jobId);
    QDir(temp).removeRecursively();
    QDir().mkpath(temp);
    QDir().mkpath(destination());

    YtDlp::Request request;
    request.url = job.url;
    request.outDir = destination();
    request.tempDir = temp;
    request.playlist = job.playlist;
    request.album = job.album;

    const bool wasIdle = m_running.isEmpty();
    QProcess *process = new QProcess(this);
    process->setProgram(m_program);
    process->setArguments(YtDlp::arguments(request));
    // Own process group on POSIX: cancelling must also stop the ffmpeg yt-dlp runs.
    ProcessTree::prepare(process);
    Running running;
    running.process = process;
    running.temp = temp;
    m_running.insert(jobId, running);
    connect(process, &QProcess::readyReadStandardOutput, this,
            [this, jobId] { readOutput(jobId, QProcess::StandardOutput); });
    connect(process, &QProcess::readyReadStandardError, this,
            [this, jobId] { readOutput(jobId, QProcess::StandardError); });
    connect(process, &QProcess::finished, this,
            [this, jobId](int code, QProcess::ExitStatus status) { onFinished(jobId, code, status); });
    connect(process, &QProcess::errorOccurred, this,
            [this, jobId](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    onFinished(jobId, -1, QProcess::CrashExit);
            });
    changed(row);
    emit countChanged();
    if (wasIdle)
        emit busyChanged();
    process->start();
}

void Downloader::stopProcess(int jobId)
{
    const auto it = m_running.find(jobId);
    if (it == m_running.end())
        return;
    QProcess *process = it.value().process;
    if (process)
        ProcessTree::stop(process);
}

void Downloader::readOutput(int jobId, QProcess::ProcessChannel channel)
{
    const auto it = m_running.find(jobId);
    if (it == m_running.end())
        return;
    Running &running = m_running[jobId];
    QProcess *process = running.process;
    if (!process)
        return;
    QByteArray &buffer = channel == QProcess::StandardOutput ? running.outBuf : running.errBuf;
    process->setReadChannel(channel);
    buffer += process->readAll();
    qsizetype start = 0;
    for (qsizetype i = 0; i < buffer.size(); ++i) {
        if (buffer.at(i) == '\n' || buffer.at(i) == '\r') {
            if (i > start)
                handleLine(jobId, buffer.mid(start, i - start));
            start = i + 1;
        }
    }
    buffer.remove(0, start);
}

void Downloader::handleLine(int jobId, const QByteArray &line)
{
    const int row = rowOf(jobId);
    if (row < 0)
        return;
    Job &job = m_jobs[row];
    const YtDlp::Event e = YtDlp::parseLine(line);
    switch (e.kind) {
    case YtDlp::Event::Progress: {
        job.itemProgress = e.total > 0 ? qBound(0.0, double(e.done) / double(e.total), 1.0) : 0.0;
        if (ItemStatus *it = findItem(job, job.index)) {
            if (it->state != u"done") {
                it->state = QStringLiteral("downloading");
                it->progress = job.itemProgress;
            }
        }
        break;
    }
    case YtDlp::Event::Item:
        job.index = e.index;
        job.count = qMax(job.count, e.count);
        job.item = e.title;
        job.itemProgress = 0;
        if (!e.list.isEmpty())
            job.list = e.list;
        ensureItems(job, job.count);
        if (ItemStatus *it = findItem(job, e.index)) {
            it->title = e.title;
            it->artist = e.artist;
            if (it->state != u"done")
                it->state = QStringLiteral("downloading");
            it->progress = 0;
        }
        break;
    case YtDlp::Event::File: {
        job.files.append(QDir::cleanPath(e.path));
        job.itemProgress = 1;
        if (ItemStatus *it = findItem(job, job.index)) {
            it->state = QStringLiteral("done");
            it->progress = 1;
            it->file = QDir::cleanPath(e.path);
        } else if (!job.items.isEmpty()) {
            // Single-song jobs that never saw an item line still get a row.
            ItemStatus *last = nullptr;
            for (ItemStatus &it : job.items) {
                if (it.state == u"downloading")
                    last = &it;
            }
            if (last) {
                last->state = QStringLiteral("done");
                last->progress = 1;
                last->file = QDir::cleanPath(e.path);
            }
        }
        // Songs show up as they finish. Not left to the file watcher: it misses
        // folders created before it watches their parent (a first download).
        m_library->rescanSoon();
        break;
    }
    case YtDlp::Event::Error: {
        job.errors.append(e.message);
        // Attribute playlist gaps (e.g. an unavailable video that never emits
        // flow-item) to the next waiting track so the expanded list shows it.
        ItemStatus *slot = nullptr;
        for (ItemStatus &it : job.items) {
            if (it.state == u"waiting" && it.index > job.index) {
                if (!slot || it.index < slot->index)
                    slot = &it;
            }
        }
        if (!slot) {
            for (ItemStatus &it : job.items) {
                if (it.state == u"waiting") {
                    if (!slot || it.index < slot->index)
                        slot = &it;
                }
            }
        }
        if (slot) {
            slot->state = QStringLiteral("failed");
            slot->progress = 0;
        }
        break;
    }
    case YtDlp::Event::None:
        return;
    }
    recordSample(job);
    changed(row);
}

void Downloader::onFinished(int jobId, int exitCode, QProcess::ExitStatus status)
{
    const auto it = m_running.find(jobId);
    if (it == m_running.end())
        return;
    Running running = m_running.take(jobId);
    // Whatever was left without a newline.
    for (QByteArray *buffer : {&running.outBuf, &running.errBuf}) {
        if (!buffer->isEmpty())
            handleLine(jobId, *buffer);
        buffer->clear();
    }
    const bool failedToStart = running.process->error() == QProcess::FailedToStart;
    running.process->disconnect(this);
    running.process->deleteLater();
    QDir(running.temp).removeRecursively();

    const bool nowIdle = m_running.isEmpty();
    const int row = rowOf(jobId);
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
            for (ItemStatus &item : job.items) {
                if (item.state == u"waiting" || item.state == u"downloading") {
                    item.state = QStringLiteral("failed");
                    item.progress = 0;
                }
            }
        } else if (n > 0) {
            job.state = QStringLiteral("done");
            job.outcome = songs;
            if (!job.errors.isEmpty())
                job.outcome += QStringLiteral(" · %1 unavailable").arg(job.errors.size());
            for (ItemStatus &item : job.items) {
                if (item.state == u"downloading" || item.state == u"waiting") {
                    // Leftover placeholders never emitted by yt-dlp belong to
                    // skipped entries; don't leave them spinning.
                    if (item.title.startsWith(QStringLiteral("Track "))) {
                        item.state = QStringLiteral("failed");
                    } else {
                        item.state = QStringLiteral("done");
                        item.progress = 1;
                    }
                }
            }
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
            if (job.items.isEmpty()) {
                ItemStatus item;
                item.index = 1;
                item.title = job.item.isEmpty() ? title : job.item;
                item.state = QStringLiteral("failed");
                job.items.append(item);
            } else {
                for (ItemStatus &item : job.items) {
                    if (item.state == u"waiting" || item.state == u"downloading")
                        item.state = QStringLiteral("failed");
                }
            }
        }
        job.itemProgress = job.state == u"done" ? 1 : job.itemProgress;
        // Keep overall progress consistent for the finished state.
        if (job.state == u"done") {
            for (ItemStatus &item : job.items) {
                if (item.state == u"done")
                    item.progress = 1;
            }
        }
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
            // Fresh files have the least metadata of anything in the library,
            // so they are looked up first. The scan the rescan triggers also
            // resolves the paths the enricher is still waiting for.
            m_library->enrichFiles(job.files);
        }
        if (job.state == u"failed")
            emit notice(QStringLiteral("Download failed: ") + job.outcome);
        else if (job.state == u"done" && !mixPending)
            emit notice(QStringLiteral("Downloaded ") + (n == 1 ? title : songs + QStringLiteral(" · ") + title));
    }

    emit countChanged();
    if (nowIdle)
        emit busyChanged();
    startNext();
}
