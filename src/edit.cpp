#include "edit.h"

#include <algorithm>
#include <cmath>

namespace edit {

const QStringList &effects() {
    static const QStringList list = {QStringLiteral("none"), QStringLiteral("fade"),
                                     QStringLiteral("slide"), QStringLiteral("pop")};
    return list;
}

namespace {
double rampLength(const TextOverlay &text) {
    return std::max(std::min(effectSeconds, text.length() / 2), 0.001);
}
}

double effectProgress(const TextOverlay &text, double t) {
    const double p = std::clamp((t - text.start) / rampLength(text), 0.0, 1.0);
    return 1.0 - (1.0 - p) * (1.0 - p);
}

double effectAlpha(const TextOverlay &text, double t) {
    if (t < text.start || t > text.end)
        return 0.0;
    if (text.effect == QLatin1String("none"))
        return 1.0;
    const double ramp = rampLength(text);
    return std::clamp(std::min((t - text.start) / ramp, (text.end - t) / ramp), 0.0, 1.0);
}

double duration(const Clips &clips) {
    double total = 0.0;
    for (const Clip &clip : clips)
        total += clip.length();
    return total;
}

bool continues(const Clip &clip, const Clip &next) {
    return clip.source == next.source && std::abs(clip.out - next.in) <= 0.001;
}

Clips merged(const Clips &clips) {
    Clips result;
    for (const Clip &clip : clips) {
        if (!result.isEmpty() && continues(result.last(), clip))
            result.last().out = clip.out;
        else
            result.append(clip);
    }
    return result;
}

bool untouched(const Clips &clips, double firstSourceDuration) {
    return merged(clips) == Clips{{0, 0.0, firstSourceDuration}};
}

}  // namespace edit
