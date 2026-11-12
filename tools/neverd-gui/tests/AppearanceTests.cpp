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
#include "DevicePixels.h"
#include "Docking.h"
#include "GnomeModalDialogs.h"
#include "HighlightLocks.h"
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
#include <QCheckBox>
#include <QColorDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFontDialog>
#include <QFontInfo>
#include <QGlyphRun>
#include <QGroupBox>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QItemSelectionModel>
#include <QKeySequence>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardItemModel>
#include <QStyle>
#include <QStyleHints>
#include <QStyleOption>
#include <QSvgRenderer>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextLayout>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QWheelEvent>
#include <QWindow>
#include <algorithm>
#include <cmath>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <memory>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

using namespace neverd::gui;

namespace {
/// \p color composited over the opaque \p surface.
QColor over(const QColor &color, const QColor &surface) {
  const qreal a = color.alphaF();
  return QColor::fromRgbF(color.redF() * a + surface.redF() * (1 - a),
                          color.greenF() * a + surface.greenF() * (1 - a),
                          color.blueF() * a + surface.blueF() * (1 - a));
}

/// WCAG's relative luminance of the opaque \p color.
qreal luminance(const QColor &color) {
  const auto channel = [](qreal c) {
    return c <= 0.03928 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * channel(color.redF()) + 0.7152 * channel(color.greenF()) +
         0.0722 * channel(color.blueF());
}

/// WCAG's contrast ratio of \p ink, composited over the opaque \p surface.
qreal contrast(const QColor &ink, const QColor &surface) {
  const qreal a = luminance(over(ink, surface)), b = luminance(surface);
  return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

/// Whether \p a and \p b differ by at most \p tolerance in every channel.
bool withinTolerance(const QColor &a, const QColor &b, int tolerance) {
  return std::abs(a.red() - b.red()) <= tolerance &&
         std::abs(a.green() - b.green()) <= tolerance &&
         std::abs(a.blue() - b.blue()) <= tolerance;
}
} // namespace

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
#ifdef Q_OS_WIN
      // A native sizing border must not inset our frameless client area.
      // Otherwise fractional-scale screen mapping samples an inner shadow
      // pixel even though the backing store paints at the window origin.
      const auto handle = reinterpret_cast<HWND>(dialog->winId());
      RECT frame{}, client{};
      POINT origin{};
      QVERIFY(GetWindowRect(handle, &frame));
      QVERIFY(GetClientRect(handle, &client));
      QVERIFY(ClientToScreen(handle, &origin));
      QCOMPARE(origin.x, frame.left);
      QCOMPARE(origin.y, frame.top);
      QCOMPARE(client.right - client.left, frame.right - frame.left);
      QCOMPARE(client.bottom - client.top, frame.bottom - frame.top);
#endif
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

  void menuShortcutsReadQuieterThanCommands_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("light") << false;
    QTest::newRow("dark") << true;
  }

