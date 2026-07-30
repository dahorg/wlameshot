#include "capturewidget.h"

#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QFont>
#include <QFontMetrics>
#include <QLineF>
#include <QScreen>
#include <QCursor>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPushButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QFrame>
#include <QColorDialog>
#include <QFileDialog>
#include <QStandardPaths>
#include <QStringList>
#include <QDir>
#include <QDateTime>
#include <QDebug>

namespace {
const QColor kAccent(155, 89, 182);   // selection border/handles (purple)
const int kHandle = 5;                // handle dot radius
const int kHandleHit = 12;            // grab tolerance around a handle
const int kMinSize = 5;
const int kPenWidth = 3;
const int kMinPenWidth = 1;
const int kMaxPenWidth = 30;
const int kTextPixelSize = 22;
const int kMosaicBlock = 12;          // blur block size, in logical units

struct Swatch { const char *hex; };
const Swatch kColors[] = {
    {"#e21b1b"}, {"#f39c12"}, {"#f1c40f"}, {"#2ecc71"},
    {"#3498db"}, {"#ffffff"}, {"#111111"},
};

// Snap `to` onto the diagonal from `from`, so shapes come out square.
QPoint squared(const QPoint &from, const QPoint &to)
{
    const int d = qMax(qAbs(to.x() - from.x()), qAbs(to.y() - from.y()));
    return QPoint(from.x() + (to.x() >= from.x() ? d : -d),
                  from.y() + (to.y() >= from.y() ? d : -d));
}
} // namespace

CaptureWidget::CaptureWidget(const QImage &screenshot, QWidget *parent)
    : QWidget(parent),
      m_screenshot(screenshot),
      m_color(QColor(kColors[0].hex))
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::CrossCursor);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);

    // Wayland ignores client-set positions, so main covers the screen with
    // showFullScreen() rather than resize()+move(0,0).
    if (QScreen *screen = QGuiApplication::primaryScreen()) {
        const QSize logical = screen->size();
        resize(logical);

        // grim hands back device pixels; widget coordinates are logical units.
        // Without this factor the overlay would draw the screenshot magnified
        // by the scale factor and every crop would land in the wrong place.
        m_scale = screen->devicePixelRatio();

        const QSize expected(qRound(logical.width() * m_scale),
                             qRound(logical.height() * m_scale));
        if (!m_screenshot.isNull() && m_screenshot.size() != expected) {
            qWarning() << "screenshot is" << m_screenshot.size() << "but this screen expects"
                       << expected << "- the overlay covers the primary screen only, so"
                          " coordinates may not line up (multi-monitor or fractional scaling)";
        }
    }

    rebuildFlattened();
    buildToolbar();
}

CaptureWidget::~CaptureWidget() = default;

// ---------------------------------------------------------------------------
// Coordinate conversion
// ---------------------------------------------------------------------------

QRect CaptureWidget::imageRect(const QRect &logical) const
{
    if (m_scale == 1.0) {
        return logical;
    }
    return QRectF(logical.x() * m_scale, logical.y() * m_scale,
                  logical.width() * m_scale, logical.height() * m_scale).toAlignedRect();
}

