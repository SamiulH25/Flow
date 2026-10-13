#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>

// The stand-in yt-dlp that the download and CLI tests run: tests/fakeytdlp.cpp,
// built as `fake-ytdlp`. FAKE_YTDLP_BIN is its path. installFakeYtDlp copies it
// into a test's folder as yt-dlp (yt-dlp.exe on Windows), so the app finds it
// the way it finds the real one on PATH.
inline QString fakeYtDlpPath(const QString &dir)
{
    const QString suffix = QFileInfo(QStringLiteral(FAKE_YTDLP_BIN)).suffix();
    return dir + QStringLiteral("/yt-dlp") + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
}

inline bool installFakeYtDlp(const QString &dir)
{
    const QString dest = fakeYtDlpPath(dir);
    QFile::remove(dest);
    return QFile::copy(QStringLiteral(FAKE_YTDLP_BIN), dest)
        && QFile::setPermissions(dest, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}
