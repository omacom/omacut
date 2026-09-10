#include "portalfilepicker.h"

#include <QDBusConnection>
#include <QDBusArgument>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QStandardPaths>

namespace {
struct PortalFilterRule {
    uint type;
    QString pattern;
};

using PortalFilterRules = QList<PortalFilterRule>;

struct PortalFileFilter {
    QString name;
    PortalFilterRules rules;
};

using PortalFileFilters = QList<PortalFileFilter>;

// The portal's combo-box "choices" option: (id, label, [(option-id, option-label)], initial).
struct PortalChoiceOption {
    QString id;
    QString label;
};

using PortalChoiceOptions = QList<PortalChoiceOption>;

struct PortalChoice {
    QString id;
    QString label;
    PortalChoiceOptions options;
    QString initial;
};

using PortalChoices = QList<PortalChoice>;

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFilterRule &rule) {
    argument.beginStructure();
    argument << rule.type << rule.pattern;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFilterRule &rule) {
    argument.beginStructure();
    argument >> rule.type >> rule.pattern;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalFileFilter &filter) {
    argument.beginStructure();
    argument << filter.name << filter.rules;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalFileFilter &filter) {
    argument.beginStructure();
    argument >> filter.name >> filter.rules;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalChoiceOption &option) {
    argument.beginStructure();
    argument << option.id << option.label;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalChoiceOption &option) {
    argument.beginStructure();
    argument >> option.id >> option.label;
    argument.endStructure();
    return argument;
}

QDBusArgument &operator<<(QDBusArgument &argument, const PortalChoice &choice) {
    argument.beginStructure();
    argument << choice.id << choice.label << choice.options << choice.initial;
    argument.endStructure();
    return argument;
}

const QDBusArgument &operator>>(const QDBusArgument &argument, PortalChoice &choice) {
    argument.beginStructure();
    argument >> choice.id >> choice.label >> choice.options >> choice.initial;
    argument.endStructure();
    return argument;
}

void registerPortalFilterTypes() {
    static const bool registered = [] {
        qDBusRegisterMetaType<PortalFilterRule>();
        qDBusRegisterMetaType<PortalFilterRules>();
        qDBusRegisterMetaType<PortalFileFilter>();
        qDBusRegisterMetaType<PortalFileFilters>();
        qDBusRegisterMetaType<PortalChoiceOption>();
        qDBusRegisterMetaType<PortalChoiceOptions>();
        qDBusRegisterMetaType<PortalChoice>();
        qDBusRegisterMetaType<PortalChoices>();
        return true;
    }();
    Q_UNUSED(registered);
}

PortalFileFilter videoFilter() {
    return {
        QStringLiteral("Video files"),
        {
            {1, QStringLiteral("video/*")},
            {0, QStringLiteral("*.avi")},
            {0, QStringLiteral("*.m4v")},
            {0, QStringLiteral("*.mkv")},
            {0, QStringLiteral("*.mov")},
            {0, QStringLiteral("*.mp4")},
            {0, QStringLiteral("*.mpeg")},
            {0, QStringLiteral("*.mpg")},
            {0, QStringLiteral("*.webm")},
        },
    };
}

PortalFileFilters videoFilters() {
    return {
        videoFilter(),
        {QStringLiteral("All files"), {{0, QStringLiteral("*")}}},
    };
}

PortalFileFilter mp4Filter() {
    return {QStringLiteral("MP4 video"), {{0, QStringLiteral("*.mp4")}}};
}

PortalFileFilters mp4Filters() {
    return {mp4Filter()};
}

QString portalToken() {
    return QStringLiteral("omacut_%1").arg(QRandomGenerator::global()->generate());
}

QByteArray portalPathBytes(const QString &path) {
    QByteArray bytes = path.toUtf8();
    bytes.append('\0');
    return bytes;
}

QString openFolder() {
    const QString videos = QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    return QDir(videos).exists() ? videos : QDir::homePath();
}
}

PortalFilePicker::PortalFilePicker(QObject *parent) : FilePicker(parent) {
    registerPortalFilterTypes();
}

void PortalFilePicker::openVideo() {
    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), QStringLiteral("Open"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("multiple"), false);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(openFolder()));
    options.insert(QStringLiteral("filters"), QVariant::fromValue(videoFilters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(videoFilter()));

    requestFile(QStringLiteral("OpenFile"), QStringLiteral("Open Video File"), options, Action::Open);
}

