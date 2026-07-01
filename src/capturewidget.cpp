#include "capturewidget.h"

#include <QApplication>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QFont>
#include <QFontMetrics>
#include <QLineF>
#include <QScreen>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPushButton>
#include <QButtonGroup>
#include <QHBoxLayout>
#include <QFrame>
#include <QColorDialog>
#include <QFileDialog>
#include <QStandardPaths>
#include <QDir>
#include <QDateTime>
#include <QDebug>

namespace {
const QColor kAccent(155, 89, 182);   // selection border/handles (purple)
const int kHandle = 5;                // handle dot radius
const int kHandleHit = 12;            // grab tolerance around a handle
const int kMinSize = 5;
const int kPenWidth = 3;

struct Swatch { const char *hex; };
const Swatch kColors[] = {
    {"#e21b1b"}, {"#f39c12"}, {"#f1c40f"}, {"#2ecc71"},
    {"#3498db"}, {"#ffffff"}, {"#111111"},
};
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
        resize(screen->size());
    }

    buildToolbar();
}

CaptureWidget::~CaptureWidget() = default;

// ---------------------------------------------------------------------------
// Toolbar
// ---------------------------------------------------------------------------

void CaptureWidget::buildToolbar()
{
    m_toolbar = new QWidget(this);
    m_toolbar->setStyleSheet(
        "QWidget { background: #2b2b2b; border-radius: 8px; }"
        "QPushButton { background: #9b59b6; color: white; border: none;"
        "  border-radius: 6px; padding: 6px 9px; font-weight: bold; }"
        "QPushButton:hover { background: #a66bbe; }"
        "QPushButton:checked { background: #d64550; }");

    auto *layout = new QHBoxLayout(m_toolbar);
    layout->setContentsMargins(6, 6, 6, 6);
    layout->setSpacing(4);

    auto addTool = [&](const QString &text, const QString &tip, Tool tool) {
        auto *btn = new QPushButton(text, m_toolbar);
        btn->setToolTip(tip);
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setFocusPolicy(Qt::NoFocus); // keep key focus on the canvas
        connect(btn, &QPushButton::clicked, this, [this, tool]() { setTool(tool); });
        layout->addWidget(btn);
        m_toolButtons.append({tool, btn});
        return btn;
    };

    addTool("➜", "Arrow (A)", Tool::Arrow);
    addTool("◯", "Circle (C)", Tool::Circle);
    addTool("▭", "Rectangle (R)", Tool::Rectangle);
    addTool("✎", "Freehand pen (P)", Tool::Pen);
    addTool("T", "Text (T)", Tool::Text);
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
    custom->setToolTip("Custom colour…");
    custom->setCursor(Qt::PointingHandCursor);
    custom->setFocusPolicy(Qt::NoFocus);
    connect(custom, &QPushButton::clicked, this, [this]() {
        const QColor c = QColorDialog::getColor(m_color, this, "Pick colour");
        if (c.isValid()) {
            m_color = c;
            for (QAbstractButton *b : m_colorGroup->buttons()) {
                b->setChecked(false);
            }
        }
    });
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
        connect(btn, &QPushButton::clicked, this, slot);
        layout->addWidget(btn);
    };

    addAction("↶", "Undo (Ctrl+Z)", [this]() { undo(); });
    addAction("💾", "Save (Ctrl+S)", [this]() { save(); });
    addAction("⧉", "Copy (Enter)", [this]() { finishCopy(); });
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
    x = qBound(0, x, width() - tb.width());
    y = qBound(0, y, height() - tb.height());

    m_toolbar->move(x, y);
    m_toolbar->show();
    m_toolbar->raise();
}

void CaptureWidget::setTool(Tool tool)
{
    if (m_editingText) {
        commitText();
    }
    m_tool = tool;
    for (const auto &pair : m_toolButtons) {
        pair.second->setChecked(pair.first == tool);
    }
}

