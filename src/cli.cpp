// flow-cli — Flow without a window: the same SQLite index, the same yt-dlp
// backend and the same settings the app uses, driven from a terminal.
//
//   flow-cli download <url>…   queue songs / playlists / albums
//   flow-cli scan              bring the index up to date
//   flow-cli list              print the library
//   flow-cli mixes             print the mixes
//   flow-cli doctor            paths and the tools it needs
//
// Paths are the app's own (~/.local/share/Flow/Flow/library.db,
// ~/.cache/Flow/Flow, settings in ~/.config/Flow/Flow.conf), so a download
// started here lands in the index the app shows — unless --library says
// otherwise. Results go to stdout, progress and diagnostics to stderr, so
// `flow-cli list | jq` stays clean.

#include "downloader.h"
#include "enricher.h"
#include "library.h"
#include "track.h"
#include "ytdlp.h"

#include <QAudioDevice>
#include <QMediaDevices>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include "settings.h"
#include <QStandardPaths>
#include <QTextStream>
#include <QTimer>

#include <csignal>
#include <cstdio>
#include <memory>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

// Progress is drawn in place only when stderr is a terminal.
bool stderrIsTty()
{
#ifdef _WIN32
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(STDERR_FILENO) != 0;
#endif
}

// How long nothing may happen before a command calls it a day. A download
// waits out the library's 2 s watcher debounce: a finished playlist becomes a
// mix inside the rescan that debounce triggers, so leaving earlier would drop
// it. Read-only commands only need the scan itself to be over.
constexpr int kDownloadSettleMs = 2500;
constexpr int kLibrarySettleMs = 400;
constexpr int kEnricherSettleMs = 500;

QTextStream &out()
{
    static QTextStream stream(stdout);
    return stream;
}

QTextStream &err()
{
    static QTextStream stream(stderr);
    return stream;
}

void say(const QString &text)
{
    out() << text << '\n';
    out().flush();
}

void note(const QString &text)
{
    err() << text << '\n';
    err().flush();
}

// Progress on stderr: one line rewritten in place on a terminal, one line per
// phase change when piped (so logs stay readable).
class Reporter
{
public:
    explicit Reporter(bool quiet)
        : m_quiet(quiet)
        , m_tty(stderrIsTty())
    {
    }

    ~Reporter() { clearLive(); }

    // A line that stays: a track finishing, a mix being made.
    void line(const QString &text)
    {
        if (m_quiet)
            return;
        clearLive();
        err() << text << '\n';
        err().flush();
    }

    // The status line. `key` identifies the phase, so a piped run prints one
    // line per ten percent instead of one per update.
    void live(const QString &line, const QString &key)
    {
        if (m_quiet)
            return;
        if (!m_tty) {
            if (key == m_lastKey)
                return;
            m_lastKey = key;
            err() << line << '\n';
            err().flush();
            return;
        }
        QString padded = line;
        if (padded.size() < m_liveSize)
            padded += QString(m_liveSize - padded.size(), u' ');
        m_liveSize = line.size();
        m_live = true;
        err() << u'\r' << padded;
        err().flush();
    }

    void clearLive()
    {
        if (!m_live)
            return;
        err() << QStringLiteral("\r\033[K");
        err().flush();
        m_live = false;
        m_liveSize = 0;
    }

private:
    bool m_quiet;
    bool m_tty;
    bool m_live = false;
    int m_liveSize = 0;
    QString m_lastKey;
};

struct Options
{
    QString libraryDir;
    QString ytDlp;
    bool json = false;
    bool quiet = false;
    bool enrich = false;
    bool playlist = false;
    bool song = false;
    int jobs = 0;
    QString search;
    QString format;
    QString sort;
    int limit = 0;
    bool tracks = false;
};

struct Session
{
    QString musicDir;
    QString dbPath;
    QString coverDir;
    QString tempDir;
    std::unique_ptr<Library> library;
    std::unique_ptr<Enricher> enricher;
    std::unique_ptr<Downloader> downloader;
};

