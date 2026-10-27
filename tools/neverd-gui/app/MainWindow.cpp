#include "MainWindow.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "ChooserView.h"
#include "CodeView.h"
#include "ComparisonDialog.h"
#include "ConnectionsDialog.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "DwarfPromptDialog.h"
#include "ExportDataDialog.h"
#include "ExtensionsView.h"
#include "FindInViewDialog.h"
#include "FocusSidebar.h"
#include "FunctionEditDialog.h"
#include "GnomeWindowEdges.h"
#include "GraphView.h"
#include "HexView.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "JumpDialog.h"
#include "Language.h"
#include "LineageDialog.h"
#include "ListingView.h"
#include "LoadFileDialog.h"
#include "LocalTypesView.h"
#include "MapPromptDialog.h"
#include "MicrosoftSymbols.h"
#include "ModelessDialog.h"
#include "NavigationBand.h"
#include "OutputWindow.h"
#include "PathfinderView.h"
#include "PdbPromptDialog.h"
#include "ProjectDatabase.h"
#include "QuickStartDialog.h"
#include "Resolve.h"
#include "SecondaryTextDelegate.h"
#include "Session.h"
#include "SetTypeDialog.h"
#include "SettingsKeys.h"
#include "ShortcutHint.h"
#include "SymbolTablePromptDialog.h"
#include "Theme.h"
#include "TitleBar.h"
#include "TypeLibrariesView.h"
#include "TypePickerDialog.h"
#include "UnwindInfoDialog.h"
#include "WindowFrame.h"
#include "X87CapabilitiesDialog.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"
#include "web/PackageReviewDialog.h"

#include "neverd/sdk/Limits.h"
#include "neverd/support/NumericInput.h"

#include <QAbstractTableModel>
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
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
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
#include <QSaveFile>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QScreen>
#include <QSet>
#include <QSettings>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStorageInfo>
#include <QStyle>
#include <QTableView>
#include <QTableWidget>
#include <QTextStream>
#include <QToolBar>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <kddockwidgets/Config.h>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/core/DockWidget.h>
#include <kddockwidgets/core/DropArea.h>
#include <kddockwidgets/core/FloatingWindow.h>
#include <kddockwidgets/qtcommon/View.h>
#include <kddockwidgets/qtwidgets/views/FloatingWindow.h>

namespace neverd::gui {
namespace {
using KDDockWidgets::InitialOption;

constexpr char LayoutFileName[] = "desktop.json";
// Application settings own main-window geometry. Automatic pane restoration
// must neither show the unfinished startup window nor unmaximize a visible one.
constexpr KDDockWidgets::RestoreOptions kPaneRestoreOptions{
    KDDockWidgets::RestoreOption_RelativeToMainWindow,
    KDDockWidgets::RestoreOption_AbsoluteFloatingDockWindows};
/// The level of a session message that reports an error.
constexpr int ErrorMessageLevel = 2;
constexpr char OpcodeBytesKey[] = "listing/opcodeBytes";
// Whether the code windows and the disassembly follow each other's cursor.
constexpr char SynchronizeCodeKey[] = "code/synchronize";
constexpr char AnalysisIndicatorKey[] = "analysis/indicator";
constexpr char BinaryProcessorKey[] = "load/binaryProcessor";
constexpr char DocumentationUrl[] =
    "https://github.com/NeverSight/NeverD/blob/dev/docs/gui.md";
// Default proportions of the side and bottom windows, clamped in pixels.
constexpr double FunctionsShare = 0.21;
constexpr int
    MinimumFunctionsWidth =
        ::neverd::limits::gui_config::kMainWindowMinimumFunctionsWidth,
    MaximumFunctionsWidth =
        ::neverd::limits::gui_config::kMainWindowMaximumFunctionsWidth;
constexpr double OutputShare = 0.19;
constexpr int MinimumOutputHeight =
                  ::neverd::limits::gui_config::kMainWindowMinimumOutputHeight,
              MaximumOutputHeight =
                  ::neverd::limits::gui_config::kMainWindowMaximumOutputHeight;
constexpr int StatusRefreshMs = 30000;

// Dock identifiers; titles follow the classic window names.
constexpr char FunctionsDock[] = "functions";
constexpr char DisassemblyDock[] = "disassembly-a";
constexpr char HexDock[] = "hex-1";
constexpr char OutputDock[] = "output";
constexpr char OverviewDock[] = "graph-overview";
constexpr char PseudocodeDock[] = "pseudocode-a";
constexpr char RepresentationDock[] = "ir-a";
constexpr char ExtensionsDock[] = "extensions";
constexpr char LocalTypesDock[] = "local-types";
constexpr char TypeLibrariesDock[] = "type-libraries";
constexpr char PathfinderDock[] = "pathfinder";

QString chooserDockId(ChooserKind kind) {
  return QStringLiteral("chooser-%1").arg(int(kind));
}

/// A popup must consume shortcuts before its owning analysis pane sees them.
class CommandPaletteDialog final : public QDialog {
public:
  using QDialog::QDialog;

protected:
  bool event(QEvent *event) override {
    if (event->type() == QEvent::ShortcutOverride) {
      event->accept();
      return true;
    }
    return QDialog::event(event);
  }
};

/// Browse references while keeping the originating analysis pane usable.
class ReferencesDialog final : public QDialog {
public:
  ReferencesDialog(Session &session, const AddressSpace &space,
                   const QString &title, const QJsonObject &request,
                   QWidget *parent)
      : QDialog(parent),
        view_(new ChooserView(session, space, ChooserKind::CrossReferences,
                              this)) {
    setObjectName(QStringLiteral("referencesDialog"));
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

/// A local's source occurrences already belong to the loaded code document.
/// Keep their rows in a model so a large function need not allocate table
/// widgets or trigger a binary reference-index query for every occurrence.
class LocalReferencesModel final : public QAbstractTableModel {
public:
  LocalReferencesModel(QVector<CodeText::LocalReference> references,
                       QStringList headers, QObject *parent)
      : QAbstractTableModel(parent), references_(std::move(references)),
        headers_(std::move(headers)) {}
  int rowCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : int(references_.size());
  }
  int columnCount(const QModelIndex &parent = {}) const override {
    return parent.isValid() ? 0 : 3;
  }
  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() >= references_.size() ||
        index.column() >= 3)
      return {};
    const auto &reference = references_[index.row()];
    if (role == Qt::DisplayRole) {
      if (index.column() == 0)
        return reference.line + 1;
      if (index.column() == 1)
        return reference.column + 1;
      return reference.text;
    }
    if (role == Qt::ToolTipRole)
      return reference.text;
    if (role == Qt::FontRole && index.column() == 2)
      return Theme::instance().codeFont();
    if (role == Qt::TextAlignmentRole && index.column() < 2)
      return int(Qt::AlignRight | Qt::AlignVCenter);
    return {};
  }
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override {
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole &&
        section >= 0 && section < headers_.size())
      return headers_[section];
    return {};
  }
  const CodeText::LocalReference *reference(int row) const {
    return row >= 0 && row < references_.size() ? &references_[row] : nullptr;
  }

private:
  QVector<CodeText::LocalReference> references_;
  QStringList headers_;
};
} // namespace

MainWindow::MainWindow(Session &session, McpConnectionManager &mcp,
                       GuiSessionBroker &broker)
    : KDDockWidgets::QtWidgets::MainWindow(QStringLiteral("NeverDMainWindow")),
      session_(session), mcp_(mcp), broker_(broker), actions_(this) {
  instance_ = this;
  synchronizeCode_ = QSettings().value(SynchronizeCodeKey, true).toBool();
  frame_ = WindowFrame::attach(*this);
  setWindowIcon(icon(QStringLiteral("app")));
  setCenterWidgetMargins(dockAreaMargins());
  setAcceptDrops(true);
  // Mouse Back/Forward buttons walk the navigation history anywhere inside.
  qApp->installEventFilter(this);
  buildMenusAndToolbars();
  buildDocks();
  buildStatusBar();
  connectSession();
  microsoftSymbols_ = new MicrosoftSymbols(session_, this);
  connect(&session_, &Session::stateChanged, this, [this] {
    if (debugPromptPending_ && session_.connected()) {
      auto [path, options] = *std::exchange(debugPromptPending_, std::nullopt);
      openAskingForDebugInfo(path, options);
    }
  });
  // A worker that cannot start leaves no question to ask; the workbench
  // shows the error instead of staying hidden.
  connect(&session_, &Session::message, this,
          [this](const QString &, int level) {
            if (level >= ErrorMessageLevel && debugPromptPending_ &&
                !session_.connected()) {
              auto [path, options] =
                  *std::exchange(debugPromptPending_, std::nullopt);
              openNow(path, options);
            }
          });
  connectActions();
  focusSequenceTimer_.setSingleShot(true);
  focusSequenceTimer_.setInterval(
      ::neverd::limits::gui_config::kFocusModeSequenceTimeoutMs);
  connect(&focusSequenceTimer_, &QTimer::timeout, this,
          &MainWindow::resetFocusSequence);
  connect(qApp, &QApplication::focusChanged, this,
          [this] { resetFocusSequence(); });
  // Other programs find this window and open links in it, as in IDA 9.4.
  instances_.setInfo([this] { return instanceInfo(); });
  instances_.setLinkHandler(
      [this](const DeepLink &link, std::function<void(const QString &)> done) {
        if (isMinimized())
          setWindowState(windowState() & ~Qt::WindowMinimized);
        raise();
        activateWindow();
        openLink(link, std::move(done));
      });
  if (!instances_.start() &&
      !QSettings().value(settings::InstancesDisabled, false).toBool())
    output_->append(tr("Other programs cannot open links in this window: %1")
                        .arg(instances_.errorString()),
                    1);
  statusTimer_.setInterval(StatusRefreshMs);
  connect(&statusTimer_, &QTimer::timeout, this, &MainWindow::updateStatusBar);
  statusTimer_.start();
  updateActions();
  updateTitle();
  updateStatusBar();
}

bool MainWindow::eventFilter(QObject *object, QEvent *event) {
#ifdef Q_OS_MACOS
  // Cocoa reports Dock reopen even when the application is already active.
  // AppKit does not restore our frameless greeting as a native main window.
  if (object == qApp && event->type() == QEvent::ApplicationActivate &&
      quickStart_ && quickStart_->isMinimized())
    showQuickStart();
#endif
  if (event->type() == QEvent::Close && focusSidebar_ &&
      object == focusSidebar_->window()) {
    // Closing the temporary workspace exits focus mode instead of closing
    // all its docks. Restore after the caption/desktop close callback unwinds.
    event->ignore();
    const QPointer<FocusSidebar> sidebar = focusSidebar_;
    QTimer::singleShot(0, this, [this, sidebar] {
      if (sidebar && sidebar == focusSidebar_)
        leaveFocusMode();
    });
    return true;
  }
  const auto inWorkbench = [this](QWidget *widget) {
    if (!widget)
      return false;
    if (widget->window() == this)
      return true;
    // Floating docks belong to this session, but have their own window.
    return std::any_of(docks_.cbegin(), docks_.cend(), [widget](auto *dock) {
      return widget == dock || dock->isAncestorOf(widget);
    });
  };
  if (event->type() == QEvent::ShortcutOverride ||
      event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    auto *widget = qobject_cast<QWidget *>(object);
    bool analysis = false;
    if (inWorkbench(widget) && !QApplication::activeModalWidget())
      for (auto *parent = widget; parent; parent = parent->parentWidget()) {
        if (qobject_cast<QLineEdit *>(parent) ||
            qobject_cast<QPlainTextEdit *>(parent) ||
            qobject_cast<QComboBox *>(parent))
          break;
        if (qobject_cast<ListingView *>(parent) ||
            qobject_cast<GraphView *>(parent) ||
            qobject_cast<CodeText *>(parent)) {
          analysis = true;
          break;
        }
      }
    const bool sequenceKey =
        analysis && key->key() == Qt::Key_F &&
        key->modifiers() == Qt::NoModifier &&
        actions_.action(ActionId::ViewFocusMode)->isEnabled();
    if (sequenceKey) {
      // Handle deliberate taps here so native auto-repeat cannot complete
      // the multi-key shortcut, and text inputs keep their ordinary typing.
      key->accept();
      if (event->type() == QEvent::ShortcutOverride || key->isAutoRepeat())
        return true;
      if (focusSequenceView_ != QApplication::focusWidget())
        resetFocusSequence();
      focusSequenceView_ = QApplication::focusWidget();
      if (++focusSequenceCount_ ==
          ::neverd::limits::gui_config::kFocusModeSequenceLength) {
        resetFocusSequence();
        actions_.action(ActionId::ViewFocusMode)->trigger();
      } else {
        focusSequenceTimer_.start();
      }
      return true;
    }
    // Native keyboard input first reaches a QWindow, then its focus widget.
    // Only the final widget delivery may break a sequence; clearing on the
    // QWindow delivery made each real tap start over at one.
    if (widget && focusSequenceCount_)
      resetFocusSequence();
  }
  if (event->type() == QEvent::MouseButtonPress && focusSequenceCount_)
    resetFocusSequence();
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
    const bool workbench = inWorkbench(widget) || quickStart;
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
        // A file dropped on the quick start starts as New does.
        const QPointer<QuickStartDialog> start =
            quickStart ? qobject_cast<QuickStartDialog *>(quickStart_.data())
                       : nullptr;
        QTimer::singleShot(0, this, [this, path, start] {
          if (start)
            start->startNew(path);
          else
            chooseLoader(path);
        });
      }
      return true;
    }
  }
  if (event->type() == QEvent::MouseButtonPress) {
    auto *widget = qobject_cast<QWidget *>(object);
    const auto button = static_cast<QMouseEvent *>(event)->button();
    if (inWorkbench(widget) &&
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
      input->setMinimumWidth(
          ::neverd::limits::gui_config::kMainWindowInputDialogWidth);
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
    // A completed document keeps its exact instruction across tab changes.
    // Only an active request needs to be stopped while its pane is hidden.
    if (event->type() == QEvent::Hide && code->text()->loading())
      code->text()->cancel();
  }
  return KDDockWidgets::QtWidgets::MainWindow::eventFilter(object, event);
}

MainWindow::~MainWindow() {
  qApp->removeEventFilter(this);
  // The startup greeting has no widget parent: it is the application's only
  // visible window until a start is chosen.
  delete quickStart_.data();
  // View teardown releases queries and moves the focus; keep the resulting
  // notifications away from this window.
  disconnect(&session_, nullptr, this, nullptr);
  disconnect(&broker_, nullptr, this, nullptr);
  disconnect(qApp, &QApplication::focusChanged, this, nullptr);
  delete focusToolbar_.data();
  focusFunctions_.clear();
  delete focusSidebar_.data();
  // Hiding can lay out child models. Their borrowed address space and the
  // other window members must still exist while that happens.
  hide();
  for (auto *dialog : findChildren<QDialog *>(Qt::FindDirectChildrenOnly))
    delete dialog;
  // Destroy every dock while its views' borrowed state is alive, including
  // closed and floating docks that are not children of this window.
  for (auto *dock : std::as_const(docks_))
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
  menuBar_ = new QMenuBar(this);
  menuBar_->setNativeMenuBar(false);
  if (frame_) {
    titleBar_ = new TitleBar(*this, menuBar_);
    titleBar_->setCommandCenterAction(
        actions_.action(ActionId::OptionsPalette));
    setMenuWidget(titleBar_);
  } else {
    setMenuBar(menuBar_);
  }
  actions_.buildMenus(menuBar_, [this](QMenu *menu, const QString &name) {
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
    openPendingLink();
  });
  Q_UNUSED(toolbars);
}

