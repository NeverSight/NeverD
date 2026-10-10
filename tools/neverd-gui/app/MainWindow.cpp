#include "MainWindow.h"

#include "ChooserView.h"
#include "CodeView.h"
#include "ConnectionsDialog.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "ExtensionsView.h"
#include "GraphView.h"
#include "HexView.h"
#include "Icons.h"
#include "JumpDialog.h"
#include "Language.h"
#include "ListingView.h"
#include "LoadFileDialog.h"
#include "NavigationBand.h"
#include "OutputWindow.h"
#include "ProjectDatabase.h"
#include "QuickStartDialog.h"
#include "Resolve.h"
#include "SecondaryTextDelegate.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTextStream>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <kddockwidgets/Config.h>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/core/DockWidget.h>

namespace neverd::gui {
namespace {
using KDDockWidgets::InitialOption;

constexpr char LayoutFileName[] = "desktop.json";
constexpr char OpcodeBytesKey[] = "listing/opcodeBytes";
constexpr char AnalysisIndicatorKey[] = "analysis/indicator";
constexpr char BinaryProcessorKey[] = "load/binaryProcessor";
// String search defaults and bounds (the worker's string_options).
constexpr int DefaultStringMinLength = 4;
constexpr int MaxStringMinLength = 1024;
constexpr char DocumentationUrl[] =
    "https://github.com/NeverSight/NeverD/blob/dev/docs/gui.md";
constexpr int FunctionsWidth = 380;
constexpr int OutputHeight = 190;
// Default proportions of the side and bottom windows, clamped in pixels.
constexpr double FunctionsShare = 0.21;
constexpr int MinimumFunctionsWidth = 240, MaximumFunctionsWidth = 420;
constexpr double OutputShare = 0.19;
constexpr int MinimumOutputHeight = 120, MaximumOutputHeight = 260;
constexpr int OverviewHeight = 170;
constexpr int StatusRefreshMs = 30000;
constexpr int MaxOpcodeBytes = 12;
// Input dialogs leave room for a full name, comment or expression.
constexpr int InputDialogWidth = 420;

// Dock identifiers; titles follow the classic window names.
constexpr char FunctionsDock[] = "functions";
constexpr char DisassemblyDock[] = "disassembly-a";
constexpr char HexDock[] = "hex-1";
constexpr char OutputDock[] = "output";
constexpr char OverviewDock[] = "graph-overview";
constexpr char PseudocodeDock[] = "pseudocode-a";
constexpr char RepresentationDock[] = "ir-a";
constexpr char ExtensionsDock[] = "extensions";

// Workbench state keys inside a NeverD database.
constexpr char LocationState[] = "location";
constexpr char BookmarksState[] = "bookmarks";
constexpr char DesktopState[] = "desktop";

QString chooserDockId(ChooserKind kind) {
  return QStringLiteral("chooser-%1").arg(int(kind));
}

/// Cross references in a modal chooser, like a classic xrefs dialog.
class ReferencesDialog final : public QDialog {
public:
  ReferencesDialog(Session &session, const AddressSpace &space,
                   const QString &title, const QJsonObject &request,
                   QWidget *parent)
      : QDialog(parent),
        view_(new ChooserView(session, space, ChooserKind::CrossReferences,
                              this)) {
    setWindowTitle(title);
    setWindowIcon(icon(QStringLiteral("xrefs")));
    resize(760, 360);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view_);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    // The list runs to the dialog's edges; the buttons keep the usual margin.
    auto *footer = new QVBoxLayout;
    const int margin = style()->pixelMetric(QStyle::PM_LayoutRightMargin);
    footer->setContentsMargins(margin, 0, margin, margin);
    footer->addWidget(buttons);
    layout->addLayout(footer);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
      chosen_ = view_->currentAddress();
      accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(view_, &ChooserView::activated, this, [this](Address address) {
      chosen_ = address;
      accept();
    });
    view_->model().setRequest(request);
    view_->table()->setFocus();
  }
  std::optional<Address> chosen() const { return chosen_; }

private:
  ChooserView *view_;
  std::optional<Address> chosen_;
};
} // namespace

MainWindow::MainWindow(Session &session, McpConnectionManager &mcp,
                       GuiSessionBroker &broker)
    : KDDockWidgets::QtWidgets::MainWindow(QStringLiteral("NeverDMainWindow")),
      session_(session), mcp_(mcp), broker_(broker), actions_(this) {
  instance_ = this;
  setWindowIcon(icon(QStringLiteral("app")));
  setCenterWidgetMargins(dockAreaMargins());
  setAcceptDrops(true);
  // Mouse Back/Forward buttons walk the navigation history anywhere inside.
  qApp->installEventFilter(this);
  buildMenusAndToolbars();
  buildDocks();
  buildStatusBar();
  connectSession();
  connectActions();
  statusTimer_.setInterval(StatusRefreshMs);
  connect(&statusTimer_, &QTimer::timeout, this, &MainWindow::updateStatusBar);
  statusTimer_.start();
  updateActions();
  updateTitle();
  updateStatusBar();
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
  // The quick start shows what a drop would do while a file is over it.
  if (event->type() == QEvent::DragLeave || event->type() == QEvent::Drop)
    if (auto *start = qobject_cast<QuickStartDialog *>(quickStart_.data())) {
      auto *widget = qobject_cast<QWidget *>(object);
      if (widget && (widget == start || start->isAncestorOf(widget)))
        start->showDropTarget(false);
    }
  if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove ||
      event->type() == QEvent::Drop) {
    auto *widget = qobject_cast<QWidget *>(object);
    auto *drop = static_cast<QDropEvent *>(event);
    const bool quickStart =
        widget && quickStart_ &&
        (widget == quickStart_ || quickStart_->isAncestorOf(widget));
    bool workbench = widget && (widget->window() == this || quickStart);
    // Floating docks belong to this session, but have their own window.
    if (widget && !workbench)
      for (auto *dock : std::as_const(docks_))
        if (widget == dock || dock->isAncestorOf(widget)) {
          workbench = true;
          break;
        }
    auto *modal = QApplication::activeModalWidget();
    if (workbench && (!modal || (quickStart && modal == quickStart_)) &&
        drop->mimeData()->hasUrls()) {
      const auto urls = drop->mimeData()->urls();
      const auto path = urls.size() == 1 && urls.first().isLocalFile()
                            ? urls.first().toLocalFile()
                            : QString();
      // One session holds one file. Opening must never tell the source to
      // move/delete it, including when Shift proposes a move operation.
      if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isFile() ||
          !(drop->possibleActions() & (Qt::CopyAction | Qt::LinkAction))) {
        drop->ignore();
        return true;
      }
      drop->setDropAction(drop->possibleActions() & Qt::CopyAction
                              ? Qt::CopyAction
                              : Qt::LinkAction);
      drop->accept();
      if (event->type() != QEvent::Drop && quickStart)
        if (auto *start = qobject_cast<QuickStartDialog *>(quickStart_.data()))
          start->showDropTarget(true);
      if (event->type() == QEvent::Drop) {
        startupQuickStartPending_ = false;
        // Leave the native drop callback before opening or asking about
        // unsaved changes. The window owns the queued callback's lifetime.
        const QPointer<QDialog> start = quickStart ? quickStart_ : nullptr;
        QTimer::singleShot(0, this, [this, path, start] {
          if (start)
            start->reject();
          chooseLoader(path);
        });
      }
      return true;
    }
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto *widget = qobject_cast<QWidget *>(object);
    const auto button = static_cast<QMouseEvent *>(event)->button();
    if (widget && widget->window() == this &&
        (button == Qt::BackButton || button == Qt::ForwardButton)) {
      actions_
          .action(button == Qt::BackButton ? ActionId::JumpBack
                                           : ActionId::JumpForward)
          ->trigger();
      return true;
    }
  }
  if (event->type() == QEvent::Show)
    if (auto *input = qobject_cast<QInputDialog *>(object))
      input->setMinimumWidth(InputDialogWidth);
  if (auto *code = qobject_cast<CodeView *>(object)) {
    // Reopening resumes this pane's interrupted function, independently of
    // the assembly cursor. Hidden panes do not keep analysis jobs running.
    if (event->type() == QEvent::Show) {
      const QPointer<CodeView> view = code;
      QTimer::singleShot(0, this, [view] {
        if (view && view->isVisible() && view->text()->interrupted())
          if (const auto function = view->text()->function())
            view->showFunction(*function);
      });
    }
    if (event->type() == QEvent::Hide)
      code->text()->cancel();
  }
  return KDDockWidgets::QtWidgets::MainWindow::eventFilter(object, event);
}

MainWindow::~MainWindow() {
  qApp->removeEventFilter(this);
  // Child views are destroyed after this body, and their teardown releases
  // queries and moves the focus; keep the resulting notifications away from
  // this window.
  disconnect(&session_, nullptr, this, nullptr);
  disconnect(&broker_, nullptr, this, nullptr);
  disconnect(qApp, &QApplication::focusChanged, this, nullptr);
  // Docked windows are children of this window; closed and floating ones are
  // not, and would keep their unique names registered after it is gone.
  for (auto *dock : std::as_const(docks_))
    if (!isAncestorOf(dock))
      delete dock;
  docks_.clear();
  statusTimer_.stop();
  if (instance_ == this)
    instance_ = nullptr;
}

QString MainWindow::layoutPath() const {
  if (!layoutPath_.isEmpty())
    return layoutPath_;
  const auto directory =
      QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
  QDir().mkpath(directory);
  return directory + QLatin1Char('/') + LayoutFileName;
}

//===----------------------------------------------------------------------===//
// Construction
//===----------------------------------------------------------------------===//

void MainWindow::buildMenusAndToolbars() {
  actions_.buildMenus(menuBar(), [this](QMenu *menu, const QString &name) {
    fillPlaceholder(menu, name);
  });
  const auto toolbars = actions_.buildToolbars(this);
  // The navigation band spans its own row under the toolbars.
  addToolBarBreak(Qt::TopToolBarArea);
  auto *bandBar = new QToolBar(tr("Navigation band"), this);
  bandBar->setObjectName(QStringLiteral("toolbarNavigationBand"));
  bandBar->setMovable(true);
  navigationBand_ = new NavigationBand(session_, space_, bandBar);
  bandBar->addWidget(navigationBand_);
  addToolBar(Qt::TopToolBarArea, bandBar);
  connect(navigationBand_, &NavigationBand::navigateRequested, this,
          [this](Address address) { navigate(address); });
  connect(navigationBand_, &NavigationBand::spaceChanged, this, [this] {
    if (disassembly_)
      disassembly_->addressSpaceChanged();
    if (hex_)
      hex_->addressSpaceChanged();
    // Lists that arrived first name their rows' segments now.
    for (auto *chooser : std::as_const(choosers_))
      chooser->model().addressSpaceChanged();
    navigateInitialAddress();
  });
  Q_UNUSED(toolbars);
}

