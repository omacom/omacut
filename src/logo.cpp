#include "logo.h"

#include <QDir>
#include <QFileInfo>
#include <QImageReader>

#include <QMetaProperty>

#include "timeline.h"

#include <algorithm>
#include <cmath>

namespace logo {

QRectF box(const Settings &logo, double frameWidth, double frameHeight) {
    if (!logo.active() || frameWidth <= 0.0 || frameHeight <= 0.0)
        return {};
    const double width = std::min(logo.size * frameWidth, frameHeight * logo.aspect);
    const double height = width / logo.aspect;
    const double left = std::clamp(logo.x * frameWidth - width / 2, 0.0, frameWidth - width);
    const double top = std::clamp(logo.y * frameHeight - height / 2, 0.0, frameHeight - height);
    return {left, top, width, height};
}

QRect pixelBox(const Settings &logo, int frameWidth, int frameHeight) {
    const QRectF ideal = box(logo, frameWidth, frameHeight);
    if (ideal.isEmpty())
        return {};
    const int width = std::clamp(int(std::lround(ideal.width())), 2, frameWidth);
    const int height = std::clamp(int(std::lround(width / logo.aspect)), 1, frameHeight);
    const int left = std::clamp(int(std::lround(logo.x * frameWidth - width / 2.0)), 0, frameWidth - width);
    const int top = std::clamp(int(std::lround(logo.y * frameHeight - height / 2.0)), 0, frameHeight - height);
    return {left, top, width, height};
}

double endOf(const Settings &logo, double duration) {
    return logo.end < 0.0 || logo.end > duration ? duration : logo.end;
}

double fadeInOf(const Settings &logo, double duration) {
    const double span = std::max(0.0, endOf(logo, duration) - logo.start);
    return std::clamp(logo.fadeIn, 0.0, span);
}

double fadeOutOf(const Settings &logo, double duration) {
    const double span = std::max(0.0, endOf(logo, duration) - logo.start);
    return std::clamp(logo.fadeOut, 0.0, span - fadeInOf(logo, duration));
}

double alphaAt(const Settings &logo, double time, double duration) {
    const double end = endOf(logo, duration);
    if (!logo.active() || time < logo.start || time > end)
        return 0.0;
    double alpha = logo.opacity;
    const double in = fadeInOf(logo, duration), out = fadeOutOf(logo, duration);
    if (in > 0.0)
        alpha *= std::clamp((time - logo.start) / in, 0.0, 1.0);
    if (out > 0.0)
        alpha *= std::clamp((end - time) / out, 0.0, 1.0);
    return alpha;
}

}  // namespace logo

namespace {
// What ffmpeg reads as one still picture, and Qt can show beside it. (A GIF
// would play its animation, and an SVG isn't something ffmpeg opens.)
bool stillImageName(const QString &suffix) {
    static const QStringList names = {QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"),
                                      QStringLiteral("webp"), QStringLiteral("bmp")};
    return names.contains(suffix.toLower());
}
}

double Logo::duration() const {
    return m_timeline ? m_timeline->duration() : 1e9;
}

QUrl Logo::source() const {
    const logo::Settings now = picked();
    return now.path.isEmpty() ? QUrl() : QUrl::fromLocalFile(now.path);
}

QVariantList Logo::itemList() const {
    QVariantList list;
    for (const logo::Settings &item : m_items)
        list.append(QVariantMap{{QStringLiteral("source"), QUrl::fromLocalFile(item.path)},
                                {QStringLiteral("start"), item.start},
                                {QStringLiteral("end"), item.end},
                                {QStringLiteral("fadeIn"), item.fadeIn},
                                {QStringLiteral("fadeOut"), item.fadeOut}});
    return list;
}

QVariantList Logo::timelineState() const {
    QVariantList state;
    if (!m_timeline)
        return state;
    static const QStringList apart = {QStringLiteral("objectName"), QStringLiteral("canUndo"),
                                      QStringLiteral("canRedo"), QStringLiteral("unexported")};
    const QMetaObject *meta = m_timeline->metaObject();
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (!apart.contains(QString::fromLatin1(property.name())))
            state.append(property.read(m_timeline));
    }
    return state;
}

