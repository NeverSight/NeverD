//===- TypeLibrariesView.cpp - IDA's type libraries window ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "TypeLibrariesView.h"

#include "Icons.h"
#include "ModelessDialog.h"
#include "Session.h"
#include "ShortcutHint.h"
#include "ShrinkableRow.h"

#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QShortcut>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {

enum Column {
  LibraryColumn,
  OriginColumn,
  DataModelColumn,
  RecordABIColumn,
  TypesColumn,
  PrototypesColumn,
  ConflictsColumn,
  Columns
};

} // namespace

TypeLibrariesView::TypeLibrariesView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);
  auto *commands = new QWidget(this);
  auto *commandLayout = new QHBoxLayout(commands);
  commandLayout->setContentsMargins(0, 0, 0, 0);
  const QKeySequence addKey(Qt::Key_Insert);
  const QKeySequence removeKey(QKeySequence::Delete);
  const auto commandButton = [&](const char *iconName, const QString &text,
                                 const QKeySequence &key) {
    auto *button = new QToolButton(commands);
    button->setIcon(icon(QString::fromLatin1(iconName)));
    button->setToolTip(QStringLiteral("%1 (%2)").arg(
        text, key.toString(QKeySequence::NativeText)));
    setShortcutHint(*button, key);
    button->setAutoRaise(true);
    commandLayout->addWidget(button);
    return button;
  };
  add_ = commandButton("local_types_add", tr("Add a type library"), addKey);
  add_->setObjectName(QStringLiteral("typeLibrariesAdd"));
  remove_ = commandButton("local_types_delete",
                          tr("Leave the selected library out"), removeKey);
  remove_->setObjectName(QStringLiteral("typeLibrariesRemove"));
  commandLayout->addStretch(1);
  makeRowShrinkable(*commands);
  layout->addWidget(commands);

  table_ = new QTreeWidget(this);
  table_->setObjectName(QStringLiteral("typeLibrariesList"));
  table_->setColumnCount(Columns);
  table_->setHeaderLabels({tr("Library"), tr("Origin"), tr("Data model"),
                           tr("Record ABI"), tr("Types"), tr("Prototypes"),
                           tr("Conflicts")});
  table_->setRootIsDecorated(false);
  table_->setUniformRowHeights(true);
  table_->setAllColumnsShowFocus(true);
  table_->setContextMenuPolicy(Qt::CustomContextMenu);
  table_->header()->setStretchLastSection(false);
  table_->header()->setSectionResizeMode(LibraryColumn,
                                         QHeaderView::ResizeToContents);
  layout->addWidget(table_, 1);
  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("typeLibrariesStatus"));
  status_->setWordWrap(true);
  layout->addWidget(status_);

  connect(add_, &QToolButton::clicked, this, &TypeLibrariesView::addLibrary);
  connect(remove_, &QToolButton::clicked, this,
          &TypeLibrariesView::removeLibrary);
  // IDA's keys of its list windows: Ins adds, Del removes.
  for (const auto &[key, slot] :
       {std::pair(addKey, &TypeLibrariesView::addLibrary),
        std::pair(removeKey, &TypeLibrariesView::removeLibrary)}) {
    auto *shortcut = new QShortcut(key, table_);
    shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut, &QShortcut::activated, this, slot);
  }
  connect(table_, &QTreeWidget::customContextMenuRequested, this,
          [this, addKey, removeKey](const QPoint &point) {
            QMenu menu(this);
            const auto item = [&](QToolButton *button, const QString &text,
                                  const QKeySequence &key, auto slot) {
              auto *action = menu.addAction(button->icon(), text, this, slot);
              action->setShortcut(key);
              action->setShortcutContext(Qt::WidgetShortcut);
              action->setShortcutVisibleInContextMenu(true);
              action->setEnabled(button->isEnabled());
            };
            item(add_, tr("Add a type library..."), addKey,
                 &TypeLibrariesView::addLibrary);
            item(remove_, tr("Leave the library out"), removeKey,
                 &TypeLibrariesView::removeLibrary);
            menu.exec(table_->viewport()->mapToGlobal(point));
          });
  connect(table_, &QTreeWidget::currentItemChanged, this,
          &TypeLibrariesView::updateButtons);
  connect(&session_, &Session::opened, this, &TypeLibrariesView::reload);
  connect(&session_, &Session::revisionChanged, this,
          &TypeLibrariesView::reload);
  connect(&session_, &Session::unloaded, this, &TypeLibrariesView::reload);
  // The first catalogue arrives asynchronously; no choice is available yet.
  updateButtons();
  reload();
}