QString musicDirFor(const Options &options)
{
    auto settings = flowSettings();   // the app's own Flow.conf
    QString dir = settings.value(QStringLiteral("library/dir")).toString();
    if (!options.libraryDir.isEmpty()) {
        dir = QDir(options.libraryDir).absolutePath();
        settings.setValue(QStringLiteral("library/dir"), dir);
    }
    if (dir.isEmpty())
        dir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);
    return QDir::cleanPath(dir);
}

Session openSession(const Options &options, bool withDownloader)
{
    Session s;
    s.musicDir = musicDirFor(options);
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    s.dbPath = dataDir + QStringLiteral("/library.db");
    s.coverDir = cacheDir + QStringLiteral("/covers");
    s.tempDir = cacheDir + QStringLiteral("/yt-dlp");
    QDir().mkpath(s.musicDir);
    QDir().mkpath(cacheDir);

    s.library = std::make_unique<Library>(s.musicDir, s.dbPath, s.coverDir);
    if (options.enrich) {
        s.enricher = std::make_unique<Enricher>(s.library.get());
        s.library->setEnricher(s.enricher.get());
    }
    if (withDownloader)
        s.downloader = std::make_unique<Downloader>(s.library.get(), s.tempDir, options.ytDlp);
    return s;
}

volatile std::sig_atomic_t g_signalled = 0;

void onSignal(int signo)
{
    g_signalled = signo;   // the settle timer picks this up; the loop then unwinds
}

// Waits until the downloader is idle, the scanner has stopped and the enricher
// has nothing queued, then until nothing has moved for `settleMs`.
// Returns 128 + signal (130 for SIGINT, 143 for SIGTERM) when interrupted.
int waitForQuiet(Session &s, Reporter &report, int settleMs)
{
    QElapsedTimer clock;
    clock.start();
    qint64 lastActivity = 0;
    const auto touch = [&] { lastActivity = clock.elapsed(); };
    Library *library = s.library.get();
    QObject context;   // the connections live exactly as long as this frame

    QObject::connect(library, &Library::scanningChanged, &context, touch);
    QObject::connect(library, &Library::countsChanged, &context, touch);
    QObject::connect(library, &Library::reloaded, &context, touch);
    QObject::connect(library, &Library::mixAdded, &context,
                     [touch](qint64, const QString &, int, int) { touch(); });
    if (s.downloader) {
        QObject::connect(s.downloader.get(), &Downloader::countChanged, &context, touch);
        QObject::connect(s.downloader.get(), &QAbstractItemModel::dataChanged, &context,
                         [touch](const QModelIndex &, const QModelIndex &, const QList<int> &) {
                             touch();
                         });
    }
    if (s.enricher)
        QObject::connect(s.enricher.get(), &Enricher::busyChanged, &context, touch);

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(120);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (g_signalled) {
            loop.quit();
            return;
        }
        const bool busy = (s.downloader && s.downloader->busy()) || library->scanning()
                          || (s.enricher && s.enricher->busy());
        if (!busy && clock.elapsed() - lastActivity > settleMs)
            loop.quit();
    });
    poll.start();
    loop.exec();
    poll.stop();
    report.clearLive();
    std::signal(SIGINT, SIG_DFL);
    std::signal(SIGTERM, SIG_DFL);
    return g_signalled ? 128 + int(g_signalled) : 0;
}

// ---------------------------------------------------------------- json

QVariantMap trackMap(const Track &t)
{
    return {
        {QStringLiteral("id"), t.id},
        {QStringLiteral("title"), t.title},
        {QStringLiteral("artist"), t.artist},
        {QStringLiteral("album"), t.album},
        {QStringLiteral("albumArtist"), t.albumArtist},
        {QStringLiteral("trackNo"), t.trackNo},
        {QStringLiteral("discNo"), t.discNo},
        {QStringLiteral("year"), t.year},
        {QStringLiteral("durationMs"), t.durationMs},
        {QStringLiteral("format"), t.format},
        {QStringLiteral("sampleRate"), t.sampleRate},
        {QStringLiteral("bitsPerSample"), t.bitsPerSample},
        {QStringLiteral("bitrate"), t.bitrate},
        {QStringLiteral("channels"), t.channels},
        {QStringLiteral("path"), t.path},
        {QStringLiteral("liked"), t.liked},
        {QStringLiteral("genre"), t.enriched.genre},
        {QStringLiteral("enriched"), t.isEnriched()},
        {QStringLiteral("rmsDb"), double(t.rmsDb)},
        {QStringLiteral("peakDb"), double(t.peakDb)},
    };
}

