//===- PathfinderView.cpp - IDA's Pathfinder window -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "PathfinderView.h"

#include "../ResourceLimits.h"
#include "AddressSpace.h"
#include "ChooserView.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "ModelessDialog.h"
#include "Session.h"
#include "ShortcutHint.h"
#include "ShrinkableRow.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QShortcut>
#include <QSpinBox>
#include <QTabWidget>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace neverd::gui {
namespace {

enum Column { NameColumn, AddressColumn, ReferenceColumn, Columns };

/// What a row of the waypoint list stands for.
enum class Row { Waypoint, Segment, Path, Step };
enum class ListKey { Add, Remove, Up, Down, Copy };

QKeySequence listKey(ListKey command) {
  switch (command) {
  case ListKey::Add:
    return QKeySequence(Qt::Key_Insert);
  case ListKey::Remove:
    return QKeySequence(QKeySequence::Delete);
  case ListKey::Up:
    return QKeySequence(Qt::ALT | Qt::Key_Up);
  case ListKey::Down:
    return QKeySequence(Qt::ALT | Qt::Key_Down);
  case ListKey::Copy:
    return QKeySequence(QKeySequence::Copy);
  }
  return {};
}

QAction *showListKey(QAction *action, ListKey command) {
  action->setShortcut(listKey(command));
  action->setShortcutContext(Qt::WidgetShortcut);
  action->setShortcutVisibleInContextMenu(true);
  return action;
}

constexpr int RowRole = Qt::UserRole;
/// The function or import a row names.
constexpr int NodeRole = Qt::UserRole + 1;
/// The instruction that refers to a step's function or import.
constexpr int SiteRole = Qt::UserRole + 2;
/// A waypoint's place in the list, or the segment a path or step is in.
constexpr int IndexRole = Qt::UserRole + 3;

// The options, which this machine keeps for every program, as IDA does.
constexpr char DataRefsKey[] = "pathfinder/dataRefs";
constexpr char ShortestOnlyKey[] = "pathfinder/shortestOnly";
constexpr char MaxDepthKey[] = "pathfinder/maxDepth";

std::optional<Address> rowAddress(const QTreeWidgetItem *item, int role) {
  const QVariant value = item ? item->data(0, role) : QVariant();
  if (!value.isValid())
    return std::nullopt;
  return Address(value.toULongLong());
}

Row rowKind(const QTreeWidgetItem *item) {
  return Row(item ? item->data(0, RowRole).toInt() : int(Row::Segment));
}

QJsonArray addressArray(const QVector<Address> &addresses) {
  QJsonArray array;
  for (const Address address : addresses)
    array.append(hexAddress(address));
  return array;
}

QVector<Address> addressVector(const QJsonValue &value) {
  QVector<Address> addresses;
  for (const QJsonValue &item : value.toArray())
    if (const auto address = addressValue(item);
        address && !addresses.contains(*address))
      addresses.push_back(*address);
  return addresses;
}

} // namespace