void MainWindow::fillPlaceholder(QMenu *menu, const QString &name) {
  if (name == QLatin1String("RecentFiles")) {
    recentMenu_ = menu;
    auto *anchor = menu->addSeparator();
    anchor->setObjectName(QStringLiteral("recentAnchor"));
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
      for (auto *action : menu->actions())
        if (action->property("recent").toBool())
          menu->removeAction(action), action->deleteLater();
      const auto files =
          QSettings().value(settings::RecentFiles).toStringList();
      QAction *before = nullptr;
      for (auto *action : menu->actions())
        if (action->objectName() == QLatin1String("recentAnchor"))
          before = action;
      int index = 0;
      for (const auto &file : files) {
        auto *action =
            new QAction(QStringLiteral("&%1 %2").arg(index++).arg(file), menu);
        action->setProperty("recent", true);
        connect(action, &QAction::triggered, this,
                [this, file] { openFile(file); });
        menu->insertAction(before, action);
      }
    });
  } else if (name == QLatin1String("Languages")) {
    auto *group = new QActionGroup(menu);
    const QString current = currentLanguage();
    for (const auto &language : languages()) {
      auto *action = menu->addAction(language.selfName);
      action->setCheckable(true);
      action->setChecked(current == language.code);
      group->addAction(action);
      const QString code = language.code;
      connect(action, &QAction::triggered, this, [this, code] {
        applyLanguage(code);
        mcp_.retranslate();
        broker_.retranslate();
      });
    }
  } else if (name == QLatin1String("ToolbarToggles")) {
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
      menu->clear();
      for (auto *toolbar : findChildren<QToolBar *>())
        menu->addAction(toolbar->toggleViewAction());
    });
  } else if (name == QLatin1String("OpenWindows")) {
    windowsMenu_ = menu;
    connect(menu, &QMenu::aboutToShow, this, [this, menu] {
      for (auto *action : menu->actions())
        if (action->property("window").toBool())
          menu->removeAction(action), action->deleteLater();
      int index = 1;
      for (auto it = docks_.cbegin(); it != docks_.cend(); ++it) {
        auto *dock = it.value();
        if (!dock->isOpen())
          continue;
        auto *action = menu->addAction(dock->icon(), dock->title());
        action->setProperty("window", true);
        if (index <= 9)
          action->setShortcut(
              QKeySequence(QStringLiteral("Alt+%1").arg(index)));
        ++index;
        connect(action, &QAction::triggered, this, [dock] {
          dock->open();
          dock->raise();
          if (auto *widget = dock->widget())
            widget->setFocus();
        });
      }
    });
  }
}

MainWindow::Dock *MainWindow::makeDock(const QString &id, const char *title,
                                       const QString &iconName,
                                       QWidget *content) {
  auto *dock = new Dock(id);
  dock->setAcceptDrops(true);
  if (title) {
    dockTitles_.insert(id, title);
    dock->setTitle(tr(title));
  }
  dock->setIcon(icon(iconName));
  dock->setWidget(content);
  docks_.insert(id, dock);
  return dock;
}

KDDockWidgets::Core::DockWidget *MainWindow::createDock(const QString &name) {
  if (!instance_)
    return nullptr;
  auto *dock = instance_->dockNamed(name);
  return dock ? dock->dockWidget() : nullptr;
}

MainWindow::Dock *MainWindow::dockNamed(const QString &name) {
  if (auto *existing = docks_.value(name))
    return existing;
  if (name == QLatin1String(PseudocodeDock)) {
    codeView(CodeView::pseudocodeRepresentation());
    return docks_.value(name);
  }
  if (name == QLatin1String(RepresentationDock)) {
    codeView(QStringLiteral("med"));
    return docks_.value(name);
  }
  if (name == QLatin1String(ExtensionsDock)) {
    extensions_ = new ExtensionsView(session_);
    extensions_->setLocationProvider([this] { return currentAddress(); });
    return makeDock(name, QT_TR_NOOP("Extensions"),
                    QStringLiteral("extensions"), extensions_);
  }
  for (int kind = 0; kind < int(ChooserKind::Count); ++kind)
    if (name == chooserDockId(ChooserKind(kind)))
      return chooserDock(ChooserKind(kind));
  return nullptr;
}

void MainWindow::openDock(Dock *dock, bool tabbed) {
  if (!dock)
    return;
  if (!dock->isOpen()) {
    if (tabbed)
      docks_.value(DisassemblyDock)->addDockWidgetAsTab(dock);
    else
      addDockWidget(dock, KDDockWidgets::Location_OnBottom);
  }
  dock->open();
  dock->raise();
}

void MainWindow::buildDocks() {
  disassembly_ = new DisassemblyView(session_, space_);
  disassembly_->setSyncName(tr("Hex View-1"));
  actions_.attachViewShortcuts(disassembly_);
  makeDock(DisassemblyDock, QT_TR_NOOP("NeverD View-A"),
           QStringLiteral("text_view"), disassembly_);

  functions_ = new ChooserView(session_, space_, ChooserKind::Functions);
  functions_->table()->setObjectName(QStringLiteral("functionsList"));
  choosers_.insert(int(ChooserKind::Functions), functions_);
  makeDock(FunctionsDock, QT_TR_NOOP("Functions"), QStringLiteral("functions"),
           functions_);

  hex_ = new HexView(session_, space_);
  makeDock(HexDock, QT_TR_NOOP("Hex View-1"), QStringLiteral("hex"), hex_);

  output_ = new OutputWindow(session_);
  makeDock(OutputDock, QT_TR_NOOP("Output"), QStringLiteral("output"), output_);
  output_->setLocationProvider([this] { return currentAddress(); });

  overview_ = new GraphOverview;
  overview_->setGraph(disassembly_->graph());
  makeDock(OverviewDock, QT_TR_NOOP("Graph overview"),
           QStringLiteral("graph_overview"), overview_);

  for (const auto kind : {ChooserKind::Imports, ChooserKind::Exports})
    chooserDock(kind);

  connect(functions_, &ChooserView::activated, this,
          &MainWindow::activateFunction);
  connect(disassembly_, &DisassemblyView::locationChanged, this,
          [this](Address address) { synchronize(address, disassembly_); });
  connect(disassembly_, &DisassemblyView::navigateRequested, this,
          [this](Address address) { navigate(address); });
  connect(disassembly_, &DisassemblyView::contextMenuRequested, this,
          &MainWindow::contextMenu);
  connect(disassembly_, &DisassemblyView::historyChanged, this, [this] {
    if (!codeHistoryNavigation_)
      ++codeNavigationSerial_;
    // An explicit navigation supersedes a deferred restored location.
    initialAddress_.reset();
    restoreGraph_ = false;
    updateActions();
  });
  connect(disassembly_, &DisassemblyView::graphModeChanged, this,
          [this](bool graph) {
            if (graph)
              showOverview();
          });
  connect(hex_, &HexView::locationChanged, this,
          [this](Address address) { disassembly_->navigate(address, false); });
  connect(output_, &OutputWindow::navigateRequested, this,
          [this](Address address) { jump(address); });
  // The output window and dialogs act on the last analysis view used.
  connect(qApp, &QApplication::focusChanged, this,
          [this](QWidget *, QWidget *now) {
            for (auto it = docks_.cbegin(); it != docks_.cend(); ++it)
              if (*it && (*it)->isAncestorOf(now)) {
                // Choosers and auxiliary windows keep the analysis context.
                if (auto *view = qobject_cast<CodeView *>((*it)->widget())) {
                  lastCodeView_ = view;
                  tabCodeView_ = view;
                } else if (it.key() == QLatin1String(DisassemblyDock) ||
                           it.key() == QLatin1String(HexDock))
                  lastCodeView_.clear();
                if (it.key() != QLatin1String(OutputDock))
                  hexActive_ = it.key() == QLatin1String(HexDock);
                return;
              }
          });
  connect(output_, &OutputWindow::commandRequested, this,
          &MainWindow::runCommand);
  connect(&session_, &Session::message, output_, &OutputWindow::append);

  // Restores create on-demand windows by name.
  KDDockWidgets::Config::self().setDockWidgetFactoryFunc(
      &MainWindow::createDock);
  connect(disassembly_->listing(), &ListingView::contentPainted, this,
          &MainWindow::firstContentPainted);
}

MainWindow::Dock *MainWindow::chooserDock(ChooserKind kind) {
  const QString id = chooserDockId(kind);
  if (auto *dock = docks_.value(id))
    return dock;
  auto *view = new ChooserView(session_, space_, kind);
  choosers_.insert(int(kind), view);
  connect(view, &ChooserView::activated, this,
          [this](Address address) { navigate(address); });
  // Lists reload when a session opens; one first shown later loads now.
  if (session_.loaded())
    view->model().reload();
  auto *dock = makeDock(id, nullptr, view->model().iconName(), view);
  dock->setTitle(view->model().title());
  return dock;
}

CodeView *MainWindow::codeView(const QString &representation) {
  const bool c = CodeView::isSource(representation);
  const QString id = c ? PseudocodeDock : RepresentationDock;
  if (auto *dock = docks_.value(id))
    return static_cast<CodeView *>(dock->widget());
  auto *view = new CodeView(session_, representation);
  actions_.attachViewShortcuts(view);
  auto *dock =
      makeDock(id, nullptr,
               c ? QStringLiteral("pseudocode") : QStringLiteral("ir"), view);
  retitleCodeDocks();
  connect(view, &CodeView::representationChanged, this,
          &MainWindow::retitleCodeDocks);
  connect(view, &CodeView::languageChanged, this,
          &MainWindow::retitleCodeDocks);
  connect(view->text(), &CodeText::addressSelected, this,
          [this](Address, bool mapped) {
            if (!mapped)
              output_->append(
                  tr("This instruction has no source-line address mapping."),
                  1);
          });
  connect(view->text(), &CodeText::nameActivated, this,
          [this, view](const QString &name) { activateCodeName(view, name); });
  // Data shows in the disassembly, as an import's name does.
  connect(view->text(), &CodeText::objectActivated, this,
          [this](Address address) { jump(address); });
  connect(view->text(), &CodeText::contextMenuRequested, this,
          &MainWindow::contextMenu);
  if (c)
    pseudocode_ = view;
  return view;
}

void MainWindow::retitleCodeDocks() {
  // Windows showing the same representation take letters in order, the
  // pseudocode window first: Pseudocode-A, Pseudocode-B.
  QSet<QString> taken;
  for (const char *id : {PseudocodeDock, RepresentationDock}) {
    auto *dock = docks_.value(QLatin1String(id));
    auto *code = dock ? qobject_cast<CodeView *>(dock->widget()) : nullptr;
    if (!code)
      continue;
    const QString title = CodeView::titleOf(code->representation());
    char letter = 'A';
    while (taken.contains(title + QLatin1Char('-') + QLatin1Char(letter)))
      ++letter;
    const QString name = title + QLatin1Char('-') + QLatin1Char(letter);
    taken.insert(name);
    // Pseudocode in the function's own language says which it chose.
    const QString language = code->chosenLanguage();
    dock->setTitle(language.isEmpty() ? name
                                      : name + QStringLiteral(" (") + language +
                                            QLatin1Char(')'));
  }
}

void MainWindow::initializeLayout() {
  if (!QFile::exists(layoutPath()) ||
      !KDDockWidgets::LayoutSaver().restoreFromFile(layoutPath()))
    applyDefaultLayout();
  // A saved arrangement must not start a decompile while opening a file.
  if (auto *dock = docks_.value(PseudocodeDock))
    dock->forceClose();
}

void MainWindow::applyDefaultLayout() {
  auto *disassembly = docks_.value(DisassemblyDock);
  addDockWidget(disassembly, KDDockWidgets::Location_OnTop);
  addDockWidget(docks_.value(FunctionsDock), KDDockWidgets::Location_OnLeft,
                nullptr, InitialOption(QSize(FunctionsWidth, 0)));
  disassembly->addDockWidgetAsTab(docks_.value(HexDock));
  disassembly->addDockWidgetAsTab(
      docks_.value(chooserDockId(ChooserKind::Imports)));
  disassembly->addDockWidgetAsTab(
      docks_.value(chooserDockId(ChooserKind::Exports)));
  disassembly->setAsCurrentTab();
  addDockWidget(docks_.value(OutputDock), KDDockWidgets::Location_OnBottom,
                nullptr, InitialOption(QSize(0, OutputHeight)));
  defaultSizesPending_ = true;
  if (isVisible())
    applyDefaultSizes();
}

