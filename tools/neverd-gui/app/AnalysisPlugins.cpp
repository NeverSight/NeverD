//===- AnalysisPlugins.cpp - External plugin workbench bridge --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin workbench bridge.
///
//===----------------------------------------------------------------------===//

#include "AnalysisPlugins.h"

#include "../ResourceLimits.h"
#include "ActionRegistry.h"
#include "FlowLayout.h"
#include "Icons.h"
#include "KeepTranslated.h"
#include "PluginServices.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGraphicsPathItem>
#include <QGraphicsPolygonItem>
#include <QGraphicsRectItem>
#include <QGraphicsScene>
#include <QGraphicsTextItem>
#include <QGraphicsView>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace neverd::gui {
using namespace ::neverd::limits::gui::plugin;
using namespace ::neverd::plugin::limits;
namespace {
constexpr int TaskRole = Qt::UserRole;
constexpr int PluginRole = Qt::UserRole + 1;

QString menuPreference(const QString &plugin, const QString &action) {
  return QStringLiteral("guiPlugins/") + plugin + QStringLiteral("/menus/") +
         action + QLatin1Char('/');
}

QIcon presentationIcon(const QString &value, const QString &manifestPath) {
  if (value.startsWith(QLatin1String("builtin:")))
    return icon(value.mid(8));
  if (value.isEmpty())
    return {};
  return iconFromFile(QFileInfo(manifestPath).dir().absoluteFilePath(value));
}

class ResultGraph final : public QGraphicsView {
public:
  std::function<void(const QString &)> activate;
  explicit ResultGraph(QWidget *parent) : QGraphicsView(parent) {
    setDragMode(QGraphicsView::ScrollHandDrag);
    setAccessibleName(tr("Plugin result graph"));
  }

protected:
  void mouseDoubleClickEvent(QMouseEvent *event) override {
    auto *item = itemAt(event->position().toPoint());
    while (item && !item->data(0).isValid())
      item = item->parentItem();
    if (item && activate)
      activate(item->data(0).toString());
    QGraphicsView::mouseDoubleClickEvent(event);
  }
  void keyPressEvent(QKeyEvent *event) override {
    if (event->key() == Qt::Key_Return && scene() &&
        !scene()->selectedItems().isEmpty()) {
      if (activate)
        activate(scene()->selectedItems().first()->data(0).toString());
      return;
    }
    if (event->key() == Qt::Key_Plus) {
      scale(1.2, 1.2);
      return;
    }
    if (event->key() == Qt::Key_Minus) {
      scale(1 / 1.2, 1 / 1.2);
      return;
    }
    QGraphicsView::keyPressEvent(event);
  }
};
} // namespace

