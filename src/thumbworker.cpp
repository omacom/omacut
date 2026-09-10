#include "thumbworker.h"

#include "ffmpeg.h"

#include <algorithm>
#include <deque>
#include <future>

namespace {
constexpr int kMaxThumbJobs = 4;
constexpr int kFrameHeight = 90;
// Waveform slices are rendered tall enough for the big preview; the trim bar
// scales them down.
constexpr int kWaveformSliceWidth = 320;
constexpr int kWaveformHeight = 240;
}

void ThumbWorker::requestStop() {
    m_cancel->store(true, std::memory_order_relaxed);
    QThread::requestInterruption();
}

void ThumbWorker::run() {
    if (m_count <= 0)
        return;

    const auto cancel = m_cancel;
    if (cancel->load(std::memory_order_relaxed))
        return;

    if (m_kind == Waveform) {
        runWaveform(cancel.get());
        return;
    }

    const int idealThreads = std::max(1, QThread::idealThreadCount());
    const int maxJobs = std::min({m_count, kMaxThumbJobs, idealThreads});

    std::deque<std::future<QImage>> jobs;
    int nextIndex = 0;
    int emitIndex = 0;

    // Each slot covers an equal slice of the stretch; a frame grab takes its
    // midpoint.
    const auto startNextJob = [this, &jobs, &nextIndex, cancel] {
        const int index = nextIndex++;
        const double time = m_start + m_len * (index + 0.5) / m_count;
        const QString path = m_path;
        jobs.push_back(std::async(std::launch::async, [path, time, cancel] {
            return ffmpeg::thumbnail(path, time, kFrameHeight, cancel.get());
        }));
    };

    while (nextIndex < m_count && static_cast<int>(jobs.size()) < maxJobs)
        startNextJob();

    while (!jobs.empty()) {
        if (isInterruptionRequested()) {
            cancel->store(true, std::memory_order_relaxed);
            return;
        }

        QImage image = jobs.front().get();
        jobs.pop_front();

        if (isInterruptionRequested()) {
            cancel->store(true, std::memory_order_relaxed);
            return;
        }

        emit thumbReady(emitIndex++, image);

        if (nextIndex < m_count)
            startNextJob();
    }
}

// The whole stretch is rendered as one picture and cut into slots. Rendering
// each slot separately would seek once per slot, and lossy codecs come back
// from a seek with a few silent milliseconds — a visible seam at every slot
// edge. One seek keeps the wave continuous, and a single decode pass is still
// only seconds for hours of audio.
void ThumbWorker::runWaveform(const std::atomic<bool> *cancel) {
    const int width = kWaveformSliceWidth * m_count;
    const QImage strip = ffmpeg::waveform(m_path, m_start, m_len, width, kWaveformHeight, cancel);

    if (isInterruptionRequested() || cancel->load(std::memory_order_relaxed))
        return;

    for (int index = 0; index < m_count; ++index) {
        // A failed render still fills every slot (copies of a null image are
        // null), so the strip completes empty rather than hanging at "Loading...".
        emit thumbReady(index, strip.copy(index * kWaveformSliceWidth, 0,
                                          kWaveformSliceWidth, kWaveformHeight));
    }
}
