//===- PluginManagerDialog.cpp - Installed plugin management --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Installation controls consume the existing Session adapter and NPH owners.
//===----------------------------------------------------------------------===//

#include "PluginManagerDialog.h"

#include "../ResourceLimits.h"
#include "DialogReturnKey.h"
#include "FlowLayout.h"
#include "Icons.h"
#include "PluginServices.h"

#include <QAbstractItemView>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {
QString activationText(PluginJobs &Jobs, const QString &Id) {
  using neverd::plugin::OwnerPhase;
  if (Jobs.retiring(Id))
    return PluginManagerDialog::tr("Stopping");
  if (const auto Owner = Jobs.componentOwner(Id))
    if (const auto State = Jobs.host().snapshot(*Owner)) {
      switch (State->Phase) {
      case OwnerPhase::Activating:
        return PluginManagerDialog::tr("Activating");
      case OwnerPhase::Active:
        return PluginManagerDialog::tr("Active");
      case OwnerPhase::Retiring:
        return PluginManagerDialog::tr("Stopping");
      case OwnerPhase::Closed:
        return PluginManagerDialog::tr("Inactive");
      }
    }
  return PluginManagerDialog::tr("Inactive");
}
} // namespace

PluginManagerDialog::PluginManagerDialog(PluginServices &Services,
                                         QWidget *Parent)
    : QDialog(Parent), Services(Services) {
  setObjectName(QStringLiteral("pluginManagerDialog"));
  setWindowTitle(tr("Plugin manager"));
  setWindowIcon(icon(QStringLiteral("extensions")));
  resize(::neverd::limits::gui::plugin::kManagerWidth,
         ::neverd::limits::gui::plugin::kManagerHeight);
  auto *Layout = new QVBoxLayout(this);
  auto *Commands = new QWidget(this);
  auto *Buttons = new FlowLayout(Commands);
  auto *Import = new QPushButton(tr("Import plugin…"), Commands);
  Import->setObjectName(QStringLiteral("pluginManagerImport"));
  Toggle = new QPushButton(tr("Disable"), Commands);
  Toggle->setObjectName(QStringLiteral("pluginManagerToggle"));
  Retry = new QPushButton(tr("Retry activation"), Commands);
  Retry->setObjectName(QStringLiteral("pluginManagerRetry"));
  Uninstall = new QPushButton(tr("Uninstall"), Commands);
  Uninstall->setObjectName(QStringLiteral("pluginManagerUninstall"));
  Uninstall->setToolTip(tr("Remove the saved registration. Plugin files and "
                           "task artifacts are kept."));
  Location = new QPushButton(tr("Open folder"), Commands);
  Location->setObjectName(QStringLiteral("pluginManagerLocation"));
  for (auto *Button : {Import, Toggle, Retry, Uninstall, Location})
    Buttons->addWidget(Button);
  Layout->addWidget(Commands);

  Filter = new QLineEdit(this);
  Filter->setObjectName(QStringLiteral("pluginManagerFilter"));
  Filter->setPlaceholderText(tr("Filter by plugin name, ID or path"));
  Filter->setAccessibleName(tr("Filter plugins"));
  Layout->addWidget(Filter);
  auto *Splitter = new QSplitter(Qt::Vertical, this);
  Plugins = new QTreeWidget(Splitter);
  Plugins->setObjectName(QStringLiteral("pluginManagerList"));
  Plugins->setAccessibleName(tr("Installed process plugins"));
  Plugins->setHeaderLabels(
      {tr("Plugin"), tr("Version"), tr("Enabled"), tr("Activation")});
  Plugins->setRootIsDecorated(false);
  Plugins->setSelectionMode(QAbstractItemView::SingleSelection);
  Plugins->setEditTriggers(QAbstractItemView::NoEditTriggers);
  Plugins->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  for (int Column = 1; Column < Plugins->columnCount(); ++Column)
    Plugins->header()->setSectionResizeMode(Column,
                                            QHeaderView::ResizeToContents);
  Details = new QPlainTextEdit(Splitter);
  Details->setObjectName(QStringLiteral("pluginManagerDetails"));
  Details->setAccessibleName(tr("Selected plugin details"));
  Details->setReadOnly(true);
  Details->setPlaceholderText(tr("Select a plugin to see its manifest, "
                                 "activation and diagnostics."));
  Splitter->setStretchFactor(0, 2);
  Splitter->setStretchFactor(1, 1);
  Layout->addWidget(Splitter, 1);
  Status = new QLabel(this);
  Status->setObjectName(QStringLiteral("pluginManagerStatus"));
  Status->setWordWrap(true);
  Status->setText(tr("Import a process-plugin manifest to add commands. "
                     "Provider programs start only when a command runs."));
  Layout->addWidget(Status);
  auto *Close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(Close, &QDialogButtonBox::rejected, this, &QDialog::reject);
  Layout->addWidget(Close);
  // Return in the filter would otherwise open the import chooser.
  leaveWithoutDefaultButton(*this);

  connect(Import, &QPushButton::clicked, this, [this] {
    const auto Path = QFileDialog::getOpenFileName(
        this, tr("Import process plugin"), {}, tr("Plugin manifests (*.json)"));
    if (Path.isEmpty())
      return;
    QString Error;
    if (!this->Services.installPlugin(Path, Error))
      Status->setText(Error);
    else
      Status->setText(tr("Plugin imported: %1").arg(Path));
  });
  connect(Toggle, &QPushButton::clicked, this, [this] {
    const auto Id = selectedId();
    for (const auto &Installation : this->Services.pluginInstallations())
      if (Installation.Id == Id) {
        changeEnabled(!Installation.Enabled);
        break;
      }
  });
  connect(Retry, &QPushButton::clicked, this, [this] { changeEnabled(true); });
  connect(Uninstall, &QPushButton::clicked, this, [this] {
    const auto Id = selectedId();
    if (Id.isEmpty())
      return;
    this->Services.uninstallPlugin(Id);
    Status->setText(this->Services.jobs().retiring(Id)
                        ? tr("Installation removed; active work is stopping.")
                        : tr("Installation removed. Plugin files and task "
                             "artifacts are kept."));
  });
  connect(Location, &QPushButton::clicked, this, [this] {
    const auto Id = selectedId();
    for (const auto &Installation : this->Services.pluginInstallations())
      if (Installation.Id == Id &&
          !QDesktopServices::openUrl(
              QUrl::fromLocalFile(QFileInfo(Installation.Path).absolutePath())))
        Status->setText(tr("Could not open the plugin folder."));
  });
  connect(Filter, &QLineEdit::textChanged, this,
          &PluginManagerDialog::filterPlugins);
  connect(Plugins, &QTreeWidget::currentItemChanged, this,
          &PluginManagerDialog::selectPlugin);
  connect(&Services, &PluginServices::pluginsChanged, this,
          &PluginManagerDialog::refresh);
  refresh();
}

