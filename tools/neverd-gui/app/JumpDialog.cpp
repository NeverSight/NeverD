#include "JumpDialog.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "Address.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "ListingView.h"
#include "Resolve.h"
#include "SecondaryTextDelegate.h"
#include "Session.h"
#include "Theme.h"

#include <QDialogButtonBox>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
constexpr char HistoryKey[] = "jump/history";
constexpr int AddressRole = Qt::UserRole;
} // namespace

void JumpDialog::remember(const QString &text) {
  if (text.trimmed().isEmpty())
    return;
  QSettings settings;
  auto history = settings.value(HistoryKey).toStringList();
  history.removeAll(text);
  history.prepend(text);
  while (history.size() > ::neverd::limits::gui::kJumpDialogMaxHistory)
    history.removeLast();
  settings.setValue(HistoryKey, history);
}

JumpDialog::JumpDialog(Session &session, const AddressSpace &space,
                       std::optional<Address> here, QWidget *parent)
    : QDialog(parent), session_(session), input_(new QLineEdit(this)),
      matches_(new QListWidget(this)), hint_(new QLabel(this)),
      preview_(new ListingView(session, space, this)), here_(here) {
  setWindowTitle(tr("Jump anywhere"));
  setWindowIcon(icon(QStringLiteral("jump")));
  activateOnReturn(*matches_);
  resize(::neverd::limits::gui_config::kJumpDialogWidth,
         ::neverd::limits::gui_config::kJumpDialogHeight);
  auto *layout = new QVBoxLayout(this);
  input_->setPlaceholderText(
      tr("Address, name or expression (0x401000, main, sub_401000+10)"));
  input_->setFont(Theme::instance().codeFont());
  input_->installEventFilter(this);
  layout->addWidget(input_);
  matches_->setUniformItemSizes(true);
  // Names read in the list's font; their addresses line up at the right.
  QFont addresses = Theme::instance().codeFont();
  addresses.setPointSizeF(QFontInfo(matches_->font()).pointSizeF());
  matches_->setItemDelegate(new SecondaryTextDelegate(addresses, matches_));
  // The preview shows where Enter goes while the typing stays in the field.
  preview_->setObjectName(QStringLiteral("jumpPreview"));
  preview_->setFocusPolicy(Qt::NoFocus);
  auto *split = new QSplitter(Qt::Horizontal, this);
  split->addWidget(matches_);
  split->addWidget(preview_);
  split->setStretchFactor(1, 1);
  split->setSizes({::neverd::limits::gui_config::kJumpDialogListWidth,
                   ::neverd::limits::gui_config::kJumpDialogWidth -
                       ::neverd::limits::gui_config::kJumpDialogListWidth});
  layout->addWidget(split, 1);
  layout->addWidget(hint_);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, this, &JumpDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(matches_, &QListWidget::itemActivated, this, [this] { accept(); });
  connect(matches_, &QListWidget::currentRowChanged, this, [this](int row) {
    if (row >= 0)
      updatePreview();
  });
  debounce_.setSingleShot(true);
  debounce_.setInterval(::neverd::limits::gui::kJumpDialogSearchDebounceMs);
  connect(&debounce_, &QTimer::timeout, this, &JumpDialog::search);
  connect(input_, &QLineEdit::textChanged, this, [this] {
    // Retire the previous text's replies before the debounce fires.
    ++serial_;
    debounce_.stop();
    matches_->clear();
    hint_->clear();
    if (input_->text().trimmed().isEmpty())
      showRecent();
    else
      debounce_.start();
  });
  showRecent();
}

void JumpDialog::showRecent() {
  matches_->clear();
  // Recent jumps while nothing is typed.
  for (const auto &text : QSettings().value(HistoryKey).toStringList()) {
    auto *item =
        new QListWidgetItem(icon(QStringLiteral("back")), text, matches_);
    item->setData(AddressRole, text);
  }
  hint_->setText(tr("Enter jumps to the expression; arrows choose a match."));
}

void JumpDialog::search() {
  const QString text = input_->text().trimmed();
  const quint64 serial = ++serial_;
  updatePreview();
  if (text.isEmpty() || !session_.loaded())
    return;
  session_.read(
      QStringLiteral("names"),
      {{"filter", text},
       {"limit", ::neverd::limits::gui::kJumpDialogMaxMatches},
       {"sort", "name"}},
      this,
      [this, serial](const QJsonObject &payload) {
        if (serial != serial_)
          return;
        matches_->clear();
        const int digits = session_.bitness() == 64 ? 16 : 8;
        for (const auto &value : payload.value("items").toArray()) {
          const auto row = value.toObject();
          const auto address = addressValue(row.value("address"));
          if (!address)
            continue;
          const auto kind = row.value("kind").toString();
          const QString iconName = kind == QLatin1String("import")   ? "imports"
                                   : kind == QLatin1String("string") ? "strings"
                                   : kind == QLatin1String("data")
                                       ? "data_item"
                                       : "functions";
          auto *item = new QListWidgetItem(
              icon(iconName), row.value("name").toString(), matches_);
          item->setData(SecondaryTextDelegate::SecondaryTextRole,
                        displayAddress(*address, digits));
          item->setData(AddressRole, hexAddress(*address));
        }
        hint_->setText(
            tr("%n matches", "", int(payload.value("total").toInt())));
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == serial_)
          hint_->setText(message);
      });
}

void JumpDialog::updatePreview() {
  const quint64 serial = ++previewSerial_;
  const auto *item = matches_->currentItem();
  const QString text = item && item->isSelected()
                           ? item->data(AddressRole).toString()
                           : input_->text().trimmed();
  if (text.isEmpty() || !session_.loaded()) {
    preview_->clear();
    return;
  }
  // The expression resolves as Enter resolves it; text that names nothing
  // yet shows nothing rather than an older place.
  resolveExpression(
      session_, this, text, here_,
      [this, serial](std::optional<Address> value, const QString &) {
        if (serial != previewSerial_)
          return;
        if (value)
          preview_->jumpTo(*value);
        else
          preview_->clear();
      });
}

bool JumpDialog::eventFilter(QObject *object, QEvent *event) {
  if (object == input_ && event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Down || key->key() == Qt::Key_Up) {
      const int count = matches_->count();
      if (!count)
        return true;
      int row = matches_->currentRow();
      row = key->key() == Qt::Key_Down ? std::min(count - 1, row + 1)
                                       : std::max(-1, row - 1);
      matches_->setCurrentRow(row);
      return true;
    }
  }
  return QDialog::eventFilter(object, event);
}

void JumpDialog::accept() {
  const auto *item = matches_->currentItem();
  if (item && item->isSelected())
    result_ = item->data(AddressRole).toString();
  else
    result_ = input_->text().trimmed();
  if (result_.isEmpty())
    return;
  remember(input_->text().trimmed().isEmpty() ? result_
                                              : input_->text().trimmed());
  QDialog::accept();
}

} // namespace neverd::gui
