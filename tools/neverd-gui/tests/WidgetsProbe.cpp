//===- WidgetsProbe.cpp - End-to-end workbench interaction probe ----------===//
//
//                         NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise workbench actions through visible widgets and keyboard input.
//===----------------------------------------------------------------------===//

#include "WidgetsProbe.h"

#include "ChooserView.h"
#include "CodeView.h"
#include "DisassemblyView.h"
#include "GraphView.h"
#include "HexView.h"
#include "JumpDialog.h"
#include "Language.h"
#include "ListingView.h"
#include "MainWindow.h"
#include "Session.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLineEdit>
#include <QList>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QTreeView>
#include <algorithm>
#include <functional>
#include <kddockwidgets/LayoutSaver.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <memory>
#include <vector>

namespace neverd::gui {
namespace {
constexpr int TickMs = 20;
constexpr int StageTimeoutMs = 20000;
constexpr Address FixtureBase = 0xffff800012340000ULL;

using Dock = KDDockWidgets::QtWidgets::DockWidget;

struct Stage {
  const char *name;
  /// Runs once when the stage begins.
  std::function<void()> start;
  /// Polled until true.
  std::function<bool()> done;
};

class WidgetsProbe final : public QObject {
public:
  WidgetsProbe(MainWindow &window, Session &session, QString binary)
      : QObject(&window), window_(window), session_(session),
        binary_(std::move(binary)),
        captureDirectory_(qEnvironmentVariable("NEVERD_PROBE_CAPTURE_DIR")) {
    buildStages();
    connect(&timer_, &QTimer::timeout, this, &WidgetsProbe::tick);
    timer_.start(TickMs);
  }

private:
  bool realEngine() const { return !binary_.isEmpty(); }
  DisassemblyView *disassembly() const { return window_.disassembly(); }
  QAction *action(ActionId id) const { return window_.actions().action(id); }
  ChooserView *functions() const {
    auto *table =
        window_.findChild<QTreeView *>(QStringLiteral("functionsList"));
    return table ? qobject_cast<ChooserView *>(table->parentWidget()) : nullptr;
  }
  CodeView *codeView(const QString &representation) const {
    for (auto *view : window_.findChildren<CodeView *>())
      if (view->representation() == representation && view->isVisible())
        return view;
    return nullptr;
  }
  Dock *dock(const QString &title) const {
    for (auto *candidate : window_.findChildren<Dock *>())
      if (candidate->title() == title)
        return candidate;
    return nullptr;
  }
  /// Press \p key in the focused analysis view, as a user would.
  void press(int key, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QWidget *target = QApplication::focusWidget();
    if (!target)
      target = disassembly()->listing()->viewport();
    QTest::keyClick(target, Qt::Key(key), modifiers);
  }
  void focusListing() {
    window_.activateWindow();
    disassembly()->focusContent();
  }
  /// The window is active and the keyboard focus is inside \p view.
  bool focusedIn(QWidget *view) const {
    auto *focus = QApplication::focusWidget();
    return QApplication::activeWindow() == &window_ && focus &&
           (focus == view || view->isAncestorOf(focus));
  }
  /// A stage that focuses the disassembly and waits until keys reach it.
  Stage focusDisassembly(const char *name) {
    return {name, [this] { focusListing(); },
            [this] { return focusedIn(disassembly()); }};
  }

