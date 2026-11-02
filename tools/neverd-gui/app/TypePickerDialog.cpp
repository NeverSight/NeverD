#include "TypePickerDialog.h"

#include "../ResourceLimits.h"
#include "Address.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSplitter>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <map>

namespace neverd::gui {
namespace {

constexpr int CandidateRole = Qt::UserRole + 1;
constexpr char EverySetting[] = "convert/everyAccess";
constexpr char GeometrySetting[] = "convert/geometry";
constexpr char SplitterSetting[] = "convert/splitter";

/// A number as the listing writes it: decimal below ten, else hexadecimal
/// with an h, a leading zero before a letter.
QString listingNumber(quint64 value) {
  if (value < 10)
    return QString::number(value);
  QString digits = QString::number(value, 16).toUpper();
  if (digits.front().isLetter())
    digits.prepend(QLatin1Char('0'));
  return digits + QLatin1Char('h');
}

/// A number as an ARM-family listing writes it: decimal below ten, else
/// lowercase hexadecimal after 0x.
QString armListingNumber(quint64 value) {
  if (value < 10)
    return QString::number(value);
  return QStringLiteral("0x") + QString::number(value, 16);
}

/// A number as SBF and EVM listings write a structure offset's rest: in
/// decimal.
QString bytecodeListingNumber(quint64 value) { return QString::number(value); }

/// An offset as a member list writes it: +0x220, -0x10.
QString offsetText(qint64 offset) {
  return (offset < 0 ? QStringLiteral("-0x") : QStringLiteral("+0x")) +
         QString::number(offset < 0 ? -offset : offset, 16).toUpper();
}

/// The color that says how well a landing fits.
QColor fitColor(const QString &fit) {
  const Theme &theme = Theme::instance();
  if (fit == QLatin1String("exact"))
    return theme.color(ColorRole::FitExact);
  if (fit == QLatin1String("outside"))
    return theme.color(ColorRole::FitOutside);
  return theme.color(ColorRole::FitPartial);
}

/// The mark a landing shows: exact, in part, or not at all.
QString fitMark(const QString &fit) {
  if (fit == QLatin1String("exact"))
    return QStringLiteral("✓");
  if (fit == QLatin1String("outside") || fit == QLatin1String("padding"))
    return QStringLiteral("✗");
  return QStringLiteral("≈");
}

/// The runs a match lit, as UTF-16 positions of \p text.
QList<QPair<int, int>> litRuns(const QString &text, const QJsonValue &runs) {
  const QByteArray bytes = text.toUtf8();
  QList<QPair<int, int>> result;
  for (const auto &value : runs.toArray()) {
    const auto run = value.toArray();
    const int begin = run.at(0).toInt(), length = run.at(1).toInt();
    if (begin < 0 || length <= 0 || begin + length > bytes.size())
      continue;
    const int from = QString::fromUtf8(bytes.left(begin)).size();
    result.append({from, QString::fromUtf8(bytes.mid(begin, length)).size()});
  }
  return result;
}

/// Draws \p text from \p x in \p rect, the runs a match lit bold in the
/// match color; returns where it ends.
int drawLit(QPainter *painter, const QRect &rect, int x, const QString &text,
            const QList<QPair<int, int>> &runs, const QFont &font,
            const QColor &color, const QColor &lit) {
  QFont bold = font;
  bold.setBold(true);
  int at = 0;
  const auto piece = [&](int from, int to, bool matched) {
    if (to <= from || x >= rect.right())
      return;
    const QString part = text.mid(from, to - from);
    const QFont &face = matched ? bold : font;
    const QFontMetrics metrics(face);
    const QString shown =
        metrics.elidedText(part, Qt::ElideRight, rect.right() - x);
    painter->setFont(face);
    painter->setPen(matched ? lit : color);
    painter->drawText(QRect(x, rect.top(), rect.right() - x, rect.height()),
                      Qt::AlignLeft | Qt::AlignVCenter, shown);
    x += metrics.horizontalAdvance(shown);
  };
  for (const auto &[from, length] : runs) {
    piece(at, from, false);
    piece(from, from + length, true);
    at = from + length;
  }
  piece(at, text.size(), false);
  return x;
}

/// A candidate of the list in one row: its name, the match lit, its
/// typedefs and the member path a search matched, then on the right how
/// many accesses land exactly, where the pointer points into it, its size
/// and where it comes from.
class CandidateDelegate final : public QStyledItemDelegate {
public:
  explicit CandidateDelegate(int accesses, QObject *parent)
      : QStyledItemDelegate(parent), accesses_(accesses) {}
  void setAccesses(int accesses) { accesses_ = accesses; }

