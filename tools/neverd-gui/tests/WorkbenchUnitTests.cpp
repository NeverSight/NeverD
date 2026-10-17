// Unit tests of workbench building blocks that need no worker.
#include "ActionRegistry.h"
#include "Address.h"
#include "AddressSpace.h"
#include "DialogReturnKey.h"
#include "Docking.h"
#include "Expression.h"
#include "FlowLayout.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "LoadFileDialog.h"
#include "ProjectDatabase.h"
#include "StyledText.h"
#include "SymbolTablePromptDialog.h"
#include "Theme.h"
#include "TitleBar.h"
#include "TypeSourceHighlighter.h"
#include "WindowFrame.h"
#include "WindowFramePlatform.h"

#include <QAbstractButton>
#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDirIterator>
#include <QFile>
#include <QFontInfo>
#include <QHash>
#include <QHoverEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaEnum>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QScreen>
#include <QSet>
#include <QSplitter>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStyleOptionButton>
#include <QStyleOptionComboBox>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextLayout>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <kddockwidgets/KDDockWidgets.h>
#include <kddockwidgets/qtwidgets/views/DockWidget.h>
#include <kddockwidgets/qtwidgets/views/MainWindow.h>
#include <kddockwidgets/qtwidgets/views/Separator.h>
#include <memory>

using namespace neverd::gui;

namespace {
/// Two docks side by side in a main window, each filled with a plain color.
struct DockPair {
  static constexpr QColor LeftColor{0x10, 0x20, 0x30};
  static constexpr QColor RightColor{0x30, 0x20, 0x10};

  KDDockWidgets::QtWidgets::MainWindow window{QStringLiteral("DockPair")};
  KDDockWidgets::QtWidgets::DockWidget *left = dock("left", LeftColor);
  KDDockWidgets::QtWidgets::DockWidget *right = dock("right", RightColor);

  DockPair() {
    window.setCenterWidgetMargins(dockAreaMargins());
    window.addDockWidget(left, KDDockWidgets::Location_OnLeft);
    window.addDockWidget(right, KDDockWidgets::Location_OnRight);
    window.resize(640, 480);
    window.show();
  }
  static KDDockWidgets::QtWidgets::DockWidget *dock(const char *name,
                                                    QColor color) {
    auto *dock =
        new KDDockWidgets::QtWidgets::DockWidget(QString::fromLatin1(name));
    auto *content = new QWidget;
    content->setAutoFillBackground(true);
    QPalette palette = content->palette();
    palette.setColor(QPalette::Window, color);
    content->setPalette(palette);
    dock->setWidget(content);
    return dock;
  }
  QRect contentRect(KDDockWidgets::QtWidgets::DockWidget *dock) {
    auto *content = dock->widget();
    return {content->mapTo(&window, QPoint()), content->size()};
  }
  KDDockWidgets::QtWidgets::Separator *separator() {
    return window.findChild<KDDockWidgets::QtWidgets::Separator *>();
  }
  /// Column \p x of the window's middle row.
  QColor pixel(int x) {
    return window.grab().toImage().pixelColor(x,
                                              contentRect(left).center().y());
  }
};

/// The keys IDA's configuration means by \p spelling: "Ctrl-Shift-Down",
/// "Enter", or a platform key such as "sys(ZoomOut)".
QList<QKeySequence> idaKeys(QLatin1StringView spelling) {
  QString rest = spelling.toString();
  if (rest.isEmpty())
    return {};
  if (rest.startsWith(u"sys(") && rest.endsWith(u')')) {
    bool known = false;
    const int standard =
        QMetaEnum::fromType<QKeySequence::StandardKey>().keyToValue(
            rest.mid(4, rest.size() - 5).toLatin1().constData(), &known);
    return known ? QKeySequence::keyBindings(
                       static_cast<QKeySequence::StandardKey>(standard))
                 : QList<QKeySequence>{};
  }
  // IDA joins the modifiers with '-' and calls Return "Enter".
  QStringList parts;
  for (bool modifier = true; modifier;) {
    modifier = false;
    for (const auto name :
         {QLatin1StringView("Ctrl-"), QLatin1StringView("Shift-"),
          QLatin1StringView("Alt-"), QLatin1StringView("Meta-")})
      if (rest.size() > name.size() &&
          rest.startsWith(name, Qt::CaseInsensitive)) {
        parts << name.chopped(1).toString();
        rest.remove(0, name.size());
        modifier = true;
      }
  }
  parts << (rest == u"Enter" ? QStringLiteral("Return") : rest);
  const auto key =
      QKeySequence::fromString(parts.join(u'+'), QKeySequence::PortableText);
  if (key.isEmpty() || key[0].key() == Qt::Key_unknown)
    return {};
  return {key};
}
/// The window states the window system confirmed on a native window, which
/// it reports with a WindowStateChange event there.
class AppliedStates final : public QObject {
public:
  Qt::WindowStates states;
  int count = 0;

protected:
  bool eventFilter(QObject *object, QEvent *event) override {
    if (event->type() == QEvent::WindowStateChange) {
      states = static_cast<QWindow *>(object)->windowStates();
      ++count;
    }
    return false;
  }
};

/// The state a window shows, of several it holds at once.
Qt::WindowState effectiveState(Qt::WindowStates states) {
  if (states & Qt::WindowMinimized)
    return Qt::WindowMinimized;
  if (states & Qt::WindowFullScreen)
    return Qt::WindowFullScreen;
  if (states & Qt::WindowMaximized)
    return Qt::WindowMaximized;
  return Qt::WindowNoState;
}

} // namespace

class WorkbenchUnitTests : public QObject {
  Q_OBJECT
private slots:
  void initTestCase() {
    // As the workbench starts.
    WindowFrame::prepareNativeWindows();
    // Docking chrome is fixed before the first dock exists.
    configureDocking();
  }

  void typeSourceColorsJavaScriptsComments() {
    // JSDoc spans lines and sits before code; a private field is no
    // directive; the offsets a comment gives stand out from its words.
    QTextDocument document;
    document.setPlainText(QStringLiteral(
        "/**\n"
        " * 0x18 int32_t Tail\n"
        " */\n"
        "export class _X {\n"
        "  #view;\n"
        "  /** @returns {number} */ get Tail() { return 0x18; } // 0x18\n"));
    TypeSourceHighlighter highlighter(&document);
    highlighter.setLanguage(QStringLiteral("javascript"));
    const Theme &theme = Theme::instance();
    // The color the highlighter gives the character at \p column of line
    // \p line, or none.
    const auto colorAt = [&](int line, int column) {
      const QTextBlock block = document.findBlockByNumber(line);
      for (const QTextLayout::FormatRange &range : block.layout()->formats())
        if (column >= range.start && column < range.start + range.length)
          return range.format.foreground().color();
      return QColor();
    };
    const QColor comment = theme.color(ColorRole::ListingComment);
    const QColor number = theme.color(ColorRole::ListingNumber);
    const QColor keyword = theme.color(ColorRole::ListingKeyword);
    QCOMPARE(colorAt(0, 0), comment);
    QCOMPARE(colorAt(1, 1), comment);
    QCOMPARE(colorAt(1, 3), number);
    QCOMPARE(colorAt(2, 1), comment);
    QCOMPARE(colorAt(3, 0), keyword);
    QVERIFY(colorAt(4, 2) != theme.color(ColorRole::ListingDirective));
    const QString last = document.findBlockByNumber(5).text();
    QCOMPARE(colorAt(5, int(last.indexOf(QLatin1String("@returns")))), comment);
    QCOMPARE(colorAt(5, int(last.indexOf(QLatin1String("get")))), keyword);
    QCOMPARE(colorAt(5, int(last.indexOf(QLatin1String("return 0x18")) + 7)),
             number);
    QCOMPARE(colorAt(5, int(last.lastIndexOf(QLatin1String("//")))), comment);
  }

  void typeSourceColorsLuasComments() {
    // Lua comments with --, after code too.
    QTextDocument document;
    document.setPlainText(
        QStringLiteral("-- 0x08 uint32_t Flags: integer\n"
                       "local T = record(\"_X\", 0x10) -- 0x10\n"));
    TypeSourceHighlighter highlighter(&document);
    highlighter.setLanguage(QStringLiteral("lua"));
    const Theme &theme = Theme::instance();
    const auto colorAt = [&](int line, int column) {
      const QTextBlock block = document.findBlockByNumber(line);
      for (const QTextLayout::FormatRange &range : block.layout()->formats())
        if (column >= range.start && column < range.start + range.length)
          return range.format.foreground().color();
      return QColor();
    };
    QCOMPARE(colorAt(0, 0), theme.color(ColorRole::ListingComment));
    QCOMPARE(colorAt(0, 4), theme.color(ColorRole::ListingNumber));
    QCOMPARE(colorAt(1, 0), theme.color(ColorRole::ListingKeyword));
    const QString code = document.findBlockByNumber(1).text();
    QCOMPARE(colorAt(1, int(code.indexOf(QLatin1String("--")))),
             theme.color(ColorRole::ListingComment));
  }