void MainWindow::applyDefaultSizes() {
  defaultSizesPending_ = false;
  if (auto *functions = docks_.value(FunctionsDock); functions->isOpen()) {
    const int target = std::clamp(int(width() * FunctionsShare),
                                  MinimumFunctionsWidth, MaximumFunctionsWidth);
    const int current = functions->dockWidget()->sizeInLayout().width();
    functions->dockWidget()->resizeInLayout(0, 0, target - current, 0);
  }
  if (auto *output = docks_.value(OutputDock); output->isOpen()) {
    const int target = std::clamp(int(height() * OutputShare),
                                  MinimumOutputHeight, MaximumOutputHeight);
    const int current = output->dockWidget()->sizeInLayout().height();
    output->dockWidget()->resizeInLayout(0, target - current, 0, 0);
  }
}

void MainWindow::showEvent(QShowEvent *event) {
  KDDockWidgets::QtWidgets::MainWindow::showEvent(event);
  if (defaultSizesPending_)
    applyDefaultSizes();
}

void MainWindow::showOverview() {
  auto *dock = docks_.value(OverviewDock);
  if (dock->isOpen())
    return;
  // Under the functions window, as in the classic graph layout.
  auto *functions = docks_.value(FunctionsDock);
  if (functions->isOpen() && !functions->isFloating())
    addDockWidget(dock, KDDockWidgets::Location_OnBottom, functions,
                  InitialOption(QSize(0, OverviewHeight)));
  else
    addDockWidget(dock, KDDockWidgets::Location_OnLeft, nullptr,
                  InitialOption(QSize(FunctionsWidth, OverviewHeight)));
  dock->open();
}

void MainWindow::buildStatusBar() {
  auto *bar = statusBar();
  // The window manager resizes the window from its edges.
  bar->setSizeGripEnabled(false);
  analysisLabel_ = new QLabel(bar);
  directionLabel_ = new QLabel(bar);
  diskLabel_ = new QLabel(bar);
  fileLabel_ = new QLabel(bar);
  analysisLabel_->setToolTip(
      tr("Background analysis: references and labels are indexed while you "
         "browse"));
  bar->addWidget(analysisLabel_);
  applyIndicator();
  bar->addWidget(directionLabel_);
  bar->addWidget(diskLabel_);
  bar->addPermanentWidget(fileLabel_);
}

//===----------------------------------------------------------------------===//
// Session wiring
//===----------------------------------------------------------------------===//