  void buildStages() {
    stages_.push_back(
        {"open",
         [this] {
           QString path = binary_;
           if (path.isEmpty()) {
             path = fixtureDirectory_.filePath(QStringLiteral("probe.bin"));
             QFile file(path);
             if (!file.open(QIODevice::WriteOnly) || file.write("fixture") != 7)
               fail(QStringLiteral("Cannot write the probe fixture"));
           }
           window_.openFile(path);
         },
         [this] {
           return session_.loaded() && disassembly()->currentItem() &&
                  functions() && functions()->model().total() > 0;
         }});
    stages_.push_back(
        {"functions",
         [this] {
           if (realEngine())
             functions()->setFilterText(QStringLiteral("main"));
         },
         [this] {
           auto &model = functions()->model();
           for (int row = 0; row < model.total(); ++row) {
             const auto name = model.rowObject(row).value("name").toString();
             if (name == (realEngine() ? QStringLiteral("main")
                                       : QStringLiteral("function_0"))) {
               target_ = model.addressAt(row);
               return target_.has_value();
             }
           }
           return false;
         }});
    stages_.push_back({"navigate",
                       [this] { emit functions() -> activated(*target_); },
                       [this] {
                         return disassembly()->currentFunction() == target_ &&
                                disassembly()->listing()->cursorLine();
                       }});
    stages_.push_back(focusDisassembly("text"));
    stages_.push_back({"text-capture", nullptr,
                       [this] { return capture(QStringLiteral("text")); }});
    // G opens "Jump anywhere"; the dialog's line edit takes an expression.
    // The jump goes to the next instruction, wherever its boundary is.
    stages_.push_back(
        {"jump-target",
         [this] {
           session_.read(
               QStringLiteral("listing"),
               {{"address", hexAddress(*target_)},
                {"before", 0},
                {"after", 24}},
               this, [this](const QJsonObject &payload) {
                 for (const auto &value : payload.value("lines").toArray()) {
                   const auto line = value.toObject();
                   const auto item = addressValue(line.value("item"));
                   if (line.value("kind").toString() == QLatin1String("insn") &&
                       item && *item > *target_) {
                     jumpTarget_ = item;
                     return;
                   }
                 }
                 fail(QStringLiteral("No instruction follows the target"));
               });
         },
         [this] { return jumpTarget_.has_value(); }});
    stages_.push_back(focusDisassembly("jump-focus"));
    stages_.push_back(
        {"jump-dialog",
         [this] { QTimer::singleShot(0, this, [this] { press(Qt::Key_G); }); },
         [this] {
           auto *dialog =
               qobject_cast<JumpDialog *>(QApplication::activeWindow());
           if (!dialog)
             return false;
           if (dialog->isModal()) {
             fail(QStringLiteral("Jump anywhere blocks the workbench"));
             return false;
           }
           jumpDialog_ = dialog;
           auto *edit = dialog->findChild<QLineEdit *>();
           edit->setText(hexAddress(*jumpTarget_));
           QTimer::singleShot(
               0, edit, [edit] { QTest::keyClick(edit, Qt::Key_Return); });
           return true;
         }});
    stages_.push_back({"jumped", nullptr, [this] {
                         return (!jumpDialog_ || !jumpDialog_->isVisible()) &&
                                disassembly()->currentItem() == jumpTarget_;
                       }});
    // Esc returns to the previous position.
    stages_.push_back(focusDisassembly("back-focus"));
    stages_.push_back(
        {"back", [this] { press(Qt::Key_Escape); },
         [this] { return disassembly()->currentItem() == target_; }});
    // Space toggles the graph of the current function.
    stages_.push_back(focusDisassembly("graph-focus"));
    stages_.push_back({"graph", [this] { press(Qt::Key_Space); },
                       [this] {
                         return disassembly()->graphMode() &&
                                !disassembly()->graph()->nodes().isEmpty() &&
                                capture(QStringLiteral("graph"));
                       }});
    stages_.push_back({"graph-overview", nullptr, [this] {
                         auto *overview =
                             dock(QStringLiteral("Graph overview"));
                         return overview && overview->isOpen();
                       }});
    stages_.push_back(focusDisassembly("text-again-focus"));
    stages_.push_back({"text-again", [this] { press(Qt::Key_Space); },
                       [this] { return !disassembly()->graphMode(); }});
    // Tab opens the pseudocode of the current function.
    stages_.push_back(focusDisassembly("pseudocode-focus"));
    stages_.push_back({"pseudocode", [this] { press(Qt::Key_Tab); },
                       [this] {
                         auto *view = codeView(QStringLiteral("source"));
                         return view && view->text()->function() == target_ &&
                                !view->text()->loading() &&
                                view->text()->lineCount() > 0 &&
                                capture(QStringLiteral("pseudocode"));
                       }});
    // Tab in pseudocode returns to the disassembly.
    stages_.push_back(
        {"pseudocode-focus-text",
         [this] { codeView(QStringLiteral("source"))->text()->setFocus(); },
         [this] { return focusedIn(codeView(QStringLiteral("source"))); }});
    stages_.push_back(
        {"pseudocode-back", [this] { press(Qt::Key_Tab); },
         [this] {
           return disassembly()->listing()->hasFocus() ||
                  disassembly()->listing()->viewport()->hasFocus();
         }});
    stages_.push_back({"hex", [this] { action(ActionId::ViewHex)->trigger(); },
                       [this] {
                         auto *hex = window_.findChild<HexView *>();
                         return hex && hex->isVisible() &&
                                hex->currentAddress() ==
                                    disassembly()->currentItem() &&
                                capture(QStringLiteral("hex"));
                       }});
    // Docking: close a window and reopen it from the View menu; the saved
    // desktop restores the same set of windows.
    stages_.push_back(
        {"docking",
         [this] {
           auto *hex = dock(tr("Hex View-1"));
           if (!hex) {
             fail(QStringLiteral("No hex dock"));
             return;
           }
           hex->forceClose();
           if (hex->isOpen())
             fail(QStringLiteral("The hex dock did not close"));
           action(ActionId::ViewHex)->trigger();
           layout_ = KDDockWidgets::LayoutSaver().serializeLayout();
           auto *functionsDock = dock(tr("Functions"));
           functionsDock->forceClose();
           if (!KDDockWidgets::LayoutSaver().restoreLayout(layout_))
             fail(QStringLiteral("The saved desktop did not restore"));
         },
         [this] {
           auto *hex = dock(tr("Hex View-1"));
           auto *functionsDock = dock(tr("Functions"));
           return hex && hex->isOpen() && functionsDock &&
                  functionsDock->isOpen();
         }});
    // Live language switching retitles menus and windows.
    stages_.push_back({"language",
                       [this] {
                         englishFileMenu_ =
                             window_.menuBar()->actions().value(0)->text();
                         applyLanguage(QStringLiteral("zh-CN"));
                       },
                       [this] {
                         const auto text =
                             window_.menuBar()->actions().value(0)->text();
                         return !text.isEmpty() && text != englishFileMenu_ &&
                                capture(QStringLiteral("language"));
                       }});
    stages_.push_back({"language-restored",
                       [] { applyLanguage(QStringLiteral("en")); },
                       [this] {
                         return window_.menuBar()->actions().value(0)->text() ==
                                englishFileMenu_;
                       }});
    stages_.push_back({"detach-before-exit",
                       [this] {
                         auto *assembly = dock(tr("NeverD View-A"));
                         auto *hex = dock(tr("Hex View-1"));
                         if (!assembly || !hex) {
                           fail(QStringLiteral("Missing panes for exit check"));
                           return;
                         }
                         for (auto *pane : {assembly, hex}) {
                           pane->setFloating(true);
                           floating_.append(pane->QWidget::window());
                         }
                         auxiliary_ = new QDialog(disassembly());
                         auxiliary_->setAttribute(Qt::WA_DeleteOnClose);
                         auxiliary_->show();
                       },
                       [this] { return detachedWindowsVisible(); }});
    // Unsaved edits ask before quitting; Cancel keeps the window open.
    stages_.push_back({"unsaved-edit",
                       [this] {
                         session_.setComment(*target_,
                                             QStringLiteral("probe comment"));
                       },
                       [this] { return session_.dirty(); }});
    stages_.push_back(
        {"quit-prompt",
         [this] { QTimer::singleShot(0, this, [this] { window_.close(); }); },
         [this] {
           auto *box =
               qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
           if (!box)
             return false;
           capture(QStringLiteral("unsaved"));
           box->button(QMessageBox::Cancel)->click();
           return true;
         }});
    stages_.push_back({"quit-cancelled", nullptr, [this] {
                         return !QApplication::activeModalWidget() &&
                                window_.isVisible() && session_.dirty() &&
                                detachedWindowsVisible();
                       }});
  }

