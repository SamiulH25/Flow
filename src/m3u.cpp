#include "m3u.h"

#include <QDir>
#include <QRegularExpression>
#include <QStringDecoder>
#include <QUrl>

namespace M3u {

QStringList parse(const QString &text, const QString &baseDir)
{
    QStringList out;
    const QDir base(baseDir);
    const QStringList lines = text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
    for (QString line : lines) {
        line = line.trimmed();
        if (line.startsWith(QChar(0xFEFF)))
            line.remove(0, 1);
        if (line.isEmpty() || line.startsWith(u'#'))
            continue;

        QString path;
        if (line.startsWith(QStringLiteral("file:"), Qt::CaseInsensitive)) {
            const QUrl url(line);
            if (!url.isLocalFile())
                continue;
            path = url.toLocalFile();
        } else if (line.contains(QStringLiteral("://"))) {
            continue;   // streams and other remote entries
        } else {
            // Playlists written on Windows use backslashes.
            path = QString(line).replace(u'\\', u'/');
            if (QDir::isRelativePath(path))
                path = base.filePath(path);
        }
        out.append(QDir::cleanPath(path));
    }
    return out;
}

QString decode(const QByteArray &bytes)
{
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(bytes);
    if (utf8.hasError())
        text = QString::fromLatin1(bytes);
    return text;
}

QByteArray write(const QList<Track> &tracks)
{
    QString text = QStringLiteral("#EXTM3U\n");
    for (const Track &t : tracks) {
        const qint64 seconds = t.durationMs > 0 ? t.durationMs / 1000 : -1;
        // Titles cannot carry line breaks inside an #EXTINF line.
        QString label = t.artist + QStringLiteral(" - ") + t.title;
        label.replace(u'\n', u' ').replace(u'\r', u' ');
        text += QStringLiteral("#EXTINF:%1,%2\n").arg(seconds).arg(label);
        text += t.path + u'\n';
    }
    return text.toUtf8();
}

} // namespace M3u