void TypeLibrariesView::reload() {
  if (!session_.loaded()) {
    libraries_ = {};
    added_.clear();
    removed_.clear();
    table_->clear();
    status_->setText(tr("Open a program to see the type libraries its types "
                        "join."));
    updateButtons();
    return;
  }
  if (!session_.choosesTypeLibraries()) {
    table_->clear();
    status_->setText(tr("This engine joins no type libraries."));
    updateButtons();
    return;
  }
  session_.read(
      QStringLiteral("type_libraries"), {}, this,
      [this](const QJsonObject &payload) { listed(payload); },
      [this](const QString &, const QString &message) { failed(message); });
}

void TypeLibrariesView::listed(const QJsonObject &payload) {
  libraries_ = payload.value(QStringLiteral("libraries")).toArray();
  added_.clear();
  removed_.clear();
  for (const QJsonValue &name :
       payload.value(QStringLiteral("added")).toArray())
    added_.push_back(name.toString());
  for (const QJsonValue &name :
       payload.value(QStringLiteral("removed")).toArray())
    removed_.push_back(name.toString());
  const QString current = table_->currentItem()
                              ? table_->currentItem()->text(LibraryColumn)
                              : QString();
  table_->clear();
  int joined = 0;
  QStringList problems;
  // The libraries joined, in the order they join: the image's, then the
  // user's.
  for (const bool image : {true, false})
    for (const QJsonValue &value : libraries_) {
      const QJsonObject row = value.toObject();
      const QString name = row.value(QStringLiteral("name")).toString();
      if (!row.value(QStringLiteral("joined")).toBool() ||
          row.value(QStringLiteral("image")).toBool() != image ||
          (!image && !added_.contains(name)))
        continue;
      auto *item = new QTreeWidgetItem(table_);
      item->setText(LibraryColumn, name);
      item->setText(OriginColumn, image ? tr("The program") : tr("Added"));
      item->setText(DataModelColumn,
                    row.value(QStringLiteral("dataModel")).toString());
      item->setText(RecordABIColumn,
                    row.value(QStringLiteral("recordABI")).toString());
      if (const QString error = row.value(QStringLiteral("error")).toString();
          !error.isEmpty()) {
        item->setToolTip(LibraryColumn, error);
        problems.push_back(tr("%1 joined nothing: %2").arg(name, error));
      } else {
        item->setText(
            TypesColumn,
            QString::number(row.value(QStringLiteral("types")).toInt()));
        item->setText(
            PrototypesColumn,
            QString::number(row.value(QStringLiteral("prototypes")).toInt()));
        const QJsonArray conflicts =
            row.value(QStringLiteral("conflicts")).toArray();
        item->setText(ConflictsColumn, QString::number(conflicts.size()));
        if (!conflicts.isEmpty()) {
          QStringList names;
          for (const QJsonValue &conflict : conflicts)
            names.push_back(conflict.toString());
          item->setToolTip(ConflictsColumn,
                           tr("Left out, as the program sizes a type they "
                              "hold otherwise: %1")
                               .arg(names.join(QStringLiteral(", "))));
        }
      }
      for (int column : {TypesColumn, PrototypesColumn, ConflictsColumn})
        item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
      if (name == current)
        table_->setCurrentItem(item);
      ++joined;
    }
  if (!table_->currentItem() && table_->topLevelItemCount())
    table_->setCurrentItem(table_->topLevelItem(0));
  QString text = joined == 1 ? tr("1 type library joins the program's types.")
                 : joined    ? tr("%n type libraries join the program's types.",
                                  nullptr, joined)
                             : tr("No type library joins the program's types.");
  if (!removed_.isEmpty())
    text += QLatin1Char(' ') +
            tr("Left out: %1.").arg(removed_.join(QStringLiteral(", ")));
  if (!problems.isEmpty())
    text += QLatin1Char(' ') + problems.join(QStringLiteral(" "));
  status_->setText(text);
  updateButtons();
}