  bool detachedWindowsVisible() const {
    return floating_.size() == 2 &&
           std::all_of(floating_.begin(), floating_.end(),
                       [](const auto &window) {
                         return window && window->isVisible();
                       }) &&
           auxiliary_ && auxiliary_->isVisible();
  }

  void closeWorkbench() {
    // A successful probe must exit through the application's real close path.
    // Forcing exit(0) here would hide orphaned floating windows and processes.
    connect(&window_, &MainWindow::shutdownAccepted, this, [this] {
      const bool leftOpen = std::any_of(floating_.begin(), floating_.end(),
                                        [](const auto &window) {
                                          return window && window->isVisible();
                                        }) ||
                            (auxiliary_ && auxiliary_->isVisible());
      if (leftOpen) {
        qCritical("Widgets probe: detached windows survived accepted exit");
        QCoreApplication::exit(1);
      } else {
        qInfo("Widgets probe passed %zu stages and closed detached windows",
              stages_.size());
      }
    });
    auto *answer = new QTimer(this);
    connect(answer, &QTimer::timeout, this, [answer] {
      if (auto *box =
              qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
        answer->stop();
        box->button(QMessageBox::Discard)->click();
      }
    });
    answer->start(TickMs);
    QTimer::singleShot(StageTimeoutMs, this, [] {
      qCritical("Widgets probe: process did not exit after closing workbench");
      QCoreApplication::exit(1);
    });
    window_.close();
  }

