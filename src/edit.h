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
    double length() const { return out - in; }
    bool operator==(const Clip &other) const {
        return source == other.source && in == other.in && out == other.out;
    }
};
using Clips = QList<Clip>;

constexpr double minimumClip = 0.1;

double duration(const Clips &clips);
// True when next picks up where clip left off in the same source. A millisecond
// covers the float noise of a split. A larger gap is a cut and must stay one.
bool continues(const Clip &clip, const Clip &next);
// Neighbours that carry on where the one before left off in the same source,
// merged: what an export encodes.
Clips merged(const Clips &clips);
// Nothing cut, added or reordered: all of the first source, as it is.
bool untouched(const Clips &clips, double firstSourceDuration);

}  // namespace edit
