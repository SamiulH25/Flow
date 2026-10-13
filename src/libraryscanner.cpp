#include "libraryscanner.h"

#include "librarydb.h"
#include "tagpath.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/flacproperties.h>
#include <taglib/tag.h>
#include <taglib/tdebuglistener.h>
#include <taglib/tpropertymap.h>
#include <taglib/wavproperties.h>

namespace {

constexpr int kBatchSize = 200;

// TagLib prints parser complaints about damaged files to stderr; a scan of a
// real-world library would flood the log. Failures are counted instead.
class QuietTagLib : public TagLib::DebugListener
{
public:
    void printMessage(const TagLib::String &) override {}
};

void silenceTagLib()
{
    static QuietTagLib listener;
    static const bool once = (TagLib::setDebugListener(&listener), true);
    Q_UNUSED(once);
}

QString toQString(const TagLib::String &s)
{
    return QString::fromStdString(s.to8Bit(true)).trimmed();
}

// Set for the duration of one scan() so folderCover() answers are memoised;
// null outside a scan (see folderCover).
thread_local QHash<QString, QString> *g_folderCoverCache = nullptr;

QString firstProperty(const TagLib::PropertyMap &props, const char *key)
{
    const auto it = props.find(key);
    if (it == props.end() || it->second.isEmpty())
        return {};
    return toQString(it->second.front());
}

// "3/12" -> 3
int leadingNumber(const QString &s)
{
    return s.section(u'/', 0, 0).trimmed().toInt();
}

QString formatForSuffix(const QString &suffix)
{
    const QString s = suffix.toLower();
    if (s == u"flac")
        return QStringLiteral("FLAC");
    if (s == u"mp3")
        return QStringLiteral("MP3");
    if (s == u"wav")
        return QStringLiteral("WAV");
    return {};
}

// Writes embedded art to coverDir/<sha1>.<ext>; identical art is stored once.
QString extractEmbeddedCover(const TagLib::FileRef &f, const QString &coverDir)
{
    const auto pictures = f.complexProperties("PICTURE");
    if (pictures.isEmpty())
        return {};

    const TagLib::VariantMap *chosen = &pictures.front();
    for (const auto &pic : pictures) {
        if (pic.value("pictureType").toString() == "Front Cover") {
            chosen = &pic;
            break;
        }
    }
    const TagLib::ByteVector bytes = chosen->value("data").toByteVector();
    if (bytes.isEmpty())
        return {};
    const QByteArray data(bytes.data(), static_cast<qsizetype>(bytes.size()));
    const QString mime = toQString(chosen->value("mimeType").toString()).toLower();
    const QString ext = mime.contains(u"png") ? QStringLiteral("png") : QStringLiteral("jpg");

    const QString name = QString::fromLatin1(
        QCryptographicHash::hash(data, QCryptographicHash::Sha1).toHex());
    const QString out = coverDir + u'/' + name + u'.' + ext;
    if (QFileInfo::exists(out))
        return out;

    QDir().mkpath(coverDir);
    QFile file(out + QStringLiteral(".part"));
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size()) {
        file.remove();
        return {};
    }
    file.close();
    // rename() is atomic, so a concurrently reading UI never sees half a file.
    if (!file.rename(out)) {
        file.remove();
        return QFileInfo::exists(out) ? out : QString();
    }
    return out;
}

QString folderCover(const QString &dir)
{
    // QDir name filters match case-insensitively by default.
    static const QStringList names = {
        QStringLiteral("cover.jpg"),  QStringLiteral("cover.jpeg"), QStringLiteral("cover.png"),
        QStringLiteral("folder.jpg"), QStringLiteral("folder.jpeg"), QStringLiteral("folder.png"),
        QStringLiteral("front.jpg"),  QStringLiteral("front.png"),  QStringLiteral("albumart.jpg"),
    };
    // A folder's cover is asked for once per track, so an album costs one
    // directory listing per track for the same answer. While a scan runs the
    // answers are memoised (see scan()); outside a scan the lookup is unchanged,
    // so a cover added between scans is always picked up.
    if (g_folderCoverCache) {
        const auto cached = g_folderCoverCache->constFind(dir);
        if (cached != g_folderCoverCache->constEnd())
            return *cached;
    }
    const QStringList found = QDir(dir).entryList(names, QDir::Files, QDir::Name);
    QString result;
    // Honour the preference order above rather than alphabetical order.
    for (const QString &want : names) {
        for (const QString &have : found) {
            if (have.compare(want, Qt::CaseInsensitive) == 0) {
                result = dir + u'/' + have;
                break;
            }
        }
        if (!result.isEmpty())
            break;
    }
    if (g_folderCoverCache)
        g_folderCoverCache->insert(dir, result);
    return result;
}

} // namespace

LibraryScanner::LibraryScanner(QString dbPath, QString coverDir)
    : m_dbPath(std::move(dbPath))
    , m_coverDir(std::move(coverDir))
{
}

bool LibraryScanner::isAudioFile(const QString &path)
{
    return !formatForSuffix(QFileInfo(path).suffix()).isEmpty();
}

