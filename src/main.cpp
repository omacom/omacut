// omacut — a dead-simple video length trimmer. Qt Quick (QML) UI, ffmpeg cuts.

#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "thumbprovider.h"
#include "ytdlp.h"

int main(int argc, char *argv[]) {
    QGuiApplication app(argc, argv);
    app.setApplicationName("omacut");

    // Associates the window with omacut.desktop so the compositor (Wayland app_id
    // = this name) and taskbars pick up our installed icon.
    app.setDesktopFileName("omacut");
    app.setWindowIcon(QIcon::fromTheme("omacut"));

    // Modern, themeable controls (the same family Quickshell builds on).
    QQuickStyle::setStyle("Material");

    auto *provider = new ThumbProvider();
    Backend backend(provider, &app);

    QQmlApplicationEngine engine;

    // The engine takes ownership of the image provider.
    engine.addImageProvider("thumbs", provider);

    engine.rootContext()->setContextProperty("backend", &backend);

    engine.load(QUrl("qrc:/Main.qml"));
    if (engine.rootObjects().isEmpty())
        return -1;

    // Optionally open a file — or a link — passed on the command line.
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        const QString target = args.at(1);
        if (ytdlp::isLink(target))
            backend.openLink(target);
        else
            backend.load(QUrl::fromLocalFile(target));
    }

    return app.exec();
}