  QSize sizeHint(const QStyleOptionViewItem &option,
                 const QModelIndex &index) const override {
    QSize size = QStyledItemDelegate::sizeHint(option, index);
    size.setHeight(std::max(size.height(), option.fontMetrics.height() + 10));
    return size;
  }

  void paint(QPainter *painter, const QStyleOptionViewItem &option,
             const QModelIndex &index) const override {
    QStyleOptionViewItem item = option;
    initStyleOption(&item, index);
    const QJsonObject candidate = index.data(CandidateRole).toJsonObject();
    item.text.clear();
    item.icon = QIcon();
    const QWidget *widget = item.widget;
    const QStyle *style = widget ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &item, painter, widget);

    const Theme &theme = Theme::instance();
    const bool selected = item.state & QStyle::State_Selected;
    const QColor text = item.palette.color(selected ? QPalette::HighlightedText
                                                    : QPalette::Text);
    QColor dim =
        selected ? text : theme.chrome(QStringLiteral("SecondaryText"));
    if (selected)
      dim.setAlphaF(0.75);
    const QColor lit = selected ? text : theme.color(ColorRole::MatchHighlight);
    painter->save();
    QRect rect = item.rect.adjusted(6, 0, -10, 0);
    icon(QStringLiteral("local_types"))
        .paint(painter, QRect(rect.left(), rect.center().y() - 8, 16, 16));
    rect.setLeft(rect.left() + 24);

    // From the right: origin, size, the pointer's place, the fit.
    const QFont font = item.font;
    QFont small = font;
    small.setPointSizeF(std::max(7.0, QFontInfo(font).pointSizeF() - 1));
    const QFontMetrics metrics(font), smallMetrics(small);
    int right = rect.right();
    const auto rightText = [&](const QString &shown, const QFont &face,
                               const QColor &color, int gap) {
      if (shown.isEmpty())
        return;
      const int width = QFontMetrics(face).horizontalAdvance(shown);
      painter->setFont(face);
      painter->setPen(color);
      painter->drawText(QRect(right - width, rect.top(), width, rect.height()),
                        Qt::AlignRight | Qt::AlignVCenter, shown);
      right -= width + gap;
    };
    const QString origin = candidate.value(QStringLiteral("origin")).toString();
    rightText(origin == QLatin1String("header") ? TypePickerDialog::tr("header")
              : origin == QLatin1String("user") ? TypePickerDialog::tr("user")
                                                : origin.toUpper(),
              small, dim, 14);
    if (candidate.contains(QStringLiteral("size")))
      rightText(QStringLiteral("0x") +
                    QString::number(
                        candidate.value(QStringLiteral("size")).toInteger(), 16)
                        .toUpper(),
                small, dim, 14);
    if (accesses_ > 0 && candidate.contains(QStringLiteral("exact"))) {
      if (candidate.contains(QStringLiteral("shift")))
        rightText(QStringLiteral("→ ") +
                      candidate.value(QStringLiteral("shiftPath")).toString(),
                  small, dim, 12);
      const int exact =
          candidate.value(QStringLiteral("exact")).toInt() +
          candidate.value(QStringLiteral("exactElements")).toInt();
      const QString fit = candidate.value(QStringLiteral("outside")).toInt()
                              ? QStringLiteral("outside")
                          : exact < accesses_ ? QStringLiteral("partial")
                                              : QStringLiteral("exact");
      QFont bold = font;
      bold.setBold(true);
      rightText(QStringLiteral("%1/%2").arg(exact).arg(accesses_), bold,
                selected ? text : fitColor(fit), 14);
    }