bool LibraryScanner::readTrack(const QString &path, const QString &coverDir, Track *out)
{
    silenceTagLib();
    const QFileInfo info(path);
    const TagPath name = tagPath(info.absoluteFilePath());
    TagLib::FileRef f(name.c_str(), true,
                      TagLib::AudioProperties::Average);
    if (f.isNull())
        return false;

    Track t;
    t.path = info.absoluteFilePath();
    t.mtime = info.lastModified().toMSecsSinceEpoch();
    t.size = info.size();
    t.format = formatForSuffix(info.suffix());

    if (const TagLib::Tag *tag = f.tag()) {
        t.title = toQString(tag->title());
        t.artist = toQString(tag->artist());
        t.album = toQString(tag->album());
        t.year = static_cast<int>(tag->year());
        t.trackNo = static_cast<int>(tag->track());
    }
    const TagLib::PropertyMap props = f.properties();
    t.albumArtist = firstProperty(props, "ALBUMARTIST");
    t.discNo = leadingNumber(firstProperty(props, "DISCNUMBER"));

    if (t.title.isEmpty())
        t.title = info.completeBaseName();
    if (t.artist.isEmpty())
        t.artist = t.albumArtist.isEmpty() ? QStringLiteral("Unknown Artist") : t.albumArtist;
    if (t.album.isEmpty())
        t.album = QStringLiteral("Unknown Album");

    // TagLib opens damaged or mislabelled files without finding any audio;
    // those are not tracks.
    const TagLib::AudioProperties *ap = f.audioProperties();
    if (!ap || ap->sampleRate() <= 0)
        return false;
    t.durationMs = ap->lengthInMilliseconds();
    t.bitrate = ap->bitrate();
    t.sampleRate = ap->sampleRate();
    t.channels = ap->channels();
    if (auto *flac = dynamic_cast<const TagLib::FLAC::Properties *>(ap))
        t.bitsPerSample = flac->bitsPerSample();
    else if (auto *wav = dynamic_cast<const TagLib::RIFF::WAV::Properties *>(ap))
        t.bitsPerSample = wav->bitsPerSample();

    t.coverPath = extractEmbeddedCover(f, coverDir);
    if (t.coverPath.isEmpty())
        t.coverPath = folderCover(info.absolutePath());

    *out = t;
    return true;
}

LibraryScanner::Result LibraryScanner::scan(const QString &root,
                                            const std::atomic_bool *cancel,
                                            const std::function<void(int)> &progress)
{
    Result r;
    const QFileInfo rootInfo(root);
    // A missing root (e.g. an unmounted drive) must not wipe the index and
    // everyone's likes, so bail out before touching the database.
    if (!rootInfo.isDir()) {
        r.error = QStringLiteral("music folder not found: %1").arg(root);
        return r;
    }

    LibraryDb db;
    if (!db.open(m_dbPath)) {
        r.error = db.lastError();
        return r;
    }

    QHash<QString, LibraryDb::Stamp> known = db.stamps();
    QSet<QString> seen;

    // Memoise folder-cover lookups for the duration of this walk: an album of
    // 20 tracks in one folder used to mean 20 identical directory listings.
    QHash<QString, QString> folderCovers;
    struct CoverCacheScope {
        explicit CoverCacheScope(QHash<QString, QString> &cache) { g_folderCoverCache = &cache; }
        ~CoverCacheScope() { g_folderCoverCache = nullptr; }
    } coverCacheScope(folderCovers);
    Q_UNUSED(coverCacheScope);

    const auto cancelled = [cancel] { return cancel && cancel->load(); };

    r.dirs.append(rootInfo.absoluteFilePath());
    int pending = 0;
    if (!db.transaction()) {
        r.error = db.lastError();
        return r;
    }

    QDirIterator it(rootInfo.absoluteFilePath(), QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (cancelled())
            break;
        const QFileInfo info = it.nextFileInfo();
        if (info.isDir()) {
            r.dirs.append(info.absoluteFilePath());
            continue;
        }
        if (!isAudioFile(info.fileName()))
            continue;

        const QString path = info.absoluteFilePath();
        seen.insert(path);
        ++r.scanned;
        if (progress && r.scanned % 50 == 0)
            progress(r.scanned);

        const auto stamp = known.constFind(path);
        if (stamp != known.cend() && stamp->mtime == info.lastModified().toMSecsSinceEpoch()
            && stamp->size == info.size())
            continue;

        Track t;
        if (!readTrack(path, m_coverDir, &t)) {
            ++r.failed;
            continue;
        }
        if (!db.upsert(t)) {
            r.error = db.lastError();
            db.commit();
            return r;
        }
        ++r.updated;
        if (++pending >= kBatchSize) {
            if (!db.commit() || !db.transaction()) {
                r.error = db.lastError();
                return r;
            }
            pending = 0;
        }
    }

    // Only prune after a complete walk; a cancelled scan saw a partial tree.
    if (!cancelled()) {
        for (auto k = known.cbegin(); k != known.cend(); ++k) {
            if (seen.contains(k.key()))
                continue;
            // Files outside the current root stay too: the root may have moved
            // back and forth and their likes are worth keeping.
            if (!k.key().startsWith(rootInfo.absoluteFilePath() + u'/'))
                continue;
            if (!db.remove(k.key())) {
                r.error = db.lastError();
                db.commit();
                return r;
            }
            ++r.removed;
        }
    }

    if (!db.commit()) {
        r.error = db.lastError();
        return r;
    }
    if (progress)
        progress(r.scanned);
    r.ok = !cancelled();
    return r;
}
