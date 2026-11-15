//===- FunctionEditDialog.cpp - Edit a function ---------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// The layout follows IDA 9's Edit function dialog: the name, bounds and
/// color on the left with the frame sizes below them, the function's flags
/// on the right, and Cancel and OK under both.
//===----------------------------------------------------------------------===//

#include "FunctionEditDialog.h"

#include "../Config.h"

#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>
#include <utility>

namespace neverd::gui {
namespace {
/// Room for a 64-bit address with its segment, as IDA's fields have.
constexpr int FieldCharacters = 28;
/// The keys function_edit and function_attributes spell the attributes with.
#define NEVERD_FUNCTION_FLAG(Member, Spelling)                                 \
  constexpr char Member##Key[] = Spelling;
#define NEVERD_FUNCTION_COLOR(Spelling) constexpr char ColorKey[] = Spelling;
#include "neverd/sdk/FunctionAttributes.def"

/// A field that shows what analysis found, which the user cannot change.
QLineEdit *shownField(QWidget *parent, const QString &text,
                      const QString &why = {}) {
  auto *field = new QLineEdit(text, parent);
  field->setReadOnly(true);
  field->setFocusPolicy(Qt::ClickFocus);
  if (!why.isEmpty()) {
    field->setEnabled(false);
    field->setToolTip(why);
  }
  return field;
}

/// A flag the user can state: checked when the user or analysis says so.
QCheckBox *flag(QWidget *parent, const QString &text, const QJsonValue &value) {
  auto *box = new QCheckBox(text, parent);
  box->setChecked(value.toBool(false));
  return box;
}

/// A flag of IDA's that NeverD does not model, shown off and disabled.
QCheckBox *unmodeled(QWidget *parent, const QString &text, const QString &why) {
  auto *box = new QCheckBox(text, parent);
  box->setEnabled(false);
  box->setToolTip(why);
  return box;
}
} // namespace

FunctionEditDialog::FunctionEditDialog(const QJsonObject &attributes,
                                       const QString &start, const QString &end,
                                       QWidget *parent)
    : QDialog(parent), attributes_(attributes) {
  setObjectName(QStringLiteral("functionEdit"));
  setWindowTitle(tr("Edit function"));
  const QString frameWhy = tr("NeverD does not keep this frame size");

  auto *fields = new QFormLayout;
  fields->setVerticalSpacing(
      ::neverd::limits::gui_config::kFunctionEditDialogRowSpacing);
  fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
  name_ = new QLineEdit(attributes.value("name").toString(), this);
  name_->setObjectName(QStringLiteral("name"));
  name_->setMinimumWidth(fontMetrics().averageCharWidth() * FieldCharacters);
  fields->addRow(tr("&Name of function"), name_);
  // A function's bounds are what analysis decodes; NeverD takes no end from
  // the user yet.
  const QString boundsWhy = tr("Analysis decides where the function ends");
  auto *startField = shownField(this, start, boundsWhy);
  startField->setObjectName(QStringLiteral("start"));
  fields->addRow(tr("&Start address"), startField);
  auto *endField = shownField(this, end, boundsWhy);
  endField->setObjectName(QStringLiteral("end"));
  fields->addRow(tr("&End address"), endField);
  color_ = new QPushButton(this);
  color_->setObjectName(QStringLiteral("color"));
  if (const auto stated = attributes.value(ColorKey).toString();
      !stated.isEmpty())
    chosen_ = QColor::fromString(stated);
  auto *colors = new QMenu(color_);
  colors->addAction(tr("&Choose…"), this, [this] {
    const QColor picked = QColorDialog::getColor(
        chosen_.value_or(Qt::white), this, tr("Function color"),
        QColorDialog::DontUseNativeDialog);
    if (!picked.isValid())
      return;
    chosen_ = picked;
    colorChanged_ = true;
    showColor();
  });
  colors->addAction(tr("&Default"), this, [this] {
    colorChanged_ = colorChanged_ || chosen_.has_value();
    chosen_.reset();
    showColor();
  });
  color_->setMenu(colors);
  showColor();
  fields->addRow(tr("&Color"), color_);

  // IDA's frame sizes: NeverD knows the arguments x86's `ret imm` pops.
  auto *sizes = new QFormLayout;
  sizes->setVerticalSpacing(
      ::neverd::limits::gui_config::kFunctionEditDialogRowSpacing);
  sizes->addRow(new QLabel(tr("Enter size of (in bytes)"), this));
  sizes->addRow(tr("Local &variables area"), shownField(this, {}, frameWhy));
  sizes->addRow(tr("Saved &registers"), shownField(this, {}, frameWhy));
  const QJsonValue purged = attributes.value("purged_bytes");
  auto *purgedField =
      purged.isDouble()
          ? shownField(this,
                       QStringLiteral("0x") +
                           QString::number(purged.toInteger(), 16).toUpper())
          : shownField(this, {}, tr("Only x86 functions pop their arguments"));
  purgedField->setObjectName(QStringLiteral("purged"));
  sizes->addRow(tr("&Purged bytes"), purgedField);
  sizes->addRow(tr("Frame pointer &delta"), shownField(this, {}, frameWhy));

  // Two forms, each with a label column of its own width, started the frame
  // sizes' fields to the right of the name's: both take the wider column.
  int labelWidth = 0;
  QList<QWidget *> labels;
  for (const QFormLayout *form : {fields, sizes})
    for (int row = 0; row < form->rowCount(); ++row)
      if (const QLayoutItem *item = form->itemAt(row, QFormLayout::LabelRole);
          item && item->widget()) {
        labels.append(item->widget());
        labelWidth = qMax(labelWidth, item->widget()->sizeHint().width());
      }
  for (QWidget *label : std::as_const(labels))
    label->setMinimumWidth(labelWidth);

  auto *left = new QVBoxLayout;
  left->setSpacing(::neverd::limits::gui_config::kFunctionEditDialogRowSpacing *
                   2);
  left->addLayout(fields);
  left->addLayout(sizes);
  left->addStretch();

  // What the user states, and the flags IDA keeps of a frame NeverD
  // recovers instead.
  auto *flags = new QGroupBox(this);
  // No title, so no room for one above the frame (theme.qss).
  flags->setProperty("untitled", true);
  auto *flagRows = new QVBoxLayout(flags);
  flagRows->setSpacing(
      ::neverd::limits::gui_config::kFunctionEditDialogRowSpacing);
  const QString flagWhy = tr("NeverD does not keep this flag");
  noReturn_ =
      flag(flags, tr("D&oes not return"), attributes.value(NoReturnKey));
  noReturn_->setObjectName(QLatin1String(NoReturnKey));
  noReturn_->setToolTip(
      tr("Analysis stops control flow at every call to the function"));
  flagRows->addWidget(noReturn_);
  flagRows->addWidget(unmodeled(flags, tr("&Far function"),
                                tr("Only 16-bit x86 code has far functions")));
  library_ = flag(flags, tr("&Library func"), attributes.value(LibraryKey));
  library_->setObjectName(QLatin1String(LibraryKey));
  flagRows->addWidget(library_);
  static_ = flag(flags, tr("Static func"), attributes.value(StaticKey));
  static_->setObjectName(QLatin1String(StaticKey));
  flagRows->addWidget(static_);
  for (const QString &text : {tr("&BP based frame"), tr("BP e&quals to SP"),
                              tr("Fuzzy SP"), tr("Outlined code")})
    flagRows->addWidget(unmodeled(flags, text, flagWhy));
  flagRows->addStretch();

  auto *columns = new QHBoxLayout;
  columns->setSpacing(
      ::neverd::limits::gui_config::kFunctionEditDialogColumnSpacing);
  columns->addLayout(left, 1);
  columns->addWidget(flags);

  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
  // As IDA's, Cancel has no access key: Alt+C chooses the color.
  buttons->button(QDialogButtonBox::Ok)->setText(tr("O&K"));
  buttons->button(QDialogButtonBox::Ok)->setObjectName(QStringLiteral("ok"));
  buttons->button(QDialogButtonBox::Cancel)->setText(tr("Cancel"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(
      ::neverd::limits::gui_config::kFunctionEditDialogMargin,
      ::neverd::limits::gui_config::kFunctionEditDialogMargin,
      ::neverd::limits::gui_config::kFunctionEditDialogMargin,
      ::neverd::limits::gui_config::kFunctionEditDialogMargin);
  layout->setSpacing(::neverd::limits::gui_config::kFunctionEditDialogMargin);
  layout->addLayout(columns);
  layout->addWidget(buttons);
  name_->setFocus();
  name_->selectAll();
}

QString FunctionEditDialog::name() const { return name_->text().trimmed(); }

QJsonObject FunctionEditDialog::changedAttributes() const {
  QJsonObject changed;
  const auto note = [&](const char *key, const QCheckBox *box) {
    if (box->isChecked() != attributes_.value(key).toBool(false))
      changed.insert(QLatin1String(key), box->isChecked());
  };
  note(NoReturnKey, noReturn_);
  note(LibraryKey, library_);
  note(StaticKey, static_);
  if (colorChanged_)
    changed.insert(QLatin1String(ColorKey),
                   chosen_ ? QJsonValue(chosen_->name().toUpper())
                           : QJsonValue(QJsonValue::Null));
  return changed;
}

void FunctionEditDialog::showColor() {
  if (!chosen_) {
    color_->setText(tr("DEFAULT"));
    color_->setIcon({});
    return;
  }
  QPixmap swatch(12, 12);
  swatch.fill(*chosen_);
  color_->setIcon(QIcon(swatch));
  color_->setText(chosen_->name().toUpper());
}

} // namespace neverd::gui