void MainWindow::fillPlaceholder(QMenu *menu, const QString &name) {
  if (name == QLatin1String("Patch")) {
    connect(menu, &QMenu::aboutToShow, this,
            [this, menu] { preparePatchMenu(menu); });
    return;
  }
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
        connect(action, &QAction::triggered, this, [this, dock] {
          openDock(dock, true);
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
  actions_.attachWindowShortcuts(dock);
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
  if (name.startsWith(QLatin1String("disassembly-")))
    return disassemblyDock(name);
  for (const auto &prefix :
       {QStringLiteral("pseudocode-"), QStringLiteral("ir-")})
    if (name.startsWith(prefix)) {
      const auto suffix = name.mid(prefix.size());
      bool valid = false;
      const int number = suffix.toInt(&valid);
      if (suffix != QLatin1String("a") &&
          (!valid || number < 1 || QString::number(number) != suffix))
        return nullptr;
      return codeDock(name, prefix == QLatin1String("pseudocode-")
                                ? CodeView::pseudocodeRepresentation()
                                : QStringLiteral("med"));
    }
  if (name == QLatin1String(ExtensionsDock)) {
    extensions_ = new ExtensionsView(session_);
    extensions_->setLocationProvider([this] { return currentAddress(); });
    return makeDock(name, QT_TR_NOOP("Analysis queries"),
                    QStringLiteral("search_text"), extensions_);
  }
  if (name == QLatin1String(LocalTypesDock)) {
    localTypes_ = new LocalTypesView(session_);
    return makeDock(name, QT_TR_NOOP("Local Types"),
                    QStringLiteral("local_types"), localTypes_);
  }
  if (name == QLatin1String(TypeLibrariesDock)) {
    typeLibraries_ = new TypeLibrariesView(session_);
    return makeDock(name, QT_TR_NOOP("Type Libraries"),
                    QStringLiteral("type_libraries"), typeLibraries_);
  }
  if (name == QLatin1String(PathfinderDock)) {
    pathfinder_ = new PathfinderView(session_, space_);
    connect(pathfinder_, &PathfinderView::activated, this,
            [this](Address address) { navigate(address); });
    // A desktop restored with the project shows the project's path.
    if (session_.loaded())
      pathfinder_->restoreState(pathfinderState_);
    return makeDock(name, QT_TR_NOOP("Pathfinder"),
                    QStringLiteral("pathfinder"), pathfinder_);
  }
  for (int kind = 0; kind < int(ChooserKind::Count); ++kind)
    if (name == chooserDockId(ChooserKind(kind)))
      return chooserDock(ChooserKind(kind));
  return nullptr;
}

void MainWindow::openDock(Dock *dock, bool tabbed,
                          KDDockWidgets::Location location) {
  if (!dock)
    return;
  if (!dock->isOpen()) {
    // Reuse the last embedded position, even if the user closed a floating
    // window. A pane that is already open keeps its current placement.
    dock->setFloating(false);
    if (dock->QWidget::window() != this) {
      auto *anchor = docks_.value(DisassemblyDock);
      if (!anchor || anchor == dock || !anchor->isOpen() ||
          anchor->QWidget::window() != this)
        anchor = nullptr;
      if (tabbed && anchor)
        anchor->addDockWidgetAsTab(dock);
      else
        addDockWidget(dock, location,
                      location == KDDockWidgets::Location_OnRight ? anchor
                                                                  : nullptr);
    }
  }
  dock->open();
  dock->raise();
}

void MainWindow::buildDocks() {
  disassembly_ = new DisassemblyView(session_, space_);
  primaryDisassembly_ = disassembly_;
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

  connectFunctions(functions_);
  connectDisassembly(disassembly_);
  connect(hex_, &HexView::locationChanged, this, [this](Address address) {
    hexFollower_ = disassembly_;
    disassembly_->navigate(address, false);
  });
  connect(&session_, &Session::unloaded, this,
          [this] { hexFollower_.clear(); });
  hex_->setExportAction(actions_.action(ActionId::EditExportData));
  connect(output_, &OutputWindow::navigateRequested, this,
          [this](Address address) { jump(address); });
  // The output window and dialogs act on the last analysis view used.
  connect(
      qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
        for (auto it = docks_.cbegin(); it != docks_.cend(); ++it)
          if (*it && (*it)->isAncestorOf(now)) {
            // Choosers and auxiliary windows keep the analysis context.
            if (auto *view = qobject_cast<CodeView *>((*it)->widget())) {
              lastCodeView_ = view;
              tabCodeView_ = view;
            } else if (auto *view =
                           qobject_cast<DisassemblyView *>((*it)->widget())) {
              disassembly_ = view;
              overview_->setGraph(view->graph());
              lastCodeView_.clear();
            } else if (it.key() == QLatin1String(HexDock))
              lastCodeView_.clear();
            if (it.key() != QLatin1String(OutputDock))
              hexActive_ = it.key() == QLatin1String(HexDock);
            if (!hexActive_)
              hexFollower_.clear();
            // What the commands act on follows the view: a pseudocode
            // window converts a parameter, the disassembly an operand.
            updateActions();
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

void MainWindow::connectDisassembly(DisassemblyView *view) {
  connect(view->listing(), &ListingView::jumpSettled, this, [this, view] {
    if (hexFollower_ == view)
      hexFollower_.clear();
  });
  connect(view, &DisassemblyView::locationChanged, this,
          [this, view](Address address) {
            if (view == disassembly_)
              synchronize(address, view);
          });
  connect(view, &DisassemblyView::navigateRequested, this,
          [this, view](Address address) {
            disassembly_ = view;
            overview_->setGraph(view->graph());
            navigate(address);
          });
  connect(view, &DisassemblyView::contextMenuRequested, this,
          [this, view](const QPoint &point) {
            disassembly_ = view;
            overview_->setGraph(view->graph());
            contextMenu(point);
          });
  connect(view, &DisassemblyView::historyChanged, this, [this, view] {
    if (view != disassembly_)
      return;
    if (!codeHistoryNavigation_)
      ++codeNavigationSerial_;
    initialAddress_.reset();
    restoreGraph_ = false;
    updateActions();
  });
  connect(view, &DisassemblyView::graphModeChanged, this,
          [this, view](bool graph) {
            if (graph && view == disassembly_)
              showOverview();
          });
  connect(view->graph(), &GraphView::statusChanged, this, [this, view] {
    // The graph's initial cursor arrives after focus and mode changed.
    if (view == disassembly_ && view->graphMode())
      updateActions();
  });
}

void MainWindow::connectFunctions(ChooserView *view) {
  const auto activate = [this, view](Address address) {
    if (view == focusFunctions_ && pseudocode_ && pseudocode_->isVisible())
      lastCodeView_ = pseudocode_;
    activateFunction(address);
  };
  connect(view, &ChooserView::activated, this, activate);
  if (view == focusFunctions_)
    connect(view->table(), &QTreeView::clicked, this,
            [view, activate](const QModelIndex &index) {
              if (const auto address = view->model().addressAt(index.row()))
                activate(*address);
            });
  connect(view, &ChooserView::commandRequested, this,
          [this](ChooserCommand command, Address entry) {
            if (command == ChooserCommand::Edit)
              editFunction(entry);
          });
  view->table()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(
      view->table(), &QWidget::customContextMenuRequested, this,
      [this, view](const QPoint &position) { functionsMenu(view, position); });
}

MainWindow::Dock *MainWindow::disassemblyDock(const QString &id) {
  if (auto *dock = docks_.value(id))
    return dock;
  bool valid = false;
  const int number =
      id.mid(QStringLiteral("disassembly-").size()).toInt(&valid);
  if (!id.startsWith(QLatin1String("disassembly-")) || !valid || number < 1 ||
      number > 64)
    return nullptr;
  auto *view = new DisassemblyView(session_, space_);
  view->setSyncName(tr("Hex View-1"));
  actions_.attachViewShortcuts(view);
  connectDisassembly(view);
  auto *dock = makeDock(id, nullptr, QStringLiteral("text_view"), view);
  connect(dock, &Dock::isOpenChanged, this, [this, view](bool open) {
    if (!open && disassembly_ == view) {
      disassembly_ = primaryDisassembly_;
      overview_->setGraph(disassembly_->graph());
      updateActions();
    }
  });
  dock->setTitle(tr("NeverD View-%1")
                     .arg(number < 26 ? QString(QChar('A' + number))
                                      : QString::number(number + 1)));
  return dock;
}

void MainWindow::jumpNewWindow(Address target) {
  int number = 1;
  while (docks_.contains(QStringLiteral("disassembly-%1").arg(number)))
    ++number;
  auto *dock = disassemblyDock(QStringLiteral("disassembly-%1").arg(number));
  if (!dock) {
    output_->append(tr("Cannot open more disassembly views."), 1);
    return;
  }
  auto *view = qobject_cast<DisassemblyView *>(dock->widget());
  openDock(dock, false, KDDockWidgets::Location_OnRight);
  disassembly_ = view;
  overview_->setGraph(view->graph());
  view->navigate(target);
  view->focusContent();
}

MainWindow::Dock *MainWindow::chooserDock(ChooserKind kind) {
  const QString id = chooserDockId(kind);
  if (auto *dock = docks_.value(id))
    return dock;
  auto *view = new ChooserView(session_, space_, kind);
  choosers_.insert(int(kind), view);
  if (kind == ChooserKind::PatchedBytes)
    connectPatchedBytes(view);
  connect(view, &ChooserView::activated, this,
          [this](Address address) { navigate(address); });
  // Lists reload when a session opens; one first shown later loads now.
  if (session_.loaded())
    view->model().reload();
  auto *dock = makeDock(id, nullptr, view->model().iconName(), view);
  dock->setTitle(view->model().title());
  return dock;
}

CodeView *MainWindow::codeView(const QString &representation, bool newWindow) {
  const bool c = CodeView::isSource(representation);
  QString id = c ? PseudocodeDock : RepresentationDock;
  if (newWindow) {
    // Closed windows can be reused; an open window keeps its own function,
    // even when the new request decompiles that same function again.
    const QString prefix =
        c ? QStringLiteral("pseudocode-") : QStringLiteral("ir-");
    int number = 1;
    while (auto *dock = docks_.value(id)) {
      if (!dock->isOpen())
        break;
      id = prefix + QString::number(number++);
    }
  }
  return static_cast<CodeView *>(codeDock(id, representation)->widget());
}

MainWindow::Dock *MainWindow::codeDock(const QString &id,
                                       const QString &representation) {
  if (auto *dock = docks_.value(id))
    return dock;
  const bool c = CodeView::isSource(representation);
  auto *view = new CodeView(session_, representation);
  actions_.attachViewShortcuts(view);
  auto *dock =
      makeDock(id, nullptr,
               c ? QStringLiteral("pseudocode") : QStringLiteral("ir"), view);
  codeDocks_.append(dock);
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
  // A click or key that moves the code's cursor takes the disassembly to the
  // row's instruction, as Hex-Rays' synchronized views do, leaving the focus
  // and the jump history alone; Tab also moves the focus there.
  connect(view->text(), &CodeText::locationChanged, this,
          [this, view](Address address) {
            if (synchronizeCode_ && view->text()->hasFocus())
              disassembly_->navigate(address, false);
          });
  connect(view->text(), &CodeText::nameActivated, this,
          [this, view](const QString &name) { activateCodeName(view, name); });
  // A goto label's block shows in the disassembly, whose history keeps the
  // goto's place for Back, while the code goes to the label.
  connect(view->text(), &CodeText::labelActivated, this,
          [this, view](Address address) {
            ++codeNavigationSerial_;
            if (const auto function = view->text()->function())
              navigateCodeFunction(view, address, *function,
                                   view->text()->currentAddress());
          });
  // Data shows in the disassembly, as an import's name does.
  connect(view->text(), &CodeText::objectActivated, this,
          [this](Address address) { jump(address); });
  connect(view->text(), &CodeText::contextMenuRequested, this,
          &MainWindow::contextMenu);
  connect(view->text(), &CodeText::cursorChanged, this,
          &MainWindow::updateActions);
  connect(view->text(), &CodeText::statusChanged, this,
          &MainWindow::updateActions);
  if (id == QLatin1String(PseudocodeDock))
    pseudocode_ = view;
  return dock;
}

void MainWindow::retitleCodeDocks() {
  // Windows showing the same representation take letters in creation order.
  QHash<QString, int> counts;
  for (auto *dock : std::as_const(codeDocks_)) {
    auto *code = static_cast<CodeView *>(dock->widget());
    const QString title = CodeView::titleOf(code->representation());
    const int number = counts[title]++;
    const QString name = title + QLatin1Char('-') +
                         (number < 26 ? QString(QChar('A' + number))
                                      : QString::number(number + 1));
    dock->setIcon(icon(CodeView::isSource(code->representation())
                           ? QStringLiteral("pseudocode")
                           : QStringLiteral("ir")));
    // Pseudocode in the function's own language says which it chose.
    const QString language = code->chosenLanguage();
    dock->setTitle(language.isEmpty() ? name
                                      : name + QStringLiteral(" (") + language +
                                            QLatin1Char(')'));
  }
}

void MainWindow::initializeLayout() {
  if (std::exchange(layoutInitialized_, true))
    return;
  if (!QFile::exists(layoutPath()) ||
      !KDDockWidgets::LayoutSaver(kPaneRestoreOptions)
           .restoreFromFile(layoutPath()))
    applyDefaultLayout();
  // Query and generated views reopen through an explicit analysis command.
  closeTransientDocks();
}

void MainWindow::applyDefaultLayout() {
  auto *disassembly = docks_.value(DisassemblyDock);
  addDockWidget(disassembly, KDDockWidgets::Location_OnTop);
  addDockWidget(
      docks_.value(FunctionsDock), KDDockWidgets::Location_OnLeft, nullptr,
      InitialOption(
          QSize(::neverd::limits::gui_config::kMainWindowFunctionsWidth, 0)));
  disassembly->addDockWidgetAsTab(docks_.value(HexDock));
  disassembly->addDockWidgetAsTab(
      docks_.value(chooserDockId(ChooserKind::Imports)));
  disassembly->addDockWidgetAsTab(
      docks_.value(chooserDockId(ChooserKind::Exports)));
  disassembly->setAsCurrentTab();
  addDockWidget(docks_.value(OutputDock), KDDockWidgets::Location_OnBottom,
                nullptr,
                InitialOption(QSize(
                    0, ::neverd::limits::gui_config::kMainWindowOutputHeight)));
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
  if (functions->isOpen() && functions->QWidget::window() == this)
    addDockWidget(
        dock, KDDockWidgets::Location_OnBottom, functions,
        InitialOption(
            QSize(0, ::neverd::limits::gui_config::kMainWindowOverviewHeight)));
  else
    addDockWidget(
        dock, KDDockWidgets::Location_OnLeft, nullptr,
        InitialOption(
            QSize(::neverd::limits::gui_config::kMainWindowFunctionsWidth,
                  ::neverd::limits::gui_config::kMainWindowOverviewHeight)));
  openDock(dock, false);
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
    disassembly_ = primaryDisassembly_;
    overview_->setGraph(disassembly_->graph());
    closeTransientDocks();
    space_.clear();
    disassembly_->setGraphMode(false);
    for (auto *chooser : std::as_const(choosers_))
      chooser->model().reload();
    restoreProjectState();
    // A database also carries a desktop. Restore its arrangement, but keep
    // transient analysis views closed for each newly opened project.
    closeTransientDocks();
    updateTitle();
    updateActions();
    disassembly_->focusContent();
    openPendingLink();
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
    pseudocodeAfterJump_.reset();
    closeTransientDocks();
    ++searchSerial_;
    searchSubscription_ = 0;
    lastTextSearch_.clear();
    lastByteSearch_.clear();
    lastImmediateSearch_.clear();
    showBookmarks({});
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
                commandWindow(), tr("Unsaved changes"),
                session_.databaseOutOfDate()
                    ? tr("%1 lacks the latest changes. Save them into it "
                         "before continuing?")
                          .arg(QFileInfo(session_.databasePath()).fileName())
                    : tr("Comments were changed. Save them before "
                         "continuing?"),
                QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel,
                QMessageBox::Save);
            session_.resolveTransition(
                choice == QMessageBox::Save      ? QStringLiteral("save")
                : choice == QMessageBox::Discard ? QStringLiteral("discard")
                                                 : QStringLiteral("cancel"));
            Q_UNUSED(action);
          });
  // Bookmarks a pull or another tool changed show as the file has them.
  connect(&session_, &Session::projectFileChanged, this,
          [this](const QString &suffix) {
            if (suffix != QLatin1String(ProjectDatabase::BookmarksSuffix))
              return;
            const auto file = ProjectDatabase::readProjectFile(
                session_.filePath(), ProjectDatabase::BookmarksSuffix);
            const auto rows = ProjectDatabase::bookmarkRows(
                file ? file->array() : QJsonArray());
            if (rows != bookmarks_)
              showBookmarks(rows);
          });
  // Discard can approve synchronously from the close event's save prompt.
  // QWidget ignores reentrant close(), so finish closing on the next turn.
  connect(
      &session_, &Session::quitApproved, this,
      [this] {
        quitting_ = true;
        close();
      },
      Qt::QueuedConnection);
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
  for (auto *widget = commandFocusWidget(); widget;
       widget = widget->parentWidget())
    if (auto *view = qobject_cast<ChooserView *>(widget))
      return view;
  return nullptr;
}

CodeView *MainWindow::focusedCodeView() const {
  for (auto *widget = commandFocusWidget(); widget;
       widget = widget->parentWidget())
    if (auto *view = qobject_cast<CodeView *>(widget))
      return view;
  return nullptr;
}

std::optional<Address> MainWindow::currentFunction() const {
  return disassembly_ ? disassembly_->currentFunction() : std::nullopt;
}

std::optional<Address> MainWindow::pathfinderTarget() const {
  if (const CodeView *code = focusedCodeView())
    return code->text()->function();
  if (const ChooserView *chooser = focusedChooser())
    return chooser->currentAddress();
  return currentAddress();
}

PathfinderView *MainWindow::showPathfinder() {
  openDock(dockNamed(QString::fromLatin1(PathfinderDock)), false);
  return pathfinder_;
}

std::optional<Address> MainWindow::imageBase() const {
  return addressValue(
      session_.metadata().value(QStringLiteral("base_address")));
}

QString MainWindow::inputSha256() const {
  const QJsonObject &metadata = session_.metadata();
  if (const QString hash = metadata.value(QStringLiteral("sha256")).toString();
      !hash.isEmpty())
    return hash;
  // A worker before metadata named the hash names it with the project.
  return metadata.value(QStringLiteral("project"))
      .toObject()
      .value(QStringLiteral("input"))
      .toObject()
      .value(QStringLiteral("sha256"))
      .toString();
}

std::optional<DeepLink> MainWindow::linkHere() const {
  const auto base = imageBase();
  if (!session_.loaded() || !base)
    return std::nullopt;
  DeepLink link;
  link.input = session_.fileName();
  link.sha256 =
      inputSha256().left(::neverd::limits::gui_config::kCopiedLinkHashDigits);
  const auto at = [&](LinkResource resource, Address address,
                      std::optional<LinkView> view =
                          std::nullopt) -> std::optional<DeepLink> {
    link.resource = resource;
    link.rva = address - *base;
    link.view = view;
    return link;
  };
  if (const CodeView *code = focusedCodeView()) {
    const auto function = code->text()->function();
    if (!function)
      return std::nullopt;
    if (!CodeView::isSource(code->representation()))
      return at(LinkResource::Functions, *function);
    // A line of the pseudocode, where IDA's links name only the function.
    const auto line = code->text()->currentAddress();
    return line && *line != *function
               ? at(LinkResource::Addresses, *line, LinkView::Pseudocode)
               : at(LinkResource::Functions, *function, LinkView::Pseudocode);
  }
  if (ChooserView *chooser = focusedChooser()) {
    const auto address = chooser->currentAddress();
    if (!address)
      return std::nullopt;
    switch (chooser->model().kind()) {
    case ChooserKind::Functions:
      return at(LinkResource::Functions, *address);
    case ChooserKind::Names:
      return at(LinkResource::Names, *address);
    case ChooserKind::Imports:
      return at(LinkResource::Imports, *address);
    case ChooserKind::Exports:
      return at(LinkResource::Exports, *address);
    case ChooserKind::Segments:
      return at(LinkResource::Segments, *address);
    case ChooserKind::Strings:
      return at(LinkResource::Strings, *address);
    case ChooserKind::Bookmarks:
      return at(LinkResource::Bookmarks, *address);
    default:
      return at(LinkResource::Addresses, *address);
    }
  }
  for (QWidget *widget = commandFocusWidget(); widget;
       widget = widget->parentWidget()) {
    if (widget == localTypes_) {
      if (localTypes_->currentType().isEmpty())
        return std::nullopt;
      link.resource = LinkResource::Types;
      link.name = localTypes_->currentType();
      return link;
    }
    if (widget == hex_) {
      if (const auto address = hex_->currentAddress())
        return at(LinkResource::Addresses, *address, LinkView::Hexdump);
      return std::nullopt;
    }
  }
  const auto address = currentAddress();
  if (!address)
    return std::nullopt;
  if (disassembly_->graphMode())
    return at(LinkResource::Addresses, *address, LinkView::Graph);
  if (currentFunction() == address)
    return at(LinkResource::Functions, *address, LinkView::Disasm);
  return at(LinkResource::Addresses, *address);
}

void MainWindow::copyLink() {
  const auto link = linkHere();
  if (!link)
    return;
  const QString text = formatDeepLink(*link);
  QApplication::clipboard()->setText(text);
  // As IDA echoes the link it copies.
  output_->append(tr("Copied link: %1").arg(text), 0);
}

void MainWindow::reportLinkError(const QString &error) {
  if (!error.isEmpty())
    output_->append(error, 2);
}

void MainWindow::jumpToLink() {
  // A link on the clipboard, as one pasted from a chat, is offered.
  const QString clipboard = QApplication::clipboard()->text().trimmed();
  bool accepted = false;
  const QString text = QInputDialog::getText(
      commandWindow(), tr("Jump to link"), tr("Link (neverd:// or ida://):"),
      QLineEdit::Normal, looksLikeDeepLink(clipboard) ? clipboard : QString(),
      &accepted);
  if (!accepted || text.trimmed().isEmpty())
    return;
  const ParsedLink parsed = parseDeepLink(text);
  if (!parsed.link) {
    output_->append(tr("Not a link to follow: %1").arg(parsed.error), 2);
    return;
  }
  openLink(*parsed.link);
}

void MainWindow::openPendingLink() {
  if (!pendingLink_ || !session_.loaded() || space_.empty())
    return;
  auto [link, done] = *std::exchange(pendingLink_, std::nullopt);
  // The link's place, not the project's last one.
  initialAddress_.reset();
  restoreGraph_ = false;
  openLink(link, std::move(done));
}

void MainWindow::openLink(const DeepLink &link,
                          std::function<void(const QString &)> done) {
  // A link that comes while the program opens waits for its addresses.
  if (session_.loaded() && space_.empty()) {
    if (pendingLink_ && pendingLink_->second)
      pendingLink_->second(tr("Another link took this one's place."));
    pendingLink_ = {link, std::move(done)};
    return;
  }
  const auto finish = [this, done = std::move(done)](const QString &error) {
    reportLinkError(error);
    if (done)
      done(error);
  };
  if (!session_.loaded())
    return finish(tr("No program is open to follow the link in."));
  const QString hash = inputSha256();
  if (!link.namesHash(hash))
    return finish(tr("The link names another build of the input: its "
                     "SHA-256 starts %1, and that of %2 starts %3.")
                      .arg(link.sha256, session_.fileName(),
                           hash.left(link.sha256.size())));
  // As IDA does, a link that names another file is followed here anyway.
  if (!link.input.isEmpty() && link.sha256.isEmpty() &&
      !linkNamesInput(link.input, session_.fileName()))
    output_->append(tr("The link names %1, not %2; following it here.")
                        .arg(link.input, session_.fileName()),
                    1);
  const quint64 epoch = session_.epoch();
  const auto serial = ++codeNavigationSerial_;
  if (link.resource == LinkResource::Types) {
    session_.read(
        QStringLiteral("local_type"), {{QStringLiteral("name"), link.name}},
        this,
        [this, link, finish, epoch, serial](const QJsonObject &) {
          if (session_.epoch() != epoch)
            return finish(tr("The program changed before the link opened."));
          if (serial != codeNavigationSerial_)
            return finish(tr("Another link took this one's place."));
          openDock(dockNamed(QString::fromLatin1(LocalTypesDock)), true);
          localTypes_->showType(link.name);
          finish({});
        },
        [link, finish](const QString &, const QString &) {
          finish(tr("No local type is named %1.").arg(link.name));
        });
    return;
  }
  const auto base = imageBase();
  const auto address = base ? link.address(*base) : std::nullopt;
  if (!address || !space_.regionOf(*address))
    return finish(
        tr("The link's address %1 is not in %2.")
            .arg(address ? displayAddress(*address) : QStringLiteral("?"),
                 session_.fileName()));
  const LinkView view = link.shownView();
  if (link.resource == LinkResource::Bookmarks &&
      std::none_of(bookmarks_.begin(), bookmarks_.end(),
                   [&](const QJsonValue &row) {
                     return addressValue(row.toObject().value(
                                QStringLiteral("address"))) == address;
                   }))
    return finish(tr("No bookmark is at %1.").arg(displayAddress(*address)));
  const auto kind = [&]() -> std::optional<ChooserKind> {
    switch (link.resource) {
    case LinkResource::Strings:
      return ChooserKind::Strings;
    case LinkResource::Segments:
      return ChooserKind::Segments;
    case LinkResource::Names:
      return ChooserKind::Names;
    case LinkResource::Imports:
      return ChooserKind::Imports;
    case LinkResource::Exports:
      return ChooserKind::Exports;
    default:
      return std::nullopt;
    }
  }();
  if (kind) {
    chooserDock(*kind);
    choosers_.value(int(*kind))
        ->locateAddress(*address, [this, address = *address, view, finish,
                                   epoch, serial](std::optional<int> row,
                                                  const QString &error) {
          if (session_.epoch() != epoch)
            return finish(tr("The program changed before the link opened."));
          if (serial != codeNavigationSerial_)
            return finish(tr("Another link took this one's place."));
          if (!row)
            return finish(error);
          showLinkedAddress(address, view);
          finish({});
        });
    return;
  }
  if (link.resource != LinkResource::Functions) {
    showLinkedAddress(*address, view);
    return finish({});
  }
  if (const auto function =
          disassembly_->listing()->loadedFunctionAt(*address)) {
    showLinkedAddress(*function, view);
    return finish({});
  }
  // A function, named by an address in it, shows from its entry.
  session_.read(
      QStringLiteral("resolve"),
      {{QStringLiteral("query"), hexAddress(*address)}}, this,
      [this, address = *address, view, finish, epoch,
       serial](const QJsonObject &payload) {
        if (session_.epoch() != epoch)
          return finish(tr("The program changed before the link opened."));
        if (serial != codeNavigationSerial_)
          return finish(tr("Another link took this one's place."));
        const auto function =
            addressValue(payload.value(QStringLiteral("function_address")));
        if (!function)
          return finish(
              tr("No function is at %1.").arg(displayAddress(address)));
        showLinkedAddress(*function, view);
        finish({});
      },
      [finish](const QString &, const QString &message) { finish(message); });
}

void MainWindow::showLinkedAddress(Address address, LinkView view) {
  // A list shows its row beside the disassembly, which goes there too.
  const auto list = [this, address](ChooserKind kind) {
    navigate(address);
    if (kind == ChooserKind::Functions) {
      openDock(docks_.value(FunctionsDock), false,
               KDDockWidgets::Location_OnLeft);
      functions_->revealAddress(address);
      return;
    }
    auto *dock = chooserDock(kind);
    openDock(dock, true);
    choosers_.value(int(kind))->revealAddress(address);
  };
  switch (view) {
  case LinkView::Disasm:
    // Leaving the graph may restore its old cursor in the listing. Do that
    // before the requested jump, and retire any deferred graph switch.
    disassembly_->setGraphMode(false);
    navigate(address);
    return;
  case LinkView::Graph: {
    navigate(address);
    const auto epoch = session_.epoch();
    const auto serial = codeNavigationSerial_;
    QTimer::singleShot(0, this, [this, epoch, serial] {
      if (session_.loaded() && session_.epoch() == epoch &&
          serial == codeNavigationSerial_ && !disassembly_->graphMode())
        toggleGraph();
    });
    return;
  }
  case LinkView::Pseudocode:
    navigate(address);
    showPseudocode(CodeView::pseudocodeRepresentation(), CodeOpen::Switch);
    return;
  case LinkView::Hexdump:
    navigate(address);
    if (auto *dock = docks_.value(HexDock)) {
      openDock(dock, true);
    }
    return;
  case LinkView::Functions:
    return list(ChooserKind::Functions);
  case LinkView::Segments:
    return list(ChooserKind::Segments);
  case LinkView::Names:
    return list(ChooserKind::Names);
  case LinkView::Imports:
    return list(ChooserKind::Imports);
  case LinkView::Exports:
    return list(ChooserKind::Exports);
  case LinkView::Strings:
    return list(ChooserKind::Strings);
  case LinkView::Bookmarks:
    return list(ChooserKind::Bookmarks);
  case LinkView::Types:
    return;
  }
}

QJsonObject MainWindow::instanceInfo() const {
  QJsonObject info{
      {QStringLiteral("version"), QCoreApplication::applicationVersion()},
      {QStringLiteral("loaded"), session_.loaded()}};
  if (!session_.loaded())
    return info;
  info.insert(QStringLiteral("input"), session_.filePath());
  info.insert(QStringLiteral("input_name"), session_.fileName());
  info.insert(QStringLiteral("project"), session_.projectPath());
  info.insert(QStringLiteral("sha256"), inputSha256());
  info.insert(QStringLiteral("analysis_complete"), session_.analysisComplete());
  if (const auto address = currentAddress()) {
    QJsonObject location{{QStringLiteral("address"), hexAddress(*address)}};
    if (const auto base = imageBase())
      location.insert(QStringLiteral("rva"), hexAddress(*address - *base));
    if (const auto function = currentFunction())
      location.insert(QStringLiteral("function"), hexAddress(*function));
    info.insert(QStringLiteral("location"), location);
  }
  return info;
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
  hexFollower_.clear();
  if (!session_.loaded())
    return;
  auto *view = disassembly_;
  for (auto *dock : std::as_const(docks_))
    if (dock->widget() == view) {
      openDock(dock, true, KDDockWidgets::Location_OnTop);
      break;
    }
  disassembly_ = view;
  view->navigate(address, true);
  view->focusContent();
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

void MainWindow::activateCodeName(CodeView *view, const QString &name,
                                  bool selectSourceAddress) {
  const auto serial = ++codeNavigationSerial_;
  const auto epoch = session_.epoch();
  const auto source = view->text()->function();
  const auto position = view->text()->currentAddress();
  const auto from = position ? position : source;
  const auto representation = view->representation();
  const auto sourceOptions = view->text()->sourceOptions();
  session_.read(
      QStringLiteral("resolve"), {{"query", name}}, view,
      [this, view, serial, epoch, source, from, representation, sourceOptions,
       name, selectSourceAddress](const QJsonObject &payload) {
        if (serial != codeNavigationSerial_ || epoch != session_.epoch() ||
            view->text()->function() != source ||
            view->text()->sourceOptions() != sourceOptions ||
            view->representation() != representation || !view->isVisible())
          return;
        const auto address = addressValue(payload.value("address"));
        const auto function = addressValue(payload.value("function_address"));
        if (function && !payload.value("import").toBool()) {
          navigateCodeFunction(view, address.value_or(*function), *function,
                               from);
          if (selectSourceAddress)
            view->text()->selectAddress(address.value_or(*function));
        } else if (address) {
          // Data and import slots retain ordinary address navigation.
          jump(*address);
        } else {
          navigateExpression(name);
        }
      },
      [this, view, name, serial, epoch, source, representation,
       sourceOptions](const QString &, const QString &error) {
        if (serial != codeNavigationSerial_ || epoch != session_.epoch() ||
            view->text()->function() != source ||
            view->text()->sourceOptions() != sourceOptions ||
            view->representation() != representation || !view->isVisible())
          return;
        // What the code declares but the binary does not name, such as an
        // LLVM intrinsic, shows at its declaration.
        if (!view->text()->showUnresolvedDeclaration(name))
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
  hexFollower_.clear();
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
          // Within the function the code goes back to the row it left, such
          // as the goto whose label it went to.
          else if (const auto address = disassembly_->currentAddress())
            view->text()->revealAddress(*address);
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
  // A refresh may resolve an operand byte back to its instruction start.
  // Keep the active Hex cursor on the edited byte.
  if (!hexActive_ && source != hex_ && source != hexFollower_)
    hex_->setCurrent(address, 1);
  const auto function = currentFunction();
  session_.publishSelection(address, function, QStringLiteral("disassembly"));
  // The user's own move in the disassembly takes the code windows showing
  // its function to the instruction's rows. One showing another function
  // stays: following it would decompile on every step.
  if (synchronizeCode_ && function &&
      disassembly_->isAncestorOf(QApplication::focusWidget()))
    for (auto *view : visibleCodeViews())
      if (view->text()->function() == function)
        view->text()->revealAddress(address);
  updateActions();
}

QVector<CodeView *> MainWindow::visibleCodeViews() const {
  QVector<CodeView *> views;
  for (auto *dock : codeDocks_)
    if (auto *view = static_cast<CodeView *>(dock->widget()); view->isVisible())
      views.append(view);
  return views;
}

//===----------------------------------------------------------------------===//
// Actions
//===----------------------------------------------------------------------===//

void MainWindow::connectActions() {
  connectPatchActions();
  const auto on = [this](ActionId id, auto slot) {
    connect(actions_.action(id), &QAction::triggered, this, slot);
  };
  on(ActionId::FileOpen, [this] { openDialog(); });
  on(ActionId::FileLineage, [this] {
    if (!lineage_)
      lineage_ = new LineageDialog(session_.workerPath(), this);
    lineage_->show();
    lineage_->raise();
    lineage_->activateWindow();
  });
  on(ActionId::FileCompare, [this] {
    if (!comparison_)
      comparison_ = new ComparisonDialog(session_.workerPath(),
                                         session_.filePath(), this);
    comparison_->show();
    comparison_->raise();
    comparison_->activateWindow();
  });
  on(ActionId::FilePackageReview, [this] {
    if (!packageReview_)
      packageReview_ =
          new web::PackageReviewDialog(session_.workerPath(), this);
    packageReview_->show();
    packageReview_->raise();
    packageReview_->activateWindow();
  });
  on(ActionId::FileReload, [this] { reloadInput(); });
  on(ActionId::FileLoadPdb, [this] { loadPdb(); });
  on(ActionId::FileLoadDwarf, [this] { loadDwarf(); });
  on(ActionId::FileLoadMap, [this] { loadMap(); });
  on(ActionId::FileLoadHeader, [this] { loadHeader(); });
  on(ActionId::FileLoadSignatures, [this] {
    const auto path = QFileDialog::getOpenFileName(
        commandWindow(), tr("Load signature pack"), {},
        tr("Signature packs (*.pat *.npat *.json);;All files (*)"));
    if (!path.isEmpty())
      session_.loadSignatures(path, false);
  });
  on(ActionId::FileLoadSignatureTree, [this] {
    const auto path = QFileDialog::getExistingDirectory(
        commandWindow(), tr("Signature directory"));
    if (!path.isEmpty())
      session_.loadSignatures(path, true);
  });
  on(ActionId::FileExportListing, [this] { exportCurrent(false); });
  on(ActionId::FileExportPseudocode, [this] { exportCurrent(true); });
  on(ActionId::FileSave, [this] { session_.save(); });
  on(ActionId::FilePackDatabase, [this] { packDatabase(); });
  on(ActionId::FileClose, [this] { session_.closeFile(); });
  on(ActionId::FileQuickStart, [this] { showQuickStart(); });
  on(ActionId::FileExit, [this] { close(); });

  on(ActionId::EditUndo, [this] { session_.undo(); });
  on(ActionId::EditRedo, [this] { session_.redo(); });
  on(ActionId::EditCopy, [this] { copySelection(); });
  on(ActionId::EditExportData, [this] { exportData(); });
  on(ActionId::EditCopyAddress, [this] {
    if (const auto address = currentAddress())
      QApplication::clipboard()->setText(hexAddress(*address));
  });
  on(ActionId::EditRename, [this] { rename(); });
  on(ActionId::EditAddAnalysisSection, [this] { addAnalysisSection(); });
  on(ActionId::EditRepeatableComment, [this] { comment(); });
  on(ActionId::EditBookmark, [this] {
    const auto address = currentAddress();
    if (!address)
      return;
    bool ok = false;
    const auto text = QInputDialog::getText(
        commandWindow(), tr("Mark position"), tr("Description:"),
        QLineEdit::Normal, displayAddress(*address), &ok);
    if (!ok)
      return;
    // Marking a marked address again renames its bookmark.
    auto rows = bookmarks();
    bool renamed = false;
    for (auto &&value : rows)
      if (auto row = value.toObject();
          row.value("address").toString() == hexAddress(*address)) {
        row.insert(QStringLiteral("text"), text);
        value = row;
        renamed = true;
      }
    if (!renamed)
      rows.append(
          QJsonObject{{"address", hexAddress(*address)}, {"text", text}});
    setBookmarks(rows);
  });

  on(ActionId::JumpOperand, [this] {
    if (const auto target = disassembly_->operandTarget())
      navigate(*target);
  });
  on(ActionId::JumpNewWindow, [this] {
    if (const auto target = disassembly_->operandTarget())
      jumpNewWindow(*target);
  });
  on(ActionId::EditCreateFunction, [this] {
    if (const auto address = currentAddress())
      session_.createFunction(*address);
  });
  on(ActionId::EditFunction, [this] {
    if (const auto function = currentFunction())
      editFunction(*function);
  });
  on(ActionId::EditDeleteFunction, [this] {
    if (const auto function = currentFunction())
      session_.deleteFunction(*function);
  });
  // An operand's number shows in the base the user picks, with its sign or
  // bits changed: the number under the cursor, else the line's last number.
  // A pseudocode number takes the bases, H toggling between hexadecimal and
  // decimal as Hex-Rays' H does.
  const auto formatOperand = [this](const QString &action) {
    if (auto *code = focusedCodeView()) {
      if (action == QLatin1String("decimal"))
        formatCodeNumber(code, std::nullopt);
      else if (const auto radix = literalRadixNamed(action.toStdString()))
        formatCodeNumber(code, *radix);
      return;
    }
    if (const auto address = currentAddress())
      session_.formatOperand(*address, disassembly_->currentOperand(), action);
  };
  for (const auto &[id, action] : {
#define NEVERD_OPERAND_BASE(Id, Spelling)                                      \
  std::pair{ActionId::EditOperand##Id, QStringLiteral(Spelling)},
#include "neverd/OperandFormats.def"
           std::pair{ActionId::EditOperandNegate, QStringLiteral("negate")},
           std::pair{ActionId::EditOperandInvert, QStringLiteral("invert")}})
    // A structure offset names a type the user picks first.
    on(id, [this, formatOperand, action] {
      if (action == QLatin1String("enum"))
        editDataPresentation("enum");
      else if (action == QLatin1String("struct"))
        chooseStructOffset();
      else
        formatOperand(action);
    });
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
  on(ActionId::EditSetType, [this] { setType(); });
  on(ActionId::EditSplitVariable, [this] { editVariable("split"); });
  on(ActionId::EditMapVariable, [this] { editVariable("map"); });
  on(ActionId::EditUnmapVariable, [this] { editVariable("unmap"); });
  on(ActionId::EditUnsplitVariable, [this] { editVariable("unsplit"); });
  on(ActionId::EditResetVariables, [this] { editVariable("clear"); });
  on(ActionId::EditMakeArray, [this] { editDataPresentation("array"); });
  on(ActionId::EditMakeAlignment,
     [this] { editDataPresentation("alignment"); });
  on(ActionId::EditStackFrame, [this] { editStackFrame(0); });
  on(ActionId::EditStackPointer, [this] { editStackFrame(1); });
  on(ActionId::EditStackOperand, [this] { editStackFrame(2); });
  on(ActionId::EditCallType, [this] { editCallType(); });
  on(ActionId::EditResetCallTypes, [this] { editCallType(true); });
  on(ActionId::EditConvertToStruct, [this] { convertToStruct(); });
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
                     CodeOpen::Switch);
    }
  });
  on(ActionId::JumpAnywhere, [this] { jumpAnywhere(); });
  on(ActionId::JumpByName,
     [this] { chooseLocation(ChooserKind::Names, ActionId::JumpByName); });
  on(ActionId::JumpFunction, [this] {
    chooseLocation(ChooserKind::Functions, ActionId::JumpFunction);
  });
  on(ActionId::JumpSegment,
     [this] { chooseLocation(ChooserKind::Segments, ActionId::JumpSegment); });
  on(ActionId::JumpEntry,
     [this] { chooseLocation(ChooserKind::Exports, ActionId::JumpEntry); });
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
    openDock(dock, true);
  });

  on(ActionId::SearchText,
     [this] { searchBinary(QStringLiteral("text"), false); });
  on(ActionId::SearchNextText,
     [this] { searchBinary(QStringLiteral("text"), true); });
  on(ActionId::SearchBytes,
     [this] { searchBinary(QStringLiteral("bytes"), false); });
  on(ActionId::SearchNextBytes,
     [this] { searchBinary(QStringLiteral("bytes"), true); });
  on(ActionId::SearchImmediate, [this] { searchBinary("immediate", false); });
  on(ActionId::SearchNextImmediate,
     [this] { searchBinary("immediate", true); });
  on(ActionId::FindRegisterDefinition,
     [this] { queryRegister("definitions"); });
  on(ActionId::FindRegisterUse, [this] { queryRegister("uses"); });
  on(ActionId::FindRegisterValue, [this] { queryRegister("value"); });
  on(ActionId::SearchHighlightUp, [this] { searchHighlight(false); });
  on(ActionId::SearchHighlightDown, [this] { searchHighlight(true); });
  on(ActionId::SearchInView, [this] {
    auto *code = focusedCodeView();
    if (!FindDialog)
      FindDialog = new FindInViewDialog(commandView());
    showModelessDialog(FindDialog, commandView(), &session_);
    FindDialog->search(code ? code->text() : nullptr, disassembly_->listing());
  });

  on(ActionId::ViewQuickView, [this] { showCommandPalette(); });
  on(ActionId::ViewDisassembly, [this] {
    disassembly_ = primaryDisassembly_;
    overview_->setGraph(disassembly_->graph());
    openDock(docks_.value(DisassemblyDock), false,
             KDDockWidgets::Location_OnTop);
  });
  on(ActionId::ViewToggleGraph, [this] { toggleGraph(); });
  actions_.action(ActionId::ViewFocusMode)->setCheckable(true);
  actions_.action(ActionId::ViewFocusMode)->setAutoRepeat(false);
  on(ActionId::ViewFocusMode, [this] { toggleFocusMode(); });
  on(ActionId::ViewPseudocode, [this] {
    showPseudocode(CodeView::pseudocodeRepresentation(), CodeOpen::Generate);
  });
  for (const auto &[action, representation] :
       {std::pair{ActionId::ViewLLVMC, "llvmc"},
        {ActionId::ViewLowIR, "low"},
        {ActionId::ViewMedIR, "med"},
        {ActionId::ViewHighIR, "high"},
        {ActionId::ViewLLVMIR, "llvm"}})
    on(action, [this, representation] {
      showPseudocode(QString::fromLatin1(representation), CodeOpen::Generate);
    });
  on(ActionId::ViewHex, [this] {
    openDock(docks_.value(HexDock), true);
    hexActive_ = true;
    hex_->setFocus();
  });
  const auto openChooser = [this](ChooserKind kind) {
    openDock(chooserDock(kind), true);
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
    if (focusSidebar_) {
      focusSidebar_->reveal();
      focusFunctions_->table()->setFocus(Qt::ShortcutFocusReason);
      return;
    }
    openDock(docks_.value(FunctionsDock), false,
             KDDockWidgets::Location_OnLeft);
  });
  on(ActionId::ViewCrossReferences,
     [this] { showCrossReferences(currentAddress(), true); });
  on(ActionId::EditCopyLink, [this] { copyLink(); });
  on(ActionId::JumpLink, [this] { jumpToLink(); });
  on(ActionId::ViewPathfinder,
     [this] { showPathfinder()->waypointList()->setFocus(); });
  // The view the command came from keeps the focus, so that more of its
  // functions can join the path.
  on(ActionId::PathfinderNewPath, [this] {
    if (const auto target = pathfinderTarget())
      showPathfinder()->startPath(*target);
  });
  on(ActionId::PathfinderAddWaypoint, [this] {
    if (const auto target = pathfinderTarget())
      showPathfinder()->addWaypoint(*target);
  });
  on(ActionId::ViewBookmarks,
     [this] { actions_.action(ActionId::JumpMark)->trigger(); });
  on(ActionId::ViewOutput, [this] {
    openDock(docks_.value(OutputDock), false);
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
  // A tab beside the disassembly, as IDA opens its subviews.
  on(ActionId::ViewLocalTypes, [this] {
    openDock(dockNamed(QString::fromLatin1(LocalTypesDock)), true);
    localTypes_->focusFilter();
  });
  on(ActionId::ViewTypeLibraries, [this] {
    openDock(dockNamed(QString::fromLatin1(TypeLibrariesDock)), true);
    typeLibraries_->table()->setFocus();
  });
  on(ActionId::ViewUndoHistory, [this] {
    const auto &history = session_.history();
    output_->append(tr("History: %1 entries, cursor %2")
                        .arg(history.value("total").toInt())
                        .arg(history.value("cursor").toInt()),
                    0);
  });
  on(ActionId::ViewCalculator, [this] { showCalculator(); });
  on(ActionId::ViewFullScreen, [this] {
    if (frame_)
      frame_->toggleFullScreen();
  });
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
        if (dock->widget() == disassembly_) {
          disassembly_ = primaryDisassembly_;
          overview_->setGraph(disassembly_->graph());
        }
        dock->forceClose();
        return;
      }
  });
  on(ActionId::WindowsFocusCommand, [this] {
    openDock(docks_.value(OutputDock), false);
    output_->focusCommandLine();
  });
  on(ActionId::HelpContents, [] {
    QDesktopServices::openUrl(QUrl(QString::fromLatin1(DocumentationUrl)));
  });
  on(ActionId::HelpAbout, [this] { showAbout(); });
  on(ActionId::ViewUnwindInfo, [this] {
    const auto PC = currentAddress();
    if (!PC || !session_.loaded() || !session_.reportsUnwindFrames() ||
        focusedCodeView())
      return;
    auto *Dialog = new UnwindInfoDialog(session_.queries(), *PC, commandView());
    const auto Update = [this, Dialog] {
      Dialog->setProviderAvailable(session_.connected() && session_.loaded(),
                                   session_.reportsUnwindFrames());
    };
    connect(&session_, &Session::stateChanged, Dialog, Update);
    Update();
    showModelessDialog(Dialog, Dialog->parentWidget(), &session_);
  });
  on(ActionId::HelpX87Capabilities, [this] {
    auto *Dialog = new X87CapabilitiesDialog(session_.queries(), commandView());
    const auto Update = [this, Dialog] {
      Dialog->setProviderAvailable(session_.connected(),
                                   session_.reportsX87Capabilities());
    };
    connect(&session_, &Session::stateChanged, Dialog, Update);
    Update();
    showModelessDialog(Dialog, Dialog->parentWidget());
  });
}

