#include "edit.h"

namespace edit {

double duration(const Clips &clips) {
    double total = 0.0;
    for (const Clip &clip : clips)
        total += clip.length();
    return total;
}

Clips merged(const Clips &clips) {
    Clips result;
    for (const Clip &clip : clips) {
        if (!result.isEmpty() && result.last().source == clip.source && result.last().out == clip.in
            && result.last().audio == clip.audio)
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
