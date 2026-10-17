// Workbench controller tests against the fixture worker: the session, the
// production main window and its views, edits and session transitions.
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "DwarfPromptDialog.h"
#include "ExtensionsView.h"
#include "FunctionEditDialog.h"
#include "GraphView.h"
#include "HexView.h"
#include "ListingView.h"
#include "LoadFileDialog.h"
#include "LocalTypesView.h"
#include "MainWindow.h"
#include "MapPromptDialog.h"
#include "OutputWindow.h"
#include "PdbPromptDialog.h"
#include "ProjectDatabase.h"
#include "QuickStartDialog.h"
#include "Session.h"
#include "SetTypeDialog.h"
#include "SettingsKeys.h"
#include "SymbolTablePromptDialog.h"
#include "Theme.h"
#include "TypePickerDialog.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QCompleter>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QHeaderView>
#include <QHelpEvent>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSizeGrip>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabBar>
#include <QTableView>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTimer>
#include <QToolButton>
#include <QToolTip>
#include <QTranslator>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrl>
#include <cmath>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <memory>

using namespace neverd::gui;

namespace {
constexpr Address Base = 0xffff800012340000ULL;
constexpr int OpenTimeoutMs = 7000;
constexpr int NativeOpenTimeoutMs = 30000;
// The widest a view may insist on being: a dock's tab bar keeps the dock some
// 130 pixels wide whatever it shows.
constexpr int NarrowViewWidth = 160;

/// Long enough for the offer of a missing PDB to follow an open.
constexpr int OfferWaitMs = 500;

QString writeFixture(const QTemporaryDir &directory, const QString &name) {
  const auto path = directory.filePath(name);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
    return {};
  return path;
}

/// The production window over the fixture worker.
struct Workbench {
  Session session{QString::fromLocal8Bit(TEST_WORKER)};
  McpConnectionManager mcp;
  GuiSessionBroker broker;
  QTemporaryDir layout;
  std::unique_ptr<MainWindow> window;

  /// Unless \p shown, the window stays as a startup without a file leaves
  /// it: hidden, its desktop not laid out yet.
  explicit Workbench(QString worker = QString::fromLocal8Bit(TEST_WORKER),
                     bool shown = true)
      : session(std::move(worker)) {
    window = std::make_unique<MainWindow>(session, mcp, broker);
    window->setLayoutPath(layout.filePath(QStringLiteral("layout.json")));
    window->resize(1400, 900);
    if (!shown)
      return;
    window->initializeLayout();
    window->show();
  }
  ChooserView *functions() const {
    auto *table =
        window->findChild<QTreeView *>(QStringLiteral("functionsList"));
    return table ? qobject_cast<ChooserView *>(table->parentWidget()) : nullptr;
  }
  QAction *action(ActionId id) const { return window->actions().action(id); }
  QString dockTitle(const QString &uniqueName) const {
    for (auto *dock :
         window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == uniqueName)
        return dock->title();
    return {};
  }
  CodeView *codeView(const QString &representation) const {
    for (auto *view : window->findChildren<CodeView *>())
      if (view->representation() == representation)
        return view;
    return nullptr;
  }
};

/// The representations a code view's menu offers, in order.
QStringList representationsOf(CodeView *view) {
  QStringList names;
  if (auto *selector = view->findChild<QComboBox *>())
    for (int i = 0; i < selector->count(); ++i)
      names << selector->itemData(i).toString();
  return names;
}

QByteArray readAll(const QString &path) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

/// A file dialog the user cancels.
QString noFile(QWidget *) { return {}; }

QPushButton *buttonNamed(QWidget *window, const QString &text) {
  for (auto *button : window->findChildren<QPushButton *>())
    if (button->text() == text)
      return button;
  return nullptr;
}
/// Accepts every "Load a new file" dialog with its defaults while it lives,
/// as pressing Enter does.
/// Shows \p code's context menu at its cursor and hands it to \p use, which
/// may trigger one of its entries; the menu then closes.
void withCodeMenu(CodeText *code, const std::function<void(QMenu *)> &use) {
  QTimer probe;
  QObject::connect(&probe, &QTimer::timeout, &probe, [&] {
    auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
    if (!menu)
      return;
    probe.stop();
    use(menu);
    menu->close();
  });
  probe.start(5);
  emit code->contextMenuRequested(
      code->viewport()->mapToGlobal(QPoint(40, 40)));
}

class LoadDialogAcceptor {
public:
  LoadDialogAcceptor() {
    QObject::connect(&timer_, &QTimer::timeout, [this] {
      if (auto *dialog = qobject_cast<LoadFileDialog *>(
              QApplication::activeModalWidget())) {
        ++accepted_;
        dialog->accept();
      }
    });
    timer_.start(10);
  }
  int accepted() const { return accepted_; }

private:
  QTimer timer_;
  int accepted_ = 0;
};

bool dropFile(QWidget *target, const QString &path) {
  QMimeData mime;
  mime.setUrls({QUrl::fromLocalFile(path)});
  QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                        Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &enter);
  if (!enter.isAccepted() || enter.dropAction() != Qt::CopyAction)
    return false;
  QDragMoveEvent move(QPoint(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                      Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &move);
  if (!move.isAccepted() || move.dropAction() != Qt::CopyAction)
    return false;
  QDropEvent drop(QPointF(10, 10), Qt::CopyAction | Qt::MoveAction, &mime,
                  Qt::LeftButton, Qt::ShiftModifier);
  QApplication::sendEvent(target, &drop);
  return drop.isAccepted() && drop.dropAction() == Qt::CopyAction;
}

void dragText(QWidget *viewport, const QPoint &begin, const QPoint &end) {
  QTest::mousePress(viewport, Qt::LeftButton, Qt::NoModifier, begin);
  QMouseEvent move(QEvent::MouseMove, end, viewport->mapToGlobal(end),
                   Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(viewport, &move);
  QTest::mouseRelease(viewport, Qt::LeftButton, Qt::NoModifier, end);
}

// Windows delivers clipboard notifications asynchronously. Pump them between
// synthetic user actions, including reads that request delayed rendering.
QString clipboardText() {
  const QString text = QApplication::clipboard()->text();
  if (QGuiApplication::platformName() == QLatin1String("windows"))
    QTest::qWait(20);
  return text;
}

void clearClipboard() {
  QApplication::clipboard()->clear();
  if (QGuiApplication::platformName() == QLatin1String("windows"))
    QTest::qWait(20);
}
} // namespace

class WorkbenchTests : public QObject {
  Q_OBJECT
  QTemporaryDir settingsDirectory_;
private slots:
  void nestedPseudocodeBlocksFoldWithoutMovingAssembly_data() {
    QTest::addColumn<QString>("representation");
    QTest::newRow("pseudocode") << QStringLiteral("source");
    QTest::newRow("explicit-c") << QStringLiteral("c");
    QTest::newRow("llvm-c") << QStringLiteral("llvmc");
  }