void Logo::update(const logo::Settings &next) {
    if (m_current < 0 || next == m_items.at(m_current))
        return;
    State state{m_items, m_current};
    state.items[m_current] = next;
    update(state);
}

void Logo::update(const State &next) {
    if (next.items == m_items && next.current == m_current)
        return;
    if (!m_gesture || !m_gestureRecorded) {
        m_undo.append({{m_items, m_current}, timelineState()});
        if (m_undo.size() > 200)
            m_undo.removeFirst();
        m_redo.clear();
        m_gestureRecorded = m_gesture;
    }
    apply(next);
}

void Logo::apply(const State &next) {
    const bool wasActive = active();
    m_items = next.items;
    m_current = m_items.isEmpty() ? -1 : std::clamp(next.current, 0, int(m_items.size()) - 1);
    emit changed();
    if (wasActive != active())
        emit activeChanged();
}

bool Logo::read(const QUrl &url, logo::Settings *into) {
    // Only an absolute local file reaches ffmpeg, as for videos.
    const QString path = url.toLocalFile();
    if (!url.isLocalFile() || !QDir::isAbsolutePath(path)) {
        emit failed(tr("Only local files can be used."));
        return false;
    }
    const QFileInfo file(path);
    if (!file.isFile() || !file.isReadable()) {
        emit failed(tr("%1 can't be read.").arg(file.fileName()));
        return false;
    }
    if (!stillImageName(file.suffix())) {
        emit failed(tr("%1 isn't a PNG, JPEG, WebP or BMP image.").arg(file.fileName()));
        return false;
    }
    QImageReader reader(path);
    const QSize size = reader.size();
    if (!size.isValid() || size.isEmpty()) {
        emit failed(tr("%1 isn't an image that can be read.").arg(file.fileName()));
        return false;
    }
    into->path = file.absoluteFilePath();
    into->aspect = double(size.width()) / size.height();
    return true;
}

bool Logo::setSource(const QUrl &url) {
    if (m_current < 0)
        return addSource(url);
    logo::Settings next = picked();
    if (!read(url, &next))
        return false;
    update(next);
    return true;
}

bool Logo::addSource(const QUrl &url) {
    logo::Settings added;
    if (!read(url, &added))
        return false;
    State state{m_items, m_current};
    state.items.append(added);
    state.current = int(state.items.size()) - 1;
    update(state);
    return true;
}

void Logo::select(int index) {
    if (index < 0 || index >= m_items.size() || index == m_current)
        return;
    // Picking one is not an edit: it isn't undone, and can't make a step.
    m_current = index;
    emit changed();
}

void Logo::remove() {
    if (m_current < 0)
        return;
    State state{m_items, m_current};
    state.items.removeAt(m_current);
    state.current = std::min(m_current, int(state.items.size()) - 1);
    update(state);
    // Nothing left to export, so nothing left unexported.
    if (m_items.isEmpty())
        m_exported.clear();
}

void Logo::clear() {
    update(State{});
    m_exported.clear();
}

void Logo::setCenter(double x, double y) {
    logo::Settings next = picked();
    next.x = std::clamp(x, 0.0, 1.0);
    next.y = std::clamp(y, 0.0, 1.0);
    update(next);
}

void Logo::setSize(double size) {
    logo::Settings next = picked();
    next.size = std::clamp(size, logo::minimumSize, logo::maximumSize);
    update(next);
}

void Logo::setOpacity(double opacity) {
    logo::Settings next = picked();
    next.opacity = std::clamp(opacity, logo::minimumOpacity, 1.0);
    update(next);
}

void Logo::setStart(double seconds) {
    logo::Settings next = picked();
    const double end = logo::endOf(next, duration());
    next.start = std::clamp(seconds, 0.0, std::max(0.0, end - logo::minimumSpan));
    update(next);
}