AnalysisPlugins::AnalysisPlugins(Session &session, ActionRegistry &actions,
                                 QWidget *parent)
    : QWidget(parent), session_(session), actions_(actions),
      services_(session.pluginServices()), jobs_(services_.jobs()),
      menu_(new QMenu(this)) {
  setObjectName(QStringLiteral("analysisPlugins"));
  menu_->setObjectName(QStringLiteral("processPluginsMenu"));
  auto *layout = new QVBoxLayout(this);
  auto *management = new QWidget(this);
  management->setObjectName(QStringLiteral("pluginManagement"));
  auto *buttons = new FlowLayout(management);
  auto *import = new QPushButton(management);
  auto *manage = new QPushButton(management);
  manage->setObjectName(QStringLiteral("pluginOpenManager"));
  auto *menus = new QPushButton(management);
  menus->setObjectName(QStringLiteral("pluginMenuSettings"));
  buttons->addWidget(import);
  buttons->addWidget(manage);
  buttons->addWidget(menus);
  connect(menus, &QPushButton::clicked, this, &AnalysisPlugins::configureMenus);
  layout->addWidget(management);

  auto *splitter = new QSplitter(Qt::Vertical, this);
  tasks_ = new QTreeWidget(splitter);
  tasks_->setObjectName(QStringLiteral("pluginTasks"));
  tasks_->setColumnCount(4);
  tasks_->setRootIsDecorated(true);
  tasks_->setUniformRowHeights(true);
  tasks_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Ignored);
  tasks_->setMinimumHeight(tasks_->header()->sizeHint().height() +
                           kTaskTreeMinimumRows *
                               tasks_->fontMetrics().height());
  tasks_->header()->setSectionResizeMode(0, QHeaderView::Interactive);
  tasks_->setColumnWidth(0, kTaskTreeNameInitialWidth);
  for (int column = 1; column < tasks_->columnCount(); ++column)
    tasks_->header()->setSectionResizeMode(column,
                                           QHeaderView::ResizeToContents);

  auto *results = new QWidget(splitter);
  results->setObjectName(QStringLiteral("pluginTaskResultPane"));
  auto *resultLayout = new QVBoxLayout(results);
  resultLayout->setContentsMargins(0, 0, 0, 0);
  resultTitle_ = new QLabel(results);
  resultTitle_->setObjectName(QStringLiteral("pluginTaskTitle"));
  resultTitle_->setTextFormat(Qt::PlainText);
  resultTitle_->setWordWrap(true);
  auto titleFont = resultTitle_->font();
  titleFont.setBold(true);
  resultTitle_->setFont(titleFont);
  resultLayout->addWidget(resultTitle_);
  resultOwner_ = new QLabel(results);
  resultOwner_->setObjectName(QStringLiteral("pluginTaskOwner"));
  resultOwner_->setTextFormat(Qt::PlainText);
  resultOwner_->setWordWrap(true);
  makeStatusLine(*resultOwner_);
  resultLayout->addWidget(resultOwner_);
  resultActions_ = new QWidget(results);
  resultActions_->setObjectName(QStringLiteral("pluginTaskActions"));
  auto *resultButtons = new FlowLayout(resultActions_);
  resultLayout->addWidget(resultActions_);
  cancel_ = new QPushButton(resultActions_);
  export_ = new QPushButton(resultActions_);
  importImage_ = new QPushButton(resultActions_);
  importImage_->setObjectName(QStringLiteral("pluginImportImage"));
  connect(importImage_, &QPushButton::clicked, this,
          &AnalysisPlugins::importImageAddition);
  showLog_ = new QPushButton(resultActions_);
  showLog_->setObjectName(QStringLiteral("pluginShowLog"));
  showLog_->setCheckable(true);
  resultButtons->addWidget(cancel_);
  resultButtons->addWidget(export_);
  resultButtons->addWidget(importImage_);
  resultButtons->addWidget(showLog_);
  status_ = new QLabel(tr("Import a process plugin to add commands."), results);
  status_->setWordWrap(true);
  status_->setTextFormat(Qt::PlainText);
  makeStatusLine(*status_);
  resultLayout->addWidget(status_);
  progress_ = new QProgressBar(results);
  resultLayout->addWidget(progress_);
  views_ = new QTabWidget(results);
  views_->setObjectName(QStringLiteral("pluginResultViews"));
  resultLayout->addWidget(views_, 1);
  log_ = new QPlainTextEdit(splitter);
  log_->setObjectName(QStringLiteral("pluginTaskLog"));
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(kMaxLogRows);
  log_->setAccessibleName(tr("Plugin task log"));
  log_->setFont(Theme::instance().codeFont());
  log_->hide();
  connect(showLog_, &QPushButton::toggled, this, [this] { showResult(); });
  splitter->setStretchFactor(0, 0);
  splitter->setStretchFactor(1, 1);
  splitter->setStretchFactor(2, 0);
  splitter->setSizes({kTaskTreeInitialHeight, 360, 80});
  splitter->setCollapsible(1, false);
  results->setMinimumHeight(18 * fontMetrics().lineSpacing());
  layout->addWidget(splitter, 1);
  connect(import, &QPushButton::clicked, this, &AnalysisPlugins::importPlugin);
  connect(manage, &QPushButton::clicked, this,
          [this] { actions_.action(ActionId::OptionsPlugins)->trigger(); });
  connect(cancel_, &QPushButton::clicked, this, [this] {
    if (auto *item = tasks_->currentItem())
      jobs_.cancel(item->data(0, TaskRole).toString());
  });
  connect(export_, &QPushButton::clicked, this, [this] {
    auto *item = tasks_->currentItem();
    const auto job =
        item ? jobs_.job(item->data(0, TaskRole).toString()) : std::nullopt;
    if (!job)
      return;
    QStringList titles;
    for (const auto &value : job->result.value("artifacts").toArray())
      titles.append(value.toObject().value("title").toString());
    bool ok = false;
    const auto title = QInputDialog::getItem(
        this, tr("Export artifact"), tr("Artifact:"), titles, 0, false, &ok);
    if (!ok)
      return;
    const auto destination =
        QFileDialog::getSaveFileName(this, tr("Export artifact to a new file"));
    if (destination.isEmpty())
      return;
    QString error;
    if (!jobs_.exportArtifact(job->id, titles.indexOf(title), destination,
                              error))
      emit message(error, 2);
    else
      emit message(tr("Plugin artifact exported to %1").arg(destination), 0);
  });
  connect(tasks_, &QTreeWidget::currentItemChanged, this,
          &AnalysisPlugins::showResult);
  connect(&services_, &PluginServices::pluginsChanged, this,
          &AnalysisPlugins::rebuildActions);
  connect(&jobs_, &PluginJobs::jobsChanged, this,
          &AnalysisPlugins::refreshJobs);
  connect(&jobs_, &PluginJobs::logMessage, this,
          [this](const QString &task, const QString &text, int) {
            auto &lines = taskLogs_[task];
            lines.append(text);
            while (lines.size() > kMaxLogRows)
              lines.removeFirst();
            if (const auto *item = tasks_->currentItem();
                item && item->data(0, TaskRole).toString() == task)
              log_->appendPlainText(text);
          });
  // PluginServices refreshes the context on session changes.
  connect(&services_, &PluginServices::contextChanged, this,
          &AnalysisPlugins::updateActions);
  connect(&services_, &PluginServices::message, this,
          &AnalysisPlugins::message);
  connect(&services_, &PluginServices::navigateRequested, this,
          &AnalysisPlugins::navigateRequested);
  connect(&services_, &PluginServices::changed, this,
          [this](const QString &task) {
            showResult();
            const auto *item = tasks_->currentItem();
            if (item && item->data(0, TaskRole).toString() == task)
              status_->setText(services_.receipt(task));
          });
  connect(qApp, &QApplication::focusChanged, this, [this] {
    QTimer::singleShot(0, this, &AnalysisPlugins::refreshContext);
  });
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    log_->setFont(Theme::instance().codeFont());
    displayed_.clear();
    showResult();
  });
  auto *timer = new QTimer(this);
  timer->setInterval(kRefreshIntervalMs);
  connect(timer, &QTimer::timeout, this, &AnalysisPlugins::refreshJobs);
  timer->start();
  restorePlugins();
  rebuildActions();
  refreshContext();
  refreshJobs();
  keepTranslated(*this, [this, import, manage, menus] {
    menu_->setTitle(tr("Plu&gins"));
    import->setText(tr("&Import plugin…"));
    manage->setText(tr("Plugin manager…"));
    menus->setText(tr("Menu settings…"));
    tasks_->setAccessibleName(tr("Tasks grouped by plugin"));
    tasks_->setHeaderLabels(
        {tr("Plugin / task"), tr("State"), tr("Progress"), tr("Elapsed")});
    cancel_->setText(tr("&Cancel task"));
    export_->setText(tr("&Export artifact…"));
    importImage_->setText(tr("Import code into image"));
    importImage_->setToolTip(tr("Add recovered code as an analysis section, "
                                "then open its first function"));
    showLog_->setText(tr("Show &log"));
    progress_->setAccessibleName(tr("Selected plugin task progress"));
    // The theme's change after a new language words the selected task again.
  });
}
bool AnalysisPlugins::installPlugin(const QString &path, QString &error) {
  return services_.installPlugin(path, error);
}