    // The name, its typedefs and a member a search matched.
    QRect left(rect.left(), rect.top(), std::max(0, right - rect.left()),
               rect.height());
    const QString name = candidate.value(QStringLiteral("name")).toString();
    const QString matched =
        candidate.value(QStringLiteral("matchedName")).toString();
    const QJsonValue runs = candidate.value(QStringLiteral("matched"));
    int x = drawLit(painter, left, left.left(), name,
                    matched == name ? litRuns(name, runs)
                                    : QList<QPair<int, int>>(),
                    font, text, lit);
    if (candidate.contains(QStringLiteral("matchedMember"))) {
      const QString member =
          QLatin1Char('.') +
          candidate.value(QStringLiteral("matchedMember")).toString();
      QList<QPair<int, int>> memberRuns;
      for (const auto &[from, length] : litRuns(
               member.mid(1), candidate.value(QStringLiteral("memberMatched"))))
        memberRuns.append({from + 1, length});
      x = drawLit(painter, left, x, member, memberRuns, font,
                  selected ? text : theme.color(ColorRole::ListingStructOffset),
                  lit);
    }
    QStringList aliases;
    for (const char *key : {"aliases", "pointerAliases"})
      for (const auto &value : candidate.value(QLatin1String(key)).toArray())
        aliases.append(value.toString());
    if (!aliases.isEmpty()) {
      x += 12;
      const QString joined = aliases.join(QStringLiteral(" · "));
      // A typedef the search matched lights up among them.
      QList<QPair<int, int>> aliasRuns;
      if (matched != name && aliases.contains(matched)) {
        int at = 0;
        for (const QString &alias : aliases) {
          if (alias == matched)
            for (const auto &[from, length] : litRuns(alias, runs))
              aliasRuns.append({at + from, length});
          at += alias.size() + 3;
        }
      }
      drawLit(painter, left, x, joined, aliasRuns, small, dim, lit);
    }
    painter->restore();
  }

private:
  int accesses_ = 0;
};

} // namespace