void Logo::setEnd(double seconds) {
    logo::Settings next = picked();
    const double total = duration();
    const double end = std::clamp(seconds, std::min(next.start + logo::minimumSpan, total), total);
    // Reaching the edit's end means staying with it, as the edit grows or shrinks.
    next.end = end >= total - 0.001 ? -1.0 : end;
    update(next);
}

void Logo::moveTo(double start) {
    logo::Settings next = picked();
    const double total = duration();
    const double length = logo::endOf(next, total) - next.start;
    next.start = std::clamp(start, 0.0, std::max(0.0, total - length));
    const double end = next.start + length;
    next.end = end >= total - 0.001 ? -1.0 : end;
    update(next);
}

void Logo::setFadeIn(double seconds) {
    logo::Settings next = picked();
    const double span = std::max(0.0, logo::endOf(next, duration()) - next.start);
    next.fadeIn = std::clamp(seconds, 0.0, std::max(0.0, span - next.fadeOut));
    update(next);
}

void Logo::setFadeOut(double seconds) {
    logo::Settings next = picked();
    const double span = std::max(0.0, logo::endOf(next, duration()) - next.start);
    next.fadeOut = std::clamp(seconds, 0.0, std::max(0.0, span - next.fadeIn));
    update(next);
}

void Logo::anchor(int horizontal, int vertical, double frameWidth, double frameHeight) {
    if (m_current < 0 || frameWidth <= 0.0 || frameHeight <= 0.0)
        return;
    logo::Settings next = picked();
    const QRectF placed = logo::box(next, frameWidth, frameHeight);
    const double margin = logo::cornerMargin * frameWidth;
    next.x = horizontal < 0 ? (margin + placed.width() / 2) / frameWidth
           : horizontal > 0 ? 1.0 - (margin + placed.width() / 2) / frameWidth : 0.5;
    next.y = vertical < 0 ? (margin + placed.height() / 2) / frameHeight
           : vertical > 0 ? 1.0 - (margin + placed.height() / 2) / frameHeight : 0.5;
    next.x = std::clamp(next.x, 0.0, 1.0);
    next.y = std::clamp(next.y, 0.0, 1.0);
    update(next);
}

QRectF Logo::box(double frameWidth, double frameHeight) const {
    return logo::box(picked(), frameWidth, frameHeight);
}

QRectF Logo::boxOf(int index, double frameWidth, double frameHeight) const {
    if (index < 0 || index >= m_items.size())
        return {};
    return logo::box(m_items.at(index), frameWidth, frameHeight);
}

double Logo::alphaOf(int index, double time, double duration) const {
    if (index < 0 || index >= m_items.size())
        return 0.0;
    return logo::alphaAt(m_items.at(index), time, duration);
}

bool Logo::ownsUndo() const {
    return !m_undo.isEmpty() && m_undo.last().timeline == timelineState();
}

bool Logo::ownsRedo() const {
    return !m_redo.isEmpty() && m_redo.last().timeline == timelineState();
}

void Logo::undo() {
    if (m_undo.isEmpty())
        return;
    const Step step = m_undo.takeLast();
    m_redo.append({{m_items, m_current}, timelineState()});
    apply(step.state);
}

void Logo::redo() {
    if (m_redo.isEmpty())
        return;
    const Step step = m_redo.takeLast();
    m_undo.append({{m_items, m_current}, timelineState()});
    apply(step.state);
}

void Logo::beginGesture() {
    m_gesture = true;
    m_gestureRecorded = false;
}

void Logo::endGesture() {
    m_gesture = false;
    m_gestureRecorded = false;
}

void Logo::clearHistory() {
    if (m_undo.isEmpty() && m_redo.isEmpty())
        return;
    m_undo.clear();
    m_redo.clear();
    emit changed();
}

void Logo::markExported(const QList<logo::Settings> &exported) {
    if (m_exported == exported)
        return;
    m_exported = exported;
    emit changed();
}
