#pragma once

#include <QList>
#include <QObject>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QUrl>
#include <QVariantList>

class Timeline;

// Logos drawn over the exported video: images, each placed by where its
// centre falls in the frame (as fractions of it, so it lands in the same
// place at any size), as wide as a fraction of the frame's width, as opaque
// as asked, and shown from one time of the edit to another, fading in and out.
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
    // When it shows, in seconds along the edit. An end below zero is the
    // end of the edit, whatever that comes to.
    double start = 0.0;
    double end = -1.0;
    // How long it takes to fade in at its start and out at its end, seconds.
    double fadeIn = 0.0;
    double fadeOut = 0.0;

    bool active() const { return !path.isEmpty() && aspect > 0.0; }
    bool operator==(const Settings &other) const {
        return path == other.path && aspect == other.aspect && x == other.x && y == other.y
            && size == other.size && opacity == other.opacity && start == other.start && end == other.end
            && fadeIn == other.fadeIn && fadeOut == other.fadeOut;
    }
    bool operator!=(const Settings &other) const { return !(*this == other); }
};

constexpr double minimumSize = 0.02;
constexpr double maximumSize = 1.0;
constexpr double minimumOpacity = 0.05;
// How far a logo placed in a corner sits from the edges, as a fraction of the frame's width.
constexpr double cornerMargin = 0.03;
// The shortest a logo shows for, seconds.
constexpr double minimumSpan = 0.2;

// Where the logo falls in a frame of this size, in the frame's own units.
// Never wider than the frame, nor taller (a tall logo is narrowed to fit), and
// always inside it however far its centre is put.
QRectF box(const Settings &logo, double frameWidth, double frameHeight);
// The same in whole pixels, for the export: the size is what ffmpeg scales
// the image to, so the position is worked out from that, not the other way.
QRect pixelBox(const Settings &logo, int frameWidth, int frameHeight);

// When the logo stops showing in an edit this long, seconds.
double endOf(const Settings &logo, double duration);
// The fades it really has in an edit this long: together they fit its time.
double fadeInOf(const Settings &logo, double duration);
double fadeOutOf(const Settings &logo, double duration);
// How visible it is at a time along an edit this long: nothing outside its
// time, its opacity in the middle, faded in and out at the ends.
double alphaAt(const Settings &logo, double time, double duration);

}  // namespace logo

// The logos being edited, shared with QML, one of them picked at a time: the
// properties that say how a logo looks are the picked one's. Not part of the
// timeline's own history, but ordered with it when undoing.
class Logo : public QObject {
    Q_OBJECT
    // The picked logo's image as a file URL, empty when there are no logos.
    Q_PROPERTY(QUrl source READ source NOTIFY changed)
    // Whether there is a logo.
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    Q_PROPERTY(int count READ count NOTIFY changed)
    // The picked logo, or -1.
    Q_PROPERTY(int current READ current NOTIFY changed)
    Q_PROPERTY(double aspect READ aspect NOTIFY changed)
    Q_PROPERTY(double x READ x NOTIFY changed)
    Q_PROPERTY(double y READ y NOTIFY changed)
    Q_PROPERTY(double size READ size NOTIFY changed)
    Q_PROPERTY(double opacity READ opacity NOTIFY changed)
    Q_PROPERTY(double start READ start NOTIFY changed)
    // Seconds along the edit; below zero is the end of the edit.
    Q_PROPERTY(double end READ end NOTIFY changed)
    Q_PROPERTY(double fadeIn READ fadeIn NOTIFY changed)
    Q_PROPERTY(double fadeOut READ fadeOut NOTIFY changed)
    // Every logo as {source, start, end, fadeIn, fadeOut}, for its strip.
    Q_PROPERTY(QVariantList items READ itemList NOTIFY changed)
    // Set up and not exported as it stands.
    Q_PROPERTY(bool unexported READ unexported NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)

public:
    explicit Logo(QObject *parent = nullptr) : QObject(parent) {}