QString PluginManagerDialog::selectedId() const {
  const auto *Item = Plugins->currentItem();
  return Item ? Item->data(0, Qt::UserRole).toString() : QString();
}

void PluginManagerDialog::refresh() {
  const auto Selected = selectedId();
  const QSignalBlocker Blocker(Plugins);
  Plugins->clear();
  for (const auto &Installation : Services.pluginInstallations()) {
    const auto State = Services.jobs().retiring(Installation.Id)
                           ? tr("Stopping")
                       : !Services.jobs().componentOwner(Installation.Id) &&
                               !Installation.Diagnostic.isEmpty()
                           ? tr("Unavailable")
                           : activationText(Services.jobs(), Installation.Id);
    auto *Item = new QTreeWidgetItem(
        Plugins, {Installation.Name, Installation.Version,
                  Installation.Enabled ? tr("Yes") : tr("No"), State});
    Item->setData(0, Qt::UserRole, Installation.Id);
    Item->setData(0, Qt::UserRole + 1, Installation.Path);
    Item->setToolTip(0,
                     Installation.Id + QLatin1Char('\n') + Installation.Path);
    if (Installation.Id == Selected)
      Plugins->setCurrentItem(Item);
  }
  if (!Plugins->currentItem() && Plugins->topLevelItemCount())
    Plugins->setCurrentItem(Plugins->topLevelItem(0));
  filterPlugins();
  selectPlugin();
}

