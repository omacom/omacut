#pragma once

#include <QList>

// What gets exported: an ordered list of clips, each a stretch of one of the
// loaded videos, played back to back. The videos are never modified; a clip's
// in and out are seconds in its source.
namespace edit {

struct Clip {
    int source = 0;
    double in = 0.0;
    double out = 0.0;
    // False drops this clip's audio on playback and in the export.
    bool audio = true;
    double length() const { return out - in; }
    bool operator==(const Clip &other) const {
        return source == other.source && in == other.in && out == other.out && audio == other.audio;
    }
};
using Clips = QList<Clip>;

constexpr double minimumClip = 0.1;

double duration(const Clips &clips);
// Neighbours that carry on where the one before left off in the same source,
// merged: what an export encodes.
Clips merged(const Clips &clips);
// Nothing cut, added or reordered: all of the first source, as it is.
bool untouched(const Clips &clips, double firstSourceDuration);

}  // namespace edit