QRect CaptureWidget::widgetRect(const QRect &pixels) const
{
    if (m_scale == 1.0) {
        return pixels;
    }
    return QRectF(pixels.x() / m_scale, pixels.y() / m_scale,
                  pixels.width() / m_scale, pixels.height() / m_scale).toAlignedRect();
}

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void CaptureWidget::buildToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setStyleSheet(
        "QWidget { background: #2b2b2b; border-radius: 8px; }"
        "QPushButton { background: #9b59b6; color: white; border: none;"
        "  border-radius: 6px; font-weight: bold; font-size: 18px; }"
        "QPushButton:hover { background: #a66bbe; }"
        "QPushButton:checked { background: #d64550; }");

    auto *layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    const QSize kButtonSize(40, 40);

    auto addTool = [&](const QString &text, const QString &tip, Tool tool) {
        auto *btn = new QPushButton(text, m_toolbar);
        btn->setToolTip(tip);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus); // keep key focus on the canvas
        btn->setFixedSize(kButtonSize);
        connect(btn, &QPushButton::clicked, this, [this, tool]() { setTool(tool); });
        layout->addWidget(btn);
        m_toolButtons.append({tool, btn});
        return btn;
    };

    addTool("➜", "Arrow (A) — click again to deselect", Tool::Arrow);
    addTool("◯", "Circle (C) — hold Shift for a circle", Tool::Circle);
    addTool("▭", "Rectangle (R) — hold Shift for a square", Tool::Rectangle);
    addTool("✎", "Freehand pen (P)", Tool::Pen);
    addTool("T", "Text (T) — Shift+Enter for a new line, click text to re-edit", Tool::Text);
    addTool("▬", "Highlight (H)", Tool::Highlight);
    addTool("①", "Numbered marker (N)", Tool::Number);
    addTool("▓", "Blur (B)", Tool::Blur);

    auto *sep1 = new QFrame(m_toolbar);
    sep1->setFrameShape(QFrame::VLine);
    sep1->setStyleSheet("color: #555;");
    layout->addWidget(sep1);

    // Colour swatches (exclusive).
    m_colorGroup = new QButtonGroup(this);
    m_colorGroup->setExclusive(true);
    for (const Swatch &sw : kColors) {
        auto *btn = new QPushButton(m_toolbar);
        btn->setCheckable(true);
        btn->setFixedSize(18, 18);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setToolTip(sw.hex);
        btn->setStyleSheet(QString("QPushButton { background: %1; border: 2px solid #2b2b2b;"
                                   " border-radius: 9px; }"
                                   "QPushButton:checked { border: 2px solid white; }")
                               .arg(sw.hex));
        const QColor c(sw.hex);
        connect(btn, &QPushButton::clicked, this, [this, c]() { m_color = c; });
        m_colorGroup->addButton(btn);
        layout->addWidget(btn);
    }
    m_colorGroup->buttons().first()->setChecked(true); // red

    auto *custom = new QPushButton("🎨", m_toolbar);
    custom->setToolTip("Custom colour… (right-click the canvas)");
    custom->setCursor(Qt::PointingHandCursor);
    custom->setFocusPolicy(Qt::NoFocus);
    custom->setFixedSize(kButtonSize);
    connect(custom, &QPushButton::clicked, this, [this]() { pickColor(); });
    layout->addWidget(custom);

    auto *sep2 = new QFrame(m_toolbar);
    sep2->setFrameShape(QFrame::VLine);
    sep2->setStyleSheet("color: #555;");
    layout->addWidget(sep2);

    auto addAction = [&](const QString &text, const QString &tip, auto slot) {
        auto *btn = new QPushButton(text, m_toolbar);
        btn->setToolTip(tip);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus);
        btn->setFixedSize(kButtonSize);
        connect(btn, &QPushButton::clicked, this, slot);
        layout->addWidget(btn);
    };

    addAction("↶", "Undo (Ctrl+Z)", [this]() { undo(); });
    addAction("↷", "Redo (Ctrl+Shift+Z)", [this]() { redo(); });
    addAction("💾", "Save (Ctrl+S)", [this]() { save(); });
    addAction("⧉", "Copy (Enter, or double-click the selection)", [this]() { finishCopy(); });
    addAction("✕", "Cancel (Esc)", [this]() { emit captureAborted(); });

    m_toolbar->adjustSize();
    m_toolbar->hide();
}

void CaptureWidget::positionToolbar()
{
    if (!m_hasSelection) {
        m_toolbar->hide();
        return;
    }

    m_toolbar->adjustSize();
    const QSize tb = m_toolbar->size();

    int x = m_selection.left();
    int y = m_selection.bottom() + 8;
    if (y + tb.height() > height()) {
        y = m_selection.top() - tb.height() - 8;
    }
    if (y < 0) {
        y = m_selection.top() + 8;
    }
    x = qBound(0, x, qMax(0, width() - tb.width()));
    y = qBound(0, y, qMax(0, height() - tb.height()));

    m_toolbar->move(x, y);
    m_toolbar->show();
    m_toolbar->raise();
}

void CaptureWidget::setTool(Tool tool)
{
    if (m_editingText) {
        commitText();
    }
    // Selecting the active tool again drops back to selection-editing mode,
    // which is the only way to move the selection by its interior again.
    m_tool = (m_tool == tool) ? Tool::None : tool;
    for (const auto &pair : m_toolButtons) {
        pair.second->setChecked(pair.first == m_tool);
    }
    updateCursor(mapFromGlobal(QCursor::pos()));
    update();
}