PathfinderView::PathfinderView(Session &session, const AddressSpace &space,
                               QWidget *parent)
    : QWidget(parent), session_(session), space_(space) {
  auto *layout = new QVBoxLayout(this);
  layout->setContentsMargins(4, 4, 4, 4);

  auto *commands = new QWidget(this);
  auto *commandLayout = new QHBoxLayout(commands);
  commandLayout->setContentsMargins(0, 0, 0, 0);
  const auto commandButton = [&](const char *iconName, const QString &text,
                                 const QKeySequence &key, const char *name) {
    auto *button = new QToolButton(commands);
    button->setObjectName(QString::fromLatin1(name));
    button->setIcon(icon(QString::fromLatin1(iconName)));
    button->setToolTip(key.isEmpty()
                           ? text
                           : QStringLiteral("%1 (%2)").arg(
                                 text, key.toString(QKeySequence::NativeText)));
    setShortcutHint(*button, key);
    button->setAutoRaise(true);
    commandLayout->addWidget(button);
    return button;
  };
  add_ = commandButton("function_create", tr("Add a waypoint"),
                       listKey(ListKey::Add), "pathfinderAdd");
  exclude_ = commandButton("pathfinder_exclude", tr("Exclude a function"),
                           listKey(ListKey::Add), "pathfinderExclude");
  remove_ = commandButton("function_delete", tr("Remove"),
                          listKey(ListKey::Remove), "pathfinderRemove");
  up_ = commandButton("up", tr("Move the waypoint up"), listKey(ListKey::Up),
                      "pathfinderUp");
  down_ = commandButton("down", tr("Move the waypoint down"),
                        listKey(ListKey::Down), "pathfinderDown");
  clear_ = commandButton("output_clear", tr("Clear the waypoints"),
                         QKeySequence(), "pathfinderClear");
  commandLayout->addStretch(1);
  makeRowShrinkable(*commands);
  layout->addWidget(commands);

  waypointList_ = new QTreeWidget(this);
  waypointList_->setObjectName(QStringLiteral("pathfinderWaypoints"));
  waypointList_->setColumnCount(Columns);
  waypointList_->setHeaderLabels(
      {tr("Function"), tr("Address"), tr("Reference")});
  waypointList_->setUniformRowHeights(true);
  waypointList_->setAllColumnsShowFocus(true);
  waypointList_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  waypointList_->setContextMenuPolicy(Qt::CustomContextMenu);
  waypointList_->header()->setStretchLastSection(true);
  waypointList_->header()->setSectionResizeMode(NameColumn,
                                                QHeaderView::Interactive);
  activateOnReturn(*waypointList_);
  layout->addWidget(waypointList_, 1);

  // Two panels a click on their label opens and closes, as IDA's.
  const auto panelToggle = [&](const QString &text, const char *name) {
    auto *toggle = new QToolButton(this);
    toggle->setObjectName(QString::fromLatin1(name));
    toggle->setText(text);
    toggle->setCheckable(true);
    toggle->setProperty("glyphToggle", true);
    toggle->setAutoRaise(true);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setArrowType(Qt::RightArrow);
    layout->addWidget(toggle);
    return toggle;
  };
  exclusionsToggle_ =
      panelToggle(tr("Exclusions"), "pathfinderExclusionsShown");
  exclusionsPanel_ = new QWidget(this);
  auto *exclusionsLayout = new QVBoxLayout(exclusionsPanel_);
  exclusionsLayout->setContentsMargins(0, 0, 0, 0);
  exclusionList_ = new QTreeWidget(exclusionsPanel_);
  exclusionList_->setObjectName(QStringLiteral("pathfinderExclusions"));
  exclusionList_->setColumnCount(2);
  exclusionList_->setHeaderLabels({tr("Function"), tr("Address")});
  exclusionList_->setRootIsDecorated(false);
  exclusionList_->setUniformRowHeights(true);
  exclusionList_->setAllColumnsShowFocus(true);
  exclusionList_->setContextMenuPolicy(Qt::CustomContextMenu);
  exclusionList_->setMaximumHeight(120);
  activateOnReturn(*exclusionList_);
  exclusionsLayout->addWidget(exclusionList_);
  exclusionsPanel_->hide();
  layout->addWidget(exclusionsPanel_);

  optionsToggle_ = panelToggle(tr("Options"), "pathfinderOptionsShown");
  optionsPanel_ = new QWidget(this);
  auto *optionsLayout = new QHBoxLayout(optionsPanel_);
  optionsLayout->setContentsMargins(0, 0, 0, 0);
  QSettings settings;
  dataRefs_ = new QCheckBox(tr("Include data references"), optionsPanel_);
  dataRefs_->setObjectName(QStringLiteral("pathfinderDataRefs"));
  dataRefs_->setChecked(settings.value(DataRefsKey, false).toBool());
  shortestOnly_ = new QCheckBox(tr("Shortest paths only"), optionsPanel_);
  shortestOnly_->setObjectName(QStringLiteral("pathfinderShortestOnly"));
  shortestOnly_->setChecked(settings.value(ShortestOnlyKey, false).toBool());
  auto *depthLabel = new QLabel(tr("Maximum depth:"), optionsPanel_);
  maxDepth_ = new QSpinBox(optionsPanel_);
  maxDepth_->setObjectName(QStringLiteral("pathfinderMaxDepth"));
  maxDepth_->setRange(1, ::neverd::limits::gui::kMaxPathfinderDepth);
  maxDepth_->setValue(std::clamp(
      settings
          .value(MaxDepthKey, ::neverd::limits::gui::kDefaultPathfinderDepth)
          .toInt(),
      1, ::neverd::limits::gui::kMaxPathfinderDepth));
  depthLabel->setBuddy(maxDepth_);
  optionsLayout->addWidget(dataRefs_);
  optionsLayout->addWidget(shortestOnly_);
  optionsLayout->addWidget(depthLabel);
  optionsLayout->addWidget(maxDepth_);
  optionsLayout->addStretch(1);
  makeRowShrinkable(*optionsPanel_);
  optionsPanel_->hide();
  layout->addWidget(optionsPanel_);

  status_ = new QLabel(this);
  status_->setObjectName(QStringLiteral("pathfinderStatus"));
  makeStatusLine(*status_);
  layout->addWidget(status_);

  for (auto [toggle, panel] : {std::pair(exclusionsToggle_, exclusionsPanel_),
                               std::pair(optionsToggle_, optionsPanel_)})
    connect(toggle, &QToolButton::toggled, this,
            [toggle = toggle, panel = panel](bool shown) {
              toggle->setArrowType(shown ? Qt::DownArrow : Qt::RightArrow);
              panel->setVisible(shown);
            });
  connect(add_, &QToolButton::clicked, this, &PathfinderView::chooseWaypoint);
  connect(exclude_, &QToolButton::clicked, this,
          &PathfinderView::chooseExclusion);
  connect(remove_, &QToolButton::clicked, this, &PathfinderView::removeCurrent);
  connect(up_, &QToolButton::clicked, this, [this] { moveCurrent(-1); });
  connect(down_, &QToolButton::clicked, this, [this] { moveCurrent(1); });
  connect(clear_, &QToolButton::clicked, this, &PathfinderView::clearWaypoints);
  for (QCheckBox *option : {dataRefs_, shortestOnly_})
    connect(option, &QCheckBox::toggled, this, [this] {
      saveOptions();
      scheduleSearch();
    });
  connect(maxDepth_, &QSpinBox::valueChanged, this, [this] {
    saveOptions();
    scheduleSearch();
  });

  // IDA's keys of its list windows: Ins adds, Del removes; Alt+arrows move
  // a waypoint, and Ctrl+C copies the rows.
  const auto shortcut = [&](QWidget *owner, const QKeySequence &key,
                            std::function<void()> slot) {
    auto *keys = new QShortcut(key, owner);
    keys->setContext(Qt::WidgetWithChildrenShortcut);
    connect(keys, &QShortcut::activated, this, std::move(slot));
  };
  shortcut(waypointList_, listKey(ListKey::Add), [this] { chooseWaypoint(); });
  shortcut(exclusionList_, listKey(ListKey::Add),
           [this] { chooseExclusion(); });
  for (QWidget *list : {static_cast<QWidget *>(waypointList_),
                        static_cast<QWidget *>(exclusionList_)}) {
    shortcut(list, listKey(ListKey::Remove), [this] { removeCurrent(); });
    shortcut(list, listKey(ListKey::Copy), [this] {
      if (const QString text = selectedText(); !text.isEmpty())
        QApplication::clipboard()->setText(text);
    });
  }
  shortcut(waypointList_, listKey(ListKey::Up), [this] { moveCurrent(-1); });
  shortcut(waypointList_, listKey(ListKey::Down), [this] { moveCurrent(1); });

  connect(waypointList_, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *item) {
            // A step goes to the instruction that refers to its function.
            if (const auto site = rowAddress(item, SiteRole))
              emit activated(*site);
            else if (const auto node = rowAddress(item, NodeRole))
              emit activated(*node);
          });
  connect(exclusionList_, &QTreeWidget::itemActivated, this,
          [this](QTreeWidgetItem *item) {
            if (const auto node = rowAddress(item, NodeRole))
              emit activated(*node);
          });
  connect(waypointList_, &QTreeWidget::customContextMenuRequested, this,
          &PathfinderView::contextMenu);
  connect(exclusionList_, &QTreeWidget::customContextMenuRequested, this,
          [this](const QPoint &point) {
            QMenu menu(this);
            showListKey(
                menu.addAction(icon(QStringLiteral("pathfinder_exclude")),
                               tr("Exclude a function..."), this,
                               &PathfinderView::chooseExclusion),
                ListKey::Add);
            showListKey(menu.addAction(icon(QStringLiteral("function_delete")),
                                       tr("Remove"), this,
                                       &PathfinderView::removeCurrent),
                        ListKey::Remove)
                ->setEnabled(exclusionList_->currentItem() != nullptr);
            menu.exec(exclusionList_->viewport()->mapToGlobal(point));
          });
  for (QTreeWidget *list : {waypointList_, exclusionList_})
    connect(list, &QTreeWidget::currentItemChanged, this,
            &PathfinderView::updateButtons);

  debounce_.setSingleShot(true);
  debounce_.setInterval(::neverd::limits::gui::kPathfinderSearchDelayMs);
  connect(&debounce_, &QTimer::timeout, this, &PathfinderView::search);
  connect(&session_, &Session::opened, this, [this] { showStatus(); });
  // Renames, new functions and their references change the paths.
  connect(&session_, &Session::revisionChanged, this,
          &PathfinderView::scheduleSearch);
  connect(&session_, &Session::functionsChanged, this,
          &PathfinderView::scheduleSearch);
  connect(&session_, &Session::unloaded, this, &PathfinderView::unload);
  showWaypoints();
  showExclusions();
  showStatus();
}

