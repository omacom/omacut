#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QQuickImageProvider>

// Wide outlines for the text preview, grown with the Jump Flooding Algorithm
// (https://bgolus.medium.com/the-quest-for-very-wide-outlines-ba82ed442cd9).
//
// Every pixel of the text is a seed that knows its own position. A few passes
// over the image, each looking at nine neighbours a step away, with the step
// halving from the width down to 1, hand every pixel the position of its
// nearest seed. That is log2(width) passes however wide the outline is, and
// the outline is round: a pixel is in it when that nearest seed is within the
// width. Export gets the same shape from drawtext's border, which FreeType
// strokes with round joins.
namespace outline {

// The seeds' nearest-seed map: for each pixel of a w x h image, the index
// (y * w + x) of the closest pixel in seeds (alpha of at least half), or -1
// when there is none within about reach pixels.
QVector<int> nearestSeeds(const QImage &mask, int reach);

// mask's silhouette grown by radius pixels, in color, anti-aliased at the
// edge. Same size as mask, so it must have room for the growth around it.
QImage grow(const QImage &mask, double radius, const QColor &color);

// The words as the preview's Text draws them (centred lines) with the
// outline of radius pixels grown around them. The image is the text's size
// plus the room the outline needs on every side; empty when there is
// nothing to draw or it would be absurdly large.
QImage render(const QString &text, const QFont &font, double radius, const QColor &color);

// How far past the text's edge render() leaves room for, in pixels.
int padding(double radius);

}  // namespace outline

// Serves outlines to QML, drawn off the UI thread. The id is
// "radius/rrggbb/pixelSize/family/text": the radius in pixels, the colour
// without its #, then the font and the words (which may hold slashes).
// QML: Image { asynchronous: true; source: "image://outline/..." }.
class OutlineProvider : public QQuickImageProvider {
public:
    OutlineProvider();
    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