void CaptureWidget::pickColor()
{
    // The overlay is always-on-top, so get the toolbar out of the dialog's way.
    const bool toolbarWasVisible = m_toolbar && m_toolbar->isVisible();
    if (toolbarWasVisible) {
        m_toolbar->hide();
    }

    const QColor c = QColorDialog::getColor(m_color, this, "Pick colour");
    if (c.isValid()) {
        m_color = c;
        for (QAbstractButton *b : m_colorGroup->buttons()) {
            b->setChecked(false);
        }
    }

    if (toolbarWasVisible) {
        positionToolbar();
    }
}

void CaptureWidget::addAnnotation(const Annotation &annotation)
{
    m_annotations.append(annotation);
    m_redoStack.clear(); // a new edit invalidates the redo history
    rebuildFlattened();
    update();
}

void CaptureWidget::undo()
{
    if (m_editingText) {
        commitText();
    }
    if (m_annotations.isEmpty()) {
        return;
    }

    const Annotation removed = m_annotations.takeLast();
    // Hand the marker number back so the next one placed reuses it.
    if (removed.tool == Tool::Number) {
        m_nextNumber = qMax(1, removed.number);
    }
    m_redoStack.append(removed);
    rebuildFlattened();
    update();
}

void CaptureWidget::redo()
{
    if (m_editingText) {
        commitText();
    }
    if (m_redoStack.isEmpty()) {
        return;
    }

    const Annotation restored = m_redoStack.takeLast();
    if (restored.tool == Tool::Number) {
        m_nextNumber = qMax(m_nextNumber, restored.number + 1);
    }
    m_annotations.append(restored);
    rebuildFlattened();
    update();
}

// ---------------------------------------------------------------------------
// Selection handles
// ---------------------------------------------------------------------------

CaptureWidget::Handle CaptureWidget::handleAt(const QPoint &pos) const
{
    if (!m_hasSelection) {
        return Handle::None;
    }
    const QRect &s = m_selection;
    const int t = kHandleHit;

    auto near = [&](const QPoint &h) {
        return qAbs(pos.x() - h.x()) <= t && qAbs(pos.y() - h.y()) <= t;
    };

    if (near(s.topLeft())) return Handle::TopLeft;
    if (near(s.topRight())) return Handle::TopRight;
    if (near(s.bottomLeft())) return Handle::BottomLeft;
    if (near(s.bottomRight())) return Handle::BottomRight;
    if (near({s.center().x(), s.top()})) return Handle::Top;
    if (near({s.center().x(), s.bottom()})) return Handle::Bottom;
    if (near({s.left(), s.center().y()})) return Handle::Left;
    if (near({s.right(), s.center().y()})) return Handle::Right;
    if (s.contains(pos)) return Handle::Move;
    return Handle::None;
}

void CaptureWidget::updateCursor(const QPoint &pos)
{
    switch (handleAt(pos)) {
    case Handle::TopLeft:
    case Handle::BottomRight:
        setCursor(Qt::SizeFDiagCursor); return;
    case Handle::TopRight:
    case Handle::BottomLeft:
        setCursor(Qt::SizeBDiagCursor); return;
    case Handle::Top:
    case Handle::Bottom:
        setCursor(Qt::SizeVerCursor); return;
    case Handle::Left:
    case Handle::Right:
        setCursor(Qt::SizeHorCursor); return;
    case Handle::Move:
        setCursor(m_tool == Tool::None ? Qt::SizeAllCursor : Qt::CrossCursor); return;
    case Handle::None:
        setCursor(Qt::CrossCursor); return;
    }
}

