// Waveform overview: real decode of generated WAVs with known loudness.
#include "waveform.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <functional>

namespace {

// 16-bit mono WAV; sample(i, n) returns -1..1 for frame i of n.
bool writeWav(const QString &path, int rate, int seconds, const std::function<double(int, int)> &sample)
{
    const int n = rate * seconds;
    QByteArray h;
    const auto u32 = [&h](quint32 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&h](quint16 v) { v = qToLittleEndian(v); h.append(reinterpret_cast<const char *>(&v), 2); };
    h.append("RIFF");
    u32(36 + quint32(n) * 2);
    h.append("WAVEfmt ");
    u32(16);
    u16(1);
    u16(1);
    u32(quint32(rate));
    u32(quint32(rate) * 2);
    u16(2);
    u16(16);
    h.append("data");
    u32(quint32(n) * 2);
    QByteArray data(n * 2, '\0');
    for (int i = 0; i < n; ++i)
        qToLittleEndian(qint16(qBound(-32767.0, sample(i, n) * 32767.0, 32767.0)), data.data() + i * 2);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(h);
    f.write(data);
    return true;
}

QByteArray decode(Waveform &w, qint64 id, const QString &path)
{
    QSignalSpy ready(&w, &Waveform::ready);
    QSignalSpy failed(&w, &Waveform::failed);
    w.request(id, path);
    if (!QTest::qWaitFor([&] { return !ready.isEmpty() || !failed.isEmpty(); }, 10000))
        return {};
    if (ready.isEmpty())
        return {};
    if (ready.first().at(0).toLongLong() != id)
        return {};
    return ready.first().at(1).toByteArray();
}

} // namespace

class TestWaveform : public QObject
{
    Q_OBJECT
    QTemporaryDir m_tmp;

private slots:
    void barsFromChunks()
    {
        QList<double> sums(192, 0.0);
        QList<int> frames(192, 100);
        for (int i = 96; i < 192; ++i)
            sums[i] = 100 * 0.25;   // RMS 0.5 in the second half
        const QByteArray bars = Waveform::toBars(sums, frames);
        QCOMPARE(bars.size(), Waveform::kBars);
        QCOMPARE(quint8(bars.at(0)), 0);
        QCOMPARE(quint8(bars.at(47)), 0);
        QCOMPARE(quint8(bars.at(48)), 255);   // normalised to the loudest bar
        QCOMPARE(quint8(bars.at(95)), 255);

        // Fewer chunks than bars: every bar still gets a value.
        const QByteArray few = Waveform::toBars({4.0, 1.0}, {1, 1});
        QCOMPARE(quint8(few.at(0)), 255);
        QCOMPARE(quint8(few.at(95)), 128);   // sqrt(1)/sqrt(4) = 0.5
        QCOMPARE(Waveform::toBars({}, {}), QByteArray(Waveform::kBars, '\0'));
    }

    void silenceThenLoud()
    {
        const QString path = m_tmp.filePath(QStringLiteral("half.wav"));
        QVERIFY(writeWav(path, 44100, 4, [](int i, int n) {
            return i < n / 2 ? 0.0 : ((i / 50) % 2 ? 0.8 : -0.8);   // square wave in the 2nd half
        }));
        Waveform w;
        const QByteArray bars = decode(w, 7, path);
        QCOMPARE(bars.size(), Waveform::kBars);
        for (int b = 0; b < 46; ++b)
            QVERIFY2(quint8(bars.at(b)) < 10, qPrintable(QString::number(b)));
        for (int b = 50; b < 96; ++b)
            QVERIFY2(quint8(bars.at(b)) > 200, qPrintable(QString::number(b)));
    }

    void rampRises()
    {
        const QString path = m_tmp.filePath(QStringLiteral("ramp.wav"));
        QVERIFY(writeWav(path, 22050, 3, [](int i, int n) {
            const double amp = double(i) / n;
            return (i / 20) % 2 ? amp : -amp;
        }));
        Waveform w;
        const QByteArray bars = decode(w, 8, path);
        QCOMPARE(bars.size(), Waveform::kBars);
        QVERIFY(quint8(bars.at(5)) < quint8(bars.at(30)));
        QVERIFY(quint8(bars.at(30)) < quint8(bars.at(60)));
        QVERIFY(quint8(bars.at(60)) < quint8(bars.at(95)));
        QVERIFY(quint8(bars.at(95)) > 240);
    }

    void newRequestReplacesOld()
    {
        const QString a = m_tmp.filePath(QStringLiteral("half.wav"));
        const QString b = m_tmp.filePath(QStringLiteral("ramp.wav"));
        Waveform w;
        QSignalSpy ready(&w, &Waveform::ready);
        w.request(1, a);
        w.request(2, b);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
        QTest::qWait(300);
        for (const auto &args : ready)
            QCOMPARE(args.at(0).toLongLong(), 2);   // the abandoned track never reports
    }

    void unreadableFails()
    {
        const QString path = m_tmp.filePath(QStringLiteral("junk.wav"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("not audio at all");
        f.close();
        Waveform w;
        QSignalSpy failed(&w, &Waveform::failed);
        w.request(9, path);
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 10000);
        QCOMPARE(failed.first().at(0).toLongLong(), 9);
    }
};

QTEST_GUILESS_MAIN(TestWaveform)
#include "tst_waveform.moc"
