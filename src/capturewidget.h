#pragma once

#include <QWidget>
#include <QImage>
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
class QPaintEvent;
class QPainter;

// Full-screen capture overlay, à la Flameshot: dims the desktop, lets the user
// drag out a selection region (resizable via handles), then annotate it with a
// set of tools chosen from a floating toolbar before copying or saving.
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

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
        QString text;           // text tool
        QColor color;
        int number = 0;         // numbered marker
    };

    void buildToolbar();
    void positionToolbar();
    void setTool(Tool tool);
    void undo();
    void finishCopy();
    void save();

    Handle handleAt(const QPoint &pos) const;
    void updateCursor(const QPoint &pos);
    void commitText();

    QImage renderResult() const;
    void drawAnnotation(QPainter &painter, const Annotation &a, bool editing = false) const;
    QImage pixelated(const QRect &region) const;
    static QColor paleColor(const QColor &c);

    QImage m_screenshot;

    QRect m_selection;
    bool m_hasSelection = false;

    Tool m_tool = Tool::None;
    QColor m_color;
    QList<Annotation> m_annotations;
    int m_nextNumber = 1;

    // Drag state.
    Drag m_drag = Drag::None;
    Handle m_activeHandle = Handle::None;
    QPoint m_pressPos;
    QPoint m_dragCur;
    QRect m_selAtPress;
    Annotation m_current; // annotation being drawn

    // Text editing.
    bool m_editingText = false;
    int m_editIndex = -1;

    QWidget *m_toolbar = nullptr;
    QList<QPair<Tool, QPushButton *>> m_toolButtons;
    QButtonGroup *m_colorGroup = nullptr;
};