void CaptureWidget::nudgeSelection(int dx, int dy)
{
    if (!m_hasSelection) {
        return;
    }
    QRect s = m_selection.translated(dx, dy);
    s.moveLeft(qBound(0, s.left(), qMax(0, width() - s.width())));
    s.moveTop(qBound(0, s.top(), qMax(0, height() - s.height())));
    m_selection = s;
    positionToolbar();
    update();
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void CaptureWidget::mousePressEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();

    if (event->button() == Qt::RightButton) {
        // Right-click backs out of whatever is in progress, and opens the
        // colour picker when there is nothing to back out of.
        if (m_drag == Drag::Draw || m_drag == Drag::NewSelection) {
            m_drag = Drag::None;
            update();
        } else if (m_editingText) {
            cancelText();
        } else if (m_tool != Tool::None) {
            setTool(m_tool); // toggle off
        } else {
            pickColor();
        }
        return;
    }

    if (event->button() != Qt::LeftButton) {
        return;
    }

    if (m_editingText) {
        commitText();
    }

    m_pressPos = pos;
    m_dragCur = pos;

    if (m_hasSelection) {
        const Handle h = handleAt(pos);

        if (h != Handle::None && h != Handle::Move) {
            m_drag = Drag::Resize;
            m_activeHandle = h;
            m_selAtPress = m_selection;
            return;
        }
        if (m_selection.contains(pos)) {
            if (m_tool == Tool::None) {
                m_drag = Drag::MoveSelection;
                m_selAtPress = m_selection;
            } else if (m_tool == Tool::Text && textAt(pos) >= 0) {
                // Clicking existing text reopens it instead of stacking a new box.
                beginTextEdit(textAt(pos));
            } else {
                m_drag = Drag::Draw;
                m_current = Annotation{};
                m_current.tool = m_tool;
                m_current.color = m_color;
                m_current.penWidth = m_penWidth;
                m_current.p1 = pos;
                m_current.p2 = pos;
                if (m_tool == Tool::Pen) {
                    m_current.points = {pos};
                }
            }
            return;
        }
    }

    // Otherwise start a fresh selection.
    m_drag = Drag::NewSelection;
    m_hasSelection = false;
    m_toolbar->hide();
    update();
}

void CaptureWidget::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();

    switch (m_drag) {
    case Drag::None:
        updateCursor(pos);
        return;
    case Drag::NewSelection:
        m_dragCur = (event->modifiers() & Qt::ShiftModifier) ? squared(m_pressPos, pos) : pos;
        break;
    case Drag::Resize: {
        const QRect s = m_selAtPress;
        int l = s.left(), r = s.right(), t = s.top(), b = s.bottom();
        switch (m_activeHandle) {
        case Handle::TopLeft:     l = pos.x(); t = pos.y(); break;
        case Handle::Top:         t = pos.y(); break;
        case Handle::TopRight:    r = pos.x(); t = pos.y(); break;
        case Handle::Right:       r = pos.x(); break;
        case Handle::BottomRight: r = pos.x(); b = pos.y(); break;
        case Handle::Bottom:      b = pos.y(); break;
        case Handle::BottomLeft:  l = pos.x(); b = pos.y(); break;
        case Handle::Left:        l = pos.x(); break;
        default: break;
        }

        // Clamp to the widget first, then hold the dragged edge kMinSize away
        // from its opposite number so the selection can never collapse (or
        // invert) into something that crops to an empty image.
        const int maxX = qMax(0, width() - 1);
        const int maxY = qMax(0, height() - 1);
        l = qBound(0, l, maxX); r = qBound(0, r, maxX);
        t = qBound(0, t, maxY); b = qBound(0, b, maxY);

        const bool dragsLeft = m_activeHandle == Handle::TopLeft
                               || m_activeHandle == Handle::Left
                               || m_activeHandle == Handle::BottomLeft;
        const bool dragsRight = m_activeHandle == Handle::TopRight
                                || m_activeHandle == Handle::Right
                                || m_activeHandle == Handle::BottomRight;
        const bool dragsTop = m_activeHandle == Handle::TopLeft
                              || m_activeHandle == Handle::Top
                              || m_activeHandle == Handle::TopRight;
        const bool dragsBottom = m_activeHandle == Handle::BottomLeft
                                 || m_activeHandle == Handle::Bottom
                                 || m_activeHandle == Handle::BottomRight;

        if (r - l + 1 < kMinSize) {
            if (dragsLeft) l = qMax(0, r - kMinSize + 1);
            else if (dragsRight) r = qMin(maxX, l + kMinSize - 1);
        }
        if (b - t + 1 < kMinSize) {
            if (dragsTop) t = qMax(0, b - kMinSize + 1);
            else if (dragsBottom) b = qMin(maxY, t + kMinSize - 1);
        }

        const QRect resized = QRect(QPoint(l, t), QPoint(r, b)).normalized();
        if (resized.width() >= kMinSize && resized.height() >= kMinSize) {
            m_selection = resized;
        }
        break;
    }
    case Drag::MoveSelection: {
        QRect s = m_selAtPress.translated(pos - m_pressPos);
        // Keep inside the widget. QRect::right() is left+width-1, so the last
        // valid edge is width()-1 -- clamping the origin avoids that off-by-one.
        s.moveLeft(qBound(0, s.left(), qMax(0, width() - s.width())));
        s.moveTop(qBound(0, s.top(), qMax(0, height() - s.height())));
        m_selection = s;
        break;
    }
    case Drag::Draw:
        // Shift squares off the box-shaped tools; a freehand or arrow stroke
        // has no meaningful aspect ratio to lock.
        switch (m_current.tool) {
        case Tool::Circle:
        case Tool::Rectangle:
        case Tool::Highlight:
        case Tool::Blur:
            m_current.p2 = (event->modifiers() & Qt::ShiftModifier)
                               ? squared(m_current.p1, pos) : pos;
            break;
        default:
            m_current.p2 = pos;
            break;
        }
        if (m_current.tool == Tool::Pen) {
            m_current.points.append(pos);
        }
        break;
    }

    if (m_drag == Drag::Resize || m_drag == Drag::MoveSelection) {
        positionToolbar();
    }
    update();
}

void CaptureWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint pos = event->pos();
    const Drag drag = m_drag;
    m_drag = Drag::None;

    switch (drag) {
    case Drag::NewSelection: {
        const QRect sel = QRect(m_pressPos, m_dragCur).normalized() & rect();
        if (sel.width() >= kMinSize && sel.height() >= kMinSize) {
            m_selection = sel;
            m_hasSelection = true;
            positionToolbar();
        } else {
            m_hasSelection = false;
            m_toolbar->hide();
        }
        break;
    }
    case Drag::Resize:
    case Drag::MoveSelection:
        positionToolbar();
        break;
    case Drag::Draw: {
        if (m_current.tool == Tool::Number) {
            // Click places an auto-incrementing marker.
            Annotation a = m_current;
            a.p1 = pos;
            a.number = m_nextNumber++;
            addAnnotation(a);
        } else if (m_current.tool == Tool::Text) {
            // Click starts a text box in edit mode.
            Annotation a = m_current;
            a.p1 = pos;
            a.text.clear();
            m_annotations.append(a);
            m_redoStack.clear();
            beginTextEdit(m_annotations.size() - 1);
        } else {
            const QRect r = QRect(m_current.p1, m_current.p2).normalized();
            const bool bigEnough = m_current.tool == Tool::Pen
                                       ? m_current.points.size() > 1
                                       : (r.width() > 2 || r.height() > 2);
            if (bigEnough) {
                addAnnotation(m_current);
            }
        }
        break;
    }
    case Drag::None:
        break;
    }

    update();
}

