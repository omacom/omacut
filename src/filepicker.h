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
    // defaultCopy preselects the stream-copy mode in the export dialog.
    virtual void exportVideo(const QUrl &suggestedUrl, double start, double end,
                             const QList<int> &scaleHeights, bool defaultCopy = false) = 0;

signals:
    void openSelected(const QUrl &url);
    // scaleHeight is 0 for "Original", otherwise the chosen short-side size.
    // copy asks for a stream-copy cut (-c:v copy -c:a copy, no re-encode);
    // copy implies scaleHeight 0 since the streams are never decoded.
    void exportSelected(const QUrl &url, double start, double end, int scaleHeight,
                        bool copy);
    void failed(const QString &message);
};
