// Stand-in for yt-dlp in the tests: speaks Flow's yt-dlp protocol ("flow-*"
// lines) for a few made-up links, writes tagged WAV files from FLOW_FAKE_DIR
// and fails on cue. No network, and it runs the same on Linux and Windows.
//
// Behaviour by link (the first match wins):
//   v=song          one song, two progress lines, exit 0
//   list=PL…        a playlist: one track, one unavailable video, exit 1
//   v=bad           an error, exit 1
//   v=slow          one item, then sleeps 30 s (for cancel tests)
// Every run first writes its arguments, one per line, to FLOW_FAKE_DIR/args.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QStringList>

#include <chrono>
#include <cstdio>
#include <thread>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

// Lines end in '\n' on every platform (text mode would add '\r' on Windows).
void write(const QString &text, bool err = false, bool newline = true)
{
    QByteArray bytes = text.toUtf8();
    if (newline)
        bytes.append('\n');
    std::FILE *stream = err ? stderr : stdout;
    std::fwrite(bytes.constData(), 1, size_t(bytes.size()), stream);
    std::fflush(stream);
}

void say(const QString &line, bool err = false) { write(line, err); }

void copyOver(const QString &from, const QString &to)
{
    QFile::remove(to);
    QFile::copy(from, to);
}

QString home(const QStringList &args)
{
    for (const QString &a : args) {
        if (a.startsWith(QStringLiteral("home:")))
            return a.mid(5);
    }
    return {};
}

} // namespace

int main(int argc, char **argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif

    QStringList args;
    for (int i = 1; i < argc; ++i)
        args << QString::fromLocal8Bit(argv[i]);

    const QString fakeDir = QString::fromLocal8Bit(qgetenv("FLOW_FAKE_DIR"));
    QFile argsFile(fakeDir + QStringLiteral("/args"));
    if (argsFile.open(QIODevice::WriteOnly)) {
        for (const QString &a : args)
            argsFile.write(a.toUtf8() + '\n');
    }

    const QString out = home(args);
    const QString url = args.isEmpty() ? QString() : args.constLast();

    if (url.contains(QStringLiteral("v=song"))) {
        QDir().mkpath(out + QStringLiteral("/Artist/Singles"));
        say(QStringLiteral("flow-item {\"title\": \"Song (Official Video)\", \"track\": \"One\"}"));
        say(QStringLiteral("flow-progress 50 100"));
        say(QStringLiteral("flow-progress 100 NA"));
        const QString file = out + QStringLiteral("/Artist/Singles/One.wav");
        copyOver(fakeDir + QStringLiteral("/one.wav"), file);
        say(QStringLiteral("flow-file \"%1\"").arg(file));
        return 0;
    }
    if (url.contains(QStringLiteral("list=PL"))) {
        QDir().mkpath(out + QStringLiteral("/Artist/Mixed"));
        say(QStringLiteral("flow-item {\"title\": \"Two\", \"playlist_index\": 1, \"n_entries\": 3, \"playlist_title\": \"Road trip\"}"));
        const QString two = out + QStringLiteral("/Artist/Mixed/Two.wav");
        copyOver(fakeDir + QStringLiteral("/two.wav"), two);
        say(QStringLiteral("flow-file \"%1\"").arg(two));
        say(QStringLiteral("ERROR: [youtube] gone: Video unavailable"), true);
        say(QStringLiteral("flow-item {\"title\": \"Three\", \"playlist_index\": 3, \"n_entries\": 3, \"playlist_title\": \"Road trip\"}"));
        const QString three = out + QStringLiteral("/Artist/Mixed/Three.wav");
        copyOver(fakeDir + QStringLiteral("/three.wav"), three);
        // No trailing newline on the last line, as the shell version printed it.
        write(QStringLiteral("flow-file \"%1\"").arg(three), false, false);
        return 1;
    }
    if (url.contains(QStringLiteral("v=bad"))) {
        say(QStringLiteral("ERROR: [youtube] bad: Video unavailable. This video is private"), true);
        return 1;
    }
    if (url.contains(QStringLiteral("v=slow"))) {
        say(QStringLiteral("flow-item {\"title\": \"Slow\"}"));
        std::this_thread::sleep_for(std::chrono::seconds(30));
        return 0;
    }
    return 0;
}
