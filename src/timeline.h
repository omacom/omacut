#pragma once

#include <QObject>
#include <QVariantList>

#include "edit.h"

// The clips being edited, with undo. A gesture (a drag) is one undo step.
// Times called "t" are seconds along the edited sequence; a clip's in and out
// are seconds in its source. Shared by the real backend and the QML tests'
// stand-in, so both edit the same way.
class Timeline : public QObject {
    Q_OBJECT
    // Each clip as {source, in, out, start, end}, start and end along the sequence.
    Q_PROPERTY(QVariantList clips READ clipList NOTIFY changed)
    Q_PROPERTY(double duration READ duration NOTIFY changed)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY changed)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY changed)
    // Cut, added to, or reordered, and not exported as it stands.
    Q_PROPERTY(bool unexported READ unexported NOTIFY changed)

public:
    explicit Timeline(QObject *parent = nullptr) : QObject(parent) {}

    edit::Clips clips() const { return m_clips; }
    QVariantList clipList() const;
    double duration() const { return edit::duration(m_clips); }
    bool canUndo() const { return !m_undo.isEmpty(); }
    bool canRedo() const { return !m_redo.isEmpty(); }
    bool unexported() const;

    // A freshly loaded video: one clip spanning all of it, and no history.
    void reset(double duration);
    // Adds a video of the given length as a clip after the one under t (at
    // the end past the last), as an undoable edit. Returns its source index.
    int addSource(double duration, double t);
    void markExported(const edit::Clips &clips);

    // The clip under t; the end of the sequence belongs to the last clip.
    Q_INVOKABLE int clipAt(double t) const;
    Q_INVOKABLE double clipStart(int index) const;
    // The nearest clip edge before (direction < 0) or after t, else the
    // sequence's start or end.
    Q_INVOKABLE double edgeFrom(double t, int direction) const;
    // Whether clip index and the next can join: the next carries on where
    // index leaves off, in the same source.
    Q_INVOKABLE bool canJoin(int index) const;

    Q_INVOKABLE void split(double t);
    // Clamps into the clip's source, keeping at least a minimum clip.
    Q_INVOKABLE void setClip(int index, double in, double out);
    // Moves one edge of a clip to a source time, stopping a minimum clip short
    // of the other edge.
    Q_INVOKABLE void moveEdge(int index, bool start, double sourceTime);
    // Moves the start or end of the clip under t to t.
    Q_INVOKABLE void trimTo(double t, bool start);
    Q_INVOKABLE void removeClip(int index);
    Q_INVOKABLE void toggleAudio(int index);
    Q_INVOKABLE void removeAt(double t);
    // Moves clip from to position to, shifting the clips in between.
    Q_INVOKABLE void moveClip(int from, int to);
    Q_INVOKABLE void joinClips(int index);
    Q_INVOKABLE void beginGesture();
    Q_INVOKABLE void endGesture();
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();

signals:
    void changed();

private:
    void apply(const edit::Clips &next);

    QList<double> m_sourceDurations;
    edit::Clips m_clips;
    edit::Clips m_exported;
    QList<edit::Clips> m_undo;
    QList<edit::Clips> m_redo;
    bool m_gesture = false;
    bool m_gestureRecorded = false;
};