void CaptureWidget::mouseDoubleClickEvent(QMouseEvent *event)
{
    // Double-clicking the selection is the quickest path to the clipboard, but
    // only when no tool is armed -- otherwise it would swallow a second stroke.
    if (event->button() == Qt::LeftButton && m_tool == Tool::None
        && m_hasSelection && m_selection.contains(event->pos())) {
        m_drag = Drag::None;
        finishCopy();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void CaptureWidget::wheelEvent(QWheelEvent *event)
{
    if (m_tool == Tool::None) {
        QWidget::wheelEvent(event);
        return;
    }

    const int steps = event->angleDelta().y() / 120;
    if (steps != 0) {
        m_penWidth = qBound(kMinPenWidth, m_penWidth + steps, kMaxPenWidth);
        if (m_drag == Drag::Draw) {
            m_current.penWidth = m_penWidth;
        }
        update();
    }
    event->accept();
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void CaptureWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_editingText) {
        // Defensive: never index m_annotations on a stale edit target.
        if (m_editIndex < 0 || m_editIndex >= m_annotations.size()) {
            m_editingText = false;
            m_editIndex = -1;
            update();
            return;
        }

        switch (event->key()) {
        case Qt::Key_Escape:
            cancelText();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            if (event->modifiers() & Qt::ShiftModifier) {
                m_annotations[m_editIndex].text += QChar('\n');
                update();
            } else {
                commitText();
            }
            return;
        case Qt::Key_Backspace:
            m_annotations[m_editIndex].text.chop(1);
            update();
            return;
        case Qt::Key_V:
            if (event->modifiers() & Qt::ControlModifier) {
                m_annotations[m_editIndex].text += QApplication::clipboard()->text();
                update();
                return;
            }
            break;
        default:
            break;
        }

        // Accept anything printable, including multi-character input from an
        // input method; reject control keys so they don't insert garbage.
        const QString typed = event->text();
        bool printable = !typed.isEmpty();
        for (const QChar &ch : typed) {
            if (!ch.isPrint()) {
                printable = false;
                break;
            }
        }
        if (printable) {
            m_annotations[m_editIndex].text += typed;
            update();
        }
        return;
    }

    const bool ctrl = event->modifiers() & Qt::ControlModifier;
    const bool shift = event->modifiers() & Qt::ShiftModifier;
    const int step = shift ? 10 : 1;

    switch (event->key()) {
    case Qt::Key_Escape:  emit captureAborted(); break;
    case Qt::Key_Return:
    case Qt::Key_Enter:   finishCopy(); break;
    case Qt::Key_Left:    nudgeSelection(-step, 0); break;
    case Qt::Key_Right:   nudgeSelection(step, 0); break;
    case Qt::Key_Up:      nudgeSelection(0, -step); break;
    case Qt::Key_Down:    nudgeSelection(0, step); break;
    case Qt::Key_A: if (m_hasSelection) setTool(Tool::Arrow); break;
    case Qt::Key_C: if (ctrl) finishCopy(); else if (m_hasSelection) setTool(Tool::Circle); break;
    case Qt::Key_R: if (m_hasSelection) setTool(Tool::Rectangle); break;
    case Qt::Key_P: if (m_hasSelection) setTool(Tool::Pen); break;
    case Qt::Key_T: if (m_hasSelection) setTool(Tool::Text); break;
    case Qt::Key_H: if (m_hasSelection) setTool(Tool::Highlight); break;
    case Qt::Key_N: if (m_hasSelection) setTool(Tool::Number); break;
    case Qt::Key_B: if (m_hasSelection) setTool(Tool::Blur); break;
    case Qt::Key_Z: if (ctrl) { if (shift) redo(); else undo(); } break;
    case Qt::Key_Y: if (ctrl) redo(); break;
    case Qt::Key_S: if (ctrl) save(); break;
    default: QWidget::keyPressEvent(event);
    }
}

// ---------------------------------------------------------------------------
// Text editing
// ---------------------------------------------------------------------------

QFont CaptureWidget::annotationFont() const
{
    QFont f = font();
    f.setPixelSize(kTextPixelSize);
    f.setBold(true);
    return f;
}

QRect CaptureWidget::textBounds(const Annotation &annotation) const
{
    const QFontMetrics fm(annotationFont());
    const QStringList lines = annotation.text.split(QChar('\n'));
    int w = 0;
    for (const QString &line : lines) {
        w = qMax(w, fm.horizontalAdvance(line));
    }
    return QRect(annotation.p1.x(), annotation.p1.y(),
                 qMax(w, kTextPixelSize), fm.lineSpacing() * lines.size());
}

int CaptureWidget::textAt(const QPoint &pos) const
{
    // Topmost first, so the most recently placed box wins an overlap.
    for (int i = m_annotations.size() - 1; i >= 0; --i) {
        if (m_annotations[i].tool == Tool::Text
            && textBounds(m_annotations[i]).adjusted(-4, -4, 4, 4).contains(pos)) {
            return i;
        }
    }
    return -1;
}

void CaptureWidget::beginTextEdit(int index)
{
    if (index < 0 || index >= m_annotations.size()) {
        return;
    }
    m_editingText = true;
    m_editIndex = index;
    // The edited box is drawn live rather than baked, so the caret stays out
    // of the flattened image.
    rebuildFlattened();
    update();
}

void CaptureWidget::commitText()
{
    if (m_editIndex >= 0 && m_editIndex < m_annotations.size()
        && m_annotations[m_editIndex].text.isEmpty()) {
        m_annotations.removeAt(m_editIndex); // discard empty box
    }
    m_editingText = false;
    m_editIndex = -1;
    rebuildFlattened();
    update();
}

void CaptureWidget::cancelText()
{
    if (m_editIndex >= 0 && m_editIndex < m_annotations.size()) {
        m_annotations.removeAt(m_editIndex);
    }
    m_editingText = false;
    m_editIndex = -1;
    rebuildFlattened();
    update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

QImage CaptureWidget::mosaic(const QImage &source, const QRect &pixelRegion) const
{
    const QRect r = pixelRegion.normalized() & source.rect();
    if (r.isEmpty()) {
        return QImage();
    }
    // Keep the blocks a constant size on screen regardless of display scaling.
    const int factor = qMax(2, qRound(kMosaicBlock * m_scale));
    const QImage sub = source.copy(r);
    const QImage small = sub.scaled(qMax(1, r.width() / factor), qMax(1, r.height() / factor),
                                    Qt::IgnoreAspectRatio, Qt::FastTransformation);
    return small.scaled(r.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

void CaptureWidget::drawAnnotation(QPainter &painter, const Annotation &a,
                                   const QImage &blurSource, bool editing) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    switch (a.tool) {
    case Tool::Arrow: {
        painter.setPen(QPen(a.color, a.penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(a.p1, a.p2);
        const QLineF line(a.p1, a.p2);
        const double len = qMax(10.0, a.penWidth * 5.0);
        QLineF h1(a.p2, a.p1); h1.setAngle(line.angle() + 150); h1.setLength(len);
        QLineF h2(a.p2, a.p1); h2.setAngle(line.angle() - 150); h2.setLength(len);
        painter.drawLine(QLineF(a.p2, h1.p2()));
        painter.drawLine(QLineF(a.p2, h2.p2()));
        break;
    }
    case Tool::Circle:
        painter.setPen(QPen(a.color, a.penWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRect(a.p1, a.p2).normalized());
        break;
    case Tool::Rectangle:
        painter.setPen(QPen(a.color, a.penWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRect(a.p1, a.p2).normalized());
        break;
    case Tool::Pen: {
        painter.setPen(QPen(a.color, a.penWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (a.points.size() > 1) {
            painter.drawPolyline(a.points.constData(), a.points.size());
        }
        break;
    }
    case Tool::Highlight: {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.setPen(Qt::NoPen);
        // Blend halfway to white so a Multiply composition tints rather than covers.
        painter.setBrush(QColor((a.color.red() + 255) / 2, (a.color.green() + 255) / 2,
                                (a.color.blue() + 255) / 2));
        painter.drawRect(QRect(a.p1, a.p2).normalized());
        break;
    }
    case Tool::Blur: {
        // Only reached for the in-progress preview; committed blurs go through
        // bakeAnnotation(), which samples the image it is drawing into.
        const QRect pixels = imageRect(QRect(a.p1, a.p2).normalized()) & blurSource.rect();
        const QImage px = mosaic(blurSource, pixels);
        if (!px.isNull()) {
            painter.drawImage(widgetRect(pixels), px);
        }
        break;
    }
    case Tool::Text: {
        const QFont font = annotationFont();
        painter.setFont(font);
        painter.setPen(a.color);
        const QFontMetrics fm(font);
        QStringList lines = a.text.split(QChar('\n'));
        if (editing) {
            lines.last() += QChar('|'); // caret
        }
        int baseline = a.p1.y() + fm.ascent();
        for (const QString &line : lines) {
            painter.drawText(a.p1.x(), baseline, line);
            baseline += fm.lineSpacing();
        }
        break;
    }
    case Tool::Number: {
        const int rad = qMax(10, 12 + a.penWidth);
        painter.setPen(Qt::NoPen);
        painter.setBrush(a.color);
        painter.drawEllipse(a.p1, rad, rad);
        QFont font = painter.font();
        font.setPixelSize(qMax(10, rad + 2));
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(Qt::white);
        painter.drawText(QRect(a.p1.x() - rad, a.p1.y() - rad, rad * 2, rad * 2),
                         Qt::AlignCenter, QString::number(a.number));
        break;
    }
    case Tool::None:
        break;
    }

    painter.restore();
}

void CaptureWidget::bakeAnnotation(QImage &target, const Annotation &a) const
{
    if (a.tool == Tool::Blur) {
        // Sample the target *before* opening a painter on it, so a blur drawn
        // over earlier annotations obscures them instead of restoring the
        // original pixels underneath.
        const QRect pixels = imageRect(QRect(a.p1, a.p2).normalized()) & target.rect();
        const QImage px = mosaic(target, pixels);
        if (px.isNull()) {
            return;
        }
        QPainter painter(&target);
        painter.drawImage(pixels.topLeft(), px);
        return;
    }

    QPainter painter(&target);
    // Annotation coordinates are logical; the target is in device pixels.
    painter.scale(m_scale, m_scale);
    drawAnnotation(painter, a, target);
}

void CaptureWidget::rebuildFlattened()
{
    m_flattened = m_screenshot.copy();
    for (int i = 0; i < m_annotations.size(); ++i) {
        if (m_editingText && i == m_editIndex) {
            continue; // drawn live, with a caret, so it must not be baked yet
        }
        bakeAnnotation(m_flattened, m_annotations[i]);
    }
}

void CaptureWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    // Outside the selection: the untouched desktop, dimmed. Annotations only
    // ever show through inside the selection, which is also all that gets
    // cropped into the result.
    painter.drawImage(rect(), m_screenshot, imageRect(rect()) & m_screenshot.rect());
    painter.fillRect(rect(), QColor(0, 0, 0, 130)); // dim

    QRect sel = m_selection;
    if (m_drag == Drag::NewSelection) {
        sel = QRect(m_pressPos, m_dragCur).normalized();
    }

    if (!sel.isNull() && (m_hasSelection || m_drag == Drag::NewSelection)) {
        // Brighten the selection, showing the flattened (annotated) pixels.
        painter.drawImage(sel, m_flattened, imageRect(sel) & m_flattened.rect());

        // In-progress work is clipped to the selection so a stroke can't spill
        // out over the dimmed desktop.
        painter.save();
        painter.setClipRect(sel);
        if (m_drag == Drag::Draw) {
            drawAnnotation(painter, m_current, m_flattened);
        }
        if (m_editingText && m_editIndex >= 0 && m_editIndex < m_annotations.size()) {
            drawAnnotation(painter, m_annotations[m_editIndex], m_flattened, true);
        }
        painter.restore();

        painter.setPen(QPen(kAccent, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(sel);

        painter.setBrush(kAccent);
        painter.setPen(Qt::NoPen);
        const QVector<QPoint> handles = {
            sel.topLeft(), sel.topRight(), sel.bottomLeft(), sel.bottomRight(),
            {sel.center().x(), sel.top()}, {sel.center().x(), sel.bottom()},
            {sel.left(), sel.center().y()}, {sel.right(), sel.center().y()}};
        for (const QPoint &h : handles) {
            painter.drawEllipse(h, kHandle, kHandle);
        }

        // Report the size of what will actually be written out (device pixels),
        // plus the pen width, which is otherwise invisible until you draw.
        const QSize out = imageRect(sel).size();
        QString label = QString("%1 x %2").arg(out.width()).arg(out.height());
        if (m_tool != Tool::None) {
            label += QString("  •  pen %1").arg(m_penWidth);
        }
        const QFontMetrics fm(painter.fontMetrics());
        painter.setPen(Qt::white);
        painter.drawText(sel.right() - fm.horizontalAdvance(label) - 6,
                         sel.bottom() - 8, label);
    }

    if (!m_hasSelection && m_drag != Drag::NewSelection) {
        painter.setPen(Qt::white);
        painter.drawText(rect(), Qt::AlignCenter,
                         "Drag to select a region  •  Esc to cancel");
    }
}

// ---------------------------------------------------------------------------
// Output
// ---------------------------------------------------------------------------

QImage CaptureWidget::renderResult() const
{
    // m_flattened already holds every committed annotation at device-pixel
    // resolution, so finishing is just a crop.
    if (!m_hasSelection) {
        return m_flattened;
    }
    const QRect crop = imageRect(m_selection) & m_flattened.rect();
    return crop.isEmpty() ? QImage() : m_flattened.copy(crop);
}

void CaptureWidget::finishCopy()
{
    if (m_editingText) {
        commitText();
    }
    if (!m_hasSelection) {
        return;
    }

    const QImage result = renderResult();
    if (result.isNull()) {
        emit captureFailed("Selection does not overlap the captured image; nothing to copy");
        return;
    }
    emit captureCompleted(result);
}

void CaptureWidget::save()
{
    if (m_editingText) {
        commitText();
    }
    if (!m_hasSelection) {
        return;
    }

    const QImage result = renderResult();
    if (result.isNull()) {
        emit captureFailed("Selection does not overlap the captured image; nothing to save");
        return;
    }

    const QString dir = QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
    const QString suggested = QString("%1/Screenshot-%2.png")
                                  .arg(dir.isEmpty() ? QDir::homePath() : dir)
                                  .arg(QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss"));

    m_toolbar->hide();
    const QString path = QFileDialog::getSaveFileName(
        this, "Save Screenshot", suggested, "PNG Image (*.png)");

    if (path.isEmpty()) {
        positionToolbar();
        return;
    }

    if (result.save(path)) {
        emit captureSaved(path);
    } else {
        emit captureFailed(QString("Failed to save screenshot to %1").arg(path));
    }
}