void PortalFilePicker::exportVideo(const QUrl &suggestedUrl, double start, double end,
                                   const QList<int> &scaleHeights) {
    const QFileInfo target(suggestedUrl.toLocalFile());

    QVariantMap options;
    options.insert(QStringLiteral("accept_label"), QStringLiteral("Export"));
    options.insert(QStringLiteral("modal"), true);
    options.insert(QStringLiteral("current_folder"), portalPathBytes(target.absolutePath()));
    options.insert(QStringLiteral("current_name"), target.fileName());
    options.insert(QStringLiteral("filters"), QVariant::fromValue(mp4Filters()));
    options.insert(QStringLiteral("current_filter"), QVariant::fromValue(mp4Filter()));

    // A "Quality" combo in the save dialog, only when there's a real downscale
    // to offer — sources at or below 720p just export as they are.
    if (!scaleHeights.isEmpty()) {
        PortalChoiceOptions qualities = {{QStringLiteral("original"), QStringLiteral("Original")}};
        for (const int height : scaleHeights)
            qualities.append({QString::number(height), QStringLiteral("%1p").arg(height)});
        options.insert(QStringLiteral("choices"),
                       QVariant::fromValue(PortalChoices{{QStringLiteral("quality"),
                                                          QStringLiteral("Quality"),
                                                          qualities,
                                                          QStringLiteral("original")}}));
    }

    if (requestFile(QStringLiteral("SaveFile"), QStringLiteral("Save Video File"),
                    options, Action::Export)) {
        m_pendingExportStart = start;
        m_pendingExportEnd = end;
    }
}

bool PortalFilePicker::connectToRequestPath(const QString &path) {
    m_pendingPath = path;
    return QDBusConnection::sessionBus().connect(
        QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
        this, SLOT(handleResponse(uint,QVariantMap)));
}

bool PortalFilePicker::requestFile(const QString &method, const QString &title,
                                   QVariantMap options, Action action) {
    if (m_pendingAction != Action::None)
        return false;

    QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusInterface portal(QStringLiteral("org.freedesktop.portal.Desktop"),
                          QStringLiteral("/org/freedesktop/portal/desktop"),
                          QStringLiteral("org.freedesktop.portal.FileChooser"),
                          bus);
    if (!portal.isValid()) {
        emit failed(QStringLiteral("The XDG desktop portal file chooser is not available."));
        return false;
    }

    // Subscribe to the Response signal at the request path the portal will
    // derive from our handle_token *before* making the call, so a response
    // can't slip past while our match rule is still being installed.
    const QString token = portalToken();
    options.insert(QStringLiteral("handle_token"), token);
    QString sender = bus.baseService().mid(1);
    sender.replace(QLatin1Char('.'), QLatin1Char('_'));
    const QString predictedPath =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);

    m_pendingAction = action;
    if (!connectToRequestPath(predictedPath)) {
        clearPending();
        emit failed(QStringLiteral("Could not listen for the portal file picker response."));
        return false;
    }

    auto *watcher = new QDBusPendingCallWatcher(
        portal.asyncCall(method, QString(), title, options), this);

    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher]() {
        QDBusPendingReply<QDBusObjectPath> reply = *watcher;
        watcher->deleteLater();

        // A response may already have been handled while the reply was in flight.
        if (m_pendingAction == Action::None)
            return;

        if (reply.isError()) {
            clearPending();
            emit failed(QStringLiteral("The portal file picker failed: %1")
                            .arg(reply.error().message()));
            return;
        }

        // Old portal versions can hand back a different request path than the
        // handle_token predicts; move our subscription over if so.
        const QString actualPath = reply.value().path();
        if (actualPath != m_pendingPath) {
            QDBusConnection::sessionBus().disconnect(
                QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
                QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
                this, SLOT(handleResponse(uint,QVariantMap)));
            if (!connectToRequestPath(actualPath)) {
                clearPending();
                emit failed(QStringLiteral("Could not listen for the portal file picker response."));
            }
        }
    });
    return true;
}

void PortalFilePicker::handleResponse(uint response, const QVariantMap &results) {
    const Action action = m_pendingAction;
    const double start = m_pendingExportStart;
    const double end = m_pendingExportEnd;
    clearPending();

    if (response != 0) {
        emit cancelled();
        return;
    }

    const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
    if (uris.isEmpty()) {
        emit cancelled();
        return;
    }

    const QUrl url(uris.first());
    if (action == Action::Open) {
        emit openSelected(url);
        return;
    }
    if (action != Action::Export)
        return;

    // The chosen quality rides along in the response: [("quality", "1080")],
    // with "original" (or no choices at all) meaning no downscale.
    int scaleHeight = 0;
    const QVariant choicesVar = results.value(QStringLiteral("choices"));
    if (choicesVar.canConvert<QDBusArgument>()) {
        const QDBusArgument arg = choicesVar.value<QDBusArgument>();
        arg.beginArray();
        while (!arg.atEnd()) {
            QString id;
            QString value;
            arg.beginStructure();
            arg >> id >> value;
            arg.endStructure();
            if (id == QStringLiteral("quality"))
                scaleHeight = value.toInt();  // "original" parses to 0
        }
        arg.endArray();
    }
    emit exportSelected(url, start, end, scaleHeight);
}

void PortalFilePicker::clearPending() {
    if (!m_pendingPath.isEmpty()) {
        QDBusConnection::sessionBus().disconnect(
            QStringLiteral("org.freedesktop.portal.Desktop"), m_pendingPath,
            QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"),
            this, SLOT(handleResponse(uint,QVariantMap)));
    }

    m_pendingPath.clear();
    m_pendingAction = Action::None;
}
