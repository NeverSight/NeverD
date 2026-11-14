//===- AgentUiPlugin.cpp - Floating Agent conversation --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The workbench's Agent entry: an "Ask Agent" button at the workbench's
/// corner, as Office places its Copilot button, and a conversation window
/// that a person moves by its title and resizes by its edges, as VS Code
/// moves its quick input. Both remember where they were left.
///
//===----------------------------------------------------------------------===//

#include "ChatTranscript.h"
#include "GuiPluginUi.h"
#include "client/AgentProcess.h"
#include "client/AgentProtocol.h"
#include "platform/AppPaths.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QShortcut>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QTranslator>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <array>
#include <functional>
#include <optional>
#include <utility>

namespace neverd::gui::pluginui {
namespace {
constexpr QSize ChatSize{440, 520};
constexpr QSize ChatMinimumSize{340, 300};
// The band inside the conversation's outline where a drag resizes it.
constexpr int ResizeBand = 6;
// A button let go this close to the workbench's side docks there.
constexpr int DockReach = 24;
constexpr int TeachingWidth = 320;
constexpr int SuggestionCount = 3;
constexpr int SpinnerStepMs = 80;
constexpr int SpinnerStepDegrees = 30;
// The corners of cards, as the workbench rounds its floating cards; tool
// buttons take the workbench's tool button corner.
constexpr int CardRadius = 6;
constexpr int ToolRadius = 3;
constexpr auto LauncherVisibleKey =
    "guiPlugins/neverd.agent-ui/launcherVisible";
constexpr auto LauncherHintKey = "guiPlugins/neverd.agent-ui/launcherHintShown";
constexpr auto LauncherOffsetKey = "guiPlugins/neverd.agent-ui/launcherOffset";
constexpr auto LauncherDockedKey = "guiPlugins/neverd.agent-ui/launcherDocked";
constexpr auto ChatOffsetKey = "guiPlugins/neverd.agent-ui/chatOffset";
constexpr auto ChatSizeKey = "guiPlugins/neverd.agent-ui/chatSize";
constexpr auto TeachingKey = "guiPlugins/neverd.agent-ui/teachingShown";
constexpr auto OpenShortcut = "Ctrl+Alt+I";

// The workbench's color roles: the host's theme when it offers one, else the
// application palette's nearest roles, which the workbench also sets.
class Colors {
public:
  explicit Colors(const AppearanceContext *appearance = nullptr)
      : Appearance(appearance) {}
  QColor operator()(const char *role) const {
    if (Appearance)
      if (const auto color = Appearance->themeColor(QLatin1String(role));
          color.isValid())
        return color;
    const auto palette = QGuiApplication::palette();
    const QLatin1StringView name(role);
    const auto is = [&name](const char *other) {
      return name == QLatin1StringView(other);
    };
    if (is("Menu"))
      return palette.color(QPalette::Window);
    if (is("Button"))
      return palette.color(QPalette::Button);
    if (is("ButtonHover") || is("ToolBarHover"))
      return palette.color(QPalette::Midlight);
    if (is("ButtonText"))
      return palette.color(QPalette::ButtonText);
    if (is("SecondaryText"))
      return palette.color(QPalette::PlaceholderText);
    if (is("ButtonPrimary") || is("ButtonPrimaryBorder") || is("Accent") ||
        is("Focus"))
      return palette.color(QPalette::Accent);
    if (is("ButtonPrimaryHover"))
      return palette.color(QPalette::Accent).lighter(115);
    if (is("ButtonPrimaryText"))
      return palette.color(QPalette::HighlightedText);
    if (is("Border") || is("FloatingWindowBorder"))
      return palette.color(QPalette::Mid);
    if (is("ToolBarHoverBorder"))
      return Qt::transparent;
    return palette.color(QPalette::Text);
  }

private:
  const AppearanceContext *Appearance;
};

// Standard controls keep the workbench's own style. This sheet gives only the
// card, the messages and the quiet labels their roles.
QString chatStyle(const Colors &color) {
  const auto hex = [&](const char *role) {
    return color(role).name(QColor::HexArgb);
  };
  return QStringLiteral(R"css(
QFrame#NeverDAgentChat, QFrame#NeverDAgentTeaching {
  background: %1; border: 1px solid %2; border-radius: %3px;
}
QLabel#NeverDAgentContext, QLabel#NeverDAgentStatus, QLabel#NeverDAgentSpeaker,
QLabel#NeverDAgentExecutionLatest, QLabel#NeverDAgentTeachingText {
  color: %4;
}
QToolButton#NeverDAgentExecutionToggle {
  color: %4; background: transparent; border: 1px solid transparent;
  border-radius: 3px; padding: 1px 4px;
}
QToolButton#NeverDAgentExecutionToggle:hover { background: %7; }
QToolButton#NeverDAgentExecutionToggle:focus { border-color: %8; }
QScrollArea#NeverDAgentTranscript, QWidget#NeverDAgentMessages {
  background: transparent; border: none;
}
QFrame#NeverDAgentUserMessage {
  background: %5; border: %6px solid %9; border-radius: %3px;
}
QFrame#NeverDAgentReply { background: transparent; border: none; }
QTextBrowser#NeverDAgentMessageText {
  background: transparent; border: none; padding: 0;
}
QTreeWidget#NeverDAgentExecutionRows { border: none; }
QLabel#NeverDAgentWelcomeText { color: %4; }
)css")
      .arg(hex("Menu"), hex("FloatingWindowBorder"),
           QString::number(CardRadius), hex("SecondaryText"), hex("Button"),
           QString::number(BubbleBorderWidth), hex("ToolBarHover"),
           hex("Focus"), hex("Border"));
}

/// The Agent's mark in a 16-unit box: a speech bubble with a spark.
void paintAgentMark(QPainter &painter, const QRectF &box, const QColor &color) {
  painter.save();
  painter.translate(box.topLeft());
  painter.scale(box.width() / 16.0, box.height() / 16.0);
  painter.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  QPainterPath bubble;
  bubble.moveTo(4, 4);
  bubble.lineTo(8.5, 4);
  bubble.quadTo(11.5, 4, 11.5, 7);
  bubble.lineTo(11.5, 9.5);
  bubble.quadTo(11.5, 12, 9, 12);
  bubble.lineTo(6.5, 12);
  bubble.lineTo(3.8, 14.5);
  bubble.lineTo(4.1, 12);
  bubble.quadTo(1.5, 12, 1.5, 9.5);
  bubble.lineTo(1.5, 6.5);
  bubble.quadTo(1.5, 4, 4, 4);
  bubble.closeSubpath();
  painter.drawPath(bubble);
  QPainterPath spark;
  spark.moveTo(13.2, 0.3);
  spark.quadTo(13.6, 2.7, 16, 3.1);
  spark.quadTo(13.6, 3.5, 13.2, 5.9);
  spark.quadTo(12.8, 3.5, 10.4, 3.1);
  spark.quadTo(12.8, 2.7, 13.2, 0.3);
  painter.setPen(Qt::NoPen);
  painter.setBrush(color);
  painter.drawPath(spark);
  for (const qreal x : {4.3, 6.5, 8.7})
    painter.drawEllipse(QPointF(x, 8), 0.95, 0.95);
  painter.restore();
}