  void nestedPseudocodeBlocksFoldWithoutMovingAssembly() {
    QFETCH(QString, representation);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    // An explicit C view is offered beside another language only.
    bench.window->openFile(
        writeFixture(directory, representation == QLatin1String("c")
                                    ? QStringLiteral("code-folding-cpp.bin")
                                    : QStringLiteral("code-folding.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    QCOMPARE(view->representation(), representation);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() == 19);
    const QString original = code->allText();
    assembly->navigate(Base + 0x180);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    // The while arrow uses the fixed line-number gutter, without selecting
    // code or changing assembly. Its parent retains the child's fold state.
    code->setCursorLine(4);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int height = int(std::ceil(metrics.lineSpacing()));
    const QPoint arrow(int(6 + 4.25 * metrics.horizontalAdvance('M')),
                       (4 - code->verticalScrollBar()->value()) * height +
                           height / 2);
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, arrow);
    QCOMPARE(code->lineCount(), 18);
    // The header stays visible: finding its text or clicking past the end of
    // its row neither expands the block nor offers a fold control there.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("while (v7)"), true));
    QCOMPARE(code->lineCount(), 18);
    const QPoint pastHeader(code->viewport()->width() - 4, arrow.y());
    QTest::mouseMove(code->viewport(), arrow);
    QCOMPARE(code->viewport()->cursor().shape(), Qt::PointingHandCursor);
    QTest::mouseMove(code->viewport(), pastHeader);
    QCOMPARE(code->viewport()->cursor().shape(), Qt::IBeamCursor);
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                      pastHeader);
    QCOMPARE(code->lineCount(), 18);
    code->setCursorLine(3);
    QTest::keyClick(code, Qt::Key_BracketLeft,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->lineCount(), 16);
    QTest::keyClick(code, Qt::Key_BracketRight,
                    Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->lineCount(), 18);
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    // Folding restored the cursor to the if header; search starts below it.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("v7 = v0"), true));
    const auto target = code->renameTarget();
    QVERIFY(target);
    const auto references = code->localReferences(*target);
    const auto hidden =
        std::find_if(references.cbegin(), references.cend(),
                     [](const auto &ref) { return ref.line == 5; });
    QVERIFY(hidden != references.cend());
    code->setCursorLine(1);
    QVERIFY(code->setCurrentBlockFolded(true));
    QCOMPARE(code->lineCount(), 3);
    QVERIFY(
        !code->currentAddress()); // The body cannot supply a header address.
    QTest::keyClick(code, Qt::Key_Home, Qt::ControlModifier);
    QTest::keyClick(code, Qt::Key_End, Qt::ControlModifier | Qt::ShiftModifier);
    QCOMPARE(code->selectedText(), original);
    QCOMPARE(code->allText(), original);
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("--v7"), true));
    QCOMPARE(code->lineCount(), 19);
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
    code->setCursorLine(1);
    QVERIFY(code->setCurrentBlockFolded(true));
    QVERIFY(code->selectLocalReference(*target, *hidden));
    QCOMPARE(code->lineCount(), 19);
    QCOMPARE(code->selectedText(), QStringLiteral("v7"));
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));

    // A refresh preserves folds, and explicit instruction navigation expands
    // all blocks hiding its mapped original row.
    code->setCursorLine(3);
    QVERIFY(code->setCurrentBlockFolded(true));
    code->load(Base + 0x140, representation);
    QTRY_VERIFY(!code->loading() && code->lineCount() == 16);
    code->selectAddress(Base + 0x142);
    QCOMPARE(code->lineCount(), 19);
    QCOMPARE(code->currentAddress(), std::optional<Address>(Base + 0x142));
    QVERIFY(code->commentTarget());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 5);
  }

  void localVariableReferencesStayInTheirCodeWindow_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("renamed");
    QTest::newRow("pseudocode") << QStringLiteral("source") << false;
    QTest::newRow("explicit-c") << QStringLiteral("c") << false;
    QTest::newRow("llvm-c") << QStringLiteral("llvmc") << false;
    QTest::newRow("renamed-local") << QStringLiteral("source") << true;
  }

  void localVariableReferencesStayInTheirCodeWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, renamed);
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, representation == QLatin1String("c")
                                    ? QStringLiteral("local-references-cpp.bin")
                                    : QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    QCOMPARE(view->representation(), representation);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->allText().contains("return v7 + v7"));
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    assembly->navigate(Base + 0x180);
    QTRY_COMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x180));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    if (renamed) {
      const auto target = code->renameTarget();
      QVERIFY(target);
      auto edit = *target;
      edit["address"] = hexAddress(Base + 0x140);
      edit["representation"] = representation;
      edit["kind"] = "name";
      edit["name"] = "counter";
      bench.session.editCode(edit, bench.session.epoch());
      QTRY_VERIFY(!code->loading() &&
                  code->allText().contains("return counter + counter"));
    }
    const auto name =
        renamed ? QStringLiteral("counter") : QStringLiteral("v7");
    // Search continues below the cursor, which is on the earlier match.
    code->setCursorLine(0);
    QVERIFY(code->findText(name + QStringLiteral(" = 0"), true));
    const auto target = code->renameTarget();
    QVERIFY(target);
    QCOMPARE(target->value("kind").toString(), QStringLiteral("local"));
    const auto references = code->localReferences(*target);
    // The complete function spans three pages. Its similarly named v70,
    // Box::v7 member, strings and comments are not this local's occurrences.
    QCOMPARE(references.size(), 5);
    QCOMPARE(references[0].line, 3);
    QCOMPARE(references[1].line, 9);
    QVERIFY(references[1].text.contains(QStringLiteral("中文")));
    QCOMPARE(references[2].line, 550);
    QCOMPARE(references[2].column, 2);
    QCOMPARE(references[3].line, 700);
    QCOMPARE(references[4].line, 700);
    QVERIFY(references[3].column < references[4].column);
    code->setFolded(true);
    QVERIFY(code->libraryFolded());
    code->setCursorLine(0);
    QVERIFY(code->findText(name + QStringLiteral(" = 0"), true));
    code->setFocus();
    bool sawDialog = false, correctRows = false;
    QTimer chooser;
    connect(&chooser, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *table = dialog ? dialog->findChild<QTableView *>(
                                 QStringLiteral("localReferencesTable"))
                           : nullptr;
      // The rows are ready at once; keys reach them once the dialog is active.
      if (!table || !dialog->isActiveWindow() || !table->hasFocus())
        return;
      chooser.stop();
      sawDialog = dialog->windowTitle().contains(name);
      const auto *model = table->model();
      correctRows = model->rowCount() == 5 &&
                    model->index(2, 0).data().toInt() == 551 &&
                    model->index(2, 1).data().toInt() == 3;
      table->setCurrentIndex(model->index(2, 0));
      auto *buttons = dialog->findChild<QDialogButtonBox *>();
      QVERIFY(buttons);
      QTest::mouseClick(buttons->button(QDialogButtonBox::Ok), Qt::LeftButton);
    });
    chooser.start(10);
    QTimer::singleShot(OpenTimeoutMs, &bench.session, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    bench.action(ActionId::JumpXrefsTo)->trigger();
    QVERIFY(sawDialog);
    QVERIFY(correctRows);
    QCOMPARE(code->selectedText(), name);
    QCOMPARE(code->currentToken(), name);
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    QCOMPARE(view->representation(), representation);
    QVERIFY(view->locked());
    QVERIFY(!code->libraryFolded());
    QVERIFY(code->preludeFolded());
    const auto row = code->commentTarget();
    QVERIFY(row);
    QCOMPARE(row->value("line").toInt(), 550);
    QCOMPARE(assembly->currentItem(), std::optional<Address>(Base + 0x180));
  }

  void closingFileDismissesLocalVariableReferences() {
    QTemporaryDir directory;
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-references.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->allText().contains("v7 = 0"));
    QVERIFY(code->findText(QStringLiteral("v7 = 0"), true));
    code->setFocus();
    bool closed = false;
    QTimer closer;
    connect(&closer, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog ||
          dialog->objectName() != QLatin1String("localReferencesDialog"))
        return;
      closer.stop();
      closed = true;
      bench.session.closeFile();
    });
    closer.start(10);
    QTimer::singleShot(OpenTimeoutMs, &bench.session, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    bench.action(ActionId::JumpXrefsTo)->trigger();
    QVERIFY(closed);
    QVERIFY(!bench.session.loaded());
    QVERIFY(!code->function());
    QVERIFY(code->selectedText().isEmpty());
  }

  void xrefsReturnToTheOriginatingCodeWindow_data() {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario :
         {"source", "llvmc", "low", "secondary-source", "published-address",
          "call-token", "unmapped", "assembly"})
      QTest::newRow(scenario) << QString::fromLatin1(scenario);
  }

  void xrefsReturnToTheOriginatingCodeWindow() {
    QFETCH(QString, scenario);
    QTemporaryDir directory;
    Workbench bench;
    const auto path = writeFixture(
        directory, scenario == "unmapped"            ? "xrefs-unmapped.bin"
                   : scenario == "published-address" ? "xrefs-address.bin"
                                                     : "xrefs-code.bin");
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *primary = bench.codeView(QStringLiteral("source"));
    QVERIFY(primary);
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *secondary = bench.codeView(QStringLiteral("low"));
    QVERIFY(secondary);
    const bool useSecondary = scenario == "llvmc" || scenario == "low" ||
                              scenario == "secondary-source";
    auto *origin = useSecondary ? secondary : primary;
    auto *other = useSecondary ? primary : secondary;
    const QString representation = scenario == "llvmc" || scenario == "low"
                                       ? scenario
                                       : QStringLiteral("source");
    origin->setRepresentation(representation);
    origin->showFunction(scenario == "call-token" ? Base + 0x160
                                                  : Base + 0x140);
    other->showFunction(Base + 0x180);
    QTRY_VERIFY(!origin->text()->loading() && !other->text()->loading());
    for (auto *button : origin->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(origin->locked());
    // Assembly has an independent location when the source query starts.
    assembly->navigate(Base + 0x1a0);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x1a0));
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    auto *code = origin->text();
    const int row = scenario == "call-token" ? 1 : 0;
    code->setCursorLine(row);
    const QString line = code->selectedText();
    const QString token = scenario == "published-address"
                              ? QStringLiteral("display_function")
                              : QStringLiteral("function_20");
    const int column = int(line.indexOf(token));
    QVERIFY(column >= 0);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(qRound(6 + (5 + column + 2) * metrics.horizontalAdvance(
                                                         QLatin1Char('M'))),
                       qRound((row - code->verticalScrollBar()->value() + 0.5) *
                              std::ceil(metrics.lineSpacing())));
    QTest::mouseClick(code->viewport(), Qt::LeftButton, {}, point);
    QCOMPARE(code->currentToken(), token);
    QTRY_VERIFY(code->hasFocus());
    if (scenario == "published-address") {
      const auto target = code->renameTarget();
      QVERIFY(target);
      QCOMPARE(addressValue(target->value("address")),
               std::optional<Address>(Base + 0x140));
    }
    QSignalSpy mapped(code, &CodeText::addressSelected);
    bool chosen = false, correctTarget = false;
    QString dialogState;
    QTimer chooser;
    connect(&chooser, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *list = dialog ? dialog->findChild<ChooserView *>() : nullptr;
      if (!list || list->model().kind() != ChooserKind::CrossReferences)
        return;
      dialogState = QStringLiteral("%1, %2 rows")
                        .arg(dialog->windowTitle())
                        .arg(list->model().total());
      for (auto *label : list->findChildren<QLabel *>())
        dialogState += QLatin1Char('\n') + label->text();
      correctTarget =
          dialog->windowTitle().contains(displayAddress(Base + 0x140, 16));
      for (int i = 0; i < list->model().total(); ++i)
        if (list->model().addressAt(i) == Base + 0x163) {
          chooser.stop();
          chosen = true;
          list->table()->setCurrentIndex(list->model().index(i, 0));
          QTest::keyClick(list->table(), Qt::Key_Return);
          return;
        }
    });
    chooser.start(5);
    QTimer timeout;
    connect(&timeout, &QTimer::timeout, this, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    timeout.setSingleShot(true);
    timeout.start(OpenTimeoutMs);
    if (scenario == "assembly") {
      assembly->navigate(Base + 0x140);
      QTRY_COMPARE(assembly->currentFunction(),
                   std::optional<Address>(Base + 0x140));
      assembly->focusContent();
      QTRY_VERIFY(assembly->listing()->hasFocus());
      QTest::keyClick(assembly->listing(), Qt::Key_X);
    } else {
      QTest::keyClick(code, Qt::Key_X);
    }
    QTRY_VERIFY2_WITH_TIMEOUT(
        chosen,
        qPrintable(dialogState + QLatin1Char('\n') +
                   bench.window->findChild<OutputWindow *>()
                       ->findChild<QPlainTextEdit *>()
                       ->toPlainText()),
        OpenTimeoutMs);
    QVERIFY(correctTarget);
    if (scenario == "assembly") {
      QTRY_COMPARE(assembly->currentAddress(),
                   std::optional<Address>(Base + 0x163));
      QTRY_VERIFY(assembly->listing()->hasFocus());
      QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
      QVERIFY(mapped.isEmpty());
      return;
    }
    QTRY_COMPARE(code->function(), std::optional<Address>(Base + 0x160));
    QTRY_VERIFY(!code->loading() && !mapped.isEmpty());
    QCOMPARE(origin->representation(), representation);
    QVERIFY(origin->locked());
    QTRY_VERIFY(code->hasFocus());
    QCOMPARE(other->text()->function(), std::optional<Address>(Base + 0x180));
    if (scenario == "unmapped") {
      QVERIFY(!mapped.back().at(1).toBool());
      QVERIFY(!code->currentAddress());
      QVERIFY(bench.window->findChild<OutputWindow *>()
                  ->findChild<QPlainTextEdit *>()
                  ->toPlainText()
                  .contains(QStringLiteral(
                      "This instruction has no source-line address mapping.")));
    } else {
      QCOMPARE(code->currentAddress(), std::optional<Address>(Base + 0x163));
      QVERIFY(code->selectedText().contains(QStringLiteral("return ") + token));
    }
  }

  void staleCodeXrefQueriesDoNotOpenDialogs_data() {
    QTest::addColumn<QString>("change");
    for (const auto *change :
         {"function", "representation", "navigation", "session", "hidden"})
      QTest::newRow(change) << QString::fromLatin1(change);
  }

  void staleCodeXrefQueriesDoNotOpenDialogs() {
    QFETCH(QString, change);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(directory, "xrefs-delayed.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    assembly->navigate(Base + 0x140);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    // IR has no pre-resolved source edit targets, so X resolves the name
    // asynchronously rather than using an address published with the text.
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *view = bench.codeView(QStringLiteral("low"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    const QFontMetricsF metrics(Theme::instance().codeFont());
    code->setCursorLine(1);
    const int column = int(code->selectedText().indexOf("delayed_function"));
    QVERIFY(column >= 0);
    QTest::mouseClick(
        code->viewport(), Qt::LeftButton, {},
        QPoint(qRound(6 + (5 + column + 2) * metrics.horizontalAdvance('M')),
               qRound((1 - code->verticalScrollBar()->value() + 0.5) *
                      std::ceil(metrics.lineSpacing()))));
    QCOMPARE(code->currentToken(), QStringLiteral("delayed_function"));
    QTRY_VERIFY(code->hasFocus());
    QSignalSpy messages(&bench.session, &Session::message);
    bool dialogOpened = false;
    QTimer guard;
    connect(&guard, &QTimer::timeout, this, [&] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
        dialogOpened = true;
        dialog->reject();
      }
    });
    guard.start(5);
    bench.action(ActionId::JumpXrefOperand)->trigger();
    const auto started = [&] {
      for (const auto &row : messages)
        if (row.first().toString().contains("fixture delayed lookup started"))
          return true;
      return false;
    };
    QTRY_VERIFY(started());
    if (change == "function")
      view->showFunction(Base + 0x180);
    else if (change == "representation")
      view->setRepresentation(QStringLiteral("llvmc"));
    else if (change == "navigation")
      assembly->navigate(Base + 0x180);
    else if (change == "session")
      bench.session.closeFile();
    else
      view->hide();
    QTest::qWait(600);
    QVERIFY(!dialogOpened);
    QVERIFY(!QApplication::activeModalWidget());
    if (change == "session") {
      QVERIFY(!bench.session.loaded());
    } else {
      QCOMPARE(code->function(),
               std::optional<Address>(change == "function" ? Base + 0x180
                                                           : Base + 0x140));
      // Navigation reads its page from the worker; a slow runner may still
      // be on the way there.
      QTRY_COMPARE(assembly->currentFunction(),
                   std::optional<Address>(
                       change == "navigation" ? Base + 0x180 : Base + 0x140));
    }
  }

  void copyingCharacterSelections_data() {
    QTest::addColumn<QString>("view");
    QTest::addColumn<bool>("reverse");
    for (const QString view :
         {QStringLiteral("address"), QStringLiteral("assembly"),
          QStringLiteral("source"), QStringLiteral("go"),
          QStringLiteral("llvmc")}) {
      QTest::newRow(qPrintable(view + "-forward")) << view << false;
      QTest::newRow(qPrintable(view + "-reverse")) << view << true;
    }
  }

  void outputLogClearsFromItsMenu() {
    Workbench bench;
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output);
    auto *log = output->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QCOMPARE(log->contextMenuPolicy(), Qt::CustomContextMenu);
    output->append(QStringLiteral("first line"));
    const auto action = [](const std::unique_ptr<QMenu> &menu,
                           const char *name) {
      return menu->findChild<QAction *>(QLatin1String(name));
    };
    std::unique_ptr<QMenu> menu(output->createLogMenu());
    QVERIFY(action(menu, "outputCopy") && action(menu, "outputSelectAll"));
    QVERIFY(!action(menu, "outputCopy")->isEnabled());
    QVERIFY(action(menu, "outputClear")->isEnabled());
    action(menu, "outputSelectAll")->trigger();
    QVERIFY(log->textCursor().hasSelection());
    menu.reset(output->createLogMenu());
    QVERIFY(action(menu, "outputCopy")->isEnabled());
    action(menu, "outputClear")->trigger();
    QVERIFY(log->document()->isEmpty());
    // An empty log has nothing to clear.
    menu.reset(output->createLogMenu());
    QVERIFY(!action(menu, "outputClear")->isEnabled());
  }

  void copyingCharacterSelections() {
    QFETCH(QString, view);
    QFETCH(bool, reverse);
    QTemporaryDir directory;
    Workbench bench;
    const auto path = writeFixture(directory, view == QLatin1String("go")
                                                  ? "pseudocode-go.bin"
                                                  : "fixture.bin");
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    const QFont font = Theme::instance().codeFont();
    const QFontMetricsF metrics(font);
    const int height = int(std::ceil(metrics.lineSpacing()));
    const qreal width = metrics.horizontalAdvance(QLatin1Char('M'));
    QAbstractScrollArea *text = nullptr;
    QString row;
    int first = 0, last = 0, y = 0, left = 0;
    if (view == QLatin1String("address") || view == QLatin1String("assembly")) {
      auto *listing = bench.window->disassembly()->listing();
      QTRY_VERIFY(listing->cursorLine());
      listing->setShowPrefixes(view == QLatin1String("address"));
      listing->horizontalScrollBar()->setValue(0);
      left = int(std::ceil(width * 7)) + 6;
      // Pick a visible row by the same native mouse input a user uses.
      for (y = height / 2; y < listing->viewport()->height(); y += height) {
        QTest::mouseClick(listing->viewport(), Qt::LeftButton, Qt::NoModifier,
                          QPoint(left, y));
        if (!listing->cursorLine())
          continue;
        row = view == QLatin1String("address")
                  ? listing->cursorLine()->prefix
                  : listing->cursorLine()->styled.text;
        if (row.size() >= 8)
          break;
      }
      QVERIFY(row.size() >= 8);
      first = view == QLatin1String("address") ? int(row.size()) - 8 : 2;
      last = view == QLatin1String("address") ? int(row.size()) : 6;
      text = listing;
    } else {
      const QString representation = view == QLatin1String("llvmc")
                                         ? QStringLiteral("llvmc")
                                         : QStringLiteral("source");
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *codeView = bench.codeView(QStringLiteral("source"));
      QVERIFY(codeView);
      codeView->setRepresentation(representation);
      auto *code = codeView->text();
      QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 3,
                               OpenTimeoutMs);
      if (view == QLatin1String("llvmc"))
        QVERIFY(code->preludeFolded());
      const QString token = view == QLatin1String("go")
                                ? QStringLiteral("int64")
                                : QStringLiteral("code");
      int line = 1;
      for (; line < std::min(50, code->lineCount()); ++line) {
        code->setCursorLine(line);
        row = code->selectedText();
        first = int(row.indexOf(token));
        if (first >= 0)
          break;
      }
      QVERIFY(first >= 0);
      last = first + int(token.size());
      y = (line - code->verticalScrollBar()->value()) * height + height / 2;
      left = 6 +
             int((std::max(3, int(QString::number(code->lineCount()).size())) +
                  2) *
                 width) -
             code->horizontalScrollBar()->value();
      text = code;
    }
    QTextLayout layout(row, font);
    layout.beginLayout();
    const auto line = layout.createLine();
    layout.endLayout();
    const QPoint begin(left + qRound(line.cursorToX(first)), y);
    const QPoint end(left + qRound(line.cursorToX(last)), y);
    dragText(text->viewport(), reverse ? end : begin, reverse ? begin : end);
    const QString expected = row.mid(first, last - first);
    clearClipboard();
    QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), expected);
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), expected);
    // A right click must keep the selection for context-menu copying.
    QTest::mouseClick(text->viewport(), Qt::RightButton, Qt::NoModifier, end);
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), expected);
    // Keyboard selection uses the same character boundaries as mouse dragging.
    QTest::keyClick(text, Qt::Key_Home);
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(
          text, view == QLatin1String("address") ? Qt::Key_Left : Qt::Key_Right,
          Qt::ShiftModifier);
    QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), view == QLatin1String("address")
                                  ? (row + QLatin1Char(' ')).right(3)
                                  : row.left(3));
    if (view != QLatin1String("address")) {
      QTest::keyClick(text, Qt::Key_Home);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Down);
      const QString next =
          qobject_cast<CodeText *>(text)
              ? qobject_cast<CodeText *>(text)->selectedText()
              : qobject_cast<ListingView *>(text)->selectedText();
      QTest::keyClick(text, Qt::Key_Up);
      QTest::keyClick(text, Qt::Key_Home);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Right);
      QTest::keyClick(text, Qt::Key_Down, Qt::ShiftModifier);
      QTest::keyClick(text, Qt::Key_C, Qt::ControlModifier);
      QCOMPARE(clipboardText(), row.mid(2) + QLatin1Char('\n') + next.left(2));
    }
  }

  void tabFromCodeUsesSelectedInstruction_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("graph");
    QTest::newRow("pseudocode") << QStringLiteral("source") << false << false;
    QTest::newRow("llvm-c") << QStringLiteral("llvmc") << false << false;
    QTest::newRow("low-ir") << QStringLiteral("low") << false << false;
    QTest::newRow("pseudocode-graph")
        << QStringLiteral("source") << false << true;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-pseudocode")
          << QStringLiteral("source") << true << false;
      QTest::newRow("native-llvm-c")
          << QStringLiteral("llvmc") << true << false;
      QTest::newRow("native-pseudocode-graph")
          << QStringLiteral("source") << true << true;
    }
  }

  void tabFromCodeUsesSelectedInstruction() {
    QFETCH(QString, representation);
    QFETCH(bool, native);
    QFETCH(bool, graph);
    QTemporaryDir directory;
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    const auto path =
        native
            ? qEnvironmentVariable("NEVERD_CODE_NAV_FILE")
            : writeFixture(directory, QStringLiteral("code-edits-mapped.bin"));
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QTRY_VERIFY(functions && functions->model().total() >= 2);
    const auto function =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    const auto other = native ? *functions->model().addressAt(1) : Base + 0x180;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    if (graph) {
      assembly->setGraphMode(true);
      QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());
    }
    bench
        .action(representation == QLatin1String("low") ? ActionId::ViewLowIR
                : representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation);
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 2);
    code->setPreludeFolded(false);
    if (native) {
      code->setCursorLine(0);
      QVERIFY(code->findText(QStringLiteral("return ("), true));
    } else
      code->setCursorLine(2);
    const auto target = code->currentAddress();
    QVERIFY(target.has_value());
    QVERIFY(*target != function);
    if (native) {
      bool decoded = false;
      bench.session.read(
          "disasm", {{"address", hexAddress(*target)}, {"limit", 1}}, code,
          [&](const QJsonObject &p) {
            const auto rows = p.value("items").toArray();
            decoded = !rows.isEmpty() &&
                      addressValue(rows.first().toObject().value("address")) ==
                          target;
          });
      QTRY_VERIFY(decoded);
    } else
      QCOMPARE(target, std::optional<Address>(function + 2));
    // Clicks and arrow keys move the source cursor and take the assembly
    // cursor to the row's instruction, leaving the focus and the jump
    // history where they are.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const bool back = assembly->canGoBack();
    const bool forward = assembly->canGoForward();
    if (!native) {
      code->setCursorLine(0);
      assembly->navigate(function, false);
      QTRY_COMPARE(assembly->currentAddress(),
                   std::optional<Address>(function));
      const int height = int(
          std::ceil(QFontMetricsF(Theme::instance().codeFont()).lineSpacing()));
      QTest::mouseClick(code->viewport(), Qt::LeftButton, {},
                        QPoint(100, 2 * height + height / 2));
      QCOMPARE(code->currentAddress(), target);
      QTRY_COMPARE(assembly->currentAddress(), target);
    }
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    QTest::keyClick(code, Qt::Key_Right);
    QTRY_COMPARE(assembly->currentAddress(), target);
    QVERIFY(code->hasFocus());
    QCOMPARE(assembly->canGoBack(), back);
    QCOMPARE(assembly->canGoForward(), forward);
    // Assembly navigation leaves this unpinned source on its own function.
    assembly->navigate(other);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(other));
    QTest::qWait(300);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->currentAddress(), target);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(), target);
    QTRY_VERIFY(!code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(function));
    // A reverse Tab must select the mapped row, rather than simply show the
    // function. Changing the source cursor first disproves a focus-only fix.
    code->setCursorLine(0);
    QVERIFY(!code->currentAddress());
    QCOMPARE(assembly->currentAddress(), target);
    assembly->focusContent();
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab);
    QTRY_VERIFY(code->hasFocus());
    QTRY_COMPARE(code->currentAddress(), target);
    QCOMPARE(view->representation(), representation);
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty())
        QVERIFY(bench.window->grab().save(capture + "/tab-" + representation +
                                          ".png"));
    }

    // A declaration has no instruction anchor; the fallback is explicit and
    // belongs to this source function, not the hidden assembly's function.
    code->setCursorLine(0);
    QVERIFY(!code->currentAddress());
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
  }

  void contextMenusOfferTabWithItsKey() {
    // Tab's switch to the other pane is in both context menus with its key:
    // a code window's as Jump to disassembly beside the synchronization
    // switch, the disassembly's as Jump to pseudocode.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(3);
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    QString shown;
    bool jumpOffered = true;
    withCodeMenu(code, [&](QMenu *menu) {
      auto *cross =
          menu->findChild<QAction *>(QStringLiteral("codeJumpDisassembly"));
      QVERIFY(cross);
      shown = cross->text();
      // The disassembly's own wording would point back at this window.
      jumpOffered =
          menu->actions().contains(bench.action(ActionId::JumpPseudocode));
      cross->trigger();
    });
    QCOMPARE(shown, QStringLiteral("Jump to disassembly\tTab"));
    QVERIFY(!jumpOffered);
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 6));

    QStringList texts;
    QTimer probe;
    connect(&probe, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      probe.stop();
      for (const QAction *action : menu->actions())
        texts << action->text();
      menu->close();
    });
    probe.start(5);
    emit assembly->contextMenuRequested(assembly->mapToGlobal(QPoint(40, 40)));
    QVERIFY(texts.contains(bench.action(ActionId::JumpPseudocode)->text()));
    QCOMPARE(bench.action(ActionId::JumpPseudocode)->shortcut(),
             QKeySequence(Qt::Key_Tab));
  }

  void assemblyAndCodeCursorsFollowEachOther() {
    // A move in the disassembly takes a code window showing the same
    // function to the instruction's row and leaves the focus there; a code
    // window showing another function stays. "Synchronize with disassembly"
    // in the code's context menu lets both browse apart.
    QTemporaryDir directory;
    Workbench bench;
    const auto restore = qScopeGuard(
        [] { QSettings().remove(QStringLiteral("code/synchronize")); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    code->setCursorLine(1);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    assembly->focusContent();
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(function + 6, false);
    QTRY_COMPARE(code->cursorLine(), 3);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 6));
    QVERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(function + 2, false);
    QTRY_COMPARE(code->cursorLine(), 2);
    // An instruction no row maps leaves the cursor where it is.
    assembly->navigate(function + 9, false);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));
    QTest::qWait(100);
    QCOMPARE(code->cursorLine(), 2);
    // Another function is not decompiled on the way.
    assembly->navigate(Base + 0x180, false);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(Base + 0x180));
    QTest::qWait(200);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->cursorLine(), 2);

    // Apart, the code's rows move neither the disassembly nor the other way.
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    withCodeMenu(code, [](QMenu *menu) {
      auto *synchronize =
          menu->findChild<QAction *>(QStringLiteral("codeSynchronize"));
      QVERIFY(synchronize && synchronize->isChecked());
      synchronize->trigger();
    });
    QVERIFY(
        !QSettings().value(QStringLiteral("code/synchronize"), true).toBool());
    code->setCursorLine(3);
    QTest::qWait(200);
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(Base + 0x180));
    assembly->navigate(function, false);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(function));
    assembly->focusContent();
    QTRY_VERIFY(assembly->isAncestorOf(QApplication::focusWidget()));
    assembly->navigate(function + 2, false);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 2));
    QTest::qWait(100);
    QCOMPARE(code->cursorLine(), 3);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    withCodeMenu(code, [](QMenu *menu) {
      auto *synchronize =
          menu->findChild<QAction *>(QStringLiteral("codeSynchronize"));
      QVERIFY(synchronize && !synchronize->isChecked());
      synchronize->trigger();
    });
    code->setCursorLine(2);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 2));
    code->setCursorLine(3);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 6));
  }

  void pseudocodeNumbersTakeTheBaseTheUserPicks() {
    // Hex-Rays' bases for a pseudocode number: H toggles between
    // hexadecimal and decimal, the context menu offers each base with what
    // the number would read in it, # gives the number back to the
    // decompiler, a base that would change its type is refused, and each
    // change is a step of the history.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 3);
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    // A comment's digits are no number.
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("8 */"), true));
    QVERIFY(!code->numberTarget());
    code->setCursorLine(1);
    QVERIFY(code->findText(QStringLiteral("24"), true));
    const auto target = code->numberTarget();
    QVERIFY(target.has_value());
    QCOMPARE(target->line, 2);
    QCOMPARE(target->index, 0);
    QCOMPARE(target->literal.Text, std::string("24"));
    QCOMPARE(target->chosen, neverd::LiteralRadix::Number);

    QVERIFY(bench.action(ActionId::EditOperandDecimal)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditOperandOffset)->isEnabled());
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 0x18; /* 8 */")),
        OpenTimeoutMs);
    QStringList offered;
    withCodeMenu(code, [&](QMenu *menu) {
      for (const QAction *action : menu->actions())
        if (action->objectName().startsWith(QStringLiteral("codeNumber_")))
          offered << action->text();
      auto *hex = menu->findChild<QAction *>(QStringLiteral("codeNumber_hex"));
      QVERIFY(hex && hex->isChecked());
      auto *octal =
          menu->findChild<QAction *>(QStringLiteral("codeNumber_octal"));
      QVERIFY(octal && octal->isEnabled());
      octal->trigger();
    });
    QCOMPARE(offered, (QStringList{QStringLiteral("&Number\t24"),
                                   QStringLiteral("&Hexadecimal\t0x18"),
                                   QStringLiteral("&Decimal\t24"),
                                   QStringLiteral("Octa&l\t030"),
                                   QStringLiteral("&Binary\t0b11000"),
                                   QStringLiteral("&Character\t'\\030'")}));
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")),
        OpenTimeoutMs);
    bench.action(ActionId::EditOperandNumber)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 24; /* 8 */")),
        OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")),
        OpenTimeoutMs);

    // An unsigned int has no character constant; in decimal it keeps its
    // type with a suffix.
    code->setCursorLine(2);
    QVERIFY(code->findText(QStringLiteral("0xFFFFFFFF"), true));
    bench.action(ActionId::EditOperandChar)->trigger();
    QTest::qWait(200);
    QVERIFY(code->allText().contains(QStringLiteral("v1 & 0xFFFFFFFF;")));
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("v1 & 4294967295U;")),
        OpenTimeoutMs);
    QVERIFY(code->allText().contains(QStringLiteral("v0 + 030; /* 8 */")));
  }

  void codeTextSelectsLikeAnEditor() {
    // Ctrl+A selects the whole function, a drag held below the text scrolls
    // and extends the selection past the first screen, and a right click
    // inside the selection keeps it for Copy, as in an editor.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    // Ctrl+A while pages still arrive selects the whole function once the
    // last one is in, its folded prelude included.
    bool pressed = false;
    QObject context;
    connect(code->verticalScrollBar(), &QScrollBar::rangeChanged, &context,
            [&] {
              if (pressed || !code->loading() || code->lineCount() == 0)
                return;
              pressed = true;
              QTest::keyClick(code, Qt::Key_A, Qt::ControlModifier);
            });
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 600,
                             OpenTimeoutMs);
    QVERIFY(pressed);
    QCOMPARE(code->selectedText(), code->allText());
    code->setPreludeFolded(false);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());

    QTest::keyClick(code, Qt::Key_A, Qt::ControlModifier);
    QCOMPARE(code->selectedText(), code->allText());
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), code->allText());

    code->setCursorLine(0);
    QCOMPARE(code->verticalScrollBar()->value(), 0);
    const int height = int(
        std::ceil(QFontMetricsF(Theme::instance().codeFont()).lineSpacing()));
    QTest::mousePress(code->viewport(), Qt::LeftButton, {},
                      QPoint(60, 2 * height + height / 2));
    const QPoint below(60, code->viewport()->height() + 3 * height);
    QTest::mouseMove(code->viewport(), below);
    QTRY_VERIFY(code->verticalScrollBar()->value() > 100);
    QTest::mouseRelease(code->viewport(), Qt::LeftButton, {}, below);
    const int last = code->cursorLine();
    QVERIFY(last > 100);
    const QString selected = code->selectedText();
    QCOMPARE(int(selected.count(QLatin1Char('\n'))), last - 2);

    // A right click inside the selection keeps it, and the menu's Copy,
    // beside Select all, copies it.
    QApplication::clipboard()->clear();
    const QPoint inside(60, code->viewport()->height() / 2);
    QTest::mousePress(code->viewport(), Qt::RightButton, {}, inside);
    QTest::mouseRelease(code->viewport(), Qt::RightButton, {}, inside);
    QCOMPARE(code->selectedText(), selected);
    auto *copy = bench.action(ActionId::EditCopy);
    bool clicked = false;
    QTimer clicker;
    connect(&clicker, &QTimer::timeout, this, [&] {
      auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
      if (!menu)
        return;
      clicker.stop();
      clicked = menu->actions().contains(copy) &&
                menu->findChild<QAction *>(QStringLiteral("codeSelectAll"));
      if (clicked)
        QTest::mouseClick(menu, Qt::LeftButton, {},
                          menu->actionGeometry(copy).center());
      if (menu->isVisible())
        menu->close();
    });
    clicker.start(5);
    QContextMenuEvent request(QContextMenuEvent::Mouse, inside,
                              code->viewport()->mapToGlobal(inside));
    QApplication::sendEvent(code->viewport(), &request);
    QVERIFY(clicked);
    QCOMPARE(clipboardText(), selected);
    QCOMPARE(code->selectedText(), selected);
  }

  void gotoLabelsGoToTheirBlock() {
    // A double-click on a goto's label takes the code to the label and the
    // disassembly to the block the label names, Back returning to the goto.
    // The label's row maps to that block, not to the statement around it
    // the engine's rows give it, so the cursor on it takes the disassembly
    // there too.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-labels.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    const Address block = function + 0xA;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 6);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const QString label =
        QStringLiteral("L_") + QString::number(block, 16).toUpper();
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("goto ") + label, true));
    const int gotoRow = code->cursorLine();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));

    // The double-click acts on the token the cursor is on.
    code->setCursorLine(gotoRow - 1);
    QVERIFY(code->findText(label, true));
    QCOMPARE(code->cursorLine(), gotoRow);
    QCOMPARE(code->currentToken(), label);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QPointF point(code->viewport()->width() / 2.0, 1);
    QMouseEvent click(QEvent::MouseButtonDblClick, point,
                      code->viewport()->mapToGlobal(point), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(code->viewport(), &click);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    QCOMPARE(names.size(), 0);
    QVERIFY(code->cursorLine() > gotoRow);
    const int labelRow = code->cursorLine();
    QCOMPARE(code->currentToken(), label);
    QCOMPARE(code->currentAddress(), std::optional<Address>(block));
    QVERIFY(code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(function));

    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));
    QTRY_COMPARE(code->cursorLine(), gotoRow);
    bench.action(ActionId::JumpForward)->trigger();
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    QTRY_COMPARE(code->cursorLine(), labelRow);

    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(gotoRow);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(function + 2));
    code->setCursorLine(labelRow);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(block));
    // A note on the label stays the line's, where the code shows it, not
    // the block instruction's comment.
    const auto note = code->commentTarget();
    QVERIFY(note);
    QVERIFY(!note->contains(QStringLiteral("mapped_address")));
  }

  void codeEditKeepsThePlaceInALongFunction() {
    // An edit loads the function's pages again. The first page used to
    // clamp the cursor and the view, which then stayed near its end: the
    // cursor on a number of the third page jumped away from it.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-numbers-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const Address function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 600,
                             OpenTimeoutMs);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("24"), true));
    const int line = code->cursorLine();
    const int top = code->verticalScrollBar()->value();
    QVERIFY(line > 512);
    QVERIFY(top > 256);
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(QStringLiteral("return v0 + 0x18;")),
        OpenTimeoutMs);
    QCOMPARE(code->cursorLine(), line);
    QCOMPARE(code->verticalScrollBar()->value(), top);
    const auto target = code->numberTarget();
    QVERIFY(target.has_value());
    QCOMPARE(target->shown, neverd::LiteralRadix::Hex);
  }

  void graphJumpDuringFunctionLoadKeepsLatestAddress() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    const Address target = first + 2;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());

    // Enter the interval after the next function was requested, before its
    // asynchronous layout arrives. A new jump must not use the old nodes as
    // though they belonged to the function being loaded.
    assembly->graph()->showFunction(second, second);
    assembly->navigate(target);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QTest::qWait(100);
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
  }

  void graphRefreshKeepsLatestRequestedAddress_data() {
    QTest::addColumn<QString>("refresh");
    QTest::newRow("navigate-during-refresh") << QStringLiteral("navigate");
    QTest::newRow("keyboard-during-refresh") << QStringLiteral("keyboard");
    QTest::newRow("mouse-during-refresh") << QStringLiteral("mouse");
    QTest::newRow("generation-during-load") << QStringLiteral("generation");
    QTest::newRow("revision-during-load") << QStringLiteral("revision");
  }

  void graphJumpDuringFunctionResolutionKeepsLatestAddress() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    const Address target = first + 2;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());

    // Another function first resolves through the listing. A new jump within
    // the visible graph supersedes that still-pending resolution as well.
    assembly->navigate(second);
    QVERIFY(assembly->listing()->jumpPending());
    assembly->navigate(target);
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QTRY_VERIFY(!assembly->listing()->jumpPending());
    QTRY_VERIFY(assembly->graph()->loaded());
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(first));
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    QCOMPARE(assembly->listing()->currentAddress(),
             std::optional<Address>(target));
  }

  void graphRefreshKeepsLatestRequestedAddress() {
    QFETCH(QString, refresh);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address first = Base + 0x140;
    const Address second = Base + 0x180;
    assembly->navigate(first);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(first));
    assembly->setGraphMode(true);
    QTRY_VERIFY(assembly->graphMode() && assembly->graph()->loaded());
    auto *graph = assembly->graph();
    QSignalSpy status(graph, &GraphView::statusChanged);
    Address target;
    if (refresh != QLatin1String("generation") &&
        refresh != QLatin1String("revision")) {
      target = first + 2;
      // A refresh retains its current nodes, so navigation can succeed before
      // the replacement layout arrives. That layout must retain the new row.
      graph->showFunction(first, first);
      if (refresh == QLatin1String("navigate")) {
        assembly->navigate(target);
      } else if (refresh == QLatin1String("keyboard")) {
        QTest::keyClick(graph, Qt::Key_Down);
        QTest::keyClick(graph, Qt::Key_Down);
      } else {
        QVERIFY(graph->nodes().size() > 2);
        graph->centerOn(graph->nodes().at(2).box.center());
        QTest::mouseClick(graph, Qt::LeftButton, {},
                          QPoint(graph->width() / 2, graph->height() / 2));
      }
      QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
    } else {
      target = second + 2;
      graph->showFunction(second, target);
      QVERIFY(!graph->currentAddress());
      // Background discovery or edits can refresh a function whose graph has
      // not arrived. Preserve the requested instruction across that refresh.
      if (refresh == QLatin1String("generation"))
        emit bench.session.generationChanged();
      else
        emit bench.session.revisionChanged();
    }
    QTRY_VERIFY(!status.isEmpty() &&
                status.back().front().toString().isEmpty());
    QVERIFY(graph->loaded());
    QCOMPARE(assembly->currentAddress(), std::optional<Address>(target));
  }

  void tabFromAssemblyWaitsForPagesAndUnfoldsTarget() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("tab-sync-paged.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    const Address function = Base + 0x140, other = Base + 0x180;
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    assembly->navigate(function + 9);
    assembly->focusContent();
    // Tab during a listing jump also waits for the correct function to land.
    bench.action(ActionId::JumpPseudocode)->trigger();
    CodeView *view = nullptr;
    QTRY_VERIFY((view = bench.codeView(QStringLiteral("source"))));
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 9));
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QVERIFY(code->preludeFolded());
    QVERIFY(code->verticalScrollBar()->value() > 256);
    QVERIFY(code->hasFocus());

    // Reuse the complete function without a decompile and expand just the
    // target operation; the declaration prelude stays folded.
    code->setFolded(true);
    QVERIFY(code->libraryFolded());
    code->setCursorLine(0);
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(code->hasFocus());
    QVERIFY(!code->libraryFolded());
    QVERIFY(code->preludeFolded());
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 9));
    // This row's primary mapping is +8: the selected secondary instruction
    // still survives the reverse Tab.
    QTest::keyClick(code, Qt::Key_Tab);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));

    const auto oldScroll = code->verticalScrollBar()->value();
    assembly->navigate(other + 9);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(other));
    QTest::qWait(300);
    QCOMPARE(code->function(), std::optional<Address>(function));
    QCOMPARE(code->verticalScrollBar()->value(), oldScroll);
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(!code->loading() && code->function() == other);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
    QCOMPARE(code->commentTarget()->value("line").toInt(), 550);
    QVERIFY(code->hasFocus());

    // Missing mappings are reported without guessing a nearby source row.
    assembly->navigate(other + 5);
    QTRY_COMPARE(assembly->currentAddress(), std::optional<Address>(other + 5));
    assembly->focusContent();
    QSignalSpy selected(code, &CodeText::addressSelected);
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_COMPARE(selected.size(), 1);
    QCOMPARE(selected.first().at(1).toBool(), false);
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));

    // Two code panes can display the same representation. Reverse Tab must
    // return to the actual pane used, rather than the default source dock.
    bench.action(ActionId::ViewLowIR)->trigger();
    auto *second = bench.codeView(QStringLiteral("low"));
    QVERIFY(second && second != view);
    second->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY(!second->text()->loading());
    second->text()->setFocus();
    QTRY_VERIFY(second->text()->hasFocus());
    assembly->navigate(function + 9);
    QTRY_COMPARE(assembly->currentAddress(),
                 std::optional<Address>(function + 9));
    assembly->focusContent();
    QTRY_VERIFY(assembly->listing()->hasFocus());
    QTest::keyClick(assembly->listing(), Qt::Key_Tab);
    QTRY_VERIFY(second->text()->hasFocus() && !second->text()->loading());
    QCOMPARE(second->text()->function(), std::optional<Address>(function));
    QCOMPARE(second->text()->currentAddress(),
             std::optional<Address>(function + 9));
    QCOMPARE(code->function(), std::optional<Address>(other));
    QCOMPARE(code->currentAddress(), std::optional<Address>(other + 9));
  }

  void pseudocodeNamesAndCommentsEditTheirSource_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("native");
    QTest::addColumn<bool>("mapped");
    for (const auto &representation :
         {QStringLiteral("source"), QStringLiteral("c"),
          QStringLiteral("llvmc")})
      QTest::newRow(qPrintable(representation))
          << representation << false << false;
    QTest::newRow("mapped-source") << QStringLiteral("source") << false << true;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-source")
          << QStringLiteral("source") << true << false;
      QTest::newRow("native-llvmc") << QStringLiteral("llvmc") << true << false;
    }
  }

  void savedSourceNoteRetainsItsOwnerOnAMappedRow() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("code-edits-mapped.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    const auto function = Base + 0x140;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading());
    // Simulate a note saved while an older engine could not map this line.
    bench.session.editCode({{"address", hexAddress(function)},
                            {"representation", "source"},
                            {"kind", "comment"},
                            {"line", 2},
                            {"anchor", "  int32_t v1 = v0 + function_22();"},
                            {"text", "old source note"}},
                           bench.session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->allText().contains("old source note"),
                             OpenTimeoutMs);
    code->setPreludeFolded(false);
    code->setCursorLine(2);
    QCOMPARE(code->currentAddress(), std::optional<Address>(function + 2));
    const auto target = code->commentTarget();
    QVERIFY(target.has_value());
    QVERIFY(!target->contains("mapped_address"));
    QCOMPARE(target->value("text").toString(),
             QStringLiteral("old source note"));
    auto edit = *target;
    edit["address"] = hexAddress(function);
    edit["representation"] = "source";
    edit["kind"] = "comment";
    edit["text"] = "updated source note";
    bench.session.editCode(edit, bench.session.epoch());
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("updated source note"),
                             OpenTimeoutMs);
    QVERIFY(!code->allText().contains("old source note"));
  }

  void pseudocodeNamesAndCommentsEditTheirSource() {
    QFETCH(QString, representation);
    QFETCH(bool, native);
    QFETCH(bool, mapped);
    QTemporaryDir directory;
    const auto input = qEnvironmentVariable("NEVERD_CODE_NAV_FILE");
    const auto path =
        native ? directory.filePath("acceptance.exe")
               : writeFixture(directory,
                              mapped ? QStringLiteral("code-edits-mapped.bin")
                              : representation == QLatin1String("c")
                                  ? QStringLiteral("code-edits-cpp.bin")
                                  : QStringLiteral("code-edits.bin"));
    if (native) {
      QVERIFY(QFile::copy(input, path));
      const auto pdb = QFileInfo(input).absolutePath() + "/" +
                       QFileInfo(input).completeBaseName() + ".pdb";
      if (QFileInfo::exists(pdb))
        QVERIFY(QFile::copy(pdb, directory.filePath("acceptance.pdb")));
    }
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *assembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_VERIFY_WITH_TIMEOUT(functions->model().total() >= 2,
                             NativeOpenTimeoutMs);
    const auto function =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    const auto elsewhere =
        native ? *functions->model().addressAt(1) : Base + 0x180;
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(representation == QLatin1String("llvmc")
                                    ? QStringLiteral("llvmc")
                                    : QStringLiteral("source"));
    QVERIFY(view);
    if (representation == QLatin1String("c")) {
      view->setRepresentation(representation);
      QCOMPARE(view->representation(), representation);
    }
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->allText().contains(native ? "value" : "int32_t v1"),
        NativeOpenTimeoutMs);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    assembly->navigate(elsewhere);
    QTRY_COMPARE(assembly->currentFunction(),
                 std::optional<Address>(elsewhere));

    int commentLine = -1;
    auto edit = [&](const QString &name, const QString &value,
                    bool comment = false) {
      bench.window->activateWindow();
      if (!QTest::qWaitForWindowActive(bench.window.get()))
        return false;
      code->setFocus();
      if (!QTest::qWaitFor([&] { return code->hasFocus(); }, OpenTimeoutMs))
        return false;
      code->setCursorLine(0);
      if (!code->findText(name, true))
        return false;
      if (comment) {
        const auto target = code->commentTarget();
        if (!target || !target->value("anchor").toString().contains(name))
          return false;
        commentLine = target->value("line").toInt(-1);
      }
      QTimer filler;
      bool accepted = false;
      connect(&filler, &QTimer::timeout, &filler, [&] {
        if (auto *dialog = qobject_cast<QInputDialog *>(
                QApplication::activeModalWidget())) {
          dialog->setTextValue(value);
          accepted = true;
          dialog->accept();
        }
      });
      filler.start(10);
      if (comment)
        bench.action(ActionId::EditComment)->trigger();
      else
        QTest::keyClick(code, Qt::Key_N);
      return QTest::qWaitFor([&] { return accepted; }, OpenTimeoutMs);
    };
    QVERIFY(edit(native ? QStringLiteral("value") : QStringLiteral("v0"),
                 QStringLiteral("input_value")));
    QTRY_VERIFY2_WITH_TIMEOUT(
        code->allText().contains(native ? "input_value" : "v1 = input_value +"),
        qPrintable(code->status() + '\n' + diagnostics.join('\n') + '\n' +
                   code->allText()),
        NativeOpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    if (!native)
      QVERIFY(code->allText().contains("\"v0 v1 function_22\""));
    QCOMPARE(assembly->currentFunction(), std::optional<Address>(elsewhere));

    QVERIFY(
        edit(native ? QStringLiteral("sample") : QStringLiteral("function_22"),
             QStringLiteral("renamed_callee")));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("renamed_callee("),
                             NativeOpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    if (!native)
      QVERIFY(code->allText().contains("\"v0 v1 function_22\""));
    QVERIFY(edit(native ? QStringLiteral("return (") : QStringLiteral("v1 ="),
                 QString::fromUtf8("本行注释\nsecond */ line"), true));
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains(QString::fromUtf8("本行注释")), OpenTimeoutMs);
    QVERIFY(code->allText().contains("second * / line"));
    QVERIFY(commentLine >= 0);
    QVERIFY(code->allText()
                .split('\n')
                .at(commentLine)
                .contains(QString::fromUtf8("本行注释")));
    QVERIFY(code->isVisible());
    QVERIFY(view->locked());
    if (mapped) {
      bool checked = false;
      bench.session.read("resolve", {{"query", hexAddress(function + 2)}}, code,
                         [&](const QJsonObject &p) {
                           checked = p.value("comment").toString().contains(
                               QString::fromUtf8("本行注释"));
                         });
      QTRY_VERIFY_WITH_TIMEOUT(checked, OpenTimeoutMs);
    }
    bench.action(ActionId::EditUndo)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->allText().contains(QString::fromUtf8("本行注释")),
        OpenTimeoutMs);
    bench.action(ActionId::EditRedo)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains(QString::fromUtf8("本行注释")), OpenTimeoutMs);
    // Save keeps the project beside the binary; packing writes a database.
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    QVERIFY(!QFileInfo::exists(path + ".nddb"));
    bench.session.packDatabase(path + ".nddb");
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path + ".nddb"), OpenTimeoutMs);
    QString databaseError;
    const auto stored = ProjectDatabase::read(path + ".nddb", &databaseError);
    QVERIFY2(stored.has_value(), qPrintable(databaseError));
    QVERIFY(stored->sidecars.contains(QStringLiteral(".neverd-code.json")));
    QVERIFY(stored->sidecars.value(QStringLiteral(".neverd-code.json"))
                .contains("input_value"));
    QTemporaryDir restored;
    const auto unpacked = ProjectDatabase::unpack(
        path + ".nddb", restored.path(), &databaseError);
    QVERIFY2(!unpacked.isEmpty(), qPrintable(databaseError));
    QCOMPARE(readAll(unpacked + ".neverd/code.json"),
             readAll(path + ".neverd/code.json"));
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    assembly->navigate(function);
    QTRY_COMPARE(assembly->currentFunction(), std::optional<Address>(function));
    view->showFunction(function);
    bench
        .action(representation == QLatin1String("llvmc")
                    ? ActionId::ViewLLVMC
                    : ActionId::ViewPseudocode)
        ->trigger();
    view->setRepresentation(representation);
    QTRY_VERIFY_WITH_TIMEOUT(
        code->allText().contains("input_value") &&
            code->allText().contains("renamed_callee(") &&
            code->allText().contains(QString::fromUtf8("本行注释")),
        NativeOpenTimeoutMs);
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        QFile source(capture + "/edits-" + representation + ".txt");
        QVERIFY(source.open(QIODevice::WriteOnly));
        QVERIFY(source.write(code->allText().toUtf8()) > 0);
        QVERIFY(bench.window->grab().save(capture + "/edits-" + representation +
                                          ".png"));
      }
    }
  }
  void independentAnalysisViewsRunConcurrently() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QObject slowOwner, fastOwner;
    bool slowFinished = false, fastFinished = false;
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base)}, {"representation", "source"}},
        &slowOwner, [&](const QJsonObject &) { slowFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        5000);
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &fastOwner, [&](const QJsonObject &page) {
          fastFinished = !page.value("text").toString().isEmpty();
        });
    // A pinned slow function must not serialize another analysis window.
    QTRY_VERIFY_WITH_TIMEOUT(fastFinished, 1500);
    QVERIFY(!slowFinished);
    bench.session.cancelReads();
  }

  void cancellingOneAnalysisLeavesTheOtherRunning() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner, replacementOwner;
    bool firstFinished = false, secondFinished = false,
         replacementFinished = false;
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base)}, {"representation", "source"}},
        &firstOwner, [&](const QJsonObject &) { firstFinished = true; });
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &secondOwner, [&](const QJsonObject &) { secondFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started") &&
            diagnostics.join('\n').contains("fixture parallel decompile " +
                                            hexAddress(Base + 16) + " started"),
        5000);
    bench.session.cancelAnalysisReads(&firstOwner);
    bench.session.read(
        "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}},
        &replacementOwner,
        [&](const QJsonObject &) { replacementFinished = true; });
    QTRY_VERIFY_WITH_TIMEOUT(replacementFinished, 1500);
    QTRY_VERIFY_WITH_TIMEOUT(secondFinished, 5000);
    QVERIFY(!firstFinished);
  }

  void analysisPagesKeepTheirReplicaWhenAnotherFunctionQueues() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner, thirdOwner;
    const QJsonObject first{{"address", hexAddress(Base)},
                            {"representation", "source"}};
    auto *firstReplica = &session.analysisQueries(first, &firstOwner);
    int completed = 0;
    session.read("decompile", first, &firstOwner,
                 [&](const QJsonObject &) { ++completed; });
    session.read(
        "decompile",
        {{"address", hexAddress(Base + 16)}, {"representation", "source"}},
        &secondOwner, [&](const QJsonObject &) { ++completed; });
    session.read(
        "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}},
        &thirdOwner, [&](const QJsonObject &) { ++completed; });
    auto next = first;
    next["offset"] = 512;
    QCOMPARE(&session.analysisQueries(next, &firstOwner), firstReplica);
    session.read("decompile", next, &firstOwner, [&](const QJsonObject &page) {
      QVERIFY(page.value("text").toString().contains("code line 699"));
      ++completed;
    });
    QTRY_COMPARE_WITH_TIMEOUT(completed, 4, 8000);
  }

  void changingProjectsRetiresBothAnalysisReplicas() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject firstOwner, secondOwner;
    int retired = 0;
    const auto oldReply = [&](const QJsonObject &response) {
      QCOMPARE(response.value("status").toString(), QStringLiteral("error"));
      QCOMPARE(response.value("error").toObject().value("code").toString(),
               QStringLiteral("worker_stopped"));
      ++retired;
    };
    for (const auto &[address, owner] :
         {std::pair{Base, &firstOwner}, std::pair{Base + 16, &secondOwner}}) {
      const QJsonObject payload{{"address", hexAddress(address)},
                                {"representation", "source"}};
      session.analysisQueries(payload, owner)
          .subscribe({"decompile", payload}, owner, oldReply);
    }
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started") &&
            diagnostics.join('\n').contains("fixture parallel decompile " +
                                            hexAddress(Base + 16) + " started"),
        5000);
    const auto nextPath =
        writeFixture(directory, QStringLiteral("pseudocode-import.bin"));
    session.open(nextPath);
    QTRY_COMPARE_WITH_TIMEOUT(retired, 2, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded() && session.filePath() == nextPath,
                             OpenTimeoutMs);
    int fresh = 0;
    for (const auto &[address, owner] :
         {std::pair{Base, &firstOwner}, std::pair{Base + 16, &secondOwner}})
      session.read(
          "decompile",
          {{"address", hexAddress(address)}, {"representation", "source"}},
          owner, [&](const QJsonObject &page) {
            QVERIFY(page.value("text").toString().contains("function_22"));
            ++fresh;
          });
    QTRY_COMPARE_WITH_TIMEOUT(fresh, 2, OpenTimeoutMs);
    QCOMPARE(retired, 2);
  }

  void externalGraphPagesKeepTheirSnapshotAcrossQueuedFunctions() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery("summary", "cfg_summary",
                          {{"address", hexAddress(Base)}}, {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    const auto summary = replies.front()[1].toJsonObject();
    QCOMPARE(summary.value("status").toString(), QStringLiteral("ok"));
    const auto layout =
        summary.value("payload").toObject().value("layout_revision");
    QVERIFY(!layout.toString().isEmpty());
    for (int i = 1; i <= 3; ++i)
      session.externalQuery(QString::number(i), "decompile",
                            {{"address", hexAddress(Base + 16 * i)},
                             {"representation", "source"}},
                            {});
    session.externalQuery("viewport", "cfg_viewport",
                          {{"address", hexAddress(Base)},
                           {"layout_revision", layout},
                           {"x", 0},
                           {"y", 0},
                           {"width", 1000},
                           {"height", 1000}},
                          {});
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 5, OpenTimeoutMs);
    QCOMPARE(replies.back()[0].toString(), QStringLiteral("viewport"));
    const auto viewport = replies.back()[1].toJsonObject();
    QCOMPARE(viewport.value("status").toString(), QStringLiteral("ok"));
    QCOMPARE(viewport.value("payload").toObject().value("layout_revision"),
             layout);
  }

  void slowPseudocodeKeepsBrowsingUntilExplicitTab() {
    QTemporaryDir directory;
    Workbench bench;
    const auto path =
        writeFixture(directory, QStringLiteral("pseudocode-slow.bin"));
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    QVERIFY(!bench.codeView(QStringLiteral("source")));
    QTest::qWait(250);
    QVERIFY(!diagnostics.join('\n').contains("fixture slow decompile started"));
    bench.session.setComment(Base, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(view->isVisible() && bench.window->disassembly()->isVisible());
    QVERIFY(view->mapToGlobal(QPoint()).x() >
            bench.window->disassembly()->mapToGlobal(QPoint()).x());
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture slow decompile started"),
        5000);
    // An uncached page and listing must finish while the 30-second engine
    // call is still running, not just leave the Qt event loop responsive.
    bool functions = false, listing = false;
    bench.session.read("functions", {{"offset", 512}, {"limit", 32}}, this,
                       [&](const QJsonObject &p) {
                         functions = !p.value("items").toArray().isEmpty();
                       });
    bench.session.read("listing",
                       {{"address", hexAddress(Base + 16)}, {"after", 16}},
                       this, [&](const QJsonObject &) { listing = true; });
    QTRY_VERIFY_WITH_TIMEOUT(functions && listing, 1500);
    QVERIFY(view->text()->loading());
    bench.window->disassembly()->navigate(Base + 16);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 16));
    QTest::qWait(300);
    QCOMPARE(view->text()->function(), std::optional<Address>(Base));
    QVERIFY(view->text()->loading());
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(view->text()->function(),
                              std::optional<Address>(Base + 16), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->text()->loading() &&
            view->text()->allText().contains("code line 699"),
        5000);
    QVERIFY(bench.session.dirty());
    QVERIFY(!QFile::exists(path + ".neverd/annotations.json"));
    // Tab changes focus without removing either side of the split.
    view->text()->setFocus();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QVERIFY(view->isVisible() && bench.window->disassembly()->isVisible());
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->widget() == view)
        dock->forceClose();
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    QTest::qWait(300);
    QCOMPARE(diagnostics.join('\n').count("fixture slow decompile started"), 1);
  }

  void sourceTransformationCancelsWithoutBlockingBrowsingOrLosingSource() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(writeFixture(directory, "source-options-slow.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    const auto original = view->text()->allText();
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(!diagnostics.join('\n').contains(
        "fixture symbolic transformation started"));
    auto *button = view->findChild<QToolButton *>("sourceOptimization");
    QVERIFY(button && button->isVisible() && button->isEnabled());
    auto *menu = button->menu();
    menu->popup(button->mapToGlobal(QPoint(0, button->height())));
    QTRY_VERIFY(menu->isVisible());
    auto *symbolic = menu->findChild<QAction *>("sourceSymbolic");
    QVERIFY(symbolic);
    QVERIFY(!symbolic->isChecked());
    menu->setActiveAction(symbolic);
    QTest::keyClick(menu, Qt::Key_Return);
    QTRY_VERIFY_WITH_TIMEOUT(diagnostics.join('\n').contains(
                                 "fixture symbolic transformation started"),
                             5000);
    QVERIFY(view->text()->loading());
    QCOMPARE(view->text()->allText(), original);
    bool functions = false, listing = false;
    bench.session.read("functions", {{"offset", 512}, {"limit", 32}}, this,
                       [&](const QJsonObject &p) {
                         functions = !p.value("items").toArray().isEmpty();
                       });
    bench.session.read("listing",
                       {{"address", hexAddress(Base + 16)}, {"after", 16}},
                       this, [&](const QJsonObject &) { listing = true; });
    QTRY_VERIFY_WITH_TIMEOUT(functions && listing, 1500);
    QMenu cancellation;
    view->addSourceActions(cancellation);
    auto *cancel = cancellation.findChild<QAction *>("sourceCancel");
    QVERIFY(cancel && cancel->isEnabled());
    cancel->trigger();
    QVERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QCOMPARE(view->text()->allText(), original);
    // A fresh request after retiring the blocked replica still succeeds.
    view->text()->load(Base + 16, "source");
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 !view->text()->allText().isEmpty(),
                             OpenTimeoutMs);
    QVERIFY(view->text()->sourceOptions().isEmpty());
  }

  void sourceTransformationFailureKeepsThePrecedingSource() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(directory, "source-options-fail.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::JumpPseudocode)->trigger();
    auto *view = bench.codeView("source");
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() && !view->text()->allText().isEmpty());
    const auto original = view->text()->allText();
    view->text()->setSourceOptions({{"symbolic", true}});
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QCOMPARE(view->text()->allText(), original);
    QVERIFY(view->text()->status().contains("Transformation failed"));
  }

  void sourceTransformationProfilesAreIndependentAndResetOnNavigation() {
    QTemporaryDir directory;
    Workbench bench;
    QStringList diagnostics;
    connect(&bench.session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    bench.window->openFile(writeFixture(directory, "source-cache.bin"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base));
    bench.action(ActionId::ViewLLVMC)->trigger();
    auto *view = bench.codeView("llvmc");
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() &&
                view->text()->allText().contains("return value"));
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(view->text()->status().contains("O2"));
    {
      QMenu choices;
      view->addSourceActions(choices);
      auto *defaultLevel = choices.findChild<QAction *>("sourceLLVM_O2");
      auto *basic = choices.findChild<QAction *>("sourceLLVM_basic");
      auto *recovery = choices.findChild<QAction *>("sourceLLVMControlFlow");
      QVERIFY(defaultLevel && defaultLevel->isChecked());
      QVERIFY(basic && !basic->isChecked());
      QVERIFY(recovery && recovery->isChecked());
      basic->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QCOMPARE(
          view->text()->sourceOptions().value("llvm_optimization").toString(),
          QStringLiteral("basic"));
      defaultLevel->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QVERIFY(view->text()->sourceOptions().isEmpty());
      recovery->trigger();
      QTRY_VERIFY(!view->text()->loading());
      QCOMPARE(view->text()->sourceOptions().value("llvm_control_flow"),
               QJsonValue(false));
    }
    for (const QString name :
         {QStringLiteral("sourceLLVM_O3"), QStringLiteral("sourceSymbolic")}) {
      QMenu choices;
      view->addSourceActions(choices);
      auto *action = choices.findChild<QAction *>(name);
      QVERIFY(action);
      action->trigger();
      QTRY_VERIFY(!view->text()->loading());
    }
    QCOMPARE(
        view->text()->sourceOptions().value("llvm_optimization").toString(),
        QStringLiteral("O3"));
    QVERIFY(view->text()->sourceOptions().value("symbolic").toBool());
    QCOMPARE(view->text()->sourceOptions().value("llvm_control_flow"),
             QJsonValue(false));
    QVERIFY(diagnostics.join('\n').contains("\"llvm_optimization\":\"O3\""));
    QVERIFY(diagnostics.join('\n').contains("\"symbolic\":true"));
    QMenu defaults;
    view->addSourceActions(defaults);
    defaults.findChild<QAction *>("sourceDefault")->trigger();
    QTRY_VERIFY(!view->text()->loading());
    QVERIFY(view->text()->sourceOptions().isEmpty());
    {
      QMenu choices;
      view->addSourceActions(choices);
      QVERIFY(choices.findChild<QAction *>("sourceLLVM_O2")->isChecked());
      QVERIFY(!choices.findChild<QAction *>("sourceSymbolic")->isChecked());
      QVERIFY(
          choices.findChild<QAction *>("sourceLLVMControlFlow")->isChecked());
    }
    if (const auto capture =
            qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
        !capture.isEmpty()) {
      QDir().mkpath(capture);
      const auto previous = Theme::instance().mode();
      for (const auto mode : {Theme::Mode::Light, Theme::Mode::Dark}) {
        Theme::instance().setMode(mode);
        QTest::qWait(30);
        const auto suffix = mode == Theme::Mode::Light ? "light" : "dark";
        QVERIFY(bench.window->grab().save(capture + "/source-options-" +
                                          suffix + ".png"));
        QMenu choices;
        view->addSourceActions(choices);
        choices.popup(view->mapToGlobal(QPoint(10, 50)));
        QTRY_VERIFY(choices.isVisible());
        QVERIFY(choices.grab().save(capture + "/source-options-menu-" + suffix +
                                    ".png"));
        auto *levels = qobject_cast<QMenu *>(
            choices.findChild<QAction *>("sourceLLVM_O2")->parent());
        QVERIFY(levels);
        levels->popup(choices.mapToGlobal(QPoint(choices.width(), 0)));
        QTRY_VERIFY(levels->isVisible());
        QVERIFY(levels->grab().save(capture + "/source-options-levels-" +
                                    suffix + ".png"));
      }
      Theme::instance().setMode(previous);
    }
    view->text()->setSourceOptions({{"symbolic", true}});
    QTRY_VERIFY(!view->text()->loading());
    view->text()->load(Base + 16, "llvmc");
    QTRY_VERIFY(!view->text()->loading() &&
                view->text()->allText().contains("function_1"));
    QVERIFY(view->text()->sourceOptions().isEmpty());
    QVERIFY(view->text()->status().contains("O2"));
  }

  void largePseudocodeKeepsProcessingEvents() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-large.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY(bench.window->disassembly()->currentFunction().has_value());
    QElapsedTimer elapsed;
    elapsed.start();
    qint64 longest = 0;
    int ticks = 0;
    QTimer heartbeat;
    connect(&heartbeat, &QTimer::timeout, this, [&] {
      longest = std::max(longest, elapsed.restart());
      ++ticks;
    });
    heartbeat.start(5);
    bench.action(ActionId::ViewLLVMC)->trigger();
    auto *view = bench.codeView(QStringLiteral("llvmc"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(
        !view->text()->loading() &&
            view->text()->allText().contains("code line 19999"),
        15000);
    QTest::qWait(10);
    QVERIFY(view->text()->preludeFolded());
    QVERIFY(view->text()->lineCount() >= 20000);
    QVERIFY(ticks > 1);
    QVERIFY2(longest < 500,
             qPrintable(QStringLiteral("UI event gap: %1 ms").arg(longest)));
    qInfo() << "Largest event gap while rendering 20,000 source lines:"
            << longest << "ms";
  }

  void codeAddressLinksRetainAddressNavigation_data() {
    QTest::addColumn<bool>("global");
    QTest::newRow("import-with-function-address") << false;
    QTest::newRow("global-object") << true;
  }

  void codeAddressLinksRetainAddressNavigation() {
    QFETCH(bool, global);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(
        directory, global ? QStringLiteral("pseudocode-global.bin")
                          : QStringLiteral("pseudocode-import.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 1);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    const auto target = global ? Base + 0x3000 : Base + 0x160;
    // The import intentionally has both address and function_address.
    if (!global) {
      QJsonObject resolved;
      bench.session.read(QStringLiteral("resolve"), {{"query", "function_22"}},
                         bench.window.get(),
                         [&](const QJsonObject &p) { resolved = p; });
      QTRY_VERIFY(!resolved.isEmpty());
      QVERIFY(resolved.value("import").toBool());
      QVERIFY(addressValue(resolved.value("function_address")).has_value());
    }
    const auto token =
        global ? QStringLiteral("global_value") : QStringLiteral("function_22");
    QSignalSpy activation(code, &CodeText::objectActivated);
    QSignalSpy names(code, &CodeText::nameActivated);
    // Clicking the use, after a real declaration, exercises object recognition.
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int row = global ? 2 : 1;
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(code->currentToken(), token);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(global ? activation.size() : names.size(), 1);
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(target));
    QTRY_VERIFY(!code->hasFocus());
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
  }

  void lateCodeLookupsDoNotReplaceNewerNavigation_data() {
    QTest::addColumn<bool>("error");
    QTest::addColumn<bool>("sessionChange");
    QTest::newRow("late-success-navigation") << false << false;
    QTest::newRow("late-error-navigation") << true << false;
    QTest::newRow("late-success-session") << false << true;
    QTest::newRow("late-error-session") << true << true;
  }

  void lateCodeLookupsDoNotReplaceNewerNavigation() {
    QFETCH(bool, error);
    QFETCH(bool, sessionChange);
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(writeFixture(
        directory, error ? QStringLiteral("pseudocode-delayed-error.bin")
                         : QStringLiteral("pseudocode-delayed.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY(!code->loading() && code->lineCount() > 1);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    QVERIFY(view->locked());
    QSignalSpy messages(&bench.session, &Session::message);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((1 - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(names.size(), 1);
    const auto started = [&] {
      for (const auto &row : messages)
        if (row.first().toString().contains(
                QStringLiteral("fixture delayed lookup started")))
          return true;
      return false;
    };
    QTRY_VERIFY(started());
    if (sessionChange) {
      const auto epoch = bench.session.epoch();
      bench.session.closeFile();
      QTRY_VERIFY(bench.session.epoch() != epoch);
      QTRY_VERIFY(!bench.session.loaded());
    } else {
      // The pinned view remains at its source; the address view moves.
      bench.window->disassembly()->navigate(Base + 0x180);
      QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                   std::optional<Address>(Base + 0x180));
    }
    QTest::qWait(600);
    if (!sessionChange) {
      QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
      QCOMPARE(bench.window->disassembly()->currentFunction(),
               std::optional<Address>(Base + 0x180));
    } else {
      QVERIFY(!bench.session.loaded());
    }
    for (const auto &row : messages)
      QVERIFY(
          !row.first().toString().startsWith(QStringLiteral("Cannot jump to")));
  }

  void pseudocodeFunctionLinksStayInTheirWindow_data() {
    QTest::addColumn<QString>("representation");
    QTest::addColumn<bool>("secondWindow");
    QTest::addColumn<bool>("locked");
    for (const auto &representation :
         {QStringLiteral("source"), QStringLiteral("llvmc")})
      for (const bool second : {false, true})
        for (const bool locked : {false, true}) {
          const auto name = representation + (second ? "-second" : "-primary") +
                            (locked ? "-locked" : "-following");
          QTest::newRow(qPrintable(name)) << representation << second << locked;
        }
  }

  void pseudocodeFunctionLinksStayInTheirWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, secondWindow);
    QFETCH(bool, locked);
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("pseudocode-navigation.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    const Address caller = Base + 0x140;
    const Address callee =
        Base + (representation == QLatin1String("source") ? 0x160 : 0x150);
    disassembly->navigate(caller);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(caller));
    bench.action(secondWindow ? ActionId::ViewLowIR : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(secondWindow ? QStringLiteral("low")
                                             : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 1,
                             OpenTimeoutMs);
    QCOMPARE(code->function(), std::optional<Address>(caller));
    if (locked) {
      QToolButton *lock = nullptr;
      for (auto *button : view->findChildren<QToolButton *>())
        if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
          lock = button;
      QVERIFY(lock);
      lock->setChecked(true);
      QVERIFY(view->locked());
      // A pinned code view can differ from the background assembly location.
      disassembly->navigate(Base + 0x180);
      QTRY_COMPARE(disassembly->currentFunction(),
                   std::optional<Address>(Base + 0x180));
      QCOMPARE(code->function(), std::optional<Address>(caller));
    }
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    const QString link = representation == QLatin1String("source")
                             ? QStringLiteral("function_22")
                             : QStringLiteral("Bar_ctor");
    if (representation == QLatin1String("llvmc"))
      QVERIFY(code->findText(QStringLiteral("caller(void)"), true));
    QVERIFY(code->findText(link, true));
    QCOMPARE(code->currentToken(), link);
    QSignalSpy activation(code, &CodeText::nameActivated);
    // The call is row 1 in C, row 3 after LLVM C's folded prelude.
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const int row = representation == QLatin1String("source") ? 1 : 3;
    const QPoint point(
        qRound(6 + 17 * metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(code->currentToken(), link);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(activation.size(), 1);
    if (representation == QLatin1String("llvmc"))
      QCOMPARE(activation.first().first().toString(),
               QStringLiteral("_ZN3BarC1Ev"));
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(callee),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QCOMPARE(view->representation(), representation);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(callee));
    QVERIFY(bench.action(ActionId::JumpBack)->isEnabled());
    bench.action(ActionId::JumpBack)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(caller),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
    QCOMPARE(view->representation(), representation);
    QVERIFY(bench.action(ActionId::JumpForward)->isEnabled());
    bench.action(ActionId::JumpForward)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(code->function(), std::optional<Address>(callee),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
    QVERIFY(code->isVisible());
    QTRY_VERIFY(code->hasFocus());
  }

  void functionsActivationKeepsCodeWindow_data() {
    pseudocodeFunctionLinksStayInTheirWindow_data();
    QTest::newRow("low-secondary-following")
        << QStringLiteral("low") << true << false;
    QTest::newRow("rust-primary-following")
        << QStringLiteral("source") << false << false;
    QTest::newRow("go-secondary-following")
        << QStringLiteral("source") << true << false;
    QTest::newRow("c-rust-primary-locked")
        << QStringLiteral("c") << false << true;
    QTest::newRow("c-rust-secondary-following")
        << QStringLiteral("c") << true << false;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_CODE_NAV_FILE").isEmpty()) {
      QTest::newRow("native-c") << QStringLiteral("source") << false << false;
      QTest::newRow("native-llvmc") << QStringLiteral("llvmc") << true << false;
    }
  }

  void functionsActivationKeepsCodeWindow() {
    QFETCH(QString, representation);
    QFETCH(bool, secondWindow);
    QFETCH(bool, locked);
    const bool native =
        QByteArray(QTest::currentDataTag()).startsWith("native-");
    QTemporaryDir directory;
    const QByteArray tag = QTest::currentDataTag();
    const QString fixture = tag.startsWith("rust-") || tag.startsWith("c-rust-")
                                ? QStringLiteral("pseudocode-rust.bin")
                            : tag.startsWith("go-")
                                ? QStringLiteral("pseudocode-go.bin")
                                : QStringLiteral("fixture.bin");
    const auto path = native ? qEnvironmentVariable("NEVERD_CODE_NAV_FILE")
                             : writeFixture(directory, fixture);
    Workbench bench(native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                           : QString::fromLocal8Bit(TEST_WORKER));
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_VERIFY_WITH_TIMEOUT(!functions->model().rowObject(2).isEmpty(),
                             OpenTimeoutMs);
    const Address caller =
        native ? *functions->model().addressAt(0) : Base + 0x140;
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(caller);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(caller));
    bench.action(secondWindow ? ActionId::ViewLowIR : ActionId::ViewPseudocode)
        ->trigger();
    auto *view = bench.codeView(secondWindow ? QStringLiteral("low")
                                             : QStringLiteral("source"));
    QVERIFY(view);
    view->setRepresentation(representation);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 0,
                             OpenTimeoutMs);
    if (locked) {
      for (auto *button : view->findChildren<QToolButton *>())
        if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
          button->setChecked(true);
      QVERIFY(view->locked());
      disassembly->navigate(Base + 0x180);
      QTRY_COMPARE(disassembly->currentFunction(),
                   std::optional<Address>(Base + 0x180));
    }

    auto *table = functions->table();
    Address previous = caller;
    for (const bool keyboard : {false, true}) {
      const int row = native ? (keyboard ? 2 : 1) : (keyboard ? 23 : 22);
      const auto target = functions->model().addressAt(row);
      QVERIFY(target);
      code->setFocus();
      QTRY_VERIFY(code->hasFocus());
      const auto index = functions->model().index(row, 0);
      table->scrollTo(index);
      // A narrow chooser can clip the name column beside a second code view.
      const QRect cell =
          table->visualRect(index).intersected(table->viewport()->rect());
      QVERIFY(!cell.isEmpty());
      const QPoint point = cell.center();
      QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                        point);
      QTRY_VERIFY(table->hasFocus());
      QCOMPARE(table->currentIndex().row(), row);
      if (keyboard)
        QTest::keyClick(table, Qt::Key_Return);
      else
        QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier,
                           point);
      QVERIFY(code->isVisible());
      QTRY_COMPARE_WITH_TIMEOUT(code->function(), target, OpenTimeoutMs);
      QTRY_VERIFY_WITH_TIMEOUT(!code->loading(), OpenTimeoutMs);
      QVERIFY(code->lineCount() > 0);
      QCOMPARE(view->representation(), representation);
      QCOMPARE(view->locked(), locked);
      QTRY_VERIFY(code->hasFocus());
      QTRY_COMPARE(disassembly->currentFunction(), target);

      bench.action(ActionId::JumpBack)->trigger();
      QTRY_COMPARE_WITH_TIMEOUT(
          code->function(), std::optional<Address>(previous), OpenTimeoutMs);
      QTRY_VERIFY(!code->loading() && code->hasFocus());
      bench.action(ActionId::JumpForward)->trigger();
      QTRY_COMPARE_WITH_TIMEOUT(code->function(), target, OpenTimeoutMs);
      QTRY_VERIFY(!code->loading() && code->hasFocus());
      previous = *target;
    }
    if (native) {
      const auto capture = qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
      if (!capture.isEmpty()) {
        QDir().mkpath(capture);
        QVERIFY(bench.window->grab().save(QDir(capture).filePath(
            QString::fromLatin1(QTest::currentDataTag()) +
            QStringLiteral(".png"))));
      }
    }
  }

  void functionsActivationUsesDisassemblyWhenCodeIsNotCurrent_data() {
    QTest::addColumn<bool>("closed");
    QTest::newRow("disassembly-current") << false;
    QTest::newRow("code-closed") << true;
  }

  void functionsActivationUsesDisassemblyWhenCodeIsNotCurrent() {
    QFETCH(bool, closed);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    QTRY_VERIFY(disassembly->currentFunction().has_value());
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY(!view->text()->loading() && view->text()->isVisible());
    const auto originalFunction = view->text()->function();
    if (closed) {
      for (auto *dock :
           bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->widget() == view)
          dock->forceClose();
      QTRY_VERIFY(!view->isVisible());
    } else {
      // Users can still put the two views in one tab group. Their default
      // arrangement is now a split, so create that hidden-view case here.
      KDDockWidgets::QtWidgets::DockWidget *codeDock = nullptr,
                                           *listingDock = nullptr;
      for (auto *dock :
           bench.window
               ->findChildren<KDDockWidgets::QtWidgets::DockWidget *>()) {
        if (dock->widget() == view)
          codeDock = dock;
        if (dock->widget() == disassembly)
          listingDock = dock;
      }
      QVERIFY(codeDock && listingDock);
      listingDock->addDockWidgetAsTab(codeDock);
      bench.action(ActionId::ViewDisassembly)->trigger();
      disassembly->focusContent();
      QTRY_VERIFY(disassembly->isVisible());
      const auto revision = bench.session.revision();
      bench.session.rename(Base, QStringLiteral("renamed_while_hidden"));
      QTRY_VERIFY(bench.session.revision() != revision);
      QVERIFY(view->text()->interrupted());
    }
    auto *table = bench.functions()->table();
    QTRY_VERIFY(!bench.functions()->model().rowObject(22).isEmpty());
    table->setCurrentIndex(bench.functions()->model().index(22, 0));
    table->setFocus();
    QTRY_VERIFY(table->hasFocus());
    QTest::keyClick(table, Qt::Key_Return);
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x160));
    QVERIFY(disassembly->isVisible());
    QVERIFY(!view->isVisible());
    if (!closed) {
      // Showing the existing tab catches up to edits of its own function,
      // without following assembly navigation performed while it was hidden.
      for (auto *dock :
           bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
        if (dock->widget() == view)
          dock->raise();
      QTRY_COMPARE(view->text()->function(), originalFunction);
      QTRY_VERIFY(!view->text()->loading() && !view->text()->interrupted());
      disassembly->focusContent();
      bench.action(ActionId::JumpPseudocode)->trigger();
      QTRY_COMPARE(view->text()->function(),
                   std::optional<Address>(Base + 0x160));
    }
  }

  void changingFiltersRetiresObsoletePages() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto &model = bench.functions()->model();
    QTRY_COMPARE(model.total(), 600);
    QTRY_VERIFY(!bench.session.busy());
    QSignalSpy failures(&model, &ChooserModel::failed);

    // Keep the worker busy while successive filters replace the needed page.
    QObject blocker;
    bench.session.read(QStringLiteral("resolve"),
                       {{"query", "delayed_function"}}, &blocker,
                       [](const QJsonObject &) {});
    QTest::qWait(25);
    for (int i = 0; i < 96; ++i)
      model.setFilter(QStringLiteral("function_%1").arg(i));
    model.setFilter(QStringLiteral("function_"));
    QTest::qWait(50);
    QCOMPARE(failures.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), 600, OpenTimeoutMs);
    QTRY_COMPARE(model.rowObject(0).value("name").toString(),
                 QStringLiteral("function_0"));
  }

  void rapidlyScrollingLargeListsKeepsTheLastPageAvailable_data() {
    QTest::addColumn<QString>("worker");
    QTest::addColumn<QString>("binary");
    QTest::newRow("fixture")
        << QString::fromLocal8Bit(TEST_WORKER) << QString();
    const auto worker = qEnvironmentVariable("NEVERD_LARGE_PE_WORKER");
    const auto binary = qEnvironmentVariable("NEVERD_LARGE_PE_FILE");
    if (!worker.isEmpty() && !binary.isEmpty())
      QTest::newRow("native-pe") << worker << binary;
  }

  void rapidlyScrollingLargeListsKeepsTheLastPageAvailable() {
    QFETCH(QString, worker);
    QFETCH(QString, binary);
    QTemporaryDir directory;
    const bool fixture = binary.isEmpty();
    const auto path =
        fixture ? writeFixture(directory, QStringLiteral("many-functions.bin"))
                : binary;
    QVERIFY(!path.isEmpty());
    Workbench bench(worker);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto &model = bench.functions()->model();
    // The optional native profile includes background discovery over the
    // entire large image; ordinary fixture deadlines remain unchanged.
    const int listTimeout = fixture ? OpenTimeoutMs : NativeOpenTimeoutMs;
    if (!fixture)
      QTRY_VERIFY_WITH_TIMEOUT(
          bench.session.background().value("state").toString() == "building" ||
              bench.session.indexReady(),
          listTimeout);
    const int total =
        fixture ? 20000 : bench.session.background().value("functions").toInt();
    QVERIFY(total > 48 * 256);
    QTRY_COMPARE_WITH_TIMEOUT(model.total(), total, listTimeout);
    if (fixture)
      QTRY_VERIFY_WITH_TIMEOUT(!bench.session.busy(), OpenTimeoutMs);
    QSignalSpy failures(&model, &ChooserModel::failed);

    QObject blocker;
    if (fixture) {
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", "delayed_function"}}, &blocker,
                         [](const QJsonObject &) {});
      QTest::qWait(25);
    }
    // Successive viewport positions span more pages than the shared queue.
    for (int row = 256; row < model.total(); row += 256)
      model.data(model.index(row, 0), Qt::DisplayRole);
    const int last = model.total() - 1;
    QVERIFY(model.index(last, 0).isValid());
    model.data(model.index(last, 0), Qt::DisplayRole);
    QTest::qWait(50);
    QCOMPARE(failures.count(), 0);
    QTRY_VERIFY_WITH_TIMEOUT(!model.rowObject(last).isEmpty(), listTimeout);
    if (fixture)
      QCOMPARE(model.rowObject(last).value("name").toString(),
               QStringLiteral("function_19999"));
    else {
      // The real worker must also survive replacing expensive filters.
      for (int i = 0; i < 96; ++i)
        model.setFilter(QStringLiteral("sub_%1").arg(i));
      model.setFilter(QString());
      QTest::qWait(50);
      QCOMPARE(failures.count(), 0);
      QTRY_COMPARE_WITH_TIMEOUT(model.total(), total, listTimeout);
      QTRY_VERIFY_WITH_TIMEOUT(!model.rowObject(0).isEmpty(), listTimeout);
      if (const auto capture = qEnvironmentVariable("NEVERD_LARGE_PE_CAPTURE");
          !capture.isEmpty())
        QVERIFY(bench.window->grab().save(capture));
    }
  }

  void pseudocodeReadsInTheFunctionsLanguage() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-rust.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    // F5 shows a Rust function as Rust, and the window says so.
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() > 1,
                             OpenTimeoutMs);
    QCOMPARE(code->language(), QStringLiteral("rust"));
    QCOMPARE(view->chosenLanguage(), QStringLiteral("Rust"));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Rust)"));
    // A source name is one name wherever it is clicked, and resolves by the
    // symbol it reads.
    const QStringList lines = code->allText().split(QLatin1Char('\n'));
    int row = -1;
    for (int i = 0; i < lines.size() && row < 0; ++i)
      if (lines[i].contains(QStringLiteral("core::fmt::write(")))
        row = i;
    QVERIFY(row >= 0);
    const int column = int(lines[row].indexOf(QStringLiteral("fmt")));
    const auto name = code->sourceNameAt(row, column);
    QVERIFY(name);
    QCOMPARE(name->first, QStringLiteral("core::fmt::write"));
    QCOMPARE(name->second.identifier, QStringLiteral("core_fmt_write"));
    QCOMPARE(name->second.address, std::optional<Address>(Base + 0x160));
    QVERIFY(!code->sourceNameAt(row,
                                int(lines[row].indexOf(QStringLiteral("v0")))));
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const QPoint point(
        qRound(6 + (6 + column + 0.5) *
                       metrics.horizontalAdvance(QLatin1Char('M'))),
        qRound((row - code->verticalScrollBar()->value() + 0.5) *
               std::ceil(metrics.lineSpacing())));
    code->setFocus();
    QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QCOMPARE(names.size(), 1);
    QCOMPARE(names.first().first().toString(),
             QStringLiteral("_ZN4core3fmt5write17h0123456789abcdefE"));
    // A function the engine refuses reads in no language.
    bench.window->disassembly()->navigate(Base + 0x180);
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() &&
            code->status().contains(QStringLiteral("fixture refuses")),
        OpenTimeoutMs);
    QCOMPARE(code->language(), QStringLiteral("c"));
    QCOMPARE(view->chosenLanguage(), QString());
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A"));
    QVERIFY(!code->sourceNameAt(0, 0));
    bench.window->disassembly()->navigate(Base + 0x140);
    bench.window->disassembly()->focusContent();
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("rust"),
                             OpenTimeoutMs);
    // Pseudocode reads the Rust; C beside it reads every function as C.
    const QStringList pseudocodeAndC = {
        QStringLiteral("source"), QStringLiteral("c"),
        QStringLiteral("llvmc"),  QStringLiteral("low"),
        QStringLiteral("med"),    QStringLiteral("high"),
        QStringLiteral("llvm")};
    QCOMPARE(representationsOf(view), pseudocodeAndC);
    view->setRepresentation(QStringLiteral("c"));
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("c"),
                             OpenTimeoutMs);
    QCOMPARE(view->chosenLanguage(), QString());
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("C-A"));
    // A language the menu does not offer leaves the view as it is.
    view->setRepresentation(QStringLiteral("go"));
    QCOMPARE(view->representation(), QStringLiteral("c"));
    view->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("rust"),
                             OpenTimeoutMs);
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Rust)"));
  }

  void llvmIrNamesGoWhereTheyStandFor() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("llvm-view.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    for (auto *button : view->findChildren<QToolButton *>())
      if (button->toolTip().startsWith(QStringLiteral("Keep this function")))
        button->setChecked(true);
    view->setRepresentation(QStringLiteral("llvm"));
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(
        !code->loading() && code->representation() == QLatin1String("llvm") &&
            code->hasPrelude(),
        OpenTimeoutMs);
    // The function shows alone: its declarations fold before it, its
    // attribute groups and metadata after it.
    QVERIFY(code->preludeFolded());
    QVERIFY(code->hasEpilogue() && code->epilogueFolded());
    QCOMPARE(code->lineCount(), 14);
    const QStringList source = code->allText().split(QLatin1Char('\n'));
    QCOMPARE(source.size(), 29);
    QSignalSpy messages(&bench.session, &Session::message);
    QSignalSpy names(code, &CodeText::nameActivated);
    const QFontMetricsF metrics(Theme::instance().codeFont());
    const qreal width = metrics.horizontalAdvance(QLatin1Char('M'));
    // Double-click occurrence \p occurrence of \p token on displayed line
    // \p line, which shows source line \p sourceLine, inside its second
    // character.
    const auto doubleClick = [&](int line, int sourceLine, const QString &token,
                                 int occurrence = 0) {
      int column = -1;
      for (int i = 0; i <= occurrence; ++i)
        column = int(source[sourceLine].indexOf(token, column + 1));
      QVERIFY2(column >= 0, qPrintable(token));
      code->setCursorLine(line);
      code->horizontalScrollBar()->setValue(0);
      const QPoint point(
          qRound(6 + int(5 * width) + (column + 1.5) * width),
          qRound((line - code->verticalScrollBar()->value() + 0.5) *
                 std::ceil(metrics.lineSpacing())));
      code->setFocus();
      QTest::mouseClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                        point);
      QTest::mouseDClick(code->viewport(), Qt::LeftButton, Qt::NoModifier,
                         point);
    };
    // Metadata the definition names opens the epilogue at its node.
    doubleClick(2, 14, QStringLiteral("!0"));
    QVERIFY(!code->epilogueFolded());
    QCOMPARE(code->cursorLine(), 28 - 12);
    // A global the binary does not name shows at its declaration.
    doubleClick(7, 19, QStringLiteral("@llvm.example"));
    QCOMPARE(names.size(), 1);
    QCOMPARE(names.last().first().toString(), QStringLiteral("llvm.example"));
    QTRY_VERIFY(!code->preludeFolded());
    QTRY_COMPARE(code->cursorLine(), 11);
    // Locals, blocks, parameters and attribute groups open where defined.
    doubleClick(18, 18, QStringLiteral("%slot"));
    QCOMPARE(code->cursorLine(), 16);
    doubleClick(20, 20, QStringLiteral("%done"));
    QCOMPARE(code->cursorLine(), 22);
    doubleClick(18, 18, QStringLiteral("%arg0"));
    QCOMPARE(code->cursorLine(), 14);
    doubleClick(14, 14, QStringLiteral("#0"));
    QCOMPARE(code->cursorLine(), 26);
    QCOMPARE(names.size(), 1);
    // A comment's address resolves as it reads.
    doubleClick(16, 16, QStringLiteral("0xFFFF800012343008"));
    QCOMPARE(names.size(), 2);
    QCOMPARE(names.last().first().toString(),
             QStringLiteral("0xFFFF800012343008"));
    // Hovering a mirror tells what it is: the comment over its declaration,
    // the declaration, and where this line's spelling goes.
    const int load = int(source[16].indexOf(QStringLiteral("@__nd_codeptr_")));
    const QString hover = code->globalHoverText(16, load + 2);
    QVERIFY2(
        hover.startsWith(QStringLiteral(
            "NeverD's mirror of the 64 image bytes at 0xFFFF800012343000")),
        qPrintable(hover));
    QVERIFY2(hover.contains(
                 QStringLiteral("\n  +8 = 0xFFFF800012343008 data_slot\n")),
             qPrintable(hover));
    QVERIFY2(hover.contains(QStringLiteral(
                 "\n@__nd_codeptr_FFFF800012343000 = external global")),
             qPrintable(hover));
    QVERIFY2(hover.endsWith(
                 QStringLiteral("Double-click to go to 0xFFFF800012343008.")),
             qPrintable(hover));
    QVERIFY(code->globalHoverText(
                    18, int(source[18].indexOf(QStringLiteral("%slot"))))
                .isEmpty());
    // An image mirror stands for its first byte where declared, and for the
    // byte the load's constant offset reaches on the load.
    doubleClick(9, 9, QStringLiteral("@__nd_codeptr_"));
    QCOMPARE(names.size(), 3);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3000));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3000));
    // A mirror a line reaches at two offsets: each spelling, its own.
    doubleClick(17, 17, QStringLiteral("@__nd_codeptr_"), 1);
    QCOMPARE(names.size(), 4);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3018));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3018));
    doubleClick(16, 16, QStringLiteral("@__nd_codeptr_"));
    QCOMPARE(names.size(), 5);
    QCOMPARE(names.last().first().toString(), hexAddress(Base + 0x3008));
    QTRY_COMPARE(bench.window->disassembly()->currentAddress(),
                 std::optional<Address>(Base + 0x3008));
    QCOMPARE(code->function(), std::optional<Address>(Base + 0x140));
    for (const auto &row : messages)
      QVERIFY(
          !row.first().toString().startsWith(QStringLiteral("Cannot jump to")));
  }

  void cppPseudocodeRetainsASeparateCChoice() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-cpp.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() ==
                                     QLatin1String("cpp"),
                             OpenTimeoutMs);
    QCOMPARE(view->chosenLanguage(), QStringLiteral("C++"));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (C++)"));
    QVERIFY(view->text()->allText().contains(QStringLiteral("std::string")));
    QVERIFY(representationsOf(view).contains(QStringLiteral("c")));
    view->setRepresentation(QStringLiteral("c"));
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() == QLatin1String("c"),
                             OpenTimeoutMs);
    QVERIFY(!view->text()->allText().contains(QStringLiteral("std::string")));
    view->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                 view->text()->language() ==
                                     QLatin1String("cpp"),
                             OpenTimeoutMs);
  }

  void pseudocodeOffersOnlyTheProgramsLanguages() {
    QTemporaryDir directory;
    const QStringList cOnly = {
        QStringLiteral("source"), QStringLiteral("llvmc"),
        QStringLiteral("low"),    QStringLiteral("med"),
        QStringLiteral("high"),   QStringLiteral("llvm")};
    {
      // A C program's pseudocode is C: no other language is offered.
      Workbench bench;
      bench.window->openFile(
          writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                   std::optional<Address>(Base + 0x140));
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      QTRY_VERIFY_WITH_TIMEOUT(!view->text()->loading() &&
                                   view->text()->lineCount() > 1,
                               OpenTimeoutMs);
      QCOMPARE(representationsOf(view), cOnly);
      QCOMPARE(view->text()->language(), QStringLiteral("c"));
      QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
               QStringLiteral("Pseudocode-A"));
    }
    // A Go program reads in Go, says so, and counts what it shows as C.
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-go.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->language() == QLatin1String("go"),
                             OpenTimeoutMs);
    QCOMPARE(representationsOf(view),
             QStringList({QStringLiteral("source"), QStringLiteral("c"),
                          QStringLiteral("llvmc"), QStringLiteral("low"),
                          QStringLiteral("med"), QStringLiteral("high"),
                          QStringLiteral("llvm")}));
    QVERIFY(code->allText().contains(QStringLiteral("func main.main()")));
    QVERIFY(code->status().contains(QStringLiteral("shown as C")));
    QTRY_COMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
                 QStringLiteral("Pseudocode-A (Go)"));
  }

  void unknownEntryBrowsesMappedCodeAndShowsDiagnostics() {
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("unknown-entry.bin"));
    Workbench bench;
    QSignalSpy messages(&bench.session, &Session::message);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.session.entryAddress(), Address(0));
    QTRY_VERIFY_WITH_TIMEOUT(
        bench.window->disassembly()->currentAddress().has_value(),
        OpenTimeoutMs);
    QVERIFY(*bench.window->disassembly()->currentAddress() >= Base);
    QVERIFY(*bench.window->disassembly()->currentAddress() < Base + 0x3000);
    bool warned = false;
    for (const auto &message : messages)
      warned |= message.first().toString() ==
                QLatin1String("Fixture PE entry is unknown.");
    QVERIFY(warned);
    QCOMPARE(bench.session.entryAddress(), Address(0));
  }

  void fileDropOpensFromWorkbenchViews_data() {
    QTest::addColumn<QString>("targetName");
    for (const auto *name : {"window", "listing", "functions", "hex", "code",
                             "output", "command", "floating"})
      QTest::newRow(name) << QString::fromLatin1(name);
  }

  void fileDropOpensFromWorkbenchViews() {
    QFETCH(QString, targetName);
    QTemporaryDir directory;
    const auto path = writeFixture(
        directory, QString::fromUtf8("\u4e2d\u6587 space #% fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    QWidget *target = bench.window.get();
    if (targetName == QLatin1String("listing"))
      target = bench.window->disassembly()->listing()->viewport();
    else if (targetName == QLatin1String("functions"))
      target = bench.functions()->table()->viewport();
    else if (targetName == QLatin1String("hex"))
      target = bench.window->findChild<HexView *>()->viewport();
    else if (targetName == QLatin1String("code")) {
      // Code docks created after startup must accept file drops too.
      bench.window->openFile(writeFixture(directory, "first.bin"));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_VERIFY_WITH_TIMEOUT(
          bench.window->disassembly()->currentFunction().has_value(),
          OpenTimeoutMs);
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      target = view->text()->viewport();
    } else if (targetName == QLatin1String("output"))
      target = bench.window->findChild<OutputWindow *>()
                   ->findChild<QPlainTextEdit *>()
                   ->viewport();
    else if (targetName == QLatin1String("command"))
      target =
          bench.window->findChild<OutputWindow *>()->findChild<QLineEdit *>();
    else if (targetName == QLatin1String("floating")) {
      auto *dock = qobject_cast<KDDockWidgets::QtWidgets::DockWidget *>(
          bench.functions()->parentWidget());
      QVERIFY(dock);
      dock->setFloating(true);
      target = bench.functions()->table()->viewport();
      QVERIFY(target->window() != bench.window.get());
    }
    QVERIFY(target);
    // A new file asks how to load it first.
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.identifiesFiles(), OpenTimeoutMs);
    LoadDialogAcceptor loadDialogs;
    QVERIFY(dropFile(target, path));
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(loadDialogs.accepted(), 1);
    QVERIFY(QFileInfo::exists(path));
    if (targetName == QLatin1String("command"))
      QVERIFY(static_cast<QLineEdit *>(target)->text().isEmpty());
  }

  void fileDropRejectsInvalidInputs() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    const QList<QList<QUrl>> rejected{
        {},
        {QUrl(QStringLiteral("https://example.com/fixture.bin"))},
        {QUrl::fromLocalFile(directory.path())},
        {QUrl::fromLocalFile(directory.filePath("missing.bin"))},
        {QUrl::fromLocalFile(path), QUrl::fromLocalFile(path)},
        {QUrl(QStringLiteral("file:relative.bin"))}};
    for (const auto &urls : rejected) {
      QMimeData mime;
      mime.setUrls(urls);
      QDragEnterEvent enter(QPoint(10, 10), Qt::CopyAction, &mime,
                            Qt::LeftButton, Qt::NoModifier);
      QApplication::sendEvent(bench.window.get(), &enter);
      QVERIFY(!enter.isAccepted());
    }
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(path)});
    QDragEnterEvent moveOnly(QPoint(10, 10), Qt::MoveAction, &mime,
                             Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &moveOnly);
    QVERIFY(!moveOnly.isAccepted());
    QMimeData text;
    text.setText(path);
    QDragEnterEvent plainText(QPoint(10, 10), Qt::CopyAction, &text,
                              Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(bench.window.get(), &plainText);
    QVERIFY(!plainText.isAccepted());
    QVERIFY(bench.session.filePath().isEmpty());
  }

  void startupQuickStartDoesNotWaitForExposureOrWorker_data() {
    QTest::addColumn<bool>("go");
    QTest::newRow("close") << false;
    QTest::newRow("go") << true;
  }

  void startupQuickStartDoesNotWaitForExposureOrWorker() {
    QFETCH(bool, go);
    QTemporaryDir directory;
    Workbench bench(directory.filePath(QStringLiteral("missing-worker.exe")));
    // The initial exposure has already happened, and no engine can become
    // ready.
    QVERIFY(QTest::qWaitForWindowExposed(bench.window.get()));
    int shown = 0;
    bool clickedGo = false;
    QTimer close;
    connect(&close, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<QuickStartDialog *>(QApplication::activeModalWidget());
      if (!dialog)
        return;
      ++shown;
      if (go)
        for (auto *button : dialog->findChildren<QPushButton *>())
          if (button->text() == QStringLiteral("&Go")) {
            clickedGo = true;
            QTest::mouseClick(button, Qt::LeftButton);
            return;
          }
      dialog->reject();
    });
    close.start(5);
    bench.window->scheduleQuickStart();
    QTRY_COMPARE_WITH_TIMEOUT(shown, 1, 1000);
    QCOMPARE(clickedGo, go);
    QVERIFY(bench.window->isVisible());
    QVERIFY(bench.session.filePath().isEmpty());
    QVERIFY(!bench.session.loaded());
    QVERIFY(!QApplication::activeModalWidget());
    bench.window->hide();
    bench.window->show();
    QTest::qWait(50);
    QCOMPARE(shown, 1);
  }

  void openingAFileCancelsStartupQuickStart_data() {
    QTest::addColumn<QString>("mode");
    QTest::newRow("before-worker-ready") << QStringLiteral("open");
    QTest::newRow("drop-before-queued-dialog") << QStringLiteral("drop");
    QTest::newRow("already-loaded") << QStringLiteral("loaded");
  }

  void openingAFileCancelsStartupQuickStart() {
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    if (mode == QLatin1String("drop"))
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.identifiesFiles(), OpenTimeoutMs);
    if (mode == QLatin1String("loaded")) {
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    }
    int shown = 0;
    QTimer dismiss;
    connect(&dismiss, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<QuickStartDialog *>(
              QApplication::activeModalWidget())) {
        ++shown;
        dialog->reject();
      }
    });
    dismiss.start(5);
    LoadDialogAcceptor loadDialogs;
    bench.window->scheduleQuickStart();
    if (mode == QLatin1String("drop"))
      QVERIFY(dropFile(bench.window.get(), path));
    else if (mode == QLatin1String("open"))
      bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // Process the startup callback even when the session was already loaded.
    QTest::qWait(50);
    QCOMPARE(shown, 0);
    QCOMPARE(bench.session.filePath(), path);
    // Explicitly opening Quick Start from the menu still works in a project.
    QTimer::singleShot(0, bench.window.get(), [&] {
      if (auto *dialog = qobject_cast<QuickStartDialog *>(
              QApplication::activeModalWidget())) {
        ++shown;
        dialog->reject();
      }
    });
    bench.window->showQuickStart();
    QCOMPARE(shown, 1);
    QVERIFY(bench.session.loaded());
  }

  void startupQuickStartGreetsAlone_data() {
    QTest::addColumn<QString>("choice");
    QTest::addColumn<QString>("button");
    QTest::newRow("go") << QStringLiteral("go") << QStringLiteral("&Go");
    QTest::newRow("escape") << QStringLiteral("escape") << QString();
    QTest::newRow("new") << QStringLiteral("new") << QStringLiteral("&New");
    QTest::newRow("previous")
        << QStringLiteral("previous") << QStringLiteral("&Previous");
    QTest::newRow("drop") << QStringLiteral("drop") << QString();
    QTest::newRow("close") << QStringLiteral("close") << QString();
  }

  void startupQuickStartGreetsAlone() {
    // As IDA's: a startup without a file shows the quick start alone, as a
    // window of its own.  The workbench lays out its desktop and shows once
    // the user chooses how to start; closing the quick start quits instead,
    // without ever showing it.
    QFETCH(QString, choice);
    QFETCH(QString, button);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QSettings().setValue(settings::RecentFiles, QStringList{path});
    const auto restore = qScopeGuard([] {
      QSettings().remove(settings::RecentFiles);
      QSettings().remove(settings::RecentOpened);
    });
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    LoadDialogAcceptor loadDialogs;
    // New asks for the file in a dialog over the quick start.
    QTimer files;
    connect(&files, &QTimer::timeout, this, [&] {
      if (auto *dialog =
              qobject_cast<QFileDialog *>(QApplication::activeModalWidget())) {
        dialog->selectFile(path);
        // As pressing Open does; QFileDialog keeps its override protected.
        static_cast<QDialog *>(dialog)->accept();
      }
    });
    files.start(10);
    bool answered = false, alone = false, dropped = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<QuickStartDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (!button.isEmpty()) {
        if (auto *start = buttonNamed(dialog, button))
          start->click();
      } else if (choice == QLatin1String("escape")) {
        QTest::keyClick(dialog, Qt::Key_Escape);
      } else if (choice == QLatin1String("drop")) {
        // The drop opens the file on the next event-loop turn.
        dropped = dropFile(dialog, path);
      }
      // Close, or end a choice that failed to answer.
      if (!dropped && dialog->isVisible())
        dialog->close();
    });
    answer.start(5);
    bench.window->scheduleQuickStart();
    QTRY_VERIFY_WITH_TIMEOUT(answered, 1000);
    QVERIFY(alone);
    QVERIFY(!QApplication::activeModalWidget());
    if (choice == QLatin1String("close")) {
      QVERIFY(!bench.window->isVisible());
      return;
    }
    // A chosen file shows the workbench as it starts to load.
    QTRY_VERIFY_WITH_TIMEOUT(bench.window->isVisible(), OpenTimeoutMs);
    QVERIFY(bench.functions() && bench.functions()->isVisible());
    if (choice == QLatin1String("go") || choice == QLatin1String("escape")) {
      QVERIFY(bench.session.filePath().isEmpty());
      return;
    }
    QCOMPARE(dropped, choice == QLatin1String("drop"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(QFileInfo(bench.session.filePath()).canonicalFilePath(),
             QFileInfo(path).canonicalFilePath());
  }

  void pdbQuestionAtStartupGreetsAlone() {
    // As the quick start: a PE named at startup asks whether to load the PDB
    // it names in a window of its own, and the workbench shows once the
    // user answered, as the file starts to load.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-missing.bin"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<PdbPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
        no->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    QCOMPARE(bench.session.metadata()
                 .value("pdb")
                 .toObject()
                 .value("mode")
                 .toString(),
             QStringLiteral("none"));
  }

  void unattendedRunsAskNoPdbQuestion() {
    // A run nobody answers, such as a capture, loads as discovery finds.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-missing.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<PdbPromptDialog *>(QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // Nor once the file lacks its PDB, which an answered run would offer.
    QTest::qWait(OfferWaitMs);
    QVERIFY(!asked);
  }

  void dwarfQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<bool>("accept");
    QTest::addColumn<QString>("kind");
    QTest::newRow("embedded, yes") << QStringLiteral("dwarf-embedded.bin")
                                   << true << QStringLiteral("dwarf");
    QTest::newRow("embedded, no") << QStringLiteral("dwarf-embedded.bin")
                                  << false << QStringLiteral("none");
    QTest::newRow("split, no")
        << QStringLiteral("dwarf-split.bin") << false << QStringLiteral("none");
  }

  void dwarfQuestionAtStartupGreetsAlone() {
    // As IDA asks "Load DWARF debug information?": an image with DWARF asks
    // in a window of its own before it loads, names the file that holds the
    // DWARF when that is not the input, and No opens it without the DWARF.
    QFETCH(QString, fixture);
    QFETCH(bool, accept);
    QFETCH(QString, kind);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, fixture);
    const bool split = fixture == QLatin1String("dwarf-split.bin");
    if (split)
      QVERIFY(!writeFixture(directory, fixture + QStringLiteral(".debug"))
                   .isEmpty());
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false, named = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<DwarfPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      named = dialog->findChild<QLabel *>(QStringLiteral("path")) != nullptr;
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(named, split);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("debug_info").toObject().value("kind").toString(),
             kind);
    QCOMPARE(metadata.value("dwarf").toObject().value("mode").toString(),
             accept ? QString() : QStringLiteral("none"));
  }

  void unattendedRunsAskNoDwarfQuestion() {
    // A capture or benchmark reads the DWARF that discovery finds.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("dwarf-embedded.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<DwarfPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("debug_info")
                 .toObject()
                 .value("kind")
                 .toString(),
             QStringLiteral("dwarf"));
  }

  void declinedDwarfStaysDeclinedInTheProject() {
    // The answer is kept with the project, as IDA keeps it in its database:
    // reopening the project asks nothing and reads no DWARF.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("dwarf-embedded.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<DwarfPromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("dwarf").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<DwarfPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("dwarf").toObject().value("mode").toString(),
             QStringLiteral("none"));
    QVERIFY(metadata.value("debug_info").toObject().value("kind").toString() !=
            QLatin1String("dwarf"));
  }

  /// Write \p text as the MAP beside the fixture \p fixture in \p directory.
  static bool writeMapBeside(const QTemporaryDir &directory,
                             const QString &fixture, const QByteArray &text) {
    QFile map(directory.filePath(QFileInfo(fixture).completeBaseName() +
                                 QStringLiteral(".map")));
    return map.open(QIODevice::WriteOnly) && map.write(text) == text.size();
  }

  void mapQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<QByteArray>("map");
    QTest::addColumn<bool>("accept");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<QString>("mode");
    QTest::newRow("same build, yes") << QByteArray("same build") << true
                                     << QStringLiteral("map") << QString();
    QTest::newRow("same build, no")
        << QByteArray("same build") << false << QStringLiteral("none")
        << QStringLiteral("none");
    QTest::newRow("other build, yes")
        << QByteArray("other build") << true << QStringLiteral("map")
        << QStringLiteral("unchecked");
  }

  void mapQuestionAtStartupGreetsAlone() {
    // An image no PDB or DWARF describes asks before it loads the MAP beside
    // it, in a window of its own that says which linker wrote the MAP and
    // whether it is of the input's build; one of another build defaults to
    // No, and Yes still loads it, as the user chose.
    QFETCH(QByteArray, map);
    QFETCH(bool, accept);
    QFETCH(QString, kind);
    QFETCH(QString, mode);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), map));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false, cautious = false;
    QString description;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog =
          qobject_cast<MapPromptDialog *>(QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *label =
              dialog->findChild<QLabel *>(QStringLiteral("description")))
        description = label->text();
      if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
        cautious = no->isDefault();
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(cautious, map == "other build");
    QVERIFY2(description.startsWith(QLatin1String("link.exe MAP, 3 names.")),
             qPrintable(description));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("debug_info").toObject().value("kind").toString(),
             kind);
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(), mode);
  }

  void unattendedRunsAskNoMapQuestion() {
    // A capture or benchmark reads the MAP that discovery finds.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), "same build"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<MapPromptDialog *>(QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("debug_info")
                 .toObject()
                 .value("kind")
                 .toString(),
             QStringLiteral("map"));
  }

  void anAcceptedPdbLeavesTheMapUnasked() {
    // A PDB outranks the MAP beside the PE: once the user takes the PDB,
    // the MAP is not asked about, and the open reads no MAP before the PDB.
    QTemporaryDir directory;
    const auto path =
        writeFixture(directory, QStringLiteral("symbols-local.bin"));
    QFile pdb(directory.filePath(QStringLiteral("fixture.pdb")));
    QVERIFY(pdb.open(QIODevice::WriteOnly));
    pdb.write("matching fixture PDB");
    pdb.close();
    QVERIFY(writeMapBeside(directory, QStringLiteral("symbols-local.bin"),
                           "same build"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answeredPdb = false, askedMap = false;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *modal = QApplication::activeModalWidget();
      if (auto *dialog = qobject_cast<PdbPromptDialog *>(modal)) {
        answeredPdb = true;
        if (auto *yes = dialog->findChild<QPushButton *>(QStringLiteral("yes")))
          yes->click();
      } else if (qobject_cast<MapPromptDialog *>(modal)) {
        askedMap = true;
        modal->close();
      }
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(answeredPdb);
    QVERIFY(!askedMap);
    const auto debug = bench.session.metadata().value("debug_info").toObject();
    QCOMPARE(debug.value("kind").toString(), QStringLiteral("pdb"));
    QVERIFY(debug.value("files").toArray().isEmpty());
  }

  void declinedMapStaysDeclinedInTheProject() {
    // The project keeps the answer, as it keeps the DWARF one: reopening it
    // asks nothing and reads no MAP.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QVERIFY(writeMapBeside(directory, QStringLiteral("map.bin"), "same build"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<MapPromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("map").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<MapPromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(),
             QStringLiteral("none"));
    QVERIFY(metadata.value("debug_info").toObject().value("kind").toString() !=
            QLatin1String("map"));
  }

  void loadFileOffersTheDebugFilesTheImageCanHave() {
    // File, Load file offers a PDB for a PE, DWARF for an ELF or Mach-O
    // image, and a MAP for any image; a chosen MAP loads beside the image's
    // other debug information.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("map.bin"));
    QFile map(directory.filePath(QStringLiteral("chosen.map")));
    QVERIFY(map.open(QIODevice::WriteOnly));
    map.write("same build");
    map.close();
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *loadMap = bench.action(ActionId::FileLoadMap);
    auto *loadDwarf = bench.action(ActionId::FileLoadDwarf);
    QVERIFY(loadMap && loadDwarf);
    QTRY_VERIFY(loadMap->isEnabled());
    QCOMPARE(loadDwarf->isEnabled(),
             bench.session.format() == QLatin1String("ELF") ||
                 bench.session.format() == QLatin1String("Mach-O"));
    QSignalSpy changed(&bench.session, &Session::functionsChanged);
    bench.session.loadMap({{"path", map.fileName()}, {"mode", "checked"}});
    QTRY_COMPARE_WITH_TIMEOUT(changed.size(), 1, OpenTimeoutMs);
    const auto metadata = bench.session.metadata();
    QCOMPARE(metadata.value("map").toObject().value("mode").toString(),
             QStringLiteral("checked"));
    const auto files =
        metadata.value("debug_info").toObject().value("files").toArray();
    QCOMPARE(files.size(), 1);
    QCOMPARE(files.first().toObject().value("kind").toString(),
             QStringLiteral("map"));
  }

  void symbolQuestionAtStartupGreetsAlone_data() {
    QTest::addColumn<bool>("accept");
    QTest::newRow("yes") << true;
    QTest::newRow("no") << false;
  }

  void symbolQuestionAtStartupGreetsAlone() {
    // Anyone can write names into a symbol table: an image with one asks
    // before it loads, in a window of its own that shows the first names it
    // holds, and No reads the image as if stripped, its functions unnamed.
    QFETCH(bool, accept);
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
    bool answered = false, alone = false;
    QString shown;
    QTimer answer;
    connect(&answer, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
          QApplication::activeModalWidget());
      if (!dialog || answered)
        return;
      answered = true;
      alone = dialog->isVisible() && !dialog->parentWidget() &&
              !bench.window->isVisible();
      if (auto *names = dialog->findChild<QLabel *>(QStringLiteral("names")))
        shown = names->toolTip();
      if (auto *button = dialog->findChild<QPushButton *>(
              accept ? QStringLiteral("yes") : QStringLiteral("no")))
        button->click();
    });
    answer.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(answered, OpenTimeoutMs);
    QVERIFY(alone);
    QCOMPARE(shown, QStringLiteral("function_0\nfunction_1\nfunction_2"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.window->isVisible());
    QCOMPARE(bench.session.metadata()
                 .value("symbols")
                 .toObject()
                 .value("mode")
                 .toString(),
             accept ? QString() : QStringLiteral("none"));
    // The function after the entry, which the analysis names "start" when
    // nothing else does, has only the table's name.
    auto &model = bench.functions()->model();
    QTRY_VERIFY(!model.rowObject(1).isEmpty());
    const QString second = model.rowObject(1).value("name").toString();
    QVERIFY2(accept ? second == QLatin1String("function_1")
                    : second.startsWith(QLatin1String("sub_")),
             qPrintable(second));
  }

  void unattendedRunsAskNoSymbolQuestion() {
    // A capture or benchmark reads the image's symbol table.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    Workbench bench;
    bench.window->setAsksForDebugInfo(false);
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (qobject_cast<SymbolTablePromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        QApplication::activeModalWidget()->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QVERIFY(bench.session.metadata().value("symbols").isNull());
  }

  void declinedSymbolTableStaysDeclinedInTheProject() {
    // The project keeps the answer, as it keeps the DWARF one: reopening it
    // asks nothing and still reads the image as if stripped.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("symtab.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench(QString::fromLocal8Bit(TEST_WORKER), false);
      QTimer answer;
      connect(&answer, &QTimer::timeout, this, [&] {
        if (auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
                QApplication::activeModalWidget()))
          if (auto *no = dialog->findChild<QPushButton *>(QStringLiteral("no")))
            no->click();
      });
      answer.start(5);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      answer.stop();
      // The answer is the project's as soon as it is given, in a file
      // beside the binary rather than a database.
      const auto debug =
          QJsonDocument::fromJson(readAll(path + ".neverd/debug.json"))
              .object();
      QCOMPARE(debug.value("symbols").toObject().value("mode").toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    QVERIFY(!QFileInfo::exists(database));
    Workbench bench;
    bool asked = false;
    QTimer watch;
    connect(&watch, &QTimer::timeout, this, [&] {
      if (auto *dialog = qobject_cast<SymbolTablePromptDialog *>(
              QApplication::activeModalWidget())) {
        asked = true;
        dialog->close();
      }
    });
    watch.start(5);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!asked);
    QCOMPARE(bench.session.metadata()
                 .value("symbols")
                 .toObject()
                 .value("mode")
                 .toString(),
             QStringLiteral("none"));
  }

  void quickStartShowsRecentFilesAndStartsAsChosen() {
    // As IDA's Quick start: New, Go and Previous, the recent files and
    // whether it greets the next start.  Each recent file shows its format,
    // name, folder and when it was last opened.
    QTemporaryDir directory;
    const QString elf = directory.filePath(QStringLiteral("program"));
    {
      QFile file(elf);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(QByteArray("\x7F"
                            "ELF\x02\x01\x01\0",
                            8));
    }
    const QString missing = directory.filePath(QStringLiteral("gone.bin"));
    QSettings().setValue(settings::RecentFiles, QStringList{elf, missing});
    QSettings().setValue(settings::RecentOpened,
                         QVariantMap{{elf, QDateTime::currentDateTime()}});
    const auto restore = qScopeGuard([] {
      QSettings().remove(settings::RecentFiles);
      QSettings().remove(settings::RecentOpened);
      QSettings().remove(settings::QuickStart);
      QSettings().remove(settings::QuickStartSize);
    });

    QuickStartDialog dialog(noFile);
    auto *recent =
        dialog.findChild<QListWidget *>(QStringLiteral("quickStartRecent"));
    QVERIFY(recent);
    QCOMPARE(recent->count(), 2);
    QCOMPARE(recent->item(0)->text(), QStringLiteral("program"));
    QCOMPARE(recent->item(0)->data(Qt::UserRole + 1).toString(),
             QStringLiteral("ELF"));
    QVERIFY(recent->item(0)
                ->data(Qt::UserRole + 3)
                .toString()
                .startsWith(QStringLiteral("Today, ")));
    QCOMPARE(recent->item(1)->data(Qt::UserRole + 3).toString(),
             QStringLiteral("Missing"));

    // Delete forgets the selected file.
    recent->setCurrentRow(1);
    QTest::keyClick(recent, Qt::Key_Delete);
    QCOMPARE(recent->count(), 1);
    QCOMPARE(QSettings().value(settings::RecentFiles).toStringList(),
             QStringList{elf});

    // Display at startup is the setting the next start reads.
    auto *atStartup = dialog.findChild<QCheckBox *>();
    QVERIFY(atStartup && atStartup->isChecked());
    atStartup->setChecked(false);
    QVERIFY(!QSettings().value(settings::QuickStart, true).toBool());

    // Previous loads the selected file.
    QPushButton *previous = nullptr;
    for (auto *button : dialog.findChildren<QPushButton *>())
      if (button->text() == QStringLiteral("&Previous"))
        previous = button;
    QVERIFY(previous && previous->isEnabled() && previous->isDefault());
    recent->setCurrentRow(0);
    previous->click();
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.start(), QuickStartDialog::Start::Previous);
    QCOMPARE(dialog.file(), elf);
  }

  void quickStartKeysFollowTheTranslatedAccessKeys() {
    // A translation moves a button's access key, and the key that presses
    // it without Alt and its key cap move with it: French "&Continuer"
    // takes C, and G no longer works on one's own.
    class French final : public QTranslator {
    public:
      bool isEmpty() const override { return false; }
      QString translate(const char *context, const char *source, const char *,
                        int) const override {
        if (qstrcmp(context, "neverd::gui::QuickStartDialog") == 0 &&
            qstrcmp(source, "&Go") == 0)
          return QStringLiteral("&Continuer");
        return {};
      }
    } french;
    QCoreApplication::installTranslator(&french);
    const auto restore =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&french); });
    const auto pick = [](QWidget *) { return QString(); };

    QuickStartDialog ignored(pick);
    ignored.show();
    QVERIFY(QTest::qWaitForWindowExposed(&ignored));
    QTest::keyClick(&ignored, Qt::Key_G);
    QVERIFY(ignored.isVisible());
    QCOMPARE(ignored.start(), QuickStartDialog::Start::None);

    QuickStartDialog dialog(pick);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY(buttonNamed(&dialog, QStringLiteral("&Continuer")));
    QTest::keyClick(&dialog, Qt::Key_C);
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.start(), QuickStartDialog::Start::Go);
  }

  void quickStartWidensForALongerTranslation() {
    // A translation longer than English, here the longest catalog's, widens
    // the side rather than cut the action's description off.
    class German final : public QTranslator {
    public:
      bool isEmpty() const override { return false; }
      QString translate(const char *context, const char *source, const char *,
                        int) const override {
        if (qstrcmp(context, "neverd::gui::QuickStartDialog") == 0 &&
            qstrcmp(source, "Load the selected recent file") == 0)
          return QStringLiteral("Ausgewählte zuletzt verwendete Datei laden");
        return {};
      }
    } german;
    QCoreApplication::installTranslator(&german);
    const auto restore =
        qScopeGuard([&] { QCoreApplication::removeTranslator(&german); });
    QuickStartDialog dialog([](QWidget *) { return QString(); });
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    // Even at its smallest the dialog keeps the side's width.
    dialog.resize(dialog.minimumSize());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    const auto actions =
        dialog.findChildren<QPushButton *>(QStringLiteral("quickStartAction"));
    QCOMPARE(actions.size(), 3);
    for (const auto *action : actions)
      QVERIFY2(action->width() >= action->sizeHint().width(),
               qPrintable(QStringLiteral("%1: %2 < %3, side %4")
                              .arg(action->text())
                              .arg(action->width())
                              .arg(action->sizeHint().width())
                              .arg(action->parentWidget()->width())));
    QVERIFY(actions.front()->parentWidget()->width() > 300);
  }

  void quickStartAsksForANewFileOverItself() {
    // New asks for the file over the quick start, which stays when the user
    // cancels.  Escape works on one's own as Go does; only closing the
    // window leaves the start unchosen.
    QWidget *askedOver = nullptr;
    QString picked;
    const auto pick = [&](QWidget *parent) {
      askedOver = parent;
      return picked;
    };
    QuickStartDialog dialog(pick);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *newButton = buttonNamed(&dialog, QStringLiteral("&New"));
    QVERIFY(newButton);
    newButton->click();
    QVERIFY(askedOver == &dialog);
    QVERIFY(dialog.isVisible());
    QCOMPARE(dialog.start(), QuickStartDialog::Start::None);
    picked = QStringLiteral("/data/program");
    newButton->click();
    QVERIFY(!dialog.isVisible());
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QCOMPARE(dialog.start(), QuickStartDialog::Start::New);
    QCOMPARE(dialog.file(), picked);

    QuickStartDialog escaped(pick);
    escaped.show();
    QVERIFY(QTest::qWaitForWindowExposed(&escaped));
    QTest::keyClick(&escaped, Qt::Key_Escape);
    QVERIFY(!escaped.isVisible());
    QCOMPARE(escaped.start(), QuickStartDialog::Start::Go);

    QuickStartDialog closed(pick);
    closed.show();
    QVERIFY(QTest::qWaitForWindowExposed(&closed));
    QVERIFY(closed.close());
    QCOMPARE(closed.result(), int(QDialog::Rejected));
    QCOMPARE(closed.start(), QuickStartDialog::Start::None);
  }

  void quickStartResizesAndRemembersItsSize() {
    QSettings().remove(settings::QuickStartSize);
    const auto restore =
        qScopeGuard([] { QSettings().remove(settings::QuickStartSize); });
    QuickStartDialog dialog(noFile);
    dialog.resize(dialog.minimumSize());
    dialog.move(20, 20);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    auto *recent =
        dialog.findChild<QListWidget *>(QStringLiteral("quickStartRecent"));
    auto *grip = dialog.findChild<QSizeGrip *>();
    auto *action =
        dialog.findChild<QPushButton *>(QStringLiteral("quickStartAction"));
    QVERIFY(recent && grip && grip->isVisible() && action);
    const QSize initialSize = dialog.size();
    const QSize initialListSize = recent->size();
    const QSize actionSize = action->size();

    // Drag the actual resize control, then check the content gained the space.
    const QPoint start = grip->rect().center();
    const QPoint destination = grip->mapToGlobal(start) + QPoint(24, 24);
    QTest::mousePress(grip, Qt::LeftButton, Qt::NoModifier, start);
    QMouseEvent move(QEvent::MouseMove, grip->mapFromGlobal(destination),
                     destination, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(grip, &move);
    QTest::mouseRelease(grip, Qt::LeftButton, Qt::NoModifier,
                        grip->mapFromGlobal(destination));
    QTRY_VERIFY(dialog.width() > initialSize.width());
    QTRY_VERIFY(dialog.height() > initialSize.height());
    const QSize chosenSize = dialog.size();
    QCOMPARE(recent->size() - initialListSize, chosenSize - initialSize);
    QCOMPARE(action->size(), actionSize);

    dialog.showDropTarget(true);
    auto *drop =
        dialog.findChild<QWidget *>(QStringLiteral("quickStartDropTarget"));
    QVERIFY(drop && drop->isVisible());
    QCOMPARE(drop->geometry(), dialog.rect());
    dialog.resize(initialSize);
    QCOMPARE(drop->geometry(), dialog.rect());
    dialog.showDropTarget(false);
    dialog.resize(chosenSize);
    QTest::keyClick(&dialog, Qt::Key_Escape);

    QuickStartDialog reopened(noFile);
    QCOMPARE(reopened.size(), chosenSize);
    reopened.resize(1, 1);
    QCOMPARE(reopened.size(), reopened.minimumSize());
  }

  void fileDropOpensFromQuickStart() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    QTimer drag;
    bool accepted = false;
    connect(&drag, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != QLatin1String("quickStartDialog"))
        return;
      drag.stop();
      accepted = dropFile(dialog, path);
      if (!accepted)
        dialog->reject();
    });
    drag.start(10);
    bench.window->showQuickStart();
    QVERIFY(accepted);
    QTRY_COMPARE_WITH_TIMEOUT(bench.session.filePath(), path, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!QApplication::activeModalWidget());
  }

  void quickStartShowsWhatADropWouldDo() {
    // A file dragged over the quick start shows that a drop opens it, until
    // it leaves; a folder, which cannot open, shows nothing.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    QTimer drag;
    bool shown = false, hidden = false, folderShown = true;
    connect(&drag, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      if (!dialog || dialog->objectName() != QLatin1String("quickStartDialog"))
        return;
      drag.stop();
      auto *target =
          dialog->findChild<QWidget *>(QStringLiteral("quickStartDropTarget"));
      const auto enter = [dialog](const QString &dragged) {
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(dragged)});
        QDragEnterEvent event(QPoint(10, 10), Qt::CopyAction, &mime,
                              Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(dialog, &event);
      };
      QDragLeaveEvent leave;
      enter(path);
      shown = target && target->isVisible();
      QApplication::sendEvent(dialog, &leave);
      hidden = target && !target->isVisible();
      enter(directory.path());
      folderShown = target && target->isVisible();
      dialog->reject();
    });
    drag.start(10);
    bench.window->showQuickStart();
    QVERIFY(shown);
    QVERIFY(hidden);
    QVERIFY(!folderShown);
  }

  void reloadReadsTheInputFileAgainWithItsSavedComments() {
    // As IDA's File, Load file, Reload the input file: a fresh worker reads
    // the file again, with the comments that were saved; unsaved ones go
    // when the user discards them.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("saved comment"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    bench.session.setComment(Base + 0x140, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());

    const quint64 epoch = bench.session.epoch();
    LoadDialogAcceptor loadDialogs;
    QTimer discard;
    bool prompted = false;
    connect(&discard, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        prompted = true;
        box->button(QMessageBox::Discard)->click();
      }
    });
    discard.start(10);
    bench.action(ActionId::FileReload)->trigger();
    QTRY_VERIFY(prompted);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded() &&
                                 bench.session.epoch() != epoch,
                             OpenTimeoutMs);
    QCOMPARE(bench.session.filePath(), path);
    QVERIFY(!bench.session.dirty());
    const auto saved = readAll(path + ".neverd/annotations.json");
    QVERIFY(saved.contains("saved comment"));
    QVERIFY(!saved.contains("unsaved comment"));
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(
        log->toPlainText().contains(QStringLiteral("Reloaded the input file")));
  }

  void fileInAReadOnlyFolderKeepsItsProjectInTheDataDirectory() {
    // IDA asks for another place for its database when the input's folder
    // is read-only.  The workbench opens the file anyway: its database,
    // sidecars and lock sit in the user's data directory, beside a copy.
    QTemporaryDir data;
    const QByteArray dataHome = qgetenv("XDG_DATA_HOME");
    qputenv("XDG_DATA_HOME", data.path().toUtf8());
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QFile::setPermissions(directory.path(),
                          QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    const auto restore = qScopeGuard([&] {
      QFile::setPermissions(directory.path(), QFileDevice::ReadOwner |
                                                  QFileDevice::WriteOwner |
                                                  QFileDevice::ExeOwner);
      if (dataHome.isNull())
        qunsetenv("XDG_DATA_HOME");
      else
        qputenv("XDG_DATA_HOME", dataHome);
    });
    if (QFileInfo(directory.path()).isWritable())
      QSKIP("This user can write to a read-only folder");
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bench.session.projectPath(), path);
    const QString copy = bench.session.filePath();
    // XDG_DATA_HOME moves the data directory on Linux; macOS and Windows
    // keep their own, which Qt names.
    QVERIFY2(copy.startsWith(QStandardPaths::writableLocation(
                 QStandardPaths::AppLocalDataLocation)),
             qPrintable(copy));
    QVERIFY2(!copy.startsWith(directory.path()), qPrintable(copy));
    QCOMPARE(readAll(copy), readAll(path));
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains(
        QStringLiteral("is in a folder you cannot write to")));

    bench.session.setComment(Base, QStringLiteral("kept elsewhere"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(copy + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QVERIFY(!QFile::exists(path + ".neverd/annotations.json"));
    QTRY_VERIFY(!bench.session.dirty());

    // Reloading reads the file the user opened, with what was saved.
    const quint64 epoch = bench.session.epoch();
    LoadDialogAcceptor loadDialogs;
    bench.action(ActionId::FileReload)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded() &&
                                 bench.session.epoch() != epoch,
                             OpenTimeoutMs);
    QCOMPARE(bench.session.projectPath(), path);
    QCOMPARE(bench.session.filePath(), copy);
    QVERIFY(
        readAll(copy + ".neverd/annotations.json").contains("kept elsewhere"));
  }

  void fileDropKeepsUnsavedChangesWhenCancelled() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Workbench bench;
    bench.window->openFile(first);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("unsaved comment"));
    QTRY_VERIFY(bench.session.dirty());
    LoadDialogAcceptor loadDialogs;
    QTimer cancel;
    bool prompted = false;
    connect(&cancel, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        prompted = true;
        box->reject();
      }
    });
    cancel.start(10);
    QVERIFY(dropFile(bench.window.get(), second));
    QTRY_VERIFY(prompted);
    QCOMPARE(bench.session.filePath(), first);
    QVERIFY(bench.session.loaded());
    QVERIFY(bench.session.dirty());
  }

  void initTestCase() {
    QVERIFY(settingsDirectory_.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("WorkbenchTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsDirectory_.path());
    configureDocking();
  }

  void windowChromeLeavesRoomToType() {
    Workbench bench;
    // The window manager resizes the window from its edges.
    QVERIFY(!bench.window->statusBar()->isSizeGripEnabled());
    // Input dialogs leave room for a full name, comment or expression.
    QInputDialog input(bench.window.get());
    input.show();
    QVERIFY(QTest::qWaitForWindowExposed(&input));
    QVERIFY2(input.width() >= 420, qPrintable(QString::number(input.width())));
  }

  void localTypesLayOutAndRenderTheProgramsTypes() {
    QTemporaryDir directory;
    // A choice kept before exact widths became the default starts over.
    QSettings().setValue(QStringLiteral("localTypes/spelling"),
                         QStringLiteral("ida"));
    QSettings().setValue(QStringLiteral("localTypes/luaSpelling"),
                         QStringLiteral("windows"));
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    auto *layout =
        types->findChild<QTreeWidget *>(QStringLiteral("localTypesLayout"));
    auto *source =
        types->findChild<QPlainTextEdit *>(QStringLiteral("localTypesSource"));
    auto *filter =
        types->findChild<QLineEdit *>(QStringLiteral("localTypesFilter"));
    QVERIFY(list && layout && source && filter);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    // The filter has the focus, as IDA's quick filter does.
    QTRY_VERIFY(filter->hasFocus());

    // The list leaves the ordinal and the member count out until asked.
    auto *listHeader = list->header();
    QVERIFY(listHeader->isSectionHidden(0));
    QVERIFY(!listHeader->isSectionHidden(1));
    QVERIFY(listHeader->isSectionHidden(4));

    // A record shows its members, the padding between and the union it
    // holds, the union's bitfield at its bit: where each lies first, as a
    // memory map reads, the tree in the member's column.
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QCOMPARE(layout->headerItem()->text(0), QStringLiteral("Offset"));
    QCOMPARE(layout->headerItem()->text(2), QStringLiteral("Member"));
    QCOMPARE(layout->treePosition(), 2);
    QCOMPARE(layout->columnCount(), 4);
    QCOMPARE(layout->topLevelItem(0)->text(2), QStringLiteral("Type"));
    QCOMPARE(layout->topLevelItem(1)->text(2), QStringLiteral("(padding)"));
    QCOMPARE(layout->topLevelItem(1)->text(1), QStringLiteral("0x6"));
    QCOMPARE(layout->topLevelItem(3)->text(2), QStringLiteral("union"));
    QCOMPARE(layout->topLevelItem(3)->child(1)->text(0),
             QStringLiteral("0x18.0"));
    QCOMPARE(layout->topLevelItem(3)->child(1)->text(1), QStringLiteral(":1"));
    // What only source takes is not on the layout's options.
    auto *dependencies = types->findChild<QCheckBox *>();
    QVERIFY(dependencies);
    for (auto *box : types->findChildren<QCheckBox *>())
      if (box->text() != QLatin1String("Hex offsets"))
        QVERIFY2(box->isHidden(), qPrintable(box->text()));

    // A member's type opens on a double click; Back returns.
    auto *links = layout->topLevelItem(2);
    QCOMPARE(links->text(2), QStringLiteral("Links"));
    emit layout->itemDoubleClicked(links, 0);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    QTest::keyClick(layout, Qt::Key_Escape);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // The source in the language of its tab, with the options asked: C/C++
    // shows C, and each language chooses among its own spellings, apart
    // from the others'.
    auto *spelling =
        types->findChild<QComboBox *>(QStringLiteral("localTypesSpelling"));
    QVERIFY(spelling);
    const auto choices = [spelling] {
      QStringList ids;
      for (int index = 0; index < spelling->count(); ++index)
        ids.append(spelling->itemData(index).toString());
      return ids;
    };
    // Exact widths come first, every language's default.
    const QStringList twoSpellings{QStringLiteral("portable"),
                                   QStringLiteral("windows")};
    types->setPage(2);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language rust spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    spelling->setCurrentIndex(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language rust spelling windows")));
    types->setPage(3);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language go spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0), QStringLiteral("Exact widths (uint32)"));
    spelling->setCurrentIndex(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language go spelling windows")));
    // Python renders through ctypes, with its own two spellings.
    types->setPage(4);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language python spelling portable")));
    QVERIFY(!spelling->isHidden());
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0), QStringLiteral("Exact widths (c_uint32)"));
    // JavaScript whose JSDoc TypeScript checks, as one tab.
    const auto *tabBar =
        types->findChild<QTabBar *>(QStringLiteral("localTypesTabs"));
    QVERIFY(tabBar);
    QCOMPARE(tabBar->tabText(5), QStringLiteral("JS/TS"));
    types->setPage(5);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language javascript spelling portable")));
    QCOMPARE(choices(), twoSpellings);
    QCOMPARE(spelling->itemText(0),
             QStringLiteral("JavaScript types (number, bigint)"));
    // Lua, whose spelling names the C types its comments give.
    QCOMPARE(tabBar->tabText(6), QStringLiteral("Lua"));
    types->setPage(6);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language lua spelling portable")));
    QCOMPARE(choices(), twoSpellings);
    types->setPage(1);
    QTRY_VERIFY(source->toPlainText().startsWith(
        QStringLiteral("// language c spelling portable")));
    QVERIFY(source->toPlainText().contains(QStringLiteral("struct _DEVICE")));
    QVERIFY(!spelling->isHidden());
    for (auto *box : types->findChildren<QCheckBox *>())
      QVERIFY2(!box->isHidden(), qPrintable(box->text()));
    QCOMPARE(choices(),
             (QStringList{QStringLiteral("portable"), QStringLiteral("windows"),
                          QStringLiteral("keywords")}));
    QCOMPARE(spelling->currentData().toString(), QStringLiteral("portable"));
    QCOMPARE(QSettings().value(QStringLiteral("localTypes/view")).toString(),
             QStringLiteral("c"));
    QCOMPARE(QSettings()
                 .value(QStringLiteral("localTypes/spellings/rust"))
                 .toString(),
             QStringLiteral("windows"));
    QCOMPARE(
        QSettings().value(QStringLiteral("localTypes/spellings/go")).toString(),
        QStringLiteral("windows"));
    QVERIFY(!QSettings().contains(QStringLiteral("localTypes/spelling")));
    QVERIFY(!QSettings().contains(QStringLiteral("localTypes/luaSpelling")));

    // The command line shows a type by name, and says when none has it.
    auto *output = bench.window->findChild<OutputWindow *>();
    QVERIFY(output);
    output->run(QStringLiteral("t _LIST_ENTRY"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    output->run(QStringLiteral("types _NO_SUCH_TYPE"));
    QTRY_VERIFY(types->findChild<QLabel *>(QStringLiteral("localTypesSummary"))
                    ->text()
                    .contains(QStringLiteral("_NO_SUCH_TYPE")));

    // The filter keeps the types whose name holds its text.
    filter->setText(QStringLiteral("list"));
    QTRY_COMPARE(list->model()->rowCount(), 1);
    types->setPage(0);
  }

  void localTypesShowKindsPathsAndCards() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    auto *layout =
        types->findChild<QTreeWidget *>(QStringLiteral("localTypesLayout"));
    auto *source =
        types->findChild<QPlainTextEdit *>(QStringLiteral("localTypesSource"));
    auto *path = types->findChild<QLabel *>(QStringLiteral("localTypesPath"));
    QVERIFY(list && layout && source && path);
    QTRY_COMPARE(list->model()->rowCount(), 3);

    // Each type shows the icon of its kind, and hovering its name says what
    // it is, how big and where it comes from.
    for (int row = 0; row < list->model()->rowCount(); ++row) {
      const QModelIndex name = list->model()->index(row, 1);
      QVERIFY2(!name.data(Qt::DecorationRole).value<QIcon>().isNull(),
               qPrintable(name.data().toString()));
      QVERIFY(name.data(Qt::ToolTipRole)
                  .toString()
                  .contains(
                      QStringLiteral("<b>%1</b>").arg(name.data().toString())));
    }
    const QModelIndexList device =
        list->model()->match(list->model()->index(0, 1), Qt::DisplayRole,
                             QStringLiteral("_DEVICE"), 1, Qt::MatchExactly);
    QCOMPARE(device.size(), 1);
    const QString card = device.first().data(Qt::ToolTipRole).toString();
    QVERIFY2(card.contains(QStringLiteral("structure")) &&
                 card.contains(QStringLiteral("bytes")),
             qPrintable(card));

    // The breadcrumb names the records that hold the member selected, and
    // a crumb selects the record it names.
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QTreeWidgetItem *bit = layout->topLevelItem(3)->child(1);
    layout->setCurrentItem(bit);
    QVERIFY(path->isVisibleTo(types));
    QVERIFY2(path->text().contains(QStringLiteral("_DEVICE")) &&
                 path->text().contains(QStringLiteral("union")) &&
                 path->text().contains(bit->text(2)),
             qPrintable(path->text()));
    emit path->linkActivated(QStringLiteral("3/"));
    QCOMPARE(layout->currentItem(), layout->topLevelItem(3));
    emit path->linkActivated(QString());
    QCOMPARE(layout->currentItem(), nullptr);
    QVERIFY(!path->isVisibleTo(types));
    // A member's row says where it lies.
    QVERIFY2(bit->toolTip(2).contains(QStringLiteral("bits")),
             qPrintable(bit->toolTip(2)));
    QVERIFY(layout->topLevelItem(2)->toolTip(2).contains(
        QStringLiteral("<b>Links</b>")));

    // The member selected shows its line in the source, which marks the line
    // its cursor is on.
    layout->setCurrentItem(layout->topLevelItem(2));
    types->setPage(1);
    QTRY_VERIFY(
        source->textCursor().block().text().contains(QStringLiteral("Links;")));
    QCOMPARE(source->extraSelections().size(), 1);
    QCOMPARE(source->extraSelections().first().cursor.block(),
             source->textCursor().block());

    // Hovering a type's name in the source says what it is; Ctrl+click opens
    // it, as an editor follows a definition.
    const QTextCursor word =
        source->document()->find(QStringLiteral("_LIST_ENTRY"));
    QVERIFY(!word.isNull());
    QTextCursor inside(source->document());
    inside.setPosition(word.selectionStart() + 3);
    const QPoint point = source->cursorRect(inside).center();
    QHelpEvent help(QEvent::ToolTip, point,
                    source->viewport()->mapToGlobal(point));
    QApplication::sendEvent(source->viewport(), &help);
    QTRY_VERIFY(QToolTip::text().contains(QStringLiteral("_LIST_ENTRY")));
    QTest::mouseClick(source->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      point);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
    QTest::keyClick(source, Qt::Key_Escape);
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // So does Ctrl+click on a member's type in the layout.
    types->setPage(0);
    QTRY_COMPARE(layout->topLevelItemCount(), 4);
    QTreeWidgetItem *links = layout->topLevelItem(2);
    const QRect row = layout->visualItemRect(links);
    QTest::mouseClick(layout->viewport(), Qt::LeftButton, Qt::ControlModifier,
                      row.center());
    QTRY_COMPARE(types->currentType(), QStringLiteral("_LIST_ENTRY"));
  }

  void structureOffsetsRankTypesByTheCodesAccesses() {
    // T on `mov rax, [rcx+8]` lists the local types by how function_7's
    // accesses through rcx land on their members, the one that holds them
    // all first and previewed as the operand becomes; Apply shows the
    // operand, and every other access through rcx, as members of it in one
    // step of the history.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.showsStructOffsets());
    auto *disassembly = bench.window->findChild<DisassemblyView *>();
    QVERIFY(disassembly);
    const Address read = Base + 0x70;
    const auto lineAt = [&](Address at) {
      disassembly->navigate(at);
      return disassembly->listing()->currentLineText();
    };
    disassembly->navigate(read);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(read));
    QTRY_VERIFY(disassembly->listing()->currentLineText().contains(
        QLatin1String("[rcx+8]")));
    QTRY_VERIFY(bench.action(ActionId::EditOperandStruct)->isEnabled());

    QStringList listed;
    QString expression, every, summary;
    auto *picker = new QTimer(&bench.session);
    connect(picker, &QTimer::timeout, picker, [&, picker] {
      auto *dialog =
          qobject_cast<TypePickerDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *list =
          dialog->findChild<QListWidget *>(QStringLiteral("typePickerList"));
      auto *search =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerSearch"));
      if (!list || !search || list->count() < 2 || !search->hasFocus())
        return;
      picker->stop();
      picker->deleteLater();
      for (int row = 0; row < list->count(); ++row)
        listed << list->item(row)->text();
      expression =
          dialog->findChild<QLabel *>(QStringLiteral("typePickerExpression"))
              ->text();
      summary = dialog->findChild<QLabel *>(QStringLiteral("typePickerSummary"))
                    ->text();
      auto *box =
          dialog->findChild<QCheckBox *>(QStringLiteral("typePickerEvery"));
      every = box->isVisibleTo(dialog) ? box->text() : QString();
      box->setChecked(true);
      QTest::keyClick(search, Qt::Key_Return);
    });
    picker->start(10);
    bench.action(ActionId::EditOperandStruct)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        listed,
        (QStringList{QStringLiteral("_DEVICE"), QStringLiteral("_LIST_ENTRY")}),
        OpenTimeoutMs);
    QCOMPARE(expression, QStringLiteral("[rcx+_DEVICE.Links.Flink]"));
    QCOMPARE(every, QStringLiteral("Also show the 2 other instructions that "
                                   "go through rcx as members"));
    QCOMPARE(summary, QStringLiteral("2 types"));
    QTRY_VERIFY_WITH_TIMEOUT(
        lineAt(read).contains(QLatin1String("[rcx+_DEVICE.Links.Flink]")),
        OpenTimeoutMs);
    QTRY_VERIFY(lineAt(read + 1).contains(QLatin1String("[rcx+_DEVICE.Type]")));
    QTRY_VERIFY(
        lineAt(read + 2).contains(QLatin1String("[rcx+_DEVICE.Links.Blink]")));
    // The number of `add rcx, 18h` stays: no access went through it.
    QTRY_VERIFY(lineAt(read + 3).contains(QLatin1String("rcx, 18h")));

    // One undo takes the three back.
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(
        lineAt(read + 2).contains(QLatin1String("[rcx+10h]")), OpenTimeoutMs);
    QTRY_VERIFY(lineAt(read).contains(QLatin1String("[rcx+8]")));
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void pseudocodeParametersTakeTypesByConvertAndY() {
    // In function_7's pseudocode, Convert to struct * on arg0 ranks the local
    // types by the reads through rcx and makes it a _DEVICE * under the name
    // the user gives, whose members the code then reads; Y on arg1 completes
    // a type's name, refuses one the program lacks and states one that
    // reads; Y on the function shows its declaration and hands it back to
    // analysis.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.convertsParameters());
    const Address function = Base + 0x70;
    bench.window->disassembly()->navigate(function);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(function));
    bench.action(ActionId::ViewPseudocode)->trigger();
    auto *view = bench.codeView(QStringLiteral("source"));
    QVERIFY(view);
    auto *code = view->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() &&
                                 code->allText().contains("void* arg0"),
                             OpenTimeoutMs);
    const auto onParameter = [&](const QString &name, int position) {
      code->setCursorLine(0);
      if (!code->findText(name, true))
        return false;
      code->setFocus();
      const auto target = code->renameTarget();
      return target && target->value("parameter").toInt(-1) == position;
    };
    QTRY_VERIFY(onParameter(QStringLiteral("arg0"), 0));
    // The commands follow the view the focus moves to, at once: T is the
    // disassembly's, Convert to struct * the pseudocode's.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    bench.window->disassembly()->focusContent();
    QVERIFY(bench.action(ActionId::EditOperandStruct)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditConvertToStruct)->isEnabled());
    code->setFocus();
    QVERIFY(bench.action(ActionId::EditConvertToStruct)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditOperandStruct)->isEnabled());

    QStringList listed;
    QString member;
    auto *picker = new QTimer(&bench.session);
    connect(picker, &QTimer::timeout, picker, [&, picker] {
      auto *dialog =
          qobject_cast<TypePickerDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *list =
          dialog->findChild<QListWidget *>(QStringLiteral("typePickerList"));
      auto *search =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerSearch"));
      auto *name =
          dialog->findChild<QLineEdit *>(QStringLiteral("typePickerName"));
      if (!list || !search || !name || list->count() < 2 || !search->hasFocus())
        return;
      picker->stop();
      picker->deleteLater();
      for (int row = 0; row < list->count(); ++row)
        listed << list->item(row)->text();
      name->setText(QStringLiteral("Device"));
      member = dialog->findChild<QLabel *>(QStringLiteral("typePickerMember"))
                   ->text();
      QTest::keyClick(search, Qt::Key_Return);
    });
    picker->start(10);
    bench.action(ActionId::EditConvertToStruct)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        listed,
        (QStringList{QStringLiteral("_DEVICE"), QStringLiteral("_LIST_ENTRY")}),
        OpenTimeoutMs);
    QCOMPARE(member, QStringLiteral("_DEVICE *Device"));
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_DEVICE *Device") &&
                                 code->allText().contains("Device->Type"),
                             OpenTimeoutMs);

    // Y on arg1: its type, a name completed, one refused, one stated.
    QTRY_VERIFY(onParameter(QStringLiteral("arg1"), 1));
    QTRY_VERIFY(bench.action(ActionId::EditSetType)->isEnabled());
    QString subject, completed, refused;
    int stage = 0;
    auto *typer = new QTimer(&bench.session);
    connect(typer, &QTimer::timeout, typer, [&, typer] {
      auto *dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      // The completion list is a window of its own while it shows.
      if (!dialog || (stage != 1 && !dialog->isActiveWindow()))
        return;
      auto *field =
          dialog->findChild<QLineEdit *>(QStringLiteral("setTypeField"));
      auto *status =
          dialog->findChild<QLabel *>(QStringLiteral("setTypeStatus"));
      auto *apply =
          dialog->findChild<QPushButton *>(QStringLiteral("setTypeApply"));
      if (!field || !status || !apply || !field->isEnabled())
        return;
      if (stage == 0) {
        if (field->text() != QLatin1String("int64_t") || !apply->isEnabled())
          return;
        subject = dialog->findChild<QLabel *>(QStringLiteral("setTypeSubject"))
                      ->text();
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("_LIS"));
        stage = 1;
      } else if (stage == 1) {
        auto *completer = dialog->findChild<QCompleter *>();
        auto *popup = completer ? completer->popup() : nullptr;
        if (!popup || !popup->isVisible() || !popup->model()->rowCount())
          return;
        completed = popup->model()->index(0, 0).data().toString();
        popup->setCurrentIndex(popup->model()->index(0, 0));
        QTest::keyClick(popup, Qt::Key_Return);
        stage = 2;
      } else if (stage == 2) {
        if (field->text() != QLatin1String("_LIST_ENTRY"))
          return;
        QTest::keyClicks(field, QStringLiteral(" *NOPE"));
        stage = 3;
      } else if (stage == 3) {
        if (!status->text().startsWith(QStringLiteral("✗")))
          return;
        refused = status->text();
        QVERIFY(!apply->isEnabled());
        field->selectAll();
        QTest::keyClicks(field, QStringLiteral("_LIST_ENTRY *"));
        stage = 4;
      } else if (stage == 4) {
        if (!apply->isEnabled() ||
            !status->text().contains(QLatin1String("_LIST_ENTRY *arg1")))
          return;
        typer->stop();
        typer->deleteLater();
        QTest::keyClick(field, Qt::Key_Return);
      }
    });
    typer->start(10);
    bench.action(ActionId::EditSetType)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_LIST_ENTRY *arg1"),
                             OpenTimeoutMs);
    QCOMPARE(subject, QStringLiteral(
                          "arg1, parameter 2 of function_7, arrives in rdx."));
    QCOMPARE(completed, QStringLiteral("_LIST_ENTRY"));
    QVERIFY2(refused.contains(QLatin1String("'NOPE' names no type")),
             qPrintable(refused));

    // Y on the function: the declaration the user gave it, which goes back
    // to analysis.
    code->setCursorLine(0);
    QVERIFY(code->findText(QStringLiteral("function_7"), true));
    code->setFocus();
    QString declaration;
    auto *resetter = new QTimer(&bench.session);
    connect(resetter, &QTimer::timeout, resetter, [&, resetter] {
      auto *dialog =
          qobject_cast<SetTypeDialog *>(QApplication::activeModalWidget());
      if (!dialog || !dialog->isActiveWindow())
        return;
      auto *field =
          dialog->findChild<QLineEdit *>(QStringLiteral("setTypeField"));
      auto *reset =
          dialog->findChild<QPushButton *>(QStringLiteral("setTypeReset"));
      if (!field || !reset || !field->isEnabled() || !reset->isVisible())
        return;
      resetter->stop();
      resetter->deleteLater();
      declaration = field->text();
      reset->click();
    });
    resetter->start(10);
    bench.action(ActionId::EditSetType)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("void* arg0"),
                             OpenTimeoutMs);
    QCOMPARE(declaration, QStringLiteral("int64_t function_7(_DEVICE *Device, "
                                         "_LIST_ENTRY *arg1)"));
    // One undo states the declaration again.
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(code->allText().contains("_LIST_ENTRY *arg1"),
                             OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void localTypesAreDeclaredEditedAndDeletedByIdasKeys() {
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("local-types.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.declaresLocalTypes());
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(types);
    auto *list =
        types->findChild<QTreeView *>(QStringLiteral("localTypesList"));
    QVERIFY(list);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    types->showType(QStringLiteral("_DEVICE"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("_DEVICE"));

    // Ins opens the editor over the list, the main window's keys notwith-
    // standing; a problem stays in it at its place, and a declaration that
    // reads adds its type and shows it.
    QStringList titles, languages;
    QString chosen;
    bool problemShown = false, problemAtItsPlace = false;
    int answers = 0;
    QTimer editor;
    connect(&editor, &QTimer::timeout, this, [&] {
      auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
      auto *text = dialog ? dialog->findChild<QPlainTextEdit *>(
                                QStringLiteral("declareEditor"))
                          : nullptr;
      if (auto *language = dialog ? dialog->findChild<QComboBox *>(
                                        QStringLiteral("declareLanguage"))
                                  : nullptr;
          language && languages.isEmpty()) {
        chosen = language->currentData().toString();
        for (int index = 0; index < language->count(); ++index)
          languages.append(language->itemText(index) + QLatin1Char('=') +
                           language->itemData(index).toString());
      }
      auto *declare =
          dialog ? dialog->findChild<QPushButton *>(QStringLiteral("declare"))
                 : nullptr;
      auto *problems = dialog ? dialog->findChild<QListWidget *>(
                                    QStringLiteral("declareProblems"))
                              : nullptr;
      if (!text || !declare || !problems || !declare->isEnabled())
        return;
      if (titles.size() == answers)
        titles.append(dialog->windowTitle());
      if (answers == 0 && !problems->isVisible()) {
        text->setPlainText(QStringLiteral("struct BAD {\n  NOPE x;\n};"));
        QTest::mouseClick(declare, Qt::LeftButton);
        return;
      }
      if (answers == 0) {
        problemShown = problems->count() == 1;
        problemAtItsPlace = text->textCursor().blockNumber() == 1 &&
                            text->textCursor().positionInBlock() == 2;
        text->setPlainText(QStringLiteral("struct OWNER { void *Next; };"));
        // Ctrl+Return declares from the editor.
        ++answers;
        QTest::keyClick(text, Qt::Key_Return, Qt::ControlModifier);
        return;
      }
      if (answers == 1 &&
          text->toPlainText().contains(QStringLiteral("struct OWNER"))) {
        // Edit: the type shown, declared in the language shown.
        ++answers;
        QTest::mouseClick(declare, Qt::LeftButton);
      }
    });
    editor.start(10);
    QTimer::singleShot(OpenTimeoutMs, &bench.session, [] {
      if (auto *dialog =
              qobject_cast<QDialog *>(QApplication::activeModalWidget()))
        dialog->reject();
    });
    // From a tab that renders only, as Python's, the editor declares C.
    types->setPage(4);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Insert);
    QVERIFY(problemShown);
    QVERIFY(problemAtItsPlace);
    QCOMPARE(chosen, QStringLiteral("c"));
    // C/C++ declares C, the language the window shows it in.
    QCOMPARE(languages, (QStringList{QStringLiteral("C/C++=c"),
                                     QStringLiteral("Rust=rust"),
                                     QStringLiteral("Go=go")}));
    QTRY_COMPARE(list->model()->rowCount(), 4);
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));

    // The list shows one origin when asked.
    auto *origin =
        types->findChild<QComboBox *>(QStringLiteral("localTypesOrigin"));
    QVERIFY(origin);
    origin->setCurrentIndex(origin->findData(QStringLiteral("user")));
    QTRY_COMPARE(list->model()->rowCount(), 1);
    origin->setCurrentIndex(origin->findData(QStringLiteral("pdb")));
    QTRY_COMPARE(list->model()->rowCount(), 3);
    // A type of another origin opens all of them again.
    types->showType(QStringLiteral("OWNER"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));
    QCOMPARE(origin->currentIndex(), 0);
    QTRY_COMPARE(list->model()->rowCount(), 4);

    // Ctrl+E edits it, though the main window jumps to an entry point by
    // the same key.
    list->setFocus();
    QTest::keyClick(list, Qt::Key_E, Qt::ControlModifier);
    QTRY_COMPARE(answers, 2);
    editor.stop();
    QCOMPARE(titles.value(0), QStringLiteral("Add types"));
    QCOMPARE(titles.value(1), QStringLiteral("Edit OWNER"));
    QTRY_COMPARE(types->currentType(), QStringLiteral("OWNER"));

    // Del deletes the selected type once asked.
    QTimer confirm;
    connect(&confirm, &QTimer::timeout, this, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        confirm.stop();
        QTest::mouseClick(box->button(QMessageBox::Yes), Qt::LeftButton);
      }
    });
    confirm.start(10);
    list->setFocus();
    QTest::keyClick(list, Qt::Key_Delete);
    QTRY_COMPARE(list->model()->rowCount(), 3);
    auto *summary =
        types->findChild<QLabel *>(QStringLiteral("localTypesSummary"));
    QVERIFY(summary);
    QTRY_VERIFY(summary->text().contains(QStringLiteral("Deleted OWNER")));
  }

  void viewsNarrowPastTheirRows() {
    // A status line or a row of buttons is cut at its view's edge.  Kept
    // whole, the disassembly's status line held its dock over 500 pixels
    // wide, and the separators beside that dock would not move.
    QTemporaryDir directory;
    Workbench bench;
    bench.window->openFile(
        writeFixture(directory, QStringLiteral("pseudocode-import.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    bench.window->disassembly()->navigate(Base + 0x140);
    QTRY_COMPARE(bench.window->disassembly()->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewPseudocode)->trigger();
    bench.action(ActionId::ViewExtensions)->trigger();
    bench.action(ActionId::ViewLocalTypes)->trigger();
    auto *code = bench.codeView(QStringLiteral("source"));
    auto *extensions = bench.window->findChild<ExtensionsView *>();
    auto *types = bench.window->findChild<LocalTypesView *>();
    QVERIFY(code && extensions && types && bench.functions());
    // Every button shows, as the fold button does for library code.
    for (auto *button : code->findChildren<QToolButton *>())
      button->show();
    const QString wide(400, QLatin1Char('x'));
    const QList<QWidget *> views{bench.window->disassembly(), code,
                                 bench.functions(), extensions, types};
    for (auto *view : views) {
      for (auto *label : view->findChildren<QLabel *>())
        label->setText(wide);
      QVERIFY2(view->minimumSizeHint().width() <= NarrowViewWidth,
               view->metaObject()->className());
    }
  }

  void closingFileClearsDisassemblyAndReopensAtTheSameAddress() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    auto *listing = disassembly->listing();
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(Base + 0x140));
    QVERIFY(!listing->currentLineText().isEmpty());
    listing->setFocus();
    QTest::keyClick(listing, Qt::Key_Home);
    QTest::keyClick(listing, Qt::Key_Right, Qt::ShiftModifier);
    QVERIFY(!listing->selectedText().isEmpty());
    listing->horizontalScrollBar()->setValue(
        listing->horizontalScrollBar()->maximum());

    bench.action(ActionId::FileClose)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    // A closed image has no cached address, copyable text or scroll position.
    QVERIFY(!listing->currentAddress());
    QVERIFY(!listing->currentItem());
    QVERIFY(!listing->topItem());
    QVERIFY(!listing->currentFunction());
    QVERIFY(!listing->cursorLine());
    QVERIFY(listing->currentLineText().isEmpty());
    QVERIFY(listing->currentToken().isEmpty());
    QVERIFY(listing->selectedText().isEmpty());
    QVERIFY(!listing->jumpPending());
    QCOMPARE(listing->horizontalScrollBar()->maximum(), 0);
    QCOMPARE(listing->horizontalScrollBar()->value(), 0);
    QCOMPARE(listing->verticalScrollBar()->maximum(), 0);
    QCOMPARE(listing->verticalScrollBar()->value(), 0);
    QVERIFY(!disassembly->currentAddress());
    QVERIFY(!disassembly->canGoBack());
    QVERIFY(!disassembly->canGoForward());

    QSignalSpy locations(listing, &ListingView::locationChanged);
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(listing->currentItem(), std::optional<Address>(Base + 0x140));
    // Reusing an address in the new image still publishes its location.
    QVERIFY(!locations.isEmpty());
  }

  void closingDuringAJumpCancelsTheDeferredGraphSwitch() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    disassembly->navigate(Base + 0x140);
    QVERIFY(disassembly->listing()->jumpPending());
    disassembly->setGraphMode(true);
    QVERIFY(!disassembly->graphMode());
    bench.action(ActionId::FileClose)->trigger();
    QVERIFY(!bench.session.loaded());
    QVERIFY(!disassembly->listing()->jumpPending());

    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    QVERIFY(!disassembly->graphMode());
  }

  void graphStopsWaitingWhenTheFileCloses() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    LoadDialogAcceptor loadDialogs;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->findChild<DisassemblyView *>();
    QVERIFY(disassembly);
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    // The graph says it is laying out until its layout arrives...
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(disassembly->graph()->waiting());
    // ...and stops when the file closes first.
    bench.session.closeFile();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!disassembly->graph()->waiting());
    QVERIFY(!disassembly->graph()->loaded());
    QVERIFY(!disassembly->graph()->function());
    QVERIFY(!disassembly->graph()->currentAddress());
    QVERIFY(!disassembly->listing()->currentItem());
  }

  void hexViewShownBeforeOpeningLoadsItsBytes() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    // A restored desktop can show the hex view before the regions arrive.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(hex->byteAt(Base).has_value(), OpenTimeoutMs);
    QVERIFY(hex->isVisible());

    // Shift and the arrows select bytes, which copy as hex text.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    hex->setCurrent(Base);
    hex->setFocus();
    QTRY_VERIFY(hex->hasFocus());
    for (int i = 0; i < 3; ++i)
      QTest::keyClick(hex, Qt::Key_Right, Qt::ShiftModifier);
    QCOMPARE(hex->selection(),
             (std::optional<std::pair<Address, Address>>({Base, Base + 3})));
    QStringList bytes;
    for (Address at = Base; at <= Base + 3; ++at) {
      QTRY_VERIFY(hex->byteAt(at).has_value());
      bytes.append(QStringLiteral("%1")
                       .arg(*hex->byteAt(at), 2, 16, QLatin1Char('0'))
                       .toUpper());
    }
    clearClipboard();
    QTest::keyClick(hex, Qt::Key_C, Qt::ControlModifier);
    QTRY_COMPARE(clipboardText(), bytes.join(' '));
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QTRY_COMPARE(clipboardText(), bytes.join(' '));
    // A move without Shift ends the selection.
    QTest::keyClick(hex, Qt::Key_Left);
    QVERIFY(!hex->selection());
  }

  void stringReferencesAndHexTextEncodings() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);

    // The reference search lists instructions with the strings they use and
    // opens ready for a query.
    bench.action(ActionId::SearchStringReferences)->trigger();
    ChooserView *references = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::StringReferences)
        references = view;
    QVERIFY(references);
    QTRY_VERIFY(references->findChild<QLineEdit *>()->isVisible());
    QTRY_COMPARE_WITH_TIMEOUT(references->model().total(), 2, OpenTimeoutMs);
    QTRY_COMPARE(references->model().rowObject(1).value("text").toString(),
                 QStringLiteral("Wide"));
    QCOMPARE(references->model().addressAt(1),
             std::optional<Address>(Base + 0x73));
    // Its cross references are those of the string, not the instruction.
    QCOMPARE(references->model().referenceAddressAt(1),
             std::optional<Address>(Base + 0x3108));
    references->setFilterText(QString::fromUtf8("\u6587"));
    QTRY_COMPARE(references->model().total(), 1);

    // In the Strings list, Ctrl+X lists the selected string's references.
    bench.action(ActionId::ViewStrings)->trigger();
    ChooserView *strings = nullptr;
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Strings)
        strings = view;
    QVERIFY(strings);
    QTRY_COMPARE_WITH_TIMEOUT(strings->model().total(), 2, OpenTimeoutMs);
    QTRY_VERIFY(strings->model().addressAt(1).has_value());
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    strings->table()->setFocus();
    strings->table()->setCurrentIndex(strings->model().index(1, 0));
    QTRY_VERIFY(strings->table()->hasFocus());
    QString title;
    QTimer::singleShot(0, [&title] {
      if (auto *dialog = QApplication::activeModalWidget()) {
        title = dialog->windowTitle();
        dialog->close();
      }
    });
    QTest::keyClick(strings->table(), Qt::Key_X, Qt::ControlModifier);
    QTRY_VERIFY(!title.isEmpty());
    QVERIFY2(
        title.contains(QStringLiteral("FFFF800012343108"), Qt::CaseInsensitive),
        qPrintable(title));

    // The hex view's text column reads the bytes in a chosen encoding.
    for (auto *dock :
         bench.window->findChildren<KDDockWidgets::QtWidgets::DockWidget *>())
      if (dock->uniqueName() == QLatin1String("hex-1"))
        dock->raise();
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_VERIFY(hex->isVisible());
    hex->setCurrent(Base + 0x3108);
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3109),
                              std::optional<QString>(QStringLiteral(".")),
                              OpenTimeoutMs);
    hex->setTextEncoding(QStringLiteral("utf-16le"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textAt(Base + 0x3108),
                              std::optional<QString>(QStringLiteral("W")),
                              OpenTimeoutMs);
    QCOMPARE(hex->textAt(Base + 0x3109), std::optional<QString>(QString()));
    QCOMPARE(QSettings().value(QStringLiteral("hex/textEncoding")).toString(),
             QStringLiteral("utf-16le"));
    // An encoding the engine does not know falls back to ASCII.
    hex->setTextEncoding(QStringLiteral("klingon"));
    QTRY_COMPARE_WITH_TIMEOUT(hex->textEncoding(), QString(), OpenTimeoutMs);
    QVERIFY(!QSettings().contains(QStringLiteral("hex/textEncoding")));
  }

  void viewsFollowTheSessionAndNavigation() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(),
                              std::optional<Address>(Base), OpenTimeoutMs);

    // The function window pages rows from the worker.
    auto *functions = bench.functions();
    QVERIFY(functions);
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 600, OpenTimeoutMs);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_0"));
    QCOMPARE(functions->model().addressAt(0), std::optional<Address>(Base));
    functions->setFilterText(QStringLiteral("function_599"));
    QTRY_COMPARE(functions->model().total(), 1);
    QTRY_COMPARE(functions->model().rowObject(0).value("name").toString(),
                 QStringLiteral("function_599"));
    QCOMPARE(functions->model().addressAt(0),
             std::optional<Address>(Base + 0x2570));
    functions->setFilterText({});
    QTRY_COMPARE(functions->model().total(), 600);
    // Rows that arrive before the regions name their segments once the
    // regions do: only the Segment column repaints.
    QTRY_VERIFY(functions->model().rowCount() > 0);
    QSignalSpy repaint(&functions->model(), &QAbstractItemModel::dataChanged);
    functions->model().addressSpaceChanged();
    QCOMPARE(repaint.size(), 1);
    QCOMPARE(repaint.first().at(0).toModelIndex().column(), 1);
    QCOMPARE(repaint.first().at(1).toModelIndex().column(), 1);

    // Navigation records history and synchronizes the hex view.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    QTRY_COMPARE(disassembly->currentFunction(),
                 std::optional<Address>(Base + 0x140));
    auto *hex = bench.window->findChild<HexView *>();
    QVERIFY(hex);
    QTRY_COMPARE(hex->currentAddress(), std::optional<Address>(Base + 0x140));
    QVERIFY(disassembly->canGoBack());
    disassembly->goBack();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(Base));
    QVERIFY(disassembly->canGoForward());
    disassembly->goForward();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));

    // Pseudocode pages every line of the function.
    bench.action(ActionId::ViewPseudocode)->trigger();
    CodeView *pseudocode = nullptr;
    QTRY_VERIFY((pseudocode = bench.codeView(QStringLiteral("source"))) !=
                nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(
        pseudocode->text()->allText().contains(QStringLiteral("code line 699")),
        OpenTimeoutMs);
    QCOMPARE(pseudocode->text()->function(),
             std::optional<Address>(Base + 0x140));

    // An IR row the user selects takes the disassembly cursor along; Tab
    // moves the focus there too.
    bench.action(ActionId::ViewLowIR)->trigger();
    CodeView *ir = nullptr;
    QTRY_VERIFY((ir = bench.codeView(QStringLiteral("low"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(ir->text()->lineCount() > 3, OpenTimeoutMs);
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    ir->text()->setCursorLine(3);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x143));
    QVERIFY(ir->text()->hasFocus());
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_VERIFY(disassembly->isAncestorOf(QApplication::focusWidget()));
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x143));

    // Copy takes the lines selected in the window holding the focus, also in
    // an IR window switched to pseudocode, which is not the pseudocode
    // window; it used to copy the disassembly.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->setRepresentation(QStringLiteral("source"));
    QTRY_VERIFY_WITH_TIMEOUT(
        !ir->text()->loading() && ir->text()->lineCount() > 3, OpenTimeoutMs);
    // Two windows showing pseudocode are lettered apart.
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("Pseudocode-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-B"));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    ir->text()->setCursorLine(1);
    QTest::keyClick(ir->text(), Qt::Key_Down, Qt::ShiftModifier);
    QTest::keyClick(ir->text(), Qt::Key_End, Qt::ShiftModifier);
    const QString lines = QStringLiteral("// code line 1\n// code line 2");
    clearClipboard();
    QTest::keyClick(ir->text(), Qt::Key_C, Qt::ControlModifier);
    QCOMPARE(clipboardText(), lines);
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), lines);

    // C opens at the definition: the includes and declarations before it fold
    // into one line, which Keypad + expands and Keypad - folds again, and
    // which copies as the lines it stands for.
    pseudocode->setRepresentation(QStringLiteral("llvmc"));
    QCOMPARE(bench.dockTitle(QStringLiteral("pseudocode-a")),
             QStringLiteral("LLVM C-A"));
    QCOMPARE(bench.dockTitle(QStringLiteral("ir-a")),
             QStringLiteral("Pseudocode-A"));
    auto *code = pseudocode->text();
    QTRY_VERIFY_WITH_TIMEOUT(!code->loading() && code->lineCount() == 702,
                             OpenTimeoutMs);
    QVERIFY(code->allText().startsWith(QStringLiteral("#include <stdint.h>")));
    QCOMPARE(code->foldableCount(), 0);
    code->setFocus();
    QTRY_VERIFY(code->hasFocus());
    code->setCursorLine(0);
    clearClipboard();
    QTest::keyClick(code, Qt::Key_C, Qt::ControlModifier);
    const QString declaration =
        QStringLiteral("typedef struct QDomNode QDomNode;");
    const QString linked = QStringLiteral(
        "extern int Bar_ctor() __asm__(\"_ZN3BarC1Ev\"); /* Bar::Bar() */");
    const QString globals =
        QStringLiteral("/* neverd.image: 0x20 */\nint64_t dso_handle = 0x20;\n"
                       "extern uint64_t qword_10; /* 0x10 */\n");
    QCOMPARE(clipboardText(), QStringLiteral("#include <stdint.h>\n") +
                                  declaration + QLatin1Char('\n') + linked +
                                  QLatin1Char('\n') + globals);
    QTest::keyClick(code, Qt::Key_Plus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 708);
    QTest::keyClick(code, Qt::Key_Minus, Qt::KeypadModifier);
    QCOMPARE(code->lineCount(), 702);
    QVERIFY(code->preludeFolded());

    // A type the prelude declares opens at its declaration, expanding the
    // prelude; C's own types are not declarations.
    QCOMPARE(code->declarationLine(QStringLiteral("QDomNode")),
             std::optional<int>(1));
    QVERIFY(!code->declarationLine(QStringLiteral("uint64_t")));
    QVERIFY(code->goToDeclaration(QStringLiteral("QDomNode")));
    QVERIFY(!code->preludeFolded());
    QCOMPARE(code->lineCount(), 708);
    // A global goes to its address, under whatever name C gives it there.
    QCOMPARE(code->objectAddress(QStringLiteral("dso_handle")),
             std::optional<Address>(0x20));
    QCOMPARE(code->objectAddress(QStringLiteral("qword_10")),
             std::optional<Address>(0x10));
    QVERIFY(!code->objectAddress(QStringLiteral("QDomNode")));
    QVERIFY(!code->objectAddress(QStringLiteral("Bar_ctor")));
    // A C++ function links by the mangled symbol its label names, which is
    // what navigation looks up.
    QCOMPARE(code->linkedSymbol(QStringLiteral("Bar_ctor")),
             std::optional<QString>(QStringLiteral("_ZN3BarC1Ev")));
    QVERIFY(!code->linkedSymbol(QStringLiteral("QDomNode")));
    QCOMPARE(code->currentToken(), QStringLiteral("QDomNode"));
    code->setPreludeFolded(true);
    QCOMPARE(code->lineCount(), 702);

    // A list copies its selected rows, by key or from the Edit menu.
    auto &model = functions->model();
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    functions->table()->selectionModel()->select(
        QItemSelection(model.index(0, 0),
                       model.index(1, model.columnCount() - 1)),
        QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    clearClipboard();
    QTest::keyClick(functions->table(), Qt::Key_C, Qt::ControlModifier);
    const QString rows = clipboardText();
    QCOMPARE(rows.count(QLatin1Char('\n')), 1);
    QVERIFY2(rows.startsWith(QStringLiteral("function_0\t")), qPrintable(rows));
    QVERIFY2(rows.contains(QStringLiteral("\nfunction_1\t")), qPrintable(rows));
    clearClipboard();
    bench.action(ActionId::EditCopy)->trigger();
    QCOMPARE(clipboardText(), rows);

    // The graph of the current function.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x140));
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(disassembly->graphMode());
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->graph()->nodes().size(),
                              qsizetype(2), OpenTimeoutMs);
    bench.action(ActionId::ViewToggleGraph)->trigger();
    QVERIFY(!disassembly->graphMode());

    // Unicode comments save atomically beside the binary.
    bench.session.setComment(Base + 0x140, QString::fromUtf8("中文 تعليق"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(QString::fromUtf8(readAll(path + ".neverd/annotations.json"))
                .contains(QString::fromUtf8("中文 تعليق")));

    // Renames reach the function window.
    bench.session.rename(Base + 0x140, QStringLiteral("renamed"));
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("renamed"), OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("function_20"), OpenTimeoutMs);

    // P starts a function inside another; the cursor stays on its
    // instruction under the new function's header.
    const Address loose = Base + 0x148;
    disassembly->navigate(loose);
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditCreateFunction)->isEnabled());
    bench.action(ActionId::EditCreateFunction)->trigger();
    const QString looseName = QStringLiteral("sub_FFFF800012340148");
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(loose), OpenTimeoutMs);
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(loose));
    QTRY_VERIFY(bench.action(ActionId::EditDeleteFunction)->isEnabled());
    QVERIFY(!bench.action(ActionId::EditCreateFunction)->isEnabled());
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd/functions.json"),
                             OpenTimeoutMs);
    // Deleting it gives the instruction back to the function around it, and
    // undo brings the new function back.
    bench.action(ActionId::EditDeleteFunction)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(),
        QStringLiteral("function_21"), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(21).value("name").toString(), looseName,
        OpenTimeoutMs);

    // Alt+P opens IDA's Edit function dialog on the function the cursor is
    // in, with its name and bounds; what the user states there is an edit of
    // the history, kept beside the input.
    disassembly->navigate(Base + 0x140);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.action(ActionId::EditFunction)->isEnabled());
    const auto statedRows = [&] {
      return QJsonDocument::fromJson(readAll(path + ".neverd/functions.json"))
          .array();
    };
    const auto statesNoReturn = [&] {
      for (const auto &row : statedRows())
        if (addressValue(row.toObject().value("addr")) == Base + 0x140)
          return row.toObject().value("noreturn").toBool(false);
      return false;
    };
    // Answers the dialog once it is active: \p check sees its fields, then
    // \p accept closes it with OK, else Cancel.
    const auto answerEditor =
        [&](std::function<void(FunctionEditDialog *)> check, bool accept) {
          auto *editor = new QTimer(&bench.session);
          connect(editor, &QTimer::timeout, editor, [editor, check, accept] {
            auto *dialog = qobject_cast<FunctionEditDialog *>(
                QApplication::activeModalWidget());
            if (!dialog || !dialog->isActiveWindow())
              return;
            editor->stop();
            editor->deleteLater();
            check(dialog);
            if (accept)
              QTest::mouseClick(
                  dialog->findChild<QPushButton *>(QStringLiteral("ok")),
                  Qt::LeftButton);
            else
              dialog->reject();
          });
          editor->start(10);
        };
    bool sawFunction = false;
    answerEditor(
        [&](FunctionEditDialog *dialog) {
          auto *name = dialog->findChild<QLineEdit *>(QStringLiteral("name"));
          auto *start = dialog->findChild<QLineEdit *>(QStringLiteral("start"));
          auto *noReturn =
              dialog->findChild<QCheckBox *>(QStringLiteral("noreturn"));
          sawFunction =
              name && name->text() == QStringLiteral("function_20") && start &&
              start->text().endsWith(QStringLiteral("FFFF800012340140")) &&
              noReturn && !noReturn->isChecked() && noReturn->isEnabled();
          if (noReturn)
            noReturn->setChecked(true);
        },
        true);
    disassembly->setFocus();
    QTRY_VERIFY(disassembly->hasFocus());
    QTest::keyClick(disassembly, Qt::Key_P, Qt::AltModifier);
    QTRY_VERIFY_WITH_TIMEOUT(sawFunction, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(statesNoReturn(), OpenTimeoutMs);
    // Ctrl+E edits the function a row of the Functions window lists, as
    // IDA's list windows do, over the window's Ctrl+E (Jump to entry point).
    bool sawStated = false;
    answerEditor(
        [&](FunctionEditDialog *dialog) {
          auto *noReturn =
              dialog->findChild<QCheckBox *>(QStringLiteral("noreturn"));
          sawStated = noReturn && noReturn->isChecked();
        },
        false);
    // The closed dialog leaves no window active.
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    functions->table()->setFocus();
    QTRY_VERIFY(functions->table()->hasFocus());
    // The edit reloads the list; the row must be back before its key.
    QTRY_COMPARE_WITH_TIMEOUT(
        functions->model().rowObject(20).value("name").toString(),
        QStringLiteral("function_20"), OpenTimeoutMs);
    functions->table()->setCurrentIndex(functions->model().index(20, 0));
    QTest::keyClick(functions->table(), Qt::Key_E, Qt::ControlModifier);
    QTRY_VERIFY_WITH_TIMEOUT(sawStated, OpenTimeoutMs);
    QCOMPARE(disassembly->currentFunction(),
             std::optional<Address>(Base + 0x140));
    // Undo takes the statement back.
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!statesNoReturn(), OpenTimeoutMs);

    // D makes the data under the cursor a value, the next size each time
    // (stderr's qword gives way to a byte, then a word); U shows its bytes as
    // bytes, and undo takes U back.  Each commits at once.
    const Address object = Base + 0x3200;
    const auto items = [&] {
      return QString::fromUtf8(readAll(path + ".neverd/items.json"));
    };
    disassembly->navigate(object);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(), std::optional<Address>(object));
    QTRY_VERIFY(bench.action(ActionId::EditDefineData)->isEnabled());
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"byte\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditDefineData)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);
    bench.action(ActionId::EditUndefine)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"undefined\"")),
                             OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(items().contains(QLatin1String("\"word\"")),
                             OpenTimeoutMs);

    // H shows the number of the operand under the cursor, or else the
    // line's last number, in decimal at once; _ changes its sign and # goes
    // back to the listing's number.  Each commits at once.
    const Address instruction = Base + 0x51;
    const auto operandFormat = [&] {
      const auto rows =
          QJsonDocument::fromJson(readAll(path + ".neverd/operands.json"))
              .array();
      for (const auto &row : rows)
        for (const auto &entry : row.toObject().value("operands").toArray())
          return entry.toObject();
      return QJsonObject();
    };
    const auto shown = [&](const char *operands) {
      return disassembly->listing()->currentLineText().contains(
          QLatin1String(operands));
    };
    disassembly->navigate(instruction);
    disassembly->focusContent();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(instruction));
    QTRY_VERIFY(shown("rsp, 20h"));
    QTRY_VERIFY(bench.action(ActionId::EditOperandDecimal)->isEnabled());
    bench.action(ActionId::EditOperandDecimal)->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(operandFormat().value("base").toString(),
                              QStringLiteral("decimal"), OpenTimeoutMs);
    QCOMPARE(operandFormat().value("operand").toInt(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(shown("rsp, 32"), OpenTimeoutMs);
    bench.action(ActionId::EditOperandNegate)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(operandFormat().value("negate").toBool(),
                             OpenTimeoutMs);
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(!operandFormat().value("negate").toBool(),
                             OpenTimeoutMs);
    QCOMPARE(operandFormat().value("base").toString(),
             QStringLiteral("decimal"));
    bench.action(ActionId::EditOperandNumber)->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(operandFormat().isEmpty(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(shown("rsp, 20h"), OpenTimeoutMs);

    // A restarted worker reopens the file where the user was.  The worker
    // writes the sidecar before it answers, and an edit in flight asks to be
    // saved first.
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    const auto place = disassembly->currentItem();
    QVERIFY(place && *place != bench.session.entryAddress());
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(), place, OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void editFunctionRenamesAndStatesInTwoSteps() {
    // IDA's Edit function, offered by the disassembly's and the Functions
    // window's context menus.  The frame sizes and the flags NeverD does not
    // keep are shown disabled; a new name and the flags are two steps of the
    // history, undone and redone in turn, and a name the worker refuses
    // applies neither.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("fixture.bin"));
    QVERIFY(!path.isEmpty());
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    auto *disassembly = bench.window->disassembly();
    auto *functions = bench.functions();
    QTRY_COMPARE_WITH_TIMEOUT(functions->model().total(), 600, OpenTimeoutMs);
    const Address entry = Base + 0x140;
    const auto listedName = [&] {
      return functions->model().rowObject(20).value("name").toString();
    };
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("function_20"),
                              OpenTimeoutMs);
    disassembly->navigate(entry);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentFunction(),
                              std::optional<Address>(entry), OpenTimeoutMs);
    QAction *edit = bench.action(ActionId::EditFunction);
    QTRY_VERIFY(edit->isEnabled());

    // The enabled entries of the context menu \p open shows, then closed.
    const auto offered = [&](const std::function<void()> &open) {
      QStringList texts;
      QTimer probe;
      connect(&probe, &QTimer::timeout, this, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu)
          return;
        probe.stop();
        for (const QAction *action : menu->actions())
          if (action->isEnabled() && !action->isSeparator())
            texts << action->text();
        menu->close();
      });
      probe.start(5);
      open();
      return texts;
    };
    QVERIFY(offered([&] {
              emit disassembly->contextMenuRequested(
                  disassembly->mapToGlobal(QPoint(40, 40)));
            }).contains(edit->text()));
    const QModelIndex row = functions->model().index(20, 0);
    functions->table()->scrollTo(row);
    QVERIFY(offered([&] {
              emit functions->table()->customContextMenuRequested(
                  functions->table()->visualRect(row).center());
            }).contains(edit->text()));

    const auto stated = [&] {
      for (const auto &row :
           QJsonDocument::fromJson(readAll(path + ".neverd/functions.json"))
               .array())
        if (addressValue(row.toObject().value("addr")) == entry)
          return row.toObject();
      return QJsonObject();
    };
    // Answers the dialog once it is active: \p check reads and sets its
    // fields, then OK closes it.
    const auto answerEditor =
        [&](std::function<void(FunctionEditDialog *)> check) {
          auto *editor = new QTimer(&bench.session);
          connect(editor, &QTimer::timeout, editor, [editor, check] {
            auto *dialog = qobject_cast<FunctionEditDialog *>(
                QApplication::activeModalWidget());
            if (!dialog || !dialog->isActiveWindow())
              return;
            editor->stop();
            editor->deleteLater();
            check(dialog);
            QTest::mouseClick(
                dialog->findChild<QPushButton *>(QStringLiteral("ok")),
                Qt::LeftButton);
          });
          editor->start(10);
        };
    const auto field = [](FunctionEditDialog *dialog, const char *name) {
      return dialog->findChild<QLineEdit *>(QLatin1String(name));
    };
    const auto flag = [](FunctionEditDialog *dialog, const char *name) {
      return dialog->findChild<QCheckBox *>(QLatin1String(name));
    };

    QStringList unmodeled;
    int shownOnly = 0;
    answerEditor([&](FunctionEditDialog *dialog) {
      for (const QCheckBox *box : dialog->findChildren<QCheckBox *>())
        if (!box->isEnabled())
          unmodeled << box->text();
      for (const QLineEdit *line : dialog->findChildren<QLineEdit *>())
        shownOnly += line->isReadOnly();
      field(dialog, "name")->setText(QStringLiteral("parse_header"));
      flag(dialog, "library")->setChecked(true);
      flag(dialog, "static")->setChecked(true);
    });
    edit->trigger();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(stated().value("library").toBool() &&
                                 stated().value("static").toBool(),
                             OpenTimeoutMs);
    QCOMPARE(unmodeled, (QStringList{QStringLiteral("&Far function"),
                                     QStringLiteral("&BP based frame"),
                                     QStringLiteral("BP e&quals to SP"),
                                     QStringLiteral("Fuzzy SP"),
                                     QStringLiteral("Outlined code")}));
    // The bounds and the four frame sizes.
    QCOMPARE(shownOnly, 6);

    // Undo takes the flags back, then the name; redo makes them again.
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_VERIFY_WITH_TIMEOUT(stated().isEmpty(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canUndo());
    bench.session.undo();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("function_20"),
                              OpenTimeoutMs);
    QTRY_VERIFY(bench.session.canRedo());
    bench.session.redo();
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QVERIFY(stated().isEmpty());
    QTRY_VERIFY(bench.session.canRedo());
    bench.session.redo();
    QTRY_VERIFY_WITH_TIMEOUT(stated().value("library").toBool(), OpenTimeoutMs);

    // A name the worker refuses, here one in an automatic form, is refused
    // with the flag.
    QSignalSpy messages(&bench.session, &Session::message);
    answerEditor([&](FunctionEditDialog *dialog) {
      field(dialog, "name")->setText(QStringLiteral("sub_10"));
      flag(dialog, "noreturn")->setChecked(true);
    });
    edit->trigger();
    const auto refused = [&] {
      for (const auto &arguments : messages)
        if (arguments.at(0).toString().contains(
                QLatin1String("is an automatic name")))
          return true;
      return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(refused(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(listedName(), QStringLiteral("parse_header"),
                              OpenTimeoutMs);
    QVERIFY(!stated().contains(QLatin1String("noreturn")));

    // Default takes a stated color away.
    bench.session.editFunction(entry, {{"color", "#20A0FF"}});
    QTRY_COMPARE_WITH_TIMEOUT(stated().value("color").toString(),
                              QStringLiteral("#20A0FF"), OpenTimeoutMs);
    QString shownColor;
    answerEditor([&](FunctionEditDialog *dialog) {
      auto *color = dialog->findChild<QPushButton *>(QStringLiteral("color"));
      shownColor = color->text();
      for (QAction *action : color->menu()->actions())
        if (action->text() == QStringLiteral("&Default"))
          action->trigger();
    });
    edit->trigger();
    QTRY_VERIFY_WITH_TIMEOUT(!stated().contains(QLatin1String("color")),
                             OpenTimeoutMs);
    QCOMPARE(shownColor, QStringLiteral("#20A0FF"));
    QVERIFY(stated().value("library").toBool());
  }

  void sessionChangesAreExplicit() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("first revision"));
    session.setComment(Base, QStringLiteral("second revision"));
    session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(first + ".neverd/annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!session.dirty());
    QVERIFY(readAll(first + ".neverd/annotations.json")
                .contains("second revision"));

    // Opening another file with unsaved edits asks first.
    session.setComment(Base + 0x140, QStringLiteral("comment on A"));
    QTRY_VERIFY(session.dirty());
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    QCOMPARE(confirmation.size(), 1);
    QCOMPARE(session.filePath(), first);
    session.resolveTransition(QStringLiteral("cancel"));
    QCOMPARE(session.filePath(), first);
    session.open(second);
    QCOMPARE(confirmation.size(), 2);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), second, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(
        readAll(first + ".neverd/annotations.json").contains("comment on A"));

    // An interrupted external read gets a terminal reply.
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-restart"),
                          QStringLiteral("metadata"), {}, {});
    session.restart();
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first().at(0).toString(),
             QStringLiteral("external-restart"));
    const auto status = replies.first().at(1).toJsonObject();
    QVERIFY2(status.value("status").toString() == QLatin1String("ok") ||
                 status.value("error").toObject().value("code").toString() ==
                     QLatin1String("worker_stopped"),
             QJsonDocument(status).toJson().constData());
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);

    // Quitting with unsaved edits saves first when asked to.
    session.setComment(Base, QStringLiteral("keep before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    QVERIFY(readAll(second + ".neverd/annotations.json")
                .contains("keep before quit"));
  }

  void editsForAnOlderSessionAreRefused() {
    QTemporaryDir directory;
    const auto first = writeFixture(directory, QStringLiteral("first.bin"));
    const auto second = writeFixture(directory, QStringLiteral("second.bin"));
    const auto third = writeFixture(directory, QStringLiteral("third.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(first);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    const quint64 oldEpoch = session.epoch();
    QSignalSpy confirmation(&session, &Session::transitionRequested);
    session.open(second);
    session.open(third);
    QTRY_COMPARE_WITH_TIMEOUT(session.filePath(), third, OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QCOMPARE(confirmation.size(), 0);
    QVERIFY(session.epoch() != oldEpoch);
    QSignalSpy messages(&session, &Session::message);
    session.setComment(Base, QStringLiteral("must not enter new project"),
                       oldEpoch);
    QVERIFY(!session.dirty());
    QVERIFY(!messages.isEmpty());
    QCOMPARE(messages.last().at(1).toInt(), 1);
  }

  void cancellingViewReadsStillCompletesExternalQueries() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("external.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject view;
    bool viewReplied = false;
    session.read(QStringLiteral("decompile"),
                 {{"address", hexAddress(Base)}, {"representation", "low"}},
                 &view, [&](const QJsonObject &) { viewReplied = true; });
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(QStringLiteral("external-survives-cancel"),
                          QStringLiteral("metadata"), {}, {});
    session.cancelReads();
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    QCOMPARE(replies.first().first().toString(),
             QStringLiteral("external-survives-cancel"));
    QCOMPARE(replies.first().at(1).toJsonObject().value("status").toString(),
             QStringLiteral("ok"));
    QTest::qWait(200);
    QVERIFY(!viewReplied);
    replies.clear();
    session.externalQuery(QStringLiteral("exact-stale"),
                          QStringLiteral("metadata"), {},
                          QStringLiteral("invalid-revision"));
    QTRY_COMPARE(replies.size(), 1);
    QCOMPARE(replies.first()
                 .at(1)
                 .toJsonObject()
                 .value("error")
                 .toObject()
                 .value("code")
                 .toString(),
             QStringLiteral("stale_revision"));
  }

  void cancellingViewReadsPreservesAnExternalAnalysisSnapshot() {
    QTemporaryDir directory;
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    QStringList diagnostics;
    connect(&session, &Session::message, this,
            [&](const QString &text, int) { diagnostics.append(text); });
    session.open(
        writeFixture(directory, QStringLiteral("pseudocode-parallel.bin")));
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QObject view;
    // Hold the project dispatcher busy so the replica's snapshot remains
    // queued when Cancel is pressed, rather than relying on a timing race.
    session.queries().subscribe(
        {"decompile",
         {{"address", hexAddress(Base)}, {"representation", "source"}}},
        &view, [](const QJsonObject &) {});
    QTRY_VERIFY_WITH_TIMEOUT(
        diagnostics.join('\n').contains("fixture parallel decompile " +
                                        hexAddress(Base) + " started"),
        OpenTimeoutMs);
    QSignalSpy replies(&session, &Session::externalResponse);
    session.externalQuery(
        "external-analysis", "decompile",
        {{"address", hexAddress(Base + 32)}, {"representation", "source"}}, {});
    QTRY_COMPARE_WITH_TIMEOUT(session.queries().pendingReadCount(), 2, 1000);
    session.cancelReads();
    QTRY_COMPARE_WITH_TIMEOUT(replies.size(), 1, OpenTimeoutMs);
    QCOMPARE(replies.front()[0].toString(),
             QStringLiteral("external-analysis"));
    QCOMPARE(replies.front()[1].toJsonObject().value("status").toString(),
             QStringLiteral("ok"));
  }

  void saveBeforeQuitRejectsALateEdit() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("transition.bin"));
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    session.setComment(Base, QStringLiteral("accepted before quit"));
    QTRY_VERIFY(session.dirty());
    QVERIFY(!session.requestQuit());
    QSignalSpy approved(&session, &Session::quitApproved);
    session.resolveTransition(QStringLiteral("save"));
    session.setComment(Base + 0x10,
                       QStringLiteral("late edit after save intent"));
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    QVERIFY(!session.dirty());
    const auto bytes = readAll(path + ".neverd/annotations.json");
    QVERIFY(bytes.contains("accepted before quit"));
    QVERIFY(!bytes.contains("late edit after save intent"));
  }

  void cDefinesInstructionsAndDatabaseKeepsThem_data() {
    QTest::addColumn<bool>("native");
    QTest::newRow("fixture") << false;
    if (!qEnvironmentVariable("NEVERD_CODE_NAV_WORKER").isEmpty() &&
        !qEnvironmentVariable("NEVERD_MAKE_CODE_FILE").isEmpty())
      QTest::newRow("native") << true;
  }

  void cDefinesInstructionsAndDatabaseKeepsThem() {
    QFETCH(bool, native);
    QTemporaryDir directory;
    const auto path = directory.filePath(QStringLiteral("make-code.bin"));
    const auto worker = native ? qEnvironmentVariable("NEVERD_CODE_NAV_WORKER")
                               : QString::fromLocal8Bit(TEST_WORKER);
    if (native)
      QVERIFY(QFile::copy(qEnvironmentVariable("NEVERD_MAKE_CODE_FILE"), path));
    else
      writeFixture(directory, QStringLiteral("make-code.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    const Address entry = native ? Address(0x400078) : Base;
    const Address start = native ? entry + 6 : Base + 0x2700;
    {
      Workbench bench(worker);
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
      auto *view = bench.window->disassembly();
      view->navigate(start);
      view->focusContent();
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start));
      QTRY_VERIFY(view->listing()->hasFocus());
      QTRY_VERIFY(bench.action(ActionId::EditDefineCode)->isEnabled());
      QCOMPARE(bench.action(ActionId::EditDefineCode)->shortcut(),
               QKeySequence("C"));
      QTest::keyClick(view->listing(), Qt::Key_C);
      const auto rows = [&] {
        return QJsonDocument::fromJson(readAll(path + ".neverd/items.json"))
            .array();
      };
      QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 3, OpenTimeoutMs);
      QTRY_VERIFY(view->listing()->currentLineText().contains("nop"));
      QCOMPARE(view->currentItem(), std::optional<Address>(start));
      QVERIFY(!view->currentFunction());
      QTRY_VERIFY(bench.session.canUndo());
      bench.session.undo();
      QTRY_VERIFY_WITH_TIMEOUT(rows().isEmpty(), OpenTimeoutMs);
      QTRY_VERIFY(view->listing()->currentLineText().contains("db"));
      QTRY_VERIFY(bench.session.canRedo());
      bench.session.redo();
      QTRY_COMPARE_WITH_TIMEOUT(rows().size(), 3, OpenTimeoutMs);
      view->navigate(start + 1);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start + 1));
      QTRY_VERIFY(view->listing()->currentLineText().contains("add"));
      if (const auto capture =
              qEnvironmentVariable("NEVERD_CODE_NAV_CAPTURE_DIR");
          !capture.isEmpty()) {
        QDir().mkpath(capture);
        QVERIFY(bench.window->grab().save(
            capture +
            (native ? "/make-code-native.png" : "/make-code-fixture.png")));
      }
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      // C is not consumed as a data definition in the source window.
      view->navigate(entry);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(entry));
      QTRY_COMPARE_WITH_TIMEOUT(view->currentFunction(),
                                std::optional<Address>(entry), OpenTimeoutMs);
      QTRY_VERIFY(bench.action(ActionId::ViewPseudocode)->isEnabled());
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *source = bench.codeView(QStringLiteral("source"));
      QVERIFY(source);
      QTRY_VERIFY(source->isVisible() && !source->text()->loading());
      source->text()->window()->activateWindow();
      QVERIFY(QTest::qWaitForWindowActive(source->text()->window()));
      source->text()->setFocus();
      QTRY_VERIFY(source->text()->hasFocus());
      QTRY_VERIFY(!bench.action(ActionId::EditDefineCode)->isEnabled());
    }
    {
      Workbench bench(worker);
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      auto *view = bench.window->disassembly();
      view->navigate(start + 1);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(start + 1));
      QTRY_VERIFY(view->listing()->currentLineText().contains("add"));
      QVERIFY(!view->currentFunction());
    }
  }

  /// The rows of the bookmarks window, which Jump to mark opens.
  static QStringList bookmarkTexts(Workbench &bench) {
    bench.action(ActionId::JumpMark)->trigger();
    for (auto *view : bench.window->findChildren<ChooserView *>())
      if (view->model().kind() == ChooserKind::Bookmarks) {
        QStringList texts;
        for (int row = 0; row < view->model().rowCount(); ++row)
          texts << view->model()
                       .data(view->model().index(row, 1), Qt::DisplayRole)
                       .toString();
        return texts;
      }
    return {};
  }

  /// Mark the current position as \p text, as Mark position does.
  bool markPosition(Workbench &bench, const QString &text) {
    QTimer answer;
    bool accepted = false;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      if (auto *dialog =
              qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
        dialog->setTextValue(text);
        accepted = true;
        dialog->accept();
      }
    });
    answer.start(5);
    if (!QTest::qWaitFor(
            [&] { return bench.action(ActionId::EditBookmark)->isEnabled(); },
            OpenTimeoutMs))
      return false;
    bench.action(ActionId::EditBookmark)->trigger();
    return QTest::qWaitFor([&] { return accepted; }, OpenTimeoutMs);
  }

  QString commentAt(Workbench &bench, Address address) {
    QString comment;
    bool read = false;
    bench.session.read(QStringLiteral("resolve"),
                       {{"query", hexAddress(address)}}, &bench.session,
                       [&](const QJsonObject &payload) {
                         comment = payload.value("comment").toString();
                         read = true;
                       });
    if (!QTest::qWaitFor([&] { return read; }, OpenTimeoutMs))
      return QStringLiteral("(no reply)");
    return comment;
  }

  void projectFilesTravelWithTheBinary() {
    // Saving keeps a binary's project in JSON files beside it and writes no
    // database: a copy of the binary with the shared files, as a git clone
    // has, opens the same analysis.  Where the user was is the user's own,
    // in a file a repository ignores.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("shared.bin"));
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      auto *view = bench.window->disassembly();
      view->navigate(Base + 0x140);
      QTRY_COMPARE(view->currentItem(), std::optional<Address>(Base + 0x140));
      QVERIFY(markPosition(bench, QString::fromUtf8("共享标记")));
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QString::fromUtf8("共享标记")});
      // The file the command line's bookmarks command keeps too.
      const auto bookmarks = readAll(path + ".neverd/bookmarks.json");
      QVERIFY2(bookmarks.startsWith("[\n  {\n    \"addr\": \"" +
                                    hexAddress(Base + 0x140).toUtf8() +
                                    "\",\n    \"name\": \""),
               bookmarks.constData());
      QVERIFY(bookmarks.contains(QString::fromUtf8("共享标记").toUtf8()));
      QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
      bench.session.setComment(Base + 0x140, QStringLiteral("shared comment"));
      QTRY_VERIFY(bench.session.dirty());
      QVERIFY(bench.action(ActionId::FileSave)->isEnabled());
      bench.session.save();
      QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
      QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
      QVERIFY(readAll(path + ".neverd/annotations.json")
                  .startsWith("[\n  {\n    \"addr\": "));
      QVERIFY(bench.window->close());
    }
    QVERIFY(QDir(directory.path())
                .entryList({QStringLiteral("*.nddb")}, QDir::Files)
                .isEmpty());
    const auto workbench =
        QJsonDocument::fromJson(readAll(path + ".neverd/local/workbench.json"))
            .object();
    QCOMPARE(workbench.value("location").toObject().value("address").toString(),
             hexAddress(Base + 0x140));

    // The project is one folder beside the binary.  It keeps each user's
    // own files in local/, which its own .gitignore leaves out, and asks
    // git to merge the rest row by row.
    const QDir project(path + ".neverd");
    QVERIFY(readAll(project.filePath(QStringLiteral(".gitignore")))
                .contains("/local/"));
    QVERIFY(readAll(project.filePath(QStringLiteral(".gitattributes")))
                .contains("*.json merge=neverd"));
    QVERIFY(QFileInfo(project.filePath(QStringLiteral("local"))).isDir());
    QVERIFY(QDir(directory.path())
                .entryList({QStringLiteral("shared.bin.neverd-*")}, QDir::Files)
                .isEmpty());

    // The clone: the binary and the project folder as git carries it,
    // without local/.
    QTemporaryDir clone;
    const auto copy = clone.filePath(QStringLiteral("shared.bin"));
    QVERIFY(QFile::copy(path, copy));
    QVERIFY(QDir().mkpath(copy + ".neverd"));
    for (const auto &name :
         project.entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot))
      QVERIFY(QFile::copy(project.filePath(name),
                          QDir(copy + ".neverd").filePath(name)));
    {
      Workbench bench;
      bench.window->openFile(copy);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(commentAt(bench, Base + 0x140),
               QStringLiteral("shared comment"));
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QString::fromUtf8("共享标记")});
      QVERIFY(!bench.session.dirty());
      QVERIFY(bench.window->close());
    }
    // With the user's own workbench file, the place comes back too.
    QFile::remove(copy + ".neverd/local/workbench.json");
    QVERIFY(QFile::copy(path + ".neverd/local/workbench.json",
                        copy + ".neverd/local/workbench.json"));
    Workbench bench;
    bench.window->openFile(copy);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentItem(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
  }

  void anOpenedDatabaseSavesBackAndAsksBeforeLosingEdits() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("packed.bin"));
    const auto database = directory.filePath(QStringLiteral("team.nddb"));
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QVERIFY(bench.action(ActionId::FilePackDatabase)->isEnabled());
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
    }
    const auto databaseRenames = [&] {
      QString error;
      const auto contents = ProjectDatabase::read(database, &error);
      return contents ? contents->sidecars.value(
                            QStringLiteral(".neverd-renames.json"))
                      : QByteArray();
    };
    Workbench bench;
    bench.window->openFile(database);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(bench.session.openedFromDatabase());
    // Opening it changes nothing that needs saving.
    QVERIFY(!bench.session.dirty());
    QVERIFY(!bench.action(ActionId::FileSave)->isEnabled());
    // An edit is kept beside the unpacked copy at once, and the database
    // lacks it until Save writes it back.
    bench.session.rename(Base + 0x140, QStringLiteral("first_name"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.databaseOutOfDate(), OpenTimeoutMs);
    QVERIFY(bench.session.dirty());
    QVERIFY(bench.action(ActionId::FileSave)->isEnabled());
    QVERIFY(!databaseRenames().contains("first_name"));
    QSignalSpy saved(&bench.session, &Session::databaseSaved);
    bench.session.save();
    QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
    QVERIFY(saved.first().first().toBool());
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(databaseRenames().contains("first_name"));
    QVERIFY(!QFileInfo::exists(ProjectDatabase::pathFor(path)));

    // Quitting asks before such an edit is lost; discarding it leaves the
    // database as it was.
    bench.session.rename(Base + 0x140, QStringLiteral("second_name"));
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.databaseOutOfDate(), OpenTimeoutMs);
    QString question;
    QTimer answer;
    connect(&answer, &QTimer::timeout, &answer, [&] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        question = box->text();
        box->button(QMessageBox::Discard)->click();
      }
    });
    answer.start(5);
    QSignalSpy approved(&bench.session, &Session::quitApproved);
    QVERIFY(!bench.session.requestQuit());
    QTRY_COMPARE_WITH_TIMEOUT(approved.size(), 1, OpenTimeoutMs);
    answer.stop();
    QVERIFY2(question.contains(QStringLiteral("team.nddb")),
             qPrintable(question));
    QVERIFY(databaseRenames().contains("first_name"));
    QVERIFY(!databaseRenames().contains("second_name"));
  }

  void projectFilesChangedOnDiskAreTaken() {
    // git pull, another tool or an AI agent changes the project's files
    // while the project is open: the workbench takes them as they are, and
    // a comment not saved yet stays on top of them.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("pulled.bin"));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    // An edit of our own leaves the undo history that a pull overtakes.
    bench.session.rename(Base + 0x140, QStringLiteral("ours"));
    QTRY_VERIFY_WITH_TIMEOUT(
        readAll(path + ".neverd/renames.json").contains("\"ours\""),
        OpenTimeoutMs);
    bench.session.setComment(Base, QStringLiteral("staged here"));
    QTRY_VERIFY(bench.session.dirty());

    // A teammate's rename and comment arrive, and a bookmark.
    const auto replace = [](const QString &file, const QByteArray &text) {
      QSaveFile out(file);
      return out.open(QIODevice::WriteOnly) && out.write(text) == text.size() &&
             out.commit();
    };
    const auto rows = [](const QJsonObject &row) {
      return QJsonDocument(QJsonArray{row}).toJson();
    };
    const QString at = hexAddress(Base + 0x140);
    QVERIFY(replace(path + ".neverd/renames.json",
                    rows({{"addr", at},
                          {"original", "function_20"},
                          {"renamed", "theirs"}})));
    QVERIFY(replace(path + ".neverd/annotations.json",
                    rows({{"addr", at}, {"text", "their note"}})));
    QVERIFY(
        replace(path + ".neverd/bookmarks.json",
                rows({{"addr", at}, {"name", "their mark"}, {"note", ""}})));
    const auto resolved = [&](Address address) {
      QJsonObject result;
      bool read = false;
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", hexAddress(address)}}, &bench.session,
                         [&](const QJsonObject &payload) {
                           result = payload;
                           read = true;
                         });
      QTest::qWaitFor([&] { return read; }, OpenTimeoutMs);
      return result;
    };
    QTRY_COMPARE_WITH_TIMEOUT(
        resolved(Base + 0x140).value("comment").toString(),
        QStringLiteral("their note"), OpenTimeoutMs);
    QCOMPARE(resolved(Base).value("comment").toString(),
             QStringLiteral("staged here"));
    QVERIFY(bench.session.dirty());
    QTRY_COMPARE(bookmarkTexts(bench),
                 QStringList{QStringLiteral("their mark")});
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains(
        QStringLiteral("changed on disk and were loaded")));

    // Edits go on from the files as they are now, and saving keeps both.
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    const auto notes = readAll(path + ".neverd/annotations.json");
    QVERIFY2(notes.contains("their note") && notes.contains("staged here"),
             notes.constData());
    bench.session.rename(Base + 0x140, QStringLiteral("after_pull"));
    QTRY_VERIFY_WITH_TIMEOUT(
        readAll(path + ".neverd/renames.json").contains("after_pull"),
        OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
  }

  void filesBesideTheBinaryMoveIntoItsProjectFolder() {
    // An older NeverD kept the project as files beside the binary: opening
    // it moves them into the binary's project folder, and they read as
    // before.
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("older.bin"));
    const auto rows = [](const QJsonObject &row) {
      return QJsonDocument(QJsonArray{row}).toJson();
    };
    const QString at = hexAddress(Base + 0x140);
    const auto write = [](const QString &file, const QByteArray &text) {
      QFile out(file);
      return out.open(QIODevice::WriteOnly) && out.write(text) == text.size();
    };
    QVERIFY(write(path + ".neverd-annotations.json",
                  rows({{"addr", at}, {"text", "older note"}})));
    QVERIFY(write(path + ".neverd-bookmarks.json",
                  rows({{"addr", at}, {"name", "older mark"}, {"note", ""}})));
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QVERIFY(!QFileInfo::exists(path + ".neverd-annotations.json"));
    QVERIFY(!QFileInfo::exists(path + ".neverd-bookmarks.json"));
    QVERIFY(readAll(path + ".neverd/annotations.json").contains("older note"));
    QCOMPARE(commentAt(bench, Base + 0x140), QStringLiteral("older note"));
    QCOMPARE(bookmarkTexts(bench), QStringList{QStringLiteral("older mark")});
  }

  void bookmarksFromBeforeProjectsKeptThemStillShow() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("legacy.bin"));
    // A database from before keeps its bookmarks and debug file choices in
    // its state alone.
    const auto database = directory.filePath(QStringLiteral("legacy.nddb"));
    QCOMPARE(ProjectDatabase::save(
                 database, path,
                 {{QStringLiteral("bookmarks"),
                   QJsonDocument(QJsonArray{QJsonObject{
                                     {"address", hexAddress(Base + 0x140)},
                                     {"text", "from the database"}}})
                       .toJson(QJsonDocument::Compact)},
                  {QStringLiteral("dwarf-input.json"), R"({"mode":"none"})"}}),
             QString());
    {
      Workbench bench;
      bench.window->openFile(database);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(bookmarkTexts(bench),
               QStringList{QStringLiteral("from the database")});
      // Its choices now have a file beside the unpacked copy too, which the
      // database lacks without anything to save: opening changed nothing.
      QCOMPARE(QJsonDocument::fromJson(
                   readAll(bench.session.filePath() + ".neverd/debug.json"))
                   .object()
                   .value("dwarf")
                   .toObject()
                   .value("mode")
                   .toString(),
               QStringLiteral("none"));
      QVERIFY(!bench.session.dirty());
    }
    // This machine's settings kept a binary's bookmarks.
    QSettings().setValue(
        QStringLiteral("bookmarks/") +
            QString::fromLatin1(QCryptographicHash::hash(
                                    path.toUtf8(), QCryptographicHash::Sha1)
                                    .toHex()),
        QVariantList{QVariantMap{{"address", hexAddress(Base)},
                                 {"text", "from the settings"}}});
    Workbench bench;
    bench.window->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QCOMPARE(bookmarkTexts(bench),
             QStringList{QStringLiteral("from the settings")});
    QVERIFY(!QFileInfo::exists(path + ".neverd/bookmarks.json"));
    // The next mark keeps them all with the project.
    auto *view = bench.window->disassembly();
    view->navigate(Base + 0x140);
    view->focusContent();
    QTRY_COMPARE(view->currentItem(), std::optional<Address>(Base + 0x140));
    QVERIFY(markPosition(bench, QStringLiteral("made now")));
    const auto rows =
        QJsonDocument::fromJson(readAll(path + ".neverd/bookmarks.json"))
            .array();
    QCOMPARE(rows.size(), 2);
    QCOMPARE(rows.at(0).toObject().value("name").toString(),
             QStringLiteral("from the settings"));
    QCOMPARE(rows.at(1).toObject().value("name").toString(),
             QStringLiteral("made now"));
    // Marking a marked address again renames its bookmark.
    QVERIFY(markPosition(bench, QStringLiteral("renamed mark")));
    QCOMPARE(bookmarkTexts(bench),
             (QStringList{QStringLiteral("from the settings"),
                          QStringLiteral("renamed mark")}));
  }

  void databaseCarriesTheProject() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("project.bin"));
    const auto database = ProjectDatabase::pathFor(path);
    {
      Workbench bench;
      bench.window->openFile(path);
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      bench.window->disassembly()->navigate(Base + 0x140);
      QTRY_COMPARE(bench.window->disassembly()->currentItem(),
                   std::optional<Address>(Base + 0x140));
      bench.action(ActionId::ViewPseudocode)->trigger();
      auto *view = bench.codeView(QStringLiteral("source"));
      QVERIFY(view);
      QTRY_VERIFY(view->isVisible() && !view->text()->loading());
      bench.session.setComment(Base + 0x140, QStringLiteral("packed comment"));
      QTRY_VERIFY(bench.session.dirty());
      // Packing commits the staged comment first.
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.packDatabase(database);
      QTRY_COMPARE_WITH_TIMEOUT(saved.size(), 1, OpenTimeoutMs);
      QVERIFY(saved.first().first().toBool());
      QVERIFY(QFileInfo::exists(database));
      QTRY_VERIFY(!bench.session.dirty());
    }
    // The database alone reopens the project: input, comment and location.
    // Its header identifies it, so a copy without the suffix opens too.
    for (const auto &name :
         {QStringLiteral("moved.nddb"), QStringLiteral("moved.db")}) {
      QTemporaryDir moved;
      const auto copy = moved.filePath(name);
      QVERIFY(QFile::copy(database, copy));
      Workbench bench;
      QVERIFY(dropFile(bench.window.get(), copy));
      QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
      QCOMPARE(bench.session.projectPath(), copy);
      QCOMPARE(bench.session.databasePath(), copy);
      QVERIFY(bench.session.filePath() != path);
      QTRY_COMPARE_WITH_TIMEOUT(bench.window->disassembly()->currentItem(),
                                std::optional<Address>(Base + 0x140),
                                OpenTimeoutMs);
      QTest::qWait(250);
      if (auto *view = bench.codeView(QStringLiteral("source"))) {
        QVERIFY(!view->isVisible());
        QVERIFY(!view->text()->function());
      }
      QString comment;
      bench.session.read(QStringLiteral("resolve"),
                         {{"query", hexAddress(Base + 0x140)}}, &bench.session,
                         [&](const QJsonObject &payload) {
                           comment = payload.value("comment").toString();
                         });
      QTRY_COMPARE(comment, QStringLiteral("packed comment"));
    }
  }

  void contributionsRegisterRunAndUnload() {
    QTemporaryDir directory;
    const auto path = writeFixture(directory, QStringLiteral("contrib.bin"));
    const auto manifest = directory.filePath(QStringLiteral("manifest.json"));
    {
      QFile file(manifest);
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write(
          R"({"schema_version": 1, "namespace": "sample", "version": "1.0",
        "contributions": [{"id": "sample:selected-code", "title": "Selected instructions",
        "kind": "panel", "query": {"operation": "disasm",
        "payload": {"address": "${address}", "limit": 3}}}]})");
    }
    Session session(QString::fromLocal8Bit(TEST_WORKER));
    session.open(path);
    QTRY_VERIFY_WITH_TIMEOUT(session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!session.contributions().isEmpty(), OpenTimeoutMs);
    const auto builtIn = session.contributions().size();
    session.registerContributions(manifest);
    QTRY_COMPARE(session.contributions().size(), builtIn + 1);
    QSignalSpy results(&session, &Session::contributionResult);
    session.executeContribution(QStringLiteral("sample:selected-code"), Base);
    QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, OpenTimeoutMs);
    const auto result = results.first().first().toJsonObject();
    QCOMPARE(result.value("contribution_id").toString(),
             QStringLiteral("sample:selected-code"));
    QCOMPARE(result.value("result").toObject().value("items").toArray().size(),
             3);
    session.unregisterContributions(QStringLiteral("sample"));
    QTRY_COMPARE(session.contributions().size(), builtIn);
  }
};

QTEST_MAIN(WorkbenchTests)
#include "WorkbenchTests.moc"
