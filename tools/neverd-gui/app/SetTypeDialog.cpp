#include "SetTypeDialog.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "Icons.h"
#include "Session.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardItemModel>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {

constexpr char GeometrySetting[] = "setType/geometry";

/// Where a problem the engine words as "line:column: message" is.
std::optional<std::pair<int, QString>> problemAt(const QString &message) {
  static const QRegularExpression at(
      QStringLiteral(R"((\d+):(\d+): (.*)$)"),
      QRegularExpression::DotMatchesEverythingOption);
  const auto match = at.match(message);
  if (!match.hasMatch())
    return std::nullopt;
  return std::pair{match.captured(2).toInt(), match.captured(3)};
}

bool identifierCharacter(QChar character) {
  return character.isLetterOrNumber() || character == QLatin1Char('_');
}

} // namespace

SetTypeDialog::SetTypeDialog(Session &session, Address function,
                             std::optional<int> parameter, const QString &name,
                             QWidget *parent, const QJsonObject &variable)
    : QDialog(parent), session_(session), function_(function),
      parameter_(parameter), name_(name), variable_(variable) {
  setWindowTitle(tr("Set the type of %1").arg(name_));
  setWindowIcon(icon(QStringLiteral("set_type")));
  setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(this);
  const QFont code = Theme::instance().codeFont();

  subject_ = new QLabel(tr("Reading the declaration…"), this);
  subject_->setObjectName(QStringLiteral("setTypeSubject"));
  subject_->setWordWrap(true);
  layout->addWidget(subject_);

  field_ = new QLineEdit(this);
  field_->setObjectName(QStringLiteral("setTypeField"));
  field_->setFont(code);
  field_->setEnabled(false);
  field_->setPlaceholderText(
      parameter_ || !variable_.isEmpty()
          ? QStringLiteral("struct _DEVICE *")
          : QStringLiteral("int f(struct _DEVICE *Device)"));
  field_->installEventFilter(this);
  layout->addWidget(field_);

  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("setTypeStatus"));
  status_->setWordWrap(true);
  status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  layout->addWidget(status_);

  parameters_ = new QTreeWidget(this);
  parameters_->setObjectName(QStringLiteral("setTypeParameters"));
  parameters_->setRootIsDecorated(false);
  parameters_->setUniformRowHeights(true);
  parameters_->setSelectionMode(QAbstractItemView::NoSelection);
  parameters_->setFocusPolicy(Qt::NoFocus);
  parameters_->setFont(code);
  parameters_->setHeaderLabels(
      {QStringLiteral("#"), tr("Name"), tr("Type"), tr("Arrives in")});
  parameters_->header()->setStretchLastSection(true);
  layout->addWidget(parameters_, 1);

  auto *buttons = new QDialogButtonBox(this);
  reset_button_ = buttons->addButton(tr("Use the &analysis' type"),
                                     QDialogButtonBox::ResetRole);
  reset_button_->setObjectName(QStringLiteral("setTypeReset"));
  reset_button_->setToolTip(tr("Hand the function's declaration back to its "
                               "debug information and analysis"));
  if (!variable_.isEmpty())
    reset_button_->setToolTip(
        tr("Remove this source type assertion and use the analysis type"));
  reset_button_->hide();
  apply_ = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
  apply_->setObjectName(QStringLiteral("setTypeApply"));
  apply_->setDefault(true);
  apply_->setEnabled(false);
  buttons->addButton(QDialogButtonBox::Cancel);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &SetTypeDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(reset_button_, &QPushButton::clicked, this, [this] {
    reset_ = true;
    QDialog::accept();
  });

  // The local types' names, completed as the user types one.
  completions_ = new QStandardItemModel(this);
  completer_ = new QCompleter(this);
  completer_->setWidget(field_);
  completer_->setModel(completions_);
  completer_->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
  completer_->setMaxVisibleItems(12);
  completer_->popup()->setFont(code);
  completer_->popup()->setObjectName(QStringLiteral("setTypeCompletions"));
  connect(completer_, qOverload<const QString &>(&QCompleter::activated), this,
          &SetTypeDialog::insertCompletion);

  checkTimer_.setSingleShot(true);
  checkTimer_.setInterval(::neverd::limits::gui::kSetTypeDialogCheckDebounceMs);
  connect(&checkTimer_, &QTimer::timeout, this, &SetTypeDialog::check);
  completeTimer_.setSingleShot(true);
  completeTimer_.setInterval(
      ::neverd::limits::gui::kSetTypeDialogCompleteDebounceMs);
  connect(&completeTimer_, &QTimer::timeout, this, &SetTypeDialog::complete);
  connect(field_, &QLineEdit::textEdited, this, [this] {
    apply_->setEnabled(false);
    checkTimer_.start();
    completeTimer_.start();
  });

  resize(760, 400);
  restoreGeometry(QSettings().value(GeometrySetting).toByteArray());
  session_.read(
      variable_.isEmpty() ? QStringLiteral("function_type")
                          : QStringLiteral("source_type"),
      variable_.isEmpty() ? QJsonObject{{"address", hexAddress(function_)}}
                          : variable_,
      this, [this](const QJsonObject &reply) { loaded(reply); },
      [this](const QString &, const QString &message) {
        subject_->setText(message);
      });
}

