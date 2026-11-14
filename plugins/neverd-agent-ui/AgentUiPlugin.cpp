//===- AgentUiPlugin.cpp - Floating Agent conversation --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ChatTranscript.h"
#include "GuiPluginUi.h"
#include "client/AgentProcess.h"
#include "client/AgentProtocol.h"
#include "platform/AppPaths.h"

#include <QAction>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QCursor>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QJsonArray>
#include <QKeyEvent>
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
#include <QStyle>
#include <QStyleHints>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QVariant>
#include <algorithm>
#include <optional>
#include <utility>

namespace neverd::gui::pluginui {
namespace {
constexpr QSize LauncherSize{48, 48};
constexpr QSize ChatSize{440, 520};
constexpr int RevealDelayMs = 180;
constexpr int HideDelayMs = 300;
constexpr auto LauncherVisibleKey =
    "guiPlugins/neverd.agent-ui/launcherVisible";
constexpr auto LauncherHintKey = "guiPlugins/neverd.agent-ui/launcherHintShown";

// The corners of cards, as the workbench rounds its floating cards.
constexpr int CardRadius = 6;

// The workbench's color roles: the host's theme when it offers one, else the
// application palette's nearest roles, which the workbench also sets.
class Colors {
public:
  explicit Colors(const ThemeContext *theme = nullptr) : Theme(theme) {}
  QColor operator()(const char *role) const {
    if (Theme)
      if (const auto color = Theme->themeColor(QLatin1String(role));
          color.isValid())
        return color;
    const auto palette = QGuiApplication::palette();
    const QLatin1StringView name(role);
    if (name == QLatin1StringView("Menu"))
      return palette.color(QPalette::Window);
    if (name == QLatin1StringView("Button"))
      return palette.color(QPalette::Button);
    if (name == QLatin1StringView("ButtonHover") ||
        name == QLatin1StringView("ToolBarHover"))
      return palette.color(QPalette::Midlight);
    if (name == QLatin1StringView("ButtonText"))
      return palette.color(QPalette::ButtonText);
    if (name == QLatin1StringView("SecondaryText"))
      return palette.color(QPalette::PlaceholderText);
    if (name == QLatin1StringView("Accent") ||
        name == QLatin1StringView("Focus"))
      return palette.color(QPalette::Accent);
    if (name == QLatin1StringView("Border"))
      return palette.color(QPalette::Mid);
    return palette.color(QPalette::Text);
  }

private:
  const ThemeContext *Theme;
};

// Standard controls keep the workbench's own style. This sheet gives only the
// card, the messages and the quiet labels their roles.
QString chatStyle(const Colors &color) {
  const auto hex = [&](const char *role) {
    return color(role).name(QColor::HexArgb);
  };
  return QStringLiteral(R"css(
QFrame#NeverDAgentChat {
  background: %1; border: 1px solid %2; border-radius: %3px;
}
QLabel#NeverDAgentContext, QLabel#NeverDAgentStatus, QLabel#NeverDAgentSpeaker,
QLabel#NeverDAgentExecutionLatest {
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
  background: %5; border: %6px solid %2; border-radius: %3px;
}
QFrame#NeverDAgentReply { background: transparent; border: none; }
QTextBrowser#NeverDAgentMessageText {
  background: transparent; border: none; padding: 0;
}
QTreeWidget#NeverDAgentExecutionRows { border: none; }
)css")
      .arg(hex("Menu"), hex("Border"), QString::number(CardRadius),
           hex("SecondaryText"), hex("Button"),
           QString::number(BubbleBorderWidth), hex("ToolBarHover"),
           hex("Focus"));
}

