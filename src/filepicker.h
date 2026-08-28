#pragma once

#include <QList>
#include <QObject>
#include <QRectF>
#include <QUrl>

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openVideo() = 0;
    // scaleHeights are the downscale choices to offer besides "Original"
    // (e.g. {1080, 720}), matched by min(width, height) of the cropped frame
    // (or the source, if there is no crop).
    virtual void exportVideo(const QUrl &suggestedUrl, double start, double end,
                             const QList<int> &scaleHeights, const QRectF &crop = {}) = 0;

signals:
    void openSelected(const QUrl &url);
    // scaleHeight is 0 for "Original", otherwise the chosen short-side size.
    // crop is empty when the export is the full frame.
    void exportSelected(const QUrl &url, double start, double end, int scaleHeight,
                        const QRectF &crop);
    void failed(const QString &message);
};
