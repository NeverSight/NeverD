#pragma once

#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "ChooserView.h"
#include "DeepLink.h"
#include "InstanceServer.h"
#include "Session.h"

#include "neverd/support/IntegerLiterals.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <functional>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <optional>
#include <utility>

class QDialog;
class QLabel;
class QMenuBar;
class QToolBar;
class McpConnectionManager;
class GuiSessionBroker;

namespace neverd::gui {

namespace web {
class PackageReviewDialog;
}

class ComparisonDialog;
class LineageDialog;
class CodeView;
class DisassemblyView;
class ExtensionsView;
class FindInViewDialog;
class FocusSidebar;
class LocalTypesView;
class PathfinderView;
class TypeLibrariesView;
class GraphOverview;
class HexView;
class NavigationBand;
class MicrosoftSymbols;
class OutputWindow;
class Session;
class TitleBar;
class WindowFrame;

/// The workbench window: the classic disassembler layout (functions on the
/// left, disassembly with hex, imports and exports in the center, output at
/// the bottom, navigation band above), its menus, toolbars and status bar.
class MainWindow final : public KDDockWidgets::QtWidgets::MainWindow {
  Q_OBJECT
public:
  MainWindow(Session &session, McpConnectionManager &mcp,
             GuiSessionBroker &broker);
  ~MainWindow() override;

  void openFile(const QString &path);
  /// Whether opening a file asks to load its debug information: the PDB a
  /// PE names, before the load or once it lacks one, or the DWARF of an ELF
  /// or Mach-O image.  Automated runs, which nobody answers, load what
  /// discovery finds.
  void setAsksForDebugInfo(bool asks);
  /// Use a disposable layout file (tests and benchmarks).
  void setLayoutPath(const QString &path) { layoutPath_ = path; }
  /// Restore the saved desktop, or lay out the default one; later calls do
  /// nothing.  Call once the window has its size so preferred dock sizes
  /// apply.
  void initializeLayout();
  /// Show the workbench, laying out its desktop first if nothing did yet.
  void showWorkbench();
  /// Before the workbench shows, the quick start greets alone and the
  /// workbench follows the user's choice; closing it quits. Returns immediately
  /// so minimizing never leaves an application-modal session running.
  void showQuickStart();
  /// Greet an empty startup on the next event-loop turn, unless opening a
  /// file takes precedence. No window-exposure or engine-ready event is needed.
  void scheduleQuickStart();
  /// Command lines to run in the output window once the next file opens.
  void runAfterOpen(const QStringList &commands) {
    pendingCommands_ = commands;
  }
  /// The link to the place the focused view shows (Copy link): the
  /// function or line of a code window, a list's row, a local type, or the
  /// disassembly's or hex view's address.  None without a program or a
  /// place.
  std::optional<DeepLink> linkHere() const;
  /// Shows the place \p link names in the view it asks for, as IDA's
  /// open_ida_link does, then calls \p done with nothing, or the reason it
  /// could not, which the output window shows too: another build of the
  /// input, no such function, type or bookmark, or no such address.
  void openLink(const DeepLink &link,
                std::function<void(const QString &error)> done = {});
  /// Report a link failure from a native URL event or local navigation.
  void reportLinkError(const QString &error);
  /// The link to open once the next file opens, as the command line asks.
  void openLinkAfterOpen(const DeepLink &link) { pendingLink_ = {link, {}}; }
  /// What other programs reach this window by.
  InstanceServer &instanceServer() { return instances_; }
  /// The active disassembly view; Alt+Enter can open independent views.
  DisassemblyView *disassembly() const { return disassembly_; }
  const AddressSpace &addressSpace() const { return space_; }
  ActionRegistry &actions() { return actions_; }
  /// The window's menus, which sit in its title bar when it draws its own
  /// frame.  This hides QMainWindow::menuBar(), which would replace that
  /// title bar with a new menu bar.
  QMenuBar *menuBar() const { return menuBar_; }