void MainWindow::addAnalysisSection() {
  const auto epoch = session_.epoch();
  auto *dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowModality(Qt::WindowModal);
  dialog->setWindowTitle(tr("Add analysis section"));
  dialog->setObjectName(QStringLiteral("addAnalysisSection"));
  auto *layout = new QVBoxLayout(dialog);
  auto *explanation = new QLabel(
      tr("Map a binary file at a virtual address in the current image. "
         "Save the project to keep it. The input binary stays unchanged."),
      dialog);
  explanation->setWordWrap(true);
  layout->addWidget(explanation);
  auto *form = new QFormLayout;
  auto *path = new QLineEdit(dialog);
  path->setObjectName(QStringLiteral("sectionFile"));
  auto *fileRow = new QHBoxLayout;
  fileRow->addWidget(path);
  auto *browse = new QPushButton(tr("Browse…"), dialog);
  fileRow->addWidget(browse);
  form->addRow(tr("Binary bytes:"), fileRow);
  auto *name = new QLineEdit(QStringLiteral(".recovered"), dialog);
  name->setObjectName(QStringLiteral("sectionName"));
  form->addRow(tr("Section name:"), name);
  auto *address = new QLineEdit(dialog);
  address->setObjectName(QStringLiteral("sectionAddress"));
  address->setPlaceholderText(QStringLiteral("0x…"));
  form->addRow(tr("Virtual address:"), address);
  auto *permissions = new QComboBox(dialog);
  permissions->addItems({QStringLiteral("r"), QStringLiteral("rw"),
                         QStringLiteral("rx"), QStringLiteral("rwx")});
  permissions->setCurrentText(QStringLiteral("rx"));
  form->addRow(tr("Permissions:"), permissions);
  auto *define =
      new QCheckBox(tr("Define a function covering these bytes"), dialog);
  define->setChecked(true);
  form->addRow(define);
  auto *function = new QLineEdit(QStringLiteral("recovered_function"), dialog);
  function->setObjectName(QStringLiteral("sectionFunction"));
  form->addRow(tr("Function name:"), function);
  layout->addLayout(form);
  auto *error = new QLabel(dialog);
  error->setObjectName(QStringLiteral("sectionError"));
  error->setWordWrap(true);
  layout->addWidget(error);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  layout->addWidget(buttons);
  connect(browse, &QPushButton::clicked, dialog, [dialog, path] {
    const auto file =
        QFileDialog::getOpenFileName(dialog, tr("Select binary bytes"));
    if (!file.isEmpty())
      path->setText(file);
  });
  connect(define, &QCheckBox::toggled, function, &QLineEdit::setEnabled);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(
      buttons, &QDialogButtonBox::accepted, dialog,
      [this, dialog, epoch, path, name, address, permissions, define, function,
       error, buttons] {
        if (session_.epoch() != epoch || !session_.loaded()) {
          error->setText(tr("The project changed. Open this dialog again."));
          return;
        }
        const QFileInfo file(path->text());
        const QString addressText = address->text().trimmed();
        bool validAddress = false;
        const auto va = addressText.mid(2).toULongLong(&validAddress, 16);
        if (!file.isFile() || file.size() <= 0 ||
            file.size() > ::neverd::sdk::kMaxAnalysisSectionBytes ||
            !addressText.startsWith(QLatin1String("0x")) || !validAddress ||
            va == 0 || name->text().trimmed().isEmpty() ||
            (define->isChecked() &&
             (function->text().trimmed().isEmpty() ||
              !permissions->currentText().contains(QLatin1Char('x'))))) {
          error->setText(
              tr("Choose a file of 1 byte to %1, a hexadecimal address and a "
                 "section name. A function needs a name and executable "
                 "permissions.")
                  .arg(locale().formattedDataSize(
                      qint64(::neverd::sdk::kMaxAnalysisSectionBytes), 0)));
          return;
        }
        QJsonObject payload{
            {"schema", "neverd.analysis-section.v1"},
            {"name", name->text().trimmed()},
            {"address", QStringLiteral("0x") + QString::number(va, 16)},
            {"permissions", permissions->currentText()},
            {"path", file.absoluteFilePath()}};
        if (define->isChecked())
          payload.insert("functions", QJsonArray{QJsonObject{
                                          {"name", function->text().trimmed()},
                                          {"offset", "0x0"},
                                          {"size", file.size()}}});
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        QPointer<QDialog> guard(dialog);
        session_.addAnalysisSection(
            payload,
            [this, guard, va](const QJsonObject &) {
              if (guard)
                guard->accept();
              navigate(va);
            },
            [guard, error, buttons](const QString &, const QString &text) {
              if (guard) {
                error->setText(text);
                buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
              }
            });
      });
  dialog->resize(560, dialog->sizeHint().height());
  dialog->show();
}