void TypeLibrariesView::failed(const QString &message) {
  status_->setText(
      tr("The type libraries could not be listed: %1").arg(message));
  updateButtons();
}

void TypeLibrariesView::updateButtons() {
  const bool chooses = session_.loaded() && session_.choosesTypeLibraries() &&
                       !session_.readOnly();
  bool another = false;
  for (const QJsonValue &value : libraries_) {
    const QJsonObject row = value.toObject();
    another |= row.value(QStringLiteral("compatible")).toBool() &&
               !row.value(QStringLiteral("joined")).toBool();
  }
  add_->setEnabled(chooses && another);
  remove_->setEnabled(chooses && table_->currentItem() != nullptr);
}

void TypeLibrariesView::addLibrary() {
  if (!add_->isEnabled())
    return;
  // The libraries whose records lay out as the program's that join none of
  // its types now: the image's left out among them.
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  auto *dialog = new QDialog(this);
  dialog->setObjectName(QStringLiteral("addTypeLibraryDialog"));
  dialog->setWindowTitle(tr("Add a Type Library"));
  auto *layout = new QVBoxLayout(dialog);
  layout->addWidget(new QLabel(
      tr("Libraries whose records lay out as the program's:"), dialog));
  auto *choices = new QTreeWidget(dialog);
  choices->setObjectName(QStringLiteral("typeLibraryChoices"));
  choices->setColumnCount(3);
  choices->setHeaderLabels({tr("Library"), tr("Data model"), tr("Record ABI")});
  choices->setRootIsDecorated(false);
  choices->setAllColumnsShowFocus(true);
  for (const QJsonValue &value : libraries_) {
    const QJsonObject row = value.toObject();
    if (!row.value(QStringLiteral("compatible")).toBool() ||
        row.value(QStringLiteral("joined")).toBool())
      continue;
    auto *item = new QTreeWidgetItem(choices);
    item->setText(0, row.value(QStringLiteral("name")).toString());
    item->setText(1, row.value(QStringLiteral("dataModel")).toString());
    item->setText(2, row.value(QStringLiteral("recordABI")).toString());
  }
  if (choices->topLevelItemCount())
    choices->setCurrentItem(choices->topLevelItem(0));
  choices->header()->resizeSections(QHeaderView::ResizeToContents);
  layout->addWidget(choices, 1);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr("Add"));
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(choices, &QTreeWidget::itemActivated, dialog, &QDialog::accept);
  layout->addWidget(buttons);
  connect(&session_, &Session::revisionChanged, dialog, &QDialog::reject);
  connect(dialog, &QDialog::accepted, this, [this, choices, epoch, revision] {
    if (!choices->currentItem() || !session_.loaded() ||
        session_.epoch() != epoch || session_.revision() != revision)
      return;
    const QString name = choices->currentItem()->text(0);
    QStringList added = added_, removed = removed_;
    // One of the image's libraries the user left out joins again.
    if (!removed.removeAll(name))
      added.push_back(name);
    choose(added, removed);
  });
  showModelessDialog(dialog, this, &session_);
}

void TypeLibrariesView::removeLibrary() {
  if (!remove_->isEnabled() || !table_->currentItem())
    return;
  const QString name = table_->currentItem()->text(LibraryColumn);
  QStringList added = added_, removed = removed_;
  // A library the user added goes; one of the image's is left out.
  if (!added.removeAll(name))
    removed.push_back(name);
  choose(added, removed);
}

void TypeLibrariesView::choose(const QStringList &added,
                               const QStringList &removed) {
  status_->setText(tr("Reading the program's types again..."));
  add_->setEnabled(false);
  remove_->setEnabled(false);
  session_.setTypeLibraries(
      added, removed, [this](const QJsonObject &payload) { listed(payload); },
      [this](const QString &, const QString &message) {
        status_->setText(
            tr("The type libraries could not be changed: %1").arg(message));
        updateButtons();
      });
}

} // namespace neverd::gui