  /// The window that creates on-demand docks while a layout is restored.
  static MainWindow *instance() { return instance_; }

signals:
  /// The disassembly painted its first content after a file opened.
  void firstContentPainted();
  /// The workbench accepted exit after save/worker checks and closed its panes.
  void shutdownAccepted();

protected:
  void closeEvent(QCloseEvent *event) override;
  void changeEvent(QEvent *event) override;
  void showEvent(QShowEvent *event) override;
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  using Dock = KDDockWidgets::QtWidgets::DockWidget;
  static KDDockWidgets::Core::DockWidget *createDock(const QString &name);
  Dock *dockNamed(const QString &name);
  void retranslateUi();
  void
  openDock(Dock *dock, bool tabbed,
           KDDockWidgets::Location location = KDDockWidgets::Location_OnBottom);
  void showConnections();

  void buildMenusAndToolbars();
  void buildStatusBar();
  void buildDocks();
  void connectDisassembly(DisassemblyView *view);
  Dock *disassemblyDock(const QString &id);
  void jumpNewWindow(Address target);
  void applyDefaultLayout();
  /// Give the default side and bottom windows their classic proportions.
  void applyDefaultSizes();
  void showOverview();
  void connectSession();
  void connectActions();
  /// \p title is an untranslated MainWindow string, or null when the
  /// caller titles the dock itself.
  Dock *makeDock(const QString &id, const char *title, const QString &icon,
                 QWidget *content);
  Dock *chooserDock(ChooserKind kind);
  CodeView *codeView(const QString &representation, bool newWindow = false);
  Dock *codeDock(const QString &id, const QString &representation);
  /// Open code beside the active disassembly, including in a floating window.
  void openCodeDock(Dock *dock);
  void toggleFocusMode();
  void leaveFocusMode();
  void resetFocusSequence();
  void connectFunctions(ChooserView *view);
  void connectPatchActions();
  void preparePatchMenu(QMenu *menu);
  void patchAction(const QString &kind);
  struct PatchOrigin {
    QPointer<QWidget> view, surface;
    QRect anchor;
  };
  PatchOrigin patchOrigin() const;
  void showPatchEditor(Address address, const QString &kind,
                       const QString &initial, const PatchOrigin &origin);
  void exportBytePatches();
  void rewriteFunction();
  QJsonObject patchRequest(const QString &kind) const;
  void connectPatchedBytes(ChooserView *view);
  /// Title the pseudocode and IR windows by what they show, lettered apart.
  void retitleCodeDocks();

  // Location and synchronization.
  std::optional<Address> currentAddress() const;
  /// The list window receiving commands, including its open context menu.
  ChooserView *focusedChooser() const;
  /// The code window receiving commands, including its open context menu.
  CodeView *focusedCodeView() const;
  std::optional<Address> currentFunction() const;
  /// The function a command on a whole function acts on: the one a focused
  /// code window shows, which can differ from the disassembly's, else the
  /// disassembly's.
  std::optional<Address> commandFunction() const;
  /// What Shift+F9 and Ctrl+Shift+F9 put on a call path: the function a
  /// focused code window shows, a list window's row, else the disassembly's
  /// location, which the Pathfinder takes as its function or import.
  std::optional<Address> pathfinderTarget() const;
  /// The image base links count relative addresses from, and the input's
  /// SHA-256 they name it by.
  std::optional<Address> imageBase() const;
  QString inputSha256() const;
  /// What get_info tells another program of this window.
  QJsonObject instanceInfo() const;
  void copyLink();
  void jumpToLink();
  /// Shows \p address in \p view, a link's.
  void showLinkedAddress(Address address, LinkView view);
  /// Open the Pathfinder below the views, raised.
  PathfinderView *showPathfinder();
  /// Open a function in the code window used before the chooser took focus.
  void activateFunction(Address address);
  void navigate(Address address);
  /// Jump in the active address view: the hex view when it was the last
  /// analysis view used, otherwise the disassembly.
  void jump(Address address);
  void navigateExpression(const QString &text);
  /// Follow a code name in \p view, optionally selecting its instruction.
  void activateCodeName(CodeView *view, const QString &name,
                        bool selectSourceAddress = false);
  void navigateCodeFunction(CodeView *view, Address address, Address function,
                            std::optional<Address> from);
  /// Go \p steps stops back or forward, the code window in focus along.
  void navigateHistory(bool forward, std::size_t steps = 1);
  /// Give the toolbar's Back and Forward buttons the lists of their stops,
  /// as IDA's do.
  void addHistoryLists(QToolBar &toolbar);
  /// List the stops Back, or Forward, reaches, nearest first, each as the
  /// listing names its place, or as its line in the code in focus, and what
  /// is there.
  void showHistoryList(QMenu &menu, bool forward);
  void synchronize(Address address, QObject *source);
  /// The pseudocode and IR windows that are showing.
  QVector<CodeView *> visibleCodeViews() const;
  void updateActions();
  void updateStatusBar();
  void updateTitle();

