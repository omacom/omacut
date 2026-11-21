#pragma once

#include <QObject>
#include <QVariantList>

#include "edit.h"

// The clips being edited, and the texts laid over them, with undo. A gesture (a drag) is one undo step.
// Times called "t" are seconds along the edited sequence; a clip's in and out
// are seconds in its source. Shared by the real backend and the QML tests'
// stand-in, so both edit the same way.
class Timeline : public QObject {
    Q_OBJECT
    // Each clip as {source, in, out, start, end}, start and end along the sequence.
    Q_PROPERTY(QVariantList clips READ clipList NOTIFY changed)
    // Each text as {text, start, end, effect, y, size, font, color, box}, start and
    // end along the sequence.
    Q_PROPERTY(QVariantList texts READ textList NOTIFY changed)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    // Each video's length, by source index.
    Q_PROPERTY(QList<double> sourceDurations READ sourceDurations NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    // Cut, added to, or reordered, and not exported as it stands.
    Q_PROPERTY(bool unexported READ unexported NOTIFY changed)

public:
    explicit Timeline(QObject *parent = nullptr) : QObject(parent) {}

    edit::Clips clips() const { return m_clips; }
    edit::Overlays texts() const { return m_texts; }
    QVariantList textList() const;
    QVariantList clipList() const;
    double duration() const { return edit::duration(m_clips); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool unexported() const;

    // A freshly loaded video: one clip spanning all of it, and no history.
    void reset(double duration);
    // An edit as if just opened, with no history: its videos' lengths, and its clips.
    void restore(const QList<double> &sourceDurations, const edit::Clips &clips,
                 const edit::Overlays &texts = {});
    QList<double> sourceDurations() const { return m_sourceDurations; }
    // Adds a video of the given length as a clip after the one under t (at
    // the end past the last), as an undoable edit. Returns its source index.
    int addSource(double duration, double t);
    void markExported(const edit::Clips &clips, const edit::Overlays &texts = {});

    // The clip under t; the end of the sequence belongs to the last clip.
    Q_INVOKABLE int clipAt(double t) const;
    Q_INVOKABLE double clipStart(int index) const;
    // The nearest clip edge before (direction < 0) or after t, else the
    // sequence's start or end.
    Q_INVOKABLE double edgeFrom(double t, int direction) const;
    // Whether clip index and the next can join: the next carries on where
    // index leaves off, in the same source.
    Q_INVOKABLE bool canJoin(int index) const;
    // Whether playback can run from index into the next clip without a seek.
    // A real cut returns false so the player jumps to the next in-point
    // instead of showing the frames that were cut out.
    Q_INVOKABLE bool continues(int index) const;

    Q_INVOKABLE void split(double t);
    // Clamps into the clip's source, keeping at least a minimum clip.
    Q_INVOKABLE void setClip(int index, double in, double out);
    // Moves one edge of a clip to a source time, stopping a minimum clip short
    // of the other edge, and at the neighbour that picks up later in the same
    // video: a handle can't drag over footage the next piece already uses.
    Q_INVOKABLE void moveEdge(int index, bool start, double sourceTime);
    // Moves the start or end of the clip under t to t.
    Q_INVOKABLE void trimTo(double t, bool start);
    Q_INVOKABLE void removeClip(int index);
    Q_INVOKABLE void removeAt(double t);
    // Moves clip from to position to, shifting the clips in between.
    Q_INVOKABLE void moveClip(int from, int to);
    Q_INVOKABLE void joinClips(int index);
    // Adds a text at t, a few seconds long, as an undoable edit. Returns its
    // index, or -1 when there is nothing to put it on.
    Q_INVOKABLE int addText(double t);
    // Sets one property of a text: text, start, end, effect, y, size, font,
    // color or box. Times are kept inside the edit and a minimum text long; the
    // rest is kept to what the preview and export understand.
    Q_INVOKABLE void setTextProperty(int index, const QString &name, const QVariant &value);
    // Moves a text's start and end together, keeping its length.
    Q_INVOKABLE void moveText(int index, double start);
    Q_INVOKABLE void removeText(int index);
    // How opaque text index is at t, and how far in its effect has come
    // (0 to 1): what the preview draws, from the rules export applies.
    Q_INVOKABLE double textAlpha(int index, double t) const;
    Q_INVOKABLE double textProgress(int index, double t) const;
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void changed();

private:
    // Everything an undo step brings back.
    struct State {
        edit::Clips clips;
        edit::Overlays texts;
    };
    void apply(const edit::Clips &next) { apply(next, m_texts); }
    // With coalesce, an edit that carries on the last one (typing in the
    // same text) joins its undo step instead of starting another.
    void apply(const edit::Clips &next, const edit::Overlays &texts, bool coalesce = false);

    QList<double> m_sourceDurations;
    edit::Clips m_clips;
    edit::Overlays m_texts;
    edit::Clips m_exported;
    edit::Overlays m_exportedTexts;
    QList<State> m_undo;
    QList<State> m_redo;
    // The text whose words were edited last, while that is still the latest edit.
    int m_typingText = -1;
    bool m_gesture = false;
    bool m_gestureRecorded = false;
};
