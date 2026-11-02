#include "OutputWindow.h"

#include "../ResourceLimits.h"
#include "Icons.h"
#include "Resolve.h"
#include "Session.h"
#include "Theme.h"

#include <QAction>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMenu>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QSettings>
#include <QTextBlock>
#include <QTextCursor>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <memory>

namespace neverd::gui {
namespace {
constexpr char HistoryKey[] = "output/history";

// Command words of the workbench command line.
struct CommandWord {
  const char *word;
  const char *command;
};
constexpr CommandWord Commands[] = {
    {"g", "jump"},          {"jump", "jump"},
    {"x", "xrefs"},         {"xrefs", "xrefs"},
    {"n", "rename"},        {"rename", "rename"},
    {"c", "comment"},       {"comment", "comment"},
    {"d", "decompile"},     {"decompile", "decompile"},
    {"f", "find"},          {"find", "find"},
    {"analyze", "analyze"}, {"save", "save"},
    {"graph", "graph"},     {"hex", "hex"},
    {"strref", "strref"},   {"strrefs", "strref"},
    {"t", "types"},         {"types", "types"},
};

/// The color the log writes a line of the given level in.
QColor levelColor(int level) {
  const auto &theme = Theme::instance();
  return level >= 2   ? theme.color(ColorRole::ListingError)
         : level == 1 ? theme.color(ColorRole::CodePreprocessor)
                      : theme.color(ColorRole::CodeDefault);
}
} // namespace

OutputWindow::OutputWindow(Session &session, QWidget *parent)
    : QWidget(parent), session_(session), log_(new QPlainTextEdit(this)),
      command_(new QLineEdit(this)), language_(new QToolButton(this)) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(
      ::neverd::limits::gui::kOutputWindowMaximumLogLines);
  log_->setLineWrapMode(QPlainTextEdit::NoWrap);
  log_->setFrameShape(QFrame::NoFrame);
  log_->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(log_, &QWidget::customContextMenuRequested, this, [this](QPoint at) {
    const std::unique_ptr<QMenu> menu(createLogMenu());
    menu->exec(log_->viewport()->mapToGlobal(at));
  });
  layout->addWidget(log_, 1);
  auto *row = new QHBoxLayout;
  row->setContentsMargins(0, 0, 0, 0);
  row->setSpacing(0);
  language_->setText(QStringLiteral("NeverD"));
  language_->setToolTip(tr("Command line language: NeverD expressions and "
                           "commands. Type help for a list."));
  language_->setAutoRaise(true);
  row->addWidget(language_);
  command_->setPlaceholderText(tr("Expression or command (help)"));
  command_->installEventFilter(this);
  row->addWidget(command_, 1);
  layout->addLayout(row);
  history_ = QSettings().value(HistoryKey).toStringList();
  historyIndex_ = int(history_.size());
  const auto applyTheme = [this] {
    log_->setFont(Theme::instance().codeFont());
    command_->setFont(Theme::instance().codeFont());
    // Lines already written keep the other theme's colors until recolored.
    if (Theme::instance().dark() != coloredDark_)
      recolor();
  };
  applyTheme();
  connect(&Theme::instance(), &Theme::changed, this, applyTheme);
  connect(command_, &QLineEdit::returnPressed, this, [this] {
    const auto line = command_->text().trimmed();
    command_->clear();
    if (line.isEmpty())
      return;
    history_.removeAll(line);
    history_.append(line);
    while (history_.size() > ::neverd::limits::gui::kOutputWindowMaximumHistory)
      history_.removeFirst();
    historyIndex_ = int(history_.size());
    QSettings().setValue(HistoryKey, history_);
    append(QStringLiteral("> ") + line, 0);
    execute(line);
  });
}

QMenu *OutputWindow::createLogMenu() {
  auto *menu = new QMenu(this);
  QAction *copy = menu->addAction(icon(QStringLiteral("copy")), tr("&Copy"));
  copy->setObjectName(QStringLiteral("outputCopy"));
  copy->setShortcut(QKeySequence::Copy);
  copy->setShortcutVisibleInContextMenu(true);
  copy->setEnabled(log_->textCursor().hasSelection());
  connect(copy, &QAction::triggered, log_, &QPlainTextEdit::copy);
  QAction *all =
      menu->addAction(icon(QStringLiteral("select_all")), tr("Select &all"));
  all->setObjectName(QStringLiteral("outputSelectAll"));
  all->setShortcut(QKeySequence::SelectAll);
  all->setShortcutVisibleInContextMenu(true);
  connect(all, &QAction::triggered, log_, &QPlainTextEdit::selectAll);
  menu->addSeparator();
  // The same as the command line's clear.
  QAction *clear =
      menu->addAction(icon(QStringLiteral("output_clear")), tr("C&lear"));
  clear->setObjectName(QStringLiteral("outputClear"));
  clear->setEnabled(!log_->document()->isEmpty());
  connect(clear, &QAction::triggered, log_, &QPlainTextEdit::clear);
  return menu;
}

