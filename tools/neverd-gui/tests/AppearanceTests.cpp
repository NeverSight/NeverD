//===- AppearanceTests.cpp - Owned desktop appearance -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Windows, menus, fonts and icons keep the application appearance with a
/// conflicting desktop theme or a retired native-caption preference.
//===----------------------------------------------------------------------===//

#include "ActionRegistry.h"
#include "Docking.h"
#include "GnomeModalDialogs.h"
#include "Language.h"
#include "OutputWindow.h"
#include "QuickStartDialog.h"
#include "Session.h"
#include "SettingsKeys.h"
#include "Theme.h"
#include "TitleBar.h"
#include "WindowFrame.h"

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFontInfo>
#include <QGlyphRun>
#include <QImage>
#include <QInputDialog>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStyle>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <memory>

using namespace neverd::gui;

class AppearanceTests final : public QObject {
  Q_OBJECT
public:
  static void initMain() {
    gnome::prepareModalDialogs();
    WindowFrame::prepareNativeWindows();
  }

private slots:
  void initTestCase() {
    QVERIFY(settings_.isValid());
    QVERIFY(!QCoreApplication::testAttribute(Qt::AA_Use96Dpi));
    QCoreApplication::setOrganizationName(QStringLiteral("NeverDTests"));
    QCoreApplication::setApplicationName(QStringLiteral("AppearanceTests"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settings_.path());
    QSettings().setValue(settings::NativeTitleBar, true);
    WindowFrame::frameDialogs(*qApp);
    gnome::detachModalDialogs(*qApp);
    Theme::instance().apply();
    configureDocking();
  }

  void dialogsUseOwnedChrome_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("dark");
    for (const char *kind :
         {"dialog", "file", "font", "color", "message", "input", "quick-start"})
      for (const bool dark : {false, true})
        QTest::newRow(
            qPrintable(QString::fromLatin1(kind) + (dark ? "-dark" : "-light")))
            << QString::fromLatin1(kind) << dark;
  }