  // Commands.
  void openDialog();
  void addAnalysisSection();
  /// The binary or database to open, asked in a dialog over \p parent;
  /// empty when the user cancels.
  QString askForFile(QWidget *parent) const;
  /// Open \p path, first asking how to load it when NeverD keeps no project
  /// for it, as IDA's "Load a new file" dialog does.
  /// Open \p path once the user answered whether to load its symbol table
  /// and its debug information, as IDA asks when it loads a PE that names a
  /// PDB or an image with DWARF.
  void openAskingForDebugInfo(const QString &path, LoadOptions options);
  /// Ask whether to load the image's static symbol table, if it has one,
  /// then about its debug information.
  void openAskingForSymbols(const QString &path, LoadOptions options);
  /// Open \p path once the user answered whether to load the PDB a PE
  /// names or the DWARF of an ELF or Mach-O image, and then, without either,
  /// the linker MAP beside it.
  void openAskingForDebugFiles(const QString &path, LoadOptions options);
  /// Open \p path once the user answered whether to load the DWARF of the
  /// ELF or Mach-O image, if it has any, and without it the MAP.
  void openAskingForDwarf(const QString &path, LoadOptions options);
  /// Open \p path once the user answered whether to load the linker MAP
  /// beside it, if it has one: a PDB's or DWARF's names outrank a MAP's, so
  /// it is asked about only for an image that loads without them.
  void openAskingForMap(const QString &path, LoadOptions options);
  /// Show the workbench and start loading \p path.
  void openNow(const QString &path, const LoadOptions &options);
  void chooseLoader(const QString &path);
  /// File, Load file, Reload the input file: read the file again, asking
  /// again how to read a binary file.
  void reloadInput();
  /// Show or hide the status line's analysis indicator, as the load dialog
  /// last chose.
  void applyIndicator();
  void rename();
  /// IDA's Edit function dialog for the function at \p entry.
  void editFunction(Address entry);
  /// T: the operand under the cursor as a member of the local type the
  /// user picks among those the code's accesses fit (TypePickerDialog).
  void chooseStructOffset();
  void editDataPresentation(const QString &kind);
  /// IDA's Y: the type of the parameter under a pseudocode cursor, else the
  /// declaration of the current function.
  void setType();
  void editVariable(const QString &action);
  void editCallType(bool resetAll = false);
  void editStackFrame(int mode);
  /// Hex-Rays' Convert to struct *: the parameter under the pseudocode
  /// cursor made a pointer to the local type its accesses fit.
  void convertToStruct();
  /// Show the number under \p code's cursor in \p radix; without one, H's
  /// toggle between hexadecimal and decimal, as in Hex-Rays.
  void formatCodeNumber(CodeView *code, std::optional<LiteralRadix> radix);
  /// The bases of the number under \p code's cursor, each with what it
  /// would read, at the top of its context menu.
  void addNumberActions(QMenu &menu, CodeView *code);
  /// The Functions window's context menu for the row at \p position.
  void functionsMenu(ChooserView *view, const QPoint &position);
  void comment();
  void jumpAnywhere();
  void showCrossReferences(std::optional<Address> address, bool to);
  void showLocalReferences(CodeView *origin, const QJsonObject &target);
  void showReferencesDialog(std::optional<Address> address, bool to,
                            CodeView *origin);
  enum class CodeOpen { Reuse, Generate, Switch };
  void showPseudocode(const QString &representation,
                      CodeOpen mode = CodeOpen::Reuse);
  void toggleGraph();
  void searchBinary(const QString &kind, bool again);
  void queryRegister(const QString &kind);
  void searchHighlight(bool forward);
  /// Lock highlight: color \p slot, or the first free color.
  void lockHighlight(std::optional<int> slot);
  void stepFunction(bool forward);
  void showCalculator();
  void showAbout();
  void showOptions();
  /// Options -> String literals: the engine's encodings and the minimum
  /// length, applied to the listing and the Strings window.
  void showStringOptions();
  void stringOptionsDialog(const QJsonArray &encodings,
                           const QJsonObject &current, QWidget *owner);
  void chooseFont();
  void chooseTheme();
  void showShortcuts();
  /// Choose a jump target without changing the corresponding dock's state.
  void chooseLocation(ChooserKind kind, ActionId command);
  QWidget *commandFocusWidget() const;
  QWidget *commandWindow() const;
  QWidget *commandView() const;
  /// Edit > Copy: the selection of the view holding the focus.
  void copySelection();
  void showCommandPalette();
  void exportCurrent(bool pseudocode);
  void exportData();
  void loadPdb();
  /// File, Load file, DWARF file: the DWARF of the open ELF or Mach-O image
  /// from a separate file the user picks.
  void loadDwarf();
  /// File, Load file, MAP file: names from a linker MAP the user picks,
  /// confirmed first when it describes another build.
  void loadMap();
  /// Opens the Local Types editor with a header file the user picks, as
  /// IDA parses a C header into its local types (Ctrl+F9).
  void loadHeader();
  /// Load the MAP \p facts describe, as map_inspect read it, once the user
  /// confirmed a MAP of another build; nothing when the session changed.
  void confirmMap(const QJsonObject &facts, quint64 epoch, QWidget *owner);
  void runCommand(const QString &command, const QString &argument);
  void saveDesktop();
  void restoreDesktop();
  void resetDesktop();
  void closeTransientDocks(bool discardContents = true);
  void cycleWindows(bool forward);
  void fillPlaceholder(QMenu *menu, const QString &name);
  void contextMenu(const QPoint &globalPosition);
  QString layoutPath() const;
  // Project state packed into the database.
  QHash<QString, QByteArray> projectState() const;
  void restoreProjectState();
  void navigateInitialAddress();
  /// Opens the link the command line gave, once the program's addresses are
  /// known, in place of the project's last place.
  void openPendingLink();
  const QJsonArray &bookmarks() const { return bookmarks_; }
  /// Show \p rows as the project's bookmarks.
  void showBookmarks(const QJsonArray &rows);
  /// Show and keep \p rows as the project's bookmarks.
  void setBookmarks(const QJsonArray &rows);
  /// Where this machine's settings kept a project's bookmarks before the
  /// project kept them.
  QString bookmarksKey() const;
  /// Ask where to pack the project, and pack it there.
  void packDatabase();