  void flowLayoutWrapsRatherThanOverlaps() {
    // A combo box that keeps its width and the checkboxes after it, as the
    // Local Types options are: narrow, they wrap to rows of their own, each
    // at its own size and none over another; wide, they share one row.
    QWidget row;
    auto *flow = new FlowLayout(&row);
    auto *combo = new QComboBox(&row);
    combo->addItem(QStringLiteral("C keywords (unsigned long)"));
    combo->setMinimumWidth(combo->sizeHint().width());
    flow->addWidget(combo);
    QList<QWidget *> widgets{combo};
    for (const char *text :
         {"Hex offsets", "With dependencies", "Layout checks"})
      widgets.append(new QCheckBox(QString::fromLatin1(text), &row));
    for (QWidget *widget : widgets.mid(1))
      flow->addWidget(widget);
    const auto layOut = [&](int width) {
      row.resize(width, flow->heightForWidth(width));
      flow->setGeometry(QRect(0, 0, width, flow->heightForWidth(width)));
    };
    const int narrow = combo->sizeHint().width() + 20;
    layOut(narrow);
    for (int first = 0; first < widgets.size(); ++first) {
      const QRect at = widgets[first]->geometry();
      QCOMPARE(at.size(), widgets[first]->sizeHint());
      for (int second = first + 1; second < widgets.size(); ++second)
        QVERIFY2(!at.intersects(widgets[second]->geometry()),
                 qPrintable(widgets[first]->objectName()));
    }
    QVERIFY(widgets[1]->geometry().top() > combo->geometry().top());
    const int wrapped = flow->heightForWidth(narrow);
    int wide = 0;
    for (QWidget *widget : widgets)
      wide += widget->sizeHint().width() + 20;
    layOut(wide);
    for (QWidget *widget : widgets)
      QCOMPARE(widget->geometry().center().y(), combo->geometry().center().y());
    QVERIFY(flow->heightForWidth(wide) < wrapped);
    // A hidden widget takes no place: the one after it moves over by its
    // width.
    const int before = widgets[3]->geometry().left();
    widgets[2]->hide();
    layOut(wide);
    QCOMPARE(widgets[3]->geometry().left(),
             before - widgets[2]->sizeHint().width() -
                 (before - widgets[2]->geometry().right() - 1));
  }

  void addressSpellings() {
    QCOMPARE(hexAddress(0xffff800012340000ULL),
             QStringLiteral("0xffff800012340000"));
    QCOMPARE(displayAddress(0x401000, 16), QStringLiteral("0000000000401000"));
    QCOMPARE(parseAddress(QStringLiteral("0x401000")),
             std::optional<Address>(0x401000));
    QCOMPARE(parseAddress(QStringLiteral("401000h")),
             std::optional<Address>(0x401000));
    QCOMPARE(parseAddress(QStringLiteral("FFFFFFFFFFFFFFFF")),
             std::optional<Address>(~Address(0)));
    QVERIFY(!parseAddress(QStringLiteral("10000000000000000")));
    QVERIFY(!parseAddress(QStringLiteral("main")));
    QVERIFY(!addressValue(QJsonValue(4096)));
    QCOMPARE(addressValue(QJsonValue(QStringLiteral("0x10"))),
             std::optional<Address>(16));
  }

  void expressions_data() {
    QTest::addColumn<QString>("text");
    QTest::addColumn<qulonglong>("value");
    QTest::newRow("bare hex") << "401000" << 0x401000ULL;
    QTest::newRow("0x") << "0x10" << 0x10ULL;
    QTest::newRow("h suffix") << "10h" << 0x10ULL;
    QTest::newRow("decimal #") << "#10" << 10ULL;
    QTest::newRow("decimal dot") << "10." << 10ULL;
    QTest::newRow("precedence") << "1+2*3" << 7ULL;
    QTest::newRow("parentheses") << "(1+2)*3" << 9ULL;
    QTest::newRow("shift") << "1<<4" << 0x10ULL;
    QTest::newRow("bitwise") << "0xf0 & 0x3c | 1" << 0x31ULL;
    QTest::newRow("unary") << "-1" << ~0ULL;
    QTest::newRow("complement") << "~0" << ~0ULL;
    QTest::newRow("wrapping") << "0xffffffffffffffff+2" << 1ULL;
  }
  void expressions() {
    QFETCH(QString, text);
    QFETCH(qulonglong, value);
    Expression expression(text);
    QVERIFY(expression.identifiers().isEmpty());
    const auto result = expression.evaluate();
    QVERIFY2(result, qPrintable(expression.error()));
    QCOMPARE(qulonglong(*result), value);
  }

  void expressionNamesAndErrors() {
    Expression named(QStringLiteral("main+0x10"));
    QCOMPARE(named.identifiers(), QStringList{QStringLiteral("main")});
    QVERIFY(!named.evaluate());
    QVERIFY(!named.error().isEmpty());
    named.bind(QStringLiteral("main"), 0x401000);
    QCOMPARE(named.evaluate(), std::optional<Address>(0x401010));
    for (const auto &bad : {QStringLiteral("1/0"), QStringLiteral("(1"),
                            QStringLiteral("1+"), QStringLiteral("")}) {
      Expression expression(bad);
      QVERIFY2(!expression.evaluate(), qPrintable(bad));
      QVERIFY(!expression.error().isEmpty());
    }
  }

  void addressSpaceMapsAcrossGaps() {
    AddressSpace space;
    space.reset(QJsonArray{
        QJsonObject{{"name", ".text"}, {"start", "0x1000"}, {"end", "0x2000"}},
        QJsonObject{{"name", ".data"},
                    {"start", "0x8000"},
                    {"end", "0x8100"},
                    {"file_offset", "0x2000"}}});
    QCOMPARE(space.total(), Address(0x1100));
    QCOMPARE(space.first(), Address(0x1000));
    QCOMPARE(space.last(), Address(0x80ff));
    QCOMPARE(space.linearOf(0x1000), Address(0));
    QCOMPARE(space.linearOf(0x8010), Address(0x1010));
    // Unmapped addresses take the next mapped position.
    QCOMPARE(space.linearOf(0x3000), Address(0x1000));
    QCOMPARE(space.addressAt(0x1010), Address(0x8010));
    QCOMPARE(space.regionOf(0x8050)->name, QStringLiteral(".data"));
    QVERIFY(!space.regionOf(0x3000));
    QCOMPARE(space.fileOffsetOf(0x8010), std::optional<Address>(0x2010));
    QVERIFY(!space.fileOffsetOf(0x1000));
  }

  void workerSpansUseUtf8Offsets() {
    StyledLine line;
    const QString text = QString::fromUtf8("call 中文_😀 ; x");
    // Bytes: "call " 5, "中文" 6, "_" 1, "😀" 4 -> the name spans 11 bytes.
    line.setFromWorker(text, QJsonArray{QJsonArray{5, 11, 3, "0x401000"},
                                        QJsonArray{17, 3, 4}});
    QCOMPARE(line.spans.size(), 2);
    QCOMPARE(line.spans[0].start, 5);
    QCOMPARE(text.mid(line.spans[0].start, line.spans[0].length),
             QString::fromUtf8("中文_😀"));
    QCOMPARE(line.spans[0].address, std::optional<Address>(0x401000));
    QCOMPARE(text.mid(line.spans[1].start, line.spans[1].length),
             QStringLiteral("; x"));
    QCOMPARE(line.tokenAt(1), QStringLiteral("call"));
    QCOMPARE(tokenOccurrences(QStringLiteral("rax, [rax+8] ; raxx"),
                              QStringLiteral("rax")),
             (QVector<int>{0, 6}));
    // Copies share text and spans but not the cached layout.
    StyledLine copy = line;
    QCOMPARE(copy.text, line.text);
    QCOMPARE(copy.spans.size(), line.spans.size());
  }

  void unicodeCommentsKeepTheCodePrefixOnItsGrid() {
    // The code views' font: Consolas where it exists, else the system's
    // fixed font.
    QFont font = Theme::defaultCodeFont();
    font.setPointSize(10);
    const auto color = [](int role) {
      return QColor::fromHsv((role * 30) % 360, 160, 230);
    };
    const QString prefix =
        QStringLiteral("    return (uint32_t)input_value * 3 + 7; ");
    StyledLine plain;
    plain.text = prefix;
    StyledLine annotated;
    annotated.text =
        prefix + QString::fromUtf8("/* 本行注释 second * / line */");
    plain.spans = {{4, 6, 2, {}},   {11, 1, 10, {}}, {12, 8, 3, {}},
                   {20, 1, 10, {}}, {21, 11, 8, {}}, {33, 1, 10, {}},
                   {35, 1, 4, {}},  {37, 1, 10, {}}, {39, 1, 4, {}},
                   {40, 1, 10, {}}};
    annotated.spans = plain.spans;
    annotated.spans.append({int(prefix.size()),
                            int(annotated.text.size() - prefix.size()),
                            6,
                            {}});
    const auto baseline = plain.layout(font, 1, color).lineAt(0);
    const auto decorated = annotated.layout(font, 1, color).lineAt(0);
    QCOMPARE(baseline.position().y() + baseline.ascent(),
             decorated.position().y() + decorated.ascent());
    for (int at = 0; at <= prefix.size(); ++at)
      QVERIFY2(std::abs(baseline.cursorToX(at) - decorated.cursorToX(at)) < 0.1,
               qPrintable(QStringLiteral("Code column %1 moved: %2 -> %3")
                              .arg(at)
                              .arg(baseline.cursorToX(at))
                              .arg(decorated.cursorToX(at))));
    const auto render = [&](StyledLine &line) {
      QImage image(800, 40, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::transparent);
      QPainter painter(&image);
      line.layout(font, 1, color).draw(&painter, QPointF());
      return image;
    };
    const int width = int(baseline.cursorToX(int(prefix.size())));
    QCOMPARE(render(annotated).copy(0, 0, width, 40),
             render(plain).copy(0, 0, width, 40));
  }

  void commandsHaveUniqueIdsAndUnambiguousShortcuts() {
    QMainWindow window;
    ActionRegistry registry;
    registry.buildMenus(window.menuBar(), [](QMenu *, const QString &) {});
    registry.buildToolbars(&window);
    QSet<QString> ids;
    QHash<QString, QString> globalKeys, viewKeys;
    const auto &views = registry.viewActions();
    for (auto *action : registry.allActions()) {
      QVERIFY2(!ids.contains(action->objectName()),
               qPrintable(action->objectName()));
      ids.insert(action->objectName());
      QVERIFY2(!action->text().isEmpty(), qPrintable(action->objectName()));
      QVERIFY2(!action->icon().isNull(), qPrintable(action->objectName()));
      const auto key = action->shortcut().toString(QKeySequence::PortableText);
      if (key.isEmpty())
        continue;
      auto &keys = views.contains(action) ? viewKeys : globalKeys;
      QVERIFY2(!keys.contains(key),
               qPrintable(key + QStringLiteral(": ") + keys.value(key) +
                          QStringLiteral(" and ") + action->objectName()));
      keys.insert(key, action->objectName());
      // A view shortcut must not also be a window-wide one.
      if (views.contains(action))
        QVERIFY2(!globalKeys.contains(key), qPrintable(key));
    }
    QCOMPARE(ids.size(), int(ActionId::Count));
    // Every command is reachable from a menu.
    QSet<QAction *> reachable;
    std::function<void(QMenu *)> walk = [&](QMenu *menu) {
      for (auto *action : menu->actions()) {
        reachable.insert(action);
        if (action->menu())
          walk(action->menu());
      }
    };
    for (auto *action : window.menuBar()->actions())
      if (action->menu())
        walk(action->menu());
    for (auto *action : registry.allActions())
      QVERIFY2(reachable.contains(action), qPrintable(action->objectName()));
  }

