#pragma once

#include <QImage>
#include <QString>
#include <QThread>

#include <atomic>
#include <memory>

// Generates the filmstrip images for a stretch of the media off the UI
// thread: frame grabs for video, waveform slices for audio. The stretch is
// the whole file normally, or the zoom window.
class ThumbWorker : public QThread {
    Q_OBJECT

public:
    enum Kind { Frames, Waveform };

    ThumbWorker(QString path, double startSec, double lenSec, int count, Kind kind = Frames,
                QObject *parent = nullptr)
        : QThread(parent), m_path(std::move(path)), m_start(startSec), m_len(lenSec),
          m_count(count), m_kind(kind) {}

    void requestStop();

signals:
    void thumbReady(int index, const QImage &image);

protected:
    void run() override;

private:
    void runWaveform(const std::atomic<bool> *cancel);

    QString m_path;
    double m_start;
    double m_len;
    int m_count;
    Kind m_kind;
    std::shared_ptr<std::atomic<bool>> m_cancel = std::make_shared<std::atomic<bool>>(false);
};
