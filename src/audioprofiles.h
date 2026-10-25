#pragma once

#include "equalizer.h"

#include <QString>
#include <QStringList>

#include <vector>

// A sound profile is the EQ curve the mixer applies for one output device.
// "flat" is the fallback when the device is not recognised; "house" is Flow's
// own tilt and stays available as a manual pick. The device profiles are
// corrections built from published measurements — see STACK.md for sources.
struct AudioProfile
{
    QString id;
    QString name;
    QString note;                  // one line, for the UI and docs
    QStringList match;             // all fragments must appear in the device name/id
    std::vector<Equalizer::Band> bands;
    float preampDb = 0.f;
};

const std::vector<AudioProfile> &audioProfiles();
const AudioProfile *audioProfile(const QString &id);

// Resolves an output device to a profile id: the most specific name match
// wins, otherwise "house". Names come from the audio server ("JLab JBuds Lux",
// "SteelSeries Arctis Pro GameDAC", ...).
QString matchAudioProfile(const QString &deviceName, const QString &deviceId);