void MainWindow::connectSession() {
  connect(&session_, &Session::opened, this, [this] {
    pseudocodeAfterJump_.reset();
    if (auto *dock = docks_.value(PseudocodeDock))
      dock->forceClose();
    if (pseudocode_)
      pseudocode_->text()->clear();
    space_.clear();
    disassembly_->setGraphMode(false);
    for (auto *chooser : std::as_const(choosers_))
      chooser->model().reload();
    restoreProjectState();
    // A database also carries a desktop. Restore its arrangement, but keep
    // source analysis explicit for each newly opened project.
    if (auto *dock = docks_.value(PseudocodeDock))
      dock->forceClose();
    updateTitle();
    updateActions();
    disassembly_->focusContent();
    // Scripted command lines run once the opening state has settled.
    if (!pendingCommands_.isEmpty())
      QTimer::singleShot(
          0, this, [this, commands = std::exchange(pendingCommands_, {})] {
            for (const auto &command : commands)
              output_->run(command);
          });
  });
  session_.setStateProvider([this] { return projectState(); });
  connect(&session_, &Session::unloaded, this, [this] {
    updateTitle();
    updateActions();
  });
  connect(&session_, &Session::stateChanged, this, [this] {
    updateActions();
    updateStatusBar();
  });
  connect(&session_, &Session::backgroundChanged, this,
          &MainWindow::updateStatusBar);
  connect(&session_, &Session::databaseSaved, this, &MainWindow::updateTitle);
  connect(&session_, &Session::historyChanged, this,
          &MainWindow::updateActions);
  connect(&session_, &Session::transitionRequested, this,
          [this](const QString &action) {
            const auto choice = QMessageBox::question(
                this, tr("Unsaved changes"),
                tr("Comments were changed. Save them before continuing?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                QMessageBox::Save);
            session_.resolveTransition(
                choice == QMessageBox::Save      ? QStringLiteral("save")
                : choice == QMessageBox::Discard ? QStringLiteral("discard")
                                                 : QStringLiteral("cancel"));
            Q_UNUSED(action);
          });
  connect(&session_, &Session::quitApproved, this, [this] {
    quitting_ = true;
    close();
  });
  connect(&session_, &Session::selectionChanged, &broker_,
          &GuiSessionBroker::setSelection);
  connect(&broker_, &GuiSessionBroker::queryRequested, &session_,
          &Session::externalQuery);
  connect(&session_, &Session::externalResponse, &broker_,
          &GuiSessionBroker::reply);
  connect(&broker_, &GuiSessionBroker::navigationRequested, this,
          [this](const QString &address, bool) {
            if (const auto parsed = parseAddress(address))
              navigate(*parsed);
          });
}

//===----------------------------------------------------------------------===//
// Location
//===----------------------------------------------------------------------===//

std::optional<Address> MainWindow::currentAddress() const {
  return disassembly_ ? disassembly_->currentItem() : std::nullopt;
}

ChooserView *MainWindow::focusedChooser() const {
  for (auto *widget = QApplication::focusWidget(); widget;
       widget = widget->parentWidget())
    if (auto *view = qobject_cast<ChooserView *>(widget))
      return view;
  return nullptr;
}

CodeView *MainWindow::focusedCodeView() const {
  for (auto *widget = QApplication::focusWidget(); widget;
       widget = widget->parentWidget())
    if (auto *view = qobject_cast<CodeView *>(widget))
      return view;
  return nullptr;
}

std::optional<Address> MainWindow::currentFunction() const {
  return disassembly_ ? disassembly_->currentFunction() : std::nullopt;
}

void MainWindow::activateFunction(Address address) {
  ++codeNavigationSerial_;
  if (!session_.loaded())
    return;
  auto *view = lastCodeView_.data();
  if (!view || !view->isVisible()) {
    navigate(address);
    return;
  }
  const auto position = view->text()->currentAddress();
  navigateCodeFunction(view, address, address,
                       position ? position : view->text()->function());
}

void MainWindow::navigateCodeFunction(CodeView *view, Address address,
                                      Address function,
                                      std::optional<Address> from) {
  // The shared history and background listing follow without changing the
  // originating code window or its representation, including a pinned view.
  disassembly_->navigate(address, true, from);
  if (view->text()->function() != function)
    view->showFunction(function);
  for (auto *dock : std::as_const(docks_))
    if (dock && dock->widget() == view) {
      dock->raise();
      break;
    }
  view->window()->activateWindow();
  view->text()->setFocus();
  updateActions();
}

void MainWindow::navigate(Address address) {
  ++codeNavigationSerial_;
  if (!session_.loaded())
    return;
  if (auto *dock = docks_.value(DisassemblyDock)) {
    dock->open();
    dock->raise();
  }
  disassembly_->navigate(address, true);
  disassembly_->focusContent();
}

void MainWindow::jump(Address address) {
  ++codeNavigationSerial_;
  if (!session_.loaded())
    return;
  if (!hexActive_ || !hex_->isVisible()) {
    navigate(address);
    return;
  }
  disassembly_->navigate(address, true);
  hex_->setCurrent(address);
  hex_->setFocus();
}

void MainWindow::activateCodeName(CodeView *view, const QString &name) {
  const auto serial = ++codeNavigationSerial_;
  const auto epoch = session_.epoch();
  const auto source = view->text()->function();
  const auto position = view->text()->currentAddress();
  const auto from = position ? position : source;
  const auto representation = view->representation();
  session_.read(
      QStringLiteral("resolve"), {{"query", name}}, view,
      [this, view, serial, epoch, source, from, representation,
       name](const QJsonObject &payload) {
        if (serial != codeNavigationSerial_ || epoch != session_.epoch() ||
            view->text()->function() != source ||
            view->representation() != representation || !view->isVisible())
          return;
        const auto address = addressValue(payload.value("address"));
        const auto function = addressValue(payload.value("function_address"));
        if (function && !payload.value("import").toBool()) {
          navigateCodeFunction(view, address.value_or(*function), *function,
                               from);
        } else if (address) {
          // Data and import slots retain ordinary address navigation.
          jump(*address);
        } else {
          navigateExpression(name);
        }
      },
      [this, view, name, serial, epoch, source,
       representation](const QString &, const QString &error) {
        if (serial == codeNavigationSerial_ && epoch == session_.epoch() &&
            view->text()->function() == source &&
            view->representation() == representation && view->isVisible())
          output_->append(tr("Cannot jump to %1: %2").arg(name, error), 2);
      });
}

void MainWindow::navigateExpression(const QString &text) {
  ++codeNavigationSerial_;
  resolveExpression(
      session_, this, text, currentAddress(),
      [this, text](std::optional<Address> value, const QString &error) {
        if (value)
          jump(*value);
        else
          output_->append(tr("Cannot jump to %1: %2").arg(text, error), 2);
      });
}

void MainWindow::navigateHistory(bool forward) {
  const auto serial = ++codeNavigationSerial_;
  if (forward ? !disassembly_->canGoForward() : !disassembly_->canGoBack())
    return;
  if (auto *view = focusedCodeView()) {
    const auto epoch = session_.epoch();
    connect(
        disassembly_->listing(), &ListingView::jumpSettled, view,
        [this, view, serial, epoch] {
          if (serial != codeNavigationSerial_ || epoch != session_.epoch() ||
              !view->isVisible())
            return;
          const auto function = disassembly_->listing()->currentFunction();
          if (function && view->text()->function() != function)
            view->showFunction(*function);
          view->text()->setFocus();
        },
        Qt::SingleShotConnection);
  }
  QScopedValueRollback<bool> guard(codeHistoryNavigation_, true);
  if (forward)
    disassembly_->goForward();
  else
    disassembly_->goBack();
}

void MainWindow::synchronize(Address address, QObject *source) {
  navigationBand_->setCurrent(address);
  if (source != hex_)
    hex_->setCurrent(address, 1);
  const auto function = currentFunction();
  session_.publishSelection(address, function, QStringLiteral("disassembly"));
  updateActions();
}

//===----------------------------------------------------------------------===//
// Actions
//===----------------------------------------------------------------------===//

void MainWindow::connectActions() {
  const auto on = [this](ActionId id, auto slot) {
    connect(actions_.action(id), &QAction::triggered, this, slot);
  };
  on(ActionId::FileOpen, [this] { openDialog(); });
  on(ActionId::FileReload, [this] { reloadInput(); });
  on(ActionId::FileLoadSignatures, [this] {
    const auto path = QFileDialog::getOpenFileName(
        this, tr("Load signature pack"), {},
        tr("Signature packs (*.pat *.json);;All files (*)"));
    if (!path.isEmpty())
      session_.loadSignatures(path, false);
  });
  on(ActionId::FileLoadSignatureTree, [this] {
    const auto path =
        QFileDialog::getExistingDirectory(this, tr("Signature directory"));
    if (!path.isEmpty())
      session_.loadSignatures(path, true);
  });
  on(ActionId::FileExportListing, [this] { exportCurrent(false); });
  on(ActionId::FileExportPseudocode, [this] { exportCurrent(true); });
  on(ActionId::FileSave, [this] { session_.save(); });
  on(ActionId::FileClose, [this] { session_.closeFile(); });
  on(ActionId::FileQuickStart, [this] { showQuickStart(); });
  on(ActionId::FileExit, [this] { close(); });

  on(ActionId::EditUndo, [this] { session_.undo(); });
  on(ActionId::EditRedo, [this] { session_.redo(); });
  on(ActionId::EditCopy, [this] { copySelection(); });
  on(ActionId::EditCopyAddress, [this] {
    if (const auto address = currentAddress())
      QApplication::clipboard()->setText(hexAddress(*address));
  });
  on(ActionId::EditRename, [this] { rename(); });
  on(ActionId::EditComment, [this] { comment(); });
  on(ActionId::EditRepeatableComment, [this] { comment(); });
  on(ActionId::EditBookmark, [this] {
    const auto address = currentAddress();
    if (!address)
      return;
    bool ok = false;
    const auto text =
        QInputDialog::getText(this, tr("Mark position"), tr("Description:"),
                              QLineEdit::Normal, displayAddress(*address), &ok);
    if (!ok)
      return;
    auto rows = bookmarks();
    rows.append(QJsonObject{{"address", hexAddress(*address)}, {"text", text}});
    setBookmarks(rows);
  });

  on(ActionId::JumpOperand, [this] {
    if (const auto target = disassembly_->operandTarget())
      navigate(*target);
  });
  on(ActionId::JumpNewWindow, [this] {
    if (const auto target = disassembly_->operandTarget())
      navigate(*target);
  });
  on(ActionId::EditCreateFunction, [this] {
    if (const auto address = currentAddress())
      session_.createFunction(*address);
  });
  on(ActionId::EditDeleteFunction, [this] {
    if (const auto function = currentFunction())
      session_.deleteFunction(*function);
  });
  // An operand's number shows in the base the user picks, with its sign or
  // bits changed: the number under the cursor, else the line's last number.
  const auto formatOperand = [this](const QString &action) {
    if (focusedCodeView())
      return;
    if (const auto address = currentAddress())
      session_.formatOperand(*address, disassembly_->currentOperand(), action);
  };
  for (const auto &[id, action] : {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  std::pair{ActionId::EditOperand##Id, QStringLiteral(Spelling)},
#include "neverd/OperandFormats.def"
           std::pair{ActionId::EditOperandNegate, QStringLiteral("negate")},
           std::pair{ActionId::EditOperandInvert, QStringLiteral("invert")}})
    on(id, [formatOperand, action] { formatOperand(action); });
  // Data items are the disassembly's; a pseudocode window keeps its keys.
  for (const auto &[id, action] :
       {std::pair{ActionId::EditDefineCode, QStringLiteral("code")},
        std::pair{ActionId::EditDefineData, QStringLiteral("data")},
        std::pair{ActionId::EditDefineString, QStringLiteral("string")},
        std::pair{ActionId::EditUndefine, QStringLiteral("undefine")}})
    on(id, [this, action] {
      if (focusedCodeView())
        return;
      if (const auto address = currentAddress())
        session_.defineItem(*address, action);
    });
  on(ActionId::JumpBack, [this] { navigateHistory(false); });
  on(ActionId::JumpForward, [this] { navigateHistory(true); });
  on(ActionId::JumpNextFunction, [this] { stepFunction(true); });
  on(ActionId::JumpPreviousFunction, [this] { stepFunction(false); });
  on(ActionId::JumpPseudocode, [this] {
    // From any pseudocode or IR window back to the disassembly.
    if (auto *view = focusedCodeView()) {
      tabCodeView_ = view;
      if (const auto address = view->text()->currentAddress())
        navigate(*address);
      else if (const auto function = view->text()->function()) {
        output_->append(tr("This source line has no instruction address "
                           "mapping; showing the function entry"),
                        1);
        navigate(*function);
      }
    } else {
      showPseudocode(tabCodeView_ ? tabCodeView_->representation()
                                  : CodeView::pseudocodeRepresentation(),
                     true);
    }
  });
  on(ActionId::JumpAnywhere, [this] { jumpAnywhere(); });
  on(ActionId::JumpByName, [this] {
    auto *dock = chooserDock(ChooserKind::Names);
    if (!dock->isOpen())
      docks_.value(DisassemblyDock)->addDockWidgetAsTab(dock);
    dock->open();
    dock->raise();
    choosers_.value(int(ChooserKind::Names))->focusFilter();
  });
  on(ActionId::JumpFunction, [this] {
    docks_.value(FunctionsDock)->open();
    docks_.value(FunctionsDock)->raise();
    functions_->focusFilter();
  });
  on(ActionId::JumpSegment, [this] {
    auto *dock = chooserDock(ChooserKind::Segments);
    if (!dock->isOpen())
      docks_.value(DisassemblyDock)->addDockWidgetAsTab(dock);
    dock->open();
    dock->raise();
  });
  on(ActionId::JumpEntry, [this] {
    auto *dock = docks_.value(chooserDockId(ChooserKind::Exports));
    dock->open();
    dock->raise();
  });
  // In a list window the current row is the item, as in the classic lists:
  // cross references to a string are those of the selected string.
  on(ActionId::JumpXrefsTo, [this] {
    auto *list = focusedChooser();
    showCrossReferences(list ? list->referenceTarget() : currentAddress(),
                        true);
  });
  on(ActionId::JumpXrefsFrom, [this] {
    auto *list = focusedChooser();
    showCrossReferences(list ? list->referenceTarget() : currentAddress(),
                        false);
  });
  on(ActionId::JumpXrefOperand, [this] {
    const auto target = disassembly_->operandTarget();
    showCrossReferences(target ? target : currentAddress(), true);
  });
  on(ActionId::JumpMark, [this] {
    auto *dock = chooserDock(ChooserKind::Bookmarks);
    choosers_.value(int(ChooserKind::Bookmarks))
        ->model()
        .setLocalRows(bookmarks());
    if (!dock->isOpen())
      docks_.value(DisassemblyDock)->addDockWidgetAsTab(dock);
    dock->open();
    dock->raise();
  });

  on(ActionId::SearchText,
     [this] { searchBinary(QStringLiteral("text"), false); });
  on(ActionId::SearchNextText,
     [this] { searchBinary(QStringLiteral("text"), true); });
  on(ActionId::SearchBytes,
     [this] { searchBinary(QStringLiteral("bytes"), false); });
  on(ActionId::SearchNextBytes,
     [this] { searchBinary(QStringLiteral("bytes"), true); });
  on(ActionId::SearchHighlightUp, [this] { searchHighlight(false); });
  on(ActionId::SearchHighlightDown, [this] { searchHighlight(true); });
  on(ActionId::SearchInView, [this] {
    bool ok = false;
    const auto text = QInputDialog::getText(this, tr("Find in view"),
                                            tr("Text:"), QLineEdit::Normal,
                                            disassembly_->currentToken(), &ok);
    if (!ok || text.isEmpty())
      return;
    bool found = false;
    if (auto *code = focusedCodeView())
      found = code->text()->findText(text, searchDown_);
    else
      found = disassembly_->listing()->findInLoaded(text, searchDown_);
    if (!found)
      output_->append(tr("'%1' was not found among the loaded lines").arg(text),
                      1);
  });

  on(ActionId::ViewQuickView, [this] { showCommandPalette(); });
  on(ActionId::ViewDisassembly, [this] {
    docks_.value(DisassemblyDock)->open();
    docks_.value(DisassemblyDock)->raise();
  });
  on(ActionId::ViewToggleGraph, [this] { toggleGraph(); });
  on(ActionId::ViewPseudocode,
     [this] { showPseudocode(CodeView::pseudocodeRepresentation()); });
  on(ActionId::ViewLLVMC, [this] { showPseudocode(QStringLiteral("llvmc")); });
  on(ActionId::ViewLowIR, [this] { showPseudocode(QStringLiteral("low")); });
  on(ActionId::ViewMedIR, [this] { showPseudocode(QStringLiteral("med")); });
  on(ActionId::ViewHighIR, [this] { showPseudocode(QStringLiteral("high")); });
  on(ActionId::ViewLLVMIR, [this] { showPseudocode(QStringLiteral("llvm")); });
  on(ActionId::ViewHex, [this] {
    docks_.value(HexDock)->open();
    docks_.value(HexDock)->raise();
    hexActive_ = true;
    hex_->setFocus();
  });
  const auto openChooser = [this](ChooserKind kind) {
    auto *dock = chooserDock(kind);
    if (!dock->isOpen())
      docks_.value(DisassemblyDock)->addDockWidgetAsTab(dock);
    dock->open();
    dock->raise();
  };
  on(ActionId::ViewExports,
     [openChooser] { openChooser(ChooserKind::Exports); });
  on(ActionId::ViewImports,
     [openChooser] { openChooser(ChooserKind::Imports); });
  on(ActionId::ViewNames, [openChooser] { openChooser(ChooserKind::Names); });
  on(ActionId::ViewStrings,
     [openChooser] { openChooser(ChooserKind::Strings); });
  // The list opens ready for a query, like a debugger's reference search.
  on(ActionId::SearchStringReferences, [this, openChooser] {
    openChooser(ChooserKind::StringReferences);
    choosers_.value(int(ChooserKind::StringReferences))->focusFilter();
  });
  on(ActionId::ViewSegments,
     [openChooser] { openChooser(ChooserKind::Segments); });
  on(ActionId::ViewFunctions, [this] {
    docks_.value(FunctionsDock)->open();
    docks_.value(FunctionsDock)->raise();
  });
  on(ActionId::ViewCrossReferences,
     [this] { showCrossReferences(currentAddress(), true); });
  on(ActionId::ViewBookmarks,
     [this] { actions_.action(ActionId::JumpMark)->trigger(); });
  on(ActionId::ViewOutput, [this] {
    docks_.value(OutputDock)->open();
    docks_.value(OutputDock)->raise();
    output_->focusCommandLine();
  });
  on(ActionId::ViewGraphOverview, [this] {
    auto *dock = docks_.value(OverviewDock);
    if (dock->isOpen())
      dock->forceClose();
    else
      showOverview();
  });
  on(ActionId::ViewConnections, [this] { showConnections(); });
  on(ActionId::ViewExtensions, [this] {
    openDock(dockNamed(QString::fromLatin1(ExtensionsDock)), false);
  });
  on(ActionId::ViewUndoHistory, [this] {
    const auto &history = session_.history();
    output_->append(tr("History: %1 entries, cursor %2")
                        .arg(history.value("total").toInt())
                        .arg(history.value("cursor").toInt()),
                    0);
  });
  on(ActionId::ViewCalculator, [this] { showCalculator(); });
  on(ActionId::ViewFullScreen,
     [this] { setWindowState(windowState() ^ Qt::WindowFullScreen); });
  on(ActionId::ViewZoomIn, [] { Theme::instance().zoomCodeFont(1); });
  on(ActionId::ViewZoomOut, [] { Theme::instance().zoomCodeFont(-1); });
  on(ActionId::ViewZoomReset, [] { Theme::instance().resetCodeFontSize(); });

  on(ActionId::OptionsGeneral, [this] { showOptions(); });
  on(ActionId::OptionsStrings, [this] { showStringOptions(); });
  on(ActionId::OptionsColors, [this] { chooseTheme(); });
  on(ActionId::OptionsFont, [this] { chooseFont(); });
  on(ActionId::OptionsShortcuts, [this] { showShortcuts(); });
  on(ActionId::OptionsPalette, [this] { showCommandPalette(); });
  on(ActionId::OptionsRepeatPalette, [this] {
    for (auto *action : actions_.allActions())
      if (action->objectName() == lastPaletteCommand_ && action->isEnabled()) {
        action->trigger();
        return;
      }
  });
  on(ActionId::OptionsAnalyze, [this] { session_.analyzeWholeProgram(); });
  on(ActionId::OptionsCancel, [this] { session_.cancelReads(); });
  on(ActionId::OptionsRestartWorker, [this] { session_.restart(); });

  on(ActionId::WindowsSaveDesktop, [this] { saveDesktop(); });
  on(ActionId::WindowsLoadDesktop, [this] { restoreDesktop(); });
  on(ActionId::WindowsResetDesktop, [this] { resetDesktop(); });
  on(ActionId::WindowsNext, [this] { cycleWindows(true); });
  on(ActionId::WindowsPrevious, [this] { cycleWindows(false); });
  on(ActionId::WindowsClose, [this] {
    for (auto *dock : std::as_const(docks_))
      if (dock->isAncestorOf(QApplication::focusWidget()) &&
          dock != docks_.value(DisassemblyDock)) {
        dock->forceClose();
        return;
      }
  });
  on(ActionId::WindowsFocusCommand, [this] {
    docks_.value(OutputDock)->open();
    output_->focusCommandLine();
  });
  on(ActionId::HelpContents, [] {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(DocumentationUrl)));
  });
  on(ActionId::HelpAbout, [this] { showAbout(); });
}

void MainWindow::updateActions() {
  const bool loaded = session_.loaded();
  const bool location = loaded && currentAddress().has_value();
  for (auto *action : actions_.allActions()) {
    const QString name = action->objectName();
    const bool always = name.startsWith(QStringLiteral("Options")) ||
                        name.startsWith(QStringLiteral("Windows")) ||
                        name.startsWith(QStringLiteral("Help")) ||
                        name == QLatin1String("FileOpen") ||
                        name == QLatin1String("FileExit") ||
                        name == QLatin1String("FileQuickStart") ||
                        name.startsWith(QStringLiteral("ViewZoom")) ||
                        name == QLatin1String("ViewFullScreen") ||
                        name == QLatin1String("ViewOutput") ||
                        name == QLatin1String("ViewCalculator") ||
                        name == QLatin1String("ViewConnections");
    action->setEnabled(always || loaded);
  }
  actions_.action(ActionId::JumpBack)
      ->setEnabled(loaded && disassembly_->canGoBack());
  actions_.action(ActionId::JumpForward)
      ->setEnabled(loaded && disassembly_->canGoForward());
  actions_.action(ActionId::EditUndo)->setEnabled(loaded && session_.canUndo());
  actions_.action(ActionId::EditRedo)->setEnabled(loaded && session_.canRedo());
  actions_.action(ActionId::FileSave)->setEnabled(loaded && session_.dirty());
  actions_.action(ActionId::EditRename)
      ->setEnabled(location && !session_.readOnly());
  // A function starts where none starts yet; the one the cursor is in can
  // stop being one.
  const bool functionEdits =
      location && !session_.readOnly() && session_.keepsFunctionEdits();
  const auto function = functionEdits ? currentFunction() : std::nullopt;
  actions_.action(ActionId::EditCreateFunction)
      ->setEnabled(functionEdits && function != currentAddress());
  actions_.action(ActionId::EditDeleteFunction)
      ->setEnabled(functionEdits && function.has_value());
  const bool dataItems =
      location && !session_.readOnly() && session_.keepsDataItems();
  for (const auto id : {ActionId::EditDefineCode, ActionId::EditDefineData,
                        ActionId::EditDefineString, ActionId::EditUndefine})
    actions_.action(id)->setEnabled(dataItems);
  actions_.action(ActionId::EditDefineCode)
      ->setEnabled(dataItems && !focusedCodeView());
  const bool operandFormats =
      location && !session_.readOnly() && session_.keepsOperandFormats();
  for (const auto id :
       {ActionId::EditOperandNumber, ActionId::EditOperandHex,
        ActionId::EditOperandDecimal, ActionId::EditOperandBinary,
        ActionId::EditOperandChar, ActionId::EditOperandOffset,
        ActionId::EditOperandNegate, ActionId::EditOperandInvert})
    actions_.action(id)->setEnabled(operandFormats);
  actions_.action(ActionId::EditComment)
      ->setEnabled(location && !session_.readOnly());
  actions_.action(ActionId::EditRepeatableComment)
      ->setEnabled(location && !session_.readOnly());
  actions_.action(ActionId::OptionsAnalyze)
      ->setEnabled(loaded && !session_.metadata().value("analyzed").toBool());
}

void MainWindow::updateTitle() {
  if (!session_.loaded()) {
    setWindowTitle(QStringLiteral("NeverD"));
    return;
  }
  // Like a classic title bar: the database, then the input it describes.
  const QString database = session_.databasePath();
  const QString shown = QFileInfo::exists(database)
                            ? QFileInfo(database).fileName()
                            : session_.fileName();
  setWindowTitle(
      QStringLiteral("NeverD - %1 %2").arg(shown, session_.projectPath()));
}

void MainWindow::updateStatusBar() {
  const auto &background = session_.background();
  const auto state = background.value("state").toString();
  QString analysis;
  if (!session_.loaded())
    analysis = tr("AU: idle");
  else if (state == QLatin1String("building") ||
           state == QLatin1String("pending")) {
    const int total = background.value("total").toInt();
    const int done = background.value("done").toInt();
    analysis = total ? tr("AU: busy %1%").arg(done * 100 / std::max(1, total))
                     : tr("AU: busy");
  } else {
    analysis = tr("AU: idle");
  }
  analysisLabel_->setText(analysis);
  directionLabel_->setText(searchDown_ ? tr("Down") : tr("Up"));
  const QString path = session_.loaded()
                           ? QFileInfo(session_.filePath()).absolutePath()
                           : QDir::homePath();
  const QStorageInfo storage(path);
  diskLabel_->setText(
      tr("Disk: %1GB").arg(storage.bytesAvailable() / (1024LL * 1024 * 1024)));
  fileLabel_->setText(
      session_.loaded()
          ? QStringLiteral("%1 · %2%3%4")
                .arg(session_.architecture(), session_.format(),
                     session_.platformName().isEmpty()
                         ? QString()
                         : QStringLiteral(" · ") + session_.platformName(),
                     session_.readOnly() ? tr(" · read-only") : QString())
          : QString());
}

//===----------------------------------------------------------------------===//
// Commands
//===----------------------------------------------------------------------===//

void MainWindow::openFile(const QString &path) {
  startupQuickStartPending_ = false;
  session_.open(path);
}

void MainWindow::openDialog() {
  startupQuickStartPending_ = false;
  // The folder of the file the user opened, not of a working copy of it.
  const QString start = QFileInfo(session_.projectPath()).absolutePath();
  const auto path = QFileDialog::getOpenFileName(
      this, tr("Open binary or database"),
      start.isEmpty() ? QDir::homePath() : start,
      tr("All files (*);;NeverD databases (*.nddb)"));
  if (!path.isEmpty())
    chooseLoader(path);
}

void MainWindow::chooseLoader(const QString &path) {
  startupQuickStartPending_ = false;
  // A project opens as it was saved, and an engine that cannot list its
  // loaders opens files as before.
  if (!session_.identifiesFiles() || ProjectDatabase::hasState(path)) {
    openFile(path);
    return;
  }
  session_.read(
      QStringLiteral("identify"), {{"path", path}}, this,
      [this, path](const QJsonObject &payload) {
        LoadFileDialog dialog(path, payload.value("rows").toArray(), this);
        dialog.setIndicator(
            QSettings().value(AnalysisIndicatorKey, true).toBool());
        dialog.setBinaryProcessor(
            QSettings().value(BinaryProcessorKey).toString());
        if (dialog.exec() != QDialog::Accepted || dialog.row() < 0)
          return;
        QSettings().setValue(AnalysisIndicatorKey, dialog.indicator());
        // The processor the user picked, not one the bytes named.
        if (const auto processor = dialog.options().processor;
            !processor.isEmpty() && processor != QLatin1String("auto"))
          QSettings().setValue(BinaryProcessorKey, processor);
        applyIndicator();
        session_.open(path, dialog.options());
      },
      [this, path](const QString &, const QString &message) {
        output_->append(
            tr("The ways to load %1 are unknown: %2").arg(path, message), 1);
        openFile(path);
      });
}

void MainWindow::reloadInput() {
  if (!session_.loaded())
    return;
  // A binary file is read as the processor and at the address the user
  // chose, which IDA asks for when it loads one: reloading asks again,
  // starting from the choice the file was loaded with.
  const QJsonObject load = session_.loadOptionsJson();
  if (load.value("loader").toString() != QLatin1String("binary") ||
      !session_.identifiesFiles()) {
    session_.reload();
    return;
  }
  const QString path = session_.filePath();
  session_.read(
      QStringLiteral("identify"), {{"path", path}}, this,
      [this, path, load](const QJsonObject &payload) {
        LoadFileDialog dialog(path, payload.value("rows").toArray(), this);
        dialog.setWindowTitle(tr("Reload the input file"));
        dialog.setIndicator(
            QSettings().value(AnalysisIndicatorKey, true).toBool());
        dialog.setOptions(load);
        if (dialog.exec() != QDialog::Accepted || dialog.row() < 0)
          return;
        QSettings().setValue(AnalysisIndicatorKey, dialog.indicator());
        applyIndicator();
        session_.reload(dialog.options());
      },
      [this](const QString &, const QString &) { session_.reload(); });
}

void MainWindow::applyIndicator() {
  analysisLabel_->setVisible(
      QSettings().value(AnalysisIndicatorKey, true).toBool());
}

void MainWindow::rename() {
  if (auto *code = focusedCodeView()) {
    const auto target = code->text()->renameTarget();
    const auto function = code->text()->function();
    if (!target || !function) {
      output_->append(
          tr("Select a declared variable or an image name to rename"), 1);
      return;
    }
    const QPointer<CodeView> view = code;
    const auto representation = code->representation();
    const auto epoch = session_.epoch();
    const auto valid = [this, view, function, representation, epoch] {
      return view && session_.epoch() == epoch &&
             view->text()->function() == function &&
             view->representation() == representation;
    };
    const auto edit = [this, view, target = *target, function = *function,
                       representation, epoch,
                       valid](std::optional<Address> address) {
      if (!valid())
        return;
      bool ok = false;
      const auto current = target.value("name").toString();
      const auto name =
          QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                QLineEdit::Normal, current, &ok)
              .trimmed();
      if (view) {
        view->window()->activateWindow();
        view->text()->setFocus();
      }
      if (!ok || !valid() || name == current)
        return;
      if (address)
        session_.rename(*address, name, epoch);
      else {
        const auto original = target.value("original").toString();
        session_.editCode({{"address", hexAddress(function)},
                           {"representation", representation},
                           {"kind", "name"},
                           {"original", original},
                           {"name", name},
                           {"identity", target.value("identity")}},
                          epoch);
      }
      if (view)
        view->text()->setFocus();
    };
    if (target->value("kind").toString() == QLatin1String("local"))
      edit(std::nullopt);
    else if (const auto address = addressValue(target->value("address")))
      edit(address);
    else
      session_.read(
          QStringLiteral("resolve"), {{"query", target->value("query")}}, code,
          [this, edit, valid](const QJsonObject &result) {
            if (valid()) {
              if (const auto address = addressValue(result.value("address")))
                edit(address);
              else
                output_->append(tr("The selected name has no image address"),
                                1);
            }
          });
    return;
  }
  const auto item = currentAddress();
  const auto target = disassembly_->operandTarget();
  if (!item && !target)
    return;
  const quint64 epoch = session_.epoch();
  // A name under the cursor renames what it denotes; otherwise the item the
  // cursor is on: a function at its entry, data, or a label in code.
  const Address address = target ? *target : *item;
  session_.read(
      QStringLiteral("resolve"), {{"query", hexAddress(address)}}, this,
      [this, address, epoch](const QJsonObject &payload) {
        const QString current = payload.value("address_name").toString();
        bool ok = false;
        const auto name = QInputDialog::getText(
            this, tr("Rename address"),
            tr("Name of %1:").arg(displayAddress(address)), QLineEdit::Normal,
            current, &ok);
        if (ok && !name.trimmed().isEmpty() && name.trimmed() != current)
          session_.rename(address, name.trimmed(), epoch);
      });
}

