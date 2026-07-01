#include "screenshotgrabber.h"

#include <QProcess>
#include <QStandardPaths>
#include <QDebug>

ScreenshotGrabber::ScreenshotGrabber(QObject *parent)
    : QObject(parent)
{
}

ScreenshotGrabber::~ScreenshotGrabber()
{
}

QImage ScreenshotGrabber::grabScreen(int screenIndex)
{
    QGuiApplication *app = static_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app) {
        return QImage();
    }

    const QList<QScreen *> screens = app->screens();
    if (screenIndex < 0 || screenIndex >= screens.size()) {
        return grabFullscreen();
    }

    QScreen *screen = screens[screenIndex];

    QImage image = grabViaQt(screen->geometry());
    if (!image.isNull()) {
        return image;
    }

    // Wayland fallback: capture just this output by its connector name.
    return grabViaGrim(screen->name());
}

QImage ScreenshotGrabber::grabFullscreen()
{
    QGuiApplication *app = static_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app) {
        return QImage();
    }

    QRect combinedRect;
    for (QScreen *screen : app->screens()) {
        combinedRect |= screen->geometry();
    }

    QImage image = grabViaQt(combinedRect);
    if (!image.isNull()) {
        return image;
    }

    // Wayland fallback: grim with no output captures the whole layout.
    return grabViaGrim();
}

QImage ScreenshotGrabber::grabViaQt(const QRect &region) const
{
    QScreen *screen = QGuiApplication::primaryScreen();
    if (!screen) {
        return QImage();
    }

    QPixmap pixmap;
    if (region.isNull()) {
        pixmap = screen->grabWindow(0);
    } else {
        pixmap = screen->grabWindow(0, region.x(), region.y(),
                                    region.width(), region.height());
    }
    return pixmap.toImage();
}

QImage ScreenshotGrabber::grabViaGrim(const QString &output) const
{
    const QString grim = QStandardPaths::findExecutable("grim");
    if (grim.isEmpty()) {
        qWarning() << "grim not found; install it to capture on Wayland "
                      "(e.g. `sudo pacman -S grim`)";
        return QImage();
    }

    QStringList args;
    if (!output.isEmpty()) {
        args << "-o" << output;
    }
    args << "-"; // write PNG to stdout

    QProcess process;
    process.start(grim, args);
    if (!process.waitForFinished(5000) || process.exitStatus() != QProcess::NormalExit
        || process.exitCode() != 0) {
        qWarning() << "grim failed:" << process.readAllStandardError().trimmed();
        return QImage();
    }

    QImage image;
    if (!image.loadFromData(process.readAllStandardOutput(), "PNG")) {
        qWarning() << "failed to decode grim output";
        return QImage();
    }
    return image;
}

QList<QRect> ScreenshotGrabber::availableGeometries() const
{
    QList<QRect> geometries;
    QGuiApplication *app = static_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!app) {
        return geometries;
    }

    for (QScreen *screen : app->screens()) {
        geometries.append(screen->geometry());
    }
    return geometries;
}
