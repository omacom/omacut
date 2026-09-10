#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    // Pick a video or audio file to trim.
    virtual void openVideo() = 0;
    // scaleHeights are the downscale choices to offer besides "Original"
    // (e.g. {1080, 720}), matched by min(width, height) of the source.
    virtual void exportVideo(const QUrl &suggestedUrl, double start, double end,
                             const QList<int> &scaleHeights) = 0;
    // Audio exports are always MP3, so there is nothing to choose but the name.
    virtual void exportAudio(const QUrl &suggestedUrl, double start, double end) = 0;

signals:
    void openSelected(const QUrl &url);
    // scaleHeight is 0 for "Original" (and always for audio), otherwise the
    // chosen short-side size.
    void exportSelected(const QUrl &url, double start, double end, int scaleHeight);
    void failed(const QString &message);
};
