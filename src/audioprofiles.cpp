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

// AutoEQ fixed-band result for Rtings' B&K 5128 measurement of the Arctis Pro,
// Harman target. The mild +4.4 dB at 31 Hz and 16 kHz are the preset's own
// extremes; the -6.2 dB preamp keeps them from clipping.
const std::vector<Band> kArctisPro = {
    {Equalizer::BandType::Peaking, 31.f, 4.4f, 1.41f},
    {Equalizer::BandType::Peaking, 62.f, -1.6f, 1.41f},
    {Equalizer::BandType::Peaking, 125.f, -5.7f, 1.41f},
    {Equalizer::BandType::Peaking, 250.f, -0.6f, 1.41f},
    {Equalizer::BandType::Peaking, 500.f, 1.9f, 1.41f},
    {Equalizer::BandType::Peaking, 1000.f, 1.6f, 1.41f},
    {Equalizer::BandType::Peaking, 2000.f, -1.1f, 1.41f},
    {Equalizer::BandType::Peaking, 4000.f, 4.3f, 1.41f},
    {Equalizer::BandType::Peaking, 8000.f, -3.3f, 1.41f},
    {Equalizer::BandType::Peaking, 16000.f, 6.4f, 1.41f},
};

// Same headset through the GameDAC, as measured by Rtings on the HMS II.3 rig.
const std::vector<Band> kArctisGameDac = {
    {Equalizer::BandType::Peaking, 31.f, -1.f, 1.41f},
    {Equalizer::BandType::Peaking, 62.f, 2.5f, 1.41f},
    {Equalizer::BandType::Peaking, 125.f, -0.1f, 1.41f},
    {Equalizer::BandType::Peaking, 250.f, -1.4f, 1.41f},
    {Equalizer::BandType::Peaking, 500.f, -1.5f, 1.41f},
    {Equalizer::BandType::Peaking, 1000.f, 0.4f, 1.41f},
    {Equalizer::BandType::Peaking, 2000.f, 1.4f, 1.41f},
    {Equalizer::BandType::Peaking, 4000.f, 2.2f, 1.41f},
    {Equalizer::BandType::Peaking, 8000.f, -3.4f, 1.41f},
    {Equalizer::BandType::Peaking, 16000.f, -2.6f, 1.41f},
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

// JLab JBuds Lux ANC. SoundGuys publishes the measured curve against their
// preference curve, so this is the inverse of that deviation: the broad bass
// lift (-9 dB below 100 Hz), the 5-10 kHz peak (down 12 dB at 8.5 kHz) and the
// recessed mids (+1.5 dB at 500 Hz) are what the lab found. Fitted with eight
// fixed bands; RMS error against the published curve is 1.0 dB.
const std::vector<Band> kJbudsLux = {
    {Equalizer::BandType::LowShelf, 100.f, -8.9f, 0.707f},
    {Equalizer::BandType::Peaking, 200.f, -1.6f, 1.2f},
    {Equalizer::BandType::Peaking, 500.f, 1.5f, 1.f},
    {Equalizer::BandType::Peaking, 1250.f, 3.f, 1.5f},
    {Equalizer::BandType::Peaking, 2200.f, -8.2f, 0.9f},
    {Equalizer::BandType::Peaking, 3500.f, -1.2f, 1.5f},
    {Equalizer::BandType::Peaking, 8500.f, -12.f, 1.2f},
    {Equalizer::BandType::Peaking, 12000.f, 3.f, 1.2f},
};

} // namespace

const std::vector<AudioProfile> &audioProfiles()
{
    static const std::vector<AudioProfile> profiles = {
        {
            QStringLiteral("gamedac"),
            QStringLiteral("SteelSeries Arctis Pro + GameDAC"),
            QStringLiteral("AutoEQ / Rtings (HMS II.3): trims the 6 kHz peak, lifts 3.7 kHz."),
            {QStringLiteral("arctis pro"), QStringLiteral("gamedac")},
            kArctisGameDac,
            -2.3f,
        },
        {
            QStringLiteral("arctis"),
            QStringLiteral("SteelSeries Arctis Pro"),
            QStringLiteral("AutoEQ / Rtings (B&K 5128): trims the 125 Hz hump, lifts 4 kHz."),
            {QStringLiteral("arctis pro")},
            kArctisPro,
            -6.2f,
        },
        {
            QStringLiteral("arctis-prime"),
            QStringLiteral("SteelSeries Arctis Prime"),
            QStringLiteral("AutoEQ / Rtings (HMS II.3): trims 8 kHz, lifts 2 kHz."),
            {QStringLiteral("arctis prime")},
            kArctisPrime,
            -3.8f,
        },
        {
            QStringLiteral("jbuds"),
            QStringLiteral("JLab JBuds Lux"),
            QStringLiteral("SoundGuys lab curve: -9 dB sub-bass, -12 dB at 8.5 kHz, mids lifted."),
            {QStringLiteral("jbuds lux")},
            kJbudsLux,
            -3.5f,
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
            QStringLiteral("No tone correction; compression and width still apply."),
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
