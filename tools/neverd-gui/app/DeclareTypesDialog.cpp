#include "DeclareTypesDialog.h"

#include "ShortcutHint.h"
#include "Theme.h"
#include "TypeSourceHighlighter.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QShortcut>
#include <QTextBlock>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {

/// The languages declarations are read in, by the engine's names.  C/C++
/// reads as C, the language the Local Types window shows it in, so that a
/// type declared lays out as the source it is shown as; C++'s references
/// and bases are refused clearly.
struct DeclarationLanguage {
  const char *id;
  const char *title;
};
constexpr DeclarationLanguage Languages[] = {
    {"c", "C/C++"}, {"rust", "Rust"}, {"go", "Go"}};

constexpr int LineRole = Qt::UserRole;
constexpr int ColumnRole = Qt::UserRole + 1;

} // namespace

DeclareTypesDialog::DeclareTypesDialog(const QString &language,
                                       const QString &source, Submit submit,
                                       QWidget *parent)
    : QDialog(parent), submit_(std::move(submit)) {
  setWindowTitle(tr("Declare types"));
  setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(this);

  auto *top = new QHBoxLayout;
  auto *hint = new QLabel(
      tr("Declare structures, unions, enumerations and typedefs. A type "
         "named as one of the program's replaces it."),
      this);
  hint->setWordWrap(true);
  language_ = new QComboBox(this);
  language_->setObjectName(QStringLiteral("declareLanguage"));
  for (const auto &entry : Languages)
    language_->addItem(QString::fromLatin1(entry.title),
                       QString::fromLatin1(entry.id));
  // C++ opens as C/C++ too.
  language_->setCurrentIndex(std::max(
      language_->findData(language == QLatin1String("cpp") ? QStringLiteral("c")
                                                           : language),
      0));
  top->addWidget(hint, 1);
  top->addWidget(language_);
  layout->addLayout(top);

  editor_ = new QPlainTextEdit(this);
  editor_->setObjectName(QStringLiteral("declareEditor"));
  editor_->setFont(Theme::instance().codeFont());
  editor_->setLineWrapMode(QPlainTextEdit::NoWrap);
  editor_->setTabStopDistance(
      4 * editor_->fontMetrics().horizontalAdvance(QLatin1Char(' ')));
  editor_->setPlainText(source);
  editor_->setMinimumSize(560, 300);
  highlighter_ = new TypeSourceHighlighter(editor_->document());
  highlighter_->setLanguage(this->language());
  layout->addWidget(editor_, 1);

  problems_ = new QListWidget(this);
  problems_->setObjectName(QStringLiteral("declareProblems"));
  problems_->setMaximumHeight(120);
  problems_->hide();
  layout->addWidget(problems_);

  auto *buttons = new QDialogButtonBox(this);
  declare_ = buttons->addButton(tr("Declare"), QDialogButtonBox::AcceptRole);
  declare_->setObjectName(QStringLiteral("declare"));
  const QKeySequence declareKey(Qt::CTRL | Qt::Key_Return);
  declare_->setToolTip(tr("Read the declarations (%1)")
                           .arg(declareKey.toString(QKeySequence::NativeText)));
  setShortcutHint(*declare_, declareKey);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);

  // Return writes a line break in the editor; Ctrl+Return declares.
  for (const auto &key : {declareKey, QKeySequence(Qt::CTRL | Qt::Key_Enter)}) {
    auto *shortcut = new QShortcut(key, this);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this,
            &DeclareTypesDialog::declare);
  }
  connect(language_, &QComboBox::currentIndexChanged, this,
          [this] { highlighter_->setLanguage(this->language()); });
  connect(buttons, &QDialogButtonBox::accepted, this,
          &DeclareTypesDialog::declare);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(problems_, &QListWidget::itemActivated, this,
          [this](QListWidgetItem *item) {
            const int line = item->data(LineRole).toInt();
            const int column = item->data(ColumnRole).toInt();
            const QTextBlock block =
                editor_->document()->findBlockByNumber(std::max(line - 1, 0));
            QTextCursor cursor(block);
            cursor.movePosition(QTextCursor::Right, QTextCursor::MoveAnchor,
                                std::max(column - 1, 0));
            editor_->setTextCursor(cursor);
            editor_->setFocus();
          });
  editor_->setFocus();
}

QString DeclareTypesDialog::language() const {
  return language_->currentData().toString();
}

QString DeclareTypesDialog::source() const { return editor_->toPlainText(); }

void DeclareTypesDialog::setTypeNames(const QSet<QString> &names) {
  highlighter_->setTypeNames(names);
}

void DeclareTypesDialog::reject() {
  if (!waiting_)
    QDialog::reject();
}

void DeclareTypesDialog::declare() {
  if (waiting_ || !submit_)
    return;
  waiting_ = true;
  declare_->setEnabled(false);
  QPointer<DeclareTypesDialog> self(this);
  submit_(language(), source(), [this, self](const QJsonObject &reply) {
    if (!self)
      return;
    waiting_ = false;
    declare_->setEnabled(true);
    const QJsonArray problems = reply.value("problems").toArray();
    if (!problems.isEmpty() || reply.contains("error")) {
      showProblems(reply.contains("error")
                       ? QJsonArray{QJsonObject{
                             {"line", 0},
                             {"column", 0},
                             {"message", reply.value("error").toString()}}}
                       : problems);
      return;
    }
    declared_.clear();
    for (const QJsonValue &name : reply.value("types").toArray())
      declared_.append(name.toString());
    accept();
  });
}

void DeclareTypesDialog::showProblems(const QJsonArray &problems) {
  problems_->clear();
  for (const QJsonValue &value : problems) {
    const QJsonObject problem = value.toObject();
    const int line = problem.value("line").toInt();
    const int column = problem.value("column").toInt();
    auto *item =
        new QListWidgetItem(line ? tr("Line %1, column %2: %3")
                                       .arg(line)
                                       .arg(column)
                                       .arg(problem.value("message").toString())
                                 : problem.value("message").toString(),
                            problems_);
    item->setData(LineRole, line);
    item->setData(ColumnRole, column);
    item->setForeground(Theme::instance().color(ColorRole::ListingError));
  }
  problems_->setVisible(problems_->count() > 0);
  if (problems_->count())
    emit problems_->itemActivated(problems_->item(0));
}

} // namespace neverd::gui