/// A progress arc in place of the mark while the Agent works.
void paintSpinner(QPainter &painter, const QRectF &box, const QColor &color,
                  int angle) {
  painter.save();
  painter.setPen(
      QPen(color, std::max(1.5, box.width() / 9), Qt::SolidLine, Qt::RoundCap));
  painter.setBrush(Qt::NoBrush);
  const auto inset = box.width() / 8;
  painter.drawArc(box.adjusted(inset, inset, -inset, -inset), -angle * 16,
                  270 * 16);
  painter.restore();
}

/// The Agent entry at the workbench's corner: an accent "Ask Agent" button,
/// or only its mark while docked at the side.
class Launcher final : public QAbstractButton {
  Q_DECLARE_TR_FUNCTIONS(neverd::gui::pluginui::Launcher)

public:
  Launcher(const AppearanceContext *appearance, QWidget *parent)
      : QAbstractButton(parent), Color(appearance) {
    setObjectName("NeverDAgentLauncher");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_Hover);
    Spin.setInterval(SpinnerStepMs);
    connect(&Spin, &QTimer::timeout, this, [this] {
      Angle = (Angle + SpinnerStepDegrees) % 360;
      update();
    });
    retranslate();
  }

  bool docked() const { return Docked; }
  void setDocked(bool docked) {
    if (std::exchange(Docked, docked) != docked)
      updateGeometry();
    update();
  }
  void setBusy(bool busy) {
    if (std::exchange(Busy, busy) == busy)
      return;
    Busy ? Spin.start() : Spin.stop();
    retranslate();
  }
  void setUnread(bool unread) {
    if (std::exchange(Unread, unread) != unread)
      update();
  }
  void setSubject(const QString &subject) {
    Subject = subject;
    retranslate();
  }

  QSize sizeHint() const override {
    const int height = buttonHeight();
    if (Docked)
      return {height, height};
    QFont bold = font();
    bold.setWeight(QFont::DemiBold);
    const int text = QFontMetrics(bold).horizontalAdvance(label());
    return {height / 2 + MarkSize + MarkGap + text + height / 2, height};
  }
  QSize minimumSizeHint() const override { return sizeHint(); }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF outline = QRectF(rect()).adjusted(1.5, 1.5, -1.5, -1.5);
    const qreal radius = outline.height() / 2;
    QColor face = Color(underMouse() || isDown() ? "ButtonPrimaryHover"
                                                 : "ButtonPrimary");
    if (isDown())
      face = face.darker(112);
    const QColor ink = Color("ButtonPrimaryText");
    if (hasFocus() && FocusByKeyboard) {
      painter.setPen(QPen(Color("Focus"), 1.5));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(QRectF(rect()).adjusted(0.75, 0.75, -0.75, -0.75),
                              radius + 0.75, radius + 0.75);
    }
    painter.setPen(QPen(Color("ButtonPrimaryBorder"), 1));
    painter.setBrush(face);
    painter.drawRoundedRect(outline.adjusted(1, 1, -1, -1), radius - 1,
                            radius - 1);
    const bool mirrored = layoutDirection() == Qt::RightToLeft;
    const qreal markLeft =
        Docked     ? outline.center().x() - MarkSize / 2.0
        : mirrored ? outline.right() - outline.height() / 2 - MarkSize + 2
                   : outline.left() + outline.height() / 2 - 2;
    const QRectF mark(markLeft, outline.center().y() - MarkSize / 2.0, MarkSize,
                      MarkSize);
    if (Busy)
      paintSpinner(painter, mark, ink, Angle);
    else
      paintAgentMark(painter, mark, ink);
    if (!Docked) {
      QFont bold = font();
      bold.setWeight(QFont::DemiBold);
      painter.setFont(bold);
      painter.setPen(ink);
      const QRectF text =
          mirrored
              ? QRectF(outline.left(), outline.top(),
                       mark.left() - MarkGap - outline.left(), outline.height())
              : QRectF(mark.right() + MarkGap, outline.top(),
                       outline.right() - mark.right() - MarkGap,
                       outline.height());
      painter.drawText(
          text, Qt::AlignVCenter | (mirrored ? Qt::AlignRight : Qt::AlignLeft),
          label());
    }
    if (Unread) {
      const QPointF center(mirrored ? outline.left() + radius * 0.55
                                    : outline.right() - radius * 0.55,
                           outline.top() + radius * 0.55);
      painter.setPen(QPen(face, 1.5));
      painter.setBrush(ink);
      painter.drawEllipse(center, 3.5, 3.5);
    }
  }
  void focusInEvent(QFocusEvent *event) override {
    FocusByKeyboard = event->reason() != Qt::MouseFocusReason;
    QAbstractButton::focusInEvent(event);
  }
  void changeEvent(QEvent *event) override {
    QAbstractButton::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
      retranslate();
    else if (event->type() == QEvent::FontChange ||
             event->type() == QEvent::LayoutDirectionChange)
      updateGeometry();
  }

private:
  static constexpr int MarkSize = 16;
  static constexpr int MarkGap = 8;

  int buttonHeight() const { return std::max(32, fontMetrics().height() + 14); }
  QString label() const { return Busy ? tr("Working…") : tr("Ask Agent"); }
  void retranslate() {
    const auto shortcut = QKeySequence(QString::fromLatin1(OpenShortcut))
                              .toString(QKeySequence::NativeText);
    setAccessibleName(tr("Ask NeverD Agent"));
    setToolTip(
        Subject.isEmpty()
            ? tr("Ask NeverD Agent (%1)").arg(shortcut)
            : tr("Ask NeverD Agent about %1 (%2)").arg(Subject, shortcut));
    updateGeometry();
    update();
  }

  Colors Color;
  QTimer Spin;
  QString Subject;
  int Angle = 0;
  bool Docked = false;
  bool Busy = false;
  bool Unread = false;
  bool FocusByKeyboard = false;
};

