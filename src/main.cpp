#include "library.h"
#include "mpris.h"
#include "player.h"

#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QSettings>
#include <QStandardPaths>
#include <QQmlError>
#include <QQuickWindow>
#include <QTimer>
#include <QDebug>

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Flow"));
    app.setOrganizationName(QStringLiteral("Flow"));

    // Music folder: --library=PATH (remembered), else the last one, else ~/Music.
    QSettings settings;
    QString musicDir = settings.value(QStringLiteral("library/dir")).toString();
    for (const QString &arg : app.arguments()) {
        if (arg.startsWith(QStringLiteral("--library="))) {
            musicDir = QDir(arg.mid(QStringLiteral("--library=").size())).absolutePath();
            settings.setValue(QStringLiteral("library/dir"), musicDir);
        }
    }
    if (musicDir.isEmpty())
        musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);

    // Declared before the engine so QML is torn down first.
    Library library(musicDir, dataDir + QStringLiteral("/library.db"),
                    cacheDir + QStringLiteral("/covers"));
    Player player(&library);
    Library::setInstance(&library);
    Player::setInstance(&player);

    QQmlApplicationEngine engine;
    QObject::connect(
        &engine, &QQmlApplicationEngine::warnings,
        &app, [](const QList<QQmlError> &warnings) {
            for (const QQmlError &w : warnings)
                qWarning().noquote() << w.toString();
        });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed,
        &app, []() { QCoreApplication::exit(1); }, Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("Flow"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty())
        return 1;

    Mpris mpris(&player, &library);
    mpris.registerOnBus();
    if (auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first())) {
        QObject::connect(&mpris, &Mpris::raiseRequested, window, [window] {
            window->show();
            window->raise();
            window->requestActivate();
        });
    }

    // Dev-only: --screenshot=path grabs the window after layout settles, then quits.
    // Works headless (offscreen platform) for visual checks without a display server.
    QString shotPath;
    for (const QString &arg : app.arguments()) {
        if (arg.startsWith(QStringLiteral("--screenshot=")))
            shotPath = arg.mid(QStringLiteral("--screenshot=").size());
    }
    if (!shotPath.isEmpty()) {
        auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        if (!window) {
            qWarning() << "flow: --screenshot needs a QQuickWindow root";
            return 1;
        }
        QTimer::singleShot(1500, &app, [window, shotPath]() {
            const QImage img = window->grabWindow();
            if (img.isNull() || !img.save(shotPath))
                qWarning() << "flow: screenshot failed";
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