void MainWindow::comment() {
  if (auto *code = focusedCodeView()) {
    const auto target = code->text()->commentTarget();
    const auto function = code->text()->function();
    if (!target || !function) {
      output_->append(
          tr("Select a source line; expand folded code before commenting it"),
          1);
      return;
    }
    const QPointer<CodeView> view = code;
    const auto representation = code->representation();
    const auto epoch = session_.epoch();
    const auto edit = [this, view, target = *target, function = *function,
                       representation, epoch](const QString &current) {
      if (!view || session_.epoch() != epoch ||
          view->text()->function() != function ||
          view->representation() != representation)
        return;
      bool ok = false;
      const auto text = QInputDialog::getMultiLineText(
          this, tr("Please enter text"), tr("Comment this source line:"),
          current, &ok);
      if (view) {
        view->window()->activateWindow();
        view->text()->setFocus();
      }
      if (!ok || !view || session_.epoch() != epoch ||
          view->text()->function() != function ||
          view->representation() != representation)
        return;
      if (const auto address = addressValue(target.value("mapped_address")))
        session_.setComment(*address, text, epoch);
      else {
        auto payload = target;
        payload["address"] = hexAddress(function);
        payload["representation"] = representation;
        payload["kind"] = "comment";
        payload["text"] = text;
        session_.editCode(payload, epoch);
      }
      view->text()->setFocus();
    };
    if (const auto address = addressValue(target->value("mapped_address")))
      session_.read(QStringLiteral("resolve"),
                    {{"query", hexAddress(*address)}}, code,
                    [edit](const QJsonObject &result) {
                      edit(result.value("comment").toString());
                    });
    else
      edit(target->value("text").toString());
    return;
  }
  const auto address = currentAddress();
  if (!address)
    return;
  const quint64 epoch = session_.epoch();
  session_.read(QStringLiteral("resolve"), {{"query", hexAddress(*address)}},
                this, [this, address, epoch](const QJsonObject &payload) {
                  bool ok = false;
                  const auto text = QInputDialog::getMultiLineText(
                      this, tr("Please enter text"),
                      tr("Comment at %1:").arg(displayAddress(*address)),
                      payload.value("comment").toString(), &ok);
                  if (ok)
                    session_.setComment(*address, text, epoch);
                });
}