void AnalysisPlugins::uninstallPlugin(const QString &id) {
  services_.uninstallPlugin(id);
}

void AnalysisPlugins::restorePlugins() { services_.restorePlugins(); }

void AnalysisPlugins::setSelectionProvider(
    std::function<QJsonObject()> provider) {
  services_.setSelectionProvider(std::move(provider));
}
void AnalysisPlugins::refreshContext() { services_.refreshContext(); }
void AnalysisPlugins::updateActions() {
  const auto context = jobs_.context();
  for (auto *action : registered_) {
    const auto required = action->property("pluginContext").toString();
    action->setEnabled(
        required == QLatin1String("none") ||
        (session_.loaded() &&
         (required == QLatin1String("project") ||
          context
              .value(required == QLatin1String("function") ? "function_address"
                                                           : "address")
              .isString())));
  }
}
void AnalysisPlugins::importPlugin() {
  const auto path = QFileDialog::getOpenFileName(
      this, tr("Import process plugin"), {}, tr("Plugin manifests (*.json)"));
  if (path.isEmpty())
    return;
  QString error;
  if (!installPlugin(path, error))
    emit message(error, 2);
  else
    emit message(tr("Process plugin imported: %1").arg(path), 0);
}
void AnalysisPlugins::rebuildActions() {
  for (auto *action : registered_) {
    actions_.unregisterExternalAction(action);
    delete action;
  }
  registered_.clear();
  menu_->clear();
  menu_->addAction(actions_.action(ActionId::OptionsPlugins));
  auto *import = menu_->addAction(tr("Import process plugin…"));
  connect(import, &QAction::triggered, this, &AnalysisPlugins::importPlugin);
  auto *tasks = menu_->addAction(tr("Plugin tasks and results"));
  tasks->setObjectName(QStringLiteral("pluginShowTasks"));
  connect(tasks, &QAction::triggered, this, &AnalysisPlugins::showRequested);
  auto *settings = menu_->addAction(tr("Context menu settings…"));
  settings->setObjectName(QStringLiteral("configurePluginMenus"));
  connect(settings, &QAction::triggered, this,
          &AnalysisPlugins::configureMenus);
  menu_->addSeparator();
  auto plugins = jobs_.plugins();
  std::sort(plugins.begin(), plugins.end(),
            [](const auto &left, const auto &right) {
              const auto order =
                  QString::compare(left.name, right.name, Qt::CaseInsensitive);
              return order ? order < 0 : left.id < right.id;
            });
  for (const auto &plugin : plugins) {
    auto displayName = plugin.name;
    if (std::count_if(plugins.begin(), plugins.end(), [&](const auto &other) {
          return other.name == plugin.name;
        }) > 1)
      displayName += QStringLiteral(" (%1)").arg(plugin.id);
    auto *submenu = menu_->addMenu(displayName);
    submenu->setIcon(presentationIcon(plugin.icon, plugin.path));
    for (const auto &value : plugin.actions) {
      const auto definition = value.toObject();
      auto *action = new QAction(definition.value("title").toString(), this);
      action->setObjectName(QStringLiteral("plugin.") + plugin.id +
                            QLatin1Char(':') +
                            definition.value("id").toString());
      action->setProperty("pluginContext",
                          definition.value("context").toString());
      action->setProperty("pluginContextMenu",
                          definition.value("context_menu").toBool());
      action->setProperty("pluginId", plugin.id);
      action->setProperty("pluginName", displayName);
      action->setProperty("pluginManifest", plugin.path);
      action->setProperty("pluginActionId", definition.value("id").toString());
      action->setProperty("pluginDefaultIcon",
                          definition.value("icon").toString());
      action->setStatusTip(plugin.name + QStringLiteral(" / ") +
                           action->text());
      connect(action, &QAction::triggered, this,
              [this, id = plugin.id, definition] { invoke(id, definition); });
      submenu->addAction(action);
      registered_.append(action);
      actions_.registerExternalAction(action);
    }
  }
  applyPresentation();
  refreshContext();
}
QList<QAction *> AnalysisPlugins::contextActions() const {
  QList<QAction *> result;
  for (auto *action : registered_)
    if (action->property("pluginContextMenu").toBool())
      result.append(action);
  return result;
}

