#pragma once

#include <QWidget>
#include <QImage>
#include <QFont>
#include <QPoint>
#include <QRect>
#include <QList>
#include <QVector>
#include <QColor>
#include <QString>

class QPushButton;
class QButtonGroup;
class QMouseEvent;
class QKeyEvent;
class QWheelEvent;
class QPaintEvent;
class QPainter;

// Full-screen capture overlay, à la Flameshot: dims the desktop, lets the user
// drag out a selection region (resizable via handles), then annotate it with a
// set of tools chosen from a floating toolbar before copying or saving.
//
// Coordinate systems: every interactive coordinate (selection, annotation
// points) is in *widget logical* units, while the screenshot is in *device
// pixels*. On a HiDPI screen those differ by m_scale; imageRect()/widgetRect()
// convert between them, and everything that touches the screenshot goes
// through one of the two.
class CaptureWidget : public QWidget
{
    Q_OBJECT

public:
    explicit CaptureWidget(const QImage &screenshot, QWidget *parent = nullptr);
    ~CaptureWidget();

signals:
    // Carries the finished image (annotations baked in, cropped to the
    // selection) ready to be placed on the clipboard.
    void captureCompleted(const QImage &image);
    void captureAborted();
    // Ctrl+S wrote the capture to disk itself.
    void captureSaved(const QString &path);
    // Something went wrong after the user asked to finish (e.g. the save
    // failed); the caller should report it and exit non-zero.
    void captureFailed(const QString &message);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    enum class Tool { None, Arrow, Circle, Rectangle, Pen, Text, Highlight, Number, Blur };

    // Which part of the selection a press landed on (for resize/move).
    enum class Handle { None, Move, TopLeft, Top, TopRight, Right,
                        BottomRight, Bottom, BottomLeft, Left };

    // What the current mouse drag is doing.
    enum class Drag { None, NewSelection, Resize, MoveSelection, Draw };

    struct Annotation {
        Tool tool = Tool::None;
        QPoint p1;
        QPoint p2;
        QVector<QPoint> points; // freehand pen
        QString text;           // text tool (may contain '\n')
        QColor color;
        int penWidth = 3;
        int number = 0;         // numbered marker
    };

    void buildToolbar();
    void positionToolbar();
    void setTool(Tool tool);
    void pickColor();
    void addAnnotation(const Annotation &annotation);
    void undo();
    void redo();
    void nudgeSelection(int dx, int dy);
    void finishCopy();
    void save();

    Handle handleAt(const QPoint &pos) const;
    void updateCursor(const QPoint &pos);
    void beginTextEdit(int index);
    void commitText();
    void cancelText();
    int textAt(const QPoint &pos) const;
    QRect textBounds(const Annotation &annotation) const;
    QFont annotationFont() const;

    // Logical widget coordinates <-> screenshot device pixels.
    QRect imageRect(const QRect &logical) const;
    QRect widgetRect(const QRect &pixels) const;

    QImage renderResult() const;
    void drawAnnotation(QPainter &painter, const Annotation &annotation,
                        const QImage &blurSource, bool editing = false) const;
    // Bakes one annotation into a device-pixel image (used to build m_flattened).
    void bakeAnnotation(QImage &target, const Annotation &annotation) const;
    void rebuildFlattened();
    QImage mosaic(const QImage &source, const QRect &pixelRegion) const;

    QImage m_screenshot;
    // m_screenshot with every committed annotation baked in, at device-pixel
    // resolution. This is both what the selection renders from and what
    // renderResult() crops, so the overlay is a true preview of the output.
    QImage m_flattened;
    qreal m_scale = 1.0;

    QRect m_selection;
    bool m_hasSelection = false;

    Tool m_tool = Tool::None;
    QColor m_color;
    int m_penWidth = 3;
    QList<Annotation> m_annotations;
    QList<Annotation> m_redoStack;
    int m_nextNumber = 1;

    // Drag state.
    Drag m_drag = Drag::None;
    Handle m_activeHandle = Handle::None;
    QPoint m_pressPos;
    QPoint m_dragCur;
    QRect m_selAtPress;
    Annotation m_current; // annotation being drawn

    // Text editing. The annotation at m_editIndex is deliberately left out of
    // m_flattened while it is being edited, so the caret never gets baked in.
    bool m_editingText = false;
    int m_editIndex = -1;

    QWidget *m_toolbar = nullptr;
    QList<QPair<Tool, QPushButton *>> m_toolButtons;
    QButtonGroup *m_colorGroup = nullptr;
};