void MainWindow::updateActions() {
  const bool loaded = session_.loaded();
  const bool location = loaded && currentAddress().has_value();
  for (auto *action : actions_.allActions()) {
    if (action->property("externalCommand").toBool())
      continue;
    const QString name = action->objectName();
    const bool always = name.startsWith(QStringLiteral("Options")) ||
                        name.startsWith(QStringLiteral("Windows")) ||
                        name.startsWith(QStringLiteral("Help")) ||
                        name == QLatin1String("FileOpen") ||
                        name == QLatin1String("FilePackageReview") ||
                        name == QLatin1String("FileCompare") ||
                        name == QLatin1String("FileExit") ||
                        name == QLatin1String("FileQuickStart") ||
                        name.startsWith(QStringLiteral("ViewZoom")) ||
                        name == QLatin1String("ViewFullScreen") ||
                        name == QLatin1String("ViewOutput") ||
                        name == QLatin1String("ViewCalculator") ||
                        name == QLatin1String("ViewConnections");
    action->setEnabled(always || loaded);
  }
  actions_.action(ActionId::ViewFocusMode)
      ->setEnabled(!focusLayout_.isEmpty() || loaded);
  actions_.action(ActionId::ViewUnwindInfo)
      ->setEnabled(location && !focusedCodeView() &&
                   session_.reportsUnwindFrames());
  actions_.action(ActionId::EditExportData)
      ->setEnabled(loaded && !focusedCodeView() &&
                   (hexActive_ ? hex_->byteSelection().has_value()
                               : !disassembly_->graphMode() && location));
  actions_.action(ActionId::JumpBack)
      ->setEnabled(loaded && disassembly_->canGoBack());
  actions_.action(ActionId::JumpForward)
      ->setEnabled(loaded && disassembly_->canGoForward());
  actions_.action(ActionId::EditUndo)->setEnabled(loaded && session_.canUndo());
  actions_.action(ActionId::EditRedo)->setEnabled(loaded && session_.canRedo());
  // Edits other than comments, and the debug files chosen, are kept as they
  // are made; a database the user opened is written back by Save.
  actions_.action(ActionId::FileSave)->setEnabled(loaded && session_.dirty());
  actions_.action(ActionId::FilePackDatabase)->setEnabled(loaded);
  // A PE has a PDB, an ELF or Mach-O image DWARF, and any image a MAP but
  // the EVM and SBF programs, which describe themselves.
  const QString format = session_.format();
  const bool selfDescribed =
      format == QLatin1String("EVM") ||
      session_.architecture().compare(QLatin1String("sbf"),
                                      Qt::CaseInsensitive) == 0;
  const bool loadsDebugFile = loaded && !session_.opening();
  actions_.action(ActionId::FileLoadPdb)
      ->setEnabled(loadsDebugFile && format == QLatin1String("PE") &&
                   session_.loadsPdbFiles());
  actions_.action(ActionId::FileLoadDwarf)
      ->setEnabled(loadsDebugFile && !selfDescribed &&
                   (format == QLatin1String("ELF") ||
                    format == QLatin1String("Mach-O")) &&
                   session_.loadsDwarfFiles());
  actions_.action(ActionId::FileLoadMap)
      ->setEnabled(loadsDebugFile && !selfDescribed &&
                   session_.loadsMapFiles());
  actions_.action(ActionId::FileLoadHeader)
      ->setEnabled(loaded && !session_.readOnly() &&
                   session_.declaresLocalTypes());
  const bool patching =
      loaded && !session_.readOnly() && session_.keepsBytePatches();
#define NEVERD_PATCH_ITEM(Id, Key, Kind)                                       \
  actions_.action(ActionId::Id)->setEnabled(patching);
#include "PatchMenu.def"
  actions_.action(ActionId::EditPatchMenu)->setEnabled(patching && location);
  actions_.action(ActionId::EditPatchedBytes)
      ->setEnabled(loaded && session_.keepsBytePatches());
  actions_.action(ActionId::EditPatchRewriteFunction)
      ->setEnabled(patching && session_.keepsSourcePatches() &&
                   !session_.hasBytePatches() && currentFunction().has_value());
  actions_.action(ActionId::FileProducePatchedBinary)
      ->setEnabled(loaded && session_.hasBytePatches());
  actions_.action(ActionId::EditRename)
      ->setEnabled(location && !session_.readOnly());
  actions_.action(ActionId::EditAddAnalysisSection)
      ->setEnabled(
          loaded && !session_.readOnly() &&
          session_.metadata().value("analysis_sections_supported").toBool());
  // A function starts where none starts yet; the one the cursor is in can
  // stop being one.
  const bool functionEdits =
      location && !session_.readOnly() && session_.keepsFunctionEdits();
  const auto function = functionEdits ? currentFunction() : std::nullopt;
  actions_.action(ActionId::EditCreateFunction)
      ->setEnabled(functionEdits && function != currentAddress());
  actions_.action(ActionId::EditDeleteFunction)
      ->setEnabled(functionEdits && function.has_value());
  const bool functionAttributes =
      functionEdits && session_.statesFunctionAttributes();
  actions_.action(ActionId::EditFunction)
      ->setEnabled(functionAttributes && function.has_value());
  functions_->enableCommand(ChooserCommand::Edit, functionAttributes);
  if (focusFunctions_)
    focusFunctions_->enableCommand(ChooserCommand::Edit, functionAttributes);
  const bool dataItems =
      location && !session_.readOnly() && session_.keepsDataItems();
  for (const auto id : {ActionId::EditDefineCode, ActionId::EditDefineData,
                        ActionId::EditDefineString, ActionId::EditUndefine})
    actions_.action(id)->setEnabled(dataItems);
  actions_.action(ActionId::EditDefineCode)
      ->setEnabled(dataItems && !focusedCodeView());
  const bool operandFormats =
      location && !session_.readOnly() && session_.keepsOperandFormats();
  CodeView *code = focusedCodeView();
  // A pseudocode window's numbers take the bases; offsets, signs and
  // complements are the disassembly's.
  const bool codeNumbers = code && loaded && !session_.readOnly() &&
                           CodeView::isSource(code->representation()) &&
                           code->text()->function().has_value();
  for (const auto id : {ActionId::EditOperandNumber, ActionId::EditOperandHex,
                        ActionId::EditOperandDecimal,
                        ActionId::EditOperandBinary, ActionId::EditOperandChar})
    actions_.action(id)->setEnabled(code ? codeNumbers : operandFormats);
  for (const auto id :
       {ActionId::EditOperandOffset, ActionId::EditOperandNegate,
        ActionId::EditOperandInvert})
    actions_.action(id)->setEnabled(operandFormats && !code);
  // A structure offset is the disassembly's; a pseudocode window's
  // parameters take a type instead.
  actions_.action(ActionId::EditOperandStruct)
      ->setEnabled(operandFormats && session_.showsStructOffsets() && !code);
  for (const auto id : {ActionId::EditMakeArray, ActionId::EditMakeAlignment})
    actions_.action(id)->setEnabled(location && !code && !session_.readOnly() &&
                                    session_.keepsDataItems());
  actions_.action(ActionId::EditOperandEnum)
      ->setEnabled(operandFormats && !code && session_.showsStructOffsets());
  const bool declarations =
      location && !session_.readOnly() && session_.typesFunctions();
  actions_.action(ActionId::EditSetType)
      ->setEnabled(declarations && (code ? code->text()->function().has_value()
                                         : currentFunction().has_value()));
  for (const auto id : {ActionId::EditStackFrame, ActionId::EditStackPointer,
                        ActionId::EditStackOperand})
    actions_.action(id)->setEnabled(location && !session_.readOnly() &&
                                    currentFunction().has_value());
  const bool variableView =
      code && loaded && !session_.readOnly() && code->text()->function() &&
      (code->representation() == "source" || code->representation() == "c" ||
       code->representation() == "llvmc");
  const auto variableTarget =
      variableView ? code->text()->renameTarget() : std::nullopt;
  const bool variable = variableTarget &&
                        variableTarget->contains("variable_id") &&
                        !variableTarget->value("object").toBool();
  for (const auto id :
       {ActionId::EditSplitVariable, ActionId::EditMapVariable,
        ActionId::EditUnmapVariable, ActionId::EditUnsplitVariable})
    actions_.action(id)->setEnabled(variableView && variable);
  actions_.action(ActionId::EditResetVariables)->setEnabled(variableView);
  actions_.action(ActionId::EditCallType)
      ->setEnabled(variableView && !code->text()->loading());
  actions_.action(ActionId::EditResetCallTypes)
      ->setEnabled(variableView && !code->text()->loading());
  actions_.action(ActionId::EditConvertToStruct)
      ->setEnabled(declarations && session_.convertsParameters() && code &&
                   code->text()->function().has_value());
  actions_.action(ActionId::EditRepeatableComment)
      ->setEnabled(location && !session_.readOnly());
  actions_.action(ActionId::OptionsAnalyze)
      ->setEnabled(loaded && !session_.metadata().value("analyzed").toBool());
  actions_.action(ActionId::EditCopyLink)
      ->setEnabled(loaded && linkHere().has_value());
  const bool callPaths = loaded && session_.findsCallPaths();
  actions_.action(ActionId::ViewPathfinder)->setEnabled(callPaths);
  for (const auto id :
       {ActionId::PathfinderNewPath, ActionId::PathfinderAddWaypoint})
    actions_.action(id)->setEnabled(callPaths &&
                                    pathfinderTarget().has_value());
}

void MainWindow::updateTitle() {
  QString shown = QStringLiteral("NeverD");
  if (!session_.loaded()) {
    setWindowTitle(shown);
  } else {
    // Like a classic title bar: the database, then the input it describes.
    const QString database = session_.databasePath();
    shown = QFileInfo::exists(database) ? QFileInfo(database).fileName()
                                        : session_.fileName();
    setWindowTitle(
        QStringLiteral("NeverD - %1 %2").arg(shown, session_.projectPath()));
  }
  // The command center names the file, as Visual Studio Code's names the
  // workspace, and its tooltip carries the full title.
  if (titleBar_)
    titleBar_->setCommandCenterText(shown, windowTitle());
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
      tr("Disk: %1 GB").arg(storage.bytesAvailable() / (1024LL * 1024 * 1024)));
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
  if (ProjectDatabase::hasState(path))
    openNow(path, {});
  else
    openAskingForDebugInfo(path, {});
}

void MainWindow::setAsksForDebugInfo(bool asks) {
  asksForDebugInfo_ = asks;
  microsoftSymbols_->setOffers(asks);
}

void MainWindow::openNow(const QString &path, const LoadOptions &options) {
  // A cached project restores its debug choices. Missing symbols in an old
  // project are loaded explicitly through File -> Load file, not offered again.
  microsoftSymbols_->setOffers(asksForDebugInfo_ &&
                               !ProjectDatabase::hasState(path));
  // The workbench shows as the file starts to load, after any question
  // asked before the load.
  showWorkbench();
  session_.open(path, options);
}

void MainWindow::openAskingForDebugInfo(const QString &path,
                                        LoadOptions options) {
  // Whether the worker finds PDBs is known once it has started; a file named
  // on the command line can come first.
  if (!session_.connected()) {
    debugPromptPending_ = {path, options};
    return;
  }
  // IDA asks as it makes a database, so a project keeps the answers.
  if (!asksForDebugInfo_ || ProjectDatabase::hasState(path)) {
    openNow(path, options);
    return;
  }
  openAskingForSymbols(path, options);
}

void MainWindow::openAskingForSymbols(const QString &path,
                                      LoadOptions options) {
  if (!options.symbols.isEmpty() || !session_.findsSymbolTables()) {
    openAskingForDebugFiles(path, options);
    return;
  }
  const QPointer<QWidget> owner = isVisible() ? commandWindow() : nullptr;
  session_.read(
      QStringLiteral("symbol_table_reference"), {{"path", path}}, this,
      [this, path, options, owner](const QJsonObject &payload) {
        const auto reference = payload.value("symbols").toObject();
        if (reference.isEmpty()) {
          openAskingForDebugFiles(path, options);
          return;
        }
        // A modal must run after the query delivery barrier is released.
        QTimer::singleShot(0, this, [this, path, options, reference, owner] {
          // Like the debug questions, it greets alone before the workbench
          // shows; No, Escape and closing it load the image as if stripped.
          SymbolTablePromptDialog dialog(
              reference, owner ? owner.data() : (isVisible() ? this : nullptr));
          dialog.setAttribute(Qt::WA_QuitOnClose, false);
          auto chosen = options;
          if (dialog.exec() != QDialog::Accepted)
            chosen.symbols = {{"mode", "none"}};
          openAskingForDebugFiles(path, chosen);
        });
      },
      [this, path, options](const QString &, const QString &message) {
        output_->append(tr("Could not tell whether %1 has a symbol table: %2")
                            .arg(path, message),
                        1);
        openAskingForDebugFiles(path, options);
      });
}

void MainWindow::openAskingForDebugFiles(const QString &path,
                                         LoadOptions options) {
  // The user's own choice of debug information stands.
  if (!options.debugInfo) {
    openNow(path, options);
    return;
  }
  // A PE names its PDB; an input that names none may carry DWARF.
  if (!options.pdb.isEmpty() || !session_.findsPdbFiles()) {
    openAskingForDwarf(path, options);
    return;
  }
  const QPointer<QWidget> owner = isVisible() ? commandWindow() : nullptr;
  session_.read(
      QStringLiteral("pdb_reference"), {{"path", path}}, this,
      [this, path, options, owner](const QJsonObject &payload) {
        const auto reference = payload.value("pdb").toObject();
        if (reference.isEmpty()) {
          openAskingForDwarf(path, options);
          return;
        }
        // A modal must run after the query delivery barrier is released.
        QTimer::singleShot(0, this, [this, path, options, reference, owner] {
          // Before the workbench shows, the question greets alone, as the
          // quick start does; closing it answers no.
          PdbPromptDialog dialog(session_, path, reference, nullptr,
                                 owner ? owner.data()
                                       : (isVisible() ? this : nullptr));
          dialog.setAttribute(Qt::WA_QuitOnClose, false);
          dialog.exec();
          auto chosen = options;
          chosen.pdb = dialog.choice();
          // A PDB outranks a MAP: the MAP is asked about only without one.
          if (chosen.pdb.contains(QStringLiteral("path")))
            openNow(path, chosen);
          else
            openAskingForMap(path, chosen);
        });
      },
      [this, path, options](const QString &, const QString &message) {
        output_->append(
            tr("Could not tell which PDB %1 names: %2").arg(path, message), 1);
        openNow(path, options);
      });
}

void MainWindow::openAskingForDwarf(const QString &path, LoadOptions options) {
  if (!options.dwarf.isEmpty() || !session_.findsDwarf()) {
    openAskingForMap(path, options);
    return;
  }
  const QPointer<QWidget> owner = isVisible() ? commandWindow() : nullptr;
  session_.read(
      QStringLiteral("dwarf_reference"), {{"path", path}}, this,
      [this, path, options, owner](const QJsonObject &payload) {
        const auto reference = payload.value("dwarf").toObject();
        if (reference.isEmpty()) {
          openAskingForMap(path, options);
          return;
        }
        // A modal must run after the query delivery barrier is released.
        QTimer::singleShot(0, this, [this, path, options, reference, owner] {
          // Like the PDB question, it greets alone before the workbench
          // shows; No, Escape and closing it load the input without it.
          DwarfPromptDialog dialog(
              reference, owner ? owner.data() : (isVisible() ? this : nullptr));
          dialog.setAttribute(Qt::WA_QuitOnClose, false);
          auto chosen = options;
          // DWARF outranks a MAP: the MAP is asked about only without it.
          if (dialog.exec() == QDialog::Accepted) {
            openNow(path, chosen);
            return;
          }
          chosen.dwarf = {{"mode", "none"}};
          openAskingForMap(path, chosen);
        });
      },
      [this, path, options](const QString &, const QString &message) {
        output_->append(
            tr("Could not tell whether %1 has DWARF: %2").arg(path, message),
            1);
        openNow(path, options);
      });
}

void MainWindow::openAskingForMap(const QString &path, LoadOptions options) {
  // The questions before this one found no PDB or DWARF, or the user
  // declined it: only then does the open read the MAP beside the input.
  if (!options.map.isEmpty() || !session_.findsMapFiles()) {
    openNow(path, options);
    return;
  }
  const QPointer<QWidget> owner = isVisible() ? commandWindow() : nullptr;
  session_.read(
      QStringLiteral("map_reference"), {{"path", path}}, this,
      [this, path, options, owner](const QJsonObject &payload) {
        const auto reference = payload.value("map").toObject();
        if (reference.isEmpty()) {
          openNow(path, options);
          return;
        }
        // A modal must run after the query delivery barrier is released.
        QTimer::singleShot(0, this, [this, path, options, reference, owner] {
          // It greets alone before the workbench shows, as the others do;
          // No, Escape and closing it load the input without the MAP.
          MapPromptDialog dialog(
              reference, owner ? owner.data() : (isVisible() ? this : nullptr));
          dialog.setAttribute(Qt::WA_QuitOnClose, false);
          auto chosen = options;
          if (dialog.exec() != QDialog::Accepted)
            chosen.map = {{"mode", "none"}};
          // The open passes over a MAP of another build unless it is named.
          else if (MapPromptDialog::mismatched(reference))
            chosen.map = {{"path", reference.value("path")},
                          {"mode", "unchecked"}};
          else
            chosen.acceptedMap = reference.value("path").toString();
          openNow(path, chosen);
        });
      },
      [this, path, options](const QString &, const QString &message) {
        output_->append(tr("Could not tell whether %1 has a MAP file: %2")
                            .arg(path, message),
                        1);
        openNow(path, options);
      });
}

void MainWindow::openDialog() {
  startupQuickStartPending_ = false;
  if (const QString path = askForFile(commandWindow()); !path.isEmpty())
    chooseLoader(path);
}

QString MainWindow::askForFile(QWidget *parent) const {
  // The folder of the file the user opened, not of a working copy of it.
  const QString start = QFileInfo(session_.projectPath()).absolutePath();
  return QFileDialog::getOpenFileName(
      parent, tr("Open binary or database"),
      start.isEmpty() ? QDir::homePath() : start,
      tr("All files (*);;NeverD databases (*.nddb)"));
}