    QUrl source() const;
    bool active() const { return !m_items.isEmpty(); }
    int count() const { return m_items.size(); }
    int current() const { return m_current; }
    double aspect() const { return picked().aspect; }
    double x() const { return picked().x; }
    double y() const { return picked().y; }
    double size() const { return picked().size; }
    double opacity() const { return picked().opacity; }
    double start() const { return picked().start; }
    double end() const { return picked().end; }
    double fadeIn() const { return picked().fadeIn; }
    double fadeOut() const { return picked().fadeOut; }
    QVariantList itemList() const;
    bool unexported() const { return !m_items.isEmpty() && m_items != m_exported; }
    // The picked logo, or none.
    logo::Settings settings() const { return picked(); }
    QList<logo::Settings> all() const { return m_items; }

    // The timeline being edited beside the logos: their times are along it,
    // and Ctrl+Z tells which edit came last by how it stood (everything it
    // shows, which is whatever it edits) when each logo edit was made.
    // Optional.
    void setTimeline(Timeline *timeline) { m_timeline = timeline; }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    // Whether the next undo (or redo) is the logos' rather than the timeline's:
    // true when the timeline is as it was when that logo edit was made.
    Q_INVOKABLE bool ownsUndo() const;
    Q_INVOKABLE bool ownsRedo() const;
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    // A drag or slider is one undo step, however many changes it makes.
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    void clearHistory();

    // Takes an image as the picked logo, keeping where, how big and how
    // opaque it was, and when it shows; with no logo yet, adds one. False,
    // with failed(), when it isn't an image the export can use.
    Q_INVOKABLE bool setSource(const QUrl &url);
    // Adds another logo with an image, and picks it.
    Q_INVOKABLE bool addSource(const QUrl &url);
    Q_INVOKABLE void select(int index);
    // Takes away the picked logo; clear() takes them all.
    Q_INVOKABLE void remove();
    Q_INVOKABLE void clear();
    // Puts the logo's centre at fractions of the frame, kept within it.
    Q_INVOKABLE void setCenter(double x, double y);
    Q_INVOKABLE void setSize(double size);
    Q_INVOKABLE void setOpacity(double opacity);
    // When it starts and stops showing, seconds along the edit. Each keeps
    // the logo shown for a minimum time; an end at the edit's end is "to the end".
    Q_INVOKABLE void setStart(double seconds);
    Q_INVOKABLE void setEnd(double seconds);
    // Moves it in time, keeping how long it shows.
    Q_INVOKABLE void moveTo(double start);
    Q_INVOKABLE void setFadeIn(double seconds);
    Q_INVOKABLE void setFadeOut(double seconds);
    // Sits the logo in a corner or edge of a frame of this size, a margin
    // from it: horizontal and vertical are -1 (left, top), 0 (middle) or 1.
    Q_INVOKABLE void anchor(int horizontal, int vertical, double frameWidth, double frameHeight);
    // Where the picked one falls in a frame of this size, as the preview draws it.
    Q_INVOKABLE QRectF box(double frameWidth, double frameHeight) const;
    Q_INVOKABLE QRectF boxOf(int index, double frameWidth, double frameHeight) const;
    // How visible logo index is at a time, in an edit this long.
    Q_INVOKABLE double alphaOf(int index, double time, double duration) const;

    // The export wrote these logos: nothing is unexported until they change.
    void markExported(const QList<logo::Settings> &exported);
    // Whatever is set now is how things start, as for a newly opened video.
    // The history goes with it: a new video starts a new edit.
    void markCurrentExported() { markExported(m_items); clearHistory(); }

signals:
    void changed();
    void activeChanged();
    void failed(const QString &message);

private:
    struct State {
        QList<logo::Settings> items;
        int current = -1;
    };
    struct Step {
        State state;
        QVariantList timeline;
    };
    logo::Settings picked() const { return m_current >= 0 ? m_items.at(m_current) : logo::Settings(); }
    // Sets how the picked logo is, recording how it was as an undo step.
    void update(const logo::Settings &next);
    // Sets the logos, recording how they were as an undo step.
    void update(const State &next);
    // Sets them without recording anything.
    void apply(const State &next);
    // What the timeline holds, apart from its history and export state.
    QVariantList timelineState() const;
    double duration() const;
    // Reads an image into settings, or says why not.
    bool read(const QUrl &url, logo::Settings *into);

    Timeline *m_timeline = nullptr;
    QList<logo::Settings> m_items;
    int m_current = -1;
    QList<logo::Settings> m_exported;
    QList<Step> m_undo;
    QList<Step> m_redo;
    bool m_gesture = false;
    bool m_gestureRecorded = false;
};
