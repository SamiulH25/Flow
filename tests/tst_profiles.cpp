#include "audioprofiles.h"

#include <QSet>
#include <QtTest>

class TestProfiles : public QObject
{
    Q_OBJECT

private slots:
    void deviceNamesResolveToProfiles()
    {
        // The most specific name wins: the GameDAC must not fall through to
        // the plain Arctis profile.
        QCOMPARE(matchAudioProfile(QStringLiteral("SteelSeries Arctis Pro GameDAC"), QString()),
                 QStringLiteral("gamedac"));
        QCOMPARE(matchAudioProfile(QStringLiteral("SteelSeries Arctis Pro"), QString()),
                 QStringLiteral("arctis"));
        QCOMPARE(matchAudioProfile(QStringLiteral("JLab JBuds Lux ANC"), QString()),
                 QStringLiteral("jbuds"));
        QCOMPARE(matchAudioProfile(QStringLiteral("Built-in Audio Analog Stereo"),
                                   QStringLiteral("alsa_output.pci-0000_00_1f.3.analog-stereo")),
                 QStringLiteral("house"));

        // Bluetooth sinks may carry the model in the id; matching is
        // case-insensitive and looks at both fields.
        QCOMPARE(matchAudioProfile(QString(), QStringLiteral("bluez_output.jlab_jbuds_lux")),
                 QStringLiteral("jbuds"));
        QCOMPARE(matchAudioProfile(QStringLiteral("steelseries arctis pro"), QString()),
                 QStringLiteral("arctis"));
    }

    void profileTableIsWellFormed()
    {
        QSet<QString> ids;
        for (const AudioProfile &profile : audioProfiles()) {
            QVERIFY(!profile.id.isEmpty());
            QVERIFY(!profile.name.isEmpty());
            QVERIFY(!profile.note.isEmpty());
            QVERIFY(!ids.contains(profile.id));
            ids.insert(profile.id);
            QVERIFY(profile.bands.size() <= 12);
            QVERIFY(profile.preampDb <= 0.f);   // corrections only attenuate
            for (const Mastering::Band &band : profile.bands) {
                QVERIFY(band.fc > 0.f && band.fc < 20000.f);
                QVERIFY(band.gainDb > -15.f && band.gainDb < 10.f);
                QVERIFY(band.q > 0.f);
            }
        }
        QVERIFY(audioProfile(QStringLiteral("house")));
        QVERIFY(audioProfile(QStringLiteral("flat")));
        QVERIFY(audioProfile(QStringLiteral("gamedac")));
        QVERIFY(audioProfile(QStringLiteral("arctis")));
        QVERIFY(audioProfile(QStringLiteral("jbuds")));
        QVERIFY(!audioProfile(QStringLiteral("nope")));
        QVERIFY(audioProfile(QStringLiteral("flat"))->bands.empty());
    }
};

QTEST_MAIN(TestProfiles)
#include "tst_profiles.moc"