/// A borderless tool button that draws one of the conversation's glyphs in
/// the text color, with the workbench's tool hover behind it.
class GlyphButton final : public QToolButton {
public:
  enum class Glyph { Close, Settings };
  GlyphButton(Glyph glyph, const AppearanceContext *appearance, QWidget *parent)
      : QToolButton(parent), Shape(glyph), Color(appearance) {
    setAutoRaise(true);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
  }
  QSize sizeHint() const override {
    const int side = std::max(24, fontMetrics().height() + 8);
    return {side, side};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    if (underMouse() || isDown()) {
      painter.setPen(QPen(Color("ToolBarHoverBorder"), 1));
      painter.setBrush(Color("ToolBarHover"));
      painter.drawRoundedRect(face, ToolRadius, ToolRadius);
    }
    if (hasFocus()) {
      painter.setPen(QPen(Color("Focus"), 1));
      painter.setBrush(Qt::NoBrush);
      painter.drawRoundedRect(face, ToolRadius, ToolRadius);
    }
    const QColor ink = Color(isEnabled() ? "Text" : "DisabledText");
    painter.translate(face.center());
    painter.setPen(QPen(ink, 1.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    if (Shape == Glyph::Close) {
      painter.drawLine(QPointF(-4, -4), QPointF(4, 4));
      painter.drawLine(QPointF(4, -4), QPointF(-4, 4));
      return;
    }
    // Settings: three sliders, as the workbench's options glyph shows them.
    for (const auto &[y, knob] :
         {std::pair{-4.5, 2.5}, std::pair{0.0, -2.5}, std::pair{4.5, 1.0}}) {
      painter.setPen(QPen(ink, 1.2, Qt::SolidLine, Qt::RoundCap));
      painter.drawLine(QPointF(-6, y), QPointF(6, y));
      painter.setPen(QPen(ink, 1.2));
      painter.setBrush(Color("Menu"));
      painter.drawEllipse(QPointF(knob, y), 1.8, 1.8);
    }
  }

private:
  Glyph Shape;
  Colors Color;
};

/// A first question: an outlined row that fills on hover, as VS Code lists
/// a new chat's suggestions.
class SuggestionButton final : public QAbstractButton {
public:
  SuggestionButton(const AppearanceContext *appearance, QWidget *parent)
      : QAbstractButton(parent), Color(appearance) {
    setObjectName("NeverDAgentSuggestion");
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
  }
  QSize sizeHint() const override {
    return {fontMetrics().horizontalAdvance(text()) + 2 * Padding,
            fontMetrics().height() + 2 * Padding - 2};
  }
  QSize minimumSizeHint() const override {
    return {4 * Padding, sizeHint().height()};
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF face = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(Color(hasFocus() ? "Focus" : "Border"), 1));
    painter.setBrush(underMouse() && isEnabled() ? Color("ToolBarHover")
                                                 : QColor(Qt::transparent));
    painter.drawRoundedRect(face, CardRadius, CardRadius);
    painter.setPen(Color(isEnabled() ? "Text" : "DisabledText"));
    const QRect text = rect().adjusted(Padding, 0, -Padding, 0);
    painter.drawText(
        text, Qt::AlignVCenter | Qt::AlignLeading,
        fontMetrics().elidedText(this->text(), Qt::ElideRight, text.width()));
  }

private:
  static constexpr int Padding = 10;
  Colors Color;
};

/// The Agent's mark at a size, in the accent color.
class Mark final : public QWidget {
public:
  Mark(int size, const AppearanceContext *appearance, QWidget *parent)
      : QWidget(parent), Size(size), Color(appearance) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setFixedSize(size, size);
  }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    paintAgentMark(painter, QRectF(0, 0, Size, Size), Color("Accent"));
  }

private:
  int Size;
  Colors Color;
};

/// Six dots that say the title moves its window.
class Grip final : public QWidget {
public:
  Grip(const AppearanceContext *appearance, QWidget *parent)
      : QWidget(parent), Color(appearance) {
    setAttribute(Qt::WA_TransparentForMouseEvents);
  }
  QSize sizeHint() const override { return {8, 14}; }

protected:
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Color("SecondaryText"));
    const QPointF center = QRectF(rect()).center();
    for (const qreal x : {-2.0, 2.0})
      for (const qreal y : {-4.5, 0.0, 4.5})
        painter.drawEllipse(center + QPointF(x, y), 1.0, 1.0);
  }

private:
  Colors Color;
};

/// A card the person moves by its title and, when it is resizable, sizes by
/// its edges. It asks the host for the place through ViewPositionProperty.
class FloatingCard : public QFrame {
public:
  using Changed = std::function<void()>;

  FloatingCard(const char *name, const AppearanceContext *appearance,
               QWidget *parent)
      : QFrame(parent), Color(appearance) {
    setObjectName(QLatin1String(name));
    setAttribute(Qt::WA_StyledBackground);
    setMouseTracking(true);
    Sheet = chatStyle(Color);
    setStyleSheet(Sheet);
  }

  /// Let \p handle move the card; a double click there calls \p reset.
  void setHandle(QWidget *handle, Changed reset) {
    Handle = handle;
    Reset = std::move(reset);
    handle->installEventFilter(this);
    handle->setCursor(Qt::OpenHandCursor);
  }
  void setResizable(bool resizable) { Resizable = resizable; }
  /// Called after the person moved or resized the card.
  void onPlaced(Changed placed) { Placed = std::move(placed); }

  QSize sizeHint() const override {
    return Chosen.isValid() ? Chosen.expandedTo(minimumSizeHint())
                            : QFrame::sizeHint();
  }
  void setChosenSize(QSize size) {
    Chosen = size;
    updateGeometry();
  }
  QSize chosenSize() const { return Chosen; }

protected:
  // The workbench sets a new application palette when its theme changes.
  void changeEvent(QEvent *event) override {
    QFrame::changeEvent(event);
    if (event->type() != QEvent::ApplicationPaletteChange)
      return;
    if (auto sheet = chatStyle(Color); sheet != Sheet) {
      Sheet = std::move(sheet);
      setStyleSheet(Sheet);
    }
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (object != Handle)
      return QFrame::eventFilter(object, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() != Qt::LeftButton)
        break;
      begin(mouse->globalPosition(), {});
      return true;
    }
    case QEvent::MouseMove:
      if (Press && !Edges) {
        drag(static_cast<QMouseEvent *>(event)->globalPosition());
        return true;
      }
      break;
    case QEvent::MouseButtonRelease:
      if (Press) {
        finish();
        return true;
      }
      break;
    case QEvent::MouseButtonDblClick:
      if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton &&
          Reset) {
        Reset();
        return true;
      }
      break;
    default:
      break;
    }
    return QFrame::eventFilter(object, event);
  }

  void mousePressEvent(QMouseEvent *event) override {
    const auto edges = edgesAt(event->position().toPoint());
    if (event->button() == Qt::LeftButton && edges) {
      begin(event->globalPosition(), edges);
      event->accept();
      return;
    }
    QFrame::mousePressEvent(event);
  }
  void mouseMoveEvent(QMouseEvent *event) override {
    if (Press) {
      drag(event->globalPosition());
      return;
    }
    showEdgeCursor(edgesAt(event->position().toPoint()));
    QFrame::mouseMoveEvent(event);
  }
  void mouseReleaseEvent(QMouseEvent *event) override {
    if (Press) {
      finish();
      return;
    }
    QFrame::mouseReleaseEvent(event);
  }
  void leaveEvent(QEvent *event) override {
    if (!Press)
      unsetCursor();
    QFrame::leaveEvent(event);
  }