  bool capture(const QString &name) {
    if (captureDirectory_.isEmpty())
      return true;
    QDir().mkpath(captureDirectory_);
    if (!window_.grab().save(QDir(captureDirectory_).filePath(name + ".png")))
      fail(QStringLiteral("Could not capture %1").arg(name));
    return true;
  }

  void fail(const QString &description) {
    if (failed_)
      return;
    failed_ = true;
    timer_.stop();
    auto *focus = QApplication::focusWidget();
    qCritical().noquote()
        << "Widgets probe failed at stage"
        << (stage_ < stages_.size() ? stages_[stage_].name : "end") << ":"
        << description << "| error:" << session_.lastError() << "| focus:"
        << (focus ? QString::fromLatin1(focus->metaObject()->className()) +
                        QLatin1Char('/') + focus->objectName()
                  : QStringLiteral("none"))
        << "| active window:"
        << (QApplication::activeWindow()
                ? QString::fromLatin1(
                      QApplication::activeWindow()->metaObject()->className())
                : QStringLiteral("none"))
        << "| item:"
        << (disassembly()->currentItem()
                ? hexAddress(*disassembly()->currentItem())
                : QStringLiteral("none"))
        << "| target:"
        << (target_ ? hexAddress(*target_) : QStringLiteral("none"))
        << "| back enabled:" << action(ActionId::JumpBack)->isEnabled()
        << "can go back:" << disassembly()->canGoBack() << "| modal:"
        << (QApplication::activeModalWidget()
                ? QApplication::activeModalWidget()->windowTitle() +
                      QStringLiteral(" / ") +
                      (QApplication::activeModalWidget()
                               ->findChild<QLineEdit *>()
                           ? QApplication::activeModalWidget()
                                 ->findChild<QLineEdit *>()
                                 ->text()
                           : QString())
                : QStringLiteral("none"));
    if (!captureDirectory_.isEmpty()) {
      QDir().mkpath(captureDirectory_);
      window_.grab().save(
          QDir(captureDirectory_)
              .filePath(QStringLiteral("widgets-probe-failure.png")));
    }
    QTimer::singleShot(0, qApp, [] { QCoreApplication::exit(1); });
  }

  void tick() {
    if (failed_ || running_)
      return;
    if (stage_ >= stages_.size()) {
      timer_.stop();
      closeWorkbench();
      return;
    }
    auto &stage = stages_[stage_];
    if (!started_) {
      started_ = true;
      clock_.restart();
      running_ = true;
      if (stage.start)
        stage.start();
      running_ = false;
      if (failed_)
        return;
    }
    running_ = true;
    const bool done = stage.done();
    running_ = false;
    if (failed_)
      return;
    if (done) {
      ++stage_;
      started_ = false;
      return;
    }
    if (clock_.elapsed() > StageTimeoutMs)
      fail(QStringLiteral("timed out"));
  }

  static QString tr(const char *text) {
    return QCoreApplication::translate("neverd::gui::MainWindow", text);
  }

  MainWindow &window_;
  Session &session_;
  QString binary_, captureDirectory_, englishFileMenu_;
  QTemporaryDir fixtureDirectory_;
  QTimer timer_;
  QPointer<JumpDialog> jumpDialog_;
  QList<QPointer<QWidget>> floating_;
  QPointer<QDialog> auxiliary_;
  QElapsedTimer clock_;
  std::vector<Stage> stages_;
  std::size_t stage_ = 0;
  std::optional<Address> target_, jumpTarget_;
  QByteArray layout_;
  bool started_ = false, running_ = false, failed_ = false;
};
} // namespace

void startWidgetsProbe(MainWindow &window, Session &session,
                       const QString &binary) {
  new WidgetsProbe(window, session, binary);
}

} // namespace neverd::gui
