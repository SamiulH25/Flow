#include "audioprofiles.h"

#include <algorithm>

using Band = Equalizer::Band;

namespace {

// Flow's own tilt: weight below 100 Hz, presence around 3 kHz, a little air.
// It is selectable manually; Auto never picks it.
const std::vector<Band> kHouse = {
    {Equalizer::BandType::LowShelf, 100.f, 4.f, 0.707f},
    {Equalizer::BandType::Peaking, 3200.f, 1.8f, 0.8f},
    {Equalizer::BandType::HighShelf, 11000.f, 1.f, 0.707f},
};

// r/steelseries community preset for the Arctis Pro + GameDAC (the thread's
// "better fit" reply): a bass- and mid-forward tuning rather than a measured
// correction. Values are the SteelSeries Engine's 10 bands; preamp -8.5 dB
// covers the summed +8.2 dB peak.
const std::vector<Band> kArctisPro = {
    {Equalizer::BandType::Peaking, 31.f, 7.1f, 1.41f},
    {Equalizer::BandType::Peaking, 62.f, 5.f, 1.41f},
    {Equalizer::BandType::Peaking, 125.f, 2.f, 1.41f},
    {Equalizer::BandType::Peaking, 250.f, 4.f, 1.41f},
    {Equalizer::BandType::Peaking, 500.f, 4.5f, 1.41f},
    {Equalizer::BandType::Peaking, 1000.f, 4.5f, 1.41f},
    {Equalizer::BandType::Peaking, 2000.f, 2.5f, 1.41f},
    {Equalizer::BandType::Peaking, 4000.f, 1.5f, 1.41f},
    {Equalizer::BandType::Peaking, 8000.f, 1.f, 1.41f},
    {Equalizer::BandType::Peaking, 16000.f, 0.5f, 1.41f},
};

// SteelSeries Arctis Prime, AutoEQ fixed-band result from Rtings' HMS II.3
// measurement, Harman target.
const std::vector<Band> kArctisPrime = {
    {Equalizer::BandType::Peaking, 31.f, 3.1f, 1.41f},
    {Equalizer::BandType::Peaking, 62.f, 3.f, 1.41f},
    {Equalizer::BandType::Peaking, 125.f, 0.3f, 1.41f},
    {Equalizer::BandType::Peaking, 250.f, -1.6f, 1.41f},
    {Equalizer::BandType::Peaking, 500.f, -0.3f, 1.41f},
    {Equalizer::BandType::Peaking, 1000.f, 1.6f, 1.41f},
    {Equalizer::BandType::Peaking, 2000.f, 3.7f, 1.41f},
    {Equalizer::BandType::Peaking, 4000.f, 0.2f, 1.41f},
    {Equalizer::BandType::Peaking, 8000.f, -7.2f, 1.41f},
    {Equalizer::BandType::Peaking, 16000.f, -4.2f, 1.41f},
};

// JLab JBuds Lux ANC, from the r/JLab "best EQ for clarity" screenshot: the
// app's 10 sliders read as a smiley — +3.6 dB bass, a small 250 Hz dip and the
// 4-16 kHz sliders at maximum. The app does not label its scale; the positions
// are mapped to ±6 dB, the range its other EQ controls use. Preamp -8 dB
// covers the summed +7.7 dB peak.
const std::vector<Band> kJbudsLux = {
    {Equalizer::BandType::Peaking, 31.f, 3.6f, 1.41f},
    {Equalizer::BandType::Peaking, 62.f, 3.6f, 1.41f},
    {Equalizer::BandType::Peaking, 125.f, 2.4f, 1.41f},
    {Equalizer::BandType::Peaking, 250.f, -1.3f, 1.41f},
    {Equalizer::BandType::Peaking, 1000.f, 1.1f, 1.41f},
    {Equalizer::BandType::Peaking, 2000.f, 3.6f, 1.41f},
    {Equalizer::BandType::Peaking, 4000.f, 6.f, 1.41f},
    {Equalizer::BandType::Peaking, 8000.f, 6.f, 1.41f},
    {Equalizer::BandType::Peaking, 16000.f, 6.f, 1.41f},
};

} // namespace

const std::vector<AudioProfile> &audioProfiles()
{
    static const std::vector<AudioProfile> profiles = {
        {
            QStringLiteral("arctis"),
            QStringLiteral("SteelSeries Arctis Pro"),
            QStringLiteral("r/steelseries preset: +7 dB sub-bass, +4.5 dB mids, gentle treble."),
            {QStringLiteral("arctis pro")},
            kArctisPro,
            -8.5f,
        },
        {
            QStringLiteral("arctis-prime"),
            QStringLiteral("SteelSeries Arctis Prime"),
            QStringLiteral("AutoEQ / Rtings (HMS II.3) — Reddit has no preset for the Prime."),
            {QStringLiteral("arctis prime")},
            kArctisPrime,
            -3.8f,
        },
        {
            QStringLiteral("jbuds"),
            QStringLiteral("JLab JBuds Lux"),
            QStringLiteral("r/JLab preset: bass lift, dipped 250 Hz, 4-16 kHz at maximum."),
            {QStringLiteral("jbuds lux")},
            kJbudsLux,
            -8.f,
        },
        {
            QStringLiteral("house"),
            QStringLiteral("House"),
            QStringLiteral("Flow's default tilt: weight below 100 Hz, presence, air."),
            {},
            kHouse,
            -4.f,
        },
        {
            QStringLiteral("flat"),
            QStringLiteral("Flat"),
            QStringLiteral("No tone correction: the raw output."),
            {},
            {},
            0.f,
        },
    };
    return profiles;
}

const AudioProfile *audioProfile(const QString &id)
{
    for (const AudioProfile &profile : audioProfiles()) {
        if (profile.id == id)
            return &profile;
    }
    return nullptr;
}

QString matchAudioProfile(const QString &deviceName, const QString &deviceId)
{
    QString haystack = QStringLiteral("%1 %2").arg(deviceName, deviceId).toLower();
    // Audio servers use "_", "-" and "." in ids; read them as word breaks so
    // "jbuds_lux" still matches the "jbuds lux" fragment.
    for (QChar &c : haystack) {
        if (!c.isLetterOrNumber())
            c = QLatin1Char(' ');
    }
    for (const AudioProfile &profile : audioProfiles()) {
        if (profile.match.isEmpty())
            continue;
        const bool all = std::all_of(profile.match.cbegin(), profile.match.cend(),
                                     [&haystack](const QString &fragment) {
                                         return haystack.contains(fragment);
                                     });
        if (all)
            return profile.id;
    }
    return QStringLiteral("flat");
}
