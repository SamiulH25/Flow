#include "downloader.h"
#include "enricher.h"
#include "library.h"
#include "mpris.h"
#include "player.h"

#include <QCommandLineParser>
#include <QDir>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QIcon>
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
    app.setApplicationVersion(QStringLiteral(FLOW_VERSION));
    // Wayland app_id / X11 class: matches flow.desktop and MPRIS DesktopEntry.
    app.setDesktopFileName(QStringLiteral("flow"));
    app.setWindowIcon(QIcon::fromTheme(QStringLiteral("flow"), QIcon(QStringLiteral(":/data/flow.svg"))));

    // Bundled UI fonts: Inter (body), Space Grotesk (display), JetBrains Mono (labels).
    for (const char *font : {":/data/fonts/Inter.ttf", ":/data/fonts/SpaceGrotesk.ttf",
                             ":/data/fonts/JetBrainsMono.ttf"}) {
        if (QFontDatabase::addApplicationFont(QString::fromLatin1(font)) < 0)
            qWarning() << "flow: could not load font" << font;
    }
    QFont body(QStringLiteral("Inter"));
    body.setPixelSize(13);
    app.setFont(body);

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("Flow — a local music player for FLAC, MP3 and WAV."));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption libraryOption(
        QStringLiteral("library"),
        QStringLiteral("Music folder to index and play (remembered for next time)."),
        QStringLiteral("path"));
    cli.addOption(libraryOption);
    // Dev-only: grab the window after layout settles, then quit. Works headless
    // (QT_QPA_PLATFORM=offscreen) for visual checks without a display server.
    QCommandLineOption screenshotOption(QStringLiteral("screenshot"),
                                       QStringLiteral("Save a screenshot and quit."),
                                       QStringLiteral("file"));
    screenshotOption.setFlags(QCommandLineOption::HiddenFromHelp);
    cli.addOption(screenshotOption);
    cli.process(app);   // exits on --help, --version or unknown options

    // Music folder: --library (remembered), else the last one, else ~/Music.
    QSettings settings;
    QString musicDir = settings.value(QStringLiteral("library/dir")).toString();
    if (cli.isSet(libraryOption)) {
        musicDir = QDir(cli.value(libraryOption)).absolutePath();
        settings.setValue(QStringLiteral("library/dir"), musicDir);
    }
    // Also changeable in the app ("Local library" card), which Library saves.
    if (musicDir.isEmpty())
        musicDir = QStandardPaths::writableLocation(QStandardPaths::MusicLocation);

    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString cacheDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);

    // Declared before the engine so QML is torn down first.
    Library library(musicDir, dataDir + QStringLiteral("/library.db"),
                    cacheDir + QStringLiteral("/covers"));
    Player player(&library);
    Downloader downloader(&library, cacheDir + QStringLiteral("/yt-dlp"));
    Enricher enricher(&library);
    library.setEnricher(&enricher);
    Library::setInstance(&library);
    Player::setInstance(&player);
    Downloader::setInstance(&downloader);
    Enricher::setInstance(&enricher);

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

    const QString shotPath = cli.value(screenshotOption);
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