  void menuShortcutsReadQuieterThanCommands() {
    QFETCH(bool, dark);
    Theme::instance().setMode(dark ? Theme::Mode::Dark : Theme::Mode::Light);
    Theme::instance().apply();
    QMenu menu;
    auto *quiet = menu.addAction(QStringLiteral("Menu entry"));
    quiet->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    auto *highlighted = menu.addAction(QStringLiteral("Menu entry"));
    highlighted->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_O));
    menu.ensurePolished();
    menu.resize(menu.sizeHint());
    menu.setActiveAction(highlighted);
    const auto capture = menu.grab().toImage();
    const qreal dpr = capture.devicePixelRatio();
    // The label and the shortcut are the two runs of ink the widest gap in a
    // row separates; each reads by its strongest contrast with the row.
    const auto contrasts = [&](QAction *action, const QColor &background) {
      // Inside the row, clear of the highlight's rounded corners and of the
      // menu's padding, only text differs from the row's background.
      const QRect row = menu.actionGeometry(action).adjusted(3, 3, -3, -3);
      const auto distance = [&](const QColor &pixel) {
        return std::max({qAbs(pixel.red() - background.red()),
                         qAbs(pixel.green() - background.green()),
                         qAbs(pixel.blue() - background.blue())});
      };
      const int left = qRound(row.left() * dpr);
      const int right = qRound((row.right() + 1) * dpr);
      QVector<int> columns(right - left);
      for (int y = qRound(row.top() * dpr);
           y < qRound((row.bottom() + 1) * dpr); ++y)
        for (int x = left; x < right; ++x)
          columns[x - left] =
              std::max(columns[x - left], distance(capture.pixelColor(x, y)));
      int split = -1, widest = 0, last = -1;
      for (int x = 0; x < columns.size(); ++x) {
        if (columns[x] <= 24)
          continue;
        if (last >= 0 && x - last > widest) {
          widest = x - last;
          split = x;
        }
        last = x;
      }
      int label = 0, shortcut = 0;
      for (int x = 0; x < columns.size(); ++x)
        (x < split ? label : shortcut) =
            std::max(x < split ? label : shortcut, columns[x]);
      return std::pair(label, shortcut);
    };
    // Visual Studio Code dims a keybinding to 70% until its item is
    // highlighted; a style sheet rule for menu items would paint it with
    // the label's pen again.
    const auto [label, shortcut] =
        contrasts(quiet, Theme::instance().chrome("Menu"));
    QVERIFY2(
        shortcut < label * 0.85,
        qPrintable(
            QStringLiteral("label %1, shortcut %2").arg(label).arg(shortcut)));
    const auto [activeLabel, activeShortcut] =
        contrasts(highlighted, Theme::instance().chrome("MenuSelection"));
    QVERIFY2(activeShortcut >= activeLabel * 0.9,
             qPrintable(QStringLiteral("label %1, shortcut %2")
                            .arg(activeLabel)
                            .arg(activeShortcut)));
  }

  void themeFollowsTheDesktopWhileChosen() {
    Theme &theme = Theme::instance();
    const auto restore = qScopeGuard([&] {
      theme.setMode(Theme::Mode::Dark);
      theme.apply();
    });
    theme.setMode(Theme::Mode::Dark);
    QSignalSpy changed(&theme, &Theme::changed);
    // The desktop's scheme, which the offscreen platform does not know: an
    // unknown one keeps the theme shown.
    const Qt::ColorScheme desktop =
        QGuiApplication::styleHints()->colorScheme();
    const Theme::Mode expected = desktop == Qt::ColorScheme::Light
                                     ? Theme::Mode::Light
                                     : Theme::Mode::Dark;
    theme.setChoice(Theme::Choice::System);
    QCOMPARE(theme.choice(), Theme::Choice::System);
    QCOMPARE(QSettings().value(QStringLiteral("appearance/theme")).toString(),
             QStringLiteral("system"));
    QCOMPARE(theme.mode(), expected);
    QCOMPARE(changed.count(), expected == Theme::Mode::Dark ? 0 : 1);
    changed.clear();
    // It takes the desktop's light or dark mode as it changes.
    theme.followColorScheme(Qt::ColorScheme::Dark);
    theme.followColorScheme(Qt::ColorScheme::Light);
    QCOMPARE(theme.mode(), Theme::Mode::Light);
    changed.clear();
    theme.followColorScheme(Qt::ColorScheme::Dark);
    QCOMPARE(theme.mode(), Theme::Mode::Dark);
    QCOMPARE(changed.count(), 1);
    theme.followColorScheme(Qt::ColorScheme::Light);
    QCOMPARE(theme.mode(), Theme::Mode::Light);
    QCOMPARE(changed.count(), 2);
    QCOMPARE(qApp->palette().color(QPalette::Window),
             theme.chrome(QStringLiteral("Window")));
    QCOMPARE(theme.chrome(QStringLiteral("Window")), QColor(0xF3F3F3));
    theme.followColorScheme(Qt::ColorScheme::Light);
    QCOMPARE(changed.count(), 2);
    theme.followColorScheme(Qt::ColorScheme::Dark);
    QCOMPARE(theme.mode(), Theme::Mode::Dark);
    QCOMPARE(theme.chrome(QStringLiteral("Window")), QColor(0x252526));
    theme.followColorScheme(Qt::ColorScheme::Unknown);
    QCOMPARE(theme.mode(), Theme::Mode::Dark);
    // A theme chosen by name stays whatever the desktop does.
    theme.setChoice(Theme::Choice::Light);
    QCOMPARE(theme.mode(), Theme::Mode::Light);
    QCOMPARE(QSettings().value(QStringLiteral("appearance/theme")).toString(),
             QStringLiteral("light"));
    theme.followColorScheme(Qt::ColorScheme::Dark);
    QCOMPARE(theme.mode(), Theme::Mode::Light);
    QCOMPARE(Theme::choiceFromName(QStringLiteral("system")),
             Theme::Choice::System);
    QCOMPARE(Theme::choiceFromName(QStringLiteral("bogus")),
             Theme::Choice::Dark);
  }

  void highContrastThemesReadClearly_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }

  void highContrastThemesReadClearly() {
    QFETCH(bool, dark);
    Theme &theme = Theme::instance();
    const auto restore = qScopeGuard([&] { theme.setMode(Theme::Mode::Dark); });
    theme.setMode(dark ? Theme::Mode::HighContrastDark
                       : Theme::Mode::HighContrastLight);
    QVERIFY(theme.highContrast());
    QCOMPARE(theme.dark(), dark);
    QCOMPARE(Theme::modeName(theme.mode()),
             dark ? QStringLiteral("dark") : QStringLiteral("light"));
    // A translucent color is composited over its surface first; a tint
    // \p under text, over the surface \p under itself sits on.
    const auto check = [&](const char *ink, const char *surface, qreal least,
                           const char *under = nullptr) {
      QColor behind = theme.chrome(QLatin1String(surface));
      if (under)
        behind = over(behind, theme.chrome(QLatin1String(under)));
      const qreal value = contrast(theme.chrome(QLatin1String(ink)), behind);
      QVERIFY2(value >= least,
               qPrintable(QStringLiteral("%1 on %2: %3:1, under %4:1")
                              .arg(QLatin1String(ink), QLatin1String(surface))
                              .arg(value, 0, 'f', 2)
                              .arg(least)));
    };
    // Text, at the 7:1 of WCAG's enhanced contrast.
    for (const auto &[ink, surface] : {std::pair{"WindowText", "Window"},
                                       {"Text", "Base"},
                                       {"Text", "Input"},
                                       {"MenuText", "Menu"},
                                       {"ButtonText", "Button"},
                                       {"ButtonPrimaryText", "ButtonPrimary"},
                                       {"TabActiveText", "TabActive"},
                                       {"TabInactiveText", "TabInactive"},
                                       {"StatusBarText", "StatusBar"},
                                       {"TitleBarText", "TitleBar"},
                                       {"MenuBarText", "MenuBar"},
                                       {"HeaderText", "HeaderBackground"},
                                       {"ToolTipText", "ToolTipBase"},
                                       {"DockTitleText", "DockTitle"},
                                       {"QuickStartTitle", "QuickStartSide"}})
      check(ink, surface, 7.0);
    // Highlighted and hovered items keep their text at 7:1 on their tint.
    check("MenuSelectionText", "MenuSelection", 7.0, "Menu");
    check("ListSelectionText", "ListSelection", 7.0, "Base");
    check("Text", "Hover", 7.0, "Base");
    check("ToolButtonCheckedText", "ToolButtonChecked", 7.0);
    // Secondary text, links and selections, at the 4.5:1 of the minimum.
    for (const auto &[ink, surface] :
         {std::pair{"SecondaryText", "Window"},
          {"PlaceholderText", "Input"},
          {"Link", "Base"},
          {"HighlightedText", "Highlight"},
          {"QuickStartSelectionText", "QuickStartSelection"}})
      check(ink, surface, 4.5);
    // WCAG exempts what cannot be used; Visual Studio Code's disabled gray is
    // 4:1 on the light theme's white, still legible.
    check("DisabledText", "Window", 3.0);
    // Every line and outline, at the 3:1 of non-text contrast.
    for (const auto &[line, surface] :
         {std::pair{"Border", "Window"},
          {"InputBorder", "Input"},
          {"Focus", "Window"},
          {"CheckboxBorder", "CheckboxBackground"},
          {"MenuSeparator", "Menu"},
          {"MenuSelectionBorder", "Menu"},
          {"OptionActiveBorder", "Menu"},
          {"MenuBarSelectionBorder", "MenuBar"},
          {"StatusBarBorder", "StatusBar"},
          {"TabSeparator", "TabBar"},
          {"TabActiveBorder", "TabActive"},
          {"ListSelectionBorder", "Base"},
          {"ToolBarHoverBorder", "ToolBar"},
          {"ButtonHoverBorder", "Window"},
          {"ButtonPrimaryBorder", "Window"},
          {"DockSeparator", "Window"},
          {"WindowEdge", "Window"},
          {"Separator", "ToolBar"},
          {"ScrollbarSliderActive", "Base"}})
      check(line, surface, 3.0);
    // The analysis views' text on their surfaces.
    const auto view = [&](ColorRole ink, ColorRole surface, qreal least) {
      const qreal value = contrast(theme.color(ink), theme.color(surface));
      QVERIFY2(value >= least,
               qPrintable(QStringLiteral("view role %1 on %2: %3:1")
                              .arg(int(ink))
                              .arg(int(surface))
                              .arg(value, 0, 'f', 2)));
    };
    view(ColorRole::ListingPlain, ColorRole::ListingBackground, 7.0);
    view(ColorRole::CodeDefault, ColorRole::CodeBackground, 7.0);
    view(ColorRole::HexByte, ColorRole::HexBackground, 7.0);
    view(ColorRole::ListingSelectionText, ColorRole::ListingSelection, 7.0);
    view(ColorRole::HexCurrentText, ColorRole::HexCurrent, 7.0);
    for (const auto role :
         {ColorRole::ListingMnemonic, ColorRole::ListingRegister,
          ColorRole::ListingNumber, ColorRole::ListingCodeName,
          ColorRole::ListingDataName, ColorRole::ListingString,
          ColorRole::ListingComment, ColorRole::ListingLibraryName,
          ColorRole::ListingImportName, ColorRole::ListingError,
          ColorRole::ListingAutoComment})
      view(role, ColorRole::ListingBackground, 4.5);
    for (const auto role :
         {ColorRole::CodeKeyword, ColorRole::CodeControl, ColorRole::CodeType,
          ColorRole::CodeNumber, ColorRole::CodeString, ColorRole::CodeComment,
          ColorRole::CodeFunction, ColorRole::CodeVariable,
          ColorRole::CodeConstant, ColorRole::CodeLineNumber})
      view(role, ColorRole::CodeBackground, 4.5);
    view(ColorRole::ListingCurrentLineBorder, ColorRole::ListingBackground,
         3.0);
    view(ColorRole::ListingHighlightBorder, ColorRole::ListingBackground, 3.0);
    // The cursor's highlight outlines a word and leaves its syntax colors on
    // the view's own background.
    QCOMPARE(theme.color(ColorRole::ListingHighlight).alpha(), 0);
  }

  void highContrastFollowsTheDesktop() {
    Theme &theme = Theme::instance();
    const auto restore = qScopeGuard([&] { theme.setMode(Theme::Mode::Dark); });
    theme.setMode(Theme::Mode::Dark);
    theme.setChoice(Theme::Choice::System);
    using platform::DesktopContrast;
    // A desktop that asks for high contrast gets the high contrast theme of
    // its brightness, whatever its color scheme says.
    theme.followDesktop(Qt::ColorScheme::Light, DesktopContrast::HighDark);
    QCOMPARE(theme.mode(), Theme::Mode::HighContrastDark);
    QCOMPARE(theme.chrome(QStringLiteral("Window")), QColor(Qt::black));
    theme.followDesktop(Qt::ColorScheme::Unknown, DesktopContrast::HighLight);
    QCOMPARE(theme.mode(), Theme::Mode::HighContrastLight);
    QCOMPARE(theme.chrome(QStringLiteral("Window")), QColor(Qt::white));
    // Turning high contrast off returns to the scheme's theme.
    theme.followDesktop(Qt::ColorScheme::Dark, DesktopContrast::Normal);
    QCOMPARE(theme.mode(), Theme::Mode::Dark);
    // A high contrast theme chosen by name stays.
    theme.setChoice(Theme::Choice::HighContrastLight);
    QCOMPARE(QSettings().value(QStringLiteral("appearance/theme")).toString(),
             QStringLiteral("hc-light"));
    theme.followDesktop(Qt::ColorScheme::Dark, DesktopContrast::HighDark);
    QCOMPARE(theme.mode(), Theme::Mode::HighContrastLight);
    QCOMPARE(Theme::choiceFromName(QStringLiteral("hc-dark")),
             Theme::Choice::HighContrastDark);
    // Only a high contrast theme adds its style sheet, which keeps an outline
    // around every menu bar item.
    const QString overlay =
        QStringLiteral("QMenuBar::item { border: 1px solid transparent");
    QVERIFY(qApp->styleSheet().contains(overlay));
    theme.setMode(Theme::Mode::Light);
    QVERIFY(!qApp->styleSheet().contains(overlay));
  }

  void highContrastOutlinesWhatOthersFill_data() {
    QTest::addColumn<bool>("dark");
    QTest::newRow("dark") << true;
    QTest::newRow("light") << false;
  }

  void highContrastOutlinesWhatOthersFill() {
    QFETCH(bool, dark);
    Theme &theme = Theme::instance();
    const auto restore = qScopeGuard([&] { theme.setMode(Theme::Mode::Dark); });
    theme.setMode(dark ? Theme::Mode::HighContrastDark
                       : Theme::Mode::HighContrastLight);
    {
      QMenu menu;
      menu.addAction(QStringLiteral("First"));
      auto *highlighted = menu.addAction(QStringLiteral("Second"));
      menu.ensurePolished();
      menu.resize(menu.sizeHint());
      menu.setActiveAction(highlighted);
      const QImage image = menu.grab().toImage();
      const QRect row = menu.actionGeometry(highlighted);
      const qreal dpr = image.devicePixelRatio();
      const auto at = [&](int x, int y) {
        return image.pixelColor(qRound(x * dpr), qRound(y * dpr));
      };
      // The highlighted item keeps the menu's color, at most tinted, and takes
      // an outline along its middle row's ends.
      const QColor outline =
          theme.chrome(QStringLiteral("MenuSelectionBorder"));
      const QColor tint = over(theme.chrome(QStringLiteral("MenuSelection")),
                               theme.chrome(QStringLiteral("Menu")));
      const int middle = row.top() + row.height() / 2;
      QCOMPARE(at(row.left(), middle), outline);
      QCOMPARE(at(row.right(), middle), outline);
      QVERIFY(withinTolerance(at(row.left() + 2, middle), tint, 1));
      // Its text stays legible on the tint.
      qreal best = 1;
      for (int y = row.top() + 3; y <= row.bottom() - 3; ++y)
        for (int x = row.left() + 3; x <= row.right() - 3; ++x)
          best = std::max(best, contrast(at(x, y), tint));
      QVERIFY2(best >= 7, qPrintable(QString::number(best)));
    }
    // The views' highlight of the word under the cursor, and of search
    // matches, outlines the word and keeps the background.
    {
      QImage canvas(40, 20, QImage::Format_ARGB32);
      const QColor background = theme.color(ColorRole::ListingBackground);
      canvas.fill(background);
      QPainter painter(&canvas);
      paintHighlight(painter, QRectF(4.5, 2, 20.25, 16),
                     ColorRole::ListingHighlight);
      painter.end();
      const QColor outline = theme.color(ColorRole::ListingHighlightBorder);
      QCOMPARE(canvas.pixelColor(4, 10), outline);
      QCOMPARE(canvas.pixelColor(12, 2), outline);
      QCOMPARE(canvas.pixelColor(24, 10), outline);
      QCOMPARE(canvas.pixelColor(12, 10), background);
    }
    // A list's selected row takes one outline around all its cells, with its
    // text in the list's color.
    QStandardItemModel model(3, 2);
    for (int row = 0; row < model.rowCount(); ++row)
      for (int column = 0; column < model.columnCount(); ++column)
        model.setItem(row, column, new QStandardItem(QStringLiteral("Cell")));
    QTreeView view;
    view.setModel(&model);
    view.setRootIsDecorated(false);
    view.header()->hide();
    view.resize(320, 120);
    view.setColumnWidth(0, 160);
    view.selectionModel()->select(model.index(1, 0),
                                  QItemSelectionModel::Select |
                                      QItemSelectionModel::Rows);
    // Item delegates draw a selected row's own text in this color.
    view.ensurePolished();
    QCOMPARE(view.palette().color(QPalette::HighlightedText),
             theme.chrome(QStringLiteral("ListSelectionText")));
    const QImage image = view.grab().toImage();
    const QPoint origin = view.viewport()->mapTo(&view, QPoint());
    const qreal dpr = image.devicePixelRatio();
    const auto at = [&](const QPoint &point) {
      return image.pixelColor(qRound((point.x() + origin.x()) * dpr),
                              qRound((point.y() + origin.y()) * dpr));
    };
    const QRect first = view.visualRect(model.index(1, 0));
    const QRect second = view.visualRect(model.index(1, 1));
    const QColor outline = theme.chrome(QStringLiteral("ListSelectionBorder"));
    const int middle = first.center().y();
    QCOMPARE(at(QPoint(first.center().x(), first.top())), outline);
    QCOMPARE(at(QPoint(second.center().x(), second.bottom())), outline);
    QCOMPARE(at(QPoint(first.left(), middle)), outline);
    QCOMPARE(at(QPoint(second.right(), middle)), outline);
    QVERIFY(at(QPoint(first.right(), middle)) != outline);
    QVERIFY(at(QPoint(second.left(), middle)) != outline);
  }

  void deviceBoxesPaintAsStyleSheetsAtWholeScales_data() {
    QTest::addColumn<QString>("rule");
    QTest::addColumn<QSize>("size");
    QTest::addColumn<QColor>("fill");
    QTest::addColumn<QColor>("border");
    QTest::addColumn<int>("edges");
    QTest::addColumn<int>("radius");
    QTest::addColumn<qreal>("scale");
    const int all = Qt::TopEdge | Qt::LeftEdge | Qt::RightEdge | Qt::BottomEdge;
    const QColor none;
    const QColor faint = QColor::fromRgba(0x660E639Cu);
    struct Box {
      const char *name;
      QSize size;
      QColor fill, border;
      int edges, radius;
    };
    const Box boxes[] = {
        {"field", {120, 24}, QColor(0xFFFFFF), QColor(0xCECECE), all, 2},
        {"tool-hover", {24, 24}, QColor(0xE1E1E1), none, all, 3},
        {"editor", {120, 60}, QColor(0xFFFFFF), none, 0, 2},
        {"tool-focus", {24, 22}, none, QColor(0x0090F1), all, 3},
        {"check", {16, 16}, QColor(0xFFFFFF), QColor(0x919191), all, 3},
        {"radio", {16, 16}, QColor(0x007ACC), QColor(0x007ACC), all, 8},
        {"card", {300, 80}, QColor(0xFFFFFF), QColor(0xE5E5E5), all, 6},
        {"faint-fill", {75, 26}, faint, none, all, 2},
        {"faint-border", {60, 20}, QColor(0xFFFFFF), faint, all, 2},
        {"menu", {180, 120}, QColor(0x252526), QColor(0x454545), all, 0},
        {"heading",
         {90, 22},
         QColor(0xF3F3F3),
         QColor(0xE5E5E5),
         Qt::RightEdge | Qt::BottomEdge,
         0},
        {"status",
         {300, 22},
         QColor(0x007ACC),
         QColor(0xE5E5E5),
         Qt::TopEdge,
         0}};
    for (const Box &box : boxes)
      for (const qreal scale : {1.0, 2.0}) {
        QString rule =
            QStringLiteral("QWidget { background: %1; border: none; ")
                .arg(box.fill.isValid() ? box.fill.name(QColor::HexArgb)
                                        : QStringLiteral("none"));
        const std::pair<Qt::Edge, const char *> sides[] = {
            {Qt::TopEdge, "top"},
            {Qt::RightEdge, "right"},
            {Qt::BottomEdge, "bottom"},
            {Qt::LeftEdge, "left"}};
        for (const auto &[edge, side] : sides)
          if (box.edges & edge)
            rule +=
                QStringLiteral("border-%1: 1px solid %2; ")
                    .arg(QLatin1String(side),
                         box.border.isValid() ? box.border.name(QColor::HexArgb)
                                              : QStringLiteral("transparent"));
        if (box.radius)
          rule += QStringLiteral("border-radius: %1px; ").arg(box.radius);
        rule += QLatin1Char('}');
        QTest::newRow(qPrintable(
            QStringLiteral("%1-%2x").arg(QLatin1String(box.name)).arg(scale)))
            << rule << box.size << box.fill << box.border << box.edges
            << box.radius << scale;
      }
  }

  void deviceBoxesPaintAsStyleSheetsAtWholeScales() {
    QFETCH(QString, rule);
    QFETCH(QSize, size);
    QFETCH(QColor, fill);
    QFETCH(QColor, border);
    QFETCH(int, edges);
    QFETCH(int, radius);
    QFETCH(qreal, scale);
    // The workbench paints its controls' boxes itself; where every logical
    // pixel covers whole device pixels, a box is exactly the style sheet's.
    const auto canvas = [&] {
      QImage image((size + QSize(8, 8)) * scale,
                   QImage::Format_ARGB32_Premultiplied);
      image.setDevicePixelRatio(scale);
      image.fill(QColor(0x808080));
      return image;
    };
    QImage expected = canvas();
    QWidget styled;
    styled.setAttribute(Qt::WA_StyledBackground);
    styled.setStyleSheet(rule);
    styled.resize(size);
    styled.ensurePolished();
    styled.render(&expected, QPoint(4, 4), QRegion(), QWidget::RenderFlags());
    QImage actual = canvas();
    {
      QPainter painter(&actual);
      painter.translate(4, 4);
      // As Qt clips a widget's painting to the widget.
      painter.setClipRect(QRect(QPoint(), size));
      DeviceBox box;
      box.fill = fill;
      box.border = border.isValid() ? border : QColor(Qt::transparent);
      box.edges = Qt::Edges(edges);
      box.radius = radius;
      paintDeviceBox(painter, QRect(QPoint(), size), box);
    }
    QCOMPARE(actual, expected);
  }

  void controlsKeepTheirLinesOnDevicePixels_data() {
    QTest::addColumn<QString>("kind");
    QTest::addColumn<qreal>("scale");
    for (const char *kind :
         {"field", "button", "default", "check", "group", "heading", "tab"})
      for (const qreal scale : {1.0, 1.25, 1.5, 1.75})
        QTest::newRow(qPrintable(
            QStringLiteral("%1-%2x").arg(QLatin1String(kind)).arg(scale)))
            << QString::fromLatin1(kind) << scale;
  }

  void controlsKeepTheirLinesOnDevicePixels() {
    QFETCH(QString, kind);
    QFETCH(qreal, scale);
    Theme::instance().setMode(Theme::Mode::Light);
    Theme::instance().apply();
    const Theme &theme = Theme::instance();
    QWidget window;
    window.setAttribute(Qt::WA_StyledBackground);
    window.setStyleSheet(QStringLiteral("QWidget#surface { background: %1; }")
                             .arg(theme.chrome("Window").name()));
    window.setObjectName(QStringLiteral("surface"));
    window.resize(220, 120);
    // The control's box, in the window, and the colors a line across it may
    // take: the surface, the box's border and its fill.
    QWidget *control = nullptr;
    QRect box;
    QList<QColor> allowed = {theme.chrome("Window")};
    // Headings without labels, so only the header's own lines cross them.
    QStandardItemModel headings(0, 2);
    headings.setHorizontalHeaderLabels(
        {QStringLiteral(" "), QStringLiteral(" ")});
    if (kind == QLatin1String("field")) {
      control = new QLineEdit(&window);
      allowed << theme.chrome("InputBorder") << theme.chrome("Input");
    } else if (kind == QLatin1String("button") ||
               kind == QLatin1String("default")) {
      auto *button = new QPushButton(&window);
      button->setDefault(kind == QLatin1String("default"));
      control = button;
      allowed << theme.chrome(button->isDefault() ? "ButtonPrimary" : "Border")
              << theme.chrome(button->isDefault() ? "ButtonPrimary" : "Button");
    } else if (kind == QLatin1String("check")) {
      control = new QCheckBox(&window);
      allowed << theme.chrome("CheckboxBorder")
              << theme.chrome("CheckboxBackground");
    } else if (kind == QLatin1String("group")) {
      control = new QGroupBox(&window);
      allowed << theme.chrome("Border");
    } else if (kind == QLatin1String("tab")) {
      // Tabs without labels, the first selected.
      auto *tabs = new QTabBar(&window);
      tabs->addTab(QStringLiteral(" "));
      tabs->addTab(QStringLiteral(" "));
      control = tabs;
      allowed << theme.chrome("TabBar") << theme.chrome("TabActive")
              << theme.chrome("TabInactive") << theme.chrome("TabActiveBorder");
    } else {
      auto *view = new QTreeView(&window);
      view->setModel(&headings);
      control = view;
      allowed << theme.chrome("Border") << theme.chrome("HeaderBackground")
              << theme.chrome("Base");
    }
    // At rest: a field with focus takes the focus color's outline.
    control->setFocusPolicy(Qt::NoFocus);
    control->setGeometry(13, 17, 150, 60);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    if (auto *check = qobject_cast<QCheckBox *>(control)) {
      QStyleOptionButton option;
      option.initFrom(check);
      box = check->style()->subElementRect(QStyle::SE_CheckBoxIndicator,
                                           &option, check);
    } else if (auto *group = qobject_cast<QGroupBox *>(control)) {
      QStyleOptionGroupBox option;
      option.initFrom(group);
      option.subControls = QStyle::SC_GroupBoxFrame;
      box = group->style()->subControlRect(QStyle::CC_GroupBox, &option,
                                           QStyle::SC_GroupBoxFrame, group);
    } else if (auto *view = qobject_cast<QTreeView *>(control)) {
      box = view->header()->geometry();
    } else if (auto *tabs = qobject_cast<QTabBar *>(control)) {
      box = tabs->tabRect(0);
    } else {
      box = control->rect();
    }
    box.translate(control->pos());
    // Each logical offset puts the box at another quarter of a device pixel.
    for (int offset = 0; offset < 4; ++offset) {
      QImage image(QSize(240, 140) * scale,
                   QImage::Format_ARGB32_Premultiplied);
      image.setDevicePixelRatio(scale);
      image.fill(theme.chrome("Window"));
      window.render(&image, QPoint(offset, offset));
      const QRect device(
          QPoint(qFloor((box.left() + offset) * scale) - 2,
                 qFloor((box.top() + offset) * scale) - 2),
          QPoint(qCeil((box.right() + 1 + offset) * scale) + 1,
                 qCeil((box.bottom() + 1 + offset) * scale) + 1));
      const auto check = [&](QPoint pixel) {
        const QColor color = image.pixelColor(pixel);
        return std::any_of(
            allowed.begin(), allowed.end(),
            [&](const QColor &c) { return c.rgb() == color.rgb(); });
      };
      const int middleY = (device.top() + device.bottom()) / 2;
      const int middleX = (device.left() + device.right()) / 2;
      for (int x = device.left(); x <= device.right(); ++x)
        QVERIFY2(check({x, middleY}),
                 qPrintable(QStringLiteral("offset %1: %2 at (%3, %4)")
                                .arg(offset)
                                .arg(image.pixelColor(x, middleY).name())
                                .arg(x)
                                .arg(middleY)));
      for (int y = device.top(); y <= device.bottom(); ++y)
        QVERIFY2(check({middleX, y}),
                 qPrintable(QStringLiteral("offset %1: %2 at (%3, %4)")
                                .arg(offset)
                                .arg(image.pixelColor(middleX, y).name())
                                .arg(middleX)
                                .arg(y)));
    }
  }

  void controlsThatCannotActRecede_data() {
    QTest::addColumn<int>("mode");
    QTest::addColumn<QString>("kind");
    QTest::addColumn<qreal>("scale");
    for (const auto mode :
         {Theme::Mode::Dark, Theme::Mode::Light, Theme::Mode::HighContrastDark,
          Theme::Mode::HighContrastLight})
      for (const char *kind : {"button", "field"})
        for (const qreal scale : {1.0, 1.25})
          QTest::newRow(qPrintable(QStringLiteral("%1-%2-%3x")
                                       .arg(int(mode))
                                       .arg(QLatin1String(kind))
                                       .arg(scale)))
              << int(mode) << QString::fromLatin1(kind) << scale;
  }

  void controlsThatCannotActRecede() {
    QFETCH(int, mode);
    QFETCH(QString, kind);
    QFETCH(qreal, scale);
    Theme &theme = Theme::instance();
    const auto restore = qScopeGuard([&] { theme.setMode(Theme::Mode::Dark); });
    theme.setMode(Theme::Mode(mode));
    theme.apply();
    const QColor surface = theme.chrome("Window");
    QWidget window;
    window.setAttribute(Qt::WA_StyledBackground);
    window.setStyleSheet(QStringLiteral("QWidget#surface { background: %1; }")
                             .arg(surface.name()));
    window.setObjectName(QStringLiteral("surface"));
    window.resize(220, 120);
    QWidget *control = kind == QLatin1String("field")
                           ? static_cast<QWidget *>(new QLineEdit(&window))
                           : new QPushButton(&window);
    control->setEnabled(false);
    control->setGeometry(13, 17, 150, 60);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QImage image(QSize(240, 140) * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale);
    image.fill(surface);
    window.render(&image);
    // What the box may show: the surface, its plate, and its edge, which lies
    // on half the plate since a box fills to the middle of its border.
    const QColor fill = theme.chrome("ControlDisabled");
    const QColor line = theme.chrome("ControlDisabledBorder");
    const QColor plate = over(fill, surface);
    QColor half = fill;
    half.setAlphaF(fill.alphaF() / 2);
    const QColor edge = line.alpha() ? over(line, over(half, surface)) : plate;
    const QRect box = control->geometry();
    const QRect device(
        QPoint(qFloor(box.left() * scale) - 2, qFloor(box.top() * scale) - 2),
        QPoint(qCeil((box.right() + 1) * scale) + 1,
               qCeil((box.bottom() + 1) * scale) + 1));
    const QPoint middle = device.center();
    // Its lines lie on whole device pixels: no pixel across it is a blend.
    const auto known = [&](QPoint pixel) {
      const QColor color = image.pixelColor(pixel);
      return withinTolerance(color, surface, 1) ||
             withinTolerance(color, plate, 1) ||
             withinTolerance(color, edge, 1);
    };
    for (int x = device.left(); x <= device.right(); ++x)
      QVERIFY2(known({x, middle.y()}),
               qPrintable(QStringLiteral("%1 at (%2, %3)")
                              .arg(image.pixelColor(x, middle.y()).name())
                              .arg(x)
                              .arg(middle.y())));
    for (int y = device.top(); y <= device.bottom(); ++y)
      QVERIFY2(known({middle.x(), y}),
               qPrintable(QStringLiteral("%1 at (%2, %3)")
                              .arg(image.pixelColor(middle.x(), y).name())
                              .arg(middle.x())
                              .arg(y)));
    QVERIFY2(withinTolerance(image.pixelColor(middle), plate, 1),
             qPrintable(image.pixelColor(middle).name()));
    // The edge, where the theme draws one, is the first pixel of the box.
    int first = device.left();
    while (first < middle.x() &&
           withinTolerance(image.pixelColor(first, middle.y()), surface, 0))
      ++first;
    QVERIFY(first < middle.x());
    QVERIFY2(withinTolerance(image.pixelColor(first, middle.y()), edge, 1),
             qPrintable(image.pixelColor(first, middle.y()).name()));
    if (theme.highContrast()) {
      // An outline alone, at the 3:1 of non-text contrast.
      QCOMPARE(plate, surface);
      QVERIFY(contrast(edge, surface) >= 3.0);
      return;
    }
    // A plate that is there, with nothing about the box a quarter louder than
    // the surface, and quieter than the face of a button that can act.  An
    // outline alone, as loud as that face, read as an empty frame.
    QVERIFY2(contrast(plate, surface) >= 1.04,
             qPrintable(QString::number(contrast(plate, surface))));
    for (const QColor &part : {plate, edge})
      QVERIFY2(contrast(part, surface) <= 1.25,
               qPrintable(QString::number(contrast(part, surface))));
    QVERIFY(contrast(plate, surface) <
            contrast(theme.chrome("Button"), surface));
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
