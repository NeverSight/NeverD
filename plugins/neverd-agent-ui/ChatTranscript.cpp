//===- ChatTranscript.cpp - Floating Agent messages -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "ChatTranscript.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTextDocument>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <utility>

namespace neverd::gui::pluginui {
namespace {
constexpr int BubblePadding = 12;
constexpr int UserWidthPercent = 82;
const QTextDocument::MarkdownFeatures MessageMarkdown =
    QTextDocument::MarkdownFeatures(QTextDocument::MarkdownDialectGitHub) |
    QTextDocument::MarkdownNoHTML;
constexpr int VisibleExecutionRows = 5;

class MessageText final : public QTextBrowser {
public:
  explicit MessageText(QWidget *parent) : QTextBrowser(parent) {
    setObjectName("NeverDAgentMessageText");
    setOpenLinks(false);
    setFrameShape(NoFrame);
    viewport()->setAutoFillBackground(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    document()->setDocumentMargin(0);
    document()->setIndentWidth(22);
    connect(document()->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this](const QSizeF &size) { fitHeight(size.height()); });
    connect(horizontalScrollBar(), &QScrollBar::rangeChanged, this,
            [this](int, int) { fitHeight(document()->size().height()); });
  }

  QSize minimumSizeHint() const override { return {0, minimumHeight()}; }

protected:
  QVariant loadResource(int, const QUrl &) override { return {}; }

  void wheelEvent(QWheelEvent *event) override {
    if (event->angleDelta().x() || event->pixelDelta().x() ||
        event->modifiers().testFlag(Qt::ShiftModifier))
      QTextBrowser::wheelEvent(event);
    else
      event->ignore();
  }

private:
  void fitHeight(qreal height) {
    const int horizontal = horizontalScrollBar()->maximum() > 0
                               ? horizontalScrollBar()->sizeHint().height()
                               : 0;
    const int needed =
        std::max(fontMetrics().height(), qCeil(height)) + horizontal;
    if (minimumHeight() != needed)
      setFixedHeight(needed);
  }
};

class ExecutionLog final : public QWidget {
public:
  ExecutionLog(QString runId, const ChatTranscript::OpenLink &openLink,
               const ChatTranscript::RequestExecution &request, QWidget *parent)
      : QWidget(parent), RunId(std::move(runId)) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 2, 0, 0);
    layout->setSpacing(4);
    Toggle = new QToolButton(this);
    Toggle->setObjectName("NeverDAgentExecutionToggle");
    // A disclosure, not a toggled tool: its chevron shows whether it is open,
    // so the workbench does not fill it with the checked-tool color. Keyboard
    // focus frames it; a click leaves no frame behind.
    Toggle->setFocusPolicy(Qt::TabFocus);
    Toggle->setArrowType(Qt::RightArrow);
    Toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    layout->addWidget(Toggle, 0, Qt::AlignLeft);
    Latest = new QLabel(this);
    Latest->setObjectName("NeverDAgentExecutionLatest");
    Latest->setTextFormat(Qt::PlainText);
    Latest->setWordWrap(true);
    layout->addWidget(Latest);

    Tree = new QTreeWidget(this);
    Tree->setObjectName("NeverDAgentExecutionRows");
    Tree->setAccessibleName(ChatTranscript::tr("Tool execution records"));
    Tree->setColumnCount(2);
    Tree->setHeaderHidden(true);
    Tree->setRootIsDecorated(false);
    Tree->setUniformRowHeights(true);
    Tree->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    Tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    Tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    layout->addWidget(Tree);

    connect(Toggle, &QToolButton::clicked, this, [this, request] {
      Expanded = !Expanded;
      Toggle->setArrowType(Expanded ? Qt::DownArrow : Qt::RightArrow);
      if (Expanded && !Received)
        request(RunId);
      refresh();
    });
    connect(Tree, &QTreeWidget::itemActivated, this,
            [openLink](const QTreeWidgetItem *item, int) {
              const auto id = item->data(0, Qt::UserRole).toString();
              if (!id.isEmpty())
                openLink(QUrl(QStringLiteral("nd-evidence://") + id));
            });
    refresh();
  }

  bool hasRecords() const { return !Records.isEmpty(); }

  void retranslate() {
    Tree->setAccessibleName(ChatTranscript::tr("Tool execution records"));
    for (qsizetype i = 0; i < Records.size(); ++i)
      describe(int(i));
    refresh();
  }

  void setRunning(bool running) {
    if (Running == running)
      return;
    Running = running;
    for (qsizetype i = 0; i < Records.size(); ++i)
      if (Records[i].value("state") == "running")
        Tree->topLevelItem(int(i))->setText(1, statusText(Records[i]));
    refresh();
  }

  void applyRows(qsizetype first, const QJsonArray &entries) {
    if (first < 0 || first > Records.size())
      return;
    Received = true;
    for (qsizetype i = 0; i < entries.size(); ++i) {
      const auto index = first + i;
      const auto record = entries.at(i).toObject();
      if (index == Records.size()) {
        Records.append(record);
        new QTreeWidgetItem(Tree);
      } else {
        Records[index] = record;
      }
      describe(int(index));
    }
    refresh();
  }

protected:
  void changeEvent(QEvent *event) override {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::LanguageChange)
      retranslate();
  }