void AnalysisPlugins::applyPresentation() {
  QSettings settings;
  for (auto *action : registered_) {
    const auto prefix =
        menuPreference(action->property("pluginId").toString(),
                       action->property("pluginActionId").toString());
    const auto value = settings
                           .value(prefix + QStringLiteral("icon"),
                                  action->property("pluginDefaultIcon"))
                           .toString();
    const auto image =
        presentationIcon(value, action->property("pluginManifest").toString());
    action->setIcon(image);
    action->setIconVisibleInMenu(true);
    if (!value.isEmpty() && image.isNull())
      emit message(tr("Plugin icon is unavailable: %1").arg(value), 1);
  }
}

void AnalysisPlugins::populateContextMenu(QMenu &menu) {
  refreshContext();
  const auto commands = contextActions();
  if (commands.isEmpty())
    return;
  QSettings settings;
  QMenu *plugins = nullptr;
  QHash<QString, QMenu *> groups;
  for (auto *action : commands) {
    const auto pluginId = action->property("pluginId").toString();
    const auto prefix =
        menuPreference(pluginId, action->property("pluginActionId").toString());
    if (settings.value(prefix + QStringLiteral("placement")).toString() ==
        QLatin1String("inline")) {
      menu.addAction(action);
      continue;
    }
    if (!plugins) {
      plugins = menu.addMenu(icon(QStringLiteral("extensions")), tr("Plugins"));
      plugins->setObjectName(QStringLiteral("pluginContextRoot"));
    }
    auto *group = groups.value(pluginId);
    if (!group) {
      group = plugins->addMenu(action->property("pluginName").toString());
      group->setObjectName(QStringLiteral("pluginContextGroup.") + pluginId);
      if (const auto descriptor = jobs_.plugin(pluginId))
        group->setIcon(presentationIcon(descriptor->icon, descriptor->path));
      groups.insert(pluginId, group);
    }
    group->addAction(action);
  }
  menu.addSeparator();
}

