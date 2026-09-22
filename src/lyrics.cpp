#include "lyrics.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringDecoder>

#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>

#include <algorithm>

namespace {

QString readSidecar(const QString &audioPath)
{
    const QFileInfo info(audioPath);
    QFile f(info.absolutePath() + u'/' + info.completeBaseName() + QStringLiteral(".lrc"));
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const QByteArray raw = f.read(1 << 20);   // lyrics files are tiny; cap junk
    QStringDecoder utf8(QStringDecoder::Utf8, QStringDecoder::Flag::Stateless);
    QString text = utf8.decode(raw);
    if (utf8.hasError())
        text = QString::fromLatin1(raw);       // old .lrc files are often Latin-1
    return text;
}

QString readEmbedded(const QString &audioPath)
{
    TagLib::FileRef f(QFile::encodeName(audioPath).constData(), false);
    if (f.isNull())
        return {};
    const TagLib::PropertyMap props = f.properties();
    const auto it = props.find("LYRICS");
    if (it == props.end() || it->second.isEmpty())
        return {};
    return QString::fromStdString(it->second.front().to8Bit(true));
}

} // namespace

Lyrics Lyrics::load(const QString &audioPath)
{
    QString text = readSidecar(audioPath);
    if (text.trimmed().isEmpty())
        text = readEmbedded(audioPath);
    return parse(text);
}

Lyrics Lyrics::parse(const QString &text)
{
    // [mm:ss], [mm:ss.x], [mm:ss.xx], [mm:ss.xxx], also with ':' before the fraction
    static const QRegularExpression stamp(
        QStringLiteral(R"(\[(\d{1,3}):(\d{1,2})(?:[.:](\d{1,3}))?\])"));

    Lyrics out;
    QList<Line> untimed;
    const QStringList rows = text.split(QRegularExpression(QStringLiteral("\r\n|\r|\n")));
    for (const QString &row : rows) {
        QList<qint64> times;
        qsizetype pos = 0;
        QRegularExpressionMatch m;
        // Leading stamps only: "[00:12.00][01:30.00]chorus line"
        while ((m = stamp.match(row, pos, QRegularExpression::NormalMatch,
                                QRegularExpression::AnchorAtOffsetMatchOption)).hasMatch()) {
            const QString frac = m.captured(3);
            qint64 ms = (m.captured(1).toLongLong() * 60 + m.captured(2).toLongLong()) * 1000;
            if (!frac.isEmpty())
                ms += frac.toLongLong() * (frac.size() == 1 ? 100 : frac.size() == 2 ? 10 : 1);
            times.append(ms);
            pos = m.capturedEnd();
        }
        const QString body = row.mid(pos).trimmed();
        if (!times.isEmpty()) {
            for (qint64 ms : times)
                out.lines.append({ms, body});
        } else if (!(row.startsWith(u'[') && row.trimmed().endsWith(u']'))) {
            // Skip LRC metadata such as [ar:Artist]; keep everything else.
            untimed.append({-1, row.trimmed()});
        }
    }

    if (!out.lines.isEmpty()) {
        std::stable_sort(out.lines.begin(), out.lines.end(),
                         [](const Line &a, const Line &b) { return a.ms < b.ms; });
        out.timed = true;
        return out;
    }

    // Plain text: drop leading/trailing blank lines, keep inner stanza breaks.
    while (!untimed.isEmpty() && untimed.first().text.isEmpty())
        untimed.removeFirst();
    while (!untimed.isEmpty() && untimed.last().text.isEmpty())
        untimed.removeLast();
    out.lines = untimed;
    return out;
}

int Lyrics::indexAt(qint64 ms) const
{
    if (!timed || lines.isEmpty())
        return -1;
    const auto it = std::upper_bound(lines.cbegin(), lines.cend(), ms,
                                     [](qint64 v, const Line &l) { return v < l.ms; });
    return static_cast<int>(it - lines.cbegin()) - 1;
}
