#pragma once

#include "mastering.h"

#include <QString>
#include <QStringList>

#include <vector>

// A sound profile is the tone stage the full-sound mixer runs: the equalizer
// curve for one output device. "house" is Flow's own tilt and the fallback
// when the device is not recognised; "flat" leaves the tone alone. The device
// profiles are corrections built from published measurements — see STACK.md
// for sources.
struct AudioProfile
{
    QString id;
    QString name;
    QString note;                  // one line, for the UI and docs
    QStringList match;             // all fragments must appear in the device name/id
    std::vector<Mastering::Band> bands;
    float preampDb = 0.f;
};

const std::vector<AudioProfile> &audioProfiles();
const AudioProfile *audioProfile(const QString &id);

// Resolves an output device to a profile id: the most specific name match
// wins, otherwise "house". Names come from the audio server ("JLab JBuds Lux",
// "SteelSeries Arctis Pro GameDAC", ...).
QString matchAudioProfile(const QString &deviceName, const QString &deviceId);