void printJson(const QVariantMap &map)
{
    const QJsonDocument doc(QJsonObject::fromVariantMap(map));
    out() << QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    out().flush();
}

QString toolVersion(const QString &program, const QString &argument)
{
    if (program.isEmpty())
        return {};
    QProcess process;
    process.start(program, QStringList{argument});
    if (!process.waitForFinished(4000)) {
        process.kill();
        process.waitForFinished(1000);
        return {};
    }
    const QString text = QString::fromUtf8(process.readAllStandardOutput());
    return text.section(u'\n', 0, 0).trimmed();
}

// ---------------------------------------------------------------- download

// A one-line job status: "Road trip 45% 3/12 · Three" (queued: "waiting").
QString jobLine(const QModelIndex &index, int percent)
{
    const QString state = index.data(Downloader::State).toString();
    const QString title = index.data(Downloader::Title).toString();
    const QString detail = index.data(Downloader::Detail).toString();
    if (state == u"queued")
        return title + QStringLiteral(" · waiting");
    return title + QStringLiteral(" · ") + QString::number(percent) + QStringLiteral("% · ")
           + detail;
}

int cmdDownload(const Options &options, const QStringList &urls, Reporter &report)
{
    Session s = openSession(options, true);
    Downloader *downloader = s.downloader.get();
    if (!downloader->available()) {
        note(QStringLiteral("%1 is not installed — Flow downloads with it")
                 .arg(downloader->missing()));
        return 1;
    }
    if (options.jobs > 0)
        downloader->setMaxConcurrent(options.jobs);

    // Queue everything first: one bad link must not half-start a run.
    QStringList rejected;
    for (const QString &url : urls) {
        const bool playlist = options.playlist || (!options.song && YtDlp::isPlaylistUrl(url));
        const QString error = downloader->add(url, playlist);
        if (!error.isEmpty())
            rejected.append(QStringLiteral("%1 — %2").arg(url, error));
    }
    for (const QString &line : rejected)
        note(QStringLiteral("not queued: ") + line);
    if (downloader->rowCount() == 0) {
        note(QStringLiteral("nothing to download"));
        return 2;
    }

    QList<QVariantMap> mixes;
    // The connections below are only wanted while this command runs, so they
    // hang off a frame-local object and go when it does.
    QObject context;
    QObject::connect(s.library.get(), &Library::mixAdded, &context,
                     [&mixes, &report](qint64 id, const QString &name, int added, int missing) {
                         mixes.append(QVariantMap{{QStringLiteral("id"), id},
                                                  {QStringLiteral("name"), name},
                                                  {QStringLiteral("tracks"), added},
                                                  {QStringLiteral("missing"), missing}});
                         if (id >= 0)
                             report.line(QStringLiteral("  mix “%1” · %2 %3")
                                             .arg(name)
                                             .arg(added)
                                             .arg(added == 1 ? QStringLiteral("song")
                                                             : QStringLiteral("songs")));
                     });
    if (s.enricher)
        QObject::connect(s.enricher.get(), &Enricher::enriched, &context,
                         [&report](qint64, bool ok, const QString &summary) {
                             report.line(QStringLiteral("  %1 %2")
                                             .arg(ok ? QStringLiteral("looked up")
                                                     : QStringLiteral("no match "))
                                             .arg(summary));
                         });

    // Per-track lines as they land, plus the live status line.
    QHash<int, QHash<int, QString>> seen;
    QHash<int, int> peak;   // jobId -> highest percent shown: a new track resets the
                            // model's progress, the report should not jump backwards
    const auto pump = [&] {
        QStringList active;
        QStringList keys;
        int finished = 0;
        for (int row = 0; row < downloader->rowCount(); ++row) {
            const QModelIndex index = downloader->index(row);
            const int jobId = index.data(Downloader::JobId).toInt();
            const QString state = index.data(Downloader::State).toString();
            const QVariantList items = downloader->jobItems(jobId);
            QHash<int, QString> &known = seen[jobId];
            for (const QVariant &value : items) {
                const QVariantMap item = value.toMap();
                const int position = item.value(QStringLiteral("index")).toInt();
                const QString itemState = item.value(QStringLiteral("state")).toString();
                if (known.value(position) == itemState)
                    continue;
                const bool first = !known.contains(position);
                known[position] = itemState;
                if (first || (itemState != u"done" && itemState != u"failed"))
                    continue;
                report.line(QStringLiteral("  %1 %2/%3  %4")
                                .arg(itemState == u"done" ? QStringLiteral("done")
                                                          : QStringLiteral("skip"))
                                .arg(position)
                                .arg(items.size())
                                .arg(item.value(QStringLiteral("title")).toString()));
            }
            if (state == u"queued" || state == u"running") {
                const int percent = qMax(peak.value(jobId, 0),
                                         int(index.data(Downloader::Progress).toDouble() * 100.0
                                             + 0.5));
                peak.insert(jobId, percent);
                active.append(jobLine(index, percent));
                keys.append(QString::number(jobId) + u':' + state + u':'
                            + QString::number(percent / 10) + u':'
                            + index.data(Downloader::Detail).toString());
            } else {
                ++finished;
            }
        }
        if (active.isEmpty())
            return;
        report.live(QStringLiteral("[%1 running · %2 done] %3")
                        .arg(active.size())
                        .arg(finished)
                        .arg(active.join(QStringLiteral(" | "))),
                    keys.join(u';'));
    };
    QObject::connect(downloader, &QAbstractItemModel::dataChanged, &context,
                     [&pump](const QModelIndex &, const QModelIndex &, const QList<int> &) { pump(); });
    pump();

    const int interrupted = waitForQuiet(s, report, kDownloadSettleMs);

    // Summary: one entry per queued job, then what became of the mix.
    QVariantList jobs;
    int songs = 0;
    int unavailable = 0;
    int failed = 0;
    for (int row = 0; row < downloader->rowCount(); ++row) {
        const QModelIndex index = downloader->index(row);
        const QString state = index.data(Downloader::State).toString();
        const QVariantList items = downloader->jobItems(index.data(Downloader::JobId).toInt());
        int jobSongs = 0;
        int jobMissing = 0;
        for (const QVariant &value : items) {
            const QString itemState = value.toMap().value(QStringLiteral("state")).toString();
            if (itemState == u"done")
                ++jobSongs;
            else if (itemState == u"failed")
                ++jobMissing;
        }
        songs += jobSongs;
        unavailable += jobMissing;
        if (state != u"done")
            ++failed;
        jobs.append(QVariantMap{
            {QStringLiteral("id"), index.data(Downloader::JobId)},
            {QStringLiteral("url"), index.data(Downloader::Url)},
            {QStringLiteral("title"), index.data(Downloader::Title)},
            {QStringLiteral("state"), state},
            {QStringLiteral("playlist"), index.data(Downloader::IsPlaylist)},
            {QStringLiteral("songs"), jobSongs},
            {QStringLiteral("unavailable"), jobMissing},
            {QStringLiteral("detail"), index.data(Downloader::Detail)},
            {QStringLiteral("items"), items},
        });
    }

    if (options.json) {
        QVariantList mixList;
        mixList.reserve(mixes.size());
        for (const QVariantMap &mix : mixes)
            mixList.append(mix);
        printJson({{QStringLiteral("ok"), failed == 0 && rejected.isEmpty()},
                   {QStringLiteral("interrupted"), interrupted != 0},
                   {QStringLiteral("music"), s.musicDir},
                   {QStringLiteral("songs"), songs},
                   {QStringLiteral("jobs"), jobs},
                   {QStringLiteral("mixes"), mixList}});
    } else {
        for (const QVariant &value : jobs) {
            const QVariantMap job = value.toMap();
            say(QStringLiteral("%1  %2  %3")
                    .arg(job.value(QStringLiteral("state")).toString(), -8)
                    .arg(job.value(QStringLiteral("title")).toString().left(40), -40)
                    .arg(job.value(QStringLiteral("detail")).toString()));
        }
        say(QStringLiteral("%1 %2 · %3 %4%5")
                .arg(jobs.size())
                .arg(jobs.size() == 1 ? QStringLiteral("link") : QStringLiteral("links"))
                .arg(songs)
                .arg(songs == 1 ? QStringLiteral("song") : QStringLiteral("songs"))
                .arg(unavailable > 0 ? QStringLiteral(" · %1 unavailable").arg(unavailable)
                                     : QString()));
    }

    if (interrupted)
        return interrupted;
    if (!rejected.isEmpty())
        return 2;
    return failed == 0 ? 0 : 1;
}

