#pragma once

#include <QList>
#include <QObject>
#include <QUrl>

class FilePicker : public QObject {
    Q_OBJECT

public:
    explicit FilePicker(QObject *parent = nullptr) : QObject(parent) {}
    virtual ~FilePicker() = default;

    // A pending chooser ignores new requests; callers must retain its snapshot.
    virtual bool isPending() const = 0;
    virtual void openVideo() = 0;
    // scaleHeights are the downscale choices to offer besides "Original"
    // (e.g. {1080, 720}), matched by min(width, height) of the source.
    virtual void exportVideo(const QUrl &suggestedUrl, const QList<int> &scaleHeights) = 0;

signals:
    void openSelected(const QUrl &url);
    // scaleHeight is 0 for "Original", otherwise the chosen short-side size.
    void exportSelected(const QUrl &url, int scaleHeight);
    void failed(const QString &message);
};
