#include "edit.h"

#include <cmath>

namespace edit {

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
