#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openVideo() = 0;
    // scaleHeights are the downscale choices to offer besides "Original"
    // (e.g. {1080, 720}), matched by min(width, height) of the source.
    virtual void exportVideo(const QUrl &suggestedUrl, const QList<int> &scaleHeights) = 0;

signals:
    void openSelected(const QUrl &url);
    // scaleHeight is 0 for "Original", otherwise the chosen short-side size.
    // preset is the chosen ffmpeg::Preset (0 when the dialog gave none).
    void exportSelected(const QUrl &url, int scaleHeight, int preset = 0);
    void failed(const QString &message);
};