  void commandsKeepIdasShortcuts() {
    ActionRegistry registry;
    QSet<QAction *> shared;
    // Every key IDA gives a command, so that a command IDA does not have
    // takes none of them.
    QHash<QKeySequence, QString> idaCommands;
#define NEVERD_IDA_ACTION(IdaAction, Shortcut, NeverDAction)                   \
  {                                                                            \
    auto *action = registry.action(ActionId::NeverDAction);                    \
    const auto keys = idaKeys(QLatin1StringView(Shortcut));                    \
    QVERIFY2(keys.isEmpty() ? action->shortcut().isEmpty()                     \
                            : keys.contains(action->shortcut()),               \
             qPrintable(QStringLiteral("%1 has \"%2\", IDA's %3 \"%4\"")       \
                            .arg(action->objectName(),                         \
                                 action->shortcut().toString(                  \
                                     QKeySequence::PortableText),              \
                                 QLatin1StringView(IdaAction),                 \
                                 QLatin1StringView(Shortcut))));               \
    shared.insert(action);                                                     \
    for (const auto &key : keys)                                               \
      idaCommands.insert(key, QStringLiteral(IdaAction));                      \
  }
#define NEVERD_IDA_PLANNED(IdaAction, Shortcut, Group)                         \
  for (const auto &key : idaKeys(QLatin1StringView(Shortcut)))                 \
    idaCommands.insert(key, QStringLiteral(IdaAction));
#include "IdaActions.def"
    QVERIFY(shared.size() > int(ActionId::Count) * 3 / 4);
    for (auto *action : registry.allActions())
      if (!shared.contains(action) && !action->shortcut().isEmpty())
        QVERIFY2(!idaCommands.contains(action->shortcut()),
                 qPrintable(action->objectName() +
                            QStringLiteral(" takes the key of IDA's ") +
                            idaCommands.value(action->shortcut())));
    // The spellings the table uses mean the keys Qt reads.
    QCOMPARE(idaKeys(QLatin1StringView("Ctrl-Shift-Down")),
             QList{QKeySequence(QStringLiteral("Ctrl+Shift+Down"))});
    QCOMPARE(idaKeys(QLatin1StringView("Enter")),
             QList{QKeySequence(Qt::Key_Return)});
    QCOMPARE(idaKeys(QLatin1StringView("sys(ZoomOut)")),
             QKeySequence::keyBindings(QKeySequence::ZoomOut));
  }

  void everyIconRenders() {
    QDirIterator icons(QStringLiteral(":/neverd/icons"),
                       {QStringLiteral("*.svg")});
    int count = 0;
    while (icons.hasNext()) {
      const auto name = QFileInfo(icons.next()).completeBaseName();
      const auto rendered = icon(name).pixmap(QSize(16, 16));
      QVERIFY2(!rendered.isNull(), qPrintable(name));
      ++count;
    }
    QVERIFY(count >= 60);
    QVERIFY(icon(QStringLiteral("no-such-icon")).isNull());
  }

  void iconsAreRecordedOriginalArtwork() {
    QSet<QString> palette;
    QHash<QString, QString> groups;
#define NEVERD_ICON_COLOR(Hex, Use) palette.insert(QStringLiteral(Hex));
#define NEVERD_ICON(Name, Group, Shows)                                        \
  groups.insert(QStringLiteral(Name), QStringLiteral(#Group));
#include "IconSet.def"
    // Drawn by hand on the grid: no raster image, text, external reference,
    // script or editor metadata, and colors only from the record.
    static const QRegularExpression Foreign(
        QStringLiteral("<image|<text|<foreignObject|<metadata|<script|href=|"
                       "url\\(|@import|style=|rgb\\(|inkscape:|sodipodi:"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression Color(QStringLiteral("#[0-9A-Fa-f]{3,8}"));
    static const QRegularExpression Paint(
        QStringLiteral("(?:fill|stroke|stop-color)=\"([^\"]*)\""));
    QSet<QString> files;
    QDirIterator icons(QStringLiteral(":/neverd/icons"),
                       {QStringLiteral("*.svg")});
    while (icons.hasNext()) {
      const QFileInfo info(icons.next());
      const auto name = info.completeBaseName();
      files.insert(name);
      QVERIFY2(groups.contains(name),
               qPrintable(name + QStringLiteral(" is not in IconSet.def")));
      if (groups.value(name) == QLatin1String("Brand"))
        continue;
      QFile file(info.filePath());
      QVERIFY(file.open(QIODevice::ReadOnly));
      const auto svg = QString::fromUtf8(file.readAll());
      // Control chrome takes the size of the part it draws, and line glyphs
      // the 24 px grid they are drawn on.
      if (groups.value(name) == QLatin1String("Glyph"))
        QVERIFY2(svg.contains(QStringLiteral("viewBox=\"0 0 24 24\"")),
                 qPrintable(name));
      else if (groups.value(name) != QLatin1String("Chrome"))
        QVERIFY2(svg.contains(QStringLiteral("viewBox=\"0 0 16 16\"")),
                 qPrintable(name));
      const auto foreign = Foreign.match(svg);
      QVERIFY2(!foreign.hasMatch(),
               qPrintable(name + QStringLiteral(": ") + foreign.captured()));
      for (auto paints = Paint.globalMatch(svg); paints.hasNext();) {
        const auto paint = paints.next().captured(1);
        QVERIFY2(paint == QLatin1String("none") ||
                     (Color.match(paint).captured() == paint &&
                      palette.contains(paint.toUpper())),
                 qPrintable(name + QStringLiteral(" paints ") + paint));
      }
    }
    for (auto it = groups.cbegin(); it != groups.cend(); ++it)
      QVERIFY2(files.contains(it.key()),
               qPrintable(it.key() + QStringLiteral(" is recorded but has no "
                                                    "file")));
  }

  void projectDatabaseRoundTrip() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    // More than one compression chunk, with incompressible content.
    QByteArray input(9 * 1024 * 1024 + 123, Qt::Uninitialized);
    QRandomGenerator generator(3389);
    for (auto &byte : input)
      byte = char(generator.bounded(256));
    const auto binary = directory.filePath(QStringLiteral("sample.bin"));
    const auto write = [](const QString &path, const QByteArray &bytes) {
      QDir().mkpath(QFileInfo(path).absolutePath());
      QFile file(path);
      return file.open(QIODevice::WriteOnly) &&
             file.write(bytes) == bytes.size();
    };
    QVERIFY(write(binary, input));
    QVERIFY(write(binary + ".neverd/annotations.json",
                  R"([{"addr":"0x10","text":"one"}])"));
    QVERIFY(write(binary + ".neverd/renames.json", "[]"));
    // Every table the worker keeps, and how a binary file is read.
    QVERIFY(write(binary + ".neverd/items.json",
                  R"([{"addr":"0x20","kind":"data","size":4}])"));
    QVERIFY(write(binary + ".neverd/load.json",
                  R"({"loader":"binary","processor":"x86_64"})"));
    const auto database = ProjectDatabase::pathFor(binary);
    QCOMPARE(database, binary + QStringLiteral(".nddb"));
    QVERIFY(ProjectDatabase::isDatabase(database));
    QCOMPARE(ProjectDatabase::save(
                 database, binary,
                 {{QStringLiteral("location"), R"({"address":"0x10"})"}}),
             QString());
    QVERIFY(QFileInfo::exists(database));
    QVERIFY(!QFileInfo::exists(database + QStringLiteral(".saving")));
    QString error;
    auto contents = ProjectDatabase::read(database, &error);
    QVERIFY2(contents, qPrintable(error));
    QCOMPARE(contents->inputName, QStringLiteral("sample.bin"));
    QCOMPARE(contents->inputSize, input.size());
    QCOMPARE(contents->inputSha256,
             QString::fromLatin1(
                 QCryptographicHash::hash(input, QCryptographicHash::Sha256)
                     .toHex()));
    QCOMPARE(contents->sidecars.size(), 4);
    QCOMPARE(contents->state.value(QStringLiteral("location")),
             QByteArray(R"({"address":"0x10"})"));

    // A second save updates sidecars and state in place.
    QVERIFY(write(binary + ".neverd/annotations.json",
                  R"([{"addr":"0x10","text":"two"}])"));
    QCOMPARE(ProjectDatabase::saveState(database,
                                        {{QStringLiteral("bookmarks"), "[]"}}),
             QString());
    QCOMPARE(ProjectDatabase::save(database, binary, {}), QString());
    contents = ProjectDatabase::read(database, &error);
    QVERIFY(contents->sidecars.value(QStringLiteral(".neverd-annotations.json"))
                .contains("two"));
    QCOMPARE(contents->state.value(QStringLiteral("bookmarks")),
             QByteArray("[]"));

    // Unpacking elsewhere restores the input and its sidecars exactly.
    QTemporaryDir elsewhere;
    const auto unpacked =
        ProjectDatabase::unpack(database, elsewhere.path(), &error);
    QVERIFY2(!unpacked.isEmpty(), qPrintable(error));
    QFile copy(unpacked);
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), input);
    QFile annotations(unpacked + ".neverd/annotations.json");
    QVERIFY(annotations.open(QIODevice::ReadOnly));
    QVERIFY(annotations.readAll().contains("two"));
    QFile items(unpacked + ".neverd/items.json");
    QVERIFY(items.open(QIODevice::ReadOnly));
    QVERIFY(items.readAll().contains("0x20"));
    QFile load(unpacked + ".neverd/load.json");
    QVERIFY(load.open(QIODevice::ReadOnly));
    QVERIFY(load.readAll().contains("x86_64"));
    QVERIFY(!QFileInfo::exists(unpacked + ".neverd/local/history.json"));

    // Damage is reported, never unpacked as a different input.
    {
      auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("damage"));
      db.setDatabaseName(database);
      QVERIFY(db.open());
      QSqlQuery query(db);
      QVERIFY(query.exec(QStringLiteral(
          "UPDATE blobs SET data = X'00000001ff' WHERE chunk = 1")));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("damage"));
    QTemporaryDir damaged;
    QVERIFY(
        ProjectDatabase::unpack(database, damaged.path(), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(damaged.filePath(QStringLiteral("sample.bin"))));

    // A database cannot name an input outside its directory.
    {
      auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("escape"));
      db.setDatabaseName(database);
      QVERIFY(db.open());
      QSqlQuery query(db);
      QVERIFY(query.exec(QStringLiteral(
          "UPDATE meta SET value = '../escape.bin' WHERE key = 'input_name'")));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("escape"));
    QVERIFY(!ProjectDatabase::read(database, &error));
    QVERIFY(!ProjectDatabase::read(
        directory.filePath(QStringLiteral("missing.nddb")), &error));
  }