class Launcher final : public QToolButton {
public:
  Launcher(const ThemeContext *theme, QWidget *parent)
      : QToolButton(parent), Color(theme) {}
  QSize sizeHint() const override { return LauncherSize; }
  bool Busy = false;
  bool Unread = false;

protected:
  // A push button's gray face inside the workbench's hairline; keyboard
  // focus rings it in the focus color, and the accent marks work or news.
  void paintEvent(QPaintEvent *) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto circle = QRectF(rect()).adjusted(1, 1, -1, -1);
    const QColor accent = Color("Accent");
    const QColor face = Color(underMouse() ? "ButtonHover" : "Button");
    const QColor glyph = Color("ButtonText");
    const bool ringed = hasFocus() && FocusByKeyboard;
    painter.setPen(QPen(ringed ? Color("Focus") : Color("Border"), 1));
    painter.setBrush(face);
    painter.drawEllipse(circle);
    painter.translate(circle.center());
    painter.setPen(
        QPen(glyph, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    QPainterPath bubble;
    bubble.moveTo(-6, -8);
    bubble.lineTo(6, -8);
    bubble.quadTo(10, -8, 10, -4);
    bubble.lineTo(10, 3);
    bubble.quadTo(10, 7, 6, 7);
    bubble.lineTo(-1, 7);
    bubble.lineTo(-6, 11);
    bubble.lineTo(-6, 7);
    bubble.quadTo(-10, 7, -10, 3);
    bubble.lineTo(-10, -4);
    bubble.quadTo(-10, -8, -6, -8);
    painter.drawPath(bubble);
    painter.setPen(Qt::NoPen);
    painter.setBrush(glyph);
    for (const int x : {-4, 0, 4})
      painter.drawEllipse(QPointF(x, 0), 1, 1);
    if (Busy || Unread) {
      painter.setPen(QPen(face, 1.4));
      painter.setBrush(accent);
      painter.drawEllipse(QPointF(14, -14), 4, 4);
    }
  }
  void focusInEvent(QFocusEvent *event) override {
    FocusByKeyboard = event->reason() != Qt::MouseFocusReason;
    QToolButton::focusInEvent(event);
  }

private:
  Colors Color;
  bool FocusByKeyboard = false;
};

class ChatFrame final : public QFrame {
public:
  ChatFrame(const ThemeContext *theme, QWidget *parent)
      : QFrame(parent), Color(theme) {
    setObjectName("NeverDAgentChat");
    setAttribute(Qt::WA_StyledBackground);
    Sheet = chatStyle(Color);
    setStyleSheet(Sheet);
  }

  QSize sizeHint() const override { return ChatSize; }

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

private:
  Colors Color;
  QString Sheet;
};

class FloatingChat final : public QObject {
public:
  FloatingChat(Context &context, QObject &lifetime)
      : QObject(&lifetime), Host(context),
        Theme(dynamic_cast<ThemeContext *>(&context)) {}

