#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDebug>
#include <QProcess>
#include <QStandardPaths>
#include "screenshotgrabber.h"
#include "capturewidget.h"

// Copy an image to the clipboard. On Wayland, QClipboard data is discarded the
// moment this process exits, so we hand the PNG to `wl-copy`, which daemonizes
// and keeps serving the selection. Falls back to QClipboard when wl-copy is
// absent (e.g. on X11, where a clipboard manager typically persists it).
static bool copyImageToClipboard(const QImage &image)
{
    const QString wlCopy = QStandardPaths::findExecutable("wl-copy");
    if (!wlCopy.isEmpty()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");

        QProcess process;
        process.start(wlCopy, {"--type", "image/png"});
        if (process.waitForStarted(2000)) {
            process.write(png);
            process.closeWriteChannel();
            // wl-copy forks a background copy of itself and then exits.
            process.waitForFinished(2000);
            return true;
        }
        qWarning() << "wl-copy failed to start; falling back to Qt clipboard";
    }

    QApplication::clipboard()->setImage(image);
    return false;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    
    QCoreApplication::setOrganizationName(APP_NAME);
    QCoreApplication::setApplicationName(APP_NAME);
    QCoreApplication::setApplicationVersion(APP_VERSION);

    QCommandLineParser parser;
    parser.setApplicationDescription(APP_NAME " - " APP_DESCRIPTION);
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption guiOption("gui", "Launch graphical capture mode");
    parser.addOption(guiOption);

    QCommandLineOption fullOption("full", "Capture fullscreen");
    parser.addOption(fullOption);

    QCommandLineOption screenOption("screen", "Capture a specific screen", "screen", "-1");
    parser.addOption(screenOption);

    parser.process(app);

    ScreenshotGrabber grabber;
    
    QImage screenshot;
    int screenIndex = parser.value(screenOption).toInt();
    
    if (parser.isSet(fullOption)) {
        screenshot = grabber.grabFullscreen();
    } else if (screenIndex >= 0) {
        screenshot = grabber.grabScreen(screenIndex);
    } else {
        screenshot = grabber.grabFullscreen();
    }

    if (screenshot.isNull()) {
        qCritical() << "Failed to capture screenshot";
        return -1;
    }

    if (parser.isSet(guiOption)) {
        CaptureWidget widget(screenshot);
        QObject::connect(&widget, &CaptureWidget::captureCompleted, [&widget](const QImage &image) {
            copyImageToClipboard(image);
            qInfo() << "Screenshot copied to clipboard";
            widget.close();
            QCoreApplication::quit();
        });
        
        QObject::connect(&widget, &CaptureWidget::captureAborted, [&widget]() {
            widget.close();
            QCoreApplication::quit();
        });
        
        widget.showFullScreen();

        return app.exec();
    } else {
        copyImageToClipboard(screenshot);
        qInfo() << "Screenshot captured and copied to clipboard";
        return 0;
    }
}
