#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

inline constexpr int kLosslessCopy = -1;

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    virtual void openVideo() = 0;
    // scaleHeights are the downscale choices to offer besides "Compressed"
    // (e.g. {1080, 720}), matched by min(width, height) of the source.
    virtual bool exportVideo(const QUrl &suggestedUrl, double start, double end,
                             int scaleHeight) = 0;

signals:
    void openSelected(const QUrl &url);
    // -1 = lossless copy; 0 = Compressed re-encode; positive = scale height (short side).
    void exportSelected(const QUrl &url, double start, double end, int scaleHeight);
    void failed(const QString &message);
    void exportCancelled();
    void exportFailed(const QString &message);
};