void PathfinderView::startPath(Address address) {
  resolve(address, Role::Waypoint, true);
}

void PathfinderView::addWaypoint(Address address) {
  resolve(address, Role::Waypoint, false);
}

void PathfinderView::addExclusion(Address address) {
  resolve(address, Role::Exclusion, false);
}

void PathfinderView::resolve(Address address, Role role, bool restart) {
  if (!session_.loaded() || !session_.findsCallPaths())
    return;
  // An address inside a function stands for the function; an import's slot
  // or veneer for the import.
  session_.read(
      QStringLiteral("resolve"),
      {{QStringLiteral("query"), hexAddress(address)}}, this,
      [this, address, role, restart](const QJsonObject &payload) {
        const auto function =
            addressValue(payload.value(QStringLiteral("function_address")));
        const bool import = payload.value(QStringLiteral("import")).toBool();
        if (!function && !import) {
          error_ = tr("No function or import is at %1.")
                       .arg(displayAddress(address));
          showStatus();
          return;
        }
        const Address node = function ? *function : address;
        Node described;
        described.import = !function;
        if (node == addressValue(payload.value(QStringLiteral("address"))))
          described.name = payload.value(QStringLiteral("name")).toString();
        add(node, described, role, restart);
      },
      [this](const QString &, const QString &message) { failed(message); });
}