  bool mount(QString &error) {
    auto *ball = Host.mountView(
        "launcher", {},
        [this](QWidget *parent) {
          auto *button = new Launcher(Theme, parent);
          button->setObjectName("NeverDAgentLauncher");
          button->setAccessibleName(tr("Open NeverD Agent"));
          button->setToolTip(tr(
              "NeverD Agent · Hover to preview, click to open, drag to move"));
          button->setCursor(Qt::OpenHandCursor);
          button->setFocusPolicy(Qt::StrongFocus);
          return button;
        },
        error);
    if (!ball)
      return false;
    Ball = static_cast<Launcher *>(ball);
    auto *panel = Host.mountView(
        "chat", {Qt::BottomRightCorner, "launcher"},
        [this](QWidget *parent) { return makePanel(parent); }, error);
    if (!panel)
      return false;
    Panel = panel;
    Panel->hide();
    if (auto *menu = dynamic_cast<MenuContext *>(&Host)) {
      if (!menu->mountMenuAction(
              "show-launcher",
              [this](QObject *parent) {
                auto *action = new QAction(tr("Show floating ball"), parent);
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
    Ball->installEventFilter(this);
    Panel->installEventFilter(this);
    Prompt->installEventFilter(this);
    Reveal.setSingleShot(true);
    Reveal.setInterval(RevealDelayMs);
    connect(&Reveal, &QTimer::timeout, this, [this] { showPanel(false); });
    Hide.setSingleShot(true);
    Hide.setInterval(HideDelayMs);
    connect(&Hide, &QTimer::timeout, this, [this] {
      if (!Pin->isChecked() && !pointerInside(Ball) && !pointerInside(Panel))
        hidePanel();
    });
    connect(Ball, &QToolButton::clicked, this, [this] {
      if (Panel->isVisible() && (Pin->isChecked() || OpenedByClick))
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
    updateContext(Host.captureContext());
    Ball->setVisible(!LauncherAction || LauncherAction->isChecked());
    return true;
  }

private:
  QWidget *makePanel(QWidget *parent) {
    auto *panel = new ChatFrame(Theme, parent);
    auto *layout = new QVBoxLayout(panel);
    layout->setContentsMargins(14, 11, 14, 14);
    layout->setSpacing(9);
    layout->setSizeConstraint(QLayout::SetNoConstraint);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(4);
    auto *title = new QLabel(tr("NeverD Agent"), panel);
    title->setObjectName("NeverDAgentTitle");
    auto font = title->font();
    font.setBold(true);
    title->setFont(font);
    heading->addWidget(title, 1);
    auto *settings = new QToolButton(panel);
    settings->setAutoRaise(true);
    settings->setText(tr("Settings"));
    settings->setToolTip(
        tr("Open the standalone Agent to configure the model"));
    connect(settings, &QToolButton::clicked, this, [this] {
      const auto program = agent::findAgentExecutable();
      if (program.isEmpty() ||
          !QProcess::startDetached(program, {"--show-settings"}))
        setStatus(tr("The standalone NeverD Agent is not available."));
    });
    heading->addWidget(settings);
    Pin = new QToolButton(panel);
    Pin->setObjectName("NeverDAgentPin");
    Pin->setAutoRaise(true);
    Pin->setText(tr("Pin"));
    Pin->setCheckable(true);
    Pin->setToolTip(tr("Keep this conversation open"));
    connect(Pin, &QToolButton::toggled, this, [this](bool value) {
      if (!value)
        Hide.start();
    });
    heading->addWidget(Pin);
    auto *close = new QToolButton(panel);
    close->setAutoRaise(true);
    close->setText(QStringLiteral("×"));
    close->setAccessibleName(tr("Hide Agent conversation"));
    connect(close, &QToolButton::clicked, this, [this] { hidePanel(); });
    heading->addWidget(close);
    layout->addLayout(heading);
    ContextLabel = new QLabel(panel);
    ContextLabel->setObjectName("NeverDAgentContext");
    ContextLabel->setWordWrap(true);
    ContextLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(ContextLabel);
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
    layout->addWidget(History, 1);
    Status = new QLabel(panel);
    Status->setObjectName("NeverDAgentStatus");
    Status->setTextFormat(Qt::PlainText);
    Status->setWordWrap(true);
    Status->hide();
    layout->addWidget(Status);

    auto *composer = new QWidget(panel);
    composer->setObjectName("NeverDAgentComposer");
    auto *compose = new QVBoxLayout(composer);
    compose->setContentsMargins(0, 0, 0, 0);
    compose->setSpacing(6);
    Prompt = new QPlainTextEdit(composer);
    Prompt->setObjectName("NeverDAgentPrompt");
    Prompt->setPlaceholderText(tr("Ask about this function…"));
    Prompt->setToolTip(tr("Enter to send · Shift+Enter for a new line"));
    Prompt->setTabChangesFocus(true);
    Prompt->ensurePolished();
    Prompt->setFixedHeight(Prompt->fontMetrics().lineSpacing() * 3 +
                           2 * Prompt->frameWidth() +
                           qRound(2 * Prompt->document()->documentMargin()));
    connect(Prompt, &QPlainTextEdit::textChanged, this,
            [this] { updateControls(); });
    compose->addWidget(Prompt);
    auto *commands = new QHBoxLayout;
    IncludeContext = new QCheckBox(tr("Attach current selection"), composer);
    IncludeContext->setObjectName("NeverDAgentIncludeContext");
    IncludeContext->setChecked(true);
    commands->addWidget(IncludeContext, 1);
    Send = new QPushButton(tr("Send"), composer);
    Send->setObjectName("NeverDAgentSend");
    // The panel's one accent, without taking Return from the prompt.
    Send->setProperty("mainAction", true);
    Send->setAutoDefault(false);
    connect(Send, &QPushButton::clicked, this, [this] { sendOrStop(); });
    commands->addWidget(Send);
    compose->addLayout(commands);
    layout->addWidget(composer);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), panel);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, [this] { hidePanel(); });
    return panel;
  }

  bool eventFilter(QObject *object, QEvent *event) override {
    if (object == Ball.data() && handleLauncherDrag(event))
      return true;
    if (object == Ball.data() && event->type() == QEvent::ContextMenu &&
        LauncherAction) {
      Reveal.stop();
      Hide.stop();
      auto *menu = new QMenu(Ball);
      connect(menu, &QMenu::aboutToHide, menu, &QObject::deleteLater);
      connect(menu, &QMenu::aboutToHide, this, [this] {
        if (Panel->isVisible() && !Pin->isChecked())
          Hide.start();
      });
      menu->addAction(tr("Hide floating ball"), this,
                      [this] { setLauncherVisible(false); });
      menu->popup(static_cast<QContextMenuEvent *>(event)->globalPos());
      return true;
    }
    if (object == Ball.data() || object == Panel.data()) {
      if (event->type() == QEvent::Enter) {
        Hide.stop();
        if (object == Ball.data() && !PressPosition && !Panel->isVisible())
          Reveal.start();
      } else if (event->type() == QEvent::Leave) {
        Reveal.stop();
        if (Panel->isVisible() && !Pin->isChecked())
          Hide.start();
      }
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

  bool handleLauncherDrag(QEvent *event) {
    switch (event->type()) {
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonDblClick: {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton) {
        PressPosition = mouse->globalPosition();
        LauncherOrigin = Ball->pos();
        Reveal.stop();
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
        if (!Pin->isChecked())
          hidePanel();
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
    Ball->setCursor(Qt::OpenHandCursor);
    return true;
  }

  static bool pointerInside(const QWidget *widget) {
    return widget && widget->isVisible() &&
           widget->rect().contains(widget->mapFromGlobal(QCursor::pos()));
  }

  void showPanel(bool focus) {
    Reveal.stop();
    Hide.stop();
    Panel->show();
    Ball->Unread = false;
    Ball->update();
    OpenedByClick = focus;
    if (focus)
      Prompt->setFocus();
    if (!Ready)
      openConversation(Host.captureContext());
  }

  void hidePanel() {
    Pin->setChecked(false);
    OpenedByClick = false;
    Hide.stop();
    Reveal.stop();
    Panel->hide();
  }

  void setLauncherVisible(bool visible) {
    QSettings settings;
    settings.setValue(LauncherVisibleKey, visible);
    LauncherAction->setChecked(visible);
    if (visible) {
      Ball->show();
      return;
    }
    hidePanel();
    Ball->hide();
    if (settings.value(LauncherHintKey, false).toBool())
      return;
    settings.setValue(LauncherHintKey, true);
    auto *hint = new QMessageBox(
        QMessageBox::Information, tr("NeverD Agent"),
        tr("To show the floating ball again, choose Plugins → NeverD Agent → "
           "Show floating ball."),
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
    updateControls();
    if (!wasLoaded && HasProject && Panel->isVisible())
      openConversation(context);
  }

  void updateControls() {
    if (!Panel)
      return;
    Send->setText(Running ? tr("Stop") : tr("Send"));
    Send->setToolTip(Running ? tr("Stop the current response")
                             : Prompt->toolTip());
    Send->setEnabled(
        Running || (HasProject && !Prompt->toPlainText().trimmed().isEmpty()));
    Ball->Busy = Running;
    Ball->update();
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
    if (!Process.start({}, error)) {
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
        Ball->Unread = true;
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
  const ThemeContext *Theme;
  QPointer<Launcher> Ball;
  QPointer<QWidget> Panel;
  QAction *LauncherAction = nullptr;
  QLabel *ContextLabel = nullptr;
  QLabel *Status = nullptr;
  QToolButton *Pin = nullptr;
  ChatTranscript *History = nullptr;
  QPlainTextEdit *Prompt = nullptr;
  QCheckBox *IncludeContext = nullptr;
  QPushButton *Send = nullptr;
  QTimer Reveal, Hide;
  agent::AgentProcess Process;
  QJsonObject Binding, Pending;
  qint64 RequestId = 0;
  std::optional<QPointF> PressPosition;
  QPoint LauncherOrigin;
  bool HasProject = false;
  bool OpenedByClick = false;
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