void AnalysisPlugins::configureMenus() {
  QDialog dialog(this);
  dialog.setObjectName(QStringLiteral("pluginMenuSettingsDialog"));
  dialog.setWindowTitle(tr("Plugin context menus"));
  dialog.resize(kMenuSettingsWidth, kMenuSettingsHeight);
  auto *layout = new QVBoxLayout(&dialog);
  auto *description = new QLabel(
      tr("Choose which commands appear directly in the context menu. Other "
         "commands are grouped under Plugins → plugin name. Icons accept "
         "builtin:name or a local PNG, ICO or SVG file."),
      &dialog);
  description->setWordWrap(true);
  layout->addWidget(description);
  auto *table = new QTableWidget(&dialog);
  table->setObjectName(QStringLiteral("pluginMenuSettingsTable"));
  table->setColumnCount(4);
  table->setHorizontalHeaderLabels(
      {tr("Plugin"), tr("Command"), tr("Position"), tr("Icon")});
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(
      QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Stretch);
  layout->addWidget(table, 1);
  struct Row {
    QString prefix, manifest, defaultIcon;
    QComboBox *placement;
    QLineEdit *image;
  };
  QList<Row> rows;
  QSettings settings;
  for (auto *action : contextActions()) {
    const auto plugin = action->property("pluginId").toString();
    const auto id = action->property("pluginActionId").toString();
    const auto prefix = menuPreference(plugin, id);
    const auto index = table->rowCount();
    table->insertRow(index);
    for (int column = 0; column < 2; ++column) {
      auto *item = new QTableWidgetItem(
          column == 0 ? action->property("pluginName").toString()
                      : action->text());
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      item->setToolTip(plugin + QLatin1Char('/') + id);
      table->setItem(index, column, item);
    }
    auto *placement = new QComboBox(table);
    placement->setObjectName(QStringLiteral("pluginMenuPlacement.") + plugin +
                             QLatin1Char(':') + id);
    placement->addItem(tr("Plugins submenu"), QStringLiteral("grouped"));
    placement->addItem(tr("Direct context menu"), QStringLiteral("inline"));
    placement->setCurrentIndex(
        settings.value(prefix + QStringLiteral("placement")).toString() ==
                QLatin1String("inline")
            ? 1
            : 0);
    placement->setAccessibleName(action->text() + tr(" position"));
    table->setCellWidget(index, 2, placement);
    auto *imageCell = new QWidget(table);
    auto *imageLayout = new QHBoxLayout(imageCell);
    imageLayout->setContentsMargins(0, 0, 0, 0);
    const auto defaultIcon = action->property("pluginDefaultIcon").toString();
    auto *image = new QLineEdit(
        settings.value(prefix + QStringLiteral("icon"), defaultIcon).toString(),
        imageCell);
    image->setObjectName(QStringLiteral("pluginMenuIcon.") + plugin +
                         QLatin1Char(':') + id);
    image->setMaxLength(kMaxPathCharacters);
    image->setAccessibleName(action->text() + tr(" icon"));
    auto *browse = new QPushButton(tr("Browse…"), imageCell);
    connect(browse, &QPushButton::clicked, &dialog, [&dialog, image] {
      const auto path =
          QFileDialog::getOpenFileName(&dialog, tr("Choose command icon"), {},
                                       tr("Icons (*.png *.ico *.svg)"));
      if (!path.isEmpty())
        image->setText(path);
    });
    imageLayout->addWidget(image, 1);
    imageLayout->addWidget(browse);
    table->setCellWidget(index, 3, imageCell);
    rows.append({prefix, action->property("pluginManifest").toString(),
                 defaultIcon, placement, image});
  }
  auto *error = new QLabel(&dialog);
  error->setObjectName(QStringLiteral("pluginMenuSettingsError"));
  error->setTextFormat(Qt::PlainText);
  error->setWordWrap(true);
  layout->addWidget(error);
  for (const auto &row : rows)
    connect(row.image, &QLineEdit::textChanged, error, &QLabel::clear);
  auto *buttons =
      new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel |
                               QDialogButtonBox::RestoreDefaults,
                           &dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults),
          &QPushButton::clicked, &dialog, [&rows, error] {
            for (const auto &row : rows) {
              row.placement->setCurrentIndex(0);
              row.image->setText(row.defaultIcon);
            }
            error->clear();
          });
  connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
    for (const auto &row : rows)
      if (row.image->text() != row.defaultIcon &&
          !row.image->text().isEmpty() &&
          presentationIcon(row.image->text(), row.manifest).isNull()) {
        error->setText(tr("Icon is unavailable: %1").arg(row.image->text()));
        return;
      }
    dialog.accept();
  });
  if (dialog.exec() != QDialog::Accepted)
    return;
  for (const auto &row : rows) {
    if (row.placement->currentData().toString() == QLatin1String("grouped"))
      settings.remove(row.prefix + QStringLiteral("placement"));
    else
      settings.setValue(row.prefix + QStringLiteral("placement"),
                        QStringLiteral("inline"));
    if (row.image->text() == row.defaultIcon)
      settings.remove(row.prefix + QStringLiteral("icon"));
    else
      settings.setValue(row.prefix + QStringLiteral("icon"), row.image->text());
  }
  applyPresentation();
}