void MainWindow::jumpAnywhere() {
  if (!session_.loaded())
    return;
  JumpDialog dialog(session_, this);
  if (dialog.exec() != QDialog::Accepted)
    return;
  navigateExpression(dialog.result());
}

void MainWindow::showCrossReferences(std::optional<Address> address, bool to) {
  if (!address || !session_.loaded())
    return;
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const QString title = (to ? tr("xrefs to %1") : tr("xrefs from %1"))
                            .arg(displayAddress(*address, digits));
  ReferencesDialog dialog(
      session_, space_, title,
      {{"address", hexAddress(*address)}, {"direction", to ? "to" : "from"}},
      this);
  if (dialog.exec() == QDialog::Accepted)
    if (const auto chosen = dialog.chosen())
      navigate(*chosen);
}

void MainWindow::showPseudocode(const QString &representation,
                                bool selectAssemblyAddress) {
  // A jump still loading decides the function, as it does for the graph:
  // decompile once it lands, not the function the cursor is leaving.
  if (disassembly_ && disassembly_->listing()->jumpPending()) {
    if (!pseudocodeAfterJump_)
      connect(
          disassembly_->listing(), &ListingView::jumpSettled, this,
          [this] {
            if (const auto pending = std::exchange(pseudocodeAfterJump_, {}))
              showPseudocode(pending->first, pending->second);
          },
          Qt::SingleShotConnection);
    pseudocodeAfterJump_ = std::pair{representation, selectAssemblyAddress};
    return;
  }
  const auto function = currentFunction();
  if (!function) {
    output_->append(tr("Place the cursor inside a function to decompile it."),
                    1);
    return;
  }
  auto *view = selectAssemblyAddress && tabCodeView_ &&
                       tabCodeView_->representation() == representation
                   ? tabCodeView_.data()
                   : codeView(representation);
  const bool c = view == pseudocode_;
  auto *dock = docks_.value(c ? PseudocodeDock : RepresentationDock);
  if (!dock->isOpen()) {
    auto *functions = docks_.value(FunctionsDock);
    const int functionsWidth =
        functions && functions->isOpen()
            ? functions->dockWidget()->sizeInLayout().width()
            : 0;
    addDockWidget(dock, KDDockWidgets::Location_OnRight,
                  docks_.value(DisassemblyDock));
    if (functionsWidth)
      functions->dockWidget()->resizeInLayout(
          0, 0,
          functionsWidth - functions->dockWidget()->sizeInLayout().width(), 0);
    const int difference =
        docks_.value(DisassemblyDock)->dockWidget()->sizeInLayout().width() -
        dock->dockWidget()->sizeInLayout().width();
    dock->dockWidget()->resizeInLayout(difference / 2, 0, 0, 0);
  }
  dock->open();
  dock->raise();
  view->setRepresentation(representation);
  // Reusing the loaded function keeps its cursor and folding state. Tab is
  // the only ordinary cursor action that selects an address in the other pane.
  if (!selectAssemblyAddress || view->text()->function() != function ||
      view->text()->interrupted())
    view->showFunction(*function);
  if (selectAssemblyAddress)
    if (const auto address = disassembly_->currentAddress())
      view->text()->selectAddress(*address);
  view->text()->setFocus();
}

void MainWindow::toggleGraph() {
  disassembly_->setGraphMode(!disassembly_->graphMode());
}

void MainWindow::searchBinary(const QString &kind, bool again) {
  QString &last =
      kind == QLatin1String("text") ? lastTextSearch_ : lastByteSearch_;
  if (!again || last.isEmpty()) {
    bool ok = false;
    const auto text = QInputDialog::getText(
        this,
        kind == QLatin1String("text") ? tr("Text search") : tr("Binary search"),
        kind == QLatin1String("text") ? tr("String:")
                                      : tr("Hex bytes (e.g. 48 8B 05):"),
        QLineEdit::Normal, last, &ok);
    if (!ok || text.trimmed().isEmpty())
      return;
    last = text.trimmed();
  }
  const auto from = currentAddress().value_or(0);
  session_.read(
      QStringLiteral("search"),
      {{"kind", kind}, {"pattern", last}, {"limit", 4096}}, this,
      [this, from, kind](const QJsonObject &payload) {
        const auto items = payload.value("items").toArray();
        if (items.isEmpty()) {
          output_->append(tr("Search failed: %1 not found")
                              .arg(kind == QLatin1String("text")
                                       ? lastTextSearch_
                                       : lastByteSearch_),
                          1);
          return;
        }
        // Results list, and a jump to the next match in the search direction.
        auto *dock = chooserDock(ChooserKind::SearchResults);
        choosers_.value(int(ChooserKind::SearchResults))
            ->model()
            .setLocalRows(items);
        if (!dock->isOpen())
          addDockWidget(dock, KDDockWidgets::Location_OnBottom,
                        docks_.value(DisassemblyDock));
        dock->open();
        std::optional<Address> next;
        for (const auto &value : items) {
          const auto address = addressValue(value.toObject().value("address"));
          if (!address)
            continue;
          if (searchDown_ ? (*address > from && (!next || *address < *next))
                          : (*address < from && (!next || *address > *next)))
            next = address;
        }
        if (next)
          navigate(*next);
      });
}

void MainWindow::searchHighlight(bool forward) {
  const QString token = disassembly_->currentToken();
  if (!token.isEmpty())
    disassembly_->listing()->findInLoaded(token, forward);
}

void MainWindow::stepFunction(bool forward) {
  const auto address = currentAddress();
  if (!address)
    return;
  session_.read(
      QStringLiteral("functions"),
      {{"offset", 0},
       {"limit", 512},
       {"sort", "address"},
       {"descending", !forward},
       {"from", hexAddress(*address)}},
      this, [this, address, forward](const QJsonObject &payload) {
        for (const auto &value : payload.value("items").toArray()) {
          const auto entry = addressValue(value.toObject().value("address"));
          if (entry && (forward ? *entry > *address : *entry < *address)) {
            navigate(*entry);
            return;
          }
        }
      });
}

void MainWindow::showCalculator() {
  bool ok = false;
  const auto text =
      QInputDialog::getText(this, tr("Evaluate expression"), tr("Expression:"),
                            QLineEdit::Normal, {}, &ok);
  if (!ok || text.trimmed().isEmpty())
    return;
  resolveExpression(
      session_, this, text, currentAddress(),
      [this](std::optional<Address> value, const QString &error) {
        if (!value) {
          QMessageBox::warning(this, tr("Evaluate expression"), error);
          return;
        }
        const QString result =
            tr("Hex: %1h\nDecimal: %2\nOctal: %3\nSigned: %4")
                .arg(displayAddress(*value))
                .arg(*value)
                .arg(QString::number(*value, 8))
                .arg(qint64(*value));
        QMessageBox::information(this, tr("Evaluate expression"), result);
        output_->append(
            result.split(QLatin1Char('\n')).join(QStringLiteral("  ")), 0);
      });
}

void MainWindow::showAbout() {
  QMessageBox::about(
      this, tr("About NeverD"),
      tr("<h3>NeverD %1</h3><p>Interactive disassembler and decompiler "
         "workbench.</p><p>Licensed under the GNU Affero General Public "
         "License v3. Icons are original NeverD artwork.</p>"
         "<p><a href=\"https://github.com/NeverSight/NeverD\">"
         "github.com/NeverSight/NeverD</a></p>")
          .arg(QCoreApplication::applicationVersion()));
}

void MainWindow::chooseFont() {
  bool ok = false;
  const QFont font =
      QFontDialog::getFont(&ok, Theme::instance().codeFont(), this,
                           tr("Code font"), QFontDialog::MonospacedFonts);
  if (ok)
    Theme::instance().setCodeFont(font);
}

