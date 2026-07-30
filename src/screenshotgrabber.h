#pragma once

#include <QObject>
#include <QRect>
#include <QImage>
#include <QScreen>
#include <QPixmap>
#include <QGuiApplication>

class ScreenshotGrabber : public QObject
{
    Q_OBJECT

public:
    explicit ScreenshotGrabber(QObject *parent = nullptr);
    ~ScreenshotGrabber();

    // Captures one output by index. Returns a null image if the index is out
    // of range -- callers should validate and report, rather than silently
    // receiving a different screen than they asked for.
    QImage grabScreen(int screenIndex);
    QImage grabFullscreen();

private:
    // Native Qt capture. Returns a null image on Wayland (wlroots/GNOME
    // reject QScreen::grabWindow), which triggers the grim fallback below.
    QImage grabViaQt(const QRect &region = QRect()) const;

    // Fallback capture via the `grim` CLI, the standard screenshot tool for
    // wlroots-based Wayland compositors (Hyprland, Sway, ...). An empty
    // output name captures the whole layout; otherwise a single output.
    QImage grabViaGrim(const QString &output = QString()) const;
};