void AnalysisPlugins::invoke(const QString &plugin, const QJsonObject &action) {
  refreshContext();
  const auto binding =
      jobs_.actionBinding(plugin, action.value("id").toString());
  if (!binding) {
    emit message(tr("This action registration is no longer active."), 1);
    return;
  }
  const auto captured = jobs_.context();
  const auto fields = action.value("parameters").toArray();
  QJsonObject parameters;
  QSettings settings;
  const auto prefix = QStringLiteral("guiPlugins/") + plugin +
                      QLatin1Char('/') + action.value("id").toString() +
                      QLatin1Char('/');
  QJsonObject preferences;
  for (const auto &value : fields) {
    const auto field = value.toObject();
    const auto id = field.value("id").toString();
    if (!settings.contains(prefix + id))
      continue;
    // INI files and the registry return remembered booleans and integers as
    // text after a restart; decode them by the declared type.
    const auto stored = settings.value(prefix + id);
    const auto type = field.value("type").toString();
    const auto text = stored.toString();
    QJsonValue remembered = text;
    bool decoded = true;
    if (type == QLatin1String("boolean")) {
      decoded = text == QLatin1String("true") || text == QLatin1String("false");
      remembered = text == QLatin1String("true");
    } else if (type == QLatin1String("integer"))
      remembered = stored.toLongLong(&decoded);
    // The dialog shows the default instead of a remembered value that the
    // current declaration rejects.
    QString rejected;
    if (decoded && resolvePluginParameters(QJsonArray{field},
                                           QJsonObject{{id, remembered}}, {},
                                           {}, rejected))
      preferences.insert(id, remembered);
  }
  QString configurationError;
  const auto effective =
      resolvePluginParameters(fields, preferences, {}, {}, configurationError);
  if (!effective) {
    emit message(configurationError, 2);
    return;
  }
  QDialog dialog(this);
  dialog.setWindowTitle(action.value("title").toString());
  auto *form = new QFormLayout(&dialog);
  QList<std::pair<QJsonObject, QWidget *>> inputs;
  for (const auto &value : fields) {
    const auto field = value.toObject();
    const auto id = field.value("id").toString();
    const auto type = field.value("type").toString();
    const auto initial = effective->value(id);
    QWidget *input;
    if (type == QLatin1String("boolean")) {
      auto *box = new QCheckBox(&dialog);
      box->setChecked(initial.toBool());
      input = box;
    } else if (type == QLatin1String("integer")) {
      auto *spin = new QSpinBox(&dialog);
      spin->setRange(field.value("minimum").toInt(0),
                     field.value("maximum").toInt(kDefaultParameterMaximum));
      spin->setValue(initial.toInt());
      input = spin;
    } else if (type == QLatin1String("choice")) {
      auto *combo = new QComboBox(&dialog);
      for (const auto &choice : field.value("choices").toArray())
        combo->addItem(choice.toString());
      combo->setCurrentText(initial.toString());
      input = combo;
    } else {
      auto *line = new QLineEdit(initial.toString(), &dialog);
      line->setMaxLength(kMaxParameterCharacters);
      input = line;
    }
    input->setObjectName(QStringLiteral("pluginParameter.") + id);
    input->setAccessibleName(field.value("title").toString());
    form->addRow(field.value("title").toString(), input);
    inputs.append({field, input});
    if (auto *label = qobject_cast<QLabel *>(form->labelForField(input)))
      label->setTextFormat(Qt::PlainText);
  }
  QCheckBox *remember = nullptr;
  if (!fields.isEmpty()) {
    remember =
        new QCheckBox(tr("Remember these values for this action"), &dialog);
    remember->setObjectName(QStringLiteral("pluginRememberParameters"));
    form->addRow(remember);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    form->addRow(buttons);
    if (dialog.exec() != QDialog::Accepted)
      return;
  }
  for (const auto &[field, input] : inputs) {
    QJsonValue value;
    if (auto *box = qobject_cast<QCheckBox *>(input))
      value = box->isChecked();
    else if (auto *spin = qobject_cast<QSpinBox *>(input))
      value = spin->value();
    else if (auto *combo = qobject_cast<QComboBox *>(input))
      value = combo->currentText();
    else
      value = qobject_cast<QLineEdit *>(input)->text();
    parameters.insert(field.value("id").toString(), value);
  }
  refreshContext();
  if (captured.value("epoch") != jobs_.context().value("epoch")) {
    emit message(tr("Project changed while the plugin dialog was open"), 1);
    return;
  }
  // The action uses the selection captured before opening the parameter dialog.
  jobs_.setContext(captured);
  QString error;
  const auto task = jobs_.start(plugin, action.value("id").toString(),
                                parameters, error, binding);
  if (task.isEmpty())
    emit message(error, 2);
  else {
    if (remember && remember->isChecked())
      for (auto it = parameters.begin(); it != parameters.end(); ++it)
        settings.setValue(prefix + it.key(), it.value().toVariant());
    emit showRequested();
    if (auto *item = taskItem(task)) {
      item->parent()->setExpanded(true);
      tasks_->setCurrentItem(item);
      tasks_->scrollToItem(item);
    }
  }
}

QTreeWidgetItem *AnalysisPlugins::taskItem(const QString &id) const {
  for (int row = 0; row < tasks_->topLevelItemCount(); ++row) {
    auto *group = tasks_->topLevelItem(row);
    for (int child = 0; child < group->childCount(); ++child)
      if (group->child(child)->data(0, TaskRole).toString() == id)
        return group->child(child);
  }
  return nullptr;
}