TypePickerDialog::TypePickerDialog(Session &session, QJsonObject query,
                                   const QString &parameterName,
                                   QWidget *parent)
    : QDialog(parent), session_(session), query_(std::move(query)),
      printedName_(parameterName),
      parameter_(query_.contains(QStringLiteral("parameter"))) {
  setWindowTitle(parameter_ ? tr("Convert to struct *")
                            : tr("Structure offset"));
  setWindowIcon(icon(QStringLiteral("operand_struct")));
  setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(this);
  // A parameter points at a record's start: C declares no pointer into one.
  if (parameter_)
    query_.insert(QStringLiteral("shifts"), false);

  subject_ = new QLabel(
      parameter_
          ? tr("Reading what the code does through the parameter…")
          : tr("Reading what the code does through the operand's pointer…"),
      this);
  subject_->setObjectName(QStringLiteral("typePickerSubject"));
  subject_->setWordWrap(true);
  layout->addWidget(subject_);

  search_ = new QLineEdit(this);
  search_->setObjectName(QStringLiteral("typePickerSearch"));
  search_->setPlaceholderText(
      tr("Search types: a name, a typedef, Type.member or .member"));
  search_->setClearButtonEnabled(true);
  search_->installEventFilter(this);
  layout->addWidget(search_);

  auto *splitter = new QSplitter(Qt::Horizontal, this);
  splitter->setChildrenCollapsible(false);
  list_ = new QListWidget(splitter);
  list_->setObjectName(QStringLiteral("typePickerList"));
  list_->setUniformItemSizes(true);
  list_->setItemDelegate(new CandidateDelegate(0, list_));
  list_->setMinimumWidth(360);
  activateOnReturn(*list_);

  auto *preview = new QWidget(splitter);
  auto *previewLayout = new QVBoxLayout(preview);
  previewLayout->setContentsMargins(12, 0, 0, 0);
  member_ = new QLabel(preview);
  member_->setObjectName(QStringLiteral("typePickerMember"));
  QFont code = Theme::instance().codeFont();
  QFont large = code;
  large.setPointSizeF(QFontInfo(code).pointSizeF() + 2);
  large.setBold(true);
  member_->setFont(large);
  member_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  memberType_ = new QLabel(preview);
  memberType_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  expression_ = new QLabel(preview);
  expression_->setObjectName(QStringLiteral("typePickerExpression"));
  expression_->setFont(code);
  expression_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  note_ = new QLabel(preview);
  note_->setWordWrap(true);
  note_->hide();
  evidenceTitle_ = new QLabel(preview);
  evidence_ = new QTreeWidget(preview);
  evidence_->setObjectName(QStringLiteral("typePickerEvidence"));
  evidence_->setRootIsDecorated(false);
  evidence_->setUniformRowHeights(true);
  evidence_->setSelectionMode(QAbstractItemView::NoSelection);
  evidence_->setFocusPolicy(Qt::NoFocus);
  evidence_->setHeaderLabels({QString(), tr("Offset"), tr("Bytes"),
                              tr("Access"), tr("Member"), tr("Type")});
  evidence_->header()->setStretchLastSection(true);
  evidence_->setFont(code);
  previewLayout->addWidget(member_);
  previewLayout->addWidget(memberType_);
  previewLayout->addWidget(expression_);
  previewLayout->addWidget(note_);
  previewLayout->addSpacing(6);
  previewLayout->addWidget(evidenceTitle_);
  previewLayout->addWidget(evidence_, 1);
  splitter->addWidget(list_);
  splitter->addWidget(preview);
  splitter->setStretchFactor(0, 5);
  splitter->setStretchFactor(1, 6);
  layout->addWidget(splitter, 1);

  every_ = new QCheckBox(this);
  every_->setObjectName(QStringLiteral("typePickerEvery"));
  every_->setChecked(QSettings().value(EverySetting, true).toBool());
  every_->hide();
  layout->addWidget(every_);

  auto *bottom = new QHBoxLayout;
  summary_ = new QLabel(this);
  summary_->setObjectName(QStringLiteral("typePickerSummary"));
  bottom->addWidget(summary_, 1);
  if (parameter_) {
    auto *label = new QLabel(tr("&Name:"), this);
    name_ = new QLineEdit(printedName_, this);
    name_->setObjectName(QStringLiteral("typePickerName"));
    name_->setFont(Theme::instance().codeFont());
    name_->setToolTip(tr("The parameter's name, which the declaration keeps"));
    label->setBuddy(name_);
    bottom->addWidget(label);
    bottom->addWidget(name_);
    connect(name_, &QLineEdit::textChanged, this, &TypePickerDialog::preview);
  }
  auto *buttons = new QDialogButtonBox(this);
  apply_ = buttons->addButton(tr("Apply"), QDialogButtonBox::AcceptRole);
  apply_->setObjectName(QStringLiteral("typePickerApply"));
  apply_->setDefault(true);
  apply_->setEnabled(false);
  buttons->addButton(QDialogButtonBox::Cancel);
  bottom->addWidget(buttons);
  layout->addLayout(bottom);

  connect(buttons, &QDialogButtonBox::accepted, this,
          &TypePickerDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(list_, &QListWidget::itemActivated, this, &TypePickerDialog::accept);
  connect(list_, &QListWidget::currentRowChanged, this,
          &TypePickerDialog::preview);
  debounce_.setSingleShot(true);
  debounce_.setInterval(
      ::neverd::limits::gui::kTypePickerDialogSearchDebounceMs);
  connect(&debounce_, &QTimer::timeout, this, &TypePickerDialog::search);
  connect(search_, &QLineEdit::textChanged, this,
          [this] { debounce_.start(); });

  resize(1000, 600);
  const QSettings settings;
  restoreGeometry(settings.value(GeometrySetting).toByteArray());
  splitter->restoreState(settings.value(SplitterSetting).toByteArray());
  connect(this, &QDialog::finished, this, [splitter] {
    QSettings().setValue(SplitterSetting, splitter->saveState());
  });
  search_->setFocus();
  search();
}

QString TypePickerDialog::parameterName() const {
  const QString name = name_ ? name_->text().trimmed() : QString();
  return name.isEmpty() ? printedName_ : name;
}

QStringList TypePickerDialog::others() const {
  if (every_->isHidden() || !every_->isChecked())
    return {};
  const auto own = addressValue(query_.value(QStringLiteral("address")));
  QStringList result;
  for (const auto &value :
       target_.value(QStringLiteral("accesses")).toArray()) {
    const auto at =
        addressValue(value.toObject().value(QStringLiteral("address")));
    if (at && at != own && !result.contains(hexAddress(*at)))
      result.append(hexAddress(*at));
  }
  return result;
}

void TypePickerDialog::search() {
  const quint64 serial = ++serial_;
  QJsonObject payload{
      {"limit", ::neverd::limits::gui::kTypePickerDialogSearchLimit}};
  const QString text = search_->text().trimmed();
  if (!text.isEmpty())
    payload.insert(QStringLiteral("text"), text);
  // The first search reads the accesses through the operand's pointer; the
  // others judge the same ones.
  const bool first = !answered_;
  if (first)
    for (auto it = query_.begin(); it != query_.end(); ++it)
      payload.insert(it.key(), it.value());
  else
    payload.insert(QStringLiteral("accesses"), accesses_);
  if (parameter_)
    payload.insert(QStringLiteral("shifts"), false);
  session_.read(
      QStringLiteral("local_types_fit"), payload, this,
      [this, serial, first](const QJsonObject &reply) {
        if (serial == serial_)
          listed(reply, first);
      },
      [this, serial, first](const QString &, const QString &message) {
        if (serial == serial_)
          failed(message, first);
      });
}

void TypePickerDialog::listed(const QJsonObject &payload, bool first) {
  if (first) {
    answered_ = true;
    accesses_ = payload.value(QStringLiteral("accesses")).toArray();
    target_ = payload.value(QStringLiteral("target")).toObject();
    const QString syntax = payload.value(QStringLiteral("syntax")).toString();
    syntax_ = syntax == QLatin1String("arm")   ? Syntax::Arm
              : syntax == QLatin1String("sbf") ? Syntax::Sbf
              : syntax == QLatin1String("evm") ? Syntax::Evm
                                               : Syntax::X86;
    if (payload.contains(QStringLiteral("operand")))
      operand_ = payload.value(QStringLiteral("operand")).toInt();
    static_cast<CandidateDelegate *>(list_->itemDelegate())
        ->setAccesses(int(accesses_.size()));
    const QJsonArray through =
        target_.value(QStringLiteral("accesses")).toArray();
    const QString base = target_.value(QStringLiteral("base")).toString();
    QSet<Address> instructions;
    for (const auto &value : through)
      if (const auto at =
              addressValue(value.toObject().value(QStringLiteral("address"))))
        instructions.insert(*at);
    const qint64 number = accesses_.isEmpty()
                              ? 0
                              : accesses_.at(0)
                                    .toObject()
                                    .value(QStringLiteral("offset"))
                                    .toInteger();
    if (parameter_)
      subject_->setText(
          tr("Types ranked by how the function's %n accesses through %1, "
             "where %2 arrives, land on their members.",
             "", int(through.size()))
              .arg(base.isEmpty() ? tr("the register") : base, printedName_));
    else if (target_.isEmpty())
      subject_->setText(
          tr("The number %1 as an offset into a structure or union.")
              .arg((number < 0 ? QStringLiteral("-") : QString()) +
                   (syntax_ == Syntax::Arm   ? armListingNumber
                    : syntax_ == Syntax::X86 ? listingNumber
                                             : bytecodeListingNumber)(
                       quint64(number < 0 ? -number : number))));
    else
      subject_->setText(
          tr("Types ranked by how the function's %n accesses through %1 land "
             "on their members.",
             "", int(through.size()))
              .arg(base.isEmpty() ? tr("the pointer") : base));
    const int others = int(instructions.size()) - 1;
    // A parameter becomes a pointer to the type: the operands of other
    // instructions stay as they are.
    every_->setVisible(!parameter_ && others > 0);
    every_->setText(
        tr("Also show the %n other instructions that go through %1 as "
           "members",
           "", others)
            .arg(base.isEmpty() ? tr("the pointer") : base));
    // A search typed while the first was on its way runs again over them.
    if (!search_->text().trimmed().isEmpty()) {
      search();
      return;
    }
  }
  const QString previous = list_->currentItem()
                               ? list_->currentItem()
                                     ->data(CandidateRole)
                                     .toJsonObject()
                                     .value(QStringLiteral("name"))
                                     .toString()
                               : QString();
  list_->clear();
  int select = 0;
  for (const auto &value :
       payload.value(QStringLiteral("candidates")).toArray()) {
    const QJsonObject candidate = value.toObject();
    auto *item = new QListWidgetItem(list_);
    item->setData(CandidateRole, candidate);
    item->setText(candidate.value(QStringLiteral("name")).toString());
    if (item->text() == previous)
      select = list_->count() - 1;
  }
  const int total = payload.value(QStringLiteral("total")).toInt();
  summary_->setText(list_->count() < total
                        ? tr("%1 of %n types", "", total).arg(list_->count())
                        : tr("%n types", "", total));
  if (list_->count()) {
    list_->setCurrentRow(select);
  } else {
    preview();
    if (!total)
      summary_->setText(
          search_->text().trimmed().isEmpty()
              ? tr("The program has no structures or unions. Declare them in "
                   "Local types (%1).")
                    .arg(QKeySequence(Qt::SHIFT | Qt::Key_F1)
                             .toString(QKeySequence::NativeText))
              : tr("No type matches."));
  }
  apply_->setEnabled(list_->count() > 0);
}

void TypePickerDialog::failed(const QString &message, bool first) {
  if (first) {
    // Without the pointer's accesses the types are searched by name alone,
    // every one listed until the user types.
    answered_ = true;
    subject_->setText(message);
    search();
    return;
  }
  list_->clear();
  preview();
  summary_->setText(message);
  apply_->setEnabled(false);
}

int TypePickerDialog::ownAccess() const {
  const QJsonArray through =
      target_.value(QStringLiteral("accesses")).toArray();
  const auto own = addressValue(query_.value(QStringLiteral("address")));
  const int before = int(accesses_.size()) - int(through.size());
  for (int i = 0; i < through.size(); ++i)
    if (addressValue(
            through.at(i).toObject().value(QStringLiteral("address"))) == own)
      return before + i;
  return accesses_.isEmpty() ? -1 : 0;
}

void TypePickerDialog::preview() {
  evidence_->clear();
  note_->hide();
  const auto *item = list_->currentItem();
  if (!item) {
    member_->clear();
    memberType_->clear();
    expression_->clear();
    evidenceTitle_->clear();
    return;
  }
  const QJsonObject candidate = item->data(CandidateRole).toJsonObject();
  const QString record =
      candidate.contains(QStringLiteral("record"))
          ? candidate.value(QStringLiteral("record")).toString()
          : candidate.value(QStringLiteral("name")).toString();
  const QJsonArray fits = candidate.value(QStringLiteral("fits")).toArray();
  const Theme &theme = Theme::instance();

  // What the parameter becomes: a pointer to the type.
  if (parameter_) {
    member_->setText(QStringLiteral("%1 *%2").arg(record, parameterName()));
    member_->setStyleSheet(
        QStringLiteral("color: %1")
            .arg(theme.color(ColorRole::ListingStructOffset).name()));
    memberType_->setText(
        tr("A pointer to %1, a record of %n bytes", "",
           int(candidate.value(QStringLiteral("recordSize"))
                   .toInteger(
                       candidate.value(QStringLiteral("size")).toInteger())))
            .arg(record));
    expression_->clear();
  }
  // What the operand becomes.
  const int own = parameter_ ? -1 : ownAccess();
  if (own >= 0 && own < fits.size()) {
    const QJsonObject fit = fits.at(own).toObject();
    const QString path = fit.value(QStringLiteral("path")).toString();
    const QString kind = fit.value(QStringLiteral("fit")).toString();
    const qint64 offset = accesses_.at(own)
                              .toObject()
                              .value(QStringLiteral("offset"))
                              .toInteger();
    const qint64 rest =
        offset - fit.value(QStringLiteral("memberOffset")).toInteger();
    QString shown =
        record + (path.isEmpty() ? QString() : QLatin1Char('.') + path);
    if (kind == QLatin1String("outside") ||
        (kind == QLatin1String("padding") && path.isEmpty())) {
      member_->setText(tr("No member holds this byte"));
      member_->setStyleSheet(
          QStringLiteral("color: %1").arg(fitColor(kind).name()));
      memberType_->setText(tr("The operand keeps its number."));
      expression_->clear();
    } else {
      member_->setText(shown);
      member_->setStyleSheet(
          QStringLiteral("color: %1")
              .arg(theme.color(ColorRole::ListingStructOffset).name()));
      const QString type = fit.value(QStringLiteral("type")).toString();
      memberType_->setText(
          QStringLiteral("%1  ·  %2  ·  %3")
              .arg(type.isEmpty() ? tr("padding") : type,
                   offsetText(
                       fit.value(QStringLiteral("memberOffset")).toInteger()),
                   tr("%n bytes", "",
                      int(fit.value(QStringLiteral("memberSize"))
                              .toInteger()))));
      // As the listing will write the operand: `[rcx+(_T.List+8)]` in x86,
      // `[x0, #_T.List+0x8]` in the ARM family, `[r1 + _T.List+8]` in SBF.
      if (rest)
        shown = (syntax_ == Syntax::X86 ? QStringLiteral("(%1%2%3)")
                                        : QStringLiteral("%1%2%3"))
                    .arg(shown,
                         rest < 0 ? QStringLiteral("-") : QStringLiteral("+"),
                         (syntax_ == Syntax::Arm   ? armListingNumber
                          : syntax_ == Syntax::X86 ? listingNumber
                                                   : bytecodeListingNumber)(
                             quint64(rest < 0 ? -rest : rest)));
      const QString base = target_.value(QStringLiteral("base")).toString();
      const QString reg = base.isEmpty() ? QStringLiteral("reg") : base;
      switch (syntax_) {
      case Syntax::Arm:
        expression_->setText(target_.isEmpty()
                                 ? QLatin1Char('#') + shown
                                 : QStringLiteral("[%1, #%2]").arg(reg, shown));
        break;
      case Syntax::Sbf:
        expression_->setText(target_.isEmpty()
                                 ? shown
                                 : QStringLiteral("[%1 + %2]").arg(reg, shown));
        break;
      case Syntax::Evm:
        expression_->setText(shown);
        break;
      case Syntax::X86:
        expression_->setText(target_.isEmpty()
                                 ? shown
                                 : QStringLiteral("[%1+%2]").arg(reg, shown));
        break;
      }
    }
  }
  if (candidate.contains(QStringLiteral("shift"))) {
    note_->setText(
        tr("The register points %1 bytes into %2, at %3: the accesses before "
           "it read the record's earlier members, as CONTAINING_RECORD "
           "undoes.")
            .arg(candidate.value(QStringLiteral("shift")).toInteger())
            .arg(record,
                 candidate.value(QStringLiteral("shiftPath")).toString()));
    note_->show();
  }

  // Every access, once per offset, width and use, as the type lands it.
  struct Row {
    qint64 offset;
    qint64 bytes;
    QString use;
    QJsonObject fit;
    QStringList at;
  };
  std::map<std::tuple<qint64, qint64, QString>, Row> rows;
  const QJsonArray through =
      target_.value(QStringLiteral("accesses")).toArray();
  const int before = int(accesses_.size()) - int(through.size());
  for (int i = 0; i < accesses_.size() && i < fits.size(); ++i) {
    const QJsonObject access = accesses_.at(i).toObject();
    const qint64 offset = access.value(QStringLiteral("offset")).toInteger();
    const qint64 bytes = access.value(QStringLiteral("size")).toInteger();
    const QString use = access.value(QStringLiteral("use")).toString();
    auto &row =
        rows.try_emplace({offset, bytes, use},
                         Row{offset, bytes, use, fits.at(i).toObject(), {}})
            .first->second;
    if (i >= before)
      row.at.append(through.at(i - before)
                        .toObject()
                        .value(QStringLiteral("address"))
                        .toString());
  }
  int exact = 0;
  for (const auto &[key, row] : rows) {
    const QString kind = row.fit.value(QStringLiteral("fit")).toString();
    exact += kind == QLatin1String("exact");
    auto *line = new QTreeWidgetItem(evidence_);
    line->setText(0, fitMark(kind));
    line->setForeground(0, fitColor(kind));
    line->setText(1, offsetText(row.offset));
    line->setText(2, row.bytes ? QString::number(row.bytes)
                               : QStringLiteral("\u2014"));
    line->setText(3, row.use == QLatin1String("write")     ? tr("write")
                     : row.use == QLatin1String("address") ? tr("address")
                                                           : tr("read"));
    const QString path = row.fit.value(QStringLiteral("path")).toString();
    line->setText(4, kind == QLatin1String("outside") ? tr("outside the type")
                     : kind == QLatin1String("padding") && path.isEmpty()
                         ? tr("padding")
                         : path);
    line->setText(5, row.fit.value(QStringLiteral("type")).toString());
    if (kind != QLatin1String("exact"))
      line->setForeground(4, fitColor(kind));
    if (!row.at.isEmpty())
      line->setToolTip(4, tr("At %1").arg(row.at.join(QStringLiteral(", "))));
  }
  for (int column = 0; column < evidence_->columnCount() - 1; ++column)
    evidence_->resizeColumnToContents(column);
  evidenceTitle_->setText(
      rows.empty()
          ? QString()
          : tr("%1 of %n offsets land exactly on members", "", int(rows.size()))
                .arg(exact));
}

bool TypePickerDialog::eventFilter(QObject *object, QEvent *event) {
  if (object == search_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    // The list answers the keys that move through it while the search
    // keeps the focus.
    switch (key->key()) {
    case Qt::Key_Up:
    case Qt::Key_Down:
    case Qt::Key_PageUp:
    case Qt::Key_PageDown:
      QApplication::sendEvent(list_, event);
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      // A search still on its way is answered first.
      if (debounce_.isActive()) {
        debounce_.stop();
        search();
        return true;
      }
      accept();
      return true;
    default:
      break;
    }
  }
  return QDialog::eventFilter(object, event);
}

void TypePickerDialog::accept() {
  const auto *item = list_->currentItem();
  if (!item)
    return;
  const QJsonObject candidate = item->data(CandidateRole).toJsonObject();
  type_ = candidate.contains(QStringLiteral("record"))
              ? candidate.value(QStringLiteral("record")).toString()
              : candidate.value(QStringLiteral("name")).toString();
  delta_ = candidate.value(QStringLiteral("shift")).toInteger();
  if (!every_->isHidden())
    QSettings().setValue(EverySetting, every_->isChecked());
  QDialog::accept();
}

void TypePickerDialog::done(int result) {
  QSettings().setValue(GeometrySetting, saveGeometry());
  QDialog::done(result);
}

} // namespace neverd::gui
