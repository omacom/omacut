// omacut — a dead-simple video length trimmer. Qt Quick (QML) UI, ffmpeg cuts.

#ifdef OMACUT_PORTAL_FILE_PICKER
#include <QGuiApplication>
#else
// QFileDialog is a widget, so the non-portal picker needs a QApplication.
#include <QApplication>
#endif
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QUrl>

#include "backend.h"
#include "thumbprovider.h"

int main(int argc, char *argv[]) {
#ifdef OMACUT_PORTAL_FILE_PICKER
    QGuiApplication app(argc, argv);
#else
    QApplication app(argc, argv);
#endif
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

    // Optionally open a file passed on the command line.
    const QStringList args = app.arguments();
    if (args.size() > 1)
        backend.load(QUrl::fromLocalFile(args.at(1)));

    return app.exec();
}