void AnalysisPlugins::refreshJobs() {
  const auto jobs = jobs_.jobs();
  QSet<QString> ids;
  {
    const QSignalBlocker block(tasks_);
    for (const auto &job : jobs) {
      ids.insert(job.id);
      auto *item = taskItem(job.id);
      if (!item) {
        QTreeWidgetItem *group = nullptr;
        for (int row = 0; row < tasks_->topLevelItemCount(); ++row)
          if (tasks_->topLevelItem(row)->data(0, PluginRole).toString() ==
              job.plugin)
            group = tasks_->topLevelItem(row);
        if (!group) {
          group = new QTreeWidgetItem(tasks_);
          group->setData(0, PluginRole, job.plugin);
          group->setIcon(0, icon(QStringLiteral("extensions")));
          group->setExpanded(true);
        }
        group->setText(0, tr("%1 [%2]").arg(job.pluginName, job.plugin));
        item = new QTreeWidgetItem(group);
        item->setData(0, TaskRole, job.id);
      }
      item->setText(0, tr("%1 · %2").arg(job.actionTitle, job.id.left(8)));
      item->setToolTip(0, job.title + QLatin1Char('\n') + job.id);
      item->setText(1, job.state);
      auto progress = job.progress.value("label").toString();
      if (job.progress.value("total").isDouble())
        progress += tr(" (%1/%2)")
                        .arg(job.progress.value("done").toInt())
                        .arg(job.progress.value("total").toInt());
      item->setText(2, progress);
      item->setText(3,
                    tr("%1 s").arg((job.terminal()        ? job.elapsedMs
                                    : job.clock.isValid() ? job.clock.elapsed()
                                                          : 0) /
                                       1000.0,
                                   0, 'f', 1));
    }
    for (int row = tasks_->topLevelItemCount() - 1; row >= 0; --row) {
      auto *group = tasks_->topLevelItem(row);
      for (int child = group->childCount() - 1; child >= 0; --child)
        if (!ids.contains(group->child(child)->data(0, TaskRole).toString()))
          delete group->takeChild(child);
      if (!group->childCount())
        delete tasks_->takeTopLevelItem(row);
    }
    if (!tasks_->currentItem() && tasks_->topLevelItemCount())
      tasks_->setCurrentItem(tasks_->topLevelItem(0)->child(0));
  }
  for (const auto &id : taskLogs_.keys())
    if (!ids.contains(id))
      taskLogs_.remove(id);
  showResult();
}
void AnalysisPlugins::activateAddress(const QString &task,
                                      const QString &address) {
  const auto job = jobs_.job(task);
  const auto parsed = addressValue(address);
  if (job &&
      job->context.value("epoch").toString() ==
          QString::number(session_.epoch()) &&
      session_.loaded() && parsed)
    emit navigateRequested(*parsed);
}
void AnalysisPlugins::importImageAddition() {
  if (const auto *item = tasks_->currentItem())
    services_.importImage(item->data(0, TaskRole).toString());
}

