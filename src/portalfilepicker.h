#pragma once

#include <QVariantMap>

#include "filepicker.h"

class PortalFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit PortalFilePicker(QObject *parent = nullptr);

    void openVideo() override;
    void exportVideo(const QUrl &suggestedUrl, double start, double end,
                     const QList<int> &scaleHeights) override;
    void exportAudio(const QUrl &suggestedUrl, double start, double end) override;

private slots:
    void handleResponse(uint response, const QVariantMap &results);

private:
    enum class Action {
        None,
        Open,
        Export
    };

    bool requestFile(const QString &method, const QString &title,
                     QVariantMap options, Action action);
    // The SaveFile options shared by every export: label, folder, name, filter.
    QVariantMap exportOptions(const QUrl &suggestedUrl, const QVariant &filters,
                              const QVariant &currentFilter) const;
    void requestExport(const QString &title, const QVariantMap &options,
                       double start, double end);
    bool connectToRequestPath(const QString &path);
    void clearPending();

    QString m_pendingPath;
    Action m_pendingAction = Action::None;
    double m_pendingExportStart = 0;
    double m_pendingExportEnd = 0;
};
