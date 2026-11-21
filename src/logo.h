#pragma once

#include <QObject>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QUrl>

#include <QVariantList>

class Timeline;

// A logo drawn over the whole exported video: an image, placed by where its
// centre falls in the frame (as fractions of it, so it lands in the same
// place at any size), as wide as a fraction of the frame's width, and as
// opaque as asked.
namespace logo {

struct Settings {
    // The image, an absolute path. Empty when there is no logo.
    QString path;
    // The image's width over its height.
    double aspect = 0.0;
    // The logo's centre, as fractions of the frame, from its top left.
    double x = 0.95;
    double y = 0.95;
    // Its width, as a fraction of the frame's width.
    double size = 0.15;
    double opacity = 1.0;

    bool active() const { return !path.isEmpty() && aspect > 0.0; }
    bool operator==(const Settings &other) const {
        return path == other.path && aspect == other.aspect && x == other.x && y == other.y
            && size == other.size && opacity == other.opacity;
    }
    bool operator!=(const Settings &other) const { return !(*this == other); }
};

constexpr double minimumSize = 0.02;
constexpr double maximumSize = 1.0;
constexpr double minimumOpacity = 0.05;
// How far a logo placed in a corner sits from the edges, as a fraction of the frame's width.
constexpr double cornerMargin = 0.03;

// Where the logo falls in a frame of this size, in the frame's own units.
// Never wider than the frame, nor taller (a tall logo is narrowed to fit), and
// always inside it however far its centre is put.
QRectF box(const Settings &logo, double frameWidth, double frameHeight);
// The same in whole pixels, for the export: the size is what ffmpeg scales
// the image to, so the position is worked out from that, not the other way.
QRect pixelBox(const Settings &logo, int frameWidth, int frameHeight);

}  // namespace logo

// The logo being edited, shared with QML. Not part of the undo history: it is
// a setting of the export, like the video's quality, and has its own remove.
class Logo : public QObject {
    Q_OBJECT
    // The image as a file URL, empty when there is no logo.
    Q_PROPERTY(QUrl source READ source NOTIFY changed)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(double aspect READ aspect NOTIFY changed)
    Q_PROPERTY(double x READ x NOTIFY changed)
    Q_PROPERTY(double y READ y NOTIFY changed)
    Q_PROPERTY(double size READ size NOTIFY changed)
    Q_PROPERTY(double opacity READ opacity NOTIFY changed)
    // Set up and not exported as it stands.
    Q_PROPERTY(bool unexported READ unexported NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)

public:
    explicit Logo(QObject *parent = nullptr) : QObject(parent) {}

    QUrl source() const { return m_settings.path.isEmpty() ? QUrl() : QUrl::fromLocalFile(m_settings.path); }
    bool active() const { return m_settings.active(); }
    double aspect() const { return m_settings.aspect; }
    double x() const { return m_settings.x; }
    double y() const { return m_settings.y; }
    double size() const { return m_settings.size; }
    double opacity() const { return m_settings.opacity; }
    bool unexported() const { return active() && m_settings != m_exported; }
    logo::Settings settings() const { return m_settings; }

    // The timeline being edited beside the logo, so Ctrl+Z can tell which edit
    // came last: the logo's own history is ordered against it by how it
    // stood (everything it shows, which is whatever it edits) when each logo
    // edit was made. Optional.
    void setTimeline(Timeline *timeline) { m_timeline = timeline; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    // Whether the next undo (or redo) is the logo's rather than the timeline's:
    // true when the timeline is as it was when that logo edit was made.
    Q_INVOKABLE bool ownsUndo() const;
    Q_INVOKABLE bool ownsRedo() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // A drag or slider is one undo step, however many changes it makes.
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    void clearHistory();

    // Takes an image as the logo, keeping where, how big and how opaque it
    // was if there already was one. False, with failed(), when it isn't an
    // image the export can use.
    Q_INVOKABLE bool setSource(const QUrl &url);
    Q_INVOKABLE void clear();
    // Puts the logo's centre at fractions of the frame, kept within it.
    Q_INVOKABLE void setCenter(double x, double y);
    Q_INVOKABLE void setSize(double size);
    Q_INVOKABLE void setOpacity(double opacity);
    // Sits the logo in a corner or edge of a frame of this size, a margin
    // from it: horizontal and vertical are -1 (left, top), 0 (middle) or 1.
    Q_INVOKABLE void anchor(int horizontal, int vertical, double frameWidth, double frameHeight);
    // Where it falls in a frame of this size, as the preview draws it.
    Q_INVOKABLE QRectF box(double frameWidth, double frameHeight) const;

    // The export wrote these settings: nothing is unexported until they change.
    void markExported(const logo::Settings &exported);
    // Whatever is set now is how things start, as for a newly opened video.
    // The history goes with it: a new video starts a new edit.
    void markCurrentExported() { markExported(m_settings); clearHistory(); }

signals:
    void changed();
    void activeChanged();
    void failed(const QString &message);

private:
    struct Step {
        logo::Settings settings;
        QVariantList timeline;
    };
    // Sets the logo, recording how it was as an undo step.
    void update(const logo::Settings &next);
    // Sets it without recording anything.
    void apply(const logo::Settings &next);
    // What the timeline holds, apart from its history and export state.
    QVariantList timelineState() const;

    Timeline *m_timeline = nullptr;
    QList<Step> m_undo;
    QList<Step> m_redo;
    bool m_gesture = false;
    bool m_gestureRecorded = false;

    logo::Settings m_settings;
    logo::Settings m_exported;
};