void SetTypeDialog::loaded(const QJsonObject &reply) {
  if (!variable_.isEmpty()) {
    reset_button_->setVisible(reply.value(QStringLiteral("stated")).toBool());
    parameters_->hide();
    subject_->setText(tr("%1 is a %2-byte source variable. Enter a scalar or "
                         "pointer type from the local types.")
                          .arg(name_)
                          .arg(reply.value(QStringLiteral("bytes")).toInt()));
    field_->setEnabled(true);
    field_->setText(reply.value(QStringLiteral("type")).toString());
    field_->selectAll();
    field_->setFocus();
    check();
    return;
  }
  const QString function = reply.value(QStringLiteral("name")).toString();
  const QJsonArray parameters =
      reply.value(QStringLiteral("parameters")).toArray();
  reset_button_->setVisible(reply.value(QStringLiteral("stated")).toBool());
  QString text = reply.value(QStringLiteral("declaration")).toString();
  if (parameter_) {
    if (!reply.contains(QStringLiteral("parameters")) ||
        *parameter_ >= parameters.size()) {
      subject_->setText(
          tr("The function's declaration does not read, so its parameters "
             "cannot be typed one by one. Set the function's type instead."));
      return;
    }
    const QJsonObject own = parameters.at(*parameter_).toObject();
    const QString where = own.value(QStringLiteral("location")).toString();
    subject_->setText(where.isEmpty()
                          ? tr("%1, parameter %2 of %3.")
                                .arg(name_)
                                .arg(*parameter_ + 1)
                                .arg(function)
                          : tr("%1, parameter %2 of %3, arrives in %4.")
                                .arg(name_)
                                .arg(*parameter_ + 1)
                                .arg(function, where));
    text = own.value(QStringLiteral("type")).toString();
  } else {
    if (!function.isEmpty())
      setWindowTitle(tr("Set the type of %1").arg(function));
    subject_->setText(
        reply.value(QStringLiteral("stated")).toBool()
            ? tr("The declaration you gave %1, read against the local types "
                 "as you type.")
                  .arg(function.isEmpty() ? name_ : function)
            : tr("The declaration of %1 as the decompiler prints it, read "
                 "against the local types as you type.")
                  .arg(function.isEmpty() ? name_ : function));
  }
  field_->setEnabled(true);
  field_->setText(text);
  field_->selectAll();
  field_->setFocus();
  check();
}

void SetTypeDialog::check() {
  checkTimer_.stop();
  const QString text = field_->text().trimmed();
  const quint64 serial = ++checkSerial_;
  if (text.isEmpty()) {
    reads_ = false;
    checkedText_ = text;
    status_->clear();
    apply_->setEnabled(false);
    return;
  }
  QJsonObject request{{"address", hexAddress(function_)}};
  if (!variable_.isEmpty()) {
    request = variable_;
    request.insert(QStringLiteral("type"), text);
  } else if (parameter_) {
    request.insert(QStringLiteral("parameter"), *parameter_);
    request.insert(QStringLiteral("type"), text);
  } else {
    request.insert(QStringLiteral("declaration"), text);
  }
  session_.read(
      variable_.isEmpty() ? QStringLiteral("function_type")
                          : QStringLiteral("source_type"),
      request, this,
      [this, serial, text](const QJsonObject &reply) {
        checked(serial, text, reply);
      },
      [this, serial, text](const QString &, const QString &message) {
        refused(serial, text, message);
      });
}