void OutputWindow::append(const QString &text, int level) {
  QTextCharFormat format;
  format.setForeground(levelColor(level));
  auto *bar = log_->verticalScrollBar();
  const bool atBottom = bar->value() >= bar->maximum() - 4;
  QTextCursor cursor(log_->document());
  cursor.movePosition(QTextCursor::End);
  if (!log_->document()->isEmpty())
    cursor.insertBlock();
  // The line keeps its level, so that another theme can color it again.
  cursor.block().setUserState(level);
  cursor.insertText(text, format);
  if (atBottom)
    bar->setValue(bar->maximum());
}

void OutputWindow::recolor() {
  coloredDark_ = Theme::instance().dark();
  QTextCursor cursor(log_->document());
  cursor.beginEditBlock();
  for (auto block = log_->document()->begin(); block.isValid();
       block = block.next()) {
    QTextCharFormat format;
    format.setForeground(levelColor(std::max(0, block.userState())));
    cursor.setPosition(block.position());
    cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    cursor.mergeCharFormat(format);
  }
  cursor.endEditBlock();
}

void OutputWindow::run(const QString &line) {
  const QString trimmed = line.trimmed();
  if (trimmed.isEmpty())
    return;
  append(QStringLiteral("> ") + trimmed, 0);
  execute(trimmed);
}

void OutputWindow::focusCommandLine() {
  command_->setFocus(Qt::ShortcutFocusReason);
  command_->selectAll();
}

bool OutputWindow::eventFilter(QObject *object, QEvent *event) {
  if (object == command_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Up && !history_.isEmpty()) {
      historyIndex_ = std::max(0, historyIndex_ - 1);
      command_->setText(history_.value(historyIndex_));
      return true;
    }
    if (key->key() == Qt::Key_Down && !history_.isEmpty()) {
      historyIndex_ = std::min(int(history_.size()), historyIndex_ + 1);
      command_->setText(history_.value(historyIndex_));
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

void OutputWindow::evaluate(
    const QString &text,
    std::function<void(std::optional<Address>, QString)> done) {
  resolveExpression(session_, this, text,
                    location_ ? location_() : std::optional<Address>(),
                    [done](std::optional<Address> value, const QString &error) {
                      done(value, error);
                    });
}

void OutputWindow::execute(const QString &line) {
  if (line == QLatin1String("help") || line == QLatin1String("?")) {
    printHelp();
    return;
  }
  if (line == QLatin1String("clear") || line == QLatin1String("cls")) {
    log_->clear();
    return;
  }
  const qsizetype space = line.indexOf(QLatin1Char(' '));
  const QString word = space < 0 ? line : line.left(space);
  const QString argument =
      space < 0 ? QString() : line.mid(space + 1).trimmed();
  for (const auto &entry : Commands) {
    if (word.compare(QLatin1String(entry.word), Qt::CaseInsensitive))
      continue;
    const QString command = QString::fromLatin1(entry.command);
    if (command == QLatin1String("jump")) {
      evaluate(argument, [this](std::optional<Address> value, QString error) {
        if (value)
          emit navigateRequested(*value);
        else
          append(error, 2);
      });
    } else {
      emit commandRequested(command, argument);
    }
    return;
  }
  // Anything else is an expression, printed like a calculator.
  evaluate(line, [this](std::optional<Address> value, QString error) {
    if (!value) {
      append(error, 2);
      return;
    }
    const Address v = *value;
    QString chars;
    for (int shift = 0; shift < 64; shift += 8) {
      const auto byte = static_cast<char>((v >> shift) & 0xff);
      if (!byte)
        break;
      chars.append(QChar::fromLatin1(byte).isPrint() ? QChar::fromLatin1(byte)
                                                     : QLatin1Char('.'));
    }
    append(QStringLiteral("%1h  %2  %3o  '%4'")
               .arg(displayAddress(v), QString::number(qint64(v)),
                    QString::number(v, 8), chars),
           0);
  });
}

void OutputWindow::printHelp() {
  append(tr("Commands:"), 0);
  append(tr("  g <expr>          jump to an address or name"), 0);
  append(tr("  x [expr]          list references to the current item or expr"),
         0);
  append(tr("  n <name>          rename the current function"), 0);
  append(tr("  c <text>          comment the current address"), 0);
  append(tr("  d [expr]          decompile the current or given function"), 0);
  append(tr("  f <hex|\"text\">    search the binary"), 0);
  append(tr("  strref [text]     list the instructions that refer to strings"),
         0);
  append(tr("  t [name]          show the local types, or the type named"), 0);
  append(tr("  graph, hex        show the graph or hex view"), 0);
  append(tr("  analyze, save     whole-program analysis, save comments"), 0);
  append(tr("  <expr>            evaluate: 0x10, 10h, #16, names, + - * / % & "
            "| ^ << >> ~"),
         0);
}

} // namespace neverd::gui
