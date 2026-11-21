#include "timeline.h"

#include <QVariantMap>

#include <QRegularExpression>

#include <algorithm>

namespace {
// What the texts offer: a colour the preview and ffmpeg both read, and
// sizes and positions that keep the text inside the frame.
// Letters, digits, spaces and a few marks: any installed family, and nothing
// that fontconfig or a filter graph would read as syntax.
bool validFont(const QString &font) {
    static const QRegularExpression plain(QStringLiteral("^[\\p{L}\\p{N}][\\p{L}\\p{N} .+]{0,63}$"));
    return plain.match(font).hasMatch();
}
bool validColor(const QString &color) {
    static const QRegularExpression hex(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    return hex.match(color).hasMatch();
}
}

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

QVariantList Timeline::textList() const {
    QVariantList result;
    for (const edit::TextOverlay &text : m_texts) {
        result.append(QVariantMap{{QStringLiteral("text"), text.text},
                                  {QStringLiteral("start"), text.start},
                                  {QStringLiteral("end"), text.end},
                                  {QStringLiteral("effect"), text.effect},
                                  {QStringLiteral("y"), text.y},
                                  {QStringLiteral("size"), text.size},
                                  {QStringLiteral("font"), text.font},
                                  {QStringLiteral("color"), text.color},
                                  {QStringLiteral("box"), text.box},
                                  {QStringLiteral("outline"), text.outline},
                                  {QStringLiteral("outlineColor"), text.outlineColor}});
    }
    return result;
}

bool Timeline::unexported() const {
    if (m_clips.isEmpty())
        return false;
    const bool untouched = edit::untouched(m_clips, m_sourceDurations.value(0)) && m_texts.isEmpty();
    return !untouched && !(m_clips == m_exported && m_texts == m_exportedTexts);
}

void Timeline::reset(double duration) {
    duration = std::max(0.0, duration);
    m_sourceDurations = {duration};
    m_clips = duration > 0.0 ? edit::Clips{{0, 0.0, duration}} : edit::Clips{};
    m_texts.clear();
    m_typingText = -1;
    m_exported.clear();
    m_exportedTexts.clear();
    m_undo.clear();
    m_redo.clear();
    m_gesture = false;
    emit changed();
}

void Timeline::restore(const QList<double> &sourceDurations, const edit::Clips &clips,
                       const edit::Overlays &texts) {
    m_sourceDurations = sourceDurations;
    m_clips = clips;
    m_texts = texts;
    m_typingText = -1;
    m_exported.clear();
    m_exportedTexts.clear();
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

void Timeline::markExported(const edit::Clips &clips, const edit::Overlays &texts) {
    m_exported = clips;
    m_exportedTexts = texts;
    emit changed();
}

void Timeline::apply(const edit::Clips &next, const edit::Overlays &texts, bool coalesce) {
    if ((next == m_clips && texts == m_texts) || next.isEmpty())
        return;
    if (coalesce && !m_undo.isEmpty()) {
        // Joins the step the last keystroke made; redo was cleared by it.
    } else if (!m_gesture || !m_gestureRecorded) {
        m_undo.append({m_clips, m_texts});
        m_redo.clear();
        m_gestureRecorded = m_gesture;
    }
    m_clips = next;
    m_texts = texts;
    if (!coalesce)
        m_typingText = -1;
    emit changed();
}

int Timeline::addText(double t) {
    if (m_clips.isEmpty())
        return -1;
    const double total = duration();
    edit::TextOverlay text;
    text.text = tr("Text");
    // A few seconds from t, pulled back from the end so it has room.
    text.end = std::min(std::max(t, 0.0) + 3.0, total);
    text.start = std::max(0.0, std::min(std::max(t, 0.0), text.end - 1.0));
    text.end = std::max(text.end, std::min(text.start + edit::minimumText, total));
    // Not on top of a text it overlaps in time: the first of the usual
    // places that none of them has taken.
    for (const double y : {0.94, 0.5, 0.06}) {
        bool taken = false;
        for (const edit::TextOverlay &other : m_texts)
            taken = taken || (other.start < text.end && other.end > text.start && std::abs(other.y - y) < 0.001);
        if (!taken) {
            text.y = y;
            break;
        }
    }
    edit::Overlays next = m_texts;
    next.append(text);
    apply(m_clips, next);
    return m_texts.size() - 1;
}

void Timeline::setTextProperty(int index, const QString &name, const QVariant &value) {
    if (index < 0 || index >= m_texts.size())
        return;
    edit::Overlays next = m_texts;
    edit::TextOverlay &text = next[index];
    const double total = duration();
    bool typing = false;
    if (name == QLatin1String("text")) {
        text.text = value.toString();
        typing = m_typingText == index;
    } else if (name == QLatin1String("start")) {
        text.start = std::clamp(value.toDouble(), 0.0, std::max(text.end - edit::minimumText, 0.0));
    } else if (name == QLatin1String("end")) {
        text.end = std::clamp(value.toDouble(), text.start + edit::minimumText,
                              std::max(total, text.start + edit::minimumText));
    } else if (name == QLatin1String("effect")) {
        if (!edit::effects().contains(value.toString()))
            return;
        text.effect = value.toString();
    } else if (name == QLatin1String("y")) {
        text.y = std::clamp(value.toDouble(), 0.0, 1.0);
    } else if (name == QLatin1String("size")) {
        text.size = std::clamp(value.toDouble(), 0.02, 0.3);
    } else if (name == QLatin1String("font")) {
        const QString font = value.toString().trimmed();
        if (!validFont(font))
            return;
        text.font = font;
    } else if (name == QLatin1String("color")) {
        if (!validColor(value.toString()))
            return;
        text.color = value.toString().toLower();
    } else if (name == QLatin1String("outline")) {
        text.outline = std::clamp(value.toDouble(), 0.0, 0.05);
    } else if (name == QLatin1String("outlineColor")) {
        if (!validColor(value.toString()))
            return;
        text.outlineColor = value.toString().toLower();
    } else if (name == QLatin1String("box")) {
        text.box = value.toBool();
    } else {
        return;
    }
    apply(m_clips, next, typing);
    if (name == QLatin1String("text"))
        m_typingText = index;
}

void Timeline::moveText(int index, double start) {
    if (index < 0 || index >= m_texts.size())
        return;
    edit::Overlays next = m_texts;
    edit::TextOverlay &text = next[index];
    const double length = text.length();
    text.start = std::clamp(start, 0.0, std::max(duration() - length, 0.0));
    text.end = text.start + length;
    apply(m_clips, next);
}

double Timeline::textAlpha(int index, double t) const {
    return index >= 0 && index < m_texts.size() ? edit::effectAlpha(m_texts[index], t) : 0.0;
}

double Timeline::textProgress(int index, double t) const {
    return index >= 0 && index < m_texts.size() ? edit::effectProgress(m_texts[index], t) : 0.0;
}

void Timeline::removeText(int index) {
    if (index < 0 || index >= m_texts.size())
        return;
    edit::Overlays next = m_texts;
    next.removeAt(index);
    apply(m_clips, next);
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
    // The same test playback and export use, so a handle dragged onto its
    // neighbour's edge, a hair off from float arithmetic, can still join.
    return index >= 0 && index + 1 < m_clips.size() && edit::continues(m_clips[index], m_clips[index + 1]);
}

bool Timeline::continues(int index) const {
    return index >= 0 && index + 1 < m_clips.size()
        && edit::continues(m_clips[index], m_clips[index + 1]);
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
    // Whether b picks up later in a's video, sharing its stretch of the
    // timeline: within a millisecond, as playback carries on.
    const auto carriesOn = [](const edit::Clip &a, const edit::Clip &b) {
        return a.source == b.source && b.in > a.out - 0.001;
    };
    if (start) {
        if (index > 0 && carriesOn(m_clips[index - 1], clip))
            sourceTime = std::max(sourceTime, m_clips[index - 1].out);
        setClip(index, std::min(sourceTime, clip.out - edit::minimumClip), clip.out);
    } else {
        if (index + 1 < m_clips.size() && carriesOn(clip, m_clips[index + 1]))
            sourceTime = std::min(sourceTime, m_clips[index + 1].in);
        setClip(index, clip.in, std::max(sourceTime, clip.in + edit::minimumClip));
    }
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

void Timeline::beginGesture() {
    if (m_clips.isEmpty())
        return;
    m_gesture = true;
    m_gestureRecorded = false;
}

void Timeline::endGesture() {
    m_gesture = false;
    m_typingText = -1;
}

void Timeline::undo() {
    if (m_gesture || m_undo.isEmpty())
        return;
    m_typingText = -1;
    m_redo.append({m_clips, m_texts});
    const State state = m_undo.takeLast();
    m_clips = state.clips;
    m_texts = state.texts;
    emit changed();
}

void Timeline::redo() {
    if (m_gesture || m_redo.isEmpty())
        return;
    m_typingText = -1;
    m_undo.append({m_clips, m_texts});
    const State state = m_redo.takeLast();
    m_clips = state.clips;
    m_texts = state.texts;
    emit changed();
}