void CaptureWidget::undo()
{
    if (m_editingText) {
        commitText();
    }
    if (!m_annotations.isEmpty()) {
        // Keep the counter in step if we removed a numbered marker.
        if (m_annotations.last().tool == Tool::Number && m_nextNumber > 1) {
            m_nextNumber--;
        }
        m_annotations.removeLast();
        update();
    }
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

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------

void CaptureWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        return;
    }
    const QPoint pos = event->pos();

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
            } else {
                m_drag = Drag::Draw;
                m_current = Annotation{};
                m_current.tool = m_tool;
                m_current.color = m_color;
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
        m_dragCur = pos;
        break;
    case Drag::Resize: {
        QRect s = m_selAtPress;
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
        m_selection = QRect(QPoint(l, t), QPoint(r, b)).normalized() & rect();
        break;
    }
    case Drag::MoveSelection: {
        QRect s = m_selAtPress;
        s.translate(pos - m_pressPos);
        // Keep inside the widget.
        if (s.left() < 0) s.moveLeft(0);
        if (s.top() < 0) s.moveTop(0);
        if (s.right() > width()) s.moveRight(width());
        if (s.bottom() > height()) s.moveBottom(height());
        m_selection = s;
        break;
    }
    case Drag::Draw:
        m_current.p2 = pos;
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
        QRect sel = QRect(m_pressPos, pos).normalized() & rect();
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
            m_annotations.append(a);
        } else if (m_current.tool == Tool::Text) {
            // Click starts a text box in edit mode.
            Annotation a = m_current;
            a.p1 = pos;
            a.text.clear();
            m_annotations.append(a);
            m_editingText = true;
            m_editIndex = m_annotations.size() - 1;
        } else {
            m_current.p2 = pos;
            const QRect r = QRect(m_current.p1, m_current.p2).normalized();
            const bool bigEnough = m_current.tool == Tool::Pen
                                       ? m_current.points.size() > 1
                                       : (r.width() > 2 || r.height() > 2);
            if (bigEnough) {
                m_annotations.append(m_current);
            }
        }
        break;
    }
    case Drag::None:
        break;
    }

    update();
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------

void CaptureWidget::keyPressEvent(QKeyEvent *event)
{
    if (m_editingText) {
        switch (event->key()) {
        case Qt::Key_Escape:
            // Cancel this text box.
            if (m_editIndex >= 0 && m_editIndex < m_annotations.size()) {
                m_annotations.removeAt(m_editIndex);
            }
            m_editingText = false;
            m_editIndex = -1;
            update();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            commitText();
            return;
        case Qt::Key_Backspace:
            if (m_editIndex >= 0) {
                m_annotations[m_editIndex].text.chop(1);
                update();
            }
            return;
        default:
            if (!event->text().isEmpty() && event->text()[0].isPrint()) {
                m_annotations[m_editIndex].text += event->text();
                update();
                return;
            }
            return;
        }
    }

    switch (event->key()) {
    case Qt::Key_Escape:  emit captureAborted(); break;
    case Qt::Key_Return:
    case Qt::Key_Enter:   finishCopy(); break;
    case Qt::Key_A: if (m_hasSelection) setTool(Tool::Arrow); break;
    case Qt::Key_C: if (m_hasSelection) setTool(Tool::Circle); break;
    case Qt::Key_R: if (m_hasSelection) setTool(Tool::Rectangle); break;
    case Qt::Key_P: if (m_hasSelection) setTool(Tool::Pen); break;
    case Qt::Key_T: if (m_hasSelection) setTool(Tool::Text); break;
    case Qt::Key_H: if (m_hasSelection) setTool(Tool::Highlight); break;
    case Qt::Key_N: if (m_hasSelection) setTool(Tool::Number); break;
    case Qt::Key_B: if (m_hasSelection) setTool(Tool::Blur); break;
    case Qt::Key_Z: if (event->modifiers() & Qt::ControlModifier) undo(); break;
    case Qt::Key_S: if (event->modifiers() & Qt::ControlModifier) save(); break;
    default: QWidget::keyPressEvent(event);
    }
}