void PathfinderView::add(Address node, const Node &described, Role role,
                         bool restart) {
  if (!described.name.isEmpty() || !nodes_.contains(node))
    nodes_.insert(node, described);
  error_.clear();
  if (role == Role::Exclusion) {
    if (exclusions_.contains(node))
      return;
    exclusions_.push_back(node);
    showExclusions();
    exclusionsToggle_->setChecked(true);
  } else {
    QVector<Address> waypoints = restart ? QVector<Address>() : waypoints_;
    // A function is a waypoint once, as in IDA.
    if (waypoints.contains(node) ||
        waypoints.size() >= ::neverd::limits::gui::kMaxPathfinderWaypoints) {
      error_ = waypoints.contains(node)
                   ? tr("%1 is a waypoint already.").arg(label(node))
                   : tr("A path takes at most %n waypoints.", nullptr,
                        ::neverd::limits::gui::kMaxPathfinderWaypoints);
      showStatus();
      return;
    }
    waypoints.push_back(node);
    setWaypoints(std::move(waypoints));
    return;
  }
  emit changed();
  scheduleSearch();
}

void PathfinderView::choose(Role role) {
  if (!session_.loaded() || !session_.findsCallPaths())
    return;
  const auto epoch = session_.epoch();
  auto *dialog = new QDialog(this);
  dialog->setObjectName(QStringLiteral("pathfinderChooser"));
  dialog->setWindowTitle(role == Role::Waypoint ? tr("Add a Waypoint")
                                                : tr("Exclude a Function"));
  dialog->setWindowIcon(icon(QStringLiteral("pathfinder")));
  dialog->resize(640, 420);
  auto *layout = new QVBoxLayout(dialog);
  auto *tabs = new QTabWidget(dialog);
  auto *functions =
      new ChooserView(session_, space_, ChooserKind::Functions, tabs);
  auto *imports = new ChooserView(session_, space_, ChooserKind::Imports, tabs);
  tabs->addTab(functions, icon(QStringLiteral("functions")), tr("Functions"));
  tabs->addTab(imports, icon(QStringLiteral("imports")), tr("Imports"));
  layout->addWidget(tabs, 1);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  buttons->button(QDialogButtonBox::Ok)
      ->setText(role == Role::Waypoint ? tr("Add") : tr("Exclude"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  // These lists are created after the session's initial load notification.
  for (ChooserView *view : {functions, imports}) {
    view->model().reload();
    connect(view, &ChooserView::activated, dialog, &QDialog::accept);
  }
  connect(dialog, &QDialog::accepted, this, [this, tabs, role, epoch] {
    if (!session_.loaded() || session_.epoch() != epoch)
      return;
    const auto chosen =
        qobject_cast<ChooserView *>(tabs->currentWidget())->currentAddress();
    if (!chosen)
      return;
    if (role == Role::Waypoint)
      addWaypoint(*chosen);
    else
      addExclusion(*chosen);
  });
  showModelessDialog(dialog, this, &session_);
  functions->focusFilter();
}

void PathfinderView::chooseWaypoint() { choose(Role::Waypoint); }

void PathfinderView::chooseExclusion() { choose(Role::Exclusion); }

void PathfinderView::removeCurrent() {
  if (exclusionList_->hasFocus() ||
      (exclusionList_->currentItem() && !waypointList_->hasFocus() &&
       !waypointList_->currentItem())) {
    if (const auto node = rowAddress(exclusionList_->currentItem(), NodeRole)) {
      exclusions_.removeAll(*node);
      showExclusions();
      emit changed();
      scheduleSearch();
    }
    return;
  }
  QTreeWidgetItem *item = waypointList_->currentItem();
  if (rowKind(item) != Row::Waypoint)
    return;
  QVector<Address> waypoints = waypoints_;
  waypoints.remove(item->data(0, IndexRole).toInt());
  setWaypoints(std::move(waypoints));
}

void PathfinderView::moveCurrent(int by) {
  QTreeWidgetItem *item = waypointList_->currentItem();
  if (rowKind(item) != Row::Waypoint)
    return;
  const int from = item->data(0, IndexRole).toInt(), to = from + by;
  if (to < 0 || to >= waypoints_.size())
    return;
  QVector<Address> waypoints = waypoints_;
  waypoints.move(from, to);
  const Address moved = waypoints[to];
  setWaypoints(std::move(waypoints));
  for (int row = 0; row < waypointList_->topLevelItemCount(); ++row)
    if (QTreeWidgetItem *waypoint = waypointList_->topLevelItem(row);
        rowKind(waypoint) == Row::Waypoint &&
        rowAddress(waypoint, NodeRole) == moved)
      waypointList_->setCurrentItem(waypoint);
}

void PathfinderView::clearWaypoints() {
  if (!waypoints_.isEmpty())
    setWaypoints({});
}

void PathfinderView::setWaypoints(QVector<Address> waypoints) {
  waypoints_ = std::move(waypoints);
  // The segments were between the waypoints there were.
  segments_ = {};
  unconnected_ = 0;
  showWaypoints();
  emit changed();
  scheduleSearch();
}

void PathfinderView::scheduleSearch() {
  if (waypoints_.size() < 2 || !session_.loaded() ||
      !session_.findsCallPaths()) {
    debounce_.stop();
    ++serial_;
    searching_ = false;
    segments_ = {};
    showWaypoints();
    showStatus();
    return;
  }
  searching_ = true;
  showStatus();
  debounce_.start();
}

void PathfinderView::search() {
  if (waypoints_.size() < 2 || !session_.loaded())
    return;
  QJsonObject request{
      {QStringLiteral("waypoints"), addressArray(waypoints_)},
      {QStringLiteral("max_depth"), maxDepth_->value()},
      {QStringLiteral("shortest_only"), shortestOnly_->isChecked()},
      {QStringLiteral("data_refs"), dataRefs_->isChecked()}};
  if (!exclusions_.isEmpty())
    request.insert(QStringLiteral("exclude"), addressArray(exclusions_));
  const quint64 serial = ++serial_;
  session_.read(
      QStringLiteral("call_paths"), request, this,
      [this, serial](const QJsonObject &payload) {
        if (serial == serial_)
          found(payload);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == serial_)
          failed(message);
      });
}

void PathfinderView::found(const QJsonObject &payload) {
  searching_ = false;
  error_.clear();
  for (const QJsonValue &value :
       payload.value(QStringLiteral("nodes")).toArray()) {
    const QJsonObject node = value.toObject();
    if (const auto address =
            addressValue(node.value(QStringLiteral("address"))))
      nodes_.insert(*address, {node.value(QStringLiteral("name")).toString(),
                               node.value(QStringLiteral("kind")).toString() ==
                                   QLatin1String("import")});
  }
  segments_ = payload.value(QStringLiteral("segments")).toArray();
  unconnected_ = payload.value(QStringLiteral("unconnected")).toInt();
  showWaypoints();
  showExclusions();
  showStatus();
}

void PathfinderView::failed(const QString &message) {
  searching_ = false;
  segments_ = {};
  error_ = tr("The call paths could not be found: %1").arg(message);
  showWaypoints();
  showStatus();
}

QString PathfinderView::label(Address address) const {
  const auto node = nodes_.find(address);
  return node != nodes_.end() && !node->name.isEmpty()
             ? node->name
             : displayAddress(address);
}

QTreeWidgetItem *PathfinderView::segmentItem(const QJsonObject &segment,
                                             int index) {
  auto *item = new QTreeWidgetItem;
  item->setData(0, RowRole, int(Row::Segment));
  item->setData(0, IndexRole, index);
  const QJsonValue shortest = segment.value(QStringLiteral("shortest"));
  const bool reversed = segment.value(QStringLiteral("reversed")).toBool();
  const QString arrow = reversed ? QStringLiteral("↑ ") : QStringLiteral("↓ ");
  if (shortest.isNull() || shortest.isUndefined()) {
    item->setText(NameColumn, arrow + tr("No path"));
    item->setToolTip(NameColumn,
                     tr("Neither waypoint reaches the other in %n references.",
                        nullptr, maxDepth_->value()));
    return item;
  }
  const QJsonArray paths = segment.value(QStringLiteral("paths")).toArray();
  const int nodes = segment.value(QStringLiteral("node_count")).toInt();
  QString text;
  if (shortest.toInt() == 1 && paths.size() == 1)
    text = reversed ? tr("Direct reference, the other way round")
                    : tr("Direct reference");
  else
    text = (reversed ? tr("%n nodes (shortest: %1), the other way round",
                          nullptr, nodes)
                     : tr("%n nodes (shortest: %1)", nullptr, nodes))
               .arg(shortest.toInt());
  if (!segment.value(QStringLiteral("complete")).toBool(true))
    text += QLatin1Char(' ') + tr("(more paths than listed)");
  item->setText(NameColumn, arrow + text);
  // Each reference between two nodes, by the pair: its kind and the first
  // instruction that makes it.
  QHash<QPair<Address, Address>, QPair<QString, Address>> references;
  for (const QJsonValue &value :
       segment.value(QStringLiteral("edges")).toArray()) {
    const QJsonArray edge = value.toArray();
    const auto from = addressValue(edge.at(0)), to = addressValue(edge.at(1));
    const auto site = addressValue(edge.at(3));
    if (from && to)
      references.insert({*from, *to},
                        {edge.at(2).toString(), site.value_or(*to)});
  }
  const auto referenceText = [](const QString &kind, Address site) {
    const QString at = displayAddress(site);
    if (kind == QLatin1String("jump"))
      return tr("jump at %1").arg(at);
    if (kind == QLatin1String("indirect"))
      return tr("call through a slot at %1").arg(at);
    if (kind == QLatin1String("data"))
      return tr("address taken at %1").arg(at);
    return tr("call at %1").arg(at);
  };
  int number = 0;
  for (const QJsonValue &value : paths) {
    const QJsonArray path = value.toArray();
    QStringList names;
    for (const QJsonValue &step : path)
      if (const auto address = addressValue(step))
        names << label(*address);
    auto *pathItem = new QTreeWidgetItem(item);
    pathItem->setData(0, RowRole, int(Row::Path));
    pathItem->setData(0, IndexRole, index);
    if (const auto first = addressValue(path.at(0)))
      pathItem->setData(0, NodeRole, *first);
    pathItem->setText(
        NameColumn,
        tr("Path %1: %2").arg(++number).arg(names.join(QStringLiteral(" → "))));
    pathItem->setToolTip(NameColumn, pathItem->text(NameColumn));
    for (int at = 1; at < path.size(); ++at) {
      const auto from = addressValue(path.at(at - 1));
      const auto to = addressValue(path.at(at));
      if (!from || !to)
        continue;
      auto *step = new QTreeWidgetItem(pathItem);
      step->setData(0, RowRole, int(Row::Step));
      step->setData(0, IndexRole, index);
      step->setData(0, NodeRole, *to);
      step->setText(NameColumn, label(*to));
      step->setText(AddressColumn, displayAddress(*to));
      if (const auto reference = references.find({*from, *to});
          reference != references.end()) {
        step->setData(0, SiteRole, reference->second);
        step->setText(ReferenceColumn,
                      referenceText(reference->first, reference->second));
      }
      if (nodes_.value(*to).import)
        step->setIcon(NameColumn, icon(QStringLiteral("imports")));
    }
  }
  return item;
}

void PathfinderView::showWaypoints() {
  const auto current = rowAddress(waypointList_->currentItem(), NodeRole);
  waypointList_->clear();
  for (int index = 0; index < waypoints_.size(); ++index) {
    const Address node = waypoints_[index];
    auto *item = new QTreeWidgetItem(waypointList_);
    item->setData(0, RowRole, int(Row::Waypoint));
    item->setData(0, NodeRole, node);
    item->setData(0, IndexRole, index);
    item->setText(NameColumn, label(node));
    item->setText(AddressColumn, displayAddress(node));
    item->setIcon(NameColumn, icon(nodes_.value(node).import
                                       ? QStringLiteral("imports")
                                       : QStringLiteral("functions")));
    QFont bold = item->font(NameColumn);
    bold.setBold(true);
    item->setFont(NameColumn, bold);
    if (current == node)
      waypointList_->setCurrentItem(item);
    if (index + 1 < waypoints_.size() && index < segments_.size()) {
      QTreeWidgetItem *segment =
          segmentItem(segments_.at(index).toObject(), index);
      waypointList_->addTopLevelItem(segment);
      segment->setExpanded(true);
    }
  }
  waypointList_->resizeColumnToContents(AddressColumn);
  updateButtons();
}

void PathfinderView::showExclusions() {
  const auto current = rowAddress(exclusionList_->currentItem(), NodeRole);
  exclusionList_->clear();
  for (const Address node : exclusions_) {
    auto *item = new QTreeWidgetItem(exclusionList_);
    item->setData(0, NodeRole, node);
    item->setText(0, label(node));
    item->setText(1, displayAddress(node));
    if (current == node)
      exclusionList_->setCurrentItem(item);
  }
  exclusionsToggle_->setText(
      exclusions_.isEmpty() ? tr("Exclusions")
                            : tr("Exclusions (%1)").arg(exclusions_.size()));
  updateButtons();
}

void PathfinderView::showStatus() {
  QString text;
  if (!session_.loaded())
    text = tr("Open a program to find the call paths between its functions.");
  else if (!session_.findsCallPaths())
    text = tr("This engine does not find call paths.");
  else if (!error_.isEmpty())
    text = error_;
  else if (waypoints_.size() < 2)
    text = tr("Add two waypoints or more to find the call paths between "
              "them: %1 adds the function under the cursor.")
               .arg(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_F9)
                        .toString(QKeySequence::NativeText));
  else if (searching_)
    text = tr("Finding call paths...");
  else if (!segments_.isEmpty()) {
    QSet<QString> nodes;
    bool complete = true;
    for (const QJsonValue &value : segments_) {
      const QJsonObject segment = value.toObject();
      complete &= segment.value(QStringLiteral("complete")).toBool(true);
      for (const QJsonValue &node :
           segment.value(QStringLiteral("nodes")).toArray())
        nodes.insert(node.toString());
    }
    if (unconnected_ == segments_.size())
      text = tr("No path found.");
    else if (unconnected_)
      text =
          tr("%1 of %n segments have no path.", nullptr, int(segments_.size()))
              .arg(unconnected_);
    else
      text = tr("Found %n nodes on the paths.", nullptr, int(nodes.size()));
    if (!complete)
      text += QLatin1Char(' ') +
              tr("Not every path is listed: lower the maximum depth or "
                 "exclude functions.");
  }
  status_->setText(text);
  updateButtons();
}