private:
  Qt::Edges edgesAt(QPoint point) const {
    if (!Resizable)
      return {};
    Qt::Edges edges;
    if (point.x() < ResizeBand)
      edges |= Qt::LeftEdge;
    else if (point.x() >= width() - ResizeBand)
      edges |= Qt::RightEdge;
    if (point.y() < ResizeBand)
      edges |= Qt::TopEdge;
    else if (point.y() >= height() - ResizeBand)
      edges |= Qt::BottomEdge;
    return edges;
  }
  void showEdgeCursor(Qt::Edges edges) {
    const bool horizontal = edges & (Qt::LeftEdge | Qt::RightEdge);
    const bool vertical = edges & (Qt::TopEdge | Qt::BottomEdge);
    if (horizontal && vertical)
      setCursor((edges == (Qt::LeftEdge | Qt::TopEdge) ||
                 edges == (Qt::RightEdge | Qt::BottomEdge))
                    ? Qt::SizeFDiagCursor
                    : Qt::SizeBDiagCursor);
    else if (horizontal)
      setCursor(Qt::SizeHorCursor);
    else if (vertical)
      setCursor(Qt::SizeVerCursor);
    else
      unsetCursor();
  }
  void begin(QPointF global, Qt::Edges edges) {
    Press = global;
    Origin = geometry();
    Edges = edges;
    Moving = false;
  }
  void drag(QPointF global) {
    const QPoint delta = (global - *Press).toPoint();
    if (!Moving && delta.manhattanLength() <
                       QGuiApplication::styleHints()->startDragDistance())
      return;
    if (!Moving && Handle && !Edges)
      Handle->setCursor(Qt::ClosedHandCursor);
    Moving = true;
    QRect target = Origin;
    if (!Edges) {
      target.moveTopLeft(Origin.topLeft() + delta);
    } else {
      const QSize least = minimumSize().expandedTo(minimumSizeHint());
      if (Edges & Qt::LeftEdge)
        target.setLeft(std::min(Origin.left() + delta.x(),
                                Origin.right() - least.width()));
      if (Edges & Qt::RightEdge)
        target.setRight(std::max(Origin.right() + delta.x(),
                                 Origin.left() + least.width()));
      if (Edges & Qt::TopEdge)
        target.setTop(std::min(Origin.top() + delta.y(),
                               Origin.bottom() - least.height()));
      if (Edges & Qt::BottomEdge)
        target.setBottom(std::max(Origin.bottom() + delta.y(),
                                  Origin.top() + least.height()));
      if (parentWidget())
        target = target.intersected(parentWidget()->rect());
      setChosenSize(target.size());
    }
    setProperty(ViewPositionProperty, target.topLeft());
  }
  void finish() {
    Press.reset();
    const bool placed = std::exchange(Moving, false);
    Edges = {};
    if (Handle)
      Handle->setCursor(Qt::OpenHandCursor);
    if (placed && Placed)
      Placed();
  }

  Colors Color;
  QString Sheet;
  QPointer<QWidget> Handle;
  Changed Reset, Placed;
  std::optional<QPointF> Press;
  QRect Origin;
  QSize Chosen;
  Qt::Edges Edges;
  bool Moving = false;
  bool Resizable = false;
};

/// The distance of \p widget's bottom-right corner from its parent's, which
/// keeps a corner element in its corner while the workbench resizes.
QPoint cornerOffset(const QWidget &widget) {
  const auto *parent = widget.parentWidget();
  if (!parent)
    return {};
  const QRect area = parent->rect();
  return {area.right() - widget.geometry().right(),
          area.bottom() - widget.geometry().bottom()};
}

QPoint topLeftFor(const QWidget &widget, QPoint offset, QSize size) {
  const auto *parent = widget.parentWidget();
  if (!parent)
    return {};
  const QRect area = parent->rect();
  return {area.right() - offset.x() - size.width() + 1,
          area.bottom() - offset.y() - size.height() + 1};
}

class FloatingChat final : public QObject {
  Q_DECLARE_TR_FUNCTIONS(neverd::gui::pluginui::FloatingChat)

public:
  FloatingChat(Context &context, QObject &lifetime)
      : QObject(&lifetime), Host(context),
        Appearance(dynamic_cast<AppearanceContext *>(&context)) {
    loadTranslations();
  }

  bool mount(QString &error) {
    const bool mirrored = QGuiApplication::layoutDirection() == Qt::RightToLeft;
    const Placement corner{
        mirrored ? Qt::BottomLeftCorner : Qt::BottomRightCorner, {}};
    auto *ball = Host.mountView(
        "launcher", corner,
        [this](QWidget *parent) { return new Launcher(Appearance, parent); },
        error);
    if (!ball)
      return false;
    Ball = static_cast<Launcher *>(ball);
    auto *panel = Host.mountView(
        "chat", {corner.Corner, "launcher"},
        [this](QWidget *parent) { return makePanel(parent); }, error);
    if (!panel)
      return false;
    Panel = static_cast<FloatingCard *>(panel);
    Panel->hide();
    auto *teaching = Host.mountView(
        "teaching", {corner.Corner, "launcher"},
        [this](QWidget *parent) { return makeTeaching(parent); }, error);
    if (!teaching)
      return false;
    Teaching = teaching;
    Teaching->hide();
    if (auto *menu = dynamic_cast<MenuContext *>(&Host)) {
      if (!menu->mountMenuAction(
              "open-chat",
              [this](QObject *parent) {
                auto *action = new QAction(parent);
                action->setObjectName("NeverDAgentOpenChat");
                action->setShortcut(
                    QKeySequence(QString::fromLatin1(OpenShortcut)));
                connect(action, &QAction::triggered, this,
                        [this] { toggleFromKeyboard(); });
                OpenAction = action;
                return action;
              },
              error) ||
          !menu->mountMenuAction(
              "show-launcher",
              [this](QObject *parent) {
                auto *action = new QAction(parent);
                action->setObjectName("NeverDAgentLauncherVisibility");
                action->setCheckable(true);
                action->setChecked(
                    QSettings().value(LauncherVisibleKey, true).toBool());
                connect(action, &QAction::triggered, this,
                        &FloatingChat::setLauncherVisible);
                LauncherAction = action;
                return action;
              },
              error))
        return false;
    }
    retranslate();
    Ball->installEventFilter(this);
    Panel->installEventFilter(this);
    Prompt->installEventFilter(this);
    if (auto *workbench = Ball->parentWidget())
      workbench->installEventFilter(this);
    connect(Ball, &QAbstractButton::clicked, this, [this] {
      if (Panel->isVisible())
        hidePanel();
      else
        showPanel(true);
    });
    Host.watchContext(this,
                      [this](const auto &context) { updateContext(context); });
    connect(&Process, &agent::AgentProcess::received, this,
            &FloatingChat::receive);
    connect(&Process, &agent::AgentProcess::failed, this,
            [this](const auto &error) { disconnected(error); });
    restorePlaces();
    updateContext(Host.captureContext());
    Ball->setVisible(!LauncherAction || LauncherAction->isChecked());
    return true;
  }

private:
  QWidget *makePanel(QWidget *parent) {
    auto *panel = new FloatingCard("NeverDAgentChat", Appearance, parent);
    panel->setResizable(true);
    panel->setMinimumSize(ChatMinimumSize);
    panel->setChosenSize(ChatSize);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 8, 10, 14);
    layout->setSpacing(9);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    // The title row moves the window, as a window's title bar does.
    auto *title = new QWidget(panel);
    title->setObjectName("NeverDAgentTitleBar");
    auto *heading = new QHBoxLayout(title);
    heading->setContentsMargins(0, 0, 0, 0);
    heading->setSpacing(6);
    heading->addWidget(new Grip(Appearance, title));
    heading->addWidget(new Mark(16, Appearance, title));
    Title = new QLabel(title);
    Title->setObjectName("NeverDAgentTitle");
    Title->setAttribute(Qt::WA_TransparentForMouseEvents);
    auto font = Title->font();
    font.setBold(true);
    Title->setFont(font);
    heading->addWidget(Title, 1);
    SettingsButton =
        new GlyphButton(GlyphButton::Glyph::Settings, Appearance, title);
    SettingsButton->setObjectName("NeverDAgentSettings");
    connect(SettingsButton, &QToolButton::clicked, this, [this] {
      const auto program = agent::findAgentExecutable();
      if (program.isEmpty() ||
          !QProcess::startDetached(program, {"--show-settings"}))
        setStatus(tr("The standalone NeverD Agent is not available."));
    });
    heading->addWidget(SettingsButton);
    CloseButton = new GlyphButton(GlyphButton::Glyph::Close, Appearance, title);
    CloseButton->setObjectName("NeverDAgentClose");
    connect(CloseButton, &QToolButton::clicked, this, [this] { hidePanel(); });
    heading->addWidget(CloseButton);
    layout->addWidget(title);
    panel->setHandle(title, [this] { resetChatPlace(); });
    panel->onPlaced([this] { saveChatPlace(); });

