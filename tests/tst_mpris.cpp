// MPRIS over a real bus. ctest runs this under dbus-run-session so it never
// touches the desktop session. A second connection plays the client role;
// calls are async because the service answers from this same event loop.
#include "library.h"
#include "mpris.h"
#include "player.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

namespace {

const QString kService = QStringLiteral("org.mpris.MediaPlayer2.flow");
const QString kPath = QStringLiteral("/org/mpris/MediaPlayer2");
const QString kPlayer = QStringLiteral("org.mpris.MediaPlayer2.Player");

bool writeWav(const QString &path, int seconds)
{
    const quint32 rate = 44100, blockAlign = 4;
    const quint32 dataSize = rate * blockAlign * quint32(seconds);
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + dataSize);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(2);
    u32(rate);
    u32(rate * blockAlign);
    u16(quint16(blockAlign));
    u16(16);
    h.append("data");
    u32(dataSize);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h);
    f.write(QByteArray(dataSize, '\0'));
    return true;
}

} // namespace

class TestMpris : public QObject
{
    Q_OBJECT

    QTemporaryDir m_tmp;
    std::unique_ptr<Library> m_lib;
    std::unique_ptr<Player> m_player;
    std::unique_ptr<Mpris> m_mpris;
    QDBusConnection m_client{QString()};

    QDBusMessage call(const QString &iface, const QString &method, const QVariantList &args = {})
    {
        QDBusMessage msg = QDBusMessage::createMethodCall(kService, kPath, iface, method);
        msg.setArguments(args);
        QDBusPendingCall pending = m_client.asyncCall(msg);
        if (!QTest::qWaitFor([&] { return pending.isFinished(); }, 5000))
            return QDBusMessage::createError(QStringLiteral("test.Timeout"), method);
        return pending.reply();
    }

    QVariant get(const QString &iface, const QString &prop)
    {
        const QDBusMessage reply = call(QStringLiteral("org.freedesktop.DBus.Properties"),
                                        QStringLiteral("Get"), {iface, prop});
        if (reply.type() != QDBusMessage::ReplyMessage || reply.arguments().isEmpty())
            return {};
        return reply.arguments().first().value<QDBusVariant>().variant();
    }