void PathfinderView::updateButtons() {
  const bool usable = session_.loaded() && session_.findsCallPaths();
  QTreeWidgetItem *item = waypointList_->currentItem();
  const bool waypoint = rowKind(item) == Row::Waypoint;
  const int index = waypoint ? item->data(0, IndexRole).toInt() : -1;
  add_->setEnabled(usable);
  exclude_->setEnabled(usable);
  remove_->setEnabled(usable && (waypoint || exclusionList_->currentItem()));
  up_->setEnabled(usable && waypoint && index > 0);
  down_->setEnabled(usable && waypoint && index + 1 < waypoints_.size());
  clear_->setEnabled(usable && !waypoints_.isEmpty());
}

void PathfinderView::saveOptions() const {
  QSettings settings;
  settings.setValue(DataRefsKey, dataRefs_->isChecked());
  settings.setValue(ShortestOnlyKey, shortestOnly_->isChecked());
  settings.setValue(MaxDepthKey, maxDepth_->value());
}

void PathfinderView::contextMenu(const QPoint &point) {
  QTreeWidgetItem *item = waypointList_->itemAt(point);
  if (item)
    waypointList_->setCurrentItem(item);
  const bool usable = session_.loaded() && session_.findsCallPaths();
  QMenu menu(this);
  showListKey(menu.addAction(icon(QStringLiteral("function_create")),
                             tr("Add a waypoint..."), this,
                             &PathfinderView::chooseWaypoint),
              ListKey::Add)
      ->setEnabled(usable);
  menu.addAction(icon(QStringLiteral("pathfinder_exclude")),
                 tr("Exclude a function..."), this,
                 &PathfinderView::chooseExclusion)
      ->setEnabled(usable);
  // A function on a path joins the waypoints between its segment's two, or
  // leaves every path, as IDA's graph offers.
  if (const auto node = rowAddress(item, NodeRole);
      node && rowKind(item) == Row::Step) {
    menu.addSeparator();
    const int segment = item->data(0, IndexRole).toInt();
    menu.addAction(icon(QStringLiteral("pathfinder")),
                   tr("Promote to waypoint"), this,
                   [this, node = *node, segment] {
                     if (waypoints_.contains(node) ||
                         waypoints_.size() >=
                             ::neverd::limits::gui::kMaxPathfinderWaypoints)
                       return;
                     QVector<Address> waypoints = waypoints_;
                     waypoints.insert(segment + 1, node);
                     setWaypoints(std::move(waypoints));
                   })
        ->setEnabled(!waypoints_.contains(*node));
    menu.addAction(icon(QStringLiteral("pathfinder_exclude")),
                   tr("Exclude from paths"), this,
                   [this, node = *node] {
                     add(node, nodes_.value(node), Role::Exclusion, false);
                   })
        ->setEnabled(!exclusions_.contains(*node));
  }
  if (rowKind(item) == Row::Waypoint) {
    menu.addSeparator();
    showListKey(menu.addAction(up_->icon(), tr("Move up"), this,
                               [this] { moveCurrent(-1); }),
                ListKey::Up)
        ->setEnabled(up_->isEnabled());
    showListKey(menu.addAction(down_->icon(), tr("Move down"), this,
                               [this] { moveCurrent(1); }),
                ListKey::Down)
        ->setEnabled(down_->isEnabled());
    showListKey(menu.addAction(remove_->icon(), tr("Remove"), this,
                               &PathfinderView::removeCurrent),
                ListKey::Remove);
  }
  menu.addSeparator();
  showListKey(menu.addAction(icon(QStringLiteral("copy")), tr("Copy"), this,
                             [this] {
                               if (const QString text = selectedText();
                                   !text.isEmpty())
                                 QApplication::clipboard()->setText(text);
                             }),
              ListKey::Copy)
      ->setEnabled(!waypointList_->selectedItems().isEmpty());
  menu.addAction(clear_->icon(), tr("Clear the waypoints"), this,
                 &PathfinderView::clearWaypoints)
      ->setEnabled(clear_->isEnabled());
  menu.exec(waypointList_->viewport()->mapToGlobal(point));
}

