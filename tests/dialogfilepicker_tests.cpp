#include <QtTest>

#include "dialogfilepicker.h"

// Covers the quality-choice mapping that replaces the portal's "choices" combo.
// Both functions are static and pure, so this runs headless on any platform.
class DialogFilePickerTests : public QObject {
    Q_OBJECT

private slots:
    void noDownscalesOffersASinglePlainFilter();
    void downscalesBecomeSaveAsTypeEntries();
    void originalMapsToNoDownscale();
    void eachEntryMapsBackToItsScaleHeight();
    void anUnknownFilterFallsBackToOriginal();
};

void DialogFilePickerTests::noDownscalesOffersASinglePlainFilter() {
    const QStringList filters = DialogFilePicker::exportFilters({});
    QCOMPARE(filters, QStringList{QStringLiteral("MP4 video (*.mp4)")});
    // With nothing to choose, the sole entry must still mean "no downscale".
    QCOMPARE(DialogFilePicker::scaleHeightForFilter(filters.first(), filters, {}), 0);
}

void DialogFilePickerTests::downscalesBecomeSaveAsTypeEntries() {
    const QStringList filters = DialogFilePicker::exportFilters({1080, 720});
    QCOMPARE(filters.size(), 3);
    QCOMPARE(filters.at(0), QStringLiteral("MP4 video - Original (*.mp4)"));
    QCOMPARE(filters.at(1), QStringLiteral("MP4 video - 1080p (*.mp4)"));
    QCOMPARE(filters.at(2), QStringLiteral("MP4 video - 720p (*.mp4)"));
    // Every entry has to keep the .mp4 glob, or the dialog would filter the
    // export list down to nothing.
    for (const QString &filter : filters)
        QVERIFY(filter.contains(QStringLiteral("(*.mp4)")));
}

void DialogFilePickerTests::originalMapsToNoDownscale() {
    const QList<int> heights = {1080, 720};
    const QStringList filters = DialogFilePicker::exportFilters(heights);
    QCOMPARE(DialogFilePicker::scaleHeightForFilter(filters.at(0), filters, heights), 0);
}

void DialogFilePickerTests::eachEntryMapsBackToItsScaleHeight() {
    const QList<int> heights = {1080, 720};
    const QStringList filters = DialogFilePicker::exportFilters(heights);
    QCOMPARE(DialogFilePicker::scaleHeightForFilter(filters.at(1), filters, heights), 1080);
    QCOMPARE(DialogFilePicker::scaleHeightForFilter(filters.at(2), filters, heights), 720);
}

void DialogFilePickerTests::anUnknownFilterFallsBackToOriginal() {
    const QList<int> heights = {1080, 720};
    const QStringList filters = DialogFilePicker::exportFilters(heights);
    // A dialog that reports a filter we never offered must not be read as a
    // downscale request -- exporting at full size is the safe reading.
    QCOMPARE(DialogFilePicker::scaleHeightForFilter(QStringLiteral("Nonsense (*.x)"),
                                                    filters, heights), 0);
}

QTEST_GUILESS_MAIN(DialogFilePickerTests)
#include "dialogfilepicker_tests.moc"