// ---------------------------------------------------------------- scan

int cmdScan(const Options &options, Reporter &report)
{
    Session s = openSession(options, false);
    Library *library = s.library.get();
    QObject context;   // the enricher connection below lives while this runs
    int interrupted = waitForQuiet(s, report, kLibrarySettleMs);
    if (interrupted == 0 && s.enricher) {
        QObject::connect(s.enricher.get(), &Enricher::enriched, &context,
                         [&report](qint64, bool ok, const QString &summary) {
                             report.line(QStringLiteral("  %1 %2")
                                             .arg(ok ? QStringLiteral("looked up")
                                                     : QStringLiteral("no match "))
                                             .arg(summary));
                         });
        // Everything the library shows and has not looked up yet.
        library->enrichShown();
        interrupted = waitForQuiet(s, report, kEnricherSettleMs);
    }

    const QString error = library->error();
    if (!error.isEmpty())
        note(QStringLiteral("library: ") + error);
    if (options.json) {
        QVariantMap map{{QStringLiteral("ok"), error.isEmpty() && interrupted == 0},
                        {QStringLiteral("music"), s.musicDir},
                        {QStringLiteral("db"), s.dbPath},
                        {QStringLiteral("tracks"), library->totalCount()},
                        {QStringLiteral("liked"), library->likedCount()},
                        {QStringLiteral("shown"), library->shownCount()},
                        {QStringLiteral("interrupted"), interrupted != 0},
                        {QStringLiteral("error"), error}};
        if (s.enricher)
            map.insert(QStringLiteral("lookedUp"), s.enricher->doneCount());
        printJson(map);
    } else {
        say(QStringLiteral("%1 tracks · %2 liked · %3")
                .arg(library->totalCount())
                .arg(library->likedCount())
                .arg(s.dbPath));
        if (s.enricher)
            say(QStringLiteral("looked up %1 tracks").arg(s.enricher->doneCount()));
    }
    if (interrupted)
        return interrupted;
    return error.isEmpty() ? 0 : 1;
}