QString PathfinderView::selectedText() const {
  QTreeWidget *list =
      exclusionList_->hasFocus() ? exclusionList_ : waypointList_;
  QStringList lines;
  // Rows in their order down the list, a path's steps under it.
  std::function<void(QTreeWidgetItem *)> visit = [&](QTreeWidgetItem *item) {
    if (item->isSelected()) {
      QStringList columns;
      for (int column = 0; column < list->columnCount(); ++column)
        columns << item->text(column);
      while (!columns.isEmpty() && columns.back().isEmpty())
        columns.removeLast();
      lines << columns.join(QLatin1Char('\t'));
    }
    for (int child = 0; child < item->childCount(); ++child)
      visit(item->child(child));
  };
  for (int row = 0; row < list->topLevelItemCount(); ++row)
    visit(list->topLevelItem(row));
  return lines.join(QLatin1Char('\n'));
}

QJsonObject PathfinderView::state() const {
  QJsonObject state;
  if (!waypoints_.isEmpty())
    state.insert(QStringLiteral("waypoints"), addressArray(waypoints_));
  if (!exclusions_.isEmpty())
    state.insert(QStringLiteral("exclude"), addressArray(exclusions_));
  return state;
}

void PathfinderView::restoreState(const QJsonObject &state) {
  exclusions_ = addressVector(state.value(QStringLiteral("exclude")));
  waypoints_ = addressVector(state.value(QStringLiteral("waypoints")));
  if (waypoints_.size() > ::neverd::limits::gui::kMaxPathfinderWaypoints)
    waypoints_.resize(::neverd::limits::gui::kMaxPathfinderWaypoints);
  segments_ = {};
  error_.clear();
  showExclusions();
  showWaypoints();
  scheduleSearch();
}

void PathfinderView::unload() {
  // The waypoints are the program's; the project keeps them.
  debounce_.stop();
  ++serial_;
  waypoints_.clear();
  exclusions_.clear();
  nodes_.clear();
  segments_ = {};
  unconnected_ = 0;
  searching_ = false;
  error_.clear();
  showWaypoints();
  showExclusions();
  showStatus();
}

} // namespace neverd::gui
