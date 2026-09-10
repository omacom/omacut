#pragma once

#include "filepicker.h"

#include <QStringList>

// A FilePicker built on Qt's own QFileDialog, for platforms that have no
// xdg-desktop-portal to ask — Windows and macOS. Both get their native dialog.
class DialogFilePicker : public FilePicker {
    Q_OBJECT

public:
    explicit DialogFilePicker(QObject *parent = nullptr);

    void openVideo() override;
    void exportVideo(const QUrl &suggestedUrl, double start, double end,
                     const QList<int> &scaleHeights) override;

    // The "Save as type" entries offered for scaleHeights, and the downscale a
    // chosen entry maps back to. Exposed so the tests can check the pairing
    // without driving a modal dialog.
    static QStringList exportFilters(const QList<int> &scaleHeights);
    static int scaleHeightForFilter(const QString &selected, const QStringList &filters,
                                    const QList<int> &scaleHeights);
};
