#include "dialogfilepicker.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QStandardPaths>

namespace {

QString openFolder() {
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(videos).exists() ? videos : QDir::homePath();
}

// Mirrors the portal picker's video filter, in QFileDialog's syntax.
QString videoFilters() {
    return QStringLiteral(
        "Video files (*.avi *.m4v *.mkv *.mov *.mp4 *.mpeg *.mpg *.webm);;All files (*)");
}

}  // namespace

DialogFilePicker::DialogFilePicker(QObject *parent) : FilePicker(parent) {}

QStringList DialogFilePicker::exportFilters(const QList<int> &scaleHeights) {
    // The portal picker offers quality as a combo box of its own. Native Windows
    // and macOS save dialogs have no field to put one in, so the choices ride in
    // the "Save as type" combo they already show: one native dialog, same pick.
    if (scaleHeights.isEmpty())
        return {QStringLiteral("MP4 video (*.mp4)")};

    QStringList filters = {QStringLiteral("MP4 video - Original (*.mp4)")};
    for (const int height : scaleHeights)
        filters << QStringLiteral("MP4 video - %1p (*.mp4)").arg(height);
    return filters;
}

int DialogFilePicker::scaleHeightForFilter(const QString &selected, const QStringList &filters,
                                           const QList<int> &scaleHeights) {
    // Position, not label text: entry 0 is "Original" (no downscale) and the
    // rest line up with scaleHeights in order.
    const qsizetype index = filters.indexOf(selected);
    if (index <= 0 || index > scaleHeights.size())
        return 0;
    return scaleHeights.at(index - 1);
}

void DialogFilePicker::openVideo() {
    const QString path = QFileDialog::getOpenFileName(nullptr, QStringLiteral("Open Video File"),
                                                      openFolder(), videoFilters());
    if (path.isEmpty())  // cancelled
        return;

    emit openSelected(QUrl::fromLocalFile(path));
}

void DialogFilePicker::exportVideo(const QUrl &suggestedUrl, double start, double end,
                                   const QList<int> &scaleHeights) {
    const QFileInfo target(suggestedUrl.toLocalFile());
    const QStringList filters = exportFilters(scaleHeights);
    QString selected = filters.first();

    const QString path =
        QFileDialog::getSaveFileName(nullptr, QStringLiteral("Save Video File"),
                                     target.absoluteFilePath(),
                                     filters.join(QStringLiteral(";;")), &selected);
    if (path.isEmpty())  // cancelled
        return;

    // Deliberately not forcing a .mp4 suffix here: Backend does that, and it
    // refuses when the rewrite lands on a file the dialog never offered to
    // overwrite. Appending it here would make that check a no-op.
    emit exportSelected(QUrl::fromLocalFile(path), start, end,
                        scaleHeightForFilter(selected, filters, scaleHeights));
}
