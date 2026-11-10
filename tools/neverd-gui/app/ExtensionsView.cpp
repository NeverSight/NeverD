#include "ExtensionsView.h"

#include "../Config.h"
#include "ItemViewActivation.h"
#include "KeepTranslated.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {
constexpr int IdRole = Qt::UserRole;
constexpr int NamespaceRole = Qt::UserRole + 1;
} // namespace

ExtensionsView::ExtensionsView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *splitter = new QSplitter(this);
  auto *left = new QWidget(splitter);
  auto *leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(4, 4, 4, 4);
  auto *buttonRow = new QWidget(left);
  auto *buttons = new QHBoxLayout(buttonRow);
  buttons->setContentsMargins(0, 0, 0, 0);
  import_ = new QPushButton(buttonRow);
  run_ = new QPushButton(buttonRow);
  unload_ = new QPushButton(buttonRow);
  buttons->addWidget(import_);
  buttons->addWidget(run_);
  buttons->addWidget(unload_);
  buttons->addStretch(1);
  makeRowShrinkable(*buttonRow);
  leftLayout->addWidget(buttonRow);
  list_ = new QListWidget(left);
  list_->setMinimumWidth(
      ::neverd::limits::gui_config::kExtensionsViewMinimumPaneWidth);
  activateOnReturn(*list_);
  leftLayout->addWidget(list_, 1);
  result_ = new QPlainTextEdit(splitter);
  result_->setMinimumWidth(
      ::neverd::limits::gui_config::kExtensionsViewMinimumPaneWidth);
  result_->setReadOnly(true);
  result_->setFont(Theme::instance().codeFont());
  splitter->setStretchFactor(1, 1);
  layout->addWidget(splitter);
  keepTranslated(*this, [this, splitter, buttonRow, leftLayout] {
    import_->setText(tr("Import queries…"));
    run_->setText(tr("Run"));
    unload_->setText(tr("Remove group"));
    unload_->setToolTip(tr("Remove all queries in the selected group."));
    list_->setToolTip(tr("Run built-in analysis queries or import query "
                         "definitions from a JSON file."));
    result_->setPlaceholderText(
        tr("Select a query and choose Run to view its result."));
    // Start wide enough for the translated actions, and widen again for a
    // language whose actions are longer, while keeping the pane shrinkable
    // when the user drags its splitter or docks it in a narrow area.
    const auto margins = leftLayout->contentsMargins();
    const int actions =
        buttonRow->sizeHint().width() + margins.left() + margins.right();
    const QList<int> sizes = splitter->sizes();
    if (sizes.size() != 2 || sizes[0] + sizes[1] <= 0)
      splitter->setSizes({actions, result_->sizeHint().width()});
    else if (sizes[0] < actions)
      splitter->setSizes({actions, std::max(0, sizes[0] + sizes[1] - actions)});
  });
  connect(import_, &QPushButton::clicked, this,
          &ExtensionsView::importManifest);
  connect(run_, &QPushButton::clicked, this, &ExtensionsView::run);
  connect(list_, &QListWidget::itemActivated, this, &ExtensionsView::run);
  connect(unload_, &QPushButton::clicked, this, [this] {
    if (auto *item = list_->currentItem())
      session_.unregisterContributions(item->data(NamespaceRole).toString());
  });
  connect(list_, &QListWidget::currentItemChanged, this,
          &ExtensionsView::updateButtons);
  connect(&session_, &Session::contributionsChanged, this,
          &ExtensionsView::refresh);
  connect(&session_, &Session::stateChanged, this,
          &ExtensionsView::updateButtons);
  connect(&session_, &Session::contributionResult, this,
          [this](const QJsonObject &result) {
            result_->setPlainText(QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Indented)));
          });
  refresh();
}

void ExtensionsView::importManifest() {
  const auto path =
      QFileDialog::getOpenFileName(this, tr("Import analysis queries"), {},
                                   tr("Query definitions (*.json)"));
  if (!path.isEmpty())
    session_.registerContributions(path);
}

void ExtensionsView::refresh() {
  const QString current = list_->currentItem()
                              ? list_->currentItem()->data(IdRole).toString()
                              : QString();
  list_->clear();
  for (const auto &value : session_.contributions()) {
    const auto entry = value.toObject();
    auto *item =
        new QListWidgetItem(QStringLiteral("%1\n%2 · %3")
                                .arg(entry.value("title").toString(),
                                     entry.value("namespace").toString(),
                                     entry.value("kind").toString()),
                            list_);
    item->setData(IdRole, entry.value("id").toString());
    item->setData(NamespaceRole, entry.value("namespace").toString());
    if (entry.value("id").toString() == current)
      list_->setCurrentItem(item);
  }
  updateButtons();
}

void ExtensionsView::updateButtons() {
  const bool selected = list_->currentItem() != nullptr;
  import_->setEnabled(session_.loaded());
  run_->setEnabled(selected && session_.loaded());
  unload_->setEnabled(selected);
}

void ExtensionsView::run() {
  if (auto *item = list_->currentItem())
    session_.executeContribution(item->data(IdRole).toString(),
                                 location_ ? location_() : std::nullopt);
}

} // namespace neverd::gui