    auto *body = new QVBoxLayout;
    body->setContentsMargins(0, 0, 4, 0);
    body->setSpacing(9);
    ContextLabel = new QLabel(panel);
    ContextLabel->setObjectName("NeverDAgentContext");
    ContextLabel->setWordWrap(true);
    ContextLabel->setTextFormat(Qt::PlainText);
    body->addWidget(ContextLabel);
    // An empty conversation says what the Agent does and offers first
    // questions, as VS Code's chat welcomes a new conversation.
    Welcome = new QWidget(panel);
    Welcome->setObjectName("NeverDAgentWelcome");
    auto *welcome = new QVBoxLayout(Welcome);
    welcome->setContentsMargins(0, 12, 0, 0);
    welcome->setSpacing(8);
    welcome->addWidget(new Mark(28, Appearance, Welcome));
    WelcomeTitle = new QLabel(Welcome);
    auto titleFont = WelcomeTitle->font();
    titleFont.setBold(true);
    WelcomeTitle->setFont(titleFont);
    WelcomeTitle->setWordWrap(true);
    welcome->addWidget(WelcomeTitle);
    WelcomeText = new QLabel(Welcome);
    WelcomeText->setObjectName("NeverDAgentWelcomeText");
    WelcomeText->setWordWrap(true);
    WelcomeText->setTextFormat(Qt::PlainText);
    welcome->addWidget(WelcomeText);
    welcome->addSpacing(4);
    for (int index = 0; index < SuggestionCount; ++index) {
      auto *suggestion = new SuggestionButton(Appearance, Welcome);
      connect(suggestion, &QAbstractButton::clicked, this, [this, suggestion] {
        Prompt->setPlainText(suggestion->text());
        sendOrStop();
      });
      welcome->addWidget(suggestion);
      Suggestions[index] = suggestion;
    }
    welcome->addStretch(1);
    body->addWidget(Welcome, 1);
    History = new ChatTranscript(
        [this](const QUrl &url) {
          if (url.scheme() == "nd-evidence" && Ready)
            invoke("evidence", {url.host()});
        },
        [this](const QString &runId) {
          if (Ready)
            invoke("execution", {runId});
        },
        panel);
    body->addWidget(History, 1);
    History->hide();
    Status = new QLabel(panel);
    Status->setObjectName("NeverDAgentStatus");
    Status->setTextFormat(Qt::PlainText);
    Status->setWordWrap(true);
    Status->hide();
    body->addWidget(Status);