  void dialogsUseOwnedChrome() {
    QFETCH(QString, kind);
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    std::unique_ptr<QDialog> dialog;
    if (kind == "file")
      dialog =
          std::make_unique<QFileDialog>(nullptr, "Open file", settings_.path());
    else if (kind == "font") {
      dialog = std::make_unique<QFontDialog>(Theme::defaultCodeFont());
      static_cast<QFontDialog *>(dialog.get())
          ->setOption(QFontDialog::MonospacedFonts);
    } else if (kind == "color")
      dialog = std::make_unique<QColorDialog>();
    else if (kind == "message")
      dialog = std::make_unique<QMessageBox>(QMessageBox::Information,
                                             "Analysis", "Analysis complete.");
    else if (kind == "input")
      dialog = std::make_unique<QInputDialog>();
    else if (kind == "quick-start")
      dialog = std::make_unique<QuickStartDialog>(
          [](QWidget *) { return QString(); });
    else
      dialog = std::make_unique<QDialog>();
    dialog->show();
    QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
    QVERIFY(dialog->windowFlags() & Qt::FramelessWindowHint);
    QVERIFY(dialog->windowFlags() & Qt::NoDropShadowWindowHint);
    QVERIFY(!dialog->testAttribute(Qt::WA_DontShowOnScreen));
    auto *bar = dialog->findChild<TitleBar *>(Qt::FindDirectChildrenOnly);
    QVERIFY(bar && bar->isVisible());
    auto *close = bar->findChild<QAbstractButton *>("closeButton");
    QVERIFY(close && close->isVisible());
    QCOMPARE(dialog->windowHandle()->frameMargins(), QMargins());
    QVERIFY(QCoreApplication::testAttribute(Qt::AA_DontUseNativeDialogs));
    QCOMPARE(
        dialog->palette().color(QPalette::Window),
        Theme::instance().chrome(kind == "quick-start" ? "Base" : "Window"));
    if (kind == "quick-start") {
      auto *minimize = bar->findChild<QAbstractButton *>("minimizeButton");
      QVERIFY(minimize && minimize->isVisible());
      const QSize normal = dialog->size();
      QTest::mouseClick(minimize, Qt::LeftButton);
      QTRY_VERIFY(dialog->isMinimized());
      dialog->showNormal();
      QTRY_VERIFY(!dialog->isMinimized());
      QCOMPARE(dialog->size(), normal);
    }
    if (auto *fonts = qobject_cast<QFontDialog *>(dialog.get()))
      QCOMPARE(fonts->currentFont().family(),
               Theme::defaultCodeFont().family());
    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(dialog->grab().save(captures + '/' + QTest::currentDataTag() +
                                  ".png"));
    }
    QTest::mouseClick(close, Qt::LeftButton);
    QTRY_VERIFY(!dialog->isVisible());
  }

  void windowShadowCompositesWithDesktop_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<bool>("modal");
    for (const char *kind :
         {"dialog", "early-native", "quick-start", "early-native-quick-start"})
      for (const bool modal : {false, true})
        QTest::newRow(
            qPrintable(QString::fromLatin1(kind) + (modal ? "-exec" : "-show")))
            << QString::fromLatin1(kind) << modal;
  }

  void windowShadowCompositesWithDesktop() {
    if (QGuiApplication::platformName() != QLatin1String("windows"))
      QSKIP("Checks Windows desktop composition, not a widget-only capture");
    QFETCH(QString, kind);
    QFETCH(bool, modal);
    class Backdrop final : public QWidget {
      void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(64, 192, 128));
      }
    } backdrop;
    backdrop.resize(1000, 720);
    backdrop.move(backdrop.screen()->availableGeometry().topLeft() +
                  QPoint(40, 40));
    backdrop.show();
    QVERIFY(QTest::qWaitForWindowExposed(&backdrop));
    std::unique_ptr<QDialog> dialog;
    if (kind.endsWith(QLatin1String("quick-start")))
      dialog = std::make_unique<QuickStartDialog>(
          [](QWidget *) { return QString(); }, modal ? nullptr : &backdrop);
    else {
      dialog = std::make_unique<QDialog>(&backdrop);
      dialog->resize(320, 240);
    }
    if (kind.startsWith(QLatin1String("early-native")))
      dialog->winId();
    dialog->move(backdrop.mapToGlobal(QPoint(40, 40)));
    const auto capture = [&] {
      const QPoint origin = dialog->mapToGlobal(QPoint());
      return dialog->screen()
          ->grabWindow(0, origin.x(), origin.y(), dialog->width(),
                       dialog->height())
          .toImage();
    };
    const auto check = [&] {
      QVERIFY(QTest::qWaitForWindowExposed(dialog.get()));
      // A hidden native window can still report its previous screen position.
      // Check placement only after showing it, before sampling the shadow.
      QVERIFY(backdrop.rect().contains(
          backdrop.mapFromGlobal(dialog->mapToGlobal(QPoint()))));
      const QImage painted = dialog->grab().toImage();
      QVERIFY(painted.pixelColor(0, 0).alpha() <
              painted.pixelColor(painted.width() / 2, painted.height() / 2)
                  .alpha());
      QImage composite(1, 1, QImage::Format_ARGB32_Premultiplied);
      composite.fill(QColor(64, 192, 128));
      {
        QPainter painter(&composite);
        painter.drawImage(QPoint(), painted, QRect(0, 0, 1, 1));
      }
      QTRY_COMPARE(capture().pixelColor(0, 0), composite.pixelColor(0, 0));
      const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
      if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        QVERIFY(capture().save(captures + '/' + QTest::currentDataTag() +
                               "-desktop.png"));
      }
    };
    if (modal) {
      QTimer::singleShot(0, dialog.get(), [&] {
        const auto close = qScopeGuard([&] { dialog->reject(); });
        check();
      });
      dialog->exec();
    } else {
      dialog->show();
      check();
    }
  }

  void desktopThemeCannotSupplyColorsOrClearIcon() {
    const auto previous = Theme::instance().mode();
    const auto oldName = QIcon::themeName();
    const auto oldPaths = QIcon::themeSearchPaths();
    const auto restore = qScopeGuard([&] {
      QIcon::setThemeName(oldName);
      QIcon::setThemeSearchPaths(oldPaths);
      Theme::instance().setMode(previous);
      Theme::instance().apply();
    });
    QTemporaryDir icons;
    QVERIFY(icons.isValid());
    QDir().mkpath(icons.filePath("test/16x16/actions"));
    QFile index(icons.filePath("test/index.theme"));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write("[Icon Theme]\nName=test\nDirectories=16x16/actions\n"
                "[16x16/actions]\nSize=16\nType=Fixed\nContext=Actions\n");
    index.close();
    for (const auto *name : {"edit-clear", "edit-clear-locationbar-rtl"}) {
      QFile file(
          icons.filePath(QString("test/16x16/actions/%1.svg").arg(name)));
      QVERIFY(file.open(QIODevice::WriteOnly));
      file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"16\" "
                 "height=\"16\">"
                 "<rect width=\"16\" height=\"16\" fill=\"#ff00ff\"/></svg>");
    }
    QIcon::setThemeSearchPaths({icons.path()});
    QIcon::setThemeName("test");
    QVERIFY(QIcon::hasThemeIcon("edit-clear"));
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      Theme::instance().setMode(mode);
      Theme::instance().apply();
      const QPalette expected = QApplication::palette();
      QPalette injected;
      for (int role = 0; role < QPalette::NColorRoles; ++role)
        if (role != QPalette::NoRole)
          injected.setColor(QPalette::ColorRole(role), Qt::magenta);
      QApplication::setPalette(injected);
      QApplication::setFont(QFont("monospace", 24));
      Theme::instance().apply();
      for (int role = 0; role < QPalette::NColorRoles; ++role)
        if (role != QPalette::NoRole)
          for (const auto group :
               {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
            QCOMPARE(
                QApplication::palette().color(group, QPalette::ColorRole(role)),
                expected.color(group, QPalette::ColorRole(role)));
      QCOMPARE(QFontInfo(QApplication::font()).family(), QString("Noto Sans"));
      QLineEdit edit("clear this");
      edit.setClearButtonEnabled(true);
      edit.show();
      QVERIFY(QTest::qWaitForWindowExposed(&edit));
      auto *clear = edit.findChild<QToolButton *>();
      QVERIFY(clear && clear->isVisible());
      const auto image = clear->icon().pixmap(16, 16).toImage();
      QVERIFY(!image.isNull());
      for (int y = 0; y < image.height(); ++y)
        for (int x = 0; x < image.width(); ++x)
          QVERIFY(image.pixelColor(x, y) != QColor(Qt::magenta));
      QTest::mouseClick(clear, Qt::LeftButton);
      QVERIFY(edit.text().isEmpty());
    }
  }

  void menusAndButtonOrderAreOwned() {
    Theme::instance().apply();
    QMenuBar bar;
    bar.addMenu("File");
    bar.ensurePolished();
    QVERIFY(!bar.isNativeMenuBar());
    QCOMPARE(bar.style()->styleHint(QStyle::SH_DialogButtonLayout),
             int(QDialogButtonBox::GnomeLayout));
    QMenu menu;
    menu.addAction("Open");
    menu.ensurePolished();
    QVERIFY(menu.windowFlags() & Qt::NoDropShadowWindowHint);
  }

  void menusKeepActionIconsWithPlatformDefaults() {
    const QDir icons(QStringLiteral(":/neverd/icons"));
    for (const auto &name : icons.entryList({"*.svg"}, QDir::Files)) {
      const QSvgRenderer renderer(icons.filePath(name));
      QVERIFY2(renderer.isValid(), qPrintable(name));
    }
    // The macOS platform enables this after QApplication is constructed.
    QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, true);
    Theme::instance().apply();
    QVERIFY(!QCoreApplication::testAttribute(Qt::AA_DontShowIconsInMenus));
    ActionRegistry registry;
    for (auto *action : registry.allActions()) {
      QVERIFY2(!action->icon().pixmap(16, 16).isNull(),
               qPrintable(action->objectName()));
      QVERIFY2(action->isIconVisibleInMenu(), qPrintable(action->objectName()));
    }
    Session session(QStringLiteral("unused-worker"));
    OutputWindow output(session);
    const std::unique_ptr<QMenu> context(output.createLogMenu());
    for (auto *action : context->actions()) {
      if (action->isSeparator())
        continue;
      QVERIFY(!action->icon().pixmap(16, 16).isNull());
      QVERIFY(action->isIconVisibleInMenu());
    }
  }

  void menuColumnsStayTogether_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void menuColumnsStayTogether() {
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    QMenu menu;
    menu.ensurePolished();
    QStyleOption option;
    option.initFrom(&menu);
    const int iconExtent =
        menu.style()->pixelMetric(QStyle::PM_SmallIconSize, &option, &menu);
    QPixmap marker(iconExtent, iconExtent);
    marker.fill(Qt::magenta);
    auto *withIcon = menu.addAction(QIcon(marker), "Menu entry");
    withIcon->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    auto *plain = menu.addAction("Menu entry");
    auto *checked = menu.addAction("Menu entry");
    checked->setCheckable(true);
    checked->setChecked(true);
    auto *checkedIcon = menu.addAction(QIcon(marker), "Menu entry");
    checkedIcon->setCheckable(true);
    checkedIcon->setChecked(true);
    auto *submenu = menu.addMenu(QIcon(marker), "Menu entry");
    submenu->addAction("Child entry");
    // Rendering columns does not need popup activation. Cocoa can dismiss
    // an unfocused popup before an exposure wait, unrelated to its layout.
    menu.resize(menu.sizeHint());
    const auto capture = menu.grab().toImage();
    const qreal dpr = capture.devicePixelRatio();
    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(capture.save(captures + "/menu-columns-" +
                           QTest::currentDataTag() + ".png"));
    }
    // Measure painted columns: Qt adds its icon column after item padding,
    // so checking the style-sheet literal alone misses a double reservation.
    const auto bounds = [&](QAction *action, auto matches, int fromX) {
      const QRect row = menu.actionGeometry(action);
      QRect ink;
      for (int y = qRound(row.top() * dpr);
           y < qRound((row.bottom() + 1) * dpr); ++y)
        for (int x = fromX; x < capture.width(); ++x) {
          const auto pixel = capture.pixelColor(x, y);
          if (matches(pixel))
            ink |= QRect(x, y, 1, 1);
        }
      return ink;
    };
    const auto magenta = [](const QColor &pixel) {
      return pixel.red() > 243 && pixel.green() < 12 && pixel.blue() > 243;
    };
    const QRect iconInk = bounds(withIcon, magenta, 0);
    QVERIFY(!iconInk.isEmpty());
    // Subpixel antialiasing need not paint any glyph pixel in the exact
    // foreground color. Measure contrast against the menu background.
    const auto background = Theme::instance().chrome("Menu");
    const auto textInk = [&](const QColor &pixel) {
      return std::max({qAbs(pixel.red() - background.red()),
                       qAbs(pixel.green() - background.green()),
                       qAbs(pixel.blue() - background.blue())}) > 24;
    };
    const QRect labelInk = bounds(withIcon, textInk, iconInk.right() + 1);
    QVERIFY(!labelInk.isEmpty());
    const qreal gap = (labelInk.left() - iconInk.right() - 1) / dpr;
    qInfo() << "Menu icon-to-label gap" << gap << "DPR" << dpr
            << "icon physical bounds" << iconInk << "label bounds" << labelInk;
    QVERIFY(gap >= 6 && gap <= 10);
    for (auto *action : {plain, checked, checkedIcon, submenu->menuAction()}) {
      // The checked icon has an additional border outside its painted square.
      // The ordinary row above already measures the complete icon/text gap.
      const QRect label =
          bounds(action, textInk, iconInk.right() + qRound(3 * dpr));
      QVERIFY(!label.isEmpty());
      QCOMPARE(label.left(), labelInk.left());
    }
    // Checking an icon must not move it when the active border appears.
    QCOMPARE(bounds(checkedIcon, magenta, 0).left(), iconInk.left());
    if (!captures.isEmpty()) {
      ActionRegistry registry;
      QMenuBar bar;
      registry.buildMenus(&bar, [](QMenu *, const QString &) {});
      for (const auto *name : {"menuFile", "menuEdit", "menuOptions"}) {
        auto *actual = bar.findChild<QMenu *>(QLatin1String(name));
        QVERIFY(actual);
        actual->ensurePolished();
        actual->resize(actual->sizeHint());
        QVERIFY(actual->grab().save(captures + "/" + QLatin1String(name) + "-" +
                                    QTest::currentDataTag() + ".png"));
      }
    }
  }

  void menuShowsOnlyThePrimaryShortcut_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void menuShowsOnlyThePrimaryShortcut() {
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    ActionRegistry registry;
    QMenuBar bar;
    registry.buildMenus(&bar, [](QMenu *, const QString &) {});
    auto *comment = registry.action(ActionId::EditRepeatableComment);
    auto *comments = bar.findChild<QMenu *>(QStringLiteral("menuComments"));
    QVERIFY(comments);
    QCOMPARE(comments->actions(), QList({comment}));

    struct InspectableMenu : QMenu {
      using QMenu::initStyleOption;
    } menu;
    menu.addAction(registry.action(ActionId::EditRename));
    menu.addAction(comment);
    menu.addAction(registry.action(ActionId::EditBookmark));
    menu.ensurePolished();
    menu.resize(menu.sizeHint());
    QStyleOptionMenuItem option;
    menu.initStyleOption(&option, comment);
    QCOMPARE(option.text, QStringLiteral("Enter &comment...\t;"));
    QVERIFY(option.reservedShortcutWidth >=
            option.fontMetrics.horizontalAdvance(QStringLiteral(";")));
    QCOMPARE(comment->shortcuts(), QList({QKeySequence(Qt::Key_Semicolon),
                                          QKeySequence(Qt::Key_Slash)}));
    QCOMPARE(comment->iconText(), QStringLiteral("Enter comment"));
    QCOMPARE(comment->toolTip(), QStringLiteral("Enter comment (;, /)"));
    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(menu.grab().save(captures + "/comment-menu-" +
                               QTest::currentDataTag() + ".png"));
    }
    // A language change keeps the menu compact and preserves both bindings.
    registry.retranslate();
    menu.initStyleOption(&option, comment);
    QCOMPARE(option.text, QStringLiteral("Enter &comment...\t;"));
    QCOMPARE(comment->text(), QStringLiteral("Enter &comment..."));
    QCOMPARE(comment->shortcuts().size(), 2);
  }

  void longMenusScrollWithinOneColumn_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void longMenusScrollWithinOneColumn() {
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    class Menu : public QMenu {
    public:
      using QMenu::columnCount;
      using QMenu::QMenu;
    };
    QWidget anchor;
    anchor.show();
    QVERIFY(QTest::qWaitForWindowExposed(&anchor));
    Menu menu(&anchor);
    auto *first = menu.addAction("First command");
    menu.ensurePolished();
    const QRect available = anchor.screen()->availableGeometry();
    const QRect screen = anchor.screen()->geometry();
    const int rowHeight = menu.actionGeometry(first).height();
    QVERIFY(rowHeight > 0);
    // Windows/KDE may use the taskbar area for popups; Cocoa/GNOME use the
    // available area. Overflow either policy, regardless of resolution/font.
    const int rows = screen.height() / rowHeight + 12;
    for (int i = 1; i < rows; ++i)
      menu.addAction(QString("Command %1").arg(i));
    auto *last = menu.actions().last();
    QSignalSpy triggered(&menu, &QMenu::triggered);
    menu.popup(available.topLeft() + QPoint(20, 20));
    QVERIFY(menu.isVisible());
    QCOMPARE(menu.columnCount(), 1);
    QVERIFY(menu.height() <= screen.height());
    const int left = menu.actionGeometry(first).left();
    for (auto *action : menu.actions())
      QCOMPARE(menu.actionGeometry(action).left(), left);
    QVERIFY(menu.actionGeometry(last).top() >= menu.height());

    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(menu.grab().save(captures + "/long-menu-" +
                               QTest::currentDataTag() + ".png"));
    }
    const auto wheel = [&](int delta) {
      const QPoint center = menu.rect().center();
      QWheelEvent event(center, menu.mapToGlobal(center), {}, QPoint(0, delta),
                        Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
      QApplication::sendEvent(&menu, &event);
    };
    const int beforeWheel = menu.actionGeometry(first).top();
    wheel(-120);
    const int afterWheel = menu.actionGeometry(first).top();
    QVERIFY(afterWheel < beforeWheel);
    wheel(120);
    QVERIFY(menu.actionGeometry(first).top() > afterWheel);

    // The bottom scroller consumes a click and scrolls without activating a
    // command or dismissing the popup. Moving away stops its repeat timer.
    const int scrollerHeight = menu.style()->pixelMetric(
        QStyle::PM_MenuScrollerHeight, nullptr, &menu);
    QVERIFY(scrollerHeight > 0);
    const QPoint down(menu.width() / 2, menu.height() - scrollerHeight / 2 - 1);
    const int beforeArrow = menu.actionGeometry(first).top();
    QTest::mouseClick(&menu, Qt::LeftButton, Qt::NoModifier, down);
    QTRY_VERIFY(menu.actionGeometry(first).top() < beforeArrow);
    QTest::mouseMove(&menu, menu.rect().center());
    QVERIFY(menu.isVisible());
    QCOMPARE(triggered.count(), 0);

    QTest::keyClick(&menu, Qt::Key_End);
    QCOMPARE(menu.activeAction(), last);
    QVERIFY(menu.rect().contains(menu.actionGeometry(last).center()));
    QTest::keyClick(&menu, Qt::Key_Home);
    QCOMPARE(menu.activeAction(), first);
    QVERIFY(menu.rect().contains(menu.actionGeometry(first).center()));
    QTest::keyClick(&menu, Qt::Key_Escape);
    QVERIFY(!menu.isVisible());
  }

  void tabCloseIconKeepsItsSizeAndInset_data() {
    QTest::addColumn<bool>("docked");
    QTest::addColumn<bool>("dark");
    for (const bool docked : {false, true})
      for (const bool dark : {false, true})
        QTest::newRow(qPrintable(QString(docked ? "dock" : "plain") +
                                 (dark ? "-dark" : "-light")))
            << docked << dark;
  }

  void tabCloseIconKeepsItsSizeAndInset() {
    QFETCH(bool, docked);
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    KDDockWidgets::QtWidgets::MainWindow window("tab-appearance");
    QTabBar plain;
    QTabBar *tabs = &plain;
    if (docked) {
      auto *dock = new KDDockWidgets::QtWidgets::DockWidget("NeverD View-A");
      dock->setWidget(new QWidget);
      window.addDockWidget(dock, KDDockWidgets::Location_OnLeft);
      window.resize(320, 160);
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      tabs = window.findChild<QTabBar *>();
      QVERIFY(tabs);
    } else {
      tabs->setTabsClosable(true);
      tabs->addTab(QStringLiteral("NeverD View-A"));
      tabs->show();
      QVERIFY(QTest::qWaitForWindowExposed(tabs));
    }
    const auto side =
        static_cast<QTabBar::ButtonPosition>(tabs->style()->styleHint(
            QStyle::SH_TabBar_CloseButtonPosition, nullptr, tabs));
    auto *button = tabs->tabButton(0, side);
    QVERIFY(button);
    const auto capture = button->grab().toImage();
    QRect ink;
    // The close glyph's muted blue is distinct from the neutral tab and hover
    // backgrounds. Measure its visible stroke, not just the button's geometry.
    for (int y = 0; y < capture.height(); ++y)
      for (int x = 0; x < capture.width(); ++x) {
        const auto color = capture.pixelColor(x, y);
        if (color.blue() - color.red() > 15 && color.red() > 70)
          ink |= QRect(x, y, 1, 1);
      }
    const qreal dpr = capture.devicePixelRatio();
    const qreal rightInset =
        tabs->tabRect(0).right() + 1 - button->x() - (ink.right() + 1) / dpr;
    qInfo() << "Tab close button" << button->size() << "DPR" << dpr
            << "glyph physical bounds" << ink << "logical right inset"
            << rightInset;
    const auto captures = qEnvironmentVariable("NEVERD_APPEARANCE_CAPTURES");
    if (!captures.isEmpty()) {
      QDir().mkpath(captures);
      QVERIFY(tabs->grab().save(captures + "/tab-close-" +
                                QTest::currentDataTag() + ".png"));
    }
    // At fractional DPR, antialiasing can leave an edge pixel below the color
    // threshold. Allow one physical pixel, not a fraction of the logical size.
    QVERIFY((ink.width() + 1) / dpr >= 8);
    QVERIFY((ink.height() + 1) / dpr >= 8);
    // A correctly sized glyph must also have room before the tab's outer edge.
    // Tab text padding alone does not inset Qt's separate close-button widget.
    QVERIFY(rightInset >= 8);
    if (docked) {
      // Preserve the docking library's workaround for closing a moving tab.
      QCOMPARE(tabs->style()->styleHint(QStyle::SH_Widget_Animation_Duration,
                                        nullptr, tabs),
               0);
    }
  }

  void bundledFontsCoverInterfaceLanguages() {
    const auto original = currentLanguage();
    const auto restore = qScopeGuard([&] { applyLanguage(original); });
    for (const auto &language : languages()) {
      applyLanguage(language.code);
      const auto sample =
          QString::fromUtf8("Open 中國 日本語 한글 العربية Русский →");
      QTextLayout layout(sample, QApplication::font());
      layout.beginLayout();
      auto line = layout.createLine();
      line.setLineWidth(2000);
      layout.endLayout();
      const auto runs = layout.glyphRuns(0, -1, QTextLayout::RetrieveAll);
      QVERIFY(!runs.isEmpty());
      for (const auto &run : runs) {
        QString text;
        for (const auto index : run.stringIndexes())
          text += sample.at(index);
        const auto diagnostic =
            QStringLiteral("%1: %2 rendered [%3]")
                .arg(language.code, run.rawFont().familyName(), text);
        const auto family = run.rawFont().familyName();
        QVERIFY2(family == "Noto Sans" || family == "Noto Sans Arabic" ||
                     family == "Noto Sans Symbols 2" ||
                     family == "JetBrains Mono NL" ||
                     family.startsWith("Noto Sans CJK "),
                 qPrintable(diagnostic));
        for (const auto glyph : run.glyphIndexes())
          QVERIFY(glyph != 0);
      }
    }
    auto code = Theme::defaultCodeFont();
    QCOMPARE(QFontInfo(code).family(), QString("JetBrains Mono NL"));
    QVERIFY(QFontInfo(code).fixedPitch());
    QVERIFY(QFontDatabase::isFixedPitch(code.family()));
    for (const int size : {6, 10, 14, 24, 48}) {
      code.setPointSize(size);
      const QFontMetricsF metrics(code);
      QCOMPARE(metrics.horizontalAdvance("WWWW"),
               metrics.horizontalAdvance("iiii"));
    }
  }

  void defaultFontSizesFollowThePlatform() {
    Theme::instance().apply();
    const auto ui = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    const auto fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const auto code = Theme::defaultCodeFont();
    QCOMPARE(QApplication::font().pointSizeF(), ui.pointSizeF());
    QCOMPARE(QApplication::font().pixelSize(), ui.pixelSize());
    QCOMPARE(code.pointSizeF(), ui.pointSizeF());
    QCOMPARE(code.pixelSize(), ui.pixelSize());
    QLineEdit field("Address or expression");
    field.ensurePolished();
    QCOMPARE(QFontInfo(field.font()).pixelSize(), QFontInfo(ui).pixelSize());
    qInfo() << "Platform font sizes: UI" << ui.pointSizeF() << "pt /"
            << QFontInfo(ui).pixelSize() << "px; code" << code.pointSizeF()
            << "pt /" << QFontInfo(code).pixelSize() << "px; system fixed font"
            << fixed.pointSizeF() << "pt; screen DPI"
            << QGuiApplication::primaryScreen()->logicalDotsPerInch() << "DPR"
            << QGuiApplication::primaryScreen()->devicePixelRatio();
  }

  void codeFontZoomPreservesFractionalAndPixelSizes_data() {
    QTest::addColumn<bool>("pixels");
    QTest::newRow("fractional-points") << false;
    QTest::newRow("logical-pixels") << true;
  }

  void codeFontZoomPreservesFractionalAndPixelSizes() {
    QFETCH(bool, pixels);
    auto &theme = Theme::instance();
    const auto original = theme.codeFont();
    const auto restore = qScopeGuard([&] { theme.setCodeFont(original); });
    QFont chosen = original;
    chosen.setItalic(true);
    if (pixels)
      chosen.setPixelSize(18);
    else
      chosen.setPointSizeF(10.5);
    theme.setCodeFont(chosen);
    theme.apply();
    QCOMPARE(theme.codeFont().pointSizeF(), chosen.pointSizeF());
    QCOMPARE(theme.codeFont().pixelSize(), chosen.pixelSize());
    const qreal initial =
        pixels ? QFontInfo(chosen).pointSizeF() : chosen.pointSizeF();
    theme.zoomCodeFont(1);
    QCOMPARE(theme.codeFont().pointSizeF(), initial + 1);
    theme.zoomCodeFont(-1);
    QCOMPARE(theme.codeFont().pointSizeF(), initial);
    theme.resetCodeFontSize();
    const auto defaults = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    QCOMPARE(theme.codeFont().pointSizeF(), defaults.pointSizeF());
    QCOMPARE(theme.codeFont().pixelSize(), defaults.pixelSize());
    QCOMPARE(theme.codeFont().family(), chosen.family());
    QVERIFY(theme.codeFont().italic());
    QLineEdit field;
    field.ensurePolished();
    QCOMPARE(QFontInfo(field.font()).pixelSize(),
             QFontInfo(defaults).pixelSize());
  }

  void clientResizeKeepsOppositeEdgesAndBounds_data() {
    QTest::addColumn<bool>("restartAtMaximum");
    QTest::newRow("held-through-maximum") << false;
    QTest::newRow("restart-at-maximum") << true;
  }

  void clientResizeKeepsOppositeEdgesAndBounds() {
    QFETCH(bool, restartAtMaximum);
    const QString platform = QGuiApplication::platformName();
    if (platform != "offscreen" && platform != "cocoa")
      QSKIP("These platforms use native resize; docking tests drive it.");
    QWidget window;
    window.setMinimumSize(240, 180);
    window.setMaximumSize(500, 400);
    QVERIFY(WindowFrame::attach(window));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    for (const auto edges :
         {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge),
          Qt::Edges(Qt::TopEdge), Qt::Edges(Qt::BottomEdge),
          Qt::LeftEdge | Qt::TopEdge, Qt::RightEdge | Qt::TopEdge,
          Qt::LeftEdge | Qt::BottomEdge, Qt::RightEdge | Qt::BottomEdge}) {
      window.setGeometry(100, 100, 320, 240);
      QCoreApplication::processEvents();
      const QRect original = window.geometry();
      const QPoint press(edges & Qt::LeftEdge    ? 2
                         : edges & Qt::RightEdge ? window.width() - 3
                                                 : window.width() / 2,
                         edges & Qt::TopEdge      ? 2
                         : edges & Qt::BottomEdge ? window.height() - 3
                                                  : window.height() / 2);
      QPoint global = window.mapToGlobal(press);
      const auto send = [&](QEvent::Type type, QPoint at) {
        QMouseEvent event(
            type, window.mapFromGlobal(at), at,
            type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton,
            type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton,
            Qt::NoModifier);
        QCoreApplication::sendEvent(window.windowHandle(), &event);
      };
      send(QEvent::MouseButtonPress, global);
      const QPoint delta(edges & Qt::LeftEdge    ? -40
                         : edges & Qt::RightEdge ? 40
                                                 : 0,
                         edges & Qt::TopEdge      ? -30
                         : edges & Qt::BottomEdge ? 30
                                                  : 0);
      send(QEvent::MouseMove, global + delta * 100);
      QCOMPARE(window.width(), delta.x() ? 500 : original.width());
      QCOMPARE(window.height(), delta.y() ? 400 : original.height());
      // Cocoa may classify the constrained maximum as a zoomed window.
      // Reproduce that notification on the headless backend as well.
      if (platform == QLatin1String("offscreen")) {
        window.windowHandle()->setWindowStates(Qt::WindowMaximized);
        QWindowStateChangeEvent maximized(Qt::WindowNoState);
        QCoreApplication::sendEvent(window.windowHandle(), &maximized);
      }
      QVERIFY(QWidget::mouseGrabber() == &window);
      if (platform == QLatin1String("offscreen")) {
        window.windowHandle()->setWindowStates(Qt::WindowNoState);
        QWindowStateChangeEvent restored(Qt::WindowMaximized);
        QCoreApplication::sendEvent(window.windowHandle(), &restored);
      }
      if (restartAtMaximum) {
        send(QEvent::MouseButtonRelease, global + delta * 100);
        const QPoint edge(edges & Qt::LeftEdge    ? 2
                          : edges & Qt::RightEdge ? window.width() - 3
                                                  : window.width() / 2,
                          edges & Qt::TopEdge      ? 2
                          : edges & Qt::BottomEdge ? window.height() - 3
                                                   : window.height() / 2);
        global = window.mapToGlobal(edge);
        send(QEvent::MouseButtonPress, global);
      }
      QVERIFY2(QWidget::mouseGrabber() == &window,
               qPrintable(QStringLiteral("Resize lost its grab in state %1")
                              .arg(int(window.windowState()))));
      // Cocoa can report the same normal state during a geometry change.
      // That notification must not end the gesture before its release.
      QWindowStateChangeEvent unchanged(window.windowState());
      QCoreApplication::sendEvent(window.windowHandle(), &unchanged);
      send(QEvent::MouseMove, global - delta * 100);
      QCOMPARE(window.width(), delta.x() ? 240 : original.width());
      QCOMPARE(window.height(), delta.y() ? 180 : original.height());
      if (restartAtMaximum) {
        send(QEvent::MouseButtonRelease, global - delta * 100);
        if (edges & Qt::LeftEdge)
          QCOMPARE(window.geometry().right(), original.right());
        else
          QCOMPARE(window.x(), original.x());
        if (edges & Qt::TopEdge)
          QCOMPARE(window.geometry().bottom(), original.bottom());
        else
          QCOMPARE(window.y(), original.y());
        continue;
      }
      send(QEvent::MouseMove, global + delta);
      send(QEvent::MouseButtonRelease, global + delta);
      QCOMPARE(window.width(), original.width() + (delta.x() ? 40 : 0));
      QCOMPARE(window.height(), original.height() + (delta.y() ? 30 : 0));
      if (edges & Qt::LeftEdge)
        QCOMPARE(window.geometry().right(), original.right());
      else
        QCOMPARE(window.x(), original.x());
      if (edges & Qt::TopEdge)
        QCOMPARE(window.geometry().bottom(), original.bottom());
      else
        QCOMPARE(window.y(), original.y());
    }
  }

private:
  QTemporaryDir settings_;
};

// QTEST_MAIN forces 96 DPI, which masks Cocoa's native point-size behavior.
// Appearance checks and their captures must use the same DPI as the product.
int main(int argc, char **argv) {
  AppearanceTests::initMain();
  QApplication app(argc, argv);
  AppearanceTests tests;
  return QTest::qExec(&tests, argc, argv);
}
#include "AppearanceTests.moc"
