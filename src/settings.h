#pragma once

#include <QSettings>

// Flow's settings: one INI file per user on every platform, never the Windows
// registry (QSettings' native default there). Linux: ~/.config/Flow/Flow.conf,
// Windows: %APPDATA%/Flow/Flow.ini. Tests move it with QSettings::setPath.
inline QSettings flowSettings()
{
    return QSettings(QSettings::IniFormat, QSettings::UserScope,
                     QStringLiteral("Flow"), QStringLiteral("Flow"));
}