void CaptureWidget::commitText()
{
    if (m_editIndex >= 0 && m_editIndex < m_annotations.size()
        && m_annotations[m_editIndex].text.isEmpty()) {
        m_annotations.removeAt(m_editIndex); // discard empty box
    }
    m_editingText = false;
    m_editIndex = -1;
    update();
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

QColor CaptureWidget::paleColor(const QColor &c)
{
    // Blend halfway to white so a Multiply composition tints rather than covers.
    return QColor((c.red() + 255) / 2, (c.green() + 255) / 2, (c.blue() + 255) / 2);
}

QImage CaptureWidget::pixelated(const QRect &region) const
{
    const QRect r = region.normalized() & m_screenshot.rect();
    if (r.isEmpty()) {
        return QImage();
    }
    const int factor = 12;
    QImage sub = m_screenshot.copy(r);
    QImage small = sub.scaled(qMax(1, r.width() / factor), qMax(1, r.height() / factor),
                              Qt::IgnoreAspectRatio, Qt::FastTransformation);
    return small.scaled(r.size(), Qt::IgnoreAspectRatio, Qt::FastTransformation);
}

void CaptureWidget::drawAnnotation(QPainter &painter, const Annotation &a, bool editing) const
{
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);

    switch (a.tool) {
    case Tool::Arrow: {
        painter.setPen(QPen(a.color, kPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawLine(a.p1, a.p2);
        QLineF line(a.p1, a.p2);
        const double len = 16.0;
        QLineF h1(a.p2, a.p1); h1.setAngle(line.angle() + 150); h1.setLength(len);
        QLineF h2(a.p2, a.p1); h2.setAngle(line.angle() - 150); h2.setLength(len);
        painter.drawLine(QLineF(a.p2, h1.p2()));
        painter.drawLine(QLineF(a.p2, h2.p2()));
        break;
    }
    case Tool::Circle:
        painter.setPen(QPen(a.color, kPenWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRect(a.p1, a.p2).normalized());
        break;
    case Tool::Rectangle:
        painter.setPen(QPen(a.color, kPenWidth));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(QRect(a.p1, a.p2).normalized());
        break;
    case Tool::Pen: {
        painter.setPen(QPen(a.color, kPenWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (a.points.size() > 1) {
            painter.drawPolyline(a.points.constData(), a.points.size());
        }
        break;
    }
    case Tool::Highlight: {
        painter.setCompositionMode(QPainter::CompositionMode_Multiply);
        painter.setPen(Qt::NoPen);
        painter.setBrush(paleColor(a.color));
        painter.drawRect(QRect(a.p1, a.p2).normalized());
        break;
    }
    case Tool::Blur: {
        const QRect r = QRect(a.p1, a.p2).normalized();
        const QImage px = pixelated(r);
        if (!px.isNull()) {
            painter.drawImage((r & m_screenshot.rect()).topLeft(), px);
        }
        break;
    }
    case Tool::Text: {
        QFont font = painter.font();
        font.setPixelSize(22);
        font.setBold(true);
        painter.setFont(font);
        painter.setPen(a.color);
        QFontMetrics fm(font);
        const int baseline = a.p1.y() + fm.ascent();
        QString shown = a.text;
        if (editing) {
            shown += QChar('|'); // caret
        }
        painter.drawText(a.p1.x(), baseline, shown);
        break;
    }
    case Tool::Number: {
        const int rad = 14;
        painter.setPen(Qt::NoPen);
        painter.setBrush(a.color);
        painter.drawEllipse(a.p1, rad, rad);
        QFont font = painter.font();
        font.setPixelSize(16);
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

void CaptureWidget::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.drawImage(0, 0, m_screenshot);
    painter.fillRect(rect(), QColor(0, 0, 0, 130)); // dim

    QRect sel = m_selection;
    if (m_drag == Drag::NewSelection) {
        sel = QRect(m_pressPos, m_dragCur).normalized();
    }

    if (!sel.isNull() && (m_hasSelection || m_drag == Drag::NewSelection)) {
        painter.drawImage(sel, m_screenshot, sel); // brighten selection

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

        painter.setPen(Qt::white);
        painter.drawText(sel.bottomRight() + QPoint(-95, -8),
                         QString("%1 x %2").arg(sel.width()).arg(sel.height()));
    }

    for (int i = 0; i < m_annotations.size(); ++i) {
        drawAnnotation(painter, m_annotations[i], m_editingText && i == m_editIndex);
    }
    if (m_drag == Drag::Draw) {
        drawAnnotation(painter, m_current);
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
    QImage result = m_screenshot.copy();
    {
        QPainter painter(&result);
        for (const Annotation &a : m_annotations) {
            drawAnnotation(painter, a);
        }
    }
    return m_hasSelection ? result.copy(m_selection) : result;
}

void CaptureWidget::finishCopy()
{
    if (m_editingText) {
        commitText();
    }
    if (!m_hasSelection) {
        return;
    }
    emit captureCompleted(renderResult());
}

void CaptureWidget::save()
{
    if (m_editingText) {
        commitText();
    }
    if (!m_hasSelection) {
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

    if (renderResult().save(path)) {
        qInfo() << "Saved screenshot to" << path;
    } else {
        qWarning() << "Failed to save screenshot to" << path;
    }
    emit captureAborted();
}