  Session &session_;
  McpConnectionManager &mcp_;
  GuiSessionBroker &broker_;
  ActionRegistry actions_;
  AddressSpace space_;
  NavigationBand *navigationBand_ = nullptr;
  /// Null where the window keeps the window manager's frame.
  WindowFrame *frame_ = nullptr;
  TitleBar *titleBar_ = nullptr;
  QMenuBar *menuBar_ = nullptr;
  DisassemblyView *disassembly_ = nullptr;
  DisassemblyView *primaryDisassembly_ = nullptr;
  HexView *hex_ = nullptr;
  OutputWindow *output_ = nullptr;
  MicrosoftSymbols *microsoftSymbols_ = nullptr;
  /// A file to open once the worker has started, asking for its debug
  /// information then.
  std::optional<std::pair<QString, LoadOptions>> debugPromptPending_;
  bool asksForDebugInfo_ = true;
  GraphOverview *overview_ = nullptr;
  ExtensionsView *extensions_ = nullptr;
  LocalTypesView *localTypes_ = nullptr;
  TypeLibrariesView *typeLibraries_ = nullptr;
  PathfinderView *pathfinder_ = nullptr;
  InstanceServer instances_;
  /// A link to open once the program's addresses are known, and what to
  /// tell its sender then.
  std::optional<std::pair<DeepLink, std::function<void(const QString &)>>>
      pendingLink_;
  /// The project's waypoints and exclusions for a Pathfinder not made yet.
  QJsonObject pathfinderState_;
  ChooserView *functions_ = nullptr;
  QHash<QString, Dock *> docks_;
  /// Creation order keeps generated window names stable across focus changes.
  QVector<Dock *> codeDocks_;
  QHash<int, ChooserView *> choosers_;
  QPointer<CodeView> pseudocode_;
  QPointer<CodeView> lastCodeView_;
  QPointer<QWidget> contextMenuFocus_;
  /// Tab returns to the last code window, even after assembly takes focus.
  QPointer<CodeView> tabCodeView_;
  std::optional<Address> initialAddress_;
  bool restoreGraph_ = false;
  QJsonArray bookmarks_;
  QPointer<QDialog> quickStart_;
  QPointer<FindInViewDialog> FindDialog;
  QPointer<QDialog> LocalReferencesDialog;
  QPointer<web::PackageReviewDialog> packageReview_;
  QPointer<ComparisonDialog> comparison_;
  QPointer<LineageDialog> lineage_;
  bool startupQuickStartPending_ = false;
  QLabel *analysisLabel_ = nullptr, *directionLabel_ = nullptr,
         *diskLabel_ = nullptr, *fileLabel_ = nullptr;
  QMenu *recentMenu_ = nullptr;
  QMenu *windowsMenu_ = nullptr;
  QString layoutPath_;
  QByteArray focusLayout_;
  QPointer<QWidget> focusReturn_;
  QPointer<FocusSidebar> focusSidebar_;
  QPointer<QToolBar> focusToolbar_;
  QPointer<ChooserView> focusFunctions_;
  QPointer<QWidget> focusSequenceView_;
  QTimer focusSequenceTimer_;
  int focusSequenceCount_ = 0;
  QString lastTextSearch_, lastByteSearch_, lastImmediateSearch_;
  quint64 searchSerial_ = 0, searchSubscription_ = 0;
  QString lastPaletteCommand_;
  bool searchDown_ = true;
  quint64 codeNavigationSerial_ = 0;
  bool codeHistoryNavigation_ = false;
  /// Code windows and the disassembly follow each other's cursor.
  bool synchronizeCode_ = true;
  bool hexActive_ = false;
  /// An asynchronous listing jump following the byte cursor must not echo its
  /// rounded instruction address back into Hex or discard its selection.
  QPointer<DisassemblyView> hexFollower_;
  bool quitting_ = false;
  bool layoutInitialized_ = false;
  bool defaultSizesPending_ = false;
  QTimer statusTimer_;
  QString diskStatusPath_;
  QElapsedTimer diskStatusAge_;
  QStringList pendingCommands_;
  /// The code view asked for while a jump was still loading.
  std::optional<std::pair<QString, CodeOpen>> pseudocodeAfterJump_;
  QHash<QString, const char *> dockTitles_;
  static inline MainWindow *instance_ = nullptr;
};

} // namespace neverd::gui
