#include "timeline.h"

#include <QVariantMap>

#include <algorithm>

QVariantList Timeline::clipList() const {
    QVariantList result;
    double start = 0.0;
    for (const edit::Clip &clip : m_clips) {
        result.append(QVariantMap{{QStringLiteral("source"), clip.source},
                                  {QStringLiteral("in"), clip.in},
                                  {QStringLiteral("out"), clip.out},
                                  {QStringLiteral("start"), start},
                                  {QStringLiteral("end"), start + clip.length()}});
        start += clip.length();
    }
    return result;
}

bool Timeline::unexported() const {
    return !m_clips.isEmpty() && !edit::untouched(m_clips, m_sourceDurations.value(0))
        && m_clips != m_exported;
}

void Timeline::reset(double duration) {
    duration = std::max(0.0, duration);
    m_sourceDurations = {duration};
    m_clips = duration > 0.0 ? edit::Clips{{0, 0.0, duration}} : edit::Clips{};
    m_exported.clear();
    m_undo.clear();
    m_redo.clear();
    m_gesture = false;
    emit changed();
}

int Timeline::addSource(double duration, double t) {
    m_sourceDurations.append(std::max(0.0, duration));
    const int source = m_sourceDurations.size() - 1;
    edit::Clips next = m_clips;
    next.insert(m_clips.isEmpty() ? 0 : clipAt(t) + 1, {source, 0.0, m_sourceDurations.last()});
    apply(next);
    return source;
}

void Timeline::markExported(const edit::Clips &clips) {
    m_exported = clips;
    emit changed();
}

void Timeline::apply(const edit::Clips &next) {
    if (next == m_clips || next.isEmpty())
        return;
    if (!m_gesture || !m_gestureRecorded) {
        m_undo.append(m_clips);
        m_redo.clear();
        m_gestureRecorded = m_gesture;
    }
    m_clips = next;
    emit changed();
}

int Timeline::clipAt(double t) const {
    double end = 0.0;
    for (int i = 0; i < m_clips.size(); ++i) {
        end += m_clips[i].length();
        if (t < end)
            return i;
    }
    return m_clips.size() - 1;
}

double Timeline::clipStart(int index) const {
    double start = 0.0;
    for (int i = 0; i < index && i < m_clips.size(); ++i)
        start += m_clips[i].length();
    return start;
}

double Timeline::edgeFrom(double t, int direction) const {
    double target = direction > 0 ? duration() : 0.0;
    double edge = 0.0;
    for (int i = 0; i <= m_clips.size(); ++i) {
        if (direction > 0 && edge > t + 0.01)
            return edge;
        if (direction < 0 && edge < t - 0.01)
            target = edge;
        if (i < m_clips.size())
            edge += m_clips[i].length();
    }
    return target;
}

bool Timeline::canJoin(int index) const {
    return index >= 0 && index + 1 < m_clips.size()
        && m_clips[index].source == m_clips[index + 1].source
        && m_clips[index].out == m_clips[index + 1].in;
}

void Timeline::split(double t) {
    const int i = clipAt(t);
    if (i < 0)
        return;
    const edit::Clip clip = m_clips[i];
    const double at = clip.in + (t - clipStart(i));
    if (at - clip.in < edit::minimumClip || clip.out - at < edit::minimumClip)
        return;
    edit::Clips next = m_clips;
    next[i].out = at;
    next.insert(i + 1, {clip.source, at, clip.out});
    apply(next);
}

void Timeline::setClip(int index, double in, double out) {
    if (index < 0 || index >= m_clips.size())
        return;
    const double sourceEnd = m_sourceDurations.value(m_clips[index].source);
    in = std::clamp(in, 0.0, sourceEnd);
    out = std::clamp(out, 0.0, sourceEnd);
    if (out - in < edit::minimumClip)
        return;
    edit::Clips next = m_clips;
    next[index].in = in;
    next[index].out = out;
    apply(next);
}

void Timeline::moveEdge(int index, bool start, double sourceTime) {
    if (index < 0 || index >= m_clips.size())
        return;
    const edit::Clip clip = m_clips[index];
    if (start)
        setClip(index, std::min(sourceTime, clip.out - edit::minimumClip), clip.out);
    else
        setClip(index, clip.in, std::max(sourceTime, clip.in + edit::minimumClip));
}

void Timeline::trimTo(double t, bool start) {
    const int i = clipAt(t);
    if (i < 0)
        return;
    const double at = m_clips[i].in + (t - clipStart(i));
    if (start)
        setClip(i, at, m_clips[i].out);
    else
        setClip(i, m_clips[i].in, at);
}

void Timeline::removeClip(int index) {
    if (index < 0 || index >= m_clips.size() || m_clips.size() < 2)
        return;
    edit::Clips next = m_clips;
    next.removeAt(index);
    apply(next);
}

void Timeline::removeAt(double t) {
    removeClip(clipAt(t));
}

void Timeline::moveClip(int from, int to) {
    if (from < 0 || from >= m_clips.size() || to < 0 || to >= m_clips.size())
        return;
    edit::Clips next = m_clips;
    next.move(from, to);
    apply(next);
}

void Timeline::joinClips(int index) {
    if (!canJoin(index))
        return;
    edit::Clips next = m_clips;
    next[index].out = next[index + 1].out;
    next.removeAt(index + 1);
    apply(next);
}

void Timeline::duplicateClip(int index) {
    if (index < 0 || index >= m_clips.size())
        return;
    edit::Clips next = m_clips;
    next.insert(index + 1, m_clips.at(index));
    apply(next);
}

void Timeline::beginGesture() {
    if (m_clips.isEmpty())
        return;
    m_gesture = true;
    m_gestureRecorded = false;
}

void Timeline::endGesture() {
    m_gesture = false;
}

void Timeline::undo() {
    if (m_gesture || m_undo.isEmpty())
        return;
    m_redo.append(m_clips);
    m_clips = m_undo.takeLast();
    emit changed();
}

void Timeline::redo() {
    if (m_gesture || m_redo.isEmpty())
        return;
    m_undo.append(m_clips);
    m_clips = m_redo.takeLast();
    emit changed();
}