void SetTypeDialog::checked(quint64 serial, const QString &text,
                            const QJsonObject &reply) {
  if (serial != checkSerial_)
    return;
  checkedText_ = text;
  const QJsonArray problems = reply.value(QStringLiteral("problems")).toArray();
  if (!problems.isEmpty()) {
    const QJsonObject first = problems.at(0).toObject();
    reads_ = false;
    showProblem(first.value(QStringLiteral("message")).toString(),
                first.value(QStringLiteral("column")).toInt());
    acceptOnAnswer_ = false;
    return;
  }
  reads_ = true;
  proposal_ = reply
                  .value(variable_.isEmpty() ? QStringLiteral("declaration")
                                             : QStringLiteral("type"))
                  .toString();
  // What the user would state, the replica renders now, so that the view after
  // the user states it reads that render.  A declaration that hides the
  // prototype its function's name binds moves what every render reads, and is
  // not rendered ahead.
  if (variable_.isEmpty() &&
      !reply.value(QStringLiteral("hides_prototype")).toBool(true))
    session_.prepareFunctionRead(function_,
                                 parameter_ ? proposal_ : checkedText_);
  const QJsonArray parameters =
      reply.value(QStringLiteral("parameters")).toArray();
  showParameters(parameters);
  const QColor good = Theme::instance().color(ColorRole::FitExact);
  status_->setStyleSheet(QStringLiteral("color: %1").arg(good.name()));
  status_->setText(parameter_ || !variable_.isEmpty()
                       ? QStringLiteral("✓ ") + proposal_
                       : QStringLiteral("✓ ") + tr("Reads: %n parameters", "",
                                                   int(parameters.size())));
  apply_->setEnabled(field_->text().trimmed() == checkedText_);
  if (std::exchange(acceptOnAnswer_, false) && apply_->isEnabled())
    accept();
}

void SetTypeDialog::refused(quint64 serial, const QString &text,
                            const QString &message) {
  if (serial != checkSerial_)
    return;
  checkedText_ = text;
  reads_ = false;
  acceptOnAnswer_ = false;
  // The engine words a type that does not read as "…: line:column: why".
  if (const auto at = problemAt(message))
    showProblem(at->second, at->first);
  else
    showProblem(message, 0);
}

void SetTypeDialog::showProblem(const QString &message, int column) {
  const QColor bad = Theme::instance().color(ColorRole::FitOutside);
  status_->setStyleSheet(QStringLiteral("color: %1").arg(bad.name()));
  status_->setText(column > 0
                       ? QStringLiteral("✗ ") +
                             tr("At column %1: %2").arg(column).arg(message)
                       : QStringLiteral("✗ ") + message);
  status_->setProperty("column", column);
  apply_->setEnabled(false);
}

void SetTypeDialog::showParameters(const QJsonArray &parameters) {
  parameters_->clear();
  for (int index = 0; index < parameters.size(); ++index) {
    const QJsonObject parameter = parameters.at(index).toObject();
    auto *row = new QTreeWidgetItem(parameters_);
    row->setText(0, QString::number(index + 1));
    row->setText(1, parameter.value(QStringLiteral("name")).toString());
    row->setText(2, parameter.value(QStringLiteral("type")).toString());
    const QString where =
        parameter.value(QStringLiteral("location")).toString();
    row->setText(3, where.isEmpty() ? QStringLiteral("—") : where);
    // The parameter the dialog types stands out.
    if (parameter_ && index == *parameter_)
      for (int column = 0; column < parameters_->columnCount(); ++column) {
        QFont bold = row->font(column);
        bold.setBold(true);
        row->setFont(column, bold);
      }
  }
  for (int column = 0; column < parameters_->columnCount() - 1; ++column)
    parameters_->resizeColumnToContents(column);
}