  void projectFilesPackWithTheProject() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto binary = directory.filePath(QStringLiteral("sample.bin"));
    const auto write = [](const QString &path, const QByteArray &bytes) {
      QDir().mkpath(QFileInfo(path).absolutePath());
      QFile file(path);
      return file.open(QIODevice::WriteOnly) &&
             file.write(bytes) == bytes.size();
    };
    const auto read = [](const QString &path) {
      QFile file(path);
      return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    QVERIFY(write(binary, "\x90\xc3"));
    // The project's own files read like the worker's: two-space indents,
    // keys sorted, one final line break.
    // Bookmarks as the command line's bookmarks command keeps them: by
    // address, one at each, addresses in lowercase.
    const QJsonArray marks = ProjectDatabase::bookmarkFile(QJsonArray{
        QJsonObject{{"address", "0x1A"}, {"text", "later"}},
        QJsonObject{{"address", "0x9"}, {"text", QString::fromUtf8("标记")}},
        QJsonObject{{"address", "0x1a"}, {"text", "last"}, {"note", "kept"}}});
    QCOMPARE(ProjectDatabase::writeProjectFile(binary,
                                               ProjectDatabase::BookmarksSuffix,
                                               QJsonDocument(marks)),
             QString());
    QCOMPARE(read(binary + ".neverd/bookmarks.json"),
             QByteArray("[\n  {\n    \"addr\": \"0x9\",\n    \"name\": \"") +
                 QString::fromUtf8("标记").toUtf8() +
                 "\",\n    \"note\": \"\"\n  },\n  {\n    \"addr\": \"0x1a\",\n"
                 "    \"name\": \"last\",\n    \"note\": \"kept\"\n  }\n]\n");
    const auto rows = ProjectDatabase::bookmarkRows(
        ProjectDatabase::readProjectFile(binary,
                                         ProjectDatabase::BookmarksSuffix)
            ->array());
    QCOMPARE(rows.size(), 2);
    QCOMPARE(
        rows.at(1).toObject(),
        (QJsonObject{{"address", "0x1a"}, {"text", "last"}, {"note", "kept"}}));
    // No bookmarks read as the command line writes none, and an empty
    // member closes where it opens.
    const auto empty = directory.filePath(QStringLiteral("empty.bin"));
    QCOMPARE(ProjectDatabase::writeProjectFile(empty,
                                               ProjectDatabase::BookmarksSuffix,
                                               QJsonDocument(QJsonArray())),
             QString());
    QCOMPARE(read(empty + ".neverd/bookmarks.json"), QByteArray("[]\n"));
    QCOMPARE(ProjectDatabase::writeProjectFile(
                 empty, ProjectDatabase::DebugFilesSuffix,
                 QJsonDocument(QJsonObject{{"pdb", QJsonObject()},
                                           {"paths", QJsonArray()}})),
             QString());
    QCOMPARE(read(empty + ".neverd/debug.json"),
             QByteArray("{\n  \"paths\": [],\n  \"pdb\": {}\n}\n"));
    QVERIFY(!ProjectDatabase::readProjectFile(
        binary, ProjectDatabase::WorkbenchSuffix));
    QVERIFY(write(binary + ".neverd/debug.json", R"({"pdb":{"mode":"none"}})"));
    QVERIFY(write(binary + ".neverd/types.json", R"({"types":["struct T"]})"));
    QVERIFY(write(binary + ".neverd/items.json", "[]"));
    // A workbench file is the user's own; a database keeps its own state.
    QVERIFY(write(binary + ".neverd/local/workbench.json", "{}"));

    // Local types, bookmarks and debug files go into the database and come
    // back out of it.
    const auto database = ProjectDatabase::pathFor(binary);
    QCOMPARE(ProjectDatabase::save(database, binary, {}), QString());
    QString error;
    auto contents = ProjectDatabase::read(database, &error);
    QVERIFY2(contents, qPrintable(error));
    for (const char *suffix : {".neverd-types.json", ".neverd-bookmarks.json",
                               ".neverd-debug.json", ".neverd-items.json"})
      QVERIFY2(contents->sidecars.contains(QLatin1String(suffix)), suffix);
    QVERIFY(
        !contents->sidecars.contains(QStringLiteral(".neverd-workbench.json")));
    QVERIFY(contents->packedSuffixes.contains(
        QStringLiteral(".neverd-types.json")));
    QTemporaryDir elsewhere;
    const auto unpacked =
        ProjectDatabase::unpack(database, elsewhere.path(), &error);
    QVERIFY2(!unpacked.isEmpty(), qPrintable(error));
    QCOMPARE(read(unpacked + ".neverd/types.json"),
             read(binary + ".neverd/types.json"));
    QCOMPARE(read(unpacked + ".neverd/bookmarks.json"),
             read(binary + ".neverd/bookmarks.json"));
    QCOMPARE(read(unpacked + ".neverd/debug.json"),
             read(binary + ".neverd/debug.json"));

    // A database written before it named the files it packs packed no local
    // types: unpacking it keeps the types beside the input, while a table it
    // did pack and now lacks is removed.
    {
      auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                          QStringLiteral("legacy"));
      db.setDatabaseName(database);
      QVERIFY(db.open());
      QSqlQuery query(db);
      QVERIFY(query.exec(
          QStringLiteral("DELETE FROM meta WHERE key = 'sidecar_suffixes'")));
      QVERIFY(query.exec(QStringLiteral(
          "DELETE FROM sidecars WHERE suffix IN ('.neverd-types.json', "
          "'.neverd-items.json', '.neverd-bookmarks.json', "
          "'.neverd-debug.json')")));
      db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("legacy"));
    contents = ProjectDatabase::read(database, &error);
    QVERIFY2(contents, qPrintable(error));
    QVERIFY(!contents->packedSuffixes.contains(
        QStringLiteral(".neverd-types.json")));
    QVERIFY(write(unpacked + ".neverd/types.json", "only copy"));
    QVERIFY(write(unpacked + ".neverd/items.json", "stale"));
    QCOMPARE(ProjectDatabase::unpack(database, elsewhere.path(), &error),
             unpacked);
    QCOMPARE(read(unpacked + ".neverd/types.json"), QByteArray("only copy"));
    QVERIFY(!QFileInfo::exists(unpacked + ".neverd/items.json"));
    QVERIFY(QFileInfo::exists(unpacked + ".neverd/bookmarks.json"));
  }

  void projectDatabaseIdentity() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto write = [](const QString &path, const QByteArray &bytes) {
      QDir().mkpath(QFileInfo(path).absolutePath());
      QFile file(path);
      return file.open(QIODevice::WriteOnly) &&
             file.write(bytes) == bytes.size();
    };
    const auto contents = [](const QString &path) {
      QFile file(path);
      return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const auto applicationId = [&](const QString &path) {
      return contents(path).mid(68, 4);
    };
    // Statements run on a SQLite file the way another application would.
    const auto sql = [](const QString &path, const QStringList &statements) {
      bool ok = false;
      {
        auto db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                            QStringLiteral("identity"));
        db.setDatabaseName(path);
        ok = db.open();
        for (const auto &statement : statements)
          ok = ok && QSqlQuery(db).exec(statement);
        db.close();
      }
      QSqlDatabase::removeDatabase(QStringLiteral("identity"));
      return ok;
    };
    // GeoPackage's application id, "GPKG".
    const auto otherId =
        QStringLiteral("PRAGMA application_id = %1").arg(0x47504B47);
    const auto binary = directory.filePath(QStringLiteral("sample.bin"));
    QVERIFY(write(binary, QByteArray(4096, '\x90')));
    const auto database = ProjectDatabase::pathFor(binary);
    QCOMPARE(ProjectDatabase::save(database, binary, {}), QString());
    QCOMPARE(applicationId(database), QByteArray("NDDB"));

    // The header, not the name, makes a database.
    const auto renamed = directory.filePath(QStringLiteral("project.sqlite"));
    QVERIFY(QFile::copy(database, renamed));
    QVERIFY(ProjectDatabase::isDatabase(renamed));
    QVERIFY(!ProjectDatabase::isDatabase(binary));
    QString error;
    QVERIFY2(ProjectDatabase::read(renamed, &error), qPrintable(error));
    const auto other = directory.filePath(QStringLiteral("other.sqlite"));
    QVERIFY(sql(other, {otherId, QStringLiteral("CREATE TABLE t(x)")}));
    QVERIFY(!ProjectDatabase::isDatabase(other));

    // A database written before the id was stamped still reads, and its next
    // write stamps the id.
    QVERIFY(sql(database, {QStringLiteral("PRAGMA application_id = 0")}));
    QCOMPARE(applicationId(database), QByteArray(4, '\0'));
    QVERIFY2(ProjectDatabase::read(database, &error), qPrintable(error));
    QCOMPARE(ProjectDatabase::saveState(database, {}), QString());
    QCOMPARE(applicationId(database), QByteArray("NDDB"));

    // Another application's SQLite file is neither read nor written, even
    // with a NeverD format row and the .nddb suffix.
    const auto foreign = directory.filePath(QStringLiteral("foreign.nddb"));
    QVERIFY(sql(foreign, {otherId,
                          QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY "
                                         "KEY, value TEXT NOT NULL)"),
                          QStringLiteral("INSERT INTO meta VALUES('format', "
                                         "'1')")}));
    const auto foreignBytes = contents(foreign);
    QVERIFY(!ProjectDatabase::read(foreign, &error));
    QCOMPARE(error,
             QStringLiteral("%1 is not a NeverD database.").arg(foreign));
    QVERIFY(!ProjectDatabase::saveState(foreign, {}).isEmpty());
    QVERIFY(!ProjectDatabase::save(foreign, binary, {}).isEmpty());
    QCOMPARE(contents(foreign), foreignBytes);

    // Nor is a SQLite file without an id that NeverD did not write.
    const auto plain = directory.filePath(QStringLiteral("plain.nddb"));
    QVERIFY(sql(plain, {QStringLiteral("CREATE TABLE t(x)")}));
    const auto plainBytes = contents(plain);
    QCOMPARE(ProjectDatabase::save(plain, binary, {}),
             QStringLiteral("%1 is not a NeverD database.").arg(plain));
    QCOMPARE(contents(plain), plainBytes);
  }

  void loadDialogOffersWhatTheEngineLoads() {
    // An ELF file NeverD loads, and the binary file it cannot load yet.
    const QJsonArray rows{
        QJsonObject{{"loader", "elf"},
                    {"text", "ELF64 for x86-64 (Shared object)"},
                    {"processor", "x86_64"},
                    {"loadable", true}},
        QJsonObject{{"loader", "binary"},
                    {"text", "Binary file"},
                    {"processor", ""},
                    {"loadable", false},
                    {"reason", "Loading a binary file is not supported yet"}}};
    LoadFileDialog dialog(QStringLiteral("/tmp/xxd"), rows);
    auto *loaders = dialog.findChild<QListWidget *>(QStringLiteral("loaders"));
    QVERIFY(loaders);
    QCOMPARE(loaders->count(), 2);
    QCOMPARE(loaders->item(0)->text(),
             QStringLiteral("ELF64 for x86-64 (Shared object) [elf]"));
    QCOMPARE(loaders->item(1)->text(), QStringLiteral("Binary file"));
    // The first loadable row is chosen; a row NeverD cannot load says why.
    QCOMPARE(dialog.row(), 0);
    QVERIFY(!(loaders->item(1)->flags() & Qt::ItemIsEnabled));
    QCOMPARE(loaders->item(1)->toolTip(),
             QStringLiteral("Loading a binary file is not supported yet"));
    // The header's processor shows, and cannot be changed.
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    QVERIFY(processors && !processors->isEnabled());
    QVERIFY(processors->currentItem());
    QCOMPARE(processors->currentItem()->data(0, Qt::UserRole).toString(),
             QStringLiteral("x86_64"));
    // Loading reads debug information and analyzes in idle time by default.
    QVERIFY(dialog.options().debugInfo && dialog.options().analysis);
    dialog.findChild<QCheckBox *>(QStringLiteral("analysis"))
        ->setChecked(false);
    QVERIFY(!dialog.options().analysis);
    dialog.setIndicator(false);
    QVERIFY(!dialog.indicator());
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());

    // A file no row can load offers nothing to accept, and says why.
    const QJsonArray mips{
        QJsonObject{{"loader", "elf"},
                    {"text", "ELF64 for MIPS (Executable)"},
                    {"processor", ""},
                    {"loadable", false},
                    {"reason", "NeverD has no MIPS processor"}},
        rows[1]};
    // A row a loader took for the file's name alone is never the default.
    const QJsonArray named{QJsonObject{{"loader", "evm"},
                                       {"text", "EVM bytecode"},
                                       {"processor", "evm"},
                                       {"loadable", true},
                                       {"by_name", true}},
                           rows[1]};
    LoadFileDialog byName(QStringLiteral("/tmp/firmware.bin"), named);
    QCOMPARE(byName.row(), -1);
    auto *accept = byName.findChild<QPushButton *>(QStringLiteral("ok"));
    QVERIFY(!accept->isEnabled());
    byName.findChild<QListWidget *>(QStringLiteral("loaders"))
        ->setCurrentRow(0);
    QCOMPARE(byName.row(), 0);
    QVERIFY(accept->isEnabled());

    LoadFileDialog refused(QStringLiteral("/tmp/mips"), mips);
    QCOMPARE(refused.row(), -1);
    QVERIFY(
        !refused.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(refused.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("NeverD has no MIPS processor")));
  }

  void loadDialogReadsABinaryFileAsTheUserSays() {
    // Data no header names: a binary file, read as the processor and at the
    // address the user picks.
    const QJsonArray rows{QJsonObject{{"loader", "binary"},
                                      {"text", "Binary file"},
                                      {"processor", ""},
                                      {"loadable", true}}};
    LoadFileDialog dialog(QStringLiteral("/tmp/firmware.bin"), rows);
    QCOMPARE(dialog.row(), 0);
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    auto *accept = dialog.findChild<QPushButton *>(QStringLiteral("ok"));
    QVERIFY(processors->isEnabled());
    // No processor is assumed.
    QVERIFY(!accept->isEnabled());
    QTreeWidgetItem *aarch64 = nullptr, *evm = nullptr;
    for (int family = 0; family < processors->topLevelItemCount(); ++family)
      for (int child = 0;
           child < processors->topLevelItem(family)->childCount(); ++child) {
        auto *item = processors->topLevelItem(family)->child(child);
        const auto name = item->data(0, Qt::UserRole).toString();
        if (name == QLatin1String("aarch64"))
          aarch64 = item;
        if (name == QLatin1String("evm"))
          evm = item;
      }
    QVERIFY(aarch64 && evm);
    // Bytecode machines read no binary file.
    QVERIFY(!(evm->flags() & Qt::ItemIsEnabled));
    processors->setCurrentItem(aarch64);
    QVERIFY(accept->isEnabled());
    dialog.findChild<QLineEdit *>(QStringLiteral("base"))
        ->setText(QStringLiteral("0x80000"));
    const auto options = dialog.options();
    QCOMPARE(options.processor, QStringLiteral("aarch64"));
    QCOMPARE(options.base, quint64(0x80000));
    QCOMPARE(options.offset, quint64(0));
    QVERIFY(!options.entry);
    // A field that holds no number stops the load and says so.
    dialog.findChild<QLineEdit *>(QStringLiteral("entry"))
        ->setText(QStringLiteral("start"));
    QVERIFY(!accept->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("start")));
    QCOMPARE(options.loader, QStringLiteral("binary"));
    // The platform is read from the code unless the user names one.
    auto *platform = dialog.findChild<QComboBox *>(QStringLiteral("platform"));
    QVERIFY(platform && platform->isEnabled());
    QVERIFY(options.platform.isEmpty());
    platform->setCurrentIndex(platform->findData(QStringLiteral("windows")));
    QCOMPARE(dialog.options().platform, QStringLiteral("windows"));
    // The processor picked last time is picked again.
    LoadFileDialog again(QStringLiteral("/tmp/firmware.bin"), rows);
    again.setBinaryProcessor(QStringLiteral("thumb"));
    QCOMPARE(again.options().processor, QStringLiteral("thumb"));
    QVERIFY(again.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    // A loader the file's name alone suggests is never the default, and
    // reads the file only when the user chooses it.
    const QJsonArray named{QJsonObject{{"loader", "evm"},
                                       {"text", "EVM bytecode"},
                                       {"processor", "evm"},
                                       {"loadable", true},
                                       {"by_name", true}},
                           rows[0]};
    LoadFileDialog chosen(QStringLiteral("/tmp/firmware.bin"), named);
    QCOMPARE(chosen.row(), 1);
    chosen.findChild<QListWidget *>(QStringLiteral("loaders"))
        ->setCurrentRow(0);
    QCOMPARE(chosen.options().loader, QStringLiteral("evm"));
    QVERIFY(!chosen.findChild<QComboBox *>(QStringLiteral("platform"))
                 ->isEnabled());
    QVERIFY(chosen.options().processor.isEmpty());
    QVERIFY(chosen.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
  }

  void loadDialogPreselectsTheProcessorTheBytesName() {
    const auto binaryRow = [](QJsonObject identified) {
      identified.insert("loader", "binary");
      identified.insert("text", "Binary file");
      identified.insert("processor", "");
      identified.insert("loadable", true);
      return QJsonArray{identified};
    };
    // The bytes read as ARM64 beat the processor picked for another file,
    // and keeping it lets the engine record it as read.
    LoadFileDialog dialog(
        QStringLiteral("/tmp/firmware.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "aarch64"},
                                                {"name", "ARM64 (AArch64)"},
                                                {"processor", "aarch64"},
                                                {"share", 0.97}}}},
             {"code_share", 0.6},
             {"status", "settled"},
             {"detected", "aarch64"},
             {"evidence", "ARM64 (AArch64) 97% of the code"}}));
    dialog.setBinaryProcessor(QStringLiteral("thumb"));
    QCOMPARE(dialog.options().processor, QStringLiteral("auto"));
    QVERIFY(dialog.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("ARM64")));
    // Another processor, chosen, is the user's.
    auto *processors =
        dialog.findChild<QTreeWidget *>(QStringLiteral("processors"));
    for (int family = 0; family < processors->topLevelItemCount(); ++family)
      for (int child = 0;
           child < processors->topLevelItem(family)->childCount(); ++child) {
        auto *item = processors->topLevelItem(family)->child(child);
        if (item->data(0, Qt::UserRole).toString() == QLatin1String("x86_64"))
          processors->setCurrentItem(item);
      }
    QCOMPARE(dialog.options().processor, QStringLiteral("x86_64"));

    // A set NeverD cannot decode preselects nothing and says what it is.
    LoadFileDialog foreign(
        QStringLiteral("/tmp/router.bin"),
        binaryRow({{"guesses",
                    QJsonArray{QJsonObject{{"isa", "mips"},
                                           {"name", "MIPS big-endian (32-bit)"},
                                           {"processor", ""},
                                           {"share", 0.94}}}},
                   {"code_share", 0.7},
                   {"status", "settled"},
                   {"detected", ""},
                   {"evidence", "MIPS big-endian (32-bit) 94% of the code"}}));
    QVERIFY(
        !foreign.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());
    QVERIFY(foreign.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .contains(QStringLiteral("NeverD cannot decode")));

    // A family whose instructions do not tell its width names both sets.
    LoadFileDialog wide(
        QStringLiteral("/tmp/dump.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "x86"},
                                                {"name", "Intel x86 (32-bit)"},
                                                {"processor", "x86"},
                                                {"share", 0.6}},
                                    QJsonObject{{"isa", "x86_64"},
                                                {"name", "x86-64"},
                                                {"processor", "x86_64"},
                                                {"share", 0.4}}}},
             {"status", "width_unclear"},
             {"detected", ""}}));
    const QString both =
        wide.findChild<QLabel *>(QStringLiteral("note"))->text();
    QVERIFY(both.contains(QStringLiteral("Intel x86 (32-bit)")) &&
            both.contains(QStringLiteral("x86-64")));
    QVERIFY(!wide.findChild<QPushButton *>(QStringLiteral("ok"))->isEnabled());

    // Code whose instructions start two bytes into its words reads from
    // there.
    LoadFileDialog shifted(
        QStringLiteral("/tmp/carved.bin"),
        binaryRow(
            {{"guesses", QJsonArray{QJsonObject{{"isa", "aarch64"},
                                                {"name", "ARM64 (AArch64)"},
                                                {"processor", "aarch64"},
                                                {"share", 1.0}}}},
             {"status", "settled"},
             {"code_unit", 4},
             {"code_offset", 2},
             {"detected", "aarch64"},
             {"evidence", "ARM64 (AArch64) 100% of the code"}}));
    QCOMPARE(shifted.findChild<QLineEdit *>(QStringLiteral("offset"))->text(),
             QStringLiteral("0x2"));
    QCOMPARE(shifted.options().offset, quint64(2));
    QCOMPARE(shifted.options().processor, QStringLiteral("auto"));

    // Bytes that look like no known code say so.
    LoadFileDialog data(QStringLiteral("/tmp/data.bin"),
                        binaryRow({{"guesses", QJsonArray()},
                                   {"status", "no_code"},
                                   {"detected", ""}}));
    QVERIFY(data.findChild<QLabel *>(QStringLiteral("note"))
                ->text()
                .startsWith(QStringLiteral("No part of the file")));

    // A Cortex-M vector table says where the code runs.
    LoadFileDialog firmware(
        QStringLiteral("/tmp/stm32.bin"),
        binaryRow(
            {{"guesses", QJsonArray()},
             {"code_share", 0.0},
             {"detected", "thumb"},
             {"evidence", "a Cortex-M vector table at the start"},
             {"fingerprint", QJsonObject{{"name", "Cortex-M vector table"},
                                         {"processor", "thumb"},
                                         {"entry", "0x8000040"},
                                         {"base", "0x8000000"}}}}));
    QCOMPARE(firmware.findChild<QLineEdit *>(QStringLiteral("base"))->text(),
             QStringLiteral("0x8000000"));
    QCOMPARE(firmware.findChild<QLineEdit *>(QStringLiteral("entry"))->text(),
             QStringLiteral("0x8000040"));
    QCOMPARE(firmware.options().processor, QStringLiteral("auto"));
    QCOMPARE(firmware.options().base, quint64(0x8000000));
  }

  void dockSeparatorsAreHairlines() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    DockPair pair;
    QVERIFY(QTest::qWaitForWindowExposed(&pair.window));
    // The docks' contents meet at a one-pixel gap, with no frame of their own.
    const QRect left = pair.contentRect(pair.left);
    const QRect right = pair.contentRect(pair.right);
    const int gap = left.right() + 1;
    QCOMPARE(right.left(), gap + 1);
    QCOMPARE(pair.pixel(gap), theme.chrome(QStringLiteral("DockSeparator")));
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);
    QCOMPARE(pair.pixel(gap + 1), DockPair::RightColor);
    // The separator's grab area spreads two pixels over either neighbor.
    auto *separator = pair.separator();
    QVERIFY(separator);
    QCOMPARE(separator->mapTo(&pair.window, QPoint()).x(), gap - 2);
    QCOMPARE(separator->width(), 5);

    theme.setMode(Theme::Mode::Light);
    QCOMPARE(pair.pixel(gap), theme.chrome(QStringLiteral("DockSeparator")));
    theme.setMode(Theme::Mode::Dark);
  }

  void dockSeparatorsLightUpWhileHoveredOrDragged() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    DockPair pair;
    QVERIFY(QTest::qWaitForWindowExposed(&pair.window));
    auto *separator = pair.separator();
    QVERIFY(separator);
    const int gap = pair.contentRect(pair.left).right() + 1;
    const QColor hover = theme.chrome(QStringLiteral("DockSeparatorHover"));
    const QPoint hairline(2, separator->height() / 2);

    // A pointer that only crosses the separator leaves it alone.
    QEnterEvent enter(hairline, separator->mapTo(&pair.window, hairline),
                      separator->mapToGlobal(hairline));
    QCoreApplication::sendEvent(separator, &enter);
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);
    // One that rests on it lights a bar over the hairline and its neighbors.
    QTRY_COMPARE(pair.pixel(gap - 1), hover);
    QCOMPARE(pair.pixel(gap), hover);
    QCOMPARE(pair.pixel(gap + 1), hover);
    QCOMPARE(pair.pixel(gap - 2), DockPair::LeftColor);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(separator, &leave);
    QTRY_COMPARE(pair.pixel(gap),
                 theme.chrome(QStringLiteral("DockSeparator")));
    QCOMPARE(pair.pixel(gap - 1), DockPair::LeftColor);

    // Dragging moves the gap with the pointer and keeps the bar lit.  On
    // Windows the layout asks the system whether the button is really down,
    // and a synthesized press is not.