void AnalysisPlugins::showResult() {
  auto *item = tasks_->currentItem();
  const auto job =
      item ? jobs_.job(item->data(0, TaskRole).toString()) : std::nullopt;
  resultTitle_->setText(job ? job->actionTitle : tr("Task results"));
  resultOwner_->setVisible(job.has_value());
  if (job)
    resultOwner_->setText(
        tr("%1 [%2] · Task %3")
            .arg(job->pluginName, job->plugin, job->id.left(8)));
  resultActions_->setVisible(job.has_value());
  cancel_->setVisible(job && !job->terminal());
  cancel_->setEnabled(job && !job->terminal());
  progress_->setVisible(job && !job->terminal());
  if (job && !job->terminal()) {
    const auto total = job->progress.value("total").toInt(0);
    progress_->setRange(0, total);
    progress_->setValue(job->progress.value("done").toInt());
  }
  export_->setEnabled(job && job->state == QLatin1String("succeeded") &&
                      !job->result.value("artifacts").toArray().isEmpty());
  export_->setVisible(export_->isEnabled());
  importImage_->setVisible(job && job->result.contains("image_addition"));
  importImage_->setEnabled(
      job && job->state == QLatin1String("succeeded") &&
      job->result.contains("image_addition") && !services_.pending(job->id) &&
      job->context.value("epoch").toString() ==
          QString::number(session_.epoch()) &&
      job->context.value("revision").toString() == session_.revision() &&
      !session_.metadata().value("read_only").toBool());
  const auto key =
      job ? job->id + QLatin1Char('/') + job->state
          : QStringLiteral("plugin/") +
                (item ? item->data(0, PluginRole).toString() : QString());
  log_->setVisible(job && showLog_->isChecked());
  if (log_->property("taskKey").toString() != key) {
    log_->setProperty("taskKey", key);
    log_->setPlainText(job ? taskLogs_.value(job->id).join(QLatin1Char('\n'))
                           : QString());
    log_->setAccessibleName(job ? tr("Log for %1").arg(job->title)
                                : tr("Plugin task log"));
  }
  if (displayed_ == key)
    return;
  displayed_ = key;
  while (views_->count()) {
    auto *widget = views_->widget(0);
    views_->removeTab(0);
    delete widget;
  }
  if (!job) {
    status_->setText(
        item ? tr("Select a task under %1 to inspect its result.")
                   .arg(item->text(0))
             : tr("Run a plugin command to see its tasks and results."));
    return;
  }
  status_->setText(job->state + (job->diagnostic.isEmpty()
                                     ? QString()
                                     : QStringLiteral(": ") + job->diagnostic));
  if (job->state != QLatin1String("succeeded"))
    return;
  for (const auto &value : job->result.value("views").toArray()) {
    const auto view = value.toObject();
    const auto kind = view.value("kind").toString();
    QWidget *widget = nullptr;
    if (kind == QLatin1String("text")) {
      auto *text = new QPlainTextEdit(views_);
      text->setReadOnly(true);
      text->setFont(Theme::instance().codeFont());
      text->setPlainText(view.value("text").toString());
      widget = text;
    } else if (kind == QLatin1String("table")) {
      const auto columns = view.value("columns").toArray();
      const auto rows = view.value("rows").toArray();
      auto *table = new QTableWidget(rows.size(), columns.size(), views_);
      QStringList titles;
      for (const auto &column : columns)
        titles.append(column.toString());
      table->setHorizontalHeaderLabels(titles);
      table->setEditTriggers(QAbstractItemView::NoEditTriggers);
      table->setSelectionBehavior(QAbstractItemView::SelectRows);
      table->setSortingEnabled(false);
      for (int row = 0; row < rows.size(); ++row) {
        const auto cells = rows[row].toObject().value("cells").toArray();
        for (int column = 0; column < cells.size(); ++column) {
          auto *cell = new QTableWidgetItem(cells[column].toString());
          cell->setData(TaskRole,
                        rows[row].toObject().value("address").toString());
          table->setItem(row, column, cell);
        }
      }
      table->setSortingEnabled(true);
      table->horizontalHeader()->setStretchLastSection(true);
      connect(table, &QTableWidget::itemActivated, this,
              [this, task = job->id](QTableWidgetItem *item) {
                activateAddress(task, item->data(TaskRole).toString());
              });
      widget = table;
    } else if (kind == QLatin1String("graph")) {
      auto *graph = new ResultGraph(views_);
      auto *scene = new QGraphicsScene(graph);
      graph->setScene(scene);
      QHash<QString, QPointF> positions;
      qreal height = 110;
      for (const auto &nodeValue : view.value("nodes").toArray()) {
        QGraphicsTextItem measure;
        measure.setFont(Theme::instance().codeFont());
        measure.setTextWidth(230);
        measure.setPlainText(nodeValue.toObject().value("text").toString());
        height = std::max(height, measure.boundingRect().height() + 8);
      }
      int index = 0;
      for (const auto &nodeValue : view.value("nodes").toArray()) {
        const auto node = nodeValue.toObject();
        const QPointF position((index % 3) * 280, (index / 3) * (height + 50));
        ++index;
        positions.insert(node.value("id").toString(),
                         position + QPointF(120, height / 2));
        auto *box = scene->addRect(QRectF(position, QSizeF(240, height)),
                                   QPen(palette().color(QPalette::Mid)),
                                   palette().brush(QPalette::Base));
        box->setFlag(QGraphicsItem::ItemIsSelectable);
        box->setData(0, node.value("address").toString());
        auto *text = new QGraphicsTextItem(box);
        text->setFont(Theme::instance().codeFont());
        text->setPlainText(node.value("text").toString());
        text->setDefaultTextColor(palette().color(QPalette::Text));
        text->setTextWidth(230);
        text->setPos(position + QPointF(4, 4));
      }
      for (const auto &edgeValue : view.value("edges").toArray()) {
        const auto edge = edgeValue.toObject();
        const auto from = positions.value(edge.value("from").toString());
        const auto to = positions.value(edge.value("to").toString());
        const auto color = palette().color(QPalette::Highlight);
        QPointF tip, direction;
        if (from == to) {
          const auto start = from + QPointF(-25, -height / 2);
          tip = from + QPointF(25, -height / 2);
          direction = QPointF(0, 1);
          QPainterPath path(start);
          path.cubicTo(start + QPointF(0, -35), tip + QPointF(0, -35), tip);
          scene->addPath(path, QPen(color, 2))->setZValue(-1);
        } else {
          const auto delta = to - from;
          direction = delta / std::hypot(delta.x(), delta.y());
          const auto border =
              std::min(120 / std::max(std::abs(delta.x()), 0.001),
                       (height / 2) / std::max(std::abs(delta.y()), 0.001));
          tip = to - delta * border;
          scene->addLine(QLineF(from, tip), QPen(color, 2))->setZValue(-1);
        }
        const QPointF normal(-direction.y(), direction.x());
        scene->addPolygon(QPolygonF{tip, tip - direction * 10 + normal * 5,
                                    tip - direction * 10 - normal * 5},
                          QPen(color), QBrush(color));
      }
      graph->activate = [this, task = job->id](const QString &address) {
        activateAddress(task, address);
      };
      widget = graph;
    }
    if (widget) {
      widget->setAccessibleName(view.value("title").toString());
      views_->addTab(widget, view.value("title").toString());
    }
  }
}
} // namespace neverd::gui