    void set(const QString &iface, const QString &prop, const QVariant &value)
    {
        const QDBusMessage reply = call(QStringLiteral("org.freedesktop.DBus.Properties"),
                                        QStringLiteral("Set"),
                                        {iface, prop, QVariant::fromValue(QDBusVariant(value))});
        QCOMPARE(reply.type(), QDBusMessage::ReplyMessage);
    }

private slots:
    void initTestCase()
    {
        if (!QDBusConnection::sessionBus().isConnected())
            QSKIP("no session bus (run via ctest, which wraps this in dbus-run-session)");
        QVERIFY(m_tmp.isValid());
        qputenv("XDG_CONFIG_HOME", m_tmp.filePath(QStringLiteral("config")).toUtf8());
        const QString music = m_tmp.filePath(QStringLiteral("music"));
        QVERIFY(QDir().mkpath(music));
        QVERIFY(writeWav(music + QStringLiteral("/a.wav"), 3));
        QVERIFY(writeWav(music + QStringLiteral("/b.wav"), 3));

        m_lib = std::make_unique<Library>(music, m_tmp.filePath(QStringLiteral("library.db")),
                                          m_tmp.filePath(QStringLiteral("covers")));
        QTRY_VERIFY_WITH_TIMEOUT(!m_lib->scanning(), 10000);
        m_lib->setSortMode(QStringLiteral("title"));
        m_player = std::make_unique<Player>(m_lib.get());
        m_player->setShuffle(false);
        m_player->setRepeat(false);
        m_mpris = std::make_unique<Mpris>(m_player.get(), m_lib.get());
        QVERIFY(m_mpris->registerOnBus());
        QCOMPARE(m_mpris->serviceName(), kService);

        m_client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("client"));
        QVERIFY(m_client.isConnected());
    }

    void rootInterface()
    {
        QCOMPARE(get(QStringLiteral("org.mpris.MediaPlayer2"), QStringLiteral("Identity")).toString(),
                 QStringLiteral("Flow"));
        QCOMPARE(get(QStringLiteral("org.mpris.MediaPlayer2"), QStringLiteral("CanRaise")).toBool(), true);
    }

    void playAndMetadata()
    {
        QCOMPARE(get(kPlayer, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Stopped"));
        QVERIFY(get(kPlayer, QStringLiteral("CanPlay")).toBool());
        call(kPlayer, QStringLiteral("Play"));
        QTRY_VERIFY_WITH_TIMEOUT(m_player->playing(), 5000);
        QCOMPARE(get(kPlayer, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Playing"));

        const QVariant meta = get(kPlayer, QStringLiteral("Metadata"));
        const QVariantMap map = qdbus_cast<QVariantMap>(meta);
        QCOMPARE(map.value(QStringLiteral("xesam:title")).toString(), QStringLiteral("a"));
        QCOMPARE(map.value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path(),
                 QStringLiteral("/org/mpris/MediaPlayer2/Track/%1").arg(m_player->currentId()));
        QVERIFY(map.value(QStringLiteral("mpris:length")).toLongLong() > 2'900'000);
    }

    void seekAndSetPosition()
    {
        call(kPlayer, QStringLiteral("Seek"), {qlonglong(1'000'000)});
        QTRY_VERIFY(m_player->position() >= 0.9);
        const QString track = QStringLiteral("/org/mpris/MediaPlayer2/Track/%1").arg(m_player->currentId());
        call(kPlayer, QStringLiteral("SetPosition"),
             {QVariant::fromValue(QDBusObjectPath(track)), qlonglong(2'000'000)});
        QTRY_VERIFY(m_player->position() >= 1.9);
        // A stale track id is ignored.
        call(kPlayer, QStringLiteral("SetPosition"),
             {QVariant::fromValue(QDBusObjectPath(QStringLiteral("/org/mpris/MediaPlayer2/Track/999999"))),
              qlonglong(0)});
        QTest::qWait(200);
        QVERIFY(m_player->position() >= 1.9);
    }

    void transportAndProperties()
    {
        m_player->jump(0);
        QTRY_VERIFY_WITH_TIMEOUT(m_player->playing(), 5000);
        call(kPlayer, QStringLiteral("Next"));
        QCOMPARE(m_player->current().value(QStringLiteral("t")).toString(), QStringLiteral("b"));
        QTRY_VERIFY_WITH_TIMEOUT(m_player->playing(), 5000);

        call(kPlayer, QStringLiteral("PlayPause"));
        QTRY_VERIFY(m_player->paused());
        QCOMPARE(get(kPlayer, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Paused"));
        call(kPlayer, QStringLiteral("PlayPause"));
        QTRY_VERIFY(m_player->playing());

        set(kPlayer, QStringLiteral("Volume"), 0.25);
        QCOMPARE(m_player->volume(), 0.25);
        set(kPlayer, QStringLiteral("LoopStatus"), QStringLiteral("Track"));
        QCOMPARE(m_player->repeatMode(), int(Player::RepeatOne));
        QCOMPARE(get(kPlayer, QStringLiteral("LoopStatus")).toString(), QStringLiteral("Track"));
        set(kPlayer, QStringLiteral("LoopStatus"), QStringLiteral("Playlist"));
        QCOMPARE(m_player->repeatMode(), int(Player::RepeatAll));
        set(kPlayer, QStringLiteral("Shuffle"), true);
        QVERIFY(m_player->shuffle());
        set(kPlayer, QStringLiteral("Shuffle"), false);

        call(kPlayer, QStringLiteral("Stop"));
        QTRY_VERIFY(!m_player->playing());
        QCOMPARE(get(kPlayer, QStringLiteral("PlaybackStatus")).toString(), QStringLiteral("Stopped"));
        call(kPlayer, QStringLiteral("PlayPause"));   // resumes from stopped
        QTRY_VERIFY_WITH_TIMEOUT(m_player->playing(), 5000);
        call(kPlayer, QStringLiteral("Stop"));
        QTRY_VERIFY(!m_player->playing());
    }

    void propertiesChangedIsEmitted()
    {
        QSignalSpy spy(this, &TestMpris::propertiesChanged);
        QVERIFY(m_client.connect(kService, kPath, QStringLiteral("org.freedesktop.DBus.Properties"),
                                 QStringLiteral("PropertiesChanged"), this,
                                 SIGNAL(propertiesChanged(QString, QVariantMap, QStringList))));
        m_player->play();
        QTRY_VERIFY_WITH_TIMEOUT(!spy.isEmpty(), 5000);
        bool sawStatus = false;
        for (const auto &args : spy) {
            if (args.at(0).toString() == kPlayer
                && args.at(1).toMap().value(QStringLiteral("PlaybackStatus")) == QStringLiteral("Playing"))
                sawStatus = true;
        }
        QVERIFY(sawStatus);
    }

    void cleanupTestCase()
    {
        m_mpris.reset();
        m_player.reset();
        m_lib.reset();
    }

signals:
    void propertiesChanged(const QString &iface, const QVariantMap &changed, const QStringList &invalidated);
};

QTEST_GUILESS_MAIN(TestMpris)
#include "tst_mpris.moc"