    auto *composer = new QWidget(panel);
    composer->setObjectName("NeverDAgentComposer");
    auto *compose = new QVBoxLayout(composer);
    compose->setContentsMargins(0, 0, 0, 0);
    compose->setSpacing(6);
    Prompt = new QPlainTextEdit(composer);
    Prompt->setObjectName("NeverDAgentPrompt");
    Prompt->setTabChangesFocus(true);
    Prompt->ensurePolished();
    Prompt->setFixedHeight(Prompt->fontMetrics().lineSpacing() * 3 +
                           2 * Prompt->frameWidth() +
                           qRound(2 * Prompt->document()->documentMargin()));
    connect(Prompt, &QPlainTextEdit::textChanged, this,
            [this] { updateControls(); });
    compose->addWidget(Prompt);
    auto *commands = new QHBoxLayout;
    IncludeContext = new QCheckBox(composer);
    IncludeContext->setObjectName("NeverDAgentIncludeContext");
    IncludeContext->setChecked(true);
    commands->addWidget(IncludeContext, 1);
    Send = new QPushButton(composer);
    Send->setObjectName("NeverDAgentSend");
    // The panel's one accent, without taking Return from the prompt.
    Send->setProperty("mainAction", true);
    Send->setAutoDefault(false);
    connect(Send, &QPushButton::clicked, this, [this] { sendOrStop(); });
    commands->addWidget(Send);
    compose->addLayout(commands);
    body->addWidget(composer);
    layout->addLayout(body, 1);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { hidePanel(); });
    return panel;
  }

  // A teaching card, shown once beside the button, as Fluent teaches a new
  // entry point: what it does and that it moves.
  QWidget *makeTeaching(QWidget *parent) {
    auto *card = new FloatingCard("NeverDAgentTeaching", Appearance, parent);
    auto *layout = new QVBoxLayout(card);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(6);
    TeachingTitle = new QLabel(card);
    auto font = TeachingTitle->font();
    font.setBold(true);
    TeachingTitle->setFont(font);
    layout->addWidget(TeachingTitle);
    TeachingText = new QLabel(card);
    TeachingText->setObjectName("NeverDAgentTeachingText");
    TeachingText->setWordWrap(true);
    TeachingText->setTextFormat(Qt::PlainText);
    layout->addWidget(TeachingText);
    TeachingDone = new QPushButton(card);
    TeachingDone->setObjectName("NeverDAgentTeachingDone");
    TeachingDone->setAutoDefault(false);
    connect(TeachingDone, &QPushButton::clicked, this,
            [this] { dismissTeaching(); });
    layout->addWidget(TeachingDone, 0, Qt::AlignRight);
    return card;
  }

  void retranslate() {
    const auto shortcut = QKeySequence(QString::fromLatin1(OpenShortcut))
                              .toString(QKeySequence::NativeText);
    if (OpenAction)
      OpenAction->setText(tr("Open NeverD Agent"));
    if (LauncherAction)
      LauncherAction->setText(tr("Show Ask Agent button"));
    Title->setText(tr("NeverD Agent"));
    SettingsButton->setToolTip(tr("Model settings…"));
    SettingsButton->setAccessibleName(SettingsButton->toolTip());
    CloseButton->setToolTip(tr("Close (Esc)"));
    CloseButton->setAccessibleName(tr("Close the conversation"));
    Panel->findChild<QWidget *>("NeverDAgentTitleBar")
        ->setToolTip(tr("Drag to move · Double-click to put it back"));
    Prompt->setPlaceholderText(tr("Ask about this function…"));
    Prompt->setToolTip(tr("Enter to send · Shift+Enter for a new line"));
    IncludeContext->setText(tr("Attach current selection"));
    WelcomeTitle->setText(tr("Ask about the code you are reading"));
    WelcomeText->setText(
        tr("NeverD Agent reads this analysis through the workbench and answers "
           "with evidence you can open. It proposes changes; it never makes "
           "them on its own."));
    const QString suggestions[SuggestionCount] = {
        tr("Explain what this function does"),
        tr("Who calls this function, and with what arguments?"),
        tr("Suggest names and types for its variables")};
    for (int index = 0; index < SuggestionCount; ++index)
      Suggestions[index]->setText(suggestions[index]);
    TeachingTitle->setText(tr("Ask NeverD Agent"));
    TeachingText->setText(
        tr("Ask about the function you are reading. Drag the button or the "
           "conversation's title to move them; %1 opens the conversation.")
            .arg(shortcut));
    TeachingDone->setText(tr("Got it"));
    if (auto *card = static_cast<FloatingCard *>(Teaching.data()))
      card->setChosenSize(
          {TeachingWidth, card->layout()->totalHeightForWidth(TeachingWidth)});
    updateContext(Host.captureContext());
  }

  void loadTranslations() {
    const QString language =
        Appearance ? Appearance->language() : QStringLiteral("en");
    if (language == LoadedLanguage)
      return;
    LoadedLanguage = language;
    QCoreApplication::removeTranslator(&Translator);
    QString resource = language;
    resource.replace(QLatin1Char('-'), QLatin1Char('_'));
    if (language != QLatin1String("en") &&
        Translator.load(QStringLiteral(":/neverd-agent-ui/i18n/"
                                       "neverd-agent-ui_") +
                        resource + QStringLiteral(".qm")))
      QCoreApplication::installTranslator(&Translator);
    // The CLI writes progress and errors in the same language.
    if (Process.active())
      Process.send({{"command", "language"}, {"language", language}});
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (object == Ball.data() && handleLauncherDrag(event))
      return true;
    if (object == Ball.data() && event->type() == QEvent::ContextMenu) {
      showLauncherMenu(static_cast<QContextMenuEvent *>(event)->globalPos());
      return true;
    }
    if (object == Ball.data() && event->type() == QEvent::LanguageChange) {
      loadTranslations();
      retranslate();
    } else if (Ball && object == Ball->parentWidget() &&
               event->type() == QEvent::Resize) {
      applyPlaces();
    } else if (object == Prompt) {
      switch (event->type()) {
      case QEvent::InputMethod:
        PromptComposing =
            !static_cast<QInputMethodEvent *>(event)->preeditString().isEmpty();
        break;
      case QEvent::KeyPress: {
        const auto *key = static_cast<QKeyEvent *>(event);
        if (PromptComposing ||
            (key->key() != Qt::Key_Return && key->key() != Qt::Key_Enter) ||
            (key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier)
          break;
        event->accept();
        if (!Running && !key->isAutoRepeat())
          sendOrStop();
        return true;
      }
      case QEvent::FocusOut:
        PromptComposing = false;
        break;
      default:
        break;
      }
    }
    return false;
  }

  void showLauncherMenu(QPoint global) {
    auto *menu = new QMenu(Ball);
    connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
    menu->addAction(
        Panel->isVisible() ? tr("Close the conversation")
                           : tr("Open the conversation"),
        this, [this] { Panel->isVisible() ? hidePanel() : showPanel(true); });
    menu->addSeparator();
    menu->addAction(Ball->docked() ? tr("Undock from the side")
                                   : tr("Dock to the side"),
                    this, [this] { setDocked(!Ball->docked()); });
    auto *reset = menu->addAction(tr("Reset position"), this, [this] {
      LauncherOffset.reset();
      QSettings().remove(LauncherOffsetKey);
      setDocked(false);
      applyPlaces();
    });
    reset->setEnabled(LauncherOffset.has_value() || Ball->docked());
    menu->addSeparator();
    menu->addAction(tr("Hide the button"), this,
                    [this] { setLauncherVisible(false); });
    menu->popup(global);
  }

  bool handleLauncherDrag(QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton) {
        PressPosition = mouse->globalPosition();
        LauncherOrigin = Ball->pos();
      }
      break;
    }
    case QEvent::MouseMove: {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (!PressPosition || !mouse->buttons().testFlag(Qt::LeftButton))
        break;
      const auto delta = (mouse->globalPosition() - *PressPosition).toPoint();
      if (!Dragging) {
        if (delta.manhattanLength() <
            QGuiApplication::styleHints()->startDragDistance())
          break;
        Dragging = true;
        Ball->setDown(false);
        Ball->setCursor(Qt::ClosedHandCursor);
        dismissTeaching();
      }
      Ball->setProperty(ViewPositionProperty, LauncherOrigin + delta);
      event->accept();
      return true;
    }
    case QEvent::MouseButtonRelease:
      if (static_cast<QMouseEvent *>(event)->button() == Qt::LeftButton &&
          finishDrag()) {
        event->accept();
        return true;
      }
      break;
    case QEvent::UngrabMouse:
    case QEvent::Hide:
      finishDrag();
      break;
    default:
      break;
    }
    return false;
  }

  bool finishDrag() {
    PressPosition.reset();
    if (!std::exchange(Dragging, false))
      return false;
    Ball->setCursor(Qt::PointingHandCursor);
    // Let go at the side, the button docks there; away from it, it is the
    // full button again.
    if (const auto *parent = Ball->parentWidget()) {
      const bool mirrored = Ball->layoutDirection() == Qt::RightToLeft;
      const int distance =
          mirrored ? Ball->geometry().left()
                   : parent->rect().right() - Ball->geometry().right();
      setDocked(distance <= DockReach, false);
    }
    LauncherOffset = cornerOffset(*Ball);
    QSettings().setValue(LauncherOffsetKey, *LauncherOffset);
    applyPlaces();
    return true;
  }

  void setDocked(bool docked, bool place = true) {
    Ball->setDocked(docked);
    QSettings().setValue(LauncherDockedKey, docked);
    if (place)
      applyPlaces();
  }

  void restorePlaces() {
    QSettings settings;
    if (const auto offset = settings.value(LauncherOffsetKey);
        offset.metaType().id() == QMetaType::QPoint)
      LauncherOffset = offset.toPoint();
    Ball->setDocked(settings.value(LauncherDockedKey, false).toBool());
    if (const auto offset = settings.value(ChatOffsetKey);
        offset.metaType().id() == QMetaType::QPoint)
      ChatOffset = offset.toPoint();
    if (const auto size = settings.value(ChatSizeKey);
        size.metaType().id() == QMetaType::QSize)
      Panel->setChosenSize(size.toSize().expandedTo(ChatMinimumSize));
    applyPlaces();
  }

  // Each element keeps its distance from the workbench's bottom-right corner;
  // a docked button keeps to the side.
  void applyPlaces() {
    if (LauncherOffset || Ball->docked()) {
      QPoint offset = LauncherOffset.value_or(QPoint());
      const bool mirrored = Ball->layoutDirection() == Qt::RightToLeft;
      const QSize size = Ball->sizeHint();
      QPoint topLeft = topLeftFor(*Ball, offset, size);
      if (Ball->docked() && Ball->parentWidget())
        topLeft.setX(mirrored ? 0
                              : Ball->parentWidget()->width() - size.width());
      Ball->setProperty(ViewPositionProperty, topLeft);
    } else {
      Ball->setProperty(ViewPositionProperty, QVariant());
    }
    if (ChatOffset)
      Panel->setProperty(ViewPositionProperty,
                         topLeftFor(*Panel, *ChatOffset, Panel->sizeHint()));
    else
      Panel->setProperty(ViewPositionProperty, QVariant());
  }

  void saveChatPlace() {
    ChatOffset = cornerOffset(*Panel);
    QSettings settings;
    settings.setValue(ChatOffsetKey, *ChatOffset);
    settings.setValue(ChatSizeKey, Panel->chosenSize());
  }

  void resetChatPlace() {
    ChatOffset.reset();
    Panel->setChosenSize(ChatSize);
    QSettings settings;
    settings.remove(ChatOffsetKey);
    settings.remove(ChatSizeKey);
    applyPlaces();
  }

  void toggleFromKeyboard() {
    auto *current = QApplication::focusWidget();
    if (!Panel->isVisible()) {
      showPanel(true);
    } else if (!Panel->isAncestorOf(current)) {
      FocusBefore = current;
      Prompt->setFocus(Qt::ShortcutFocusReason);
    } else {
      hidePanel();
    }
  }

  void showPanel(bool focus) {
    dismissTeaching();
    if (auto *current = QApplication::focusWidget();
        current && !Panel->isAncestorOf(current))
      FocusBefore = current;
    Panel->show();
    Panel->raise();
    Ball->setUnread(false);
    if (focus)
      Prompt->setFocus(Qt::OtherFocusReason);
    if (!Ready)
      openConversation(Host.captureContext());
  }

  // Closing gives the keyboard back to the view that had it, as VS Code's
  // quick chat returns to the editor.
  void hidePanel() {
    const bool hadFocus = Panel->isAncestorOf(QApplication::focusWidget());
    Panel->hide();
    if (hadFocus && FocusBefore && FocusBefore->isVisible())
      FocusBefore->setFocus(Qt::OtherFocusReason);
  }

  void maybeTeach() {
    if (!Teaching || Teaching->isVisible() || !HasProject ||
        !Ball->isVisible() || Panel->isVisible() ||
        QSettings().value(TeachingKey, false).toBool())
      return;
    Teaching->show();
    Teaching->raise();
  }

  void dismissTeaching() {
    if (!Teaching || !Teaching->isVisible())
      return;
    Teaching->hide();
    QSettings().setValue(TeachingKey, true);
  }

  void setLauncherVisible(bool visible) {
    QSettings settings;
    settings.setValue(LauncherVisibleKey, visible);
    if (LauncherAction)
      LauncherAction->setChecked(visible);
    if (visible) {
      Ball->show();
      return;
    }
    dismissTeaching();
    Ball->hide();
    if (settings.value(LauncherHintKey, false).toBool())
      return;
    settings.setValue(LauncherHintKey, true);
    const auto shortcut = QKeySequence(QString::fromLatin1(OpenShortcut))
                              .toString(QKeySequence::NativeText);
    auto *hint = new QMessageBox(
        QMessageBox::Information, tr("NeverD Agent"),
        tr("%1 still opens the conversation. To show the button again, choose "
           "Plugins → NeverD Agent → Show Ask Agent button.")
            .arg(shortcut),
        QMessageBox::Ok, Ball->parentWidget());
    hint->setObjectName("NeverDAgentLauncherHint");
    hint->setAttribute(Qt::WA_DeleteOnClose);
    hint->open();
  }

  void updateContext(const QJsonObject &context) {
    const bool wasLoaded = HasProject;
    HasProject = context.value("loaded").toBool();
    const auto function = context.value("function_address").toString();
    const auto address = context.value("address").toString();
    if (!HasProject)
      ContextLabel->setText(tr("Open a binary to ask about its analysis."));
    else if (!function.isEmpty())
      ContextLabel->setText(tr("Current function · %1").arg(function));
    else if (!address.isEmpty())
      ContextLabel->setText(tr("Current address · %1").arg(address));
    else
      ContextLabel->setText(tr("Current workbench project"));
    Ball->setSubject(HasProject ? (!function.isEmpty() ? function : address)
                                : QString());
    updateControls();
    if (!wasLoaded && HasProject && Panel->isVisible())
      openConversation(context);
    maybeTeach();
  }

  void showConversation(bool hasMessages) {
    Welcome->setVisible(!hasMessages);
    History->setVisible(hasMessages);
  }

  void updateControls() {
    if (!Panel)
      return;
    for (auto *suggestion : Suggestions)
      suggestion->setEnabled(HasProject && !Running);
    Send->setText(Running ? tr("Stop") : tr("Send"));
    Send->setToolTip(Running ? tr("Stop the current response")
                             : Prompt->toolTip());
    Send->setEnabled(
        Running || (HasProject && !Prompt->toPlainText().trimmed().isEmpty()));
    Ball->setBusy(Running);
  }

  void setStatus(const QString &text) {
    if (Panel) {
      Status->setText(text);
      Status->setVisible(!text.isEmpty());
    }
  }

  bool invoke(const QString &method, const QJsonArray &args = {}) {
    return Process.send(agent::wire::invoke(++RequestId, method, args));
  }

  void sendOrStop() {
    if (Running) {
      if (!Pending.isEmpty()) {
        Pending = {};
        Running = false;
        setStatus(Ready ? QString{} : tr("Loading saved conversation…"));
        updateControls();
      } else if (Ready) {
        invoke("stop");
      }
      return;
    }
    auto context = Host.captureContext();
    const auto text = Prompt->toPlainText().trimmed();
    if (!context.value("loaded").toBool() || text.isEmpty())
      return;
    if (!IncludeContext->isChecked()) {
      context.remove("address");
      context.remove("function_address");
      context.remove("selection_text");
    }
    Pending = {{"text", text}, {"context", context}};
    Running = true;
    updateControls();
    if (Ready) {
      preparePending();
      return;
    }
    openConversation(context);
  }

  void openConversation(const QJsonObject &context) {
    if (Ready || Process.active() || !context.value("loaded").toBool())
      return;
    Binding = {};
    for (const auto *field :
         {"loaded", "epoch", "project_id", "input_path", "project_path"})
      Binding.insert(field, context.value(field));
    QString error;
    if (!Process.start({}, LoadedLanguage, error)) {
      disconnected(error, true);
      return;
    }
    setStatus(tr("Loading saved conversation…"));
    auto request = agent::wire::initialize("workbench");
    request["context"] = Binding;
    if (!Process.send(request))
      disconnected(tr("Could not initialize NeverD Agent CLI."), true);
  }

  void preparePending() {
    if (AnalysisReady) {
      submit();
      return;
    }
    QString error;
    const auto attachment =
        Host.attachSession(Pending.value("context").toObject(), error);
    if (attachment.isEmpty()) {
      Pending = {};
      Running = false;
      setStatus(error);
      updateControls();
      return;
    }
    if (!invoke("prepare", {attachment})) {
      disconnected(tr("Could not prepare NeverD Agent analysis."), true);
      return;
    }
    setStatus(tr("Preparing analysis…"));
  }

  void submit() {
    showConversation(true);
    const auto request = std::exchange(Pending, {});
    if (!invoke("send", {request.value("text"), request.value("context")})) {
      disconnected(tr("The question could not be delivered."), true);
      return;
    }
    Prompt->clear();
    setStatus(tr("Analyzing the captured selection…"));
  }

  void receive(const QJsonObject &message) {
    const auto type = message.value("type").toString();
    if (type == "ready") {
      if (message.value("protocol").toInt() != agent::wire::kVersion) {
        disconnected(tr("Incompatible NeverD Agent CLI protocol."), true);
        return;
      }
      Ready = true;
      if (Pending.isEmpty())
        setStatus({});
      else
        preparePending();
    } else if (type == "analysis_ready") {
      AnalysisReady = true;
      if (!Pending.isEmpty())
        submit();
    } else if (type == "messages") {
      History->replaceTail(message.value("from").toInteger(-1),
                           message.value("entries").toArray());
      showConversation(!History->empty());
    } else if (type == "execution") {
      History->updateExecution(message.value("runId").toString(),
                               message.value("from").toInteger(-1),
                               message.value("rows").toArray());
    } else if (type == "error" && message.value("fatal").toBool()) {
      disconnected(message.value("text").toString(), true);
    } else if (type == "response" && message.contains("error")) {
      setStatus(message.value("error").toString());
    } else if (type == "state" || type == "error") {
      if (!Pending.isEmpty()) {
        if (type == "state")
          return;
        if (Ready && !message.value("fatal").toBool()) {
          setStatus(message.value("text").toString());
          return;
        }
        Pending = {};
      }
      if (Running && !message.value("running").toBool() && !Panel->isVisible())
        Ball->setUnread(true);
      Running = message.value("running").toBool();
      setStatus(type == "error" ? message.value("text").toString()
                : Running       ? message.value("activity").toString()
                                : QString{});
      updateControls();
    } else if (type == "navigate" &&
               !Host.navigate(message.value("address").toString(), Binding)) {
      setStatus(tr("The evidence belongs to a closed analysis session."));
    }
  }

  void disconnected(const QString &text, bool replaceStatus = false) {
    Process.close();
    const bool wasRunning = Running;
    Running = Ready = AnalysisReady = false;
    Pending = {};
    if (Panel && (replaceStatus || wasRunning || Status->text().isEmpty()))
      setStatus(text);
    updateControls();
  }

  Context &Host;
  const AppearanceContext *Appearance;
  QTranslator Translator;
  QString LoadedLanguage;
  QPointer<Launcher> Ball;
  QPointer<FloatingCard> Panel;
  QPointer<QWidget> Teaching;
  QPointer<QWidget> FocusBefore;
  QAction *OpenAction = nullptr;
  QAction *LauncherAction = nullptr;
  QLabel *Title = nullptr;
  QLabel *ContextLabel = nullptr;
  QLabel *Status = nullptr;
  QWidget *Welcome = nullptr;
  QLabel *WelcomeTitle = nullptr;
  QLabel *WelcomeText = nullptr;
  std::array<SuggestionButton *, SuggestionCount> Suggestions{};
  QLabel *TeachingTitle = nullptr;
  QLabel *TeachingText = nullptr;
  QPushButton *TeachingDone = nullptr;
  GlyphButton *SettingsButton = nullptr;
  GlyphButton *CloseButton = nullptr;
  ChatTranscript *History = nullptr;
  QPlainTextEdit *Prompt = nullptr;
  QCheckBox *IncludeContext = nullptr;
  QPushButton *Send = nullptr;
  agent::AgentProcess Process;
  QJsonObject Binding, Pending;
  qint64 RequestId = 0;
  std::optional<QPointF> PressPosition;
  std::optional<QPoint> LauncherOffset, ChatOffset;
  QPoint LauncherOrigin;
  bool HasProject = false;
  bool Ready = false;
  bool AnalysisReady = false;
  bool Running = false;
  bool PromptComposing = false;
  bool Dragging = false;
};
} // namespace

class AgentUiPlugin final : public QObject, public Module {
  Q_OBJECT
  Q_PLUGIN_METADATA(IID NEVERD_GUI_PLUGIN_UI_IID)
  Q_INTERFACES(neverd::gui::pluginui::Module)

public:
  bool activate(Context &context, QObject &lifetime, QString &error) override {
    return (new FloatingChat(context, lifetime))->mount(error);
  }
};
} // namespace neverd::gui::pluginui

#include "AgentUiPlugin.moc"
