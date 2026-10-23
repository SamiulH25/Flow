#include "mixer.h"

#include <QAudioBuffer>
#include <QAudioFormat>
#include <QMediaDevices>
#include <QtTest>

// The mixer holds a second QAudioSink open; a broken lifecycle there is silent
// (the player's own output is muted), so these tests cover the states a track
// change puts it through. They need a real output device and skip without one.
class TestMixer : public QObject
{
    Q_OBJECT

private slots:
    void sinkReopensAfterFlush()
    {
        const QAudioDevice device = QMediaDevices::defaultAudioOutput();
        if (device.isNull())
            QSKIP("no audio output device");

        Mixer mixer;
        mixer.setDevice(device);
        mixer.setVolume(0.0f);   // silent: the test only watches the sink lifecycle
        mixer.setEnabled(true);

        const QAudioFormat format = Mixer::format();
        QByteArray pcm(format.bytesForFrames(4096), '\0');
        const QAudioBuffer buffer(pcm, format);

        for (int i = 0; i < 4; ++i)
            mixer.process(buffer);
        QTRY_VERIFY_WITH_TIMEOUT(mixer.running(), 3000);

        // A seek or a new track flushes. The next buffer must reopen the sink:
        // reset() alone would leave the backend stream stopped for good.
        mixer.flush();
        QVERIFY(!mixer.running());

        for (int i = 0; i < 4; ++i)
            mixer.process(buffer);
        QTRY_VERIFY_WITH_TIMEOUT(mixer.running(), 3000);
    }
};

QTEST_GUILESS_MAIN(TestMixer)
#include "tst_mixer.moc"