// ---------------------------------------------------------------- list

int cmdList(const Options &options, Reporter &report)
{
    Session s = openSession(options, false);
    Library *library = s.library.get();
    const int interrupted = waitForQuiet(s, report, kLibrarySettleMs);
    if (interrupted)
        return interrupted;
    if (!options.search.isEmpty())
        library->setQuery(options.search);
    if (!options.format.isEmpty()) {
        static const QStringList formats{QStringLiteral("All"), QStringLiteral("FLAC"),
                                         QStringLiteral("MP3"), QStringLiteral("WAV"),
                                         QStringLiteral("Liked")};
        if (!formats.contains(options.format)) {
            note(QStringLiteral("--format takes ") + formats.join(QStringLiteral(", ")));
            return 2;
        }
        library->setFormat(options.format);
    }
    if (!options.sort.isEmpty()) {
        static const QStringList sorts{QStringLiteral("def"), QStringLiteral("title"),
                                       QStringLiteral("artist"), QStringLiteral("dur")};
        if (!sorts.contains(options.sort)) {
            note(QStringLiteral("--sort takes ") + sorts.join(QStringLiteral(", ")));
            return 2;
        }
        library->setSortMode(options.sort);
    }

    const QVariantList ids = library->visibleTrackIds();
    const int count = options.limit > 0 ? qMin<int>(options.limit, int(ids.size()))
                                        : int(ids.size());
    if (options.json) {
        QVariantList tracks;
        tracks.reserve(count);
        for (int i = 0; i < count; ++i) {
            const Track *track = library->trackById(ids.at(i).toLongLong());
            if (track)
                tracks.append(trackMap(*track));
        }
        printJson({{QStringLiteral("music"), s.musicDir},
                   {QStringLiteral("count"), count},
                   {QStringLiteral("tracks"), tracks}});
        return 0;
    }
    for (int i = 0; i < count; ++i) {
        const Track *track = library->trackById(ids.at(i).toLongLong());
        if (!track)
            continue;
        say(QStringLiteral("%1\t%2\t%3\t%4\t%5\t%6\t%7")
                .arg(i + 1)
                .arg(track->title, track->artist, track->album,
                     TrackFormat::duration(track->durationMs), track->format, track->path));
    }
    return 0;
}