void PluginManagerDialog::filterPlugins() {
  const auto Text = Filter->text().trimmed();
  for (int Row = 0; Row < Plugins->topLevelItemCount(); ++Row) {
    auto *Item = Plugins->topLevelItem(Row);
    const auto Search = Item->text(0) + QLatin1Char('\n') +
                        Item->data(0, Qt::UserRole).toString() +
                        QLatin1Char('\n') +
                        Item->data(0, Qt::UserRole + 1).toString();
    Item->setHidden(!Search.contains(Text, Qt::CaseInsensitive));
  }
}

void PluginManagerDialog::selectPlugin() {
  const auto Id = selectedId();
  Toggle->setEnabled(false);
  Retry->setEnabled(false);
  Uninstall->setEnabled(!Id.isEmpty());
  Location->setEnabled(!Id.isEmpty());
  Details->clear();
  for (const auto &Installation : Services.pluginInstallations()) {
    if (Installation.Id != Id)
      continue;
    auto &Jobs = Services.jobs();
    const bool Stopping = Jobs.retiring(Id);
    Uninstall->setEnabled(Installation.Installed);
    Toggle->setText(Installation.Enabled ? tr("Disable") : tr("Enable"));
    Toggle->setEnabled(Installation.Installed && !Stopping);
    Retry->setEnabled(Installation.Installed && Installation.Enabled &&
                      !Stopping && !Jobs.componentOwner(Id));
    QStringList Lines{tr("Plugin: %1").arg(Installation.Name),
                      tr("ID: %1").arg(Id),
                      tr("Version: %1").arg(Installation.Version),
                      tr("Type: External process"),
                      tr("Manifest: %1").arg(Installation.Path),
                      tr("Enabled on startup: %1")
                          .arg(Installation.Enabled ? tr("Yes") : tr("No")),
                      tr("Activation: %1").arg(activationText(Jobs, Id))};
    if (const auto Owner = Jobs.componentOwner(Id))
      if (const auto Snapshot = Jobs.host().snapshot(*Owner)) {
        Lines.append(tr("Component generation: %1").arg(Snapshot->Id));
        Lines.append(tr("Registrations: %1; active invocations: %2")
                         .arg(Snapshot->Registrations)
                         .arg(Snapshot->Invocations));
      }
    if (!Installation.Diagnostic.isEmpty())
      Lines.append(tr("Diagnostic: %1").arg(Installation.Diagnostic));
    if (Stopping)
      Lines.append(
          tr("Waiting for task exit and accepted publication cleanup."));
    if (const auto Descriptor = Jobs.plugin(Id))
      Lines.append(
          tr("Declared read services: %1").arg(Descriptor->queries.join(", ")));
    Details->setPlainText(Lines.join(QLatin1Char('\n')));
    break;
  }
}

void PluginManagerDialog::changeEnabled(bool Enabled) {
  const auto Id = selectedId();
  if (Id.isEmpty())
    return;
  QString Error;
  if (!Services.setPluginEnabled(Id, Enabled, Error))
    Status->setText(Error);
  else
    Status->setText(Enabled ? tr("Plugin enabled.")
                            : tr("Plugin disabled. Its installation is kept; "
                                 "active work is cancelled."));
}
} // namespace neverd::gui