void MainWindow::chooseTheme() {
  QStringList names{tr("Dark (Visual Studio Code Dark+)"),
                    tr("Light (Visual Studio Code Light+)")};
  bool ok = false;
  const auto choice = QInputDialog::getItem(
      this, tr("Colors"), tr("Theme:"), names,
      Theme::instance().mode() == Theme::Mode::Dark ? 0 : 1, false, &ok);
  if (ok)
    Theme::instance().setMode(choice == names.front() ? Theme::Mode::Dark
                                                      : Theme::Mode::Light);
}

void MainWindow::showOptions() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("General options"));
  auto *form = new QFormLayout(&dialog);
  auto *bytes = new QSpinBox(&dialog);
  bytes->setRange(0, MaxOpcodeBytes);
  bytes->setValue(disassembly_->listing()->opcodeBytes());
  form->addRow(tr("Number of opcode bytes:"), bytes);
  auto *theme = new QComboBox(&dialog);
  theme->addItems({tr("Dark (Visual Studio Code Dark+)"),
                   tr("Light (Visual Studio Code Light+)")});
  theme->setCurrentIndex(Theme::instance().mode() == Theme::Mode::Dark ? 0 : 1);
  form->addRow(tr("Theme:"), theme);
  auto *prefixes =
      new QCheckBox(tr("Show segment:address line prefixes"), &dialog);
  prefixes->setChecked(true);
  form->addRow(prefixes);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  QSettings().setValue(OpcodeBytesKey, bytes->value());
  disassembly_->listing()->setOpcodeBytes(bytes->value());
  disassembly_->listing()->setShowPrefixes(prefixes->isChecked());
  Theme::instance().setMode(theme->currentIndex() == 0 ? Theme::Mode::Dark
                                                       : Theme::Mode::Light);
}

void MainWindow::showStringOptions() {
  const auto failed = [this](const QString &, const QString &message) {
    output_->append(tr("String options are unavailable: %1").arg(message), 2);
  };
  session_.read(
      QStringLiteral("string_encodings"), {}, this,
      [this, failed](const QJsonObject &encodings) {
        session_.read(
            QStringLiteral("string_options"), {}, this,
            [this, encodings](const QJsonObject &current) {
              stringOptionsDialog(encodings.value("items").toArray(), current);
            },
            failed);
      },
      failed);
}

void MainWindow::stringOptionsDialog(const QJsonArray &encodings,
                                     const QJsonObject &current) {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("String literals"));
  auto *form = new QFormLayout(&dialog);
  auto *choices = new QWidget(&dialog);
  auto *column = new QVBoxLayout(choices);
  column->setContentsMargins(0, 0, 0, 0);
  QStringList chosen;
  for (const auto &name : current.value("encodings").toArray())
    chosen.append(name.toString());
  QVector<QPair<QString, QCheckBox *>> boxes;
  // Code pages: those searched by default together, and the one preferred.
  QStringList common, commonSpellings;
  auto *preferred = new QComboBox(&dialog);
  preferred->addItem(tr("None"), QString());
  for (const auto &value : encodings) {
    const auto encoding = value.toObject();
    const auto name = encoding.value("name").toString();
    const auto spelling = encoding.value("spelling").toString();
    if (encoding.value("legacy").toBool()) {
      preferred->addItem(spelling.isEmpty() ? name : spelling, name);
      if (current.value("preferred").toString() == name)
        preferred->setCurrentIndex(preferred->count() - 1);
      if (encoding.value("default").toBool()) {
        common.append(name);
        commonSpellings.append(spelling.isEmpty() ? name : spelling);
      }
      continue;
    }
    // Plain ASCII has no listing spelling of its own.
    auto *box = new QCheckBox(
        spelling.isEmpty() ? QStringLiteral("ASCII") : spelling, choices);
    box->setChecked(chosen.contains(name));
    column->addWidget(box);
    boxes.append({name, box});
  }
  form->addRow(tr("Encodings:"), choices);
  auto *detect = new QCheckBox(tr("Detect common code pages"), &dialog);
  detect->setChecked(
      !common.isEmpty() &&
      std::all_of(common.begin(), common.end(),
                  [&](const QString &name) { return chosen.contains(name); }));
  detect->setToolTip(tr("Also read C strings that are not UTF-8 in %1, where "
                        "one of them reads them as text and no code page for "
                        "another script does")
                         .arg(commonSpellings.join(QStringLiteral(", "))));
  preferred->setToolTip(tr("Read C strings that are not UTF-8 in this code "
                           "page first, so that it wins where other code "
                           "pages read them too"));
  if (preferred->count() > 1) {
    if (!common.isEmpty())
      form->addRow(tr("Code pages:"), detect);
    else
      detect->hide();
    form->addRow(tr("Preferred code page:"), preferred);
  } else {
    detect->hide();
    preferred->hide();
  }
  auto *length = new QSpinBox(&dialog);
  length->setRange(1, MaxStringMinLength);
  length->setValue(current.value("min_length").toInt(DefaultStringMinLength));
  length->setToolTip(
      tr("Display columns: a wide East Asian character counts two"));
  form->addRow(tr("Minimum length:"), length);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  // At least one encoding stays selected.
  const auto update = [&] {
    buttons->button(QDialogButtonBox::Ok)
        ->setEnabled(preferred->currentIndex() > 0 || detect->isChecked() ||
                     std::any_of(boxes.begin(), boxes.end(), [](auto &box) {
                       return box.second->isChecked();
                     }));
  };
  for (auto &box : boxes)
    connect(box.second, &QCheckBox::toggled, &dialog, update);
  connect(detect, &QCheckBox::toggled, &dialog, update);
  connect(preferred, &QComboBox::currentIndexChanged, &dialog, update);
  update();
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  if (dialog.exec() != QDialog::Accepted)
    return;
  QStringList names;
  for (auto &box : boxes)
    if (box.second->isChecked())
      names.append(box.first);
  if (detect->isChecked())
    names.append(common);
  // A preferred code page alone searches only it.
  const auto page = preferred->currentData().toString();
  if (names.isEmpty())
    names.append(page);
  session_.setStringOptions(names, page, length->value());
}

void MainWindow::copySelection() {
  // The view holding the focus copies its selection: any code view, a
  // chooser's rows, the hex view's bytes or a text field; the disassembly
  // otherwise.
  for (QWidget *widget = QApplication::focusWidget(); widget;
       widget = widget->parentWidget()) {
    if (auto *field = qobject_cast<QLineEdit *>(widget)) {
      field->copy();
      return;
    }
    if (auto *log = qobject_cast<QPlainTextEdit *>(widget)) {
      log->copy();
      return;
    }
    if (auto *code = qobject_cast<CodeText *>(widget)) {
      QApplication::clipboard()->setText(code->selectedText());
      return;
    }
    if (auto *chooser = qobject_cast<ChooserView *>(widget)) {
      QApplication::clipboard()->setText(chooser->selectedText());
      return;
    }
    if (auto *hex = qobject_cast<HexView *>(widget)) {
      hex->copySelection();
      return;
    }
  }
  QApplication::clipboard()->setText(disassembly_->selectedText());
}

void MainWindow::showShortcuts() {
  QDialog dialog(this);
  dialog.setWindowTitle(tr("Shortcuts"));
  dialog.resize(560, 520);
  auto *layout = new QVBoxLayout(&dialog);
  auto *tree = new QTreeWidget(&dialog);
  tree->setHeaderLabels({tr("Action"), tr("Shortcut")});
  tree->setRootIsDecorated(false);
  for (auto *action : actions_.allActions()) {
    if (action->shortcut().isEmpty())
      continue;
    auto *item = new QTreeWidgetItem(tree);
    item->setIcon(0, action->icon());
    item->setText(0, action->text().remove(QLatin1Char('&')));
    item->setText(1, action->shortcut().toString(QKeySequence::NativeText));
  }
  tree->sortItems(0, Qt::AscendingOrder);
  tree->header()->resizeSection(0, 360);
  layout->addWidget(tree);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  dialog.exec();
}

void MainWindow::showCommandPalette() {
  QDialog dialog(this, Qt::Popup | Qt::FramelessWindowHint);
  dialog.setObjectName(QStringLiteral("commandPalette"));
  dialog.setWindowTitle(tr("Command palette"));
  dialog.resize(560, 420);
  auto *layout = new QVBoxLayout(&dialog);
  auto *filter = new QLineEdit(&dialog);
  filter->setPlaceholderText(tr("Type a command"));
  auto *list = new QListWidget(&dialog);
  // Shortcuts line up at the right of their commands.
  list->setItemDelegate(new SecondaryTextDelegate(std::nullopt, list));
  layout->addWidget(filter);
  layout->addWidget(list, 1);
  const auto populate = [this, list](const QString &text) {
    list->clear();
    for (auto *action : actions_.allActions()) {
      if (!action->isEnabled())
        continue;
      const QString label = action->text().remove(QLatin1Char('&'));
      if (!text.isEmpty() && !label.contains(text, Qt::CaseInsensitive))
        continue;
      auto *item = new QListWidgetItem(action->icon(), label, list);
      item->setData(SecondaryTextDelegate::SecondaryTextRole,
                    action->shortcut().toString(QKeySequence::NativeText));
      item->setData(Qt::UserRole, action->objectName());
    }
    if (list->count())
      list->setCurrentRow(0);
  };
  populate({});
  connect(filter, &QLineEdit::textChanged, &dialog, populate);
  connect(filter, &QLineEdit::returnPressed, &dialog, &QDialog::accept);
  connect(list, &QListWidget::itemActivated, &dialog, &QDialog::accept);
  filter->installEventFilter(&dialog);
  dialog.move(geometry().center() - QPoint(280, 240));
  if (dialog.exec() != QDialog::Accepted || !list->currentItem())
    return;
  const QString id = list->currentItem()->data(Qt::UserRole).toString();
  for (auto *action : actions_.allActions())
    if (action->objectName() == id) {
      lastPaletteCommand_ = id;
      action->trigger();
      return;
    }
}

void MainWindow::exportCurrent(bool pseudocode) {
  const auto function = currentFunction();
  if (!function)
    return;
  // The pseudocode keeps the language its window shows.
  auto *pseudocodeView =
      pseudocode ? codeView(CodeView::pseudocodeRepresentation()) : nullptr;
  const QString language =
      pseudocodeView ? pseudocodeView->text()->language() : QString();
  const QString suffix =
      !pseudocode                         ? QStringLiteral(".lst")
      : language == QLatin1String("rust") ? QStringLiteral(".rs")
      : language == QLatin1String("go")   ? QStringLiteral(".go")
                                          : QStringLiteral(".c");
  const auto path = QFileDialog::getSaveFileName(
      this, pseudocode ? tr("Create source file") : tr("Create LST file"),
      QFileInfo(session_.filePath()).completeBaseName() + suffix);
  if (path.isEmpty())
    return;
  if (pseudocode) {
    auto *view = pseudocodeView;
    view->showFunction(*function);
    QTimer::singleShot(0, this, [this, path, view] {
      QFile file(path);
      if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        output_->append(tr("Cannot write %1").arg(path), 2);
        return;
      }
      QTextStream(&file) << view->text()->allText() << '\n';
      output_->append(tr("Wrote %1").arg(path), 0);
    });
    return;
  }
  session_.read(
      QStringLiteral("listing"),
      {{"address", hexAddress(*function)}, {"before", 0}, {"after", 2000}},
      this, [this, path, function](const QJsonObject &payload) {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
          output_->append(tr("Cannot write %1").arg(path), 2);
          return;
        }
        QTextStream stream(&file);
        for (const auto &value : payload.value("lines").toArray()) {
          const auto line = value.toObject();
          const auto fn = addressValue(line.value("function_address"));
          if (line.value("kind").toString() == QLatin1String("insn") &&
              fn != function)
            break;
          stream << line.value("prefix").toString() << ' '
                 << line.value("text").toString() << '\n';
        }
        output_->append(tr("Wrote %1").arg(path), 0);
      });
}