// ---------------------------------------------------------------- mixes

int cmdMixes(const Options &options, Reporter &report)
{
    Session s = openSession(options, false);
    Library *library = s.library.get();
    const int interrupted = waitForQuiet(s, report, kLibrarySettleMs);
    if (interrupted)
        return interrupted;

    QAbstractItemModel *model = library->mixModel();
    QVariantList mixes;
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        const qint64 id = index.data(MixListModel::MixId).toLongLong();
        const QString name = index.data(MixListModel::Name).toString();
        QVariantList tracks;
        if (options.tracks || options.json) {
            for (const QVariant &trackId : library->mixTrackIds(id)) {
                if (const Track *track = library->trackById(trackId.toLongLong()))
                    tracks.append(trackMap(*track));
            }
        }
        mixes.append(QVariantMap{{QStringLiteral("id"), id},
                                 {QStringLiteral("name"), name},
                                 {QStringLiteral("count"),
                                  index.data(MixListModel::TrackCount)},
                                 {QStringLiteral("tracks"), tracks}});
    }

    if (options.json) {
        printJson({{QStringLiteral("count"), int(mixes.size())},
                   {QStringLiteral("mixes"), mixes}});
        return 0;
    }
    for (const QVariant &value : mixes) {
        const QVariantMap mix = value.toMap();
        say(QStringLiteral("%1\t%2\t%3")
                .arg(mix.value(QStringLiteral("count")).toInt())
                .arg(mix.value(QStringLiteral("name")).toString())
                .arg(mix.value(QStringLiteral("id")).toLongLong()));
        if (!options.tracks)
            continue;
        for (const QVariant &trackValue :
             mix.value(QStringLiteral("tracks")).toList()) {
            const QVariantMap track = trackValue.toMap();
            say(QStringLiteral("    %1\t%2")
                    .arg(track.value(QStringLiteral("title")).toString(),
                         track.value(QStringLiteral("artist")).toString()));
        }
    }
    return 0;
}

// ---------------------------------------------------------------- doctor