void MainWindow::chooseLoader(const QString &path) {
  startupQuickStartPending_ = false;
  // A project opens as it was saved, and an engine that cannot list its
  // loaders opens files as before.
  if (!session_.identifiesFiles() || ProjectDatabase::hasState(path)) {
    openFile(path);
    return;
  }
  const QPointer<QWidget> owner = isVisible() ? commandWindow() : nullptr;
  session_.read(
      QStringLiteral("identify"), {{"path", path}}, this,
      [this, path, owner](const QJsonObject &payload) {
        // Before the workbench shows, the dialog greets alone, as IDA's
        // does for a file named at startup.
        LoadFileDialog dialog(path, payload.value("rows").toArray(),
                              owner ? owner.data()
                                    : (isVisible() ? this : nullptr));
        dialog.setAttribute(Qt::WA_QuitOnClose, false);
        dialog.setIndicator(
            QSettings().value(AnalysisIndicatorKey, true).toBool());
        dialog.setBinaryProcessor(
            QSettings().value(BinaryProcessorKey).toString());
        if (dialog.exec() != QDialog::Accepted || dialog.row() < 0) {
          // Without a file to open, the workbench takes its place.
          showWorkbench();
          return;
        }
        QSettings().setValue(AnalysisIndicatorKey, dialog.indicator());
        // The processor the user picked, not one the bytes named.
        if (const auto processor = dialog.options().processor;
            !processor.isEmpty() && processor != QLatin1String("auto"))
          QSettings().setValue(BinaryProcessorKey, processor);
        applyIndicator();
        openAskingForDebugInfo(path, dialog.options());
      },
      [this, path](const QString &, const QString &message) {
        output_->append(
            tr("Cannot determine how to load %1: %2").arg(path, message), 1);
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
  const QPointer<QWidget> owner = commandWindow();
  const auto epoch = session_.epoch();
  session_.read(
      QStringLiteral("identify"), {{"path", path}}, this,
      [this, path, load, owner, epoch](const QJsonObject &payload) {
        if (!owner || session_.epoch() != epoch)
          return;
        LoadFileDialog dialog(path, payload.value("rows").toArray(), owner);
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
      [this, epoch](const QString &, const QString &) {
        if (session_.epoch() == epoch)
          session_.reload();
      });
}

void MainWindow::loadPdb() {
  if (!session_.loaded() || !session_.loadsPdbFiles())
    return;
  microsoftSymbols_->cancel();
  const QPointer<QWidget> owner = commandWindow();
  const auto epoch = session_.epoch();
  const auto path = QFileDialog::getOpenFileName(
      owner, tr("Load PDB file"),
      QFileInfo(session_.projectPath()).absolutePath(),
      tr("PDB files (*.pdb);;All files (*)"));
  if (path.isEmpty() || !session_.loaded() || session_.epoch() != epoch)
    return;
  session_.read(
      QStringLiteral("pdb_inspect"), {{"path", path}}, this,
      [this, epoch, owner](const QJsonObject &result) {
        // A modal must run after the query delivery barrier is released.
        QTimer::singleShot(0, this, [this, epoch, owner, result] {
          if (!owner || !session_.loaded() || session_.epoch() != epoch)
            return;
          const bool verified = result.value("verified").toBool();
          if (!verified) {
            if (!result.value("supports_unverified_analysis").toBool()) {
              output_->append(
                  tr("This engine cannot use an unverified PDB for analysis"),
                  1);
              return;
            }
            const auto answer = QMessageBox::warning(
                owner, tr("Use unverified PDB for analysis"),
                tr("This PE has no PDB identity (GUID/age) to match against. "
                   "The selected PDB's machine and section layout match, "
                   "but its build cannot be verified.\n\n"
                   "Use this PDB for analysis? Its symbols, types, function "
                   "signatures and address ranges will be used as analysis "
                   "input. Choosing the wrong PDB can produce incorrect "
                   "results."),
                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
            if (answer != QMessageBox::Yes || !session_.loaded() ||
                session_.epoch() != epoch)
              return;
          }
          QJsonObject selection{{"path", result.value("path")},
                                {"mode", verified ? "verified" : "analysis"},
                                {"stamp", result.value("stamp")}};
          session_.loadPdb(selection);
        });
      });
}

void MainWindow::loadDwarf() {
  if (!session_.loaded() || !session_.loadsDwarfFiles())
    return;
  const auto epoch = session_.epoch();
  const auto path = QFileDialog::getOpenFileName(
      commandWindow(), tr("Load DWARF file"),
      QFileInfo(session_.projectPath()).absolutePath(),
      tr("DWARF files (*.debug *.dSYM);;All files (*)"));
  if (path.isEmpty() || !session_.loaded() || session_.epoch() != epoch)
    return;
  session_.loadDwarf({{"path", path}});
}

void MainWindow::loadMap() {
  if (!session_.loaded() || !session_.loadsMapFiles())
    return;
  const auto epoch = session_.epoch();
  const QPointer<QWidget> owner = commandWindow();
  const auto path = QFileDialog::getOpenFileName(
      owner, tr("Load MAP file"),
      QFileInfo(session_.projectPath()).absolutePath(),
      tr("MAP files (*.map);;All files (*)"));
  if (path.isEmpty() || !session_.loaded() || session_.epoch() != epoch)
    return;
  session_.read(QStringLiteral("map_inspect"), {{"path", path}}, this,
                [this, epoch, owner](const QJsonObject &facts) {
                  // A modal must run after the query delivery barrier is
                  // released.
                  QTimer::singleShot(0, this, [this, epoch, owner, facts] {
                    if (owner)
                      confirmMap(facts, epoch, owner);
                  });
                });
}

void MainWindow::loadHeader() {
  if (!session_.loaded() || session_.readOnly() ||
      !session_.declaresLocalTypes())
    return;
  const QPointer<QWidget> owner = commandWindow();
  const auto path = QFileDialog::getOpenFileName(
      owner, tr("Parse C header file"),
      QFileInfo(session_.projectPath()).absolutePath(),
      tr("C and C++ headers (*.h *.hh *.hpp *.hxx);;All files (*)"));
  if (path.isEmpty() || !session_.loaded())
    return;
  QFile file(path);
  if (file.size() > ::neverd::limits::gui::kMainWindowMaxHeaderBytes) {
    QMessageBox::warning(
        owner, tr("Parse C header file"),
        tr("%1 is larger than 4 MiB.").arg(QDir::toNativeSeparators(path)));
    return;
  }
  if (!file.open(QIODevice::ReadOnly)) {
    QMessageBox::warning(
        owner, tr("Parse C header file"),
        tr("Cannot read %1: %2")
            .arg(QDir::toNativeSeparators(path), file.errorString()));
    return;
  }
  const QString source = QString::fromUtf8(file.readAll());
  openDock(dockNamed(QString::fromLatin1(LocalTypesDock)), true);
  localTypes_->declareSource(QStringLiteral("c"), source,
                             tr("Parse %1").arg(QFileInfo(path).fileName()));
}

void MainWindow::confirmMap(const QJsonObject &facts, quint64 epoch,
                            QWidget *owner) {
  if (!session_.loaded() || session_.epoch() != epoch)
    return;
  QString mode = QStringLiteral("checked");
  if (MapPromptDialog::mismatched(facts)) {
    const auto answer = QMessageBox::warning(
        owner, tr("Load a MAP file of another build"),
        tr("%1\n\nLoad its names anyway? Names at the wrong addresses "
           "mislead the analysis.")
            .arg(MapPromptDialog::description(facts)),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes || !session_.loaded() ||
        session_.epoch() != epoch)
      return;
    mode = QStringLiteral("unchecked");
  }
  session_.loadMap({{"path", facts.value("path")},
                    {"mode", mode},
                    {"stamp", facts.value("stamp")}});
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
    const auto sourceOptions = code->text()->sourceOptions();
    const auto epoch = session_.epoch();
    const auto valid = [this, view, function, representation, sourceOptions,
                        epoch] {
      return view && session_.epoch() == epoch &&
             view->text()->function() == function &&
             view->representation() == representation &&
             view->text()->sourceOptions() == sourceOptions;
    };
    const auto edit = [this, view, target = *target, function = *function,
                       representation, sourceOptions, epoch,
                       valid](std::optional<Address> address) {
      if (!valid())
        return;
      bool ok = false;
      const auto current = target.value("name").toString();
      if (!address)
        session_.prepareCodeName({{"address", hexAddress(function)},
                                  {"representation", representation},
                                  {"source_options", sourceOptions},
                                  {"kind", "name"},
                                  {"original", target.value("original")},
                                  {"name", target.value("original")},
                                  {"identity", target.value("identity")}},
                                 epoch);
      const auto name =
          QInputDialog::getText(view->window(), tr("Rename"), tr("New name:"),
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
                           {"source_options", sourceOptions},
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
  const QPointer<QWidget> owner = commandWindow();
  session_.read(
      QStringLiteral("resolve"), {{"query", hexAddress(address)}}, this,
      [this, address, epoch, owner](const QJsonObject &payload) {
        if (!owner || session_.epoch() != epoch)
          return;
        const QString current = payload.value("address_name").toString();
        bool ok = false;
        const auto name = QInputDialog::getText(
            owner, tr("Rename address"),
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
    const auto sourceOptions = code->text()->sourceOptions();
    const auto epoch = session_.epoch();
    const auto edit = [this, view, target = *target, function = *function,
                       representation, sourceOptions,
                       epoch](const QString &current) {
      if (!view || session_.epoch() != epoch ||
          view->text()->function() != function ||
          view->representation() != representation ||
          view->text()->sourceOptions() != sourceOptions)
        return;
      bool ok = false;
      const auto text = QInputDialog::getMultiLineText(
          view->window(), tr("Please enter text"),
          tr("Comment this source line:"), current, &ok);
      if (view) {
        view->window()->activateWindow();
        view->text()->setFocus();
      }
      if (!ok || !view || session_.epoch() != epoch ||
          view->text()->function() != function ||
          view->representation() != representation ||
          view->text()->sourceOptions() != sourceOptions)
        return;
      if (const auto address = addressValue(target.value("mapped_address")))
        session_.setComment(*address, text, epoch);
      else {
        auto payload = target;
        payload["address"] = hexAddress(function);
        payload["representation"] = representation;
        payload["source_options"] = sourceOptions;
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
  const QPointer<QWidget> owner = commandWindow();
  session_.read(QStringLiteral("resolve"), {{"query", hexAddress(*address)}},
                this,
                [this, address, epoch, owner](const QJsonObject &payload) {
                  if (!owner || session_.epoch() != epoch)
                    return;
                  bool ok = false;
                  const auto text = QInputDialog::getMultiLineText(
                      owner, tr("Please enter text"),
                      tr("Comment at %1:").arg(displayAddress(*address)),
                      payload.value("comment").toString(), &ok);
                  if (ok)
                    session_.setComment(*address, text, epoch);
                });
}

void MainWindow::jumpAnywhere() {
  if (!session_.loaded())
    return;
  const auto epoch = session_.epoch();
  const auto base = currentAddress();
  const QPointer<DisassemblyView> assembly = disassembly_;
  const bool hex = hexActive_;
  auto *dialog = new JumpDialog(session_, commandView());
  dialog->setObjectName(QStringLiteral("jumpDialog"));
  connect(dialog, &QDialog::accepted, this,
          [this, dialog, epoch, base, assembly, hex] {
            if (!assembly || session_.epoch() != epoch)
              return;
            const auto text = dialog->result();
            const auto serial = ++codeNavigationSerial_;
            resolveExpression(
                session_, this, text, base,
                [this, text, epoch, serial, assembly,
                 hex](std::optional<Address> value, const QString &error) {
                  if (!assembly || session_.epoch() != epoch ||
                      serial != codeNavigationSerial_)
                    return;
                  if (!value) {
                    output_->append(
                        tr("Cannot jump to %1: %2").arg(text, error), 2);
                    return;
                  }
                  disassembly_ = assembly;
                  overview_->setGraph(assembly->graph());
                  hexActive_ = hex;
                  jump(*value);
                  (hex ? static_cast<QWidget *>(hex_) : assembly.data())
                      ->window()
                      ->activateWindow();
                });
          });
  showModelessDialog(dialog, dialog->parentWidget(), &session_);
}

void MainWindow::showCrossReferences(std::optional<Address> address, bool to) {
  if (!session_.loaded())
    return;
  const auto serial = ++codeNavigationSerial_;
  auto *view = focusedCodeView();
  if (!view) {
    showReferencesDialog(address, to, nullptr);
    return;
  }
  if (!to) {
    const auto position = view->text()->currentAddress();
    showReferencesDialog(position ? position : view->text()->function(), false,
                         view);
    return;
  }
  // A local's identity comes from the complete document; until then its
  // name could resolve as an unrelated image symbol.
  if (view->text()->loading()) {
    output_->append(
        tr("Wait for the function to finish loading before finding references"),
        1);
    return;
  }
  // A source name can denote a different function from both its source row
  // and the independent assembly cursor. Prefer its published image identity.
  const auto target = view->text()->renameTarget();
  if (target && target->value("kind").toString() == QLatin1String("local")) {
    showLocalReferences(view, *target);
    return;
  }
  if (target)
    if (const auto at = addressValue(target->value("address"))) {
      showReferencesDialog(at, to, view);
      return;
    }
  const auto query = target && !target->value("query").toString().isEmpty()
                         ? target->value("query").toString()
                         : view->text()->currentToken();
  if (query.isEmpty()) {
    const auto position = view->text()->currentAddress();
    showReferencesDialog(position ? position : view->text()->function(), to,
                         view);
    return;
  }
  const auto epoch = session_.epoch();
  const auto function = view->text()->function();
  const auto representation = view->representation();
  const auto sourceOptions = view->text()->sourceOptions();
  const auto valid = [this, view, serial, epoch, function, representation,
                      sourceOptions] {
    return serial == codeNavigationSerial_ && epoch == session_.epoch() &&
           view->text()->function() == function &&
           view->text()->sourceOptions() == sourceOptions &&
           view->representation() == representation && view->isVisible();
  };
  session_.read(
      QStringLiteral("resolve"), {{"query", query}}, view,
      [this, view, to, valid](const QJsonObject &payload) {
        const auto address = addressValue(payload.value("address"));
        // Release the query delivery barrier before the chooser asks
        // the same dispatcher to read its reference pages.
        QTimer::singleShot(0, view, [this, view, to, valid, address] {
          if (valid())
            showReferencesDialog(address, to, view);
        });
      },
      [this, query, valid](const QString &, const QString &error) {
        if (valid())
          output_->append(tr("Cannot jump to %1: %2").arg(query, error), 2);
      });
}

void MainWindow::showLocalReferences(CodeView *origin,
                                     const QJsonObject &target) {
  const QPointer<CodeView> view = origin;
  auto references = view->text()->localReferences(target);
  if (references.isEmpty())
    return;
  if (LocalReferencesDialog)
    LocalReferencesDialog->reject();
  QPointer<Dock> sourceDock;
  for (auto *dock : std::as_const(docks_))
    if (dock->widget() == view) {
      sourceDock = dock;
      break;
    }
  const auto epoch = session_.epoch();
  const auto function = view->text()->function();
  const auto representation = view->representation();
  const auto version = view->text()->documentVersion();
  const auto valid = [this, view, sourceDock, epoch, function, representation,
                      version] {
    return view && sourceDock && sourceDock->isOpen() && session_.loaded() &&
           view->text()->complete() && epoch == session_.epoch() &&
           function == view->text()->function() &&
           representation == view->representation() &&
           version == view->text()->documentVersion();
  };
  // The source view survives floating/docking; its temporary top-level
  // window does not. Keep the modeless list owned by that stable view.
  auto *dialog = new QDialog(view);
  LocalReferencesDialog = dialog;
  dialog->setObjectName(QStringLiteral("localReferencesDialog"));
  dialog->setWindowTitle(
      tr("xrefs to %1").arg(target.value("name").toString()));
  dialog->setWindowIcon(icon(QStringLiteral("xrefs")));
  dialog->resize(760, 360);
  auto *layout = new QVBoxLayout(dialog);
  auto *table = new QTableView(dialog);
  table->setObjectName(QStringLiteral("localReferencesTable"));
  auto *model = new LocalReferencesModel(
      std::move(references), {tr("Line"), tr("Column"), tr("Code")}, dialog);
  table->setModel(model);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->setWordWrap(false);
  table->verticalHeader()->hide();
  table->horizontalHeader()->setSectionResizeMode(
      0, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setSectionResizeMode(
      1, QHeaderView::ResizeToContents);
  table->horizontalHeader()->setStretchLastSection(true);
  table->selectRow(0);
  // Preview inside the originating document without taking focus from the
  // list. clicked also covers clicking its already selected initial row.
  const auto preview = [view, sourceDock, model, target,
                        valid](const QModelIndex &index) {
    if (valid())
      if (const auto *reference = model->reference(index.row()))
        if (view->text()->selectLocalReference(target, *reference))
          sourceDock->setAsCurrentTab();
  };
  connect(table, &QTableView::clicked, dialog, preview);
  connect(table->selectionModel(), &QItemSelectionModel::currentRowChanged,
          dialog, preview);
  activateOnReturn(*table);
  layout->addWidget(table);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(table, &QTableView::activated, dialog,
          [dialog](const QModelIndex &) { dialog->accept(); });
  connect(view->text(), &CodeText::statusChanged, dialog, [dialog, valid] {
    if (!valid())
      dialog->reject();
  });
  connect(dialog, &QDialog::accepted, dialog,
          [view, sourceDock, model, table, target, valid] {
            if (!valid())
              return;
            if (const auto *chosen =
                    model->reference(table->currentIndex().row()))
              if (view->text()->selectLocalReference(target, *chosen)) {
                sourceDock->raise();
                view->window()->activateWindow();
                view->text()->setFocus();
              }
          });
  showModelessDialog(dialog, view, &session_);
  table->setFocus();
}

void MainWindow::showReferencesDialog(std::optional<Address> address, bool to,
                                      CodeView *origin) {
  if (!address || !session_.loaded())
    return;
  const QPointer<CodeView> view = origin;
  const QPointer<DisassemblyView> assembly = disassembly_;
  auto *source = origin ? static_cast<QWidget *>(origin) : commandView();
  const auto epoch = session_.epoch();
  const auto function = view ? view->text()->function() : std::nullopt;
  const auto representation = view ? view->representation() : QString();
  const auto version = view ? view->text()->documentVersion() : 0;
  const auto valid = [this, view, assembly, epoch, function, representation,
                      version, code = bool(view)] {
    return session_.loaded() && session_.epoch() == epoch && assembly &&
           (!code || (view && view->text()->function() == function &&
                      view->representation() == representation &&
                      view->text()->documentVersion() == version));
  };
  const int digits = session_.bitness() == 64 ? 16 : 8;
  const QString title = (to ? tr("xrefs to %1") : tr("xrefs from %1"))
                            .arg(displayAddress(*address, digits));
  auto *dialog = new ReferencesDialog(
      session_, space_, title,
      {{"address", hexAddress(*address)}, {"direction", to ? "to" : "from"}},
      source);
  if (view)
    connect(view->text(), &CodeText::statusChanged, dialog, [dialog, valid] {
      if (!valid())
        dialog->reject();
    });
  connect(dialog, &QDialog::accepted, this,
          [this, dialog, view, assembly, valid] {
            if (!valid())
              return;
            if (const auto chosen = dialog->chosen()) {
              disassembly_ = assembly;
              overview_->setGraph(assembly->graph());
              if (view) {
                for (auto *dock : std::as_const(docks_))
                  if (dock->widget() == view)
                    dock->setAsCurrentTab();
                activateCodeName(view, hexAddress(*chosen), true);
              } else {
                navigate(*chosen);
                assembly->window()->activateWindow();
                assembly->focusContent();
              }
            }
          });
  showModelessDialog(dialog, source, &session_);
}

void MainWindow::showPseudocode(const QString &representation, CodeOpen mode) {
  auto *origin = mode == CodeOpen::Generate ? focusedCodeView() : nullptr;
  // A jump still loading decides the function, as it does for the graph:
  // decompile once it lands, not the function the cursor is leaving.
  if (!origin && disassembly_ && disassembly_->listing()->jumpPending()) {
    if (!pseudocodeAfterJump_)
      connect(
          disassembly_->listing(), &ListingView::jumpSettled, this,
          [this] {
            if (const auto pending = std::exchange(pseudocodeAfterJump_, {}))
              showPseudocode(pending->first, pending->second);
          },
          Qt::SingleShotConnection);
    pseudocodeAfterJump_ = std::pair{representation, mode};
    return;
  }
  const auto function = origin ? origin->text()->function() : currentFunction();
  if (!function) {
    output_->append(tr("Place the cursor inside a function to decompile it."),
                    1);
    return;
  }
  const bool selectAssemblyAddress = mode == CodeOpen::Switch;
  // F5 inside source refreshes that document, including its chosen language.
  // A different representation opens alongside it; generation from assembly
  // always gets an independent document. Tab alone reuses the last code pane.
  const bool refreshOrigin =
      origin && (origin->representation() == representation ||
                 (representation == CodeView::pseudocodeRepresentation() &&
                  CodeView::isSource(origin->representation())));
  auto *view = refreshOrigin ? origin
               : selectAssemblyAddress && tabCodeView_ &&
                       tabCodeView_->representation() == representation
                   ? tabCodeView_.data()
                   : codeView(representation, mode == CodeOpen::Generate);
  Dock *dock = nullptr;
  for (auto *candidate : std::as_const(codeDocks_))
    if (candidate->widget() == view) {
      dock = candidate;
      break;
    }
  openCodeDock(dock);
  if (!refreshOrigin)
    view->setRepresentation(representation);
  // Reusing the loaded function keeps its cursor and folding state. Tab is
  // the only ordinary cursor action that selects an address in the other pane.
  if (!selectAssemblyAddress || view->text()->function() != function ||
      view->text()->interrupted())
    view->showFunction(*function);
  if (selectAssemblyAddress)
    if (const auto address = disassembly_->currentAddress())
      view->text()->selectAddress(*address);
  view->window()->activateWindow();
  view->text()->setFocus();
}

void MainWindow::openCodeDock(Dock *dock) {
  Dock *assembly = nullptr;
  for (auto *candidate : std::as_const(docks_))
    if (candidate->widget() == disassembly_)
      assembly = candidate;
  if (!dock->isOpen()) {
    // Subsequent documents become tabs, so generating several functions does
    // not divide the work area into ever narrower columns. Prefer the code
    // window issuing the command, including a detached window.
    Dock *anchor = nullptr;
    auto *origin = focusedCodeView();
    for (auto *candidate : std::as_const(codeDocks_))
      if (candidate != dock && candidate->isOpen()) {
        if (candidate->widget() == origin) {
          anchor = candidate;
          break;
        }
        if (!anchor && assembly &&
            candidate->QWidget::window() == assembly->QWidget::window())
          anchor = candidate;
      }
    if (anchor) {
      anchor->addDockWidgetAsTab(dock);
      openDock(dock, true);
      return;
    }
  }
  if (assembly && assembly->isOpen() && assembly->QWidget::window() != this &&
      (!dock->isOpen() ||
       (dock->QWidget::window() == this && !focusedCodeView()))) {
    // Keep analysis in the window the user is working in. Already open panes
    // in a separate floating workspace keep their deliberate placement.
    assembly->dockWidget()->addDockWidgetToContainingWindow(
        dock->dockWidget(), KDDockWidgets::Location_OnRight,
        assembly->dockWidget());
    const int difference = assembly->dockWidget()->sizeInLayout().width() -
                           dock->dockWidget()->sizeInLayout().width();
    dock->dockWidget()->resizeInLayout(difference / 2, 0, 0, 0);
  }
  if (!dock->isOpen()) {
    auto *functions = docks_.value(FunctionsDock);
    const int functionsWidth =
        functions && functions->isOpen()
            ? functions->dockWidget()->sizeInLayout().width()
            : 0;
    openDock(dock, false, KDDockWidgets::Location_OnRight);
    if (functionsWidth)
      functions->dockWidget()->resizeInLayout(
          0, 0,
          functionsWidth - functions->dockWidget()->sizeInLayout().width(), 0);
    if (assembly && assembly->QWidget::window() == this) {
      const int difference = assembly->dockWidget()->sizeInLayout().width() -
                             dock->dockWidget()->sizeInLayout().width();
      dock->dockWidget()->resizeInLayout(difference / 2, 0, 0, 0);
    }
  }
  openDock(dock, false, KDDockWidgets::Location_OnRight);
}

void MainWindow::toggleGraph() {
  disassembly_->setGraphMode(!disassembly_->graphMode());
}

void MainWindow::searchBinary(const QString &kind, bool again) {
  QString &last = kind == QLatin1String("text")        ? lastTextSearch_
                  : kind == QLatin1String("immediate") ? lastImmediateSearch_
                                                       : lastByteSearch_;
  if (!again || last.isEmpty()) {
    bool ok = false;
    const auto text = QInputDialog::getText(
        commandWindow(),
        kind == QLatin1String("text")        ? tr("Text search")
        : kind == QLatin1String("immediate") ? tr("Immediate search")
                                             : tr("Binary search"),
        kind == QLatin1String("text") ? tr("String:")
        : kind == QLatin1String("immediate")
            ? tr("Integer (decimal or 0x hex; negative allowed):")
            : tr("Hex bytes (e.g. 48 8B 05):"),
        QLineEdit::Normal, last, &ok);
    if (!ok || text.trimmed().isEmpty())
      return;
    last = text.trimmed();
  }
  const auto pattern = last;
  const bool backward = !searchDown_;
  const auto serial = ++searchSerial_, epoch = session_.epoch();
  const auto revision = session_.revision();
  if (searchSubscription_)
    session_.queries().unsubscribe(searchSubscription_);
  if (auto *results = choosers_.value(int(ChooserKind::SearchResults)))
    results->model().setLocalRows({});
  // A previous search can still be navigating to its first match. Use its
  // settled cursor, just as the source view does, so that completion cannot
  // invalidate the search the user has just accepted.
  const auto beginSearch = [this, kind, pattern, backward, serial, epoch,
                            revision] {
    if (serial != searchSerial_ || epoch != session_.epoch() ||
        revision != session_.revision() || !session_.loaded())
      return;
    const auto from = currentAddress().value_or(0);
    auto request = std::make_shared<std::function<void(bool)>>();
    const std::weak_ptr<std::function<void(bool)>> weak = request;
    *request = [this, kind, pattern, from, backward, serial, epoch, revision,
                weak](bool wrapped) {
      auto keep = weak.lock();
      if (!keep)
        return;
      QJsonObject query{{"kind", kind},
                        {"pattern", pattern},
                        {"limit", 4096},
                        {"backward", backward}};
      if (!wrapped)
        query["from"] = hexAddress(from);
      searchSubscription_ = session_.read(
          QStringLiteral("search"), query, this,
          [this, kind, pattern, serial, epoch, revision, from, backward,
           wrapped, keep](const QJsonObject &payload) {
            if (serial != searchSerial_ || epoch != session_.epoch() ||
                revision != session_.revision() || !session_.loaded() ||
                currentAddress().value_or(0) != from)
              return;
            if (payload.value("building").toBool()) {
              QTimer::singleShot(
                  ::neverd::limits::gui::
                      kMainWindowSearchBinaryBeginSearchRequestSearchSubscriptionSingleShotMilliseconds,
                  this, [keep, wrapped] { (*keep)(wrapped); });
              return;
            }
            if (payload.value("coverage").toString() == "partial")
              output_->append(
                  tr("Immediate search has undecoded ranges; results "
                     "cover decoded executable ranges only."),
                  1);
            const auto items = payload.value("items").toArray();
            if (items.isEmpty() && !wrapped) {
              (*keep)(true);
              return;
            }
            if (items.isEmpty()) {
              output_->append(tr("Search failed: %1 not found").arg(pattern),
                              1);
              return;
            }
            auto *dock = chooserDock(ChooserKind::SearchResults);
            choosers_.value(int(ChooserKind::SearchResults))
                ->model()
                .setLocalRows(items);
            openDock(dock, false);
            if (!payload.value("complete").toBool())
              output_->append(
                  tr("Showing %1 matches; use Next to continue the search.")
                      .arg(items.size()),
                  0);
            // The worker already ordered and bounded the page from this cursor.
            if (const auto address =
                    addressValue(items.first().toObject().value("address")))
              navigate(*address);
          },
          [this, serial, epoch](const QString &, const QString &message) {
            if (serial == searchSerial_ && epoch == session_.epoch())
              output_->append(message, 2);
          });
    };
    (*request)(false);
  };
  if (disassembly_->listing()->jumpPending())
    connect(disassembly_->listing(), &ListingView::jumpSettled, this,
            beginSearch, Qt::SingleShotConnection);
  else
    beginSearch();
}

void MainWindow::queryRegister(const QString &kind) {
  if (focusedCodeView())
    return;
  const auto function = currentFunction(), address = currentAddress();
  if (!function || !address)
    return;
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  const QPointer<QWidget> owner = commandView();
  const auto token = disassembly_->currentToken();
  QJsonObject request{{"address", hexAddress(*function)},
                      {"instruction", hexAddress(*address)},
                      {"kind", "registers"}};
  session_.read(
      "register_flow", request, this,
      [this, kind, function, address, epoch, revision, token, request,
       owner](const QJsonObject &inventory) mutable {
        const auto valid = [&] {
          return owner && session_.epoch() == epoch &&
                 session_.revision() == revision &&
                 currentFunction() == function && currentAddress() == address;
        };
        if (!valid())
          return;
        QStringList names;
        for (const auto &value : inventory.value("registers").toArray())
          names.append(value.toObject().value("name").toString());
        if (names.isEmpty()) {
          output_->append(tr("This instruction has no retained physical "
                             "register operands."),
                          1);
          return;
        }
        QString name;
        for (const auto &candidate : names)
          if (candidate.compare(token, Qt::CaseInsensitive) == 0)
            name = candidate;
        if (name.isEmpty()) {
          if (names.size() == 1)
            name = names.first();
          else {
            bool accepted = false;
            name = QInputDialog::getItem(
                owner, tr("Physical register"),
                tr("Definitions and static values are before the instruction; "
                   "uses start after it."),
                names, 0, false, &accepted);
            if (!accepted || !valid())
              return;
          }
        }
        request["kind"] = kind;
        request["register"] = name;
        session_.read(
            "register_flow", request, this,
            [this, kind, function, address, epoch, revision,
             owner](const QJsonObject &reply) {
              if (!owner || session_.epoch() != epoch ||
                  session_.revision() != revision ||
                  currentFunction() != function || currentAddress() != address)
                return;
              if (kind == "value") {
                auto *dialog = new QMessageBox(
                    QMessageBox::Information, tr("Static register value"),
                    reply.value("register").toString() + " @ " +
                        hexAddress(*address) + " (before instruction)\n" +
                        (reply.value("value").isString()
                             ? reply.value("value").toString()
                             : tr("Unknown: ") +
                                   reply.value("reason").toString()),
                    QMessageBox::Ok, owner);
                dialog->setObjectName(QStringLiteral("registerValueDialog"));
                showModelessDialog(dialog, owner, &session_);
                return;
              }
              auto items = reply.value("items").toArray();
              for (int i = 0; i < items.size(); ++i) {
                auto item = items[i].toObject();
                item["context"] = item.value("kind").toString() + ": " +
                                  item.value("context").toString() + " - " +
                                  item.value("name").toString();
                items[i] = item;
              }
              if (items.isEmpty()) {
                output_->append(
                    tr("No register flow sites within this function."), 0);
                return;
              }
              auto *dock = chooserDock(ChooserKind::SearchResults);
              choosers_.value(int(ChooserKind::SearchResults))
                  ->model()
                  .setLocalRows(items);
              openDock(dock, false);
              if (items.size() == 1 && !reply.value("unknown").toBool())
                if (auto target =
                        addressValue(items.first().toObject().value("address")))
                  navigate(*target);
            },
            [this, epoch, revision](const QString &, const QString &message) {
              if (epoch == session_.epoch() && revision == session_.revision())
                output_->append(message, 2);
            });
      },
      [this, epoch, revision](const QString &, const QString &message) {
        if (epoch == session_.epoch() && revision == session_.revision())
          output_->append(message, 2);
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
  const auto serial = ++codeNavigationSerial_;
  const auto epoch = session_.epoch();
  auto *view = disassembly_;
  session_.read(
      QStringLiteral("functions"),
      {{"offset", 0},
       {"limit", 512},
       {"sort", "address"},
       {"descending", !forward},
       {"from", hexAddress(*address)}},
      this,
      [this, view, serial, epoch, address,
       forward](const QJsonObject &payload) {
        if (serial != codeNavigationSerial_ || epoch != session_.epoch() ||
            view != disassembly_)
          return;
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
  const QPointer<QWidget> owner = commandView();
  const auto epoch = session_.epoch();
  const auto base = currentAddress();
  auto *input = new QInputDialog(owner);
  input->setObjectName(QStringLiteral("calculatorDialog"));
  input->setWindowTitle(tr("Evaluate expression"));
  input->setLabelText(tr("Expression:"));
  input->setInputMode(QInputDialog::TextInput);
  connect(input, &QDialog::accepted, this, [this, input, owner, epoch, base] {
    const auto text = input->textValue();
    if (text.trimmed().isEmpty() || !owner || session_.epoch() != epoch)
      return;
    resolveExpression(
        session_, this, text, base,
        [this, owner, epoch](std::optional<Address> value,
                             const QString &error) {
          if (!owner || session_.epoch() != epoch)
            return;
          const QString result =
              value ? tr("Hex: %1h\nDecimal: %2\nOctal: %3\nSigned: %4")
                          .arg(displayAddress(*value))
                          .arg(*value)
                          .arg(QString::number(*value, 8))
                          .arg(qint64(*value))
                    : error;
          auto *dialog = new QMessageBox(
              value ? QMessageBox::Information : QMessageBox::Warning,
              tr("Evaluate expression"), result, QMessageBox::Ok, owner);
          dialog->setObjectName(QStringLiteral("calculatorResultDialog"));
          showModelessDialog(dialog, owner, &session_);
          if (value)
            output_->append(
                result.split(QLatin1Char('\n')).join(QStringLiteral("  ")), 0);
        });
  });
  showModelessDialog(input, owner, &session_);
}

void MainWindow::showAbout() {
  auto *dialog = new QMessageBox(
      QMessageBox::NoIcon, tr("About NeverD"),
      tr("<h3>NeverD %1</h3><p>Interactive disassembler and decompiler "
         "workbench.</p><p>Licensed under the GNU Affero General Public "
         "License v3. Icons are original NeverD artwork.</p>"
         "<p><a href=\"https://github.com/NeverSight/NeverD\">"
         "github.com/NeverSight/NeverD</a></p>")
          .arg(QCoreApplication::applicationVersion()),
      QMessageBox::Ok, commandView());
  dialog->setObjectName(QStringLiteral("aboutDialog"));
  dialog->setIconPixmap(
      windowIcon().pixmap(style()->pixelMetric(QStyle::PM_MessageBoxIconSize)));
  showModelessDialog(dialog, dialog->parentWidget());
}

void MainWindow::chooseFont() {
  bool ok = false;
  const QFont font =
      QFontDialog::getFont(&ok, Theme::instance().codeFont(), commandWindow(),
                           tr("Code font"), QFontDialog::MonospacedFonts);
  if (ok)
    Theme::instance().setCodeFont(font);
}

void MainWindow::chooseTheme() {
  QStringList names{tr("Dark (Visual Studio Code Dark+)"),
                    tr("Light (Visual Studio Code Light+)")};
  bool ok = false;
  const auto choice = QInputDialog::getItem(
      commandWindow(), tr("Colors"), tr("Theme:"), names,
      Theme::instance().mode() == Theme::Mode::Dark ? 0 : 1, false, &ok);
  if (ok)
    Theme::instance().setMode(choice == names.front() ? Theme::Mode::Dark
                                                      : Theme::Mode::Light);
}

void MainWindow::showOptions() {
  QDialog dialog(commandWindow());
  dialog.setWindowTitle(tr("General options"));
  auto *form = new QFormLayout(&dialog);
  auto *bytes = new QSpinBox(&dialog);
  bytes->setRange(0, ::neverd::limits::gui::kMainWindowMaxOpcodeBytes);
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
  auto *symbolic =
      new QCheckBox(tr("Symbolic simplification for every function"), &dialog);
  symbolic->setObjectName(QStringLiteral("optionsSymbolicEvery"));
  symbolic->setChecked(SourceDefaults::instance().symbolic());
  form->addRow(symbolic);
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
  SourceDefaults::instance().setSymbolic(symbolic->isChecked());
}

void MainWindow::showStringOptions() {
  const QPointer<QWidget> owner = commandWindow();
  const auto epoch = session_.epoch();
  const auto failed = [this](const QString &, const QString &message) {
    output_->append(tr("String options are unavailable: %1").arg(message), 2);
  };
  session_.read(
      QStringLiteral("string_encodings"), {}, this,
      [this, failed, owner, epoch](const QJsonObject &encodings) {
        if (!owner || session_.epoch() != epoch)
          return;
        session_.read(
            QStringLiteral("string_options"), {}, this,
            [this, encodings, owner, epoch](const QJsonObject &current) {
              if (owner && session_.epoch() == epoch)
                stringOptionsDialog(encodings.value("items").toArray(), current,
                                    owner);
            },
            failed);
      },
      failed);
}

void MainWindow::stringOptionsDialog(const QJsonArray &encodings,
                                     const QJsonObject &current,
                                     QWidget *owner) {
  QDialog dialog(owner);
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
  detect->setToolTip(tr("Also read C strings that are not UTF-8 in %1, when "
                        "the code pages of only one script read them as text")
                         .arg(commonSpellings.join(QStringLiteral(", "))));
  preferred->setToolTip(tr("Try this code page first for C strings that are "
                           "not UTF-8; it wins when other code pages read "
                           "them too"));
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
  length->setRange(1, ::neverd::limits::gui::kMainWindowMaxStringMinLength);
  length->setValue(
      current.value("min_length")
          .toInt(::neverd::limits::gui::kMainWindowDefaultStringMinLength));
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

QWidget *MainWindow::commandFocusWidget() const {
  if (contextMenuFocus_ && !QApplication::activeModalWidget())
    return contextMenuFocus_;
  return QApplication::focusWidget();
}

QWidget *MainWindow::commandView() const {
  if (auto *focus = commandFocusWidget())
    for (auto *dock : docks_)
      if (dock == focus || dock->isAncestorOf(focus))
        return dock->widget();
  if (auto *active = QApplication::activeWindow())
    for (auto *dock : docks_)
      if (dock->QWidget::window() == active && dock->isFocused())
        return dock->widget();
  return const_cast<MainWindow *>(this);
}

QWidget *MainWindow::commandWindow() const {
  if (auto *focus = commandFocusWidget())
    for (auto *dock : docks_)
      if (dock == focus || dock->isAncestorOf(focus))
        return dock->QWidget::window();
  // Activating a window shortcut may briefly leave the focus widget null.
  // Keep its active floating owner instead of falling back behind it.
  if (auto *active = QApplication::activeWindow())
    for (auto *dock : docks_)
      if (dock->QWidget::window() == active)
        return active;
  return const_cast<MainWindow *>(this);
}

void MainWindow::exportData() {
  const auto range = hexActive_ ? hex_->byteSelection()
                                : disassembly_->listing()->byteSelection();
  if (!range || focusedCodeView())
    return;
  auto *dialog = new ExportDataDialog(session_, range->first, range->second,
                                      commandView());
  showModelessDialog(dialog, dialog->parentWidget(), &session_);
}

void MainWindow::copySelection() {
  // The view holding the focus copies its selection: any code view, a
  // chooser's rows, the hex view's bytes or a text field; the disassembly
  // otherwise.
  for (QWidget *widget = commandFocusWidget(); widget;
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

void MainWindow::chooseLocation(ChooserKind kind, ActionId command) {
  const auto epoch = session_.epoch();
  const QPointer<DisassemblyView> assembly = disassembly_;
  const QPointer<CodeView> code = lastCodeView_;
  const bool hex = hexActive_;
  auto *dialog = new QDialog(commandView());
  dialog->setObjectName(QStringLiteral("locationChooser"));
  dialog->setWindowTitle(
      actions_.action(command)->text().remove(QLatin1Char('&')));
  dialog->resize(::neverd::limits::gui_config::kLocationChooserDialogSize);
  dialog->setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(dialog);
  auto *view = new ChooserView(session_, space_, kind, dialog);
  view->setObjectName(QStringLiteral("locationChooserView"));
  view->table()->setSelectionMode(QAbstractItemView::SingleSelection);
  layout->addWidget(view);
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
  auto *ok = buttons->button(QDialogButtonBox::Ok);
  ok->setEnabled(false);
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, dialog, [view, dialog] {
    if (view->currentAddress())
      dialog->accept();
  });
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  connect(view, &ChooserView::activated, dialog, &QDialog::accept);
  connect(view->table()->selectionModel(), &QItemSelectionModel::currentChanged,
          dialog,
          [view, ok] { ok->setEnabled(view->currentAddress().has_value()); });
  connect(&view->model(), &ChooserModel::totalChanged, dialog,
          [view, ok](int total) {
            if (total && !view->table()->currentIndex().isValid())
              view->table()->setCurrentIndex(view->model().index(0, 0));
            ok->setEnabled(view->currentAddress().has_value());
          });
  view->model().reload();
  view->table()->setFocus();
  connect(dialog, &QDialog::accepted, this,
          [this, view, assembly, code, hex, epoch, kind] {
            if (!assembly || session_.epoch() != epoch)
              return;
            if (const auto target = view->currentAddress()) {
              disassembly_ = assembly;
              overview_->setGraph(assembly->graph());
              lastCodeView_ = code;
              hexActive_ = hex;
              if (kind == ChooserKind::Functions) {
                if (code)
                  for (auto *dock : std::as_const(docks_))
                    if (dock->widget() == code)
                      dock->setAsCurrentTab();
                activateFunction(*target);
              } else {
                jump(*target);
              }
            }
          });
  showModelessDialog(dialog, dialog->parentWidget(), &session_);
}

void MainWindow::showShortcuts() {
  auto *dialog = new QDialog(commandView());
  dialog->setObjectName(QStringLiteral("shortcutsDialog"));
  dialog->setWindowTitle(tr("Shortcuts"));
  dialog->resize(::neverd::limits::gui_config::kShortcutsDialogSize);
  dialog->setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(dialog);
  auto *note = new QLabel(
      tr("List commands take priority while their list has focus."), dialog);
  note->setWordWrap(true);
  layout->addWidget(note);
  auto *filter = new QLineEdit(dialog);
  filter->setObjectName(QStringLiteral("shortcutsFilter"));
  filter->setPlaceholderText(tr("Filter by action, shortcut or scope"));
  filter->setClearButtonEnabled(true);
  layout->addWidget(filter);
  auto *tree = new QTreeWidget(dialog);
  tree->setObjectName(QStringLiteral("shortcutsList"));
  tree->setHeaderLabels({tr("Action"), tr("Shortcut"), tr("Scope")});
  tree->setRootIsDecorated(false);
  tree->setUniformRowHeights(true);
  const auto add = [&](const QString &text, const QList<QKeySequence> &keys,
                       const QString &scope, const QIcon &icon = {}) {
    auto *item = new QTreeWidgetItem(tree);
    item->setIcon(0, icon);
    item->setText(0, QString(text).remove(QLatin1Char('&')));
    const auto hint = shortcutHint(keys);
    item->setText(1, hint.isEmpty() ? tr("Unassigned") : hint);
    item->setText(2, scope);
    item->setData(1, Qt::UserRole,
                  shortcutHint(keys, QKeySequence::PortableText));
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
  };
  for (auto *action : actions_.allActions()) {
    auto *item =
        add(action->text(), action->shortcuts(),
            actions_.viewActions().contains(action) ? tr("Analysis views")
                                                    : tr("Workbench"),
            action->icon());
    item->setData(0, Qt::UserRole, action->objectName());
    item->setToolTip(0, action->statusTip());
  }
  add(actions_.action(ActionId::EditFunction)->text(),
      {ChooserView::commandKey(ChooserCommand::Edit)}, tr("Functions"));
  add(tr("Quick filter"), {QKeySequence(QKeySequence::Find)}, tr("Lists"));
  add(tr("Add types…"), {LocalTypesView::commandKey(TypesCommand::Add)},
      tr("Local Types"));
  add(tr("Edit type…"), {LocalTypesView::commandKey(TypesCommand::Edit)},
      tr("Local Types"));
  add(tr("Delete types…"), {LocalTypesView::commandKey(TypesCommand::Delete)},
      tr("Local Types"));
  tree->sortItems(0, Qt::AscendingOrder);
  tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
  tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
  tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  connect(filter, &QLineEdit::textChanged, dialog, [tree](const QString &text) {
    const QString query = text.trimmed();
    for (int row = 0; row < tree->topLevelItemCount(); ++row) {
      auto *item = tree->topLevelItem(row);
      bool matches = item->data(1, Qt::UserRole)
                         .toString()
                         .contains(query, Qt::CaseInsensitive);
      for (int column = 0; column < tree->columnCount(); ++column)
        matches |= item->text(column).contains(query, Qt::CaseInsensitive);
      item->setHidden(!matches);
    }
  });
  layout->addWidget(tree);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
  connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
  layout->addWidget(buttons);
  filter->setFocus();
  showModelessDialog(dialog, dialog->parentWidget());
}

void MainWindow::showCommandPalette() {
  auto *owner = commandView();
  const QPointer<QWidget> focus = commandFocusWidget();
  // A click on the title bar's command center drops the palette from it, as
  // Visual Studio Code's does.  A key opens it over the pane its commands act
  // on, which also serves focus mode, whose caption has no command center.
  QWidget *center = titleBar_ ? titleBar_->clickedCommandCenter() : nullptr;
  auto *dialog =
      new CommandPaletteDialog(owner, Qt::Popup | Qt::FramelessWindowHint);
  dialog->setObjectName(QStringLiteral("commandPalette"));
  dialog->setWindowTitle(tr("Command palette"));
  const QSize size = ::neverd::limits::gui_config::kCommandPaletteSize;
  dialog->resize(center ? std::max(size.width(), center->width())
                        : size.width(),
                 size.height());
  auto *layout = new QVBoxLayout(dialog);
  auto *filter = new QLineEdit(dialog);
  filter->setPlaceholderText(tr("Type a command"));
  auto *list = new QListWidget(dialog);
  activateOnReturn(*list);
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
                    shortcutHint(action->shortcuts()));
      item->setData(Qt::UserRole, action->objectName());
    }
    if (list->count())
      list->setCurrentRow(0);
  };
  populate({});
  connect(filter, &QLineEdit::textChanged, dialog, populate);
  connect(filter, &QLineEdit::returnPressed, dialog, &QDialog::accept);
  connect(list, &QListWidget::itemActivated, dialog, &QDialog::accept);
  if (center) {
    QRect place(
        QPoint(center->mapToGlobal(QPoint(center->width() / 2, 0)).x() -
                   dialog->width() / 2,
               titleBar_->mapToGlobal(QPoint(0, titleBar_->height())).y()),
        dialog->size());
    if (const QScreen *screen = center->screen()) {
      const QRect area = screen->availableGeometry();
      place.moveLeft(
          std::clamp(place.left(), area.left(),
                     std::max(area.left(), area.right() + 1 - place.width())));
    }
    dialog->move(place.topLeft());
  } else {
    dialog->move(owner->mapToGlobal(owner->rect().center()) -
                 dialog->rect().center());
  }
  connect(dialog, &QDialog::accepted, this, [this, list, focus] {
    if (!list->currentItem())
      return;
    const QString id = list->currentItem()->data(Qt::UserRole).toString();
    // Release the popup grab before a command opens another dialog. The
    // command still belongs to the analysis widget that opened the palette.
    QTimer::singleShot(0, this, [this, id, focus] {
      QScopedValueRollback<QPointer<QWidget>> context(contextMenuFocus_, focus);
      for (auto *action : actions_.allActions())
        if (action->objectName() == id && action->isEnabled()) {
          lastPaletteCommand_ = id;
          action->trigger();
          return;
        }
    });
  });
  showModelessDialog(dialog, owner, &session_);
  filter->setFocus();
}

void MainWindow::exportCurrent(bool pseudocode) {
  // The pseudocode keeps the language its window shows.
  auto *pseudocodeView = pseudocode ? focusedCodeView() : nullptr;
  const auto function =
      pseudocodeView ? pseudocodeView->text()->function() : currentFunction();
  if (!function)
    return;
  if (pseudocode && (!pseudocodeView ||
                     !CodeView::isSource(pseudocodeView->representation())))
    pseudocodeView = codeView(CodeView::pseudocodeRepresentation());
  const QString language =
      pseudocodeView ? pseudocodeView->text()->language() : QString();
  const QString suffix =
      !pseudocode                         ? QStringLiteral(".lst")
      : language == QLatin1String("rust") ? QStringLiteral(".rs")
      : language == QLatin1String("go")   ? QStringLiteral(".go")
                                          : QStringLiteral(".c");
  const auto path = QFileDialog::getSaveFileName(
      commandWindow(),
      pseudocode ? tr("Create source file") : tr("Create LST file"),
      QFileInfo(session_.filePath()).completeBaseName() + suffix);
  if (path.isEmpty())
    return;
  if (pseudocode) {
    // A private document pins this export to its requested function. The pane
    // may still hold preceding source, be hidden, or move while pages arrive.
    auto *document = new CodeText(session_, this);
    const auto epoch = session_.epoch();
    const auto revision = session_.revision();
    const auto finished = std::make_shared<bool>(false);
    const auto retire = [document, finished] {
      if (std::exchange(*finished, true))
        return;
      document->cancel();
      document->deleteLater();
    };
    connect(&session_, &Session::unloaded, document, retire);
    connect(&session_, &Session::revisionChanged, document, retire);
    connect(
        document, &CodeText::statusChanged, document,
        [this, path, document, function, epoch, revision, finished, retire] {
          if (*finished || document->loading())
            return;
          if (!session_.loaded() || session_.epoch() != epoch ||
              session_.revision() != revision ||
              document->function() != function || !document->complete() ||
              document->allText().isEmpty()) {
            output_->append(
                tr("Could not export source: %1").arg(document->status()), 2);
            retire();
            return;
          }
          QSaveFile file(path);
          const auto contents =
              (document->allText() + QLatin1Char('\n')).toUtf8();
          if (!file.open(QIODevice::WriteOnly) ||
              file.write(contents) != contents.size() || !file.commit()) {
            output_->append(tr("Cannot write %1").arg(path), 2);
          } else
            output_->append(tr("Wrote %1").arg(path), 0);
          retire();
        });
    const auto options = pseudocodeView->text()->function() == function
                             ? pseudocodeView->text()->sourceOptions()
                             : pseudocodeView->text()->defaultSourceOptions();
    document->load(*function, pseudocodeView->representation(), options);
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
      showPseudocode(CodeView::pseudocodeRepresentation(), CodeOpen::Generate);
    } else {
      resolveExpression(
          session_, this, argument, currentAddress(),
          [this](std::optional<Address> value, const QString &error) {
            if (!value) {
              output_->append(error, 2);
              return;
            }
            navigate(*value);
            showPseudocode(CodeView::pseudocodeRepresentation(),
                           CodeOpen::Generate);
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
  } else if (command == QLatin1String("types")) {
    actions_.action(ActionId::ViewLocalTypes)->trigger();
    if (!argument.isEmpty())
      localTypes_->showType(argument);
  }
}

void MainWindow::contextMenu(const QPoint &globalPosition) {
  // A popup can take the keyboard focus before its action fires. Keep the
  // invoking view as the command target; modal dialogs still use their own
  // fields. Nested menus restore the enclosing menu's context.
  const QPointer<MainWindow> window = this;
  const QPointer<QWidget> previous = contextMenuFocus_;
  const auto restore = qScopeGuard([window, previous] {
    if (window) {
      window->contextMenuFocus_ = previous;
      window->updateActions();
    }
  });
  auto *origin = qobject_cast<QWidget *>(sender());
  contextMenuFocus_ = origin ? origin : commandFocusWidget();
  // The click moved the cursor: the commands act where it is now.
  updateActions();
  QMenu menu(commandWindow());
  menu.setObjectName(QStringLiteral("analysisContextMenu"));
  if (auto *code = focusedCodeView())
    addNumberActions(menu, code);
  for (const auto id : {ActionId::JumpOperand, ActionId::JumpXrefOperand,
                        ActionId::JumpXrefsTo, ActionId::JumpXrefsFrom})
    menu.addAction(actions_.action(id));
  // IDA offers the function's own dialog after its cross references.
  if (auto *edit = actions_.action(ActionId::EditFunction); edit->isEnabled())
    menu.addAction(edit);
  // The disassembly's function also opens its stack variables, and an
  // instruction the SP it leaves, where a wrong SP shows. A pseudocode
  // window's function can differ from the disassembly's, which these edit.
  if (!focusedCodeView()) {
    if (auto *frame = actions_.action(ActionId::EditStackFrame);
        frame->isEnabled())
      menu.addAction(frame);
    if (auto *pointer = actions_.action(ActionId::EditStackPointer);
        pointer->isEnabled() && disassembly_->onInstruction())
      menu.addAction(pointer);
  }
  // IDA's Pathfinding: the function under the cursor joins the call path,
  // or starts a new one.
  if (auto *add = actions_.action(ActionId::PathfinderAddWaypoint);
      add->isEnabled()) {
    auto *pathfinding =
        menu.addMenu(icon(QStringLiteral("pathfinder")), tr("Pathfinding"));
    pathfinding->setObjectName(QStringLiteral("pathfindingMenu"));
    pathfinding->addAction(add);
    pathfinding->addAction(actions_.action(ActionId::PathfinderNewPath));
  }
  menu.addSeparator();
  if (session_.keepsBytePatches()) {
    auto *patch = menu.addMenu(icon(QStringLiteral("patch")), tr("Patch"));
    preparePatchMenu(patch);
  }
  menu.addAction(actions_.action(ActionId::EditRename));
  menu.addAction(actions_.action(ActionId::EditExportData));
  // Hex-Rays offers a variable's type, and a parameter's pointer to a
  // record, after its rename.
  if (auto *type = actions_.action(ActionId::EditSetType); type->isEnabled())
    menu.addAction(type);
  for (const auto id :
       {ActionId::EditSplitVariable, ActionId::EditMapVariable,
        ActionId::EditUnmapVariable, ActionId::EditUnsplitVariable,
        ActionId::EditResetVariables})
    if (auto *action = actions_.action(id); action->isEnabled())
      menu.addAction(action);
  for (const auto id : {ActionId::EditCallType, ActionId::EditResetCallTypes})
    if (auto *action = actions_.action(id); action->isEnabled())
      menu.addAction(action);
  if (auto *convert = actions_.action(ActionId::EditConvertToStruct);
      convert->isEnabled()) {
    const auto target = focusedCodeView()->text()->renameTarget();
    if (target && target->contains(QStringLiteral("parameter")))
      menu.addAction(convert);
  }
  for (const auto id :
       {ActionId::EditRepeatableComment, ActionId::EditBookmark})
    menu.addAction(actions_.action(id));
  // IDA offers the operand's structure offset after its rename.
  if (auto *structure = actions_.action(ActionId::EditOperandStruct);
      structure->isEnabled() && !focusedCodeView())
    menu.addAction(structure);
  // An instruction's other operand forms, as the Edit menu's Operand type
  // lists them.
  if (const auto *forms = actions_.menu(QStringLiteral("EditOperandType"));
      forms && !focusedCodeView() && disassembly_->onInstruction()) {
    QMenu *operand = nullptr;
    for (auto *action : forms->actions()) {
      if (action->isSeparator()) {
        if (operand)
          operand->addSeparator();
      } else if (action->isEnabled() && !menu.actions().contains(action)) {
        if (!operand) {
          operand = menu.addMenu(forms->icon(), forms->title());
          operand->setObjectName(QStringLiteral("operandTypeMenu"));
        }
        operand->addAction(action);
      }
    }
  }
  for (const auto id :
       {ActionId::EditCreateFunction, ActionId::EditDeleteFunction,
        ActionId::EditDefineCode, ActionId::EditDefineData,
        ActionId::EditDefineString, ActionId::EditUndefine})
    if (auto *action = actions_.action(id); action->isEnabled())
      menu.addAction(action);
  // A data item also takes an array or an alignment.
  if (!focusedCodeView() && disassembly_->onData())
    for (const auto id : {ActionId::EditMakeArray, ActionId::EditMakeAlignment})
      if (auto *action = actions_.action(id); action->isEnabled())
        menu.addAction(action);
  menu.addSeparator();
  menu.addAction(actions_.action(ActionId::ViewToggleGraph));
  // Tab, the switch to the other pane's row, with its key; a code window
  // names it by where it goes.
  if (!focusedCodeView())
    menu.addAction(actions_.action(ActionId::JumpPseudocode));
  for (const auto id : {ActionId::ViewPseudocode, ActionId::ViewMedIR})
    menu.addAction(actions_.action(id));
  menu.addSeparator();
  menu.addAction(actions_.action(ActionId::EditCopy));
  menu.addAction(actions_.action(ActionId::EditCopyAddress));
  menu.addAction(actions_.action(ActionId::EditCopyLink));
  // A code window selects its whole text for Copy, as an editor does.
  if (auto *code = focusedCodeView()) {
    auto *all = menu.addAction(icon(QStringLiteral("select_all")),
                               tr("Select &all") + QLatin1Char('\t') +
                                   QKeySequence(QKeySequence::SelectAll)
                                       .toString(QKeySequence::NativeText));
    all->setObjectName(QStringLiteral("codeSelectAll"));
    connect(all, &QAction::triggered, code->text(), &CodeText::selectAll);
  }
  // A code window's folds: the declarations before the function and the
  // recognized library operations.
  if (auto *code = focusedCodeView()) {
    CodeText *text = code->text();
    menu.addSeparator();
    // Tab goes to the row's instruction and takes the focus there.
    auto *jump = actions_.action(ActionId::JumpPseudocode);
    auto *cross =
        menu.addAction(icon(QStringLiteral("jump_disassembly")),
                       tr("Jump to disassembly") + QLatin1Char('\t') +
                           jump->shortcut().toString(QKeySequence::NativeText));
    cross->setObjectName(QStringLiteral("codeJumpDisassembly"));
    connect(cross, &QAction::triggered, jump, &QAction::trigger);
    // Hex-Rays' Synchronize with: the code and the disassembly follow each
    // other's cursor, or browse apart with Tab to cross over.
    auto *follow = menu.addAction(icon(QStringLiteral("sync")),
                                  tr("Synchronize with disassembly"));
    follow->setObjectName(QStringLiteral("codeSynchronize"));
    follow->setCheckable(true);
    follow->setChecked(synchronizeCode_);
    connect(follow, &QAction::toggled, this, [this](bool enabled) {
      synchronizeCode_ = enabled;
      QSettings().setValue(SynchronizeCodeKey, enabled);
    });
    code->addSourceActions(menu);
    if (text->hasPrelude()) {
      const bool folded = text->preludeFolded();
      auto *declarations = menu.addAction(
          icon(folded ? QStringLiteral("expand") : QStringLiteral("collapse")),
          folded ? tr("Expand declarations") : tr("Collapse declarations"));
      declarations->setStatusTip(
          tr("Show or hide the includes and declarations before the function "
             "(Keypad + / Keypad -)"));
      connect(declarations, &QAction::triggered, text,
              [text, folded] { text->setPreludeFolded(!folded); });
    }
    if (text->foldableCount() > 0) {
      const bool folded = text->libraryFolded();
      auto *library =
          menu.addAction(icon(QStringLiteral("function_library")),
                         folded ? tr("Expand library operations")
                                : tr("Collapse library operations"));
      connect(library, &QAction::triggered, text,
              [text, folded] { text->setFolded(!folded); });
    }
  }
  menu.addSeparator();
  menu.addAction(actions_.action(ActionId::OptionsFont));
  menu.exec(globalPosition);
}

void MainWindow::functionsMenu(ChooserView *view, const QPoint &position) {
  auto *table = view->table();
  const QModelIndex row = table->indexAt(position);
  if (!row.isValid())
    return;
  table->setCurrentIndex(row);
  const auto entry = view->currentAddress();
  if (!entry)
    return;
  QMenu menu(table->window());
  // The commands of the disassembly's, run on the row's function.
  auto *edit = actions_.action(ActionId::EditFunction);
  auto *rowEdit = menu.addAction(edit->icon(), edit->text());
  rowEdit->setShortcut(ChooserView::commandKey(ChooserCommand::Edit));
  rowEdit->setShortcutContext(Qt::WidgetShortcut);
  rowEdit->setShortcutVisibleInContextMenu(true);
  rowEdit->setEnabled(!session_.readOnly() && session_.keepsFunctionEdits() &&
                      session_.statesFunctionAttributes());
  connect(rowEdit, &QAction::triggered, this,
          [this, entry = *entry] { editFunction(entry); });
  auto *remove = actions_.action(ActionId::EditDeleteFunction);
  auto *rowRemove = menu.addAction(remove->icon(), remove->text());
  rowRemove->setEnabled(!session_.readOnly() && session_.keepsFunctionEdits());
  connect(rowRemove, &QAction::triggered, this,
          [this, entry = *entry] { session_.deleteFunction(entry); });
  // The row's function joins the call path, or starts a new one.
  if (session_.findsCallPaths()) {
    menu.addSeparator();
    for (const auto id :
         {ActionId::PathfinderAddWaypoint, ActionId::PathfinderNewPath}) {
      auto *command = actions_.action(id);
      auto *row = menu.addAction(command->icon(), command->text());
      connect(row, &QAction::triggered, this, [this, id, entry = *entry] {
        PathfinderView *pathfinder = showPathfinder();
        if (id == ActionId::PathfinderNewPath)
          pathfinder->startPath(entry);
        else
          pathfinder->addWaypoint(entry);
      });
    }
  }
  menu.exec(table->viewport()->mapToGlobal(position));
}

void MainWindow::chooseStructOffset() {
  // Pseudocode windows keep their keys.
  if (focusedCodeView() || !session_.showsStructOffsets())
    return;
  const auto address = currentAddress();
  if (!address)
    return;
  QJsonObject query{{"address", hexAddress(*address)}};
  if (const auto function = currentFunction())
    query.insert(QStringLiteral("function"), hexAddress(*function));
  if (const auto operand = disassembly_->currentOperand())
    query.insert(QStringLiteral("operand"), *operand);
  // The edit goes to the session the dialog was opened for.
  const auto epoch = session_.epoch();
  TypePickerDialog dialog(session_, query, {}, commandWindow());
  if (dialog.exec() != QDialog::Accepted || session_.epoch() != epoch)
    return;
  session_.formatOperandStruct(*address, dialog.operand(), dialog.typeName(),
                               dialog.delta(), dialog.others(), epoch);
}

void MainWindow::setType() {
  if (session_.readOnly() || !session_.typesFunctions())
    return;
  // The parameter under a pseudocode cursor, else the function the cursor
  // is in, as Y types a variable or the function in IDA.
  const QPointer<CodeView> view = focusedCodeView();
  std::optional<Address> function;
  std::optional<int> parameter;
  QString name;
  QJsonObject variable;
  if (view) {
    function = view->text()->function();
    if (const auto target = view->text()->renameTarget();
        target && target->contains(QStringLiteral("parameter"))) {
      parameter = target->value(QStringLiteral("parameter")).toInt();
      name = target->value(QStringLiteral("name")).toString();
    } else if (target && (target->contains(QStringLiteral("variable_id")) ||
                          target->value(QStringLiteral("object")).toBool() ||
                          target->value(QStringLiteral("kind")).toString() ==
                              QStringLiteral("local"))) {
      if (!target->contains(QStringLiteral("variable_id"))) {
        output_->append(
            tr("The engine cannot identify this source variable unambiguously"),
            1);
        return;
      }
      name = target->value(QStringLiteral("name")).toString();
      variable = {{"address", hexAddress(*function)},
                  {"representation", view->text()->representation()},
                  {"source_options", view->text()->sourceOptions()},
                  {"variable_id", target->value(QStringLiteral("variable_id"))},
                  {"snapshot", target->value(QStringLiteral("snapshot"))}};
    }
  } else {
    function = currentFunction();
  }
  if (!function) {
    output_->append(tr("Put the cursor in a function to set its type"), 1);
    return;
  }
  if (name.isEmpty())
    name = displayAddress(*function);
  const auto epoch = session_.epoch();
  SetTypeDialog dialog(session_, *function, parameter, name, commandWindow(),
                       variable);
  const bool accepted = dialog.exec() == QDialog::Accepted;
  if (view) {
    view->window()->activateWindow();
    view->text()->setFocus();
  }
  if (!accepted || session_.epoch() != epoch)
    return;
  if (!variable.isEmpty()) {
    variable.insert(QStringLiteral("kind"), QStringLiteral("type"));
    variable.insert(QStringLiteral("reset"), dialog.resets());
    if (!dialog.resets())
      variable.insert(QStringLiteral("type"), dialog.declaration());
    session_.editCode(variable, epoch);
    return;
  }
  session_.editFunction(
      *function,
      {{"type", dialog.resets() ? QJsonValue(QJsonValue::Null)
                                : QJsonValue(dialog.declaration())}},
      epoch);
}

void MainWindow::editDataPresentation(const QString &kind) {
  const auto address = currentAddress();
  if (!address || focusedCodeView() || session_.readOnly())
    return;
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  const auto operand = disassembly_->currentOperand();
  QDialog dialog(commandWindow());
  dialog.setObjectName("dataPresentationDialog");
  dialog.setWindowTitle(kind == "array"       ? tr("Make array")
                        : kind == "alignment" ? tr("Make alignment")
                                              : tr("Enum operand"));
  auto *layout = new QVBoxLayout(&dialog);
  layout->addWidget(
      new QLabel(tr("Address: %1").arg(hexAddress(*address)), &dialog));
  auto *type = new QLineEdit(&dialog);
  type->setObjectName("dataPresentationType");
  auto *width = new QSpinBox(&dialog);
  width->setObjectName("arrayElementSize");
  width->setRange(1, ::neverd::limits::gui::kMaxArrayElementBytes);
  width->setValue(::neverd::limits::gui_config::kDefaultArrayElementBytes);
  auto *count = new QSpinBox(&dialog);
  count->setObjectName("arrayCount");
  count->setRange(1, ::neverd::limits::gui::kMaxArrayElements);
  count->setValue(::neverd::limits::gui_config::kDefaultArrayElements);
  auto *alignment = new QComboBox(&dialog);
  alignment->setObjectName("dataAlignment");
  for (int value = 2; value <= ::neverd::limits::gui::kMaxDataAlignment;
       value *= 2)
    alignment->addItem(QString::number(value), value);
  alignment->setCurrentIndex(
      alignment->findData(::neverd::limits::gui_config::kDefaultDataAlignment));
  auto *flags = new QCheckBox(
      tr("Combine bit flags; preserve unknown bits numerically"), &dialog);
  flags->setObjectName("enumFlags");
  type->setVisible(kind != "alignment");
  width->setVisible(kind == "array");
  count->setVisible(kind == "array");
  alignment->setVisible(kind == "alignment");
  flags->setVisible(kind == "enum");
  if (kind == "array") {
    layout->addWidget(new QLabel(tr("Element size in bytes:"), &dialog));
    layout->addWidget(width);
    layout->addWidget(new QLabel(
        tr("Element count (complete initialized span, at most 64 MiB):"),
        &dialog));
    layout->addWidget(count);
    layout->addWidget(new QLabel(tr("Local type name (optional; its layout "
                                    "must match the element size):"),
                                 &dialog));
    layout->addWidget(type);
  } else if (kind == "alignment") {
    layout->addWidget(new QLabel(
        tr("Alignment boundary (the marked span ends on this boundary):"),
        &dialog));
    layout->addWidget(alignment);
  } else {
    layout->addWidget(
        new QLabel(tr("Enumeration declared in Local Types:"), &dialog));
    layout->addWidget(type);
    layout->addWidget(flags);
  }
  auto *buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);
  if (dialog.exec() != QDialog::Accepted || session_.epoch() != epoch ||
      session_.revision() != revision || currentAddress() != address)
    return;
  QJsonObject request{{"address", hexAddress(*address)}, {"action", kind}};
  if (kind == "array") {
    request.insert("element_size", width->value());
    request.insert("count", count->value());
    request.insert("type", type->text().trimmed());
  }
  if (kind == "alignment")
    request.insert("alignment", alignment->currentData().toInt());
  if (kind == "enum") {
    request.insert("type", type->text().trimmed());
    request.insert("flags", flags->isChecked());
    if (operand)
      request.insert("operand", *operand);
  }
  session_.editDataPresentation(
      kind == "enum" ? "operand_format" : "item_define", request, epoch);
}

void MainWindow::editStackFrame(int mode) {
  const auto function = currentFunction();
  const auto address = currentAddress();
  if (!function || session_.readOnly())
    return;
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  const QPointer<QWidget> owner = commandWindow();
  session_.read(
      "stack_frame", {{"address", hexAddress(*function)}}, this,
      [this, function, address, epoch, revision, owner,
       mode](const QJsonObject &reply) {
        if (!owner || session_.epoch() != epoch ||
            session_.revision() != revision || currentFunction() != function)
          return;
        auto policy = reply.value("policy").toObject();
        policy.remove("input_sha256");
        policy.insert("snapshot", reply.value("snapshot"));
        const auto stale = reply.value("stale").toString();
        QDialog dialog(owner);
        dialog.setObjectName("stackFrameDialog");
        dialog.setWindowTitle(mode == 1 ? tr("Change stack pointer")
                                        : tr("Stack variables"));
        dialog.resize(680, 440);
        auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel(
            stale.isEmpty()
                ? tr("Offsets are relative to SP at function entry. "
                     "Corrections apply to source analysis. Reset restores "
                     "automatic analysis. Naming physical stack storage "
                     "pauses LLVM optimizations for that function's C view.")
                : stale +
                      tr(". Reset the obsolete corrections before editing."),
            &dialog);
        label->setWordWrap(true);
        layout->addWidget(label);
        QLineEdit *after = nullptr;
        QComboBox *basis = nullptr;
        QTableWidget *table = nullptr;
        QJsonObject instruction;
        if (mode == 1) {
          for (const auto &value : reply.value("instructions").toArray())
            if (address &&
                parseAddress(value.toObject().value("address").toString()) ==
                    address)
              instruction = value.toObject();
          if (!instruction.value("correctable").toBool()) {
            output_->append(tr("Select a retained non-branch instruction in "
                               "the disassembly"),
                            1);
            return;
          }
          layout->addWidget(new QLabel(
              tr("Instruction: %1; SP before: %2; SP after: %3")
                  .arg(instruction.value("address").toString(),
                       instruction.value("before").isDouble()
                           ? QString::number(
                                 instruction.value("before").toInteger())
                           : tr("unknown"),
                       instruction.value("after").isDouble()
                           ? QString::number(
                                 instruction.value("after").toInteger())
                           : tr("unknown")),
              &dialog));
          basis = new QComboBox(&dialog);
          basis->addItem(
              tr("Absolute offset after instruction, relative to entry SP"));
          if (instruction.value("before").isDouble())
            basis->addItem(tr("Net SP change made by this instruction"));
          layout->addWidget(basis);
          after = new QLineEdit(
              instruction.value("after").isDouble()
                  ? QString::number(instruction.value("after").toInteger())
                  : QString(),
              &dialog);
          after->setObjectName("stackPointerAfter");
          layout->addWidget(after);
        } else {
          table = new QTableWidget(0, 4, &dialog);
          table->setObjectName("stackVariablesTable");
          table->setHorizontalHeaderLabels({tr("Entry offset"), tr("Bytes"),
                                            tr("Name"),
                                            tr("C type (optional)")});
          table->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
          QMap<QString, QJsonObject> rows;
          for (const auto &value : reply.value("accesses").toArray()) {
            const auto row = value.toObject();
            if (row.value("offset").toInteger() >= 0)
              continue;
            if (mode == 2 &&
                (!address ||
                 parseAddress(row.value("address").toString()) != address))
              continue;
            rows.insert(QString::number(row.value("offset").toInteger()) + ":" +
                            QString::number(row.value("bytes").toInt()),
                        row);
          }
          for (const auto &value : policy.value("variables").toArray()) {
            const auto row = value.toObject();
            rows.insert(QString::number(row.value("offset").toInteger()) + ":" +
                            QString::number(row.value("bytes").toInt()),
                        row);
          }
          if (rows.isEmpty()) {
            output_->append(tr("No exact local stack access is available here. "
                               "Repair unknown SP first."),
                            1);
            return;
          }
          for (const auto &row : rows) {
            const int index = table->rowCount();
            table->insertRow(index);
            const QStringList values{
                QString::number(row.value("offset").toInteger()),
                QString::number(row.value("bytes").toInt()),
                row.value("name").toString(), row.value("type").toString()};
            for (int col = 0; col < 4; ++col)
              table->setItem(index, col, new QTableWidgetItem(values[col]));
          }
          layout->addWidget(table);
        }
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                                 QDialogButtonBox::Cancel |
                                                 QDialogButtonBox::Reset,
                                             &dialog);
        bool reset = false;
        buttons->button(QDialogButtonBox::Apply)->setEnabled(stale.isEmpty());
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog,
                &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked,
                &dialog, [&] {
                  reset = true;
                  dialog.accept();
                });
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted || session_.epoch() != epoch ||
            session_.revision() != revision || currentFunction() != function)
          return;
        if (!reset && after) {
          const auto parsed =
              parseSignedDecimalOrHex(after->text().trimmed().toStdString());
          if (!parsed || *parsed < -(1 << 20) || *parsed > (1 << 20)) {
            output_->append(tr("Enter a signed SP offset within one MiB"), 1);
            return;
          }
          qint64 value = *parsed;
          if (basis->currentIndex() == 1)
            value += instruction.value("before").toInteger();
          QJsonArray points;
          for (const auto &p : policy.value("points").toArray())
            if (p.toObject().value("address") != instruction.value("address"))
              points.append(p);
          points.append(QJsonObject{{"address", instruction.value("address")},
                                    {"after", value}});
          policy.insert("points", points);
        }
        if (!reset && table) {
          QJsonArray variables;
          for (int row = 0; row < table->rowCount(); ++row) {
            if (table->item(row, 2)->text().trimmed().isEmpty())
              continue;
            const auto at = parseSignedDecimalOrHex(
                table->item(row, 0)->text().trimmed().toStdString());
            const auto bytes = parseSignedDecimalOrHex(
                table->item(row, 1)->text().trimmed().toStdString());
            if (!at || !bytes) {
              output_->append(tr("Invalid stack slot offset or width"), 1);
              return;
            }
            variables.append(
                QJsonObject{{"offset", qint64(*at)},
                            {"bytes", qint64(*bytes)},
                            {"name", table->item(row, 2)->text().trimmed()},
                            {"type", table->item(row, 3)->text().trimmed()}});
          }
          policy.insert("variables", variables);
        }
        session_.editCode({{"address", hexAddress(*function)},
                           {"kind", "stack_frame"},
                           {"representation", "c"},
                           {"reset", reset},
                           {"stack_frame", policy}},
                          epoch);
      });
}

void MainWindow::editCallType(bool resetAll) {
  const QPointer<CodeView> view = focusedCodeView();
  if (!view || session_.readOnly() || !view->text()->function() ||
      view->text()->loading())
    return;
  const auto epoch = session_.epoch();
  const auto revision = session_.revision();
  const auto function = view->text()->function();
  const auto options = view->text()->sourceOptions();
  const auto addresses = view->text()->currentAddresses();
  QJsonObject request{{"address", hexAddress(*function)},
                      {"representation", view->representation()},
                      {"source_options", options},
                      {"kind", "call_type"}};
  session_.read(
      "call_sites", request, this,
      [this, view, epoch, revision, function, options, addresses, request,
       resetAll](const QJsonObject &reply) mutable {
        if (!view || session_.epoch() != epoch ||
            session_.revision() != revision ||
            view->text()->function() != function ||
            view->text()->sourceOptions() != options)
          return;
        request.insert("snapshot", reply.value("snapshot"));
        if (resetAll) {
          request.insert("action", "clear");
          session_.editCode(request, epoch);
          return;
        }
        QList<QJsonObject> calls;
        QStringList labels;
        for (const auto &value : reply.value("calls").toArray()) {
          const auto call = value.toObject();
          const auto address =
              parseAddress(call.value("instruction").toString());
          if (!address || !addresses.contains(*address))
            continue;
          calls.append(call);
          labels.append(call.value("instruction").toString() + "  →  " +
                        call.value("target").toString(tr("indirect call")) +
                        "  (" +
                        QString::number(call.value("sequence").toInt()) + ")");
        }
        if (calls.isEmpty()) {
          output_->append(tr("Put the cursor on a pseudocode row mapped to an "
                             "original call instruction"),
                          1);
          return;
        }
        int index = 0;
        if (calls.size() > 1) {
          bool accepted = false;
          const auto selected = QInputDialog::getItem(
              view->window(), tr("Select call instruction"),
              tr("This source row contains multiple calls:"), labels, 0, false,
              &accepted);
          if (!accepted)
            return;
          index = labels.indexOf(selected);
        }
        const auto call = calls[index];
        const auto id = call.value("id").toString();
        const auto stated =
            reply.value("types").toObject().value(id).toObject();
        QDialog dialog(view->window());
        dialog.setObjectName("callTypeDialog");
        dialog.setWindowTitle(tr("Set call type"));
        dialog.resize(660, 440);
        auto *layout = new QVBoxLayout(&dialog);
        auto *subject =
            new QLabel(tr("Call at %1. This declaration applies to this call "
                          "only in both C views.")
                           .arg(call.value("instruction").toString()),
                       &dialog);
        subject->setWordWrap(true);
        layout->addWidget(subject);
        layout->addWidget(new QLabel(
            tr("C declaration (include ... for a variadic call):"), &dialog));
        auto *declaration = new QPlainTextEdit(&dialog);
        declaration->setObjectName("callTypeDeclaration");
        declaration->setPlainText(
            stated.value("declaration").toString("int64_t call(void);"));
        layout->addWidget(declaration);
        layout->addWidget(new QLabel(
            tr("Actual variadic argument types for this call, one per line "
               "(use int or double after default promotions):"),
            &dialog));
        auto *actuals = new QPlainTextEdit(&dialog);
        actuals->setObjectName("callTypeActuals");
        QStringList types;
        for (const auto &type : stated.value("variadic_types").toArray())
          types.append(type.toString());
        actuals->setPlainText(types.join('\n'));
        layout->addWidget(actuals);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Apply |
                                                 QDialogButtonBox::Cancel |
                                                 QDialogButtonBox::Reset,
                                             &dialog);
        buttons->button(QDialogButtonBox::Apply)
            ->setObjectName("callTypeApply");
        buttons->button(QDialogButtonBox::Reset)
            ->setObjectName("callTypeReset");
        bool reset = false;
        connect(buttons->button(QDialogButtonBox::Apply), &QPushButton::clicked,
                &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog,
                &QDialog::reject);
        connect(buttons->button(QDialogButtonBox::Reset), &QPushButton::clicked,
                &dialog, [&] {
                  reset = true;
                  dialog.accept();
                });
        layout->addWidget(buttons);
        if (dialog.exec() != QDialog::Accepted || !view ||
            session_.epoch() != epoch || session_.revision() != revision ||
            view->text()->function() != function ||
            view->text()->sourceOptions() != options)
          return;
        request.insert("call_id", id);
        request.insert("reset", reset);
        request.insert("declaration", declaration->toPlainText().trimmed());
        QJsonArray arguments;
        for (const auto &line : actuals->toPlainText().split('\n'))
          if (!line.trimmed().isEmpty())
            arguments.append(line.trimmed());
        request.insert("variadic_types", arguments);
        session_.editCode(request, epoch);
        // Restore focus after the modal dialog and its activation state retire.
        QTimer::singleShot(0, view, [this, view, epoch, function] {
          if (!view || session_.epoch() != epoch ||
              view->text()->function() != function)
            return;
          view->window()->activateWindow();
          view->text()->setFocus();
        });
      });
}

void MainWindow::editVariable(const QString &action) {
  const QPointer<CodeView> view = focusedCodeView();
  if (!view || session_.readOnly() || !view->text()->function())
    return;
  const auto target = view->text()->renameTarget();
  if (action != "clear" && (!target || !target->contains("variable_id")))
    return;
  const auto epoch = session_.epoch();
  const auto identity = view->text()->function();
  const auto options = view->text()->sourceOptions();
  QJsonObject request{{"address", hexAddress(*identity)},
                      {"representation", view->representation()},
                      {"source_options", options},
                      {"kind", "variables"},
                      {"action", action}};
  if (target) {
    request.insert("variable_id", target->value("variable_id"));
    if (action != "clear")
      request.insert("snapshot", target->value("snapshot"));
  }
  session_.read(
      "source_variables", request, this,
      [this, view, epoch, identity, options, request,
       action](const QJsonObject &reply) mutable {
        if (!view || session_.epoch() != epoch ||
            view->text()->function() != identity ||
            view->text()->sourceOptions() != options)
          return;
        if (request.contains("snapshot") &&
            request.value("snapshot") != reply.value("snapshot")) {
          output_->append(
              tr("The source analysis changed; select the variable again"), 1);
          return;
        }
        request.insert("snapshot", reply.value("snapshot"));
        if (action == "map") {
          QStringList labels;
          QList<QString> ids;
          for (const auto &entry : reply.value("variables").toArray()) {
            const auto v = entry.toObject();
            if (v.value("ambiguous").toBool(true) || v.contains("address") ||
                v.value("id") == request.value("variable_id"))
              continue;
            labels.append(v.value("name").toString() + "  (" +
                          v.value("type").toString() + ")  [" +
                          v.value("id").toString() + "]");
            ids.append(v.value("id").toString());
          }
          if (labels.isEmpty()) {
            output_->append(
                tr("No other unambiguous local variable is available"), 1);
            return;
          }
          bool accepted = false;
          const auto selected = QInputDialog::getItem(
              view->window(), tr("Map variable"),
              tr("Destination variable (the engine checks copy relationships "
                 "and live ranges):"),
              labels, 0, false, &accepted);
          if (!accepted)
            return;
          request.insert("to", ids.at(labels.indexOf(selected)));
        } else if (action == "unmap" || action == "unsplit") {
          const auto edits = reply.value("associations").toArray();
          const auto results = reply.value("results").toArray();
          const auto kind = action == "unmap" ? "map" : "split";
          int found = -1;
          for (int i = 0; i < edits.size() && i < results.size(); ++i)
            if (edits[i].toObject().value("kind") == kind &&
                results[i].toObject().value("outputs").toArray().contains(
                    request.value("variable_id")))
              found = i;
          if (found < 0) {
            output_->append(
                tr("This variable has no matching association. Use Reset "
                   "variable associations to remove stale edits."),
                1);
            return;
          }
          request.insert("association", found);
        }
        if (session_.epoch() != epoch || !view ||
            view->text()->function() != identity ||
            view->text()->sourceOptions() != options)
          return;
        session_.editCode(request, epoch);
        view->text()->setFocus();
      });
}

void MainWindow::convertToStruct() {
  CodeView *code = focusedCodeView();
  if (!code || session_.readOnly() || !session_.convertsParameters())
    return;
  const QPointer<CodeView> view = code;
  const auto function = code->text()->function();
  const auto target = code->text()->renameTarget();
  if (!function || !target || !target->contains(QStringLiteral("parameter"))) {
    output_->append(
        tr("Put the cursor on a parameter of the function to convert it"), 1);
    return;
  }
  const int parameter = target->value(QStringLiteral("parameter")).toInt();
  const QString name = target->value(QStringLiteral("name")).toString();
  const auto epoch = session_.epoch();
  TypePickerDialog dialog(
      session_, {{"function", hexAddress(*function)}, {"parameter", parameter}},
      name, view->window());
  const bool accepted = dialog.exec() == QDialog::Accepted;
  if (view) {
    view->window()->activateWindow();
    view->text()->setFocus();
  }
  if (!accepted || session_.epoch() != epoch)
    return;
  // The type names the parameter too when the user gave it another name.
  QString type = dialog.typeName() + QStringLiteral(" *");
  if (dialog.parameterName() != name)
    type += dialog.parameterName();
  session_.read(
      QStringLiteral("function_type"),
      {{"address", hexAddress(*function)},
       {"parameter", parameter},
       {"type", type}},
      this,
      [this, function = *function, epoch](const QJsonObject &reply) {
        if (session_.epoch() == epoch)
          session_.editFunction(
              function, {{"type", reply.value(QStringLiteral("declaration"))}},
              epoch);
      },
      [this](const QString &, const QString &message) {
        output_->append(tr("Cannot convert the parameter: %1").arg(message), 2);
      });
}

void MainWindow::formatCodeNumber(CodeView *code,
                                  std::optional<LiteralRadix> radix) {
  if (session_.readOnly())
    return;
  const auto function = code->text()->function();
  const auto target = code->text()->numberTarget();
  if (!function || !target) {
    output_->append(tr("Put the cursor on a number of the pseudocode to "
                       "change its base"),
                    1);
    return;
  }
  const auto base =
      radix.value_or(target->shown == LiteralRadix::Hex ? LiteralRadix::Decimal
                                                        : LiteralRadix::Hex);
  // A base whose spelling would change the literal's C type, or a character
  // constant of something else than ASCII characters, says so instead.
  if (base != LiteralRadix::Number &&
      !spellIntegerLiteral(target->literal, base, target->syntax)) {
    output_->append(tr("%1 has no spelling in that base with the same type")
                        .arg(QString::fromStdString(target->literal.Text)),
                    1);
    return;
  }
  const auto name = literalRadixName(base);
  session_.editCode(
      {{"address", hexAddress(*function)},
       {"representation", code->representation()},
       {"source_options", code->text()->sourceOptions()},
       {"kind", "number"},
       {"line", target->line},
       {"anchor", target->anchor},
       {"index", target->index},
       {"radix", QString::fromLatin1(name.data(), qsizetype(name.size()))}},
      session_.epoch());
}

void MainWindow::addNumberActions(QMenu &menu, CodeView *code) {
  const auto target = code->text()->numberTarget();
  if (!target || session_.readOnly())
    return;
  // Each base shows what the number would read in it, the decompiler's own
  // choice first; the one the user gave it is checked.
  auto *group = new QActionGroup(&menu);
  const QPointer<CodeView> view = code;
  const auto add = [&](LiteralRadix radix, const QIcon &image,
                       const QString &title) {
    const auto spelled =
        radix == LiteralRadix::Number
            ? std::optional(target->literal.Text)
            : spellIntegerLiteral(target->literal, radix, target->syntax);
    auto *action =
        menu.addAction(image, spelled ? title + QLatin1Char('\t') +
                                            QString::fromStdString(*spelled)
                                      : title);
    action->setObjectName(QStringLiteral("codeNumber_") +
                          QString::fromLatin1(literalRadixName(radix).data()));
    action->setCheckable(true);
    action->setChecked(target->chosen == radix);
    action->setEnabled(spelled.has_value());
    group->addAction(action);
    connect(action, &QAction::triggered, this, [this, view, radix] {
      if (view)
        formatCodeNumber(view, radix);
    });
  };
  const auto like = [this](ActionId id) {
    auto *action = actions_.action(id);
    return std::pair{action->icon(), action->text()};
  };
  for (const auto &[radix, id] :
       {std::pair{LiteralRadix::Number, ActionId::EditOperandNumber},
        std::pair{LiteralRadix::Hex, ActionId::EditOperandHex},
        std::pair{LiteralRadix::Decimal, ActionId::EditOperandDecimal}}) {
    const auto [image, title] = like(id);
    add(radix, image, title);
  }
  add(LiteralRadix::Octal, icon(QStringLiteral("operand_octal")), tr("Octa&l"));
  for (const auto &[radix, id] :
       {std::pair{LiteralRadix::Binary, ActionId::EditOperandBinary},
        std::pair{LiteralRadix::Char, ActionId::EditOperandChar}}) {
    const auto [image, title] = like(id);
    add(radix, image, title);
  }
  menu.addSeparator();
}

void MainWindow::editFunction(Address entry) {
  // The dialog shows what the worker reports now, and its edits go to the
  // session they were made for.
  const auto epoch = session_.epoch();
  const QPointer<QWidget> owner = commandWindow();
  session_.read(
      QStringLiteral("function_attributes"), {{"address", hexAddress(entry)}},
      this, [this, entry, epoch, owner](const QJsonObject &attributes) {
        if (!owner || session_.epoch() != epoch)
          return;
        const int digits = session_.bitness() == 64 ? 16 : 8;
        const auto shown = [&](const QJsonValue &value) {
          const auto address = addressValue(value);
          if (!address)
            return QString();
          const auto *region = space_.regionOf(*address);
          return (region ? region->name + QLatin1Char(':') : QString()) +
                 displayAddress(*address, digits);
        };
        FunctionEditDialog dialog(attributes, shown(attributes.value("start")),
                                  shown(attributes.value("end")), owner);
        if (dialog.exec() != QDialog::Accepted || session_.epoch() != epoch)
          return;
        // A new name is a rename, and the attributes an edit of their own:
        // each is a step of the history.  The edit follows the rename, so a
        // name the worker refuses applies neither.
        const auto edit = [this, entry, epoch,
                           changed = dialog.changedAttributes()] {
          session_.editFunction(entry, changed, epoch);
        };
        if (const auto name = dialog.name();
            !name.isEmpty() && name != attributes.value("name").toString())
          session_.rename(entry, name, epoch, edit);
        else
          edit();
      });
}

//===----------------------------------------------------------------------===//
// Desktop
//===----------------------------------------------------------------------===//

void MainWindow::toggleFocusMode() {
  if (!focusLayout_.isEmpty()) {
    leaveFocusMode();
    return;
  }
  auto *action = actions_.action(ActionId::ViewFocusMode);
  if (!session_.loaded()) {
    action->setChecked(false);
    return;
  }
  Dock *assembly = nullptr;
  for (auto *dock : std::as_const(docks_))
    if (dock->widget() == disassembly_)
      assembly = dock;
  if (!assembly) {
    action->setChecked(false);
    return;
  }
  // Include the on-demand code pane in the snapshot, even when it is closed.
  auto *code = codeView(CodeView::pseudocodeRepresentation());
  auto *codeDock = docks_.value(PseudocodeDock);
  focusLayout_ = KDDockWidgets::LayoutSaver().serializeLayout();
  focusReturn_ = QApplication::focusWidget();
  if (!focusReturn_)
    focusReturn_ = disassembly_;
  action->setChecked(true);

  assembly->open();
  assembly->setFloating(true);
  auto *floating = assembly->QWidget::window();
  if (!floating->isMaximized())
    floating->resize(
        size().boundedTo(floating->screen()->availableGeometry().size()));
  assembly->dockWidget()->addDockWidgetToContainingWindow(
      codeDock->dockWidget(), KDDockWidgets::Location_OnRight,
      assembly->dockWidget());
  auto *output = docks_.value(OutputDock);
  const int outputHeight = std::clamp(int(floating->height() * OutputShare),
                                      MinimumOutputHeight, MaximumOutputHeight);
  assembly->dockWidget()->addDockWidgetToContainingWindow(
      output->dockWidget(), KDDockWidgets::Location_OnBottom, nullptr,
      InitialOption(QSize(0, outputHeight)));
  const int difference = assembly->dockWidget()->sizeInLayout().width() -
                         codeDock->dockWidget()->sizeInLayout().width();
  codeDock->dockWidget()->resizeInLayout(difference / 2, 0, 0, 0);
  assembly->setAsCurrentTab();
  codeDock->setAsCurrentTab();
  output->setAsCurrentTab();
  showPseudocode(code->representation());
  floating->showMaximized();
  focusToolbar_ = actions_.buildFocusToolbar(floating);
  if (!addFloatingWindowToolbar(*floating, *focusToolbar_))
    delete focusToolbar_.data();
  floating->raise();
  floating->activateWindow();
  disassembly_->focusContent();
  auto *workspace =
      qobject_cast<KDDockWidgets::QtWidgets::FloatingWindow *>(floating);
  auto *area = KDDockWidgets::QtCommon::View_qt::asQWidget(
      workspace->floatingWindow()->dropArea()->view());
  auto *functions = new ChooserView(session_, space_, ChooserKind::Functions);
  focusFunctions_ = functions;
  functions->table()->setObjectName(QStringLiteral("focusFunctionsList"));
  functions->setFilterText({});
  connectFunctions(functions);
  functions->model().reload();
  if (const auto function = disassembly_->currentFunction())
    functions->revealAddress(*function);
  focusSidebar_ =
      new FocusSidebar(*floating, *area, *functions, tr("Functions"));
  auto *header = functions->table()->header();
  connect(focusSidebar_, &FocusSidebar::widthChanged, functions,
          [header, width = focusSidebar_->width()](int newWidth) mutable {
            const int difference = newWidth - std::exchange(width, newWidth);
            header->resizeSection(
                0, std::max(header->minimumSectionSize(),
                            header->sectionSize(0) + difference));
          });
  updateActions();
}

void MainWindow::resetFocusSequence() {
  focusSequenceTimer_.stop();
  focusSequenceCount_ = 0;
  focusSequenceView_.clear();
}

void MainWindow::leaveFocusMode() {
  resetFocusSequence();
  if (focusLayout_.isEmpty())
    return;
  const auto layout = std::exchange(focusLayout_, {});
  const auto focus = std::exchange(focusReturn_, {});
  focusFunctions_.clear();
  delete focusSidebar_.data();
  if (auto *toolbar = focusToolbar_.data()) {
    focusToolbar_.clear();
    toolbar->hide();
    // A toolbar command can exit focus mode while its button is dispatching.
    // Keep it alive until that event returns, even if the floating pane closes.
    toolbar->setParent(this);
    toolbar->deleteLater();
  }
  actions_.action(ActionId::ViewFocusMode)->setChecked(false);
  if (!KDDockWidgets::LayoutSaver(kPaneRestoreOptions).restoreLayout(layout))
    output_->append(tr("Could not restore the layout before focus mode."), 1);
  if (focus && focus->isVisible()) {
    focus->window()->raise();
    focus->window()->activateWindow();
    focus->setFocus();
  }
}

void MainWindow::saveDesktop() {
  KDDockWidgets::LayoutSaver().saveToFile(layoutPath());
  output_->append(tr("Desktop saved"), 0);
}

void MainWindow::restoreDesktop() {
  leaveFocusMode();
  if (!KDDockWidgets::LayoutSaver().restoreFromFile(layoutPath()))
    output_->append(tr("No saved desktop"), 1);
  closeTransientDocks(false);
}

void MainWindow::resetDesktop() {
  leaveFocusMode();
  for (auto *dock : std::as_const(docks_))
    dock->forceClose();
  applyDefaultLayout();
}

void MainWindow::closeTransientDocks(bool discardContents) {
  if (discardContents)
    leaveFocusMode();
  for (auto *dock : std::as_const(codeDocks_)) {
    auto *text = static_cast<CodeView *>(dock->widget())->text();
    if (discardContents || !text->function()) {
      dock->forceClose();
      text->clear();
    }
  }
  for (const auto kind :
       {ChooserKind::SearchResults, ChooserKind::CrossReferences})
    if (auto *view = choosers_.value(int(kind))) {
      auto &model = view->model();
      if (discardContents || (!model.rowCount() && !model.loading())) {
        docks_.value(chooserDockId(kind))->forceClose();
        if (discardContents) {
          model.setRequest({});
          if (kind == ChooserKind::SearchResults)
            model.setLocalRows({});
        }
      }
    }
  if (discardContents || !disassembly_->graphMode())
    docks_.value(OverviewDock)->forceClose();
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
  if (quickStart_) {
    quickStart_->setWindowState(quickStart_->windowState() &
                                ~Qt::WindowMinimized);
    quickStart_->raise();
    quickStart_->activateWindow();
    return;
  }
  // Before the workbench shows, the quick start greets alone, as IDA's
  // does: a window of its own until the user chooses how to start, when the
  // workbench takes its place.  Closing it then quits.
  const bool greeting = !isVisible();
  auto *dialog = new QuickStartDialog(
      [this](QWidget *parent) { return askForFile(parent); },
      greeting ? nullptr : this);
  // Qt would quit when the greeting closes as the last window, even once
  // the user chose how to start; closing it quits below instead.
  dialog->setAttribute(Qt::WA_QuitOnClose, false);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  quickStart_ = dialog;
  connect(dialog, &QDialog::finished, this, [this, dialog, greeting] {
    quickStart_ = nullptr;
    const auto start = dialog->start();
    const auto file = dialog->file();
    if (start == QuickStartDialog::Start::None) {
      if (greeting)
        // Closing can originate inside Cocoa's termination callback.
        QMetaObject::invokeMethod(qApp, &QCoreApplication::quit,
                                  Qt::QueuedConnection);
      return;
    }
    // A chosen file shows the workbench as it starts to load, after the
    // questions asked before the load greet alone too.
    if (start == QuickStartDialog::Start::New)
      chooseLoader(file);
    else if (start == QuickStartDialog::Start::Previous)
      openFile(file);
    else
      showWorkbench();
  });
  // Quick Start is a restorable application window. An application-modal
  // Cocoa session keeps blocking Dock reopen and Quit after minimization.
  dialog->show();
}

void MainWindow::showWorkbench() {
  if (isVisible())
    return;
  initializeLayout();
  gnome::boundMaximumSize(*this);
  show();
  gnome::widenResizeEdges(*this);
}

QHash<QString, QByteArray> MainWindow::projectState() const {
  QHash<QString, QByteArray> state;
  QJsonObject location;
  if (const auto address = currentAddress())
    location.insert(QStringLiteral("address"), hexAddress(*address));
  if (const auto function = currentFunction())
    location.insert(QStringLiteral("function"), hexAddress(*function));
  location.insert(QStringLiteral("graph"), disassembly_->graphMode());
  state.insert(QString::fromLatin1(ProjectDatabase::LocationState),
               QJsonDocument(location).toJson(QJsonDocument::Compact));
  state.insert(QString::fromLatin1(ProjectDatabase::BookmarksState),
               QJsonDocument(bookmarks_).toJson(QJsonDocument::Compact));
  state.insert(QString::fromLatin1(ProjectDatabase::DesktopState),
               focusLayout_.isEmpty()
                   ? KDDockWidgets::LayoutSaver().serializeLayout()
                   : focusLayout_);
  state.insert(
      QString::fromLatin1(ProjectDatabase::PathfinderState),
      QJsonDocument(pathfinder_ ? pathfinder_->state() : pathfinderState_)
          .toJson(QJsonDocument::Compact));
  return state;
}

void MainWindow::restoreProjectState() {
  const auto &state = session_.databaseState();
  const auto location =
      QJsonDocument::fromJson(
          state.value(QString::fromLatin1(ProjectDatabase::LocationState)))
          .object();
  // The project's bookmarks, else those this machine's settings kept for it.
  QJsonArray rows =
      QJsonDocument::fromJson(
          state.value(QString::fromLatin1(ProjectDatabase::BookmarksState)))
          .array();
  if (rows.isEmpty())
    for (const auto &row : QSettings().value(bookmarksKey()).toList())
      rows.append(QJsonObject::fromVariantMap(row.toMap()));
  showBookmarks(rows);
  // The project's call path, for the Pathfinder its desktop shows or one
  // opened later.
  pathfinderState_ =
      QJsonDocument::fromJson(
          state.value(QString::fromLatin1(ProjectDatabase::PathfinderState)))
          .object();
  if (pathfinder_)
    pathfinder_->restoreState(pathfinderState_);
  if (const auto desktop =
          state.value(QString::fromLatin1(ProjectDatabase::DesktopState));
      !desktop.isEmpty() &&
      !KDDockWidgets::LayoutSaver(kPaneRestoreOptions).restoreLayout(desktop))
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

void MainWindow::showBookmarks(const QJsonArray &rows) {
  bookmarks_ = rows;
  if (auto *chooser = choosers_.value(int(ChooserKind::Bookmarks)))
    chooser->model().setLocalRows(rows);
}

void MainWindow::setBookmarks(const QJsonArray &rows) {
  showBookmarks(rows);
  // A read-only project is another workbench's to write; its bookmarks stay
  // in this machine's settings.
  if (session_.readOnly()) {
    QVariantList list;
    for (const auto &row : rows)
      list.append(row.toObject().toVariantMap());
    QSettings().setValue(bookmarksKey(), list);
    return;
  }
  session_.saveBookmarks(rows);
}

void MainWindow::packDatabase() {
  if (!session_.loaded())
    return;
  const auto path = QFileDialog::getSaveFileName(
      commandWindow(), tr("Pack database"), session_.databasePath(),
      tr("NeverD databases (*%1)")
          .arg(QLatin1String(ProjectDatabase::Extension)));
  if (path.isEmpty())
    return;
  session_.packDatabase(path.endsWith(QLatin1String(ProjectDatabase::Extension),
                                      Qt::CaseInsensitive)
                            ? path
                            : path + QLatin1String(ProjectDatabase::Extension));
}

QString MainWindow::bookmarksKey() const {
  return QStringLiteral("bookmarks/") +
         QString::fromLatin1(
             QCryptographicHash::hash(session_.projectPath().toUtf8(),
                                      QCryptographicHash::Sha1)
                 .toHex());
}

void MainWindow::showConnections() {
  auto *dialog = new ConnectionsDialog(session_, mcp_, broker_, commandView());
  showModelessDialog(dialog, dialog->parentWidget());
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
  if (packageReview_ && !packageReview_->close()) {
    event->ignore();
    return;
  }
  if (!quitting_ && !session_.requestQuit()) {
    event->ignore();
    return;
  }
  leaveFocusMode();
  // The project remembers where the user was, like the desktop does.
  if (session_.loaded())
    session_.saveDatabaseState();
  KDDockWidgets::LayoutSaver().saveToFile(layoutPath());
  event->accept();
  KDDockWidgets::QtWidgets::MainWindow::closeEvent(event);
  if (!event->isAccepted())
    return;

  // Floating panes with minimize controls are independent top-level windows.
  // Save their layout first, then close every pane and its auxiliary windows
  // while the session and borrowed view state still exist.
  QSet<QWidget *> owners{this};
  QList<QPointer<Dock>> panes;
  for (auto *dock : std::as_const(docks_)) {
    owners.insert(dock->QWidget::window());
    panes.append(dock);
  }
  QList<QPointer<QWidget>> windows;
  for (auto *window : QApplication::topLevelWidgets()) {
    if (window == this)
      continue;
    for (auto *owner = window; owner; owner = owner->parentWidget())
      if (owners.contains(owner)) {
        windows.append(window);
        break;
      }
  }
  for (const auto &dock : panes)
    if (dock)
      dock->dockWidget()->forceClose();
  for (const auto &window : windows)
    if (window)
      window->close();
  emit shutdownAccepted();
}

} // namespace neverd::gui