private:
  void describe(int index) {
    const auto &record = Records[index];
    auto *item = Tree->topLevelItem(index);
    const auto title = description(record);
    const auto evidence = record.value("evidenceId").toString();
    const auto error = record.value("error").toString();
    item->setText(0, title);
    item->setText(1, statusText(record));
    item->setData(0, Qt::UserRole, evidence);
    auto details = title.toHtmlEscaped();
    if (!error.isEmpty())
      details += QStringLiteral("<br>") + error.toHtmlEscaped();
    if (!evidence.isEmpty())
      details += QStringLiteral("<br>") +
                 ChatTranscript::tr("Activate this row to open its evidence.")
                     .toHtmlEscaped();
    item->setToolTip(0, QStringLiteral("<qt>%1</qt>").arg(details));
  }

  static QString description(const QJsonObject &record) {
    QStringList parts;
    for (const auto *field : {"label", "name", "address"})
      if (const auto value = record.value(field).toString(); !value.isEmpty())
        parts.append(value);
    return parts.join(QStringLiteral(" · "));
  }

  QString statusText(const QJsonObject &record) const {
    const auto state = record.value("state").toString();
    QString text;
    if (state == "running")
      return Running ? ChatTranscript::tr("Running")
                     : ChatTranscript::tr("Interrupted");
    if (state == "succeeded")
      text = ChatTranscript::tr("Done");
    else if (state == "failed")
      text = ChatTranscript::tr("Failed");
    else if (state == "cancelled")
      text = ChatTranscript::tr("Stopped");
    else if (state == "skipped")
      text = ChatTranscript::tr("Skipped");
    const auto elapsed = record.value("elapsedMs").toInteger();
    if (elapsed > 0)
      text = ChatTranscript::tr("%1 · %2 s")
                 .arg(text, QLocale().toString(double(elapsed) / 1000, 'f', 1));
    return text;
  }

  void refresh() {
    Toggle->setText(
        Records.isEmpty()
            ? ChatTranscript::tr("Execution")
            : ChatTranscript::tr("Execution · %1").arg(Records.size()));
    const bool expanded = Expanded;
    QString latest;
    if (expanded && Records.isEmpty())
      latest = Running ? ChatTranscript::tr("Waiting for tool calls…")
                       : ChatTranscript::tr("No tool calls.");
    else if (!expanded && Running && !Records.isEmpty())
      latest = description(Records.last()) + QStringLiteral(" · ") +
               statusText(Records.last());
    Latest->setText(latest);
    Latest->setVisible(!latest.isEmpty());
    Tree->setVisible(expanded && !Records.isEmpty());
    if (expanded && !Records.isEmpty()) {
      const int rows =
          std::min(Tree->topLevelItemCount(), VisibleExecutionRows);
      const int rowHeight = Tree->sizeHintForRow(0);
      Tree->setFixedHeight(
          rows * std::max(rowHeight, Tree->fontMetrics().height()) + 4);
    }
  }

  QString RunId;
  QToolButton *Toggle = nullptr;
  QLabel *Latest = nullptr;
  QTreeWidget *Tree = nullptr;
  QList<QJsonObject> Records;
  bool Received = false;
  bool Running = false;
  bool Expanded = false;
};
} // namespace

class ChatTranscript::Bubble final : public QFrame {
public:
  Bubble(bool user, const OpenLink &openLink, QWidget *parent)
      : QFrame(parent), User(user) {
    setObjectName(User ? "NeverDAgentUserMessage" : "NeverDAgentReply");
    setAttribute(Qt::WA_StyledBackground);
    // Never taller than its text: a message that unwraps gives back the
    // height its wrapped lines took.
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(BubblePadding, 9, BubblePadding, 10);
    layout->setSpacing(4);
    Speaker = new QLabel(
        User ? ChatTranscript::tr("You") : ChatTranscript::tr("Agent"), this);
    Speaker->setObjectName("NeverDAgentSpeaker");
    layout->addWidget(Speaker);
    Body = new MessageText(this);
    layout->addWidget(Body);
    connect(Body, &QTextBrowser::anchorClicked, this, openLink);
  }

  bool User;

  void setMessage(const QJsonObject &entry, const OpenLink &openLink,
                  const RequestExecution &request) {
    Running = entry.value("status") == "running";
    Source = entry.value("text").toString();
    const auto id = entry.value("runId").toString();
    if (RunId != id) {
      delete Log;
      Log = nullptr;
      RunId = id;
      if (!User && !RunId.isEmpty()) {
        Log = new ExecutionLog(RunId, openLink, request, this);
        static_cast<QVBoxLayout *>(layout())->addWidget(Log);
      }
    }
    if (Log)
      Log->setRunning(Running);
    refreshBody();
  }

  const QString &runId() const { return RunId; }

  void retranslate() {
    Speaker->setText(User ? ChatTranscript::tr("You")
                          : ChatTranscript::tr("Agent"));
    Displayed.clear();
    refreshBody();
  }