int cmdDoctor(const Options &options, Reporter &report)
{
    Session s = openSession(options, false);
    Library *library = s.library.get();
    const QString program = options.ytDlp.isEmpty()
                                ? QStandardPaths::findExecutable(QStringLiteral("yt-dlp"))
                                : options.ytDlp;
    const QString ffmpeg = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
    const QString ytDlpVersion = toolVersion(program, QStringLiteral("--version"));
    const QString ffmpegVersion = toolVersion(ffmpeg, QStringLiteral("-version"));

    const QList<QAudioDevice> audioOutputs = QMediaDevices::audioOutputs();
    const QAudioDevice defaultAudio = QMediaDevices::defaultAudioOutput();

    if (options.json) {
        QVariantList audioList;
        for (const auto &d : audioOutputs) {
            audioList.append(QVariantMap{
                {QStringLiteral("id"), QString::fromUtf8(d.id())},
                {QStringLiteral("name"), d.description()},
                {QStringLiteral("default"), d.isDefault() || d == defaultAudio}
            });
        }
        printJson({{QStringLiteral("music"), s.musicDir},
                   {QStringLiteral("db"), s.dbPath},
                   {QStringLiteral("covers"), s.coverDir},
                   {QStringLiteral("temp"), s.tempDir},
                   {QStringLiteral("tracks"), library->totalCount()},
                   {QStringLiteral("liked"), library->likedCount()},
                   {QStringLiteral("ytdlp"), program},
                   {QStringLiteral("ytdlpVersion"), ytDlpVersion},
                   {QStringLiteral("ffmpeg"), ffmpeg},
                   {QStringLiteral("ffmpegVersion"), ffmpegVersion},
                   {QStringLiteral("audioDevices"), audioList},
                   {QStringLiteral("audioDefault"), defaultAudio.description()}});
    } else {
        const auto describe = [](const QString &tool, const QString &version) {
            if (tool.isEmpty())
                return QStringLiteral("not found");
            return version.isEmpty() ? tool : tool + QStringLiteral("  ") + version;
        };
        say(QStringLiteral("library   %1").arg(s.musicDir));
        say(QStringLiteral("index     %1  (%2 tracks, %3 liked)")
                .arg(s.dbPath)
                .arg(library->totalCount())
                .arg(library->likedCount()));
        say(QStringLiteral("covers    %1").arg(s.coverDir));
        say(QStringLiteral("audio     %1  (%2 available)")
                .arg(defaultAudio.description().isEmpty() ? QStringLiteral("none") : defaultAudio.description())
                .arg(audioOutputs.size()));
        say(QStringLiteral("yt-dlp    %1").arg(describe(program, ytDlpVersion)));
        say(QStringLiteral("ffmpeg    %1").arg(describe(ffmpeg, ffmpegVersion)));
    }
    Q_UNUSED(report);
    return program.isEmpty() || ffmpeg.isEmpty() ? 1 : 0;
}

// ---------------------------------------------------------------- devices

