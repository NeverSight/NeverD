// Workbench controller tests against the fixture worker: the session, the
// production main window and its views, edits and session transitions.
#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "Docking.h"
#include "ExtensionsView.h"
#include "GraphView.h"
#include "HexView.h"
#include "ListingView.h"
#include "LoadFileDialog.h"
#include "MainWindow.h"
#include "OutputWindow.h"
#include "ProjectDatabase.h"
#include "QuickStartDialog.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "mcp/GuiSessionBroker.h"
#include "mcp/McpConnectionManager.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFontMetricsF>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScopeGuard>
#include <QScrollBar>
#include <QSettings>
#include <QSignalSpy>
#include <QSizeGrip>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
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

  explicit Workbench(QString worker = QString::fromLocal8Bit(TEST_WORKER))
      : session(std::move(worker)) {
    window = std::make_unique<MainWindow>(session, mcp, broker);
    window->setLayoutPath(layout.filePath(QStringLiteral("layout.json")));
    window->resize(1400, 900);
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
/// Accepts every "Load a new file" dialog with its defaults while it lives,
/// as pressing Enter does.
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
    const auto initialAssembly = assembly->currentAddress();
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
    // Clicks and arrow keys select source rows without moving assembly.
    if (!native) {
      const int height = int(
          std::ceil(QFontMetricsF(Theme::instance().codeFont()).lineSpacing()));
      QTest::mouseClick(code->viewport(), Qt::LeftButton, {},
                        QPoint(100, 2 * height + height / 2));
      QCOMPARE(code->currentAddress(), target);
    }
    QTest::keyClick(code, Qt::Key_Right);
    QTest::qWait(250);
    QCOMPARE(assembly->currentAddress(), initialAssembly);
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
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(path + ".nddb"), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
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
    QCOMPARE(readAll(unpacked + ".neverd-code.json"),
             readAll(path + ".neverd-code.json"));
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
    QVERIFY(!QFile::exists(path + ".neverd-annotations.json"));
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

    QuickStartDialog dialog;
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

  void quickStartResizesAndRemembersItsSize() {
    QSettings().remove(settings::QuickStartSize);
    const auto restore =
        qScopeGuard([] { QSettings().remove(settings::QuickStartSize); });
    QuickStartDialog dialog;
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

    QuickStartDialog reopened;
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
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd-annotations.json"),
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
    const auto saved = readAll(path + ".neverd-annotations.json");
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
    QVERIFY2(copy.startsWith(data.path()), qPrintable(copy));
    QCOMPARE(readAll(copy), readAll(path));
    auto *log = bench.window->findChild<OutputWindow *>()
                    ->findChild<QPlainTextEdit *>();
    QVERIFY(log);
    QTRY_VERIFY(log->toPlainText().contains(
        QStringLiteral("is in a folder you cannot write to")));

    bench.session.setComment(Base, QStringLiteral("kept elsewhere"));
    QTRY_VERIFY(bench.session.dirty());
    bench.session.save();
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(copy + ".neverd-annotations.json"),
                             OpenTimeoutMs);
    QVERIFY(!QFile::exists(path + ".neverd-annotations.json"));
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
        readAll(copy + ".neverd-annotations.json").contains("kept elsewhere"));
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
    auto *code = bench.codeView(QStringLiteral("source"));
    auto *extensions = bench.window->findChild<ExtensionsView *>();
    QVERIFY(code && extensions && bench.functions());
    // Every button shows, as the fold button does for library code.
    for (auto *button : code->findChildren<QToolButton *>())
      button->show();
    const QString wide(400, QLatin1Char('x'));
    const QList<QWidget *> views{bench.window->disassembly(), code,
                                 bench.functions(), extensions};
    for (auto *view : views) {
      for (auto *label : view->findChildren<QLabel *>())
        label->setText(wide);
      QVERIFY2(view->minimumSizeHint().width() <= NarrowViewWidth,
               view->metaObject()->className());
    }
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

    // IR rows select independently; Tab moves the disassembly cursor.
    bench.action(ActionId::ViewLowIR)->trigger();
    CodeView *ir = nullptr;
    QTRY_VERIFY((ir = bench.codeView(QStringLiteral("low"))) != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(ir->text()->lineCount() > 3, OpenTimeoutMs);
    ir->text()->setCursorLine(3);
    QCOMPARE(disassembly->currentItem(), std::optional<Address>(Base + 0x140));
    bench.window->activateWindow();
    QVERIFY(QTest::qWaitForWindowActive(bench.window.get()));
    ir->text()->setFocus();
    QTRY_VERIFY(ir->text()->hasFocus());
    bench.action(ActionId::JumpPseudocode)->trigger();
    QTRY_COMPARE(disassembly->currentItem(),
                 std::optional<Address>(Base + 0x143));

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
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd-annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!bench.session.dirty());
    QVERIFY(QString::fromUtf8(readAll(path + ".neverd-annotations.json"))
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
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(path + ".neverd-functions.json"),
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

    // D makes the data under the cursor a value, the next size each time
    // (stderr's qword gives way to a byte, then a word); U shows its bytes as
    // bytes, and undo takes U back.  Each commits at once.
    const Address object = Base + 0x3200;
    const auto items = [&] {
      return QString::fromUtf8(readAll(path + ".neverd-items.json"));
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
          QJsonDocument::fromJson(readAll(path + ".neverd-operands.json"))
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

    // A restarted worker reopens the file where its database left it.  The
    // worker writes the sidecar before it answers, and an edit in flight asks
    // to be saved first.
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.dirty(), OpenTimeoutMs);
    bench.session.restart();
    QTRY_VERIFY_WITH_TIMEOUT(!bench.session.loaded(), OpenTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(bench.session.loaded(), OpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(disassembly->currentItem(),
                              std::optional<Address>(Base + 0x140),
                              OpenTimeoutMs);
    QVERIFY2(bench.session.lastError().isEmpty(),
             qPrintable(bench.session.lastError()));
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
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(first + ".neverd-annotations.json"),
                             OpenTimeoutMs);
    QTRY_VERIFY(!session.dirty());
    QVERIFY(readAll(first + ".neverd-annotations.json")
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
        readAll(first + ".neverd-annotations.json").contains("comment on A"));

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
    QVERIFY(readAll(second + ".neverd-annotations.json")
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
    const auto bytes = readAll(path + ".neverd-annotations.json");
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
        return QJsonDocument::fromJson(readAll(path + ".neverd-items.json"))
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
      bench.session.save();
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
      QSignalSpy saved(&bench.session, &Session::databaseSaved);
      bench.session.save();
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