  void updateExecution(qsizetype first, const QJsonArray &records) {
    if (Log) {
      Log->applyRows(first, records);
      refreshBody();
    }
  }

  void refreshBody() {
    const auto text = Source.isEmpty() && Running && !(Log && Log->hasRecords())
                          ? ChatTranscript::tr("Working…")
                          : Source;
    Body->setVisible(!text.isEmpty());
    if (Displayed == text)
      return;
    Displayed = text;
    Body->document()->setMarkdown(Displayed, MessageMarkdown);
  }

  void fitWidth(int width) {
    int bubbleWidth = width;
    if (User) {
      // The widest line as the message's own layout sets it, unwrapped, and
      // the cursor's pixel: summed font advances round below what the text
      // layout needs, which wrapped short messages that had room.
      QTextDocument probe;
      probe.setDocumentMargin(0);
      probe.setDefaultFont(Body->document()->defaultFont());
      probe.setMarkdown(Source, MessageMarkdown);
      const int natural =
          std::max(Speaker->sizeHint().width(), qCeil(probe.idealWidth()) + 1);
      bubbleWidth = std::min(natural + 2 * (BubblePadding + BubbleBorderWidth),
                             width * UserWidthPercent / 100);
    }
    setFixedWidth(std::max(1, bubbleWidth));
  }

private:
  QLabel *Speaker = nullptr;
  MessageText *Body = nullptr;
  ExecutionLog *Log = nullptr;
  QString Source, Displayed, RunId;
  bool Running = false;
};

ChatTranscript::ChatTranscript(OpenLink openLink, RequestExecution request,
                               QWidget *parent)
    : QScrollArea(parent), Navigate(std::move(openLink)),
      RequestLog(std::move(request)) {
  setObjectName("NeverDAgentTranscript");
  setAccessibleName(tr("Conversation"));
  setFrameShape(NoFrame);
  viewport()->setAutoFillBackground(false);
  setWidgetResizable(true);
  setFocusPolicy(Qt::StrongFocus);
  setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  verticalScrollBar()->setObjectName("NeverDAgentChatScrollBar");

  auto *content = new QWidget;
  content->setObjectName("NeverDAgentMessages");
  Messages = new QVBoxLayout(content);
  Messages->setContentsMargins(0, 3, 3, 3);
  Messages->setSpacing(9);
  Messages->setSizeConstraint(QLayout::SetMinAndMaxSize);
  Messages->addStretch();
  setWidget(content);
  content->setAutoFillBackground(false);

  auto *scroll = verticalScrollBar();
  connect(scroll, &QScrollBar::valueChanged, this, [this, scroll](int value) {
    FollowingTail = value == scroll->maximum();
  });
  // A hidden panel can get its final scroll range only when reopened.
  connect(scroll, &QScrollBar::rangeChanged, this,
          [this, scroll](int, int maximum) {
            if (FollowingTail)
              scroll->setValue(maximum);
          });
}

int ChatTranscript::messageWidth() const {
  const auto margins = Messages->contentsMargins();
  return viewport()->width() - margins.left() - margins.right();
}

void ChatTranscript::changeEvent(QEvent *event) {
  QScrollArea::changeEvent(event);
  if (event->type() != QEvent::LanguageChange)
    return;
  setAccessibleName(tr("Conversation"));
  for (auto *bubble : Bubbles)
    bubble->retranslate();
}

bool ChatTranscript::viewportEvent(QEvent *event) {
  const bool handled = QScrollArea::viewportEvent(event);
  if (event->type() == QEvent::Resize && Messages)
    for (auto *bubble : Bubbles)
      bubble->fitWidth(messageWidth());
  return handled;
}

void ChatTranscript::replaceTail(qsizetype first, const QJsonArray &entries) {
  if (first < 0 || first > Bubbles.size())
    return;
  const auto end = first + entries.size();
  while (Bubbles.size() > end)
    delete Bubbles.takeLast();
  for (qsizetype index = first; index < end; ++index) {
    const auto entry = entries.at(index - first).toObject();
    const bool user = entry.value("role") == "user";
    auto *bubble = Bubbles.value(index);
    if (bubble && bubble->User != user) {
      delete bubble;
      bubble = nullptr;
    }
    if (!bubble) {
      bubble = new Bubble(user, Navigate, widget());
      if (index == Bubbles.size())
        Bubbles.append(bubble);
      else
        Bubbles[index] = bubble;
      Messages->insertWidget(int(index), bubble, 0,
                             user ? Qt::AlignRight : Qt::AlignLeft);
    }
    bubble->setMessage(entry, Navigate, RequestLog);
    bubble->fitWidth(messageWidth());
  }
}

void ChatTranscript::updateExecution(const QString &runId, qsizetype first,
                                     const QJsonArray &records) {
  if (runId.isEmpty())
    return;
  for (auto *bubble : Bubbles)
    if (bubble->runId() == runId) {
      bubble->updateExecution(first, records);
      return;
    }
}
} // namespace neverd::gui::pluginui
