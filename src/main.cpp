#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QCommandLineParser>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>
#include <QTextStream>
#include <cstdio>
#include "screenshotgrabber.h"
#include "capturewidget.h"

// Several distros (Arch, Fedora) build Qt with journald support, which routes
// qInfo()/qWarning()/qCritical() into the systemd journal instead of the
// terminal -- so a user running this from a shell or a keybind would never see
// "grim not found" or "failed to save". A CLI tool has to talk to its terminal,
// so take the messages back and print them plainly, without the Qt decoration.
static void messageHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    QTextStream stream(type == QtInfoMsg ? stdout : stderr);
    stream << msg << Qt::endl;
}

// Copy an image to the clipboard. On Wayland, QClipboard data is discarded the
// moment this process exits, so we hand the PNG to `wl-copy`, which daemonizes
// and keeps serving the selection. Falls back to QClipboard when wl-copy is
// absent (e.g. on X11, where a clipboard manager typically persists it).
// Returns false if the image could not be handed off.
static bool copyImageToClipboard(const QImage &image)
{
    const QString wlCopy = QStandardPaths::findExecutable("wl-copy");
    if (!wlCopy.isEmpty()) {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        if (!image.save(&buffer, "PNG")) {
            qWarning() << "failed to encode the capture as PNG";
            return false;
        }

        QProcess process;
        process.start(wlCopy, {"--type", "image/png"});
        if (process.waitForStarted(2000)) {
            process.write(png);
            process.closeWriteChannel();
            // wl-copy forks a background copy of itself and then exits.
            if (!process.waitForFinished(2000)) {
                // Unexpected, but the data is already written; assume the
                // background copy took ownership rather than failing loudly.
                qWarning() << "wl-copy did not exit within 2s; assuming it took the selection";
                return true;
            }
            if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
                qWarning() << "wl-copy failed:" << process.readAllStandardError().trimmed();
                return false;
            }
            return true;
        }
        qWarning() << "wl-copy failed to start; falling back to Qt clipboard";
    }

    QApplication::clipboard()->setImage(image);
    return !QApplication::clipboard()->image().isNull();
}

static bool writeImageToFile(const QImage &image, const QString &path)
{
    if (!image.save(path)) {
        qCritical() << "Failed to write capture to" << path;
        return false;
    }
    qInfo() << "Saved screenshot to" << QFileInfo(path).absoluteFilePath();
    return true;
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(messageHandler);

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

    QCommandLineOption screenOption("screen", "Capture a specific screen by index", "index");
    parser.addOption(screenOption);

    QCommandLineOption outputOption(
        {"o", "output"}, "Write the capture to <file> instead of the clipboard", "file");
    parser.addOption(outputOption);

    parser.process(app);

    int screenIndex = -1;
    if (parser.isSet(screenOption)) {
        bool ok = false;
        screenIndex = parser.value(screenOption).toInt(&ok);
        if (!ok || screenIndex < 0) {
            qCritical().noquote()
                << QString("Invalid --screen value '%1': expected a screen index (0, 1, ...)")
                       .arg(parser.value(screenOption));
            return 1;
        }
        const int screenCount = QGuiApplication::screens().size();
        if (screenIndex >= screenCount) {
            QStringList names;
            const QList<QScreen *> screens = QGuiApplication::screens();
            for (int i = 0; i < screens.size(); ++i) {
                names << QString("%1=%2").arg(i).arg(screens[i]->name());
            }
            qCritical().noquote()
                << QString("--screen %1 is out of range; available screens: %2")
                       .arg(screenIndex).arg(names.join(", "));
            return 1;
        }
    }

    const QString outputPath = parser.value(outputOption);

    ScreenshotGrabber grabber;
    const QImage screenshot = screenIndex >= 0 ? grabber.grabScreen(screenIndex)
                                               : grabber.grabFullscreen();

    if (screenshot.isNull()) {
        qCritical() << "Failed to capture screenshot";
        return 1;
    }

    if (!parser.isSet(guiOption)) {
        if (!outputPath.isEmpty()) {
            return writeImageToFile(screenshot, outputPath) ? 0 : 1;
        }
        if (!copyImageToClipboard(screenshot)) {
            qCritical() << "Failed to copy screenshot to clipboard";
            return 1;
        }
        qInfo() << "Screenshot captured and copied to clipboard";
        return 0;
    }

    CaptureWidget widget(screenshot);

    QObject::connect(&widget, &CaptureWidget::captureCompleted,
                     [&widget, outputPath](const QImage &image) {
        bool ok = false;
        if (!outputPath.isEmpty()) {
            ok = writeImageToFile(image, outputPath);
        } else {
            ok = copyImageToClipboard(image);
            if (ok) {
                qInfo() << "Screenshot copied to clipboard";
            } else {
                qCritical() << "Failed to copy screenshot to clipboard";
            }
        }
        widget.close();
        QCoreApplication::exit(ok ? 0 : 1);
    });

    QObject::connect(&widget, &CaptureWidget::captureSaved, [&widget](const QString &path) {
        qInfo() << "Saved screenshot to" << QFileInfo(path).absoluteFilePath();
        widget.close();
        QCoreApplication::exit(0);
    });

    QObject::connect(&widget, &CaptureWidget::captureFailed, [&widget](const QString &message) {
        qCritical().noquote() << message;
        widget.close();
        QCoreApplication::exit(1);
    });

    QObject::connect(&widget, &CaptureWidget::captureAborted, [&widget]() {
        widget.close();
        QCoreApplication::exit(0);
    });

    widget.showFullScreen();

    return app.exec();
}
