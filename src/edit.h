#pragma once

#include <QList>
#include <QString>
#include <QStringList>

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

// A text drawn over the finished video from start to end, seconds along the
// edit. It lives on its own track: cutting or moving clips doesn't move it.
// size is a fraction of the frame's height; y places the text from the top
// of the frame (0) to the bottom (1), so the whole of it stays inside.
struct TextOverlay {
    QString text;
    double start = 0.0;
    double end = 0.0;
    // How it comes and goes: "none", "fade", "slide" (rises as it fades in)
    // or "pop" (grows as it fades in). The animated ones also fade out.
    QString effect = QStringLiteral("none");
    double y = 0.94;
    double size = 0.08;
    // A font family, as fontconfig and Qt both know it.
    QString font = QStringLiteral("Sans");
    QString color = QStringLiteral("#ffffff");
    // A dark box behind the text, for readability over busy footage.
    bool box = false;
    double length() const { return end - start; }
    bool operator==(const TextOverlay &other) const {
        return text == other.text && start == other.start && end == other.end
            && effect == other.effect && y == other.y && size == other.size
            && font == other.font
            && color == other.color && box == other.box;
    }
};
using Overlays = QList<TextOverlay>;

constexpr double minimumText = 0.1;
// How long an effect takes to come in, and to go out, at most.
constexpr double effectSeconds = 0.4;
// The effects, in the order they are offered.
const QStringList &effects();
// How far in (0 to 1) an effect has come at time t: the first stretch of the
// text, never more than half of it, eased out.
double effectProgress(const TextOverlay &text, double t);
// How opaque the text is at time t: 0 outside of [start, end], and for the
// animated effects ramping in and out. Preview and export share it.
double effectAlpha(const TextOverlay &text, double t);

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