std::pair<int, QString> SetTypeDialog::wordAtCursor() const {
  const QString text = field_->text();
  const int end = field_->cursorPosition();
  int begin = end;
  while (begin > 0 && identifierCharacter(text.at(begin - 1)))
    --begin;
  // A number is no type's name.
  if (begin < end && text.at(begin).isDigit())
    return {end, QString()};
  return {begin, text.mid(begin, end - begin)};
}

void SetTypeDialog::complete() {
  completeTimer_.stop();
  const auto [begin, word] = wordAtCursor();
  const quint64 serial = ++completeSerial_;
  if (word.isEmpty()) {
    completer_->popup()->hide();
    return;
  }
  session_.read(
      QStringLiteral("local_types_fit"),
      {{"text", word},
       {"records", false},
       {"limit", ::neverd::limits::gui::kSetTypeDialogCompletionLimit}},
      this,
      [this, serial](const QJsonObject &reply) { completed(serial, reply); },
      [this](const QString &, const QString &) {
        completer_->popup()->hide();
      });
}

void SetTypeDialog::completed(quint64 serial, const QJsonObject &reply) {
  if (serial != completeSerial_ || !field_->hasFocus())
    return;
  const auto [begin, word] = wordAtCursor();
  completions_->clear();
  for (const auto &value :
       reply.value(QStringLiteral("candidates")).toArray()) {
    const QJsonObject candidate = value.toObject();
    auto *item =
        new QStandardItem(candidate.value(QStringLiteral("name")).toString());
    item->setToolTip(
        tr("%1, %n bytes", "",
           int(candidate.value(QStringLiteral("size")).toInteger()))
            .arg(candidate.value(QStringLiteral("kind")).toString()));
    completions_->appendRow(item);
  }
  // A name typed out whole needs no list.
  if (!completions_->rowCount() || (completions_->rowCount() == 1 &&
                                    completions_->item(0)->text() == word)) {
    completer_->popup()->hide();
    return;
  }
  // The list opens under the word it completes.
  const int width = std::max(
      ::neverd::limits::gui_config::kSetTypeDialogCompletedWidthMaxArgument1,
      completer_->popup()->sizeHintForColumn(0) + 32);
  const int x = std::clamp(QFontMetrics(field_->font())
                               .horizontalAdvance(field_->text().left(begin)),
                           0, std::max(0, field_->width() - width));
  completer_->complete(QRect(x, 0, width, field_->height()));
}

void SetTypeDialog::insertCompletion(const QString &name) {
  const auto [begin, word] = wordAtCursor();
  QString text = field_->text();
  text.replace(begin, word.size(), name);
  field_->setText(text);
  field_->setCursorPosition(begin + int(name.size()));
  apply_->setEnabled(false);
  checkTimer_.start();
}

bool SetTypeDialog::eventFilter(QObject *object, QEvent *event) {
  if (object == field_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    // Ctrl+Space lists the names that complete the one typed.
    if (key->key() == Qt::Key_Space &&
        key->modifiers().testFlag(Qt::ControlModifier)) {
      complete();
      return true;
    }
    if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        !completer_->popup()->isVisible()) {
      // Text not yet read is read first, and accepted when it reads; a
      // problem moves the cursor where the text stops reading.
      if (checkTimer_.isActive() || field_->text().trimmed() != checkedText_) {
        acceptOnAnswer_ = true;
        check();
      } else if (reads_) {
        accept();
      } else if (const int column = status_->property("column").toInt();
                 column > 0) {
        field_->setCursorPosition(column - 1);
      }
      return true;
    }
  }
  return QDialog::eventFilter(object, event);
}

void SetTypeDialog::accept() {
  if (!reads_ || field_->text().trimmed() != checkedText_)
    return;
  declaration_ = parameter_ ? proposal_ : checkedText_;
  QDialog::accept();
}

void SetTypeDialog::done(int result) {
  QSettings().setValue(GeometrySetting, saveGeometry());
  QDialog::done(result);
}

} // namespace neverd::gui
