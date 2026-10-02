#pragma once

#include <QVariantMap>

#include "filepicker.h"

class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    bool isPending() const override { return m_pendingAction != Action::None; }
    void openVideo() override;
    void exportVideo(const QUrl &suggestedUrl, const QList<int> &scaleHeights) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action {
        None,
        Open,
        Export
    };

    void requestFile(const QString &method, const QString &title,
                     QVariantMap options, Action action);
    bool connectToRequestPath(const QString &path);
    void clearPending();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
};