#ifndef Q_OS_WIN
    const int width = pair.left->widget()->width();
    QTest::mousePress(separator, Qt::LeftButton, {}, hairline);
    QTest::mouseMove(separator, hairline + QPoint(40, 0));
    QTest::mouseRelease(separator, Qt::LeftButton, {}, hairline);
    QCOMPARE(pair.left->widget()->width(), width + 40);
    QTRY_COMPARE(pair.pixel(gap + 40), hover);
#endif
  }

  void floatingWindowsTakeTheThemeOutline() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    auto dock = std::unique_ptr<KDDockWidgets::QtWidgets::DockWidget>(
        DockPair::dock("floating", DockPair::LeftColor));
    dock->show();
    QWidget *window = dock->QWidget::window();
    QVERIFY(window != dock.get());
    QVERIFY(QTest::qWaitForWindowExposed(window));
    QCOMPARE(window->grab().toImage().pixelColor(0, window->height() / 2),
             theme.chrome(QStringLiteral("FloatingWindowBorder")));
  }

  void tabsShowCloseButtonsWhenCurrentOrHovered() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    DockPair pair;
    pair.right->addDockWidgetAsTab(
        DockPair::dock("tabbed", DockPair::LeftColor));
    QVERIFY(QTest::qWaitForWindowExposed(&pair.window));
    QTabBar *bar = nullptr;
    for (auto *candidate : pair.window.findChildren<QTabBar *>())
      if (candidate->count() == 2)
        bar = candidate;
    QVERIFY(bar);
    const auto side =
        static_cast<QTabBar::ButtonPosition>(bar->style()->styleHint(
            QStyle::SH_TabBar_CloseButtonPosition, nullptr, bar));
    const int current = bar->currentIndex();
    const int other = 1 - current;
    const auto closable = [&](int index) {
      return bar->tabButton(index, side)->isVisible();
    };
    QVERIFY(closable(current));
    QVERIFY(!closable(other));

    // A hovered tab shows its button in the room it kept.
    const QRect room = bar->tabRect(other);
    const QPointF center = room.center();
    QHoverEvent hover(QEvent::HoverMove, center, bar->mapToGlobal(center),
                      center);
    QCoreApplication::sendEvent(bar, &hover);
    QVERIFY(closable(other));
    QCOMPARE(bar->tabRect(other), room);
    QHoverEvent leave(QEvent::HoverLeave, QPointF(-1, -1), QPointF(-1, -1),
                      center);
    QCoreApplication::sendEvent(bar, &leave);
    QVERIFY(!closable(other));

    // The button moves with the current tab.
    bar->setCurrentIndex(other);
    QVERIFY(closable(other));
    QVERIFY(!closable(current));
  }

  void mnemonicsShowWhileAltIsHeld() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    QLabel label(QStringLiteral("&Address"));
    label.show();
    const auto underlined = [&] {
      return label.style()->styleHint(QStyle::SH_UnderlineShortcut, nullptr,
                                      &label);
    };
    QCOMPARE(underlined(), 0);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Alt, Qt::AltModifier);
    QKeyEvent release(QEvent::KeyRelease, Qt::Key_Alt, Qt::NoModifier);
    QCoreApplication::sendEvent(&label, &press);
    QCOMPARE(underlined(), 1);
    QCoreApplication::sendEvent(&label, &release);
    QCOMPARE(underlined(), 0);

    // A menu that Alt opened keeps them until it closes.
    QCoreApplication::sendEvent(&label, &press);
    QMenu menu;
    menu.addAction(QStringLiteral("&Open"));
    menu.popup(label.mapToGlobal(QPoint()));
    QVERIFY(QTest::qWaitForWindowExposed(&menu));
    QCoreApplication::sendEvent(&label, &release);
    QCOMPARE(underlined(), 1);
    menu.hide();
    QTRY_COMPARE(underlined(), 0);

    // An Alt whose release went elsewhere ends with the next plain input.
    QCoreApplication::sendEvent(&label, &press);
    QMouseEvent move(QEvent::MouseMove, QPointF(3, 3),
                     label.mapToGlobal(QPointF(3, 3)), Qt::NoButton,
                     Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(&label, &move);
    QCOMPARE(underlined(), 0);
  }

  void windowFrameResizesFromEdgesAndCorners() {
    // The visible window spans [10, 390) x [10, 290) inside its shadow.
    const QSize size(400, 300);
    const QMargins shadow(10, 10, 10, 10);
    const auto at = [&](int x, int y) {
      return WindowFrame::edgesAt(QPoint(x, y), size, shadow);
    };
    QCOMPARE(at(200, 150), Qt::Edges());
    QCOMPARE(at(2, 150), Qt::Edges(Qt::LeftEdge));
    QCOMPARE(at(12, 150), Qt::Edges(Qt::LeftEdge));
    QCOMPARE(at(14, 150), Qt::Edges());
    QCOMPARE(at(395, 150), Qt::Edges(Qt::RightEdge));
    QCOMPARE(at(200, 5), Qt::Edges(Qt::TopEdge));
    QCOMPARE(at(200, 292), Qt::Edges(Qt::BottomEdge));
    QCOMPARE(at(5, 5), Qt::TopEdge | Qt::LeftEdge);
    // Within a corner's reach an edge resizes both ways.
    QCOMPARE(at(12, 20), Qt::TopEdge | Qt::LeftEdge);
    QCOMPARE(at(380, 292), Qt::BottomEdge | Qt::RightEdge);
    // Without a shadow the edges lie just inside the window.
    QCOMPARE(WindowFrame::edgesAt(QPoint(1, 150), size, QMargins()),
             Qt::Edges(Qt::LeftEdge));
    QCOMPARE(WindowFrame::edgesAt(QPoint(5, 150), size, QMargins()),
             Qt::Edges());
  }

  void titleBarControlsTheWindow() {
    auto &theme = Theme::instance();
    theme.setMode(Theme::Mode::Dark);
    theme.apply();
    QWidget window;
    auto *menus = new QMenuBar;
    menus->addMenu(QStringLiteral("&File"));
    auto *bar = new TitleBar(window, menus);
    QCOMPARE(menus->parentWidget(), bar);
    QAction palette;
    int runs = 0;
    connect(&palette, &QAction::triggered, &window, [&runs] { ++runs; });
    bar->setCommandCenterAction(&palette);
    bar->setCommandCenterText(QStringLiteral("xxd"),
                              QStringLiteral("NeverD - xxd /tmp"));
    auto *layout = new QVBoxLayout(&window);
    layout->setContentsMargins(QMargins());
    layout->addWidget(bar);
    layout->addStretch();
    window.resize(900, 400);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const auto button = [&](const char *name) {
      return bar->findChild<QAbstractButton *>(QLatin1String(name));
    };
    QVERIFY(button("minimizeButton") && button("maximizeButton") &&
            button("closeButton"));
    QCOMPARE(button("maximizeButton")->accessibleName(),
             QStringLiteral("Maximize"));

    // The command center names the file and runs its command.
    auto *center = button("commandCenter");
    QVERIFY(center && center->isVisible());
    QCOMPARE(center->text(), QStringLiteral("xxd"));
    QTest::mouseClick(center, Qt::LeftButton);
    QCOMPARE(runs, 1);

    // The maximize button and a double click on the bar's empty part toggle
    // the maximized state, and the button then offers to restore.
    QTest::mouseClick(button("maximizeButton"), Qt::LeftButton);
    QTRY_VERIFY(window.isMaximized());
    QCOMPARE(button("maximizeButton")->accessibleName(),
             QStringLiteral("Restore"));
    const QPoint empty(center->geometry().right() + 20, bar->height() / 2);
    QVERIFY(!bar->childAt(empty));
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier, empty);
    QTRY_VERIFY(!window.isMaximized());

    // Full screen hides the window controls.
    window.showFullScreen();
    QTRY_VERIFY(!button("closeButton")->isVisible());
    window.showNormal();
    QTRY_VERIFY(button("closeButton")->isVisible());
  }

  void loadQuestionFitsTheFontsItShowsWith() {
    // The style sheet gives the detail line its font only as the dialog
    // polishes; the question measured before then was a pixel short in
    // Chinese and Japanese.  A larger detail font shows the same gap.
    SymbolTablePromptDialog dialog(QJsonObject{
        {"table", QStringLiteral(".symtab")},
        {"names", QJsonArray{QStringLiteral("main"), QStringLiteral("die")}}});
    dialog.setStyleSheet(
        QStringLiteral("QLabel[statusLine=\"true\"] { font-size: 40px; }"));
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    QVERIFY2(
        dialog.height() >= dialog.layout()->totalHeightForWidth(dialog.width()),
        qPrintable(
            QStringLiteral("%1 < %2")
                .arg(dialog.height())
                .arg(dialog.layout()->totalHeightForWidth(dialog.width()))));
  }

  void titleFitLeavesAMessageBoxItsWrapping() {
    // A rich text wraps from the start, as About NeverD's does.  Fitting the
    // title measured it wrapped, so the box then showed it unwrapped and cut
    // off at the title's width, with no way to resize it.
    const QString text = QStringLiteral(
        "<h3>NeverD 1.0</h3><p>Interactive disassembler and decompiler "
        "workbench.</p><p>Licensed under the GNU Affero General Public "
        "License v3. Icons are original NeverD artwork.</p>");
    QMessageBox plain(QMessageBox::Information, QStringLiteral("About"), text);
    QMessageBox fitted(QMessageBox::Information, QStringLiteral("About"), text);
    WindowFrame::fitTitle(fitted, 240);
    plain.show();
    fitted.show();
    QVERIFY(QTest::qWaitForWindowExposed(&fitted));
    QCOMPARE(fitted.size(), plain.size());
    const auto *label =
        fitted.findChild<QLabel *>(QStringLiteral("qt_msgbox_label"));
    QVERIFY(label && label->wordWrap());
    QVERIFY(label->heightForWidth(label->width()) <= label->height());
  }

  void titleFitWidensAShortMessage() {
    // A text narrower than the title still widens to show the title whole.
    for (const QString &text :
         {QStringLiteral("Done."), QStringLiteral("<b>Done.</b>")}) {
      QMessageBox box(QMessageBox::Information, QStringLiteral("Saved"), text);
      WindowFrame::fitTitle(box, 420);
      box.show();
      QVERIFY(QTest::qWaitForWindowExposed(&box));
      QVERIFY2(box.width() >= 420, qPrintable(text));
    }
  }

  void earlyDialogWindowsLeaveTheWorkbenchWhole() {
    // A file dialog is polished while it is built, and its frame then takes
    // its native window before it first shows (WindowFrame::attach).  Qt
    // gave every child of the dialog's parent a native window as well: the
    // workbench's title bar, toolbars, docks and status bar became Wayland
    // subsurfaces that GNOME stopped updating once Load file > FLIRT
    // signature file was cancelled.  Over a view inside the workbench, as
    // Extensions' Import manifest opens one, this climbed through every
    // widget around the view.
    QMainWindow workbench;
    workbench.setMenuBar(new QMenuBar);
    workbench.menuBar()->addMenu(QStringLiteral("&File"));
    workbench.addToolBar(QStringLiteral("File"))
        ->addAction(QStringLiteral("Open"));
    auto *central = new QWidget;
    auto *layout = new QVBoxLayout(central);
    layout->addWidget(new QLabel(QStringLiteral("Status")));
    auto *view = new QPlainTextEdit;
    layout->addWidget(view);
    workbench.setCentralWidget(central);
    workbench.show();
    QVERIFY(QTest::qWaitForWindowExposed(&workbench));
    for (QWidget *parent :
         {static_cast<QWidget *>(&workbench), static_cast<QWidget *>(view)}) {
      QDialog dialog(parent);
      QVERIFY(!dialog.windowHandle());
      dialog.winId();
      QVERIFY(dialog.windowHandle());
      const auto widgets = workbench.findChildren<QWidget *>();
      QVERIFY(widgets.size() >= 5);
      for (const QWidget *widget : widgets)
        if (!widget->isWindow())
          QVERIFY2(!widget->internalWinId(), widget->metaObject()->className());
    }
  }

  void nativeFrameKeepsTheTitleBarOnScreen() {
    // Windows and macOS keep the system frame without its caption, so the
    // window system decides where a maximized or full-screen window's client
    // area lies; CI runs this with their native platforms.
    if (platform::frameKind() != platform::FrameKind::Captionless)
      QSKIP("The platform has no captionless system frame");
    QWidget window;
    auto *menus = new QMenuBar;
    menus->addMenu(QStringLiteral("&File"));
    auto *bar = new TitleBar(window, menus);
    QVERIFY(WindowFrame::attach(window, bar));
    // Small CI screens (1024x768) leave the default placement partly off
    // screen; start inside the available area.
    window.resize(900, 600);
    window.move(window.screen()->availableGeometry().topLeft() +
                QPoint(40, 40));
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    AppliedStates applied;
    window.windowHandle()->installEventFilter(&applied);
    QScreen *screen = window.screen();
    const auto barRect = [&] {
      return QRect(bar->mapToGlobal(QPoint(0, 0)), bar->size());
    };
    const auto report = [&](const char *step) {
      qInfo() << step << "applied" << applied.states << "frame"
              << window.frameGeometry() << "client" << window.geometry()
              << "contents" << window.contentsMargins() << "bar" << barRect()
              << "screen" << screen->geometry() << "available"
              << screen->availableGeometry();
    };
    // Each step waits until the window system confirms the state on the
    // native window: macOS animates full screen and ignores a request made
    // before an animation ends, and reports full screen alone over maximized.
    const auto request = [&](Qt::WindowStates states, const char *step) {
      const int before = applied.count;
      window.setWindowState(states);
      const bool confirmed = QTest::qWaitFor(
          [&] {
            return applied.count > before &&
                   effectiveState(applied.states) == effectiveState(states);
          },
          10000);
      QTest::qWait(300);
      report(step);
      return confirmed;
    };
    report("normal");
    const QSize normal = window.size();
    QVERIFY(screen->geometry().contains(barRect()));

    QVERIFY(request(Qt::WindowMaximized, "maximized"));
    QVERIFY(screen->availableGeometry().contains(barRect()));
    QVERIFY(request(Qt::WindowNoState, "restored"));
    QCOMPARE(window.size(), normal);

    // Full screen twice: its client area must not lose the title bar, and a
    // restore must not move the window's size.
    for (int round = 0; round < 2; ++round) {
      QVERIFY(request(Qt::WindowFullScreen, "full screen"));
      QVERIFY(screen->geometry().contains(barRect()));
      QCOMPARE(barRect().top(), screen->geometry().top());
      QVERIFY(request(Qt::WindowNoState, "restored"));
      QCOMPARE(window.size(), normal);
    }

    // F11 keeps a maximized window maximized under full screen: no border
    // inset there, and leaving full screen returns to the maximized size.
    QVERIFY(request(Qt::WindowMaximized, "maximized"));
    const QSize maximized = window.size();
    QVERIFY(request(Qt::WindowMaximized | Qt::WindowFullScreen,
                    "full screen from maximized"));
    QVERIFY(screen->geometry().contains(barRect()));
    QCOMPARE(barRect().top(), screen->geometry().top());
    QVERIFY(request(Qt::WindowMaximized, "maximized again"));
    QCOMPARE(window.size(), maximized);
    QVERIFY(screen->availableGeometry().contains(barRect()));
    QVERIFY(request(Qt::WindowNoState, "restored"));
    QCOMPARE(window.size(), normal);
  }

  void dialogTitleBarOffersTheButtonsItsFlagsAskFor() {
    QDialog dialog;
    dialog.setWindowTitle(QStringLiteral("Jump anywhere"));
    auto *bar = new TitleBar(dialog);
    dialog.resize(500, 300);
    bar->setGeometry(0, 0, 500, bar->sizeHint().height());
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    const auto button = [](TitleBar *owner, const char *name) {
      return owner->findChild<QAbstractButton *>(QLatin1String(name));
    };
    // A dialog's bar has no command center and offers Close alone.
    QVERIFY(!button(bar, "commandCenter"));
    QVERIFY(button(bar, "closeButton")->isVisible());
    QVERIFY(!button(bar, "maximizeButton")->isVisible());
    QVERIFY(!button(bar, "minimizeButton")->isVisible());
    QTest::mouseDClick(bar, Qt::LeftButton, Qt::NoModifier,
                       QPoint(20, bar->height() / 2));
    QVERIFY(!dialog.isMaximized());
    QTest::mouseClick(button(bar, "closeButton"), Qt::LeftButton);
    QTRY_VERIFY(!dialog.isVisible());

    // A dialog whose flags ask for Maximize offers it.
    QDialog resizable(
        nullptr, Qt::Dialog | Qt::CustomizeWindowHint | Qt::WindowTitleHint |
                     Qt::WindowCloseButtonHint | Qt::WindowMaximizeButtonHint);
    auto *second = new TitleBar(resizable);
    resizable.resize(500, 300);
    second->setGeometry(0, 0, 500, second->sizeHint().height());
    resizable.show();
    QVERIFY(QTest::qWaitForWindowExposed(&resizable));
    QAbstractButton *maximize = button(second, "maximizeButton");
    QVERIFY(maximize->isVisible());
    QTest::mouseClick(maximize, Qt::LeftButton);
    QTRY_VERIFY(resizable.isMaximized());
  }

  void returnOpensTheCurrentItemOnce() {
    // macOS item views would edit the item on Return instead of opening it;
    // elsewhere the filter must not open it twice.
    QListWidget list;
    list.addItems({QStringLiteral("first"), QStringLiteral("second")});
    activateOnReturn(list);
    list.show();
    QVERIFY(QTest::qWaitForWindowExposed(&list));
    list.setCurrentRow(1);
    QStringList opened;
    connect(&list, &QListWidget::itemActivated, &list,
            [&opened](QListWidgetItem *item) { opened << item->text(); });
    QTest::keyClick(&list, Qt::Key_Return);
    QCOMPARE(opened, QStringList{QStringLiteral("second")});
    QTest::keyClick(&list, Qt::Key_Enter, Qt::KeypadModifier);
    QCOMPARE(opened.size(), 2);
  }

  void returnInADialogPressesItsDefaultButton() {
    // Qt's GNOME platform theme makes Return press the focused check box,
    // as this one does: after Alt+O checked "Does not return", Return
    // cleared it and the dialog stayed open.
    class ReturnPressedCheckBox final : public QCheckBox {
    public:
      using QCheckBox::QCheckBox;

    protected:
      void keyPressEvent(QKeyEvent *event) override {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
          toggle();
        else
          QCheckBox::keyPressEvent(event);
      }
    };
    const std::unique_ptr<QObject> filter(acceptDialogsOnReturn(*qApp));
    QDialog dialog;
    auto *layout = new QVBoxLayout(&dialog);
    auto *box =
        new ReturnPressedCheckBox(QStringLiteral("D&oes not return"), &dialog);
    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(box);
    layout->addWidget(buttons);
    dialog.show();
    QVERIFY(QTest::qWaitForWindowExposed(&dialog));
    box->setFocus();
    QTest::keyClick(box, Qt::Key_Space);
    QVERIFY(box->isChecked());
    QTest::keyClick(box, Qt::Key_Return);
    QVERIFY(box->isChecked());
    QCOMPARE(dialog.result(), int(QDialog::Accepted));
    QVERIFY(!dialog.isVisible());
    // Outside a dialog the button keeps the key.
    ReturnPressedCheckBox alone(QStringLiteral("Alone"));
    alone.show();
    QVERIFY(QTest::qWaitForWindowExposed(&alone));
    QTest::keyClick(&alone, Qt::Key_Return);
    QVERIFY(alone.isChecked());
  }

  void themeDrawsFlatControls() {
    auto &theme = Theme::instance();
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      theme.setMode(mode);
      theme.apply();
      QDialog dialog;
      auto *layout = new QVBoxLayout(&dialog);
      auto *box = new QCheckBox(QStringLiteral("Unchecked"), &dialog);
      auto *combo = new QComboBox(&dialog);
      combo->addItem(QStringLiteral("Item"));
      auto *edit = new QLineEdit(&dialog);
      auto *log = new QPlainTextEdit(&dialog);
      log->setReadOnly(true);
      auto *buttons = new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
      for (QWidget *widget :
           {static_cast<QWidget *>(box), static_cast<QWidget *>(combo),
            static_cast<QWidget *>(edit), static_cast<QWidget *>(log),
            static_cast<QWidget *>(buttons)})
        layout->addWidget(widget);
      dialog.show();
      QVERIFY(QTest::qWaitForWindowExposed(&dialog));
      const QImage image = dialog.grab().toImage();
      const auto pixel = [&](QWidget *widget, QPoint point) {
        return image.pixelColor(widget->mapTo(&dialog, point));
      };

      // An unchecked box stands out from the dialog behind it.
      QStyleOptionButton option;
      option.initFrom(box);
      const QRect indicator = box->style()->subElementRect(
          QStyle::SE_CheckBoxIndicator, &option, box);
      const QPoint middle(indicator.left(), indicator.center().y());
      QCOMPARE(pixel(box, middle),
               theme.chrome(QStringLiteral("CheckboxBorder")));
      QCOMPARE(pixel(box, indicator.center()),
               theme.chrome(QStringLiteral("CheckboxBackground")));
      QVERIFY(theme.chrome(QStringLiteral("CheckboxBorder")) !=
              theme.chrome(QStringLiteral("Window")));

      // The combo box draws its chevron on the field.
      QStyleOptionComboBox comboOption;
      comboOption.initFrom(combo);
      const QRect arrow = combo->style()->subControlRect(
          QStyle::CC_ComboBox, &comboOption, QStyle::SC_ComboBoxArrow, combo);
      int glyph = 0;
      for (int y = arrow.top(); y <= arrow.bottom(); ++y)
        for (int x = arrow.left(); x <= arrow.right(); ++x)
          glyph +=
              pixel(combo, {x, y}) != theme.chrome(QStringLiteral("Input"));
      QVERIFY(glyph > 4);

      // Text fields type in the code font; read-only text reads as content.
      // The families are what the theme asks for: a platform without the
      // code font (Windows offscreen) resolves it to another face.
      QCOMPARE(edit->font().family(), theme.codeFont().family());
      QVERIFY(box->font().family() != theme.codeFont().family());
      QCOMPARE(pixel(log, log->rect().center()),
               theme.chrome(QStringLiteral("Base")));
      // Dialog buttons carry no icons.
      for (auto *button : buttons->buttons())
        QVERIFY(button->icon().isNull());
      // Splitters inside views are hairlines, like those between docks.
      QSplitter splitter;
      splitter.addWidget(new QWidget);
      splitter.addWidget(new QWidget);
      splitter.ensurePolished();
      QCOMPARE(splitter.handleWidth(), 1);
    }
    theme.setMode(Theme::Mode::Dark);
  }

  void themesDefineEveryColor() {
    auto &theme = Theme::instance();
    for (const auto mode : {Theme::Mode::Dark, Theme::Mode::Light}) {
      theme.setMode(mode);
      for (int role = 0; role < int(ColorRole::Count); ++role)
        QVERIFY2(theme.color(ColorRole(role)).isValid(),
                 qPrintable(QString::number(role)));
    }
    theme.setMode(Theme::Mode::Dark);
    QVERIFY(theme.dark());
    // Visual Studio Code Dark+ editor background.
    QCOMPARE(theme.color(ColorRole::ListingBackground),
             QColor(0x1e, 0x1e, 0x1e));
  }
};

QTEST_MAIN(WorkbenchUnitTests)
#include "WorkbenchUnitTests.moc"
