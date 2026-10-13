#pragma once

#include <QFile>
#include <QString>

#include <string>

// TagLib opens files with its native path type: wide on Windows (a narrow path
// there goes through the ANSI code page, so accented folder names fail), UTF-8
// bytes elsewhere. Keep the returned string alive while the TagLib object exists.
#ifdef _WIN32
using TagPath = std::wstring;
inline TagPath tagPath(const QString &path) { return path.toStdWString(); }
#else
using TagPath = std::string;
inline TagPath tagPath(const QString &path) { return QFile::encodeName(path).toStdString(); }
#endif