int cmdDevices(const Options &options, Reporter &report)
{
    const QList<QAudioDevice> outputs = QMediaDevices::audioOutputs();
    const QAudioDevice def = QMediaDevices::defaultAudioOutput();

    if (options.json) {
        QVariantList list;
        for (const QAudioDevice &d : outputs) {
            list.append(QVariantMap{
                {QStringLiteral("id"), QString::fromUtf8(d.id())},
                {QStringLiteral("name"), d.description()},
                {QStringLiteral("default"), d.isDefault() || d == def}
            });
        }
        printJson({{QStringLiteral("count"), int(outputs.size())},
                   {QStringLiteral("default"), def.description()},
                   {QStringLiteral("devices"), list}});
    } else {
        if (outputs.isEmpty()) {
            say(QStringLiteral("no audio output devices found"));
        } else {
            for (const QAudioDevice &d : outputs) {
                const bool isDef = (d.isDefault() || d == def);
                const QString marker = isDef ? QStringLiteral("* ") : QStringLiteral("  ");
                const QString defTag = isDef ? QStringLiteral(" (default)") : QString();
                say(QStringLiteral("%1%2 %3%4")
                        .arg(marker)
                        .arg(QString::fromUtf8(d.id()), -44)
                        .arg(d.description())
                        .arg(defTag));
            }
        }
    }
    Q_UNUSED(report);
    return 0;
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Flow"));
    app.setOrganizationName(QStringLiteral("Flow"));
    app.setApplicationVersion(QStringLiteral(FLOW_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("Flow's headless front end: the app's own library index and "
                       "yt-dlp backend, no window."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"),
                                 QStringLiteral("download | scan | list | mixes | doctor"));
    parser.addPositionalArgument(QStringLiteral("arguments"),
                                 QStringLiteral("links for download, none for the rest"),
                                 QStringLiteral("[arguments…]"));

    const QCommandLineOption libraryOption(
        QStringLiteral("library"),
        QStringLiteral("Music folder to index and download into (remembered, like the app)."),
        QStringLiteral("path"));
    const QCommandLineOption ytDlpOption(
        QStringLiteral("yt-dlp"), QStringLiteral("yt-dlp binary to run."),
        QStringLiteral("path"));
    const QCommandLineOption jsonOption(QStringLiteral("json"),
                                        QStringLiteral("Print the result as JSON."));
    const QCommandLineOption quietOption(QStringLiteral("quiet"),
                                         QStringLiteral("No progress, only the result."));
    const QCommandLineOption enrichOption(
        QStringLiteral("enrich"),
        QStringLiteral("Download: look up the new tracks (MusicBrainz). "
                       "Scan: look up everything not looked up yet."));
    const QCommandLineOption playlistOption(QStringLiteral("playlist"),
                                            QStringLiteral("A watch link also takes its list."));
    const QCommandLineOption songOption(QStringLiteral("song"),
                                        QStringLiteral("A watch link takes only the video."));
    const QCommandLineOption jobsOption({QStringLiteral("j"), QStringLiteral("jobs")},
                                        QStringLiteral("Downloads at once (1-8, default 3)."),
                                        QStringLiteral("n"));
    const QCommandLineOption searchOption(QStringLiteral("search"),
                                          QStringLiteral("Only tracks matching this text."),
                                          QStringLiteral("text"));
    const QCommandLineOption formatOption(QStringLiteral("format"),
                                          QStringLiteral("All | FLAC | MP3 | WAV | Liked."),
                                          QStringLiteral("name"));
    const QCommandLineOption sortOption(QStringLiteral("sort"),
                                        QStringLiteral("def | title | artist | dur."),
                                        QStringLiteral("mode"));
    const QCommandLineOption limitOption(QStringLiteral("limit"),
                                         QStringLiteral("Stop after this many tracks."),
                                         QStringLiteral("n"));
    const QCommandLineOption tracksOption(QStringLiteral("tracks"),
                                          QStringLiteral("Also list each mix's tracks."));
    for (const QCommandLineOption &option :
         {libraryOption, ytDlpOption, jsonOption, quietOption, enrichOption, playlistOption,
          songOption, jobsOption, searchOption, formatOption, sortOption, limitOption,
          tracksOption})
        parser.addOption(option);
    parser.process(app);

    Options options;
    options.libraryDir = parser.value(libraryOption);
    options.ytDlp = parser.value(ytDlpOption);
    options.json = parser.isSet(jsonOption);
    options.quiet = parser.isSet(quietOption);
    options.enrich = parser.isSet(enrichOption);
    options.playlist = parser.isSet(playlistOption);
    options.song = parser.isSet(songOption);
    options.search = parser.value(searchOption);
    options.format = parser.value(formatOption);
    options.sort = parser.value(sortOption);
    options.tracks = parser.isSet(tracksOption);
    bool ok = false;
    options.jobs = parser.value(jobsOption).toInt(&ok);
    if (!ok)
        options.jobs = 0;
    options.limit = parser.value(limitOption).toInt();

    const QStringList arguments = parser.positionalArguments();
    if (arguments.isEmpty())
        parser.showHelp(2);   // prints the help, exits 2
    const QString command = arguments.first();
    const QStringList rest = arguments.mid(1);
    Reporter report(options.quiet);

    if (command == QLatin1String("download")) {
        if (rest.isEmpty()) {
            note(QStringLiteral("download needs at least one link"));
            return 2;
        }
        return cmdDownload(options, rest, report);
    }
    if (command == QLatin1String("scan"))
        return cmdScan(options, report);
    if (command == QLatin1String("list"))
        return cmdList(options, report);
    if (command == QLatin1String("mixes"))
        return cmdMixes(options, report);
    if (command == QLatin1String("doctor"))
        return cmdDoctor(options, report);
    if (command == QLatin1String("devices"))
        return cmdDevices(options, report);
    note(QStringLiteral("unknown command “%1” (download, scan, list, mixes, doctor, devices)")
             .arg(command));
    return 2;
}