void MainWindow::runCommand(const QString &command, const QString &argument) {
  if (command == QLatin1String("xrefs")) {
    if (argument.isEmpty())
      showCrossReferences(currentAddress(), true);
    else
      resolveExpression(
          session_, this, argument, currentAddress(),
          [this](std::optional<Address> value, const QString &error) {
            if (value)
              showCrossReferences(value, true);
            else
              output_->append(error, 2);
          });
  } else if (command == QLatin1String("rename")) {
    if (const auto function = currentFunction();
        function && !argument.isEmpty())
      session_.rename(*function, argument);
  } else if (command == QLatin1String("comment")) {
    if (const auto address = currentAddress())
      session_.setComment(*address, argument);
  } else if (command == QLatin1String("decompile")) {
    if (argument.isEmpty()) {
      showPseudocode(CodeView::pseudocodeRepresentation());
    } else {
      resolveExpression(
          session_, this, argument, currentAddress(),
          [this](std::optional<Address> value, const QString &error) {
            if (!value) {
              output_->append(error, 2);
              return;
            }
            navigate(*value);
            showPseudocode(CodeView::pseudocodeRepresentation());
          });
    }
  } else if (command == QLatin1String("find")) {
    QString pattern = argument;
    const bool text = pattern.startsWith(QLatin1Char('"'));
    if (text)
      pattern =
          pattern.mid(1).chopped(pattern.endsWith(QLatin1Char('"')) ? 1 : 0);
    (text ? lastTextSearch_ : lastByteSearch_) = pattern;
    searchBinary(text ? QStringLiteral("text") : QStringLiteral("bytes"), true);
  } else if (command == QLatin1String("analyze")) {
    session_.analyzeWholeProgram();
  } else if (command == QLatin1String("save")) {
    session_.save();
  } else if (command == QLatin1String("graph")) {
    toggleGraph();
  } else if (command == QLatin1String("hex")) {
    actions_.action(ActionId::ViewHex)->trigger();
  } else if (command == QLatin1String("strref")) {
    actions_.action(ActionId::SearchStringReferences)->trigger();
    if (!argument.isEmpty())
      choosers_.value(int(ChooserKind::StringReferences))
          ->setFilterText(argument);
  }
}

void MainWindow::contextMenu(const QPoint &globalPosition) {
  QMenu menu(this);
  for (const auto id : {ActionId::JumpOperand, ActionId::JumpXrefOperand,
                        ActionId::JumpXrefsTo, ActionId::JumpXrefsFrom})
    menu.addAction(actions_.action(id));
  menu.addSeparator();
  for (const auto id :
       {ActionId::EditRename, ActionId::EditComment, ActionId::EditBookmark})
    menu.addAction(actions_.action(id));
  for (const auto id :
       {ActionId::EditCreateFunction, ActionId::EditDeleteFunction,
        ActionId::EditDefineCode, ActionId::EditDefineData,
        ActionId::EditDefineString, ActionId::EditUndefine})
    if (auto *action = actions_.action(id); action->isEnabled())
      menu.addAction(action);
  menu.addSeparator();
  for (const auto id : {ActionId::ViewToggleGraph, ActionId::ViewPseudocode,
                        ActionId::ViewMedIR})
    menu.addAction(actions_.action(id));
  menu.addSeparator();
  menu.addAction(actions_.action(ActionId::EditCopy));
  menu.addAction(actions_.action(ActionId::EditCopyAddress));
  // A code window's folds: the declarations before the function and the
  // recognized library operations.
  if (auto *code = focusedCodeView()) {
    CodeText *text = code->text();
    menu.addSeparator();
    if (text->hasPrelude()) {
      const bool folded = text->preludeFolded();
      auto *declarations = menu.addAction(folded ? tr("Expand declarations")
                                                 : tr("Collapse declarations"));
      declarations->setStatusTip(
          tr("Show or hide the includes and declarations before the function "
             "(Keypad + / Keypad -)"));
      connect(declarations, &QAction::triggered, text,
              [text, folded] { text->setPreludeFolded(!folded); });
    }
    if (text->foldableCount() > 0) {
      const bool folded = text->libraryFolded();
      auto *library =
          menu.addAction(folded ? tr("Expand library operations")
                                : tr("Collapse library operations"));
      connect(library, &QAction::triggered, text,
              [text, folded] { text->setFolded(!folded); });
    }
  }
  menu.addSeparator();
  menu.addAction(actions_.action(ActionId::OptionsFont));
  menu.exec(globalPosition);
}

//===----------------------------------------------------------------------===//
// Desktop
//===----------------------------------------------------------------------===//

void MainWindow::saveDesktop() {
  KDDockWidgets::LayoutSaver().saveToFile(layoutPath());
  output_->append(tr("Desktop saved"), 0);
}

void MainWindow::restoreDesktop() {
  if (!KDDockWidgets::LayoutSaver().restoreFromFile(layoutPath()))
    output_->append(tr("No saved desktop"), 1);
}

void MainWindow::resetDesktop() {
  for (auto *dock : std::as_const(docks_))
    dock->forceClose();
  applyDefaultLayout();
}

void MainWindow::cycleWindows(bool forward) {
  QList<Dock *> open;
  for (auto *dock : std::as_const(docks_))
    if (dock->isOpen() && dock->widget() && dock->widget()->isVisible())
      open.append(dock);
  if (open.isEmpty())
    return;
  int current = -1;
  for (int i = 0; i < open.size(); ++i)
    if (open[i]->isAncestorOf(QApplication::focusWidget()))
      current = i;
  const int next = (current + (forward ? 1 : -1) + open.size()) % open.size();
  open[next]->raise();
  open[next]->widget()->setFocus(Qt::TabFocusReason);
}

void MainWindow::scheduleQuickStart() {
  if (startupQuickStartPending_)
    return;
  startupQuickStartPending_ = true;
  QTimer::singleShot(0, this, [this] {
    if (!std::exchange(startupQuickStartPending_, false) || session_.loaded() ||
        session_.opening() || quickStart_ || quitting_)
      return;
    showQuickStart();
  });
}

void MainWindow::showQuickStart() {
  startupQuickStartPending_ = false;
  QuickStartDialog dialog(this);
  quickStart_ = &dialog;
  if (dialog.exec() != QDialog::Accepted)
    return;
  if (dialog.start() == QuickStartDialog::Start::New)
    openDialog();
  else if (dialog.start() == QuickStartDialog::Start::Previous)
    openFile(dialog.file());
}

QHash<QString, QByteArray> MainWindow::projectState() const {
  QHash<QString, QByteArray> state;
  QJsonObject location;
  if (const auto address = currentAddress())
    location.insert(QStringLiteral("address"), hexAddress(*address));
  if (const auto function = currentFunction())
    location.insert(QStringLiteral("function"), hexAddress(*function));
  location.insert(QStringLiteral("graph"), disassembly_->graphMode());
  state.insert(QString::fromLatin1(LocationState),
               QJsonDocument(location).toJson(QJsonDocument::Compact));
  state.insert(QString::fromLatin1(BookmarksState),
               QJsonDocument(bookmarks()).toJson(QJsonDocument::Compact));
  state.insert(QString::fromLatin1(DesktopState),
               KDDockWidgets::LayoutSaver().serializeLayout());
  return state;
}

void MainWindow::restoreProjectState() {
  const auto &state = session_.databaseState();
  const auto location =
      QJsonDocument::fromJson(state.value(QString::fromLatin1(LocationState)))
          .object();
  if (const auto bookmarks = QJsonDocument::fromJson(
          state.value(QString::fromLatin1(BookmarksState)));
      bookmarks.isArray() && !bookmarks.array().isEmpty())
    setBookmarks(bookmarks.array());
  if (const auto desktop = state.value(QString::fromLatin1(DesktopState));
      !desktop.isEmpty() &&
      !KDDockWidgets::LayoutSaver().restoreLayout(desktop))
    output_->append(
        tr("The saved desktop of this database could not be restored."), 1);
  const auto address = addressValue(location.value(QStringLiteral("address")));
  initialAddress_ = address.value_or(session_.entryAddress());
  restoreGraph_ = location.value(QStringLiteral("graph")).toBool();
  navigateInitialAddress();
}

void MainWindow::navigateInitialAddress() {
  if (!initialAddress_ || !session_.loaded() || space_.empty())
    return;
  Address address = *initialAddress_;
  initialAddress_.reset();
  if (!space_.regionOf(address)) {
    address = space_.first();
    for (const auto &region : space_.regions())
      if (region.exec) {
        address = region.start;
        break;
      }
  }
  // This is a browsing position, never a reconstructed program entry point.
  disassembly_->navigate(address, false);
  if (restoreGraph_) {
    const auto epoch = session_.epoch();
    QTimer::singleShot(0, this, [this, epoch] {
      if (session_.loaded() && session_.epoch() == epoch &&
          !disassembly_->graphMode())
        toggleGraph();
    });
  }
  restoreGraph_ = false;
}

QJsonArray MainWindow::bookmarks() const {
  QJsonArray rows;
  for (const auto &row : QSettings().value(bookmarksKey()).toList())
    rows.append(QJsonObject::fromVariantMap(row.toMap()));
  return rows;
}

void MainWindow::setBookmarks(const QJsonArray &rows) {
  QVariantList list;
  for (const auto &row : rows)
    list.append(row.toObject().toVariantMap());
  QSettings().setValue(bookmarksKey(), list);
  if (auto *chooser = choosers_.value(int(ChooserKind::Bookmarks)))
    chooser->model().setLocalRows(rows);
}

QString MainWindow::bookmarksKey() const {
  return QStringLiteral("bookmarks/") +
         QString::fromLatin1(
             QCryptographicHash::hash(session_.projectPath().toUtf8(),
                                      QCryptographicHash::Sha1)
                 .toHex());
}

void MainWindow::showConnections() {
  ConnectionsDialog dialog(session_, mcp_, broker_, this);
  dialog.exec();
}

void MainWindow::changeEvent(QEvent *event) {
  if (event->type() == QEvent::LanguageChange)
    retranslateUi();
  KDDockWidgets::QtWidgets::MainWindow::changeEvent(event);
}

void MainWindow::retranslateUi() {
  actions_.retranslate();
  for (auto it = docks_.cbegin(); it != docks_.cend(); ++it) {
    if (const char *title = dockTitles_.value(it.key()))
      it.value()->setTitle(tr(title));
    else if (auto *chooser = qobject_cast<ChooserView *>(it.value()->widget()))
      it.value()->setTitle(chooser->model().title());
  }
  retitleCodeDocks();
  disassembly_->setSyncName(tr("Hex View-1"));
  updateTitle();
  updateStatusBar();
}

void MainWindow::closeEvent(QCloseEvent *event) {
  if (!quitting_ && !session_.requestQuit()) {
    event->ignore();
    return;
  }
  // The database remembers where the user was, like the desktop does.
  if (session_.loaded())
    session_.saveDatabaseState();
  KDDockWidgets::LayoutSaver().saveToFile(layoutPath());
  event->accept();
  KDDockWidgets::QtWidgets::MainWindow::closeEvent(event);
}

} // namespace neverd::gui
