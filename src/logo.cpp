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
    if (next == m_settings)
        return;
    if (!m_gesture || !m_gestureRecorded) {
        m_undo.append({m_settings, timelineState()});
        if (m_undo.size() > 200)
            m_undo.removeFirst();
        m_redo.clear();
        m_gestureRecorded = m_gesture;
    }
    apply(next);
}

void Logo::apply(const logo::Settings &next) {
    if (next == m_settings) {
        emit changed();
        return;
    }
    const bool wasActive = active();
    m_settings = next;
    emit changed();
    if (wasActive != active())
        emit activeChanged();
}

bool Logo::setSource(const QUrl &url) {
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

    logo::Settings next = m_settings;
    next.path = file.absoluteFilePath();
    next.aspect = double(size.width()) / size.height();
    update(next);
    return true;
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
    m_redo.append({m_settings, timelineState()});
    apply(step.settings);
}

void Logo::redo() {
    if (m_redo.isEmpty())
        return;
    const Step step = m_redo.takeLast();
    m_undo.append({m_settings, timelineState()});
    apply(step.settings);
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

void Logo::clear() {
    update({});
    // Nothing left to export, so nothing left unexported.
    m_exported = {};
}

void Logo::setCenter(double x, double y) {
    logo::Settings next = m_settings;
    next.x = std::clamp(x, 0.0, 1.0);
    next.y = std::clamp(y, 0.0, 1.0);
    update(next);
}

void Logo::setSize(double size) {
    logo::Settings next = m_settings;
    next.size = std::clamp(size, logo::minimumSize, logo::maximumSize);
    update(next);
}

void Logo::setOpacity(double opacity) {
    logo::Settings next = m_settings;
    next.opacity = std::clamp(opacity, logo::minimumOpacity, 1.0);
    update(next);
}

void Logo::anchor(int horizontal, int vertical, double frameWidth, double frameHeight) {
    if (!active() || frameWidth <= 0.0 || frameHeight <= 0.0)
        return;
    const QRectF placed = logo::box(m_settings, frameWidth, frameHeight);
    const double margin = logo::cornerMargin * frameWidth;
    logo::Settings next = m_settings;
    next.x = horizontal < 0 ? (margin + placed.width() / 2) / frameWidth
           : horizontal > 0 ? 1.0 - (margin + placed.width() / 2) / frameWidth : 0.5;
    next.y = vertical < 0 ? (margin + placed.height() / 2) / frameHeight
           : vertical > 0 ? 1.0 - (margin + placed.height() / 2) / frameHeight : 0.5;
    next.x = std::clamp(next.x, 0.0, 1.0);
    next.y = std::clamp(next.y, 0.0, 1.0);
    update(next);
}

QRectF Logo::box(double frameWidth, double frameHeight) const {
    return logo::box(m_settings, frameWidth, frameHeight);
}

void Logo::markExported(const logo::Settings &exported) {
    if (m_exported == exported)
        return;
    m_exported = exported;
    emit changed();
}
