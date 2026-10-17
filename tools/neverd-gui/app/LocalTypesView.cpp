#include "LocalTypesView.h"

#include "DeclareTypesDialog.h"
#include "FlowLayout.h"
#include "Icons.h"
#include "ItemViewActivation.h"
#include "Session.h"
#include "ShrinkableRow.h"
#include "Theme.h"
#include "TypeSourceHighlighter.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QHelpEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QSettings>
#include <QSortFilterProxyModel>
#include <QSplitter>
#include <QStackedWidget>
#include <QStandardItemModel>
#include <QStyledItemDelegate>
#include <QTabBar>
#include <QTextBlock>
#include <QTextDocument>
#include <QToolTip>
#include <QToolButton>
#include <QTreeView>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <functional>
#include <span>
#include <utility>

namespace neverd::gui {

namespace {

// The list's columns, and the roles the list and the layout keep.
enum ListColumn {
  OrdinalColumn,
  NameColumn,
  KindColumn,
  SizeColumn,
  MembersColumn,
  ListColumns
};
// Where a member lies first, as a memory map reads; the tree indents what a
// record holds in the member's own column.
enum LayoutColumn {
  OffsetColumn,
  SizeOfColumn,
  MemberColumn,
  TypeColumn,
  LayoutColumns
};
constexpr int SortRole = Qt::UserRole;
constexpr int NameRole = Qt::UserRole + 1;
constexpr int RefersRole = Qt::UserRole + 2;
/// A member's name as the source declares it: empty for an anonymous
/// record and for padding.
constexpr int DeclaredRole = Qt::UserRole + 3;

/// What each kind of type reads as, and the icon the list gives it.
struct KindEntry {
  const char *kind;
  const char *title;
  const char *icon;
};
constexpr KindEntry Kinds[] = {
    {"struct", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "structure"),
     "type_struct"},
    {"union", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "union"),
     "type_union"},
    {"enum", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "enumeration"),
     "type_enum"},
    {"typedef", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "typedef"),
     "type_typedef"},
    {"function",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "function type"),
     "functions"}};
const KindEntry *kindEntry(const QString &kind) {
  for (const KindEntry &entry : Kinds)
    if (kind == QLatin1String(entry.kind))
      return &entry;
  return nullptr;
}

/// The languages a type renders in, in tab order after the layout, and
/// whether their declarations read back as the engine's parseTypes reads.
struct SourceLanguage {
  const char *id;
  const char *title;
  const char *filter;
  const char *suffix;
  bool reads;
};
// C/C++ shows C, which C++ compiles too.
constexpr SourceLanguage Languages[] = {
    {"c", "C/C++",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "C/C++ header (*.h)"),
     "h", true},
    {"rust", "Rust",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Rust (*.rs)"), "rs",
     true},
    {"go", "Go", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Go (*.go)"),
     "go", true},
    {"python", "Python",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Python (*.py)"), "py",
     false},
    // JavaScript whose JSDoc TypeScript checks.
    {"javascript", "JS/TS",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                       "JavaScript module (*.js *.mjs)"),
     "js", false},
    {"lua", "Lua",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Lua (*.lua)"), "lua",
     false},
};
/// The language types shown in \p language are declared in: that one, or C
/// where it renders only.
QString declaredIn(const QString &language) {
  for (const SourceLanguage &entry : Languages)
    if (language == QLatin1String(entry.id) && entry.reads)
      return language;
  return QStringLiteral("c");
}

/// How C and C++, and the layout, spell builtins (the engine's spellings),
/// exact widths, the default, first.
struct Spelling {
  const char *id;
  const char *title;
};
constexpr Spelling Spellings[] = {
    {"portable", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "Exact widths (uint32_t)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
    {"keywords", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "C keywords (unsigned long)")},
};
/// How Rust and Go do: exact widths, or Windows' names, which they declare
/// as aliases as wdk-sys does.
constexpr Spelling RustSpellings[] = {
    {"portable",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Exact widths (u32)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
};
constexpr Spelling GoSpellings[] = {
    {"portable",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Exact widths (uint32)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
};
constexpr Spelling PythonSpellings[] = {
    {"portable", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "Exact widths (c_uint32)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
};
// Lua's values are its integers either way; the spelling names the C types
// the comments give.
constexpr Spelling LuaSpellings[] = {
    {"portable", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "Exact widths (uint32_t)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
};
constexpr Spelling JavaScriptSpellings[] = {
    {"portable", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView",
                                   "JavaScript types (number, bigint)")},
    {"windows",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Windows names (ULONG)")},
};

/// The spellings each language chooses from, the first until one is
/// chosen, and where the choice is kept.  C's serves the layout too.
struct SpellingChoice {
  const char *language;
  std::span<const Spelling> spellings;
  const char *setting;
};
const SpellingChoice SpellingChoices[] = {
    {"c", Spellings, "localTypes/spellings/c"},
    {"rust", RustSpellings, "localTypes/spellings/rust"},
    {"go", GoSpellings, "localTypes/spellings/go"},
    {"python", PythonSpellings, "localTypes/spellings/python"},
    {"javascript", JavaScriptSpellings, "localTypes/spellings/javascript"},
    {"lua", LuaSpellings, "localTypes/spellings/lua"},
};
/// Where the choices were kept until exact widths became every language's
/// default (2026-10): the choices made until then start over.
constexpr const char *RetiredSpellingSettings[] = {
    "localTypes/spelling",           "localTypes/rustSpelling",
    "localTypes/goSpelling",         "localTypes/pythonSpelling",
    "localTypes/javascriptSpelling", "localTypes/luaSpelling",
};

/// The choice \p language makes: C++'s and the layout's are C's.
const SpellingChoice &spellingChoice(const QString &language) {
  for (const SpellingChoice &choice : SpellingChoices)
    if (language == QLatin1String(choice.language))
      return choice;
  return SpellingChoices[0];
}

// The tab shown, by its language's id; empty for the layout.  It replaces
// "localTypes/page", the tab's index when C and C++ had a tab each.
constexpr char ViewSetting[] = "localTypes/view";
constexpr char OldPageSetting[] = "localTypes/page";
constexpr char HexSetting[] = "localTypes/hexOffsets";
constexpr char DependenciesSetting[] = "localTypes/dependencies";
constexpr char AssertsSetting[] = "localTypes/asserts";
constexpr char OriginSetting[] = "localTypes/origin";
constexpr char SplitterSetting[] = "localTypes/splitter";
constexpr char ListHeaderSetting[] = "localTypes/listHeader";

/// Where the types the list shows come from: every origin, or one the
/// worker names.
struct OriginChoice {
  const char *id;
  const char *title;
};
constexpr OriginChoice Origins[] = {
    {"", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "All types")},
    {"pdb", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "From the PDB")},
    {"header",
     QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "From system headers")},
    {"user", QT_TRANSLATE_NOOP("neverd::gui::LocalTypesView", "Declared")}};
/// The narrowest each pane may get, as the extensions window keeps.
constexpr int MinimumPaneWidth = 48;

QString hex(qint64 value) {
  return QStringLiteral("0x%1")
      .arg(value, 0, 16)
      .toUpper()
      .replace(QLatin1String("0X"), QLatin1String("0x"));
}

} // namespace

LocalTypesView::LocalTypesView(Session &session, QWidget *parent)
    : QWidget(parent), session_(session) {
  auto *layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto *splitter = new QSplitter(this);
  layout->addWidget(splitter);

  // The list: the filter as wide as it, and below it what narrows the list
  // and changes it.
  auto *left = new QWidget(splitter);
  left->setMinimumWidth(MinimumPaneWidth);
  auto *leftLayout = new QVBoxLayout(left);
  leftLayout->setContentsMargins(4, 4, 4, 4);
  filter_ = new QLineEdit(left);
  filter_->setObjectName(QStringLiteral("localTypesFilter"));
  filter_->setPlaceholderText(tr("Filter types by name"));
  filter_->setClearButtonEnabled(true);
  filter_->installEventFilter(this);
  leftLayout->addWidget(filter_);
  auto *filterRow = new QWidget(left);
  auto *filterLayout = new QHBoxLayout(filterRow);
  filterLayout->setContentsMargins(0, 0, 0, 0);
  origin_ = new QComboBox(filterRow);
  origin_->setObjectName(QStringLiteral("localTypesOrigin"));
  origin_->setToolTip(tr("Show the types of one origin"));
  for (const auto &entry : Origins)
    origin_->addItem(tr(entry.title), QString::fromLatin1(entry.id));
  filterLayout->addWidget(origin_);
  filterLayout->addStretch(1);
  const auto commandButton = [&](TypesCommand command, const char *iconName,
                                 const QString &text) {
    auto *button = new QToolButton(filterRow);
    button->setIcon(icon(QString::fromLatin1(iconName)));
    button->setToolTip(QStringLiteral("%1 (%2)").arg(
        text, commandKey(command).toString(QKeySequence::NativeText)));
    button->setAutoRaise(true);
    connect(button, &QToolButton::clicked, this,
            [this, command] { run(command); });
    filterLayout->addWidget(button);
    return button;
  };
  add_ = commandButton(TypesCommand::Add, "local_types_add",
                       tr("Add types from a declaration"));
  add_->setObjectName(QStringLiteral("localTypesAdd"));
  edit_ = commandButton(TypesCommand::Edit, "local_types_edit",
                        tr("Edit the type's declaration"));
  edit_->setObjectName(QStringLiteral("localTypesEdit"));
  delete_ = commandButton(TypesCommand::Delete, "local_types_delete",
                          tr("Delete the selected types"));
  delete_->setObjectName(QStringLiteral("localTypesDelete"));
  makeRowShrinkable(*filterRow);
  leftLayout->addWidget(filterRow);
  model_ = new QStandardItemModel(0, ListColumns, this);
  model_->setHorizontalHeaderLabels(
      {tr("#"), tr("Name"), tr("Kind"), tr("Size"), tr("Members")});
  proxy_ = new QSortFilterProxyModel(this);
  proxy_->setSourceModel(model_);
  proxy_->setFilterKeyColumn(NameColumn);
  proxy_->setFilterCaseSensitivity(Qt::CaseInsensitive);
  proxy_->setSortRole(SortRole);
  list_ = new QTreeView(left);
  list_->setObjectName(QStringLiteral("localTypesList"));
  list_->setModel(proxy_);
  list_->setRootIsDecorated(false);
  list_->setUniformRowHeights(true);
  list_->setSortingEnabled(true);
  list_->sortByColumn(OrdinalColumn, Qt::AscendingOrder);
  list_->setSelectionMode(QAbstractItemView::ExtendedSelection);
  list_->setAllColumnsShowFocus(true);
  list_->header()->setStretchLastSection(false);
  list_->header()->setSectionResizeMode(NameColumn, QHeaderView::Stretch);
  // The ordinal and the member count stay out of the way unless asked for,
  // leaving the names room; the header's menu shows and hides every column
  // but the name.
  list_->header()->setSectionHidden(OrdinalColumn, true);
  list_->header()->setSectionHidden(MembersColumn, true);
  list_->header()->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(list_->header(), &QWidget::customContextMenuRequested, this,
          [this](const QPoint &at) {
            QMenu menu(this);
            for (int column = 0; column < ListColumns; ++column) {
              if (column == NameColumn)
                continue;
              auto *action = menu.addAction(
                  model_->headerData(column, Qt::Horizontal).toString());
              action->setCheckable(true);
              action->setChecked(!list_->header()->isSectionHidden(column));
              connect(action, &QAction::toggled, this, [this, column](bool on) {
                list_->header()->setSectionHidden(column, !on);
                QSettings().setValue(ListHeaderSetting,
                                     list_->header()->saveState());
              });
            }
            menu.exec(list_->header()->viewport()->mapToGlobal(at));
          });
  list_->installEventFilter(this);
  list_->setContextMenuPolicy(Qt::CustomContextMenu);
  activateOnReturn(*list_);
  leftLayout->addWidget(list_, 1);
  summary_ = new QLabel(left);
  summary_->setObjectName(QStringLiteral("localTypesSummary"));
  // Every count shows, on more lines when the list is narrow.
  summary_->setWordWrap(true);
  leftLayout->addWidget(summary_);

  // The type.
  auto *right = new QWidget(splitter);
  right->setMinimumWidth(MinimumPaneWidth);
  auto *rightLayout = new QVBoxLayout(right);
  rightLayout->setContentsMargins(4, 4, 4, 4);
  auto *headingRow = new QWidget(right);
  auto *headingLayout = new QHBoxLayout(headingRow);
  headingLayout->setContentsMargins(0, 0, 0, 0);
  back_ = new QToolButton(headingRow);
  back_->setIcon(icon(QStringLiteral("back")));
  back_->setToolTip(tr("Back to the type shown before (Esc)"));
  back_->setAutoRaise(true);
  heading_ = new QLabel(headingRow);
  heading_->setObjectName(QStringLiteral("localTypesHeading"));
  heading_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  headingLayout->addWidget(back_);
  headingLayout->addWidget(heading_, 1);
  makeRowShrinkable(*headingRow);
  rightLayout->addWidget(headingRow);
  // The breadcrumb of the member selected, each record a link.
  path_ = new QLabel(right);
  path_->setObjectName(QStringLiteral("localTypesPath"));
  path_->setTextFormat(Qt::RichText);
  path_->setTextInteractionFlags(Qt::LinksAccessibleByMouse |
                                 Qt::LinksAccessibleByKeyboard);
  path_->setVisible(false);
  rightLayout->addWidget(path_);

  auto *tabRow = new QWidget(right);
  auto *tabLayout = new QHBoxLayout(tabRow);
  tabLayout->setContentsMargins(0, 0, 0, 0);
  tabs_ = new QTabBar(tabRow);
  tabs_->setObjectName(QStringLiteral("localTypesTabs"));
  tabs_->setDrawBase(false);
  tabs_->setExpanding(false);
  tabs_->addTab(tr("Layout"));
  for (const auto &language : Languages)
    tabs_->addTab(QString::fromLatin1(language.title));
  copy_ = new QToolButton(tabRow);
  copy_->setIcon(icon(QStringLiteral("copy")));
  copy_->setToolTip(tr("Copy the layout or the source"));
  copy_->setAutoRaise(true);
  export_ = new QToolButton(tabRow);
  export_->setIcon(icon(QStringLiteral("file_produce")));
  export_->setToolTip(tr("Export types as source"));
  export_->setAutoRaise(true);
  export_->setPopupMode(QToolButton::InstantPopup);
  auto *exportMenu = new QMenu(export_);
  auto *exportOne = exportMenu->addAction(tr("Export this type…"));
  auto *exportSelected = exportMenu->addAction(tr("Export selected types…"));
  auto *exportAll = exportMenu->addAction(tr("Export all types…"));
  export_->setMenu(exportMenu);
  tabLayout->addWidget(tabs_);
  tabLayout->addStretch(1);
  tabLayout->addWidget(copy_);
  tabLayout->addWidget(export_);
  makeRowShrinkable(*tabRow);
  rightLayout->addWidget(tabRow);

  // The options wrap to another row where the pane is too narrow for them,
  // rather than one running over the next.
  auto *optionRow = new QWidget(right);
  optionRow->setObjectName(QStringLiteral("localTypesOptions"));
  auto *optionLayout = new FlowLayout(optionRow);
  spelling_ = new QComboBox(optionRow);
  spelling_->setObjectName(QStringLiteral("localTypesSpelling"));
  spelling_->setToolTip(tr("How the source names builtin types"));
  // As wide as the widest choice of any language, so that switching tabs
  // neither moves the row nor cuts a choice short.
  for (const SpellingChoice &choice : SpellingChoices)
    for (const auto &entry : choice.spellings)
      spelling_->addItem(tr(entry.title));
  spelling_->setMinimumWidth(spelling_->sizeHint().width());
  spelling_->clear();
  hex_ = new QCheckBox(tr("Hex offsets"), optionRow);
  dependencies_ = new QCheckBox(tr("With dependencies"), optionRow);
  dependencies_->setToolTip(
      tr("Also declare every type this one needs, so the source compiles"));
  asserts_ = new QCheckBox(tr("Layout checks"), optionRow);
  asserts_->setToolTip(tr("Follow each type with checks of its size and "
                          "offsets that the compiler verifies"));
  optionLayout->addWidget(spelling_);
  optionLayout->addWidget(hex_);
  optionLayout->addWidget(dependencies_);
  optionLayout->addWidget(asserts_);
  makeRowShrinkable(*optionRow);
  rightLayout->addWidget(optionRow);

  pages_ = new QStackedWidget(right);
  layout_ = new QTreeWidget(pages_);
  layout_->setObjectName(QStringLiteral("localTypesLayout"));
  layout_->setColumnCount(LayoutColumns);
  layout_->setHeaderLabels(
      {tr("Offset"), tr("Size"), tr("Member"), tr("Type")});
  layout_->setTreePosition(MemberColumn);
  layout_->setUniformRowHeights(true);
  layout_->setAllColumnsShowFocus(true);
  layout_->setFont(Theme::instance().codeFont());
  layout_->header()->setStretchLastSection(true);
  layout_->installEventFilter(this);
  layout_->viewport()->installEventFilter(this);
  source_ = new QPlainTextEdit(pages_);
  source_->setObjectName(QStringLiteral("localTypesSource"));
  source_->setReadOnly(true);
  source_->setLineWrapMode(QPlainTextEdit::NoWrap);
  source_->setFont(Theme::instance().codeFont());
  source_->viewport()->installEventFilter(this);
  // A type's name is a link while Ctrl is held over it.
  source_->viewport()->setMouseTracking(true);
  source_->installEventFilter(this);
  highlighter_ = new TypeSourceHighlighter(source_->document());
  pages_->addWidget(layout_);
  pages_->addWidget(source_);
  rightLayout->addWidget(pages_, 1);
  splitter->setStretchFactor(1, 1);
  splitter->setSizes({340, 560});

  const QSettings settings;
  // The panes and the list's columns as the user left them.
  splitter->restoreState(settings.value(SplitterSetting).toByteArray());
  connect(splitter, &QSplitter::splitterMoved, this, [splitter] {
    QSettings().setValue(SplitterSetting, splitter->saveState());
  });
  list_->header()->restoreState(
      settings.value(ListHeaderSetting).toByteArray());
  list_->header()->setSectionHidden(NameColumn, false);
  QString view = settings.value(ViewSetting).toString();
  if (!settings.contains(ViewSetting)) {
    // Layout, C, C++, Rust, Go: C++ shows as C now.
    static const char *const OldPages[] = {"", "c", "c", "rust", "go"};
    const int old = settings.value(OldPageSetting, 0).toInt();
    view = QString::fromLatin1(
        OldPages[std::clamp(old, 0, int(std::size(OldPages)) - 1)]);
  }
  int shown = 0;
  for (size_t I = 0; I < std::size(Languages); ++I)
    if (view == QLatin1String(Languages[I].id))
      shown = int(I) + 1;
  tabs_->setCurrentIndex(shown);
  pages_->setCurrentIndex(tabs_->currentIndex() ? 1 : 0);
  for (const char *retired : RetiredSpellingSettings)
    QSettings().remove(QString::fromLatin1(retired));
  for (const SpellingChoice &choice : SpellingChoices) {
    // A choice the language does not offer, as a setting of another build
    // may hold, is the first.
    const QString initial = QString::fromLatin1(choice.spellings.front().id);
    QString chosen =
        settings.value(QString::fromLatin1(choice.setting), initial).toString();
    if (std::none_of(choice.spellings.begin(), choice.spellings.end(),
                     [&](const Spelling &entry) {
                       return chosen == QLatin1String(entry.id);
                     }))
      chosen = initial;
    spellings_.insert(QString::fromLatin1(choice.language), chosen);
  }
  showSpellings();
  hex_->setChecked(settings.value(HexSetting, true).toBool());
  dependencies_->setChecked(
      settings.value(DependenciesSetting, false).toBool());
  asserts_->setChecked(settings.value(AssertsSetting, false).toBool());
  highlighter_->setLanguage(language());

  origin_->setCurrentIndex(std::max(
      origin_->findData(settings.value(OriginSetting, QString()).toString()),
      0));
  connect(origin_, &QComboBox::currentIndexChanged, this, [this] {
    QSettings().setValue(OriginSetting, origin_->currentData().toString());
    if (!lastList_.isEmpty())
      listed(lastList_);
  });
  connect(filter_, &QLineEdit::textChanged, this, [this](const QString &text) {
    proxy_->setFilterFixedString(text);
    updateActions();
  });
  connect(list_->selectionModel(), &QItemSelectionModel::selectionChanged, this,
          &LocalTypesView::selectionChanged);
  connect(list_->selectionModel(), &QItemSelectionModel::currentRowChanged,
          this, &LocalTypesView::selectionChanged);
  connect(tabs_, &QTabBar::currentChanged, this, [this](int index) {
    QSettings().setValue(ViewSetting, index ? language() : QString());
    pages_->setCurrentIndex(index ? 1 : 0);
    highlighter_->setLanguage(language());
    showSpellings();
    if (index)
      requestSource();
    else
      fitLayoutColumns();
    showPath();
    updateActions();
  });
  connect(spelling_, &QComboBox::currentIndexChanged, this, [this] {
    // The layout spells builtins as C does.
    const SpellingChoice &choice =
        spellingChoice(tabs_->currentIndex() ? language() : QString());
    const QString chosen = spelling_->currentData().toString();
    spellings_.insert(QString::fromLatin1(choice.language), chosen);
    QSettings().setValue(QString::fromLatin1(choice.setting), chosen);
    if (QLatin1String(choice.language) == QLatin1String("c"))
      requestLayout();
    requestSource();
  });
  connect(hex_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(HexSetting, on);
    laidOut(type_);
    requestSource();
  });
  connect(dependencies_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(DependenciesSetting, on);
    requestSource();
  });
  connect(asserts_, &QCheckBox::toggled, this, [this](bool on) {
    QSettings().setValue(AssertsSetting, on);
    requestSource();
  });
  connect(list_, &QTreeView::customContextMenuRequested, this,
          [this](const QPoint &at) {
            QMenu menu(this);
            const auto item = [&](TypesCommand command, const QString &text,
                                  const QToolButton *button) {
              auto *action = menu.addAction(button->icon(), text, this,
                                            [this, command] { run(command); });
              action->setShortcut(commandKey(command));
              action->setEnabled(button->isEnabled());
            };
            item(TypesCommand::Add, tr("Add types…"), add_);
            item(TypesCommand::Edit, tr("Edit type…"), edit_);
            item(TypesCommand::Delete, tr("Delete types…"), delete_);
            menu.exec(list_->viewport()->mapToGlobal(at));
          });
  connect(back_, &QToolButton::clicked, this, &LocalTypesView::back);
  connect(copy_, &QToolButton::clicked, this, &LocalTypesView::copy);
  connect(exportOne, &QAction::triggered, this, [this] {
    if (!current_.isEmpty())
      exportTypes({current_});
  });
  connect(exportSelected, &QAction::triggered, this,
          [this] { exportTypes(selectedNames()); });
  connect(exportAll, &QAction::triggered, this, [this] { exportTypes({}); });
  connect(exportMenu, &QMenu::aboutToShow, this,
          [this, exportOne, exportSelected, exportAll] {
            exportOne->setEnabled(!current_.isEmpty());
            exportSelected->setEnabled(selectedNames().size() > 1);
            exportAll->setEnabled(model_->rowCount() > 0);
          });
  connect(layout_, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem *item) {
            const QString refers = item->data(0, RefersRole).toString();
            if (!refers.isEmpty())
              open(refers, true);
          });
  connect(layout_, &QTreeWidget::currentItemChanged, this,
          &LocalTypesView::showPath);
  connect(path_, &QLabel::linkActivated, this, [this](const QString &link) {
    // The type's crumb leads to its first member; a record's, to it.
    QTreeWidgetItem *item = nullptr;
    for (const QString &step : link.split(QLatin1Char('/'),
                                          Qt::SkipEmptyParts)) {
      const int row = step.toInt();
      item = item ? item->child(row) : layout_->topLevelItem(row);
      if (!item)
        return;
    }
    if (!item) {
      layout_->setCurrentItem(nullptr);
      layout_->scrollToTop();
      return;
    }
    layout_->setCurrentItem(item);
    layout_->scrollToItem(item);
    layout_->setFocus();
  });
  connect(source_, &QPlainTextEdit::cursorPositionChanged, this,
          &LocalTypesView::highlightSourceLine);
  connect(&session_, &Session::opened, this, &LocalTypesView::reload);
  connect(&session_, &Session::revisionChanged, this, &LocalTypesView::reload);
  connect(&session_, &Session::unloaded, this, &LocalTypesView::reload);
  connect(&Theme::instance(), &Theme::changed, this, [this] {
    layout_->setFont(Theme::instance().codeFont());
    source_->setFont(Theme::instance().codeFont());
    highlighter_->rehighlight();
    highlightSourceLine();
    showPath();
  });
  // The options the restored tab shows, before any type is read.
  updateActions();
  reload();
}

void LocalTypesView::focusFilter() {
  filter_->setFocus();
  filter_->selectAll();
}

int LocalTypesView::page() const { return tabs_->currentIndex(); }

void LocalTypesView::setPage(int page) {
  tabs_->setCurrentIndex(std::clamp(page, 0, tabs_->count() - 1));
}

QString LocalTypesView::language() const {
  const int index = tabs_->currentIndex() - 1;
  return index >= 0 && index < int(std::size(Languages))
             ? QString::fromLatin1(Languages[index].id)
             : QStringLiteral("c");
}

QString LocalTypesView::spelling(const QString &language) const {
  return spellings_.value(
      QString::fromLatin1(spellingChoice(language).language));
}

void LocalTypesView::showSpellings() {
  const SpellingChoice &choice =
      spellingChoice(tabs_->currentIndex() ? language() : QString());
  const QSignalBlocker blocker(spelling_);
  spelling_->clear();
  for (const auto &entry : choice.spellings)
    spelling_->addItem(tr(entry.title), QString::fromLatin1(entry.id));
  const QString key = QString::fromLatin1(choice.language);
  spelling_->setCurrentIndex(
      std::max(spelling_->findData(spellings_.value(key)), 0));
  // A choice no longer offered falls back to the first.
  spellings_.insert(key, spelling_->currentData().toString());
}

QString LocalTypesView::offsetText(qint64 offset, qint64 bit) const {
  QString text = hex_->isChecked() ? hex(offset) : QString::number(offset);
  if (bit >= 0)
    text += QLatin1Char('.') + QString::number(bit);
  return text;
}

QStringList LocalTypesView::selectedNames() const {
  QStringList names;
  for (const QModelIndex &index :
       list_->selectionModel()->selectedRows(NameColumn))
    names.append(index.data(NameRole).toString());
  return names;
}

void LocalTypesView::reload() {
  const quint64 serial = ++listSerial_;
  if (!session_.loaded() || !session_.listsLocalTypes()) {
    lastList_ = {};
    model_->removeRows(0, model_->rowCount());
    names_.clear();
    highlighter_->setTypeNames(names_);
    current_.clear();
    type_ = {};
    history_.clear();
    layout_->clear();
    source_->clear();
    heading_->clear();
    summary_->setText(session_.loaded() ? tr("This engine has no local types.")
                                        : QString());
    updateActions();
    return;
  }
  summary_->setText(tr("Reading the program's types…"));
  session_.read(
      QStringLiteral("local_types"), {}, this,
      [this, serial](const QJsonObject &payload) {
        if (serial == listSerial_)
          listed(payload);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == listSerial_)
          summary_->setText(tr("Cannot read the types: %1").arg(message));
      });
}

void LocalTypesView::listed(const QJsonObject &payload) {
  lastList_ = payload;
  const QString keep = current_;
  model_->removeRows(0, model_->rowCount());
  names_.clear();
  cards_.clear();
  const QString origin = origin_->currentData().toString();
  const QJsonArray types = payload.value("types").toArray();
  qint64 fromPdb = 0, fromHeaders = 0, declared = 0;
  for (const QJsonValue &value : types) {
    const QJsonObject type = value.toObject();
    const QString name = type.value("name").toString();
    // Every name stays known: a member's type opens whatever its origin.
    names_.insert(name);
    const QString from = type.value("origin").toString();
    fromPdb += from == QLatin1String("pdb");
    fromHeaders += from == QLatin1String("header");
    declared += from == QLatin1String("user");
    if (!origin.isEmpty() && from != origin)
      continue;
    QList<QStandardItem *> row;
    const auto cell = [&](const QString &text, const QVariant &sort) {
      auto *item = new QStandardItem(text);
      item->setEditable(false);
      item->setData(sort, SortRole);
      item->setData(name, NameRole);
      row.append(item);
    };
    const qint64 ordinal = type.value("ordinal").toInteger();
    cell(QString::number(ordinal), ordinal);
    cell(name, name.toLower());
    const QString kind = type.value("kind").toString();
    const KindEntry *entry = kindEntry(kind);
    if (entry)
      row[NameColumn]->setIcon(icon(QString::fromLatin1(entry->icon)));
    cell(kind, kind);
    // What hovering the type shows, here and in the source: what it is,
    // how big, and where it comes from.
    QString card = QStringLiteral("<b>%1</b> %2")
                       .arg(name.toHtmlEscaped(),
                            entry ? tr(entry->title) : kind.toHtmlEscaped());
    QStringList facts;
    if (type.contains("size")) {
      const qint64 size = type.value("size").toInteger();
      facts << tr("%1 (%2) bytes").arg(hex(size)).arg(size);
    }
    if (type.contains("members"))
      facts << tr("%n members", "", int(type.value("members").toInteger()));
    for (const auto &choice : Origins)
      if (*choice.id && from == QLatin1String(choice.id))
        facts << tr(choice.title);
    if (!facts.isEmpty())
      card += QStringLiteral("<br>") +
              facts.join(QStringLiteral(" · ")).toHtmlEscaped();
    row[NameColumn]->setToolTip(card);
    cards_.insert(name, card);
    if (type.contains("size")) {
      const qint64 size = type.value("size").toInteger();
      cell(hex(size), size);
    } else {
      cell(QStringLiteral("-"), qint64(-1));
    }
    if (type.contains("members")) {
      const qint64 members = type.value("members").toInteger();
      cell(QString::number(members), members);
    } else {
      cell(QString(), qint64(-1));
    }
    row[SizeColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row[MembersColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row[OrdinalColumn]->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    model_->appendRow(row);
  }
  highlighter_->setTypeNames(names_);
  for (int column : {OrdinalColumn, KindColumn, SizeColumn, MembersColumn})
    list_->resizeColumnToContents(column);
  const QString source = payload.value("source").toString();
  if (types.isEmpty())
    summary_->setText(source == QLatin1String("pdb")
                          ? tr("The PDB defines no types.")
                          : tr("No local types: this program has no PDB "
                               "with types."));
  else {
    // How many, from where: the program's PDB, the system's headers, the
    // user's declarations.
    QStringList parts{
        tr("Types: %1").arg(QLocale().toString(qlonglong(types.size())))};
    if (fromPdb)
      parts << tr("%n from the PDB", "", int(fromPdb));
    if (fromHeaders)
      parts << tr("%n from system headers", "", int(fromHeaders));
    if (declared)
      parts << tr("%n declared", "", int(declared));
    parts << payload.value("dataModel").toString();
    summary_->setText(parts.join(QStringLiteral(" · ")));
  }
  const QJsonArray mismatches =
      payload.value("stats").toObject().value("nestingMismatches").toArray();
  if (!mismatches.isEmpty())
    summary_->setToolTip(
        tr("Types that do not lay out as the PDB lists them: %1")
            .arg(mismatches.toVariantList().size()));
  const QJsonArray problems = payload.value("declarationProblems").toArray();
  if (!problems.isEmpty()) {
    const QJsonObject first = problems.first().toObject();
    note_ = (problems.size() == 1
                 ? tr("A declaration of yours no longer reads: %1")
                 : tr("%n declarations of yours no longer read: %1", "",
                      int(problems.size())))
                .arg(first.value("message").toString());
  }
  if (!note_.isEmpty())
    summary_->setText(summary_->text() + QLatin1Char('\n') + note_);
  note_.clear();
  // The type just declared shows; else the one shown before stays, laid out
  // anew; else the first.
  current_.clear();
  const QString next = std::exchange(showNext_, QString());
  // The type shown before stays only where the origin shown holds it.
  const bool listsKeep =
      !keep.isEmpty() && !model_
                              ->match(model_->index(0, NameColumn), NameRole,
                                      keep, 1, Qt::MatchExactly)
                              .isEmpty();
  if (!next.isEmpty() && names_.contains(next))
    open(next, !keep.isEmpty());
  else if (listsKeep)
    open(keep, false);
  else if (proxy_->rowCount() > 0)
    list_->setCurrentIndex(proxy_->index(0, NameColumn));
  else {
    // No type to show: what would show one.
    laidOut({});
    source_->clear();
  }
  updateActions();
}

void LocalTypesView::selectionChanged() {
  // Deleting takes the selection, whatever row is current.
  updateActions();
  const QModelIndex index = list_->currentIndex();
  if (!index.isValid())
    return;
  const QString name = index.data(NameRole).toString();
  const bool remember = rememberNext_;
  rememberNext_ = true;
  if (name == current_)
    return;
  if (remember && !current_.isEmpty())
    history_.append(current_);
  current_ = name;
  requestLayout();
  requestSource();
  updateActions();
}

void LocalTypesView::showType(const QString &name) {
  if (names_.contains(name)) {
    open(name, true);
    return;
  }
  // A type the list has not read yet shows once it has.
  if (lastList_.isEmpty()) {
    showNext_ = name;
    return;
  }
  summary_->setText(tr("No local type is named %1").arg(name));
}

void LocalTypesView::open(const QString &name, bool remember) {
  auto rows = model_->match(model_->index(0, NameColumn), NameRole, name, 1,
                            Qt::MatchExactly);
  // A type of another origin than the one shown shows every origin.
  if (rows.isEmpty() && names_.contains(name) && origin_->currentIndex()) {
    origin_->setCurrentIndex(0);
    rows = model_->match(model_->index(0, NameColumn), NameRole, name, 1,
                         Qt::MatchExactly);
  }
  if (rows.isEmpty())
    return;
  // Show the type even where the filter hides it.
  if (!proxy_->mapFromSource(rows.first()).isValid())
    filter_->clear();
  const QModelIndex index = proxy_->mapFromSource(rows.first());
  rememberNext_ = remember;
  list_->selectionModel()->setCurrentIndex(
      index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
  rememberNext_ = true;
  list_->scrollTo(index);
}

void LocalTypesView::back() {
  while (!history_.isEmpty()) {
    const QString previous = history_.takeLast();
    if (names_.contains(previous)) {
      open(previous, false);
      break;
    }
  }
  updateActions();
}

void LocalTypesView::requestLayout() {
  if (current_.isEmpty())
    return;
  const quint64 serial = ++layoutSerial_;
  session_.read(
      QStringLiteral("local_type"),
      {{"name", current_}, {"spelling", spelling(QStringLiteral("c"))}}, this,
      [this, serial](const QJsonObject &payload) {
        if (serial == layoutSerial_)
          laidOut(payload);
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == layoutSerial_)
          heading_->setText(tr("Cannot lay the type out: %1").arg(message));
      });
}

void LocalTypesView::requestSource() {
  if (current_.isEmpty() || tabs_->currentIndex() == 0)
    return;
  const quint64 serial = ++sourceSerial_;
  QJsonObject options{{"language", language()},
                      {"hexOffsets", hex_->isChecked()},
                      {"asserts", asserts_->isChecked()},
                      {"dependencies", dependencies_->isChecked()},
                      {"types", QJsonArray{current_}}};
  if (const QString chosen = spelling(language()); !chosen.isEmpty())
    options.insert("spelling", chosen);
  session_.read(
      QStringLiteral("local_types_source"), options, this,
      [this, serial](const QJsonObject &payload) {
        if (serial != sourceSerial_)
          return;
        const int scroll = source_->verticalScrollBar()->value();
        source_->setPlainText(payload.value("source").toString());
        source_->verticalScrollBar()->setValue(scroll);
        // The member selected in the layout shows where it is declared.
        showMemberInSource();
        highlightSourceLine();
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == sourceSerial_)
          source_->setPlainText(
              tr("// Cannot render the type: %1").arg(message));
      });
}

void LocalTypesView::laidOut(const QJsonObject &type) {
  // The member selected stays selected when the same type is laid out anew.
  const QStringList selected =
      type.value("name").toString() == type_.value("name").toString()
          ? memberPath_
          : QStringList();
  type_ = type;
  layout_->clear();
  memberPath_.clear();
  if (type.isEmpty()) {
    // Where a type would be, what shows one.
    heading_->setText(
        QStringLiteral("<span style=\"color:%1\">%2</span>")
            .arg(Theme::instance().color(ColorRole::PrefixUnmapped).name(),
                 tr("Select a type to see its layout and source")
                     .toHtmlEscaped()));
    return;
  }
  const KindEntry *kind = kindEntry(type.value("kind").toString());
  QString heading = QStringLiteral("<b>%1</b> %2")
                        .arg(type.value("name").toString().toHtmlEscaped(),
                             kind ? tr(kind->title) : QString());
  if (type.contains("size")) {
    const qint64 size = type.value("size").toInteger();
    heading +=
        QStringLiteral(" · ") + tr("%1 (%2) bytes").arg(hex(size)).arg(size);
  }
  if (const qint64 align = type.value("alignment").toInteger())
    heading += QStringLiteral(" · ") + tr("aligned to %1").arg(align);
  if (const qint64 pack = type.value("pack").toInteger())
    heading += QStringLiteral(" · ") + tr("packed to %1").arg(pack);
  if (type.value("defined").isBool() && !type.value("defined").toBool())
    heading += QStringLiteral(" · ") + tr("declared only");
  heading_->setText(heading);

  // An enumeration lists values where a record lists offsets, and a
  // typedef or function type the one type it names.
  const bool record = type.contains("members");
  const bool enumeration = type.contains("values");
  if (enumeration)
    layout_->setHeaderLabels(
        {tr("Value"), QString(), tr("Enumerator"), tr("Type")});
  else
    layout_->setHeaderLabels(
        {tr("Offset"), tr("Size"), tr("Member"), tr("Type")});
  layout_->setColumnHidden(OffsetColumn, !record && !enumeration);
  layout_->setColumnHidden(SizeOfColumn, !record);
  if (record) {
    addMembers(nullptr, type.value("members").toArray());
  } else if (type.contains("values")) {
    for (const QJsonValue &value : type.value("values").toArray()) {
      const QJsonObject entry = value.toObject();
      auto *item = new QTreeWidgetItem(layout_);
      const QJsonValue number = entry.value("value");
      const qint64 raw = number.toInteger();
      item->setText(OffsetColumn, hex_->isChecked() && raw >= 0
                                      ? hex(raw)
                                      : QString::number(raw));
      item->setText(MemberColumn, entry.value("name").toString());
      item->setText(TypeColumn, type.value("underlying").toString());
    }
  } else {
    auto *item = new QTreeWidgetItem(layout_);
    item->setText(MemberColumn, type.value("name").toString());
    item->setText(TypeColumn, type.contains("target")
                                  ? type.value("target").toString()
                                  : type.value("declaration").toString());
    if (const QString refers = type.value("refers").toString();
        !refers.isEmpty())
      item->setData(0, RefersRole, refers);
  }
  layout_->expandAll();
  fitLayoutColumns();
  // The member selected before, found again by its names.
  if (!selected.isEmpty()) {
    QTreeWidgetItem *found = nullptr;
    std::function<void(QTreeWidgetItem *, int)> find =
        [&](QTreeWidgetItem *item, int depth) {
          if (found || depth >= selected.size() ||
              item->text(MemberColumn) != selected[depth])
            return;
          if (depth + 1 == selected.size()) {
            found = item;
            return;
          }
          for (int child = 0; child < item->childCount(); ++child)
            find(item->child(child), depth + 1);
        };
    for (int row = 0; row < layout_->topLevelItemCount() && !found; ++row)
      find(layout_->topLevelItem(row), 0);
    if (found)
      layout_->setCurrentItem(found);
  }
  showPath();
}

void LocalTypesView::showPath() {
  QTreeWidgetItem *item = layout_->currentItem();
  memberPath_.clear();
  if (!item || type_.isEmpty()) {
    path_->setVisible(false);
    return;
  }
  // The records that hold the member, outermost first, each with the rows
  // that lead to it.
  std::vector<QTreeWidgetItem *> chain;
  for (QTreeWidgetItem *at = item; at; at = at->parent())
    chain.insert(chain.begin(), at);
  const QString separator = QStringLiteral(" › ");
  const QColor muted = Theme::instance().color(ColorRole::PrefixUnmapped);
  QString text = QStringLiteral("<a href=\"\" style=\"text-decoration:none\">"
                                "%1</a>")
                     .arg(type_.value("name").toString().toHtmlEscaped());
  QString rows;
  for (QTreeWidgetItem *at : chain) {
    QTreeWidgetItem *parent = at->parent();
    const int row = parent ? parent->indexOfChild(at)
                           : layout_->indexOfTopLevelItem(at);
    rows += QString::number(row) + QLatin1Char('/');
    memberPath_ << at->text(MemberColumn);
    text += QStringLiteral("<span style=\"color:%1\">%2</span>")
                .arg(muted.name(), separator.toHtmlEscaped());
    if (at == item)
      text += QStringLiteral("<b>%1</b>")
                  .arg(at->text(MemberColumn).toHtmlEscaped());
    else
      text += QStringLiteral("<a href=\"%1\" style=\"text-decoration:none\">"
                             "%2</a>")
                  .arg(rows, at->text(MemberColumn).toHtmlEscaped());
  }
  // Where the member lies, as its row says.
  const QString offset = item->text(OffsetColumn);
  if (!offset.isEmpty())
    text += QStringLiteral("<span style=\"color:%1\"> · %2</span>")
                .arg(muted.name(), offset.toHtmlEscaped());
  path_->setText(text);
  path_->setVisible(tabs_->currentIndex() == 0 || !memberPath_.isEmpty());
  if (tabs_->currentIndex() > 0)
    showMemberInSource();
}

void LocalTypesView::highlightSourceLine() {
  QTextEdit::ExtraSelection line;
  line.format.setBackground(
      Theme::instance().color(ColorRole::ListingCurrentLine));
  line.format.setProperty(QTextFormat::FullWidthSelection, true);
  line.cursor = source_->textCursor();
  line.cursor.clearSelection();
  source_->setExtraSelections({line});
}

void LocalTypesView::showMemberInSource() {
  if (memberPath_.isEmpty() || tabs_->currentIndex() == 0 ||
      source_->document()->isEmpty())
    return;
  // The declared name of the member: an anonymous record's line opens
  // with its keyword, which names nothing.
  QTreeWidgetItem *item = layout_->currentItem();
  const QString member = item ? item->data(0, DeclaredRole).toString() : "";
  if (member.isEmpty())
    return;
  QTextDocument *document = source_->document();
  // The member's line follows the line that opens its type's definition,
  // past the types the source declares or defines before it.
  const QString type = QRegularExpression::escape(current_);
  QTextCursor from(document);
  const QTextCursor opens = document->find(
      QRegularExpression(QStringLiteral("\\b(struct|union|class|type|enum)\\s+"
                                        "%1\\b(?![^;]*;\\s*$)")
                             .arg(type)),
      from);
  if (!opens.isNull())
    from = opens;
  // How each language declares a member: C's declarator and bit-field,
  // Rust's and Go's fields and the methods that read what they cannot
  // hold as one, Python's field list, JavaScript's getters, Lua's keys.
  const QString name = QRegularExpression::escape(member);
  const QRegularExpression declares(
      QStringLiteral("(\\b%1\\s*(\\[[^\\]]*\\]\\s*)*(:\\s*\\d+\\s*)?;)|"
                     "(\\bpub\\s+%1\\s*:)|(\\bfn\\s+%1\\s*\\()|"
                     "(^\\s*%1\\s)|(\\)\\s*%1\\s*\\()|(\\(\\s*\"%1\")|"
                     "(\\bget\\s+%1\\s*\\()|(\\b%1\\s*=)")
          .arg(name));
  QTextCursor found = document->find(declares, from);
  if (found.isNull())
    found = document->find(
        QRegularExpression(QStringLiteral("\\b%1\\b").arg(name)), from);
  if (found.isNull())
    return;
  QTextCursor cursor(found.block());
  source_->setTextCursor(cursor);
  source_->centerCursor();
}

QString LocalTypesView::typeAt(const QPoint &position) const {
  QTextCursor cursor = source_->cursorForPosition(position);
  cursor.select(QTextCursor::WordUnderCursor);
  if (!cursor.hasSelection())
    return {};
  // A point past the word, as past a line's end, is on no word.
  QTextCursor start(cursor), end(cursor);
  start.setPosition(cursor.selectionStart());
  end.setPosition(cursor.selectionEnd());
  const int left = source_->cursorRect(start).left();
  const int right = source_->cursorRect(end).right();
  if (position.x() < left || position.x() > right)
    return {};
  const QString type = highlighter_->typeOf(cursor.selectedText());
  return names_.contains(type) ? type : QString();
}

void LocalTypesView::fitLayoutColumns() {
  for (int column : {OffsetColumn, SizeOfColumn, MemberColumn})
    layout_->resizeColumnToContents(column);
  // Long nested names may not push the types out of sight, measured where
  // the layout shows: hidden behind a source tab, it has no width to share.
  if (!layout_->isVisible())
    return;
  const int widest = layout_->viewport()->width() * 2 / 5;
  if (layout_->columnWidth(MemberColumn) > widest && widest > 0)
    layout_->setColumnWidth(MemberColumn, widest);
}

void LocalTypesView::addMembers(QTreeWidgetItem *parent,
                                const QJsonArray &members,
                                const QString &path) {
  const auto &theme = Theme::instance();
  for (const QJsonValue &value : members) {
    const QJsonObject member = value.toObject();
    const QString kind = member.value("kind").toString();
    auto *item =
        parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(layout_);
    const qint64 offset = member.value("offset").toInteger();
    const qint64 size = member.value("size").toInteger();
    if (kind == QLatin1String("bitfield")) {
      const qint64 storage = member.value("storageOffset").toInteger();
      const qint64 bit = member.value("bitOffset").toInteger() - storage * 8;
      const qint64 width = member.value("bitWidth").toInteger();
      item->setText(OffsetColumn, offsetText(storage, bit));
      // A bitfield's width as C declares it.
      item->setText(SizeOfColumn, QStringLiteral(":%1").arg(width));
    } else {
      item->setText(OffsetColumn, offsetText(offset));
      item->setText(SizeOfColumn, offsetText(size));
    }
    item->setTextAlignment(OffsetColumn, Qt::AlignRight | Qt::AlignVCenter);
    item->setTextAlignment(SizeOfColumn, Qt::AlignRight | Qt::AlignVCenter);
    const QString name = member.value("name").toString();
    if (kind == QLatin1String("padding")) {
      item->setText(MemberColumn, tr("(padding)"));
      for (int column = 0; column < LayoutColumns; ++column)
        item->setForeground(column, theme.color(ColorRole::PrefixUnmapped));
      continue;
    }
    // Where the member lies and what it is, as C reaches it: through the
    // records that hold it, anonymous ones adding no name.
    const QString reached =
        name.isEmpty() ? path
                       : (path.isEmpty() ? name : path + QLatin1Char('.') + name);
    const auto card = [&] {
      QString text = QStringLiteral("<b>%1</b>").arg(
          (reached.isEmpty() ? kind : reached).toHtmlEscaped());
      text += QStringLiteral("<br>") +
              (kind == QLatin1String("bitfield")
                   ? tr("bits %1 of the unit at %2")
                         .arg(item->text(SizeOfColumn).mid(1),
                              item->text(OffsetColumn))
                   : tr("at %1, %2 bytes")
                         .arg(item->text(OffsetColumn),
                              item->text(SizeOfColumn)))
                  .toHtmlEscaped();
      if (const QString type = member.value("type").toString(); !type.isEmpty())
        text += QStringLiteral("<br><code>%1</code>").arg(type.toHtmlEscaped());
      item->setToolTip(MemberColumn, text);
    };
    if (kind == QLatin1String("struct") || kind == QLatin1String("union")) {
      // An anonymous record reads as the keyword that opens it in source.
      if (name.isEmpty()) {
        item->setText(MemberColumn, kind);
        item->setForeground(MemberColumn,
                            theme.color(ColorRole::ListingKeyword));
      } else {
        item->setText(MemberColumn, name);
        item->setData(0, DeclaredRole, name);
        item->setText(TypeColumn, member.value("type").toString());
        item->setForeground(TypeColumn, theme.color(ColorRole::ListingKeyword));
      }
      card();
      addMembers(item, member.value("members").toArray(), reached);
      continue;
    }
    item->setText(MemberColumn, name);
    item->setData(0, DeclaredRole, name);
    item->setText(TypeColumn, member.value("type").toString());
    card();
    if (const QString refers = member.value("refers").toString();
        !refers.isEmpty() && names_.contains(refers)) {
      item->setData(0, RefersRole, refers);
      item->setForeground(TypeColumn,
                          theme.color(ColorRole::ListingLibraryName));
      item->setToolTip(TypeColumn,
                       tr("Double-click or Ctrl+click to open %1").arg(refers));
    }
  }
}

void LocalTypesView::copy() {
  QString text;
  if (tabs_->currentIndex() == 0) {
    // The layout as a table, one member a line.
    std::function<void(QTreeWidgetItem *, int)> add = [&](QTreeWidgetItem *item,
                                                          int depth) {
      text += item->text(OffsetColumn) + QLatin1Char('\t') +
              item->text(SizeOfColumn) + QLatin1Char('\t') +
              QString(depth * 2, QLatin1Char(' ')) + item->text(MemberColumn) +
              QLatin1Char('\t') + item->text(TypeColumn) + QLatin1Char('\n');
      for (int child = 0; child < item->childCount(); ++child)
        add(item->child(child), depth + 1);
    };
    for (int row = 0; row < layout_->topLevelItemCount(); ++row)
      add(layout_->topLevelItem(row), 0);
  } else {
    text = source_->textCursor().hasSelection()
               ? source_->textCursor().selectedText().replace(
                     QChar::ParagraphSeparator, QLatin1Char('\n'))
               : source_->toPlainText();
  }
  QApplication::clipboard()->setText(text);
}

void LocalTypesView::exportTypes(const QStringList &names) {
  const int index = std::max(tabs_->currentIndex() - 1, 0);
  const SourceLanguage &target = Languages[index];
  const QString base = names.size() == 1
                           ? names.first()
                           : QFileInfo(session_.fileName()).completeBaseName() +
                                 QStringLiteral("_types");
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Export local types"),
      base + QLatin1Char('.') + QString::fromLatin1(target.suffix),
      tr(target.filter));
  if (path.isEmpty())
    return;
  QJsonObject options{{"language", QString::fromLatin1(target.id)},
                      {"hexOffsets", hex_->isChecked()},
                      {"asserts", asserts_->isChecked()}};
  if (const QString chosen = spelling(QString::fromLatin1(target.id));
      !chosen.isEmpty())
    options.insert("spelling", chosen);
  if (!names.isEmpty()) {
    options.insert("types", QJsonArray::fromStringList(names));
    options.insert("dependencies", true);
  }
  session_.read(
      QStringLiteral("local_types_source"), options, this,
      [this, path](const QJsonObject &payload) {
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly) ||
            file.write(payload.value("source").toString().toUtf8()) < 0 ||
            !file.commit())
          summary_->setText(tr("Cannot write %1").arg(path));
        else
          summary_->setText(tr("Exported to %1").arg(path));
      },
      [this](const QString &, const QString &message) {
        summary_->setText(tr("Cannot export the types: %1").arg(message));
      });
}

void LocalTypesView::updateActions() {
  const bool declares = session_.loaded() && session_.declaresLocalTypes();
  add_->setEnabled(declares);
  edit_->setEnabled(declares && !current_.isEmpty());
  delete_->setEnabled(declares && !selectedNames().isEmpty());
  back_->setEnabled(!history_.isEmpty());
  copy_->setEnabled(!current_.isEmpty());
  export_->setEnabled(model_->rowCount() > 0);
  // What only source takes is not there on the layout, rather than there
  // and greyed out.
  const bool source = tabs_->currentIndex() > 0;
  dependencies_->setVisible(source);
  asserts_->setVisible(source);
}

QKeySequence LocalTypesView::commandKey(TypesCommand command) {
#define NEVERD_IDA_TYPES_COMMAND(IdaAction, Shortcut, QtShortcut, Command)     \
  if (command == TypesCommand::Command)                                        \
    return QKeySequence(QStringLiteral(QtShortcut), QKeySequence::PortableText);
#include "IdaActions.def"
  return {};
}

std::optional<TypesCommand>
LocalTypesView::commandFor(const QKeySequence &key) const {
#define NEVERD_IDA_TYPES_COMMAND(IdaAction, Shortcut, QtShortcut, Command)     \
  if (key == commandKey(TypesCommand::Command))                                \
    return TypesCommand::Command;
#include "IdaActions.def"
  return std::nullopt;
}

void LocalTypesView::run(TypesCommand command) {
  if (!session_.loaded() || !session_.declaresLocalTypes())
    return;
  switch (command) {
  case TypesCommand::Add:
    openEditor(declaredIn(language()), QString(), tr("Add types"));
    return;
  case TypesCommand::Edit: {
    if (current_.isEmpty())
      return;
    // The type as the language shown declares it, or C where that language
    // renders only: alone and unchecked, since its checks would refuse the
    // change they were made before.
    const QString name = current_, lang = declaredIn(language());
    QJsonObject options{{"language", lang},
                        {"hexOffsets", hex_->isChecked()},
                        {"asserts", false},
                        {"dependencies", false},
                        {"types", QJsonArray{name}}};
    if (const QString chosen = spelling(lang); !chosen.isEmpty())
      options.insert("spelling", chosen);
    session_.read(
        QStringLiteral("local_types_source"), options, this,
        [this, name, lang](const QJsonObject &payload) {
          // The editor's declaration is a command, which waits for this
          // read's answer to finish: the editor opens after it.
          const QString source = payload.value("source").toString();
          QMetaObject::invokeMethod(
              this,
              [this, name, lang, source] {
                openEditor(lang, source, tr("Edit %1").arg(name));
              },
              Qt::QueuedConnection);
        },
        [this](const QString &, const QString &message) {
          summary_->setText(tr("Cannot read the type: %1").arg(message));
        });
    return;
  }
  case TypesCommand::Delete:
    deleteTypes(selectedNames());
    return;
  }
}

void LocalTypesView::openEditor(const QString &language, const QString &source,
                                const QString &title) {
  QPointer<LocalTypesView> view(this);
  DeclareTypesDialog dialog(
      language, source,
      [view](const QString &lang, const QString &text, auto answered) {
        if (!view) {
          answered({{"error", tr("The window closed")}});
          return;
        }
        view->session_.declareTypes(
            lang, text, answered,
            [answered](const QString &, const QString &message) {
              answered({{"error", message}});
            });
      },
      this);
  dialog.setWindowTitle(title);
  dialog.setTypeNames(names_);
  if (dialog.exec() != QDialog::Accepted || !view)
    return;
  const QStringList declared = dialog.declared();
  showNext_ = declared.value(0);
  note_ = declared.size() == 1
              ? tr("Declared %1").arg(declared.first())
              : tr("Declared %n types", "", int(declared.size()));
  reload();
}

void LocalTypesView::deleteTypes(const QStringList &names) {
  if (names.isEmpty())
    return;
  const QString question = names.size() == 1
                               ? tr("Delete the type %1?").arg(names.first())
                               : tr("Delete %n types?", "", int(names.size()));
  if (QMessageBox::question(this, tr("Delete types"), question,
                            QMessageBox::Yes | QMessageBox::No,
                            QMessageBox::No) != QMessageBox::Yes)
    return;
  session_.forgetTypes(
      names,
      [this](const QJsonObject &payload) {
        const QJsonArray removed = payload.value("removed").toArray();
        const QJsonArray usedBy = payload.value("usedBy").toArray();
        note_ = removed.size() == 1
                    ? tr("Deleted %1").arg(removed.first().toString())
                    : tr("Deleted %n types", "", int(removed.size()));
        if (!usedBy.isEmpty())
          note_ +=
              QLatin1Char(' ') +
              (usedBy.size() == 1
                   ? tr("(%1 still names it)").arg(usedBy.first().toString())
                   : tr("(%n types still name it)", "", int(usedBy.size())));
        reload();
      },
      [this](const QString &, const QString &message) {
        summary_->setText(tr("Cannot delete the types: %1").arg(message));
      });
}

bool LocalTypesView::eventFilter(QObject *object, QEvent *event) {
  if (!source_)
    return QWidget::eventFilter(object, event);
  // The window's own commands take their keys before window-wide ones
  // while a list or a type has the focus, as in IDA.
  if ((object == list_ || object == layout_ || object == source_) &&
      (event->type() == QEvent::ShortcutOverride ||
       event->type() == QEvent::KeyPress)) {
    const auto *pressed = static_cast<QKeyEvent *>(event);
    if (const auto command =
            commandFor(QKeySequence(pressed->keyCombination()))) {
      event->accept();
      if (event->type() == QEvent::KeyPress)
        run(*command);
      return true;
    }
  }
  if (event->type() == QEvent::KeyPress) {
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->matches(QKeySequence::Find)) {
      focusFilter();
      return true;
    }
    if (key->key() == Qt::Key_Escape && object != filter_ &&
        !history_.isEmpty()) {
      back();
      return true;
    }
    if (object == filter_ && key->key() == Qt::Key_Down) {
      list_->setFocus();
      return true;
    }
  }
  // Ctrl+click follows a member's type, as an editor follows a definition.
  if (object == layout_->viewport() &&
      event->type() == QEvent::MouseButtonRelease) {
    const auto *mouse = static_cast<QMouseEvent *>(event);
    QTreeWidgetItem *item = layout_->itemAt(mouse->position().toPoint());
    const QString refers =
        item ? item->data(0, RefersRole).toString() : QString();
    if (mouse->button() == Qt::LeftButton &&
        mouse->modifiers().testFlag(Qt::ControlModifier) &&
        !refers.isEmpty()) {
      open(refers, true);
      return true;
    }
  }
  if (object == source_->viewport()) {
    switch (event->type()) {
    case QEvent::ToolTip: {
      // Hovering a type's name shows what it is.
      const auto *help = static_cast<QHelpEvent *>(event);
      const QString type = typeAt(help->pos());
      if (type.isEmpty() || !cards_.contains(type)) {
        QToolTip::hideText();
        event->ignore();
        return true;
      }
      QToolTip::showText(
          help->globalPos(),
          cards_.value(type) + QStringLiteral("<br><i>%1</i>")
                                   .arg(tr("Ctrl+click to open")
                                            .toHtmlEscaped()),
          source_->viewport());
      return true;
    }
    case QEvent::MouseMove: {
      // Ctrl over a type's name makes it a link.
      const auto *mouse = static_cast<QMouseEvent *>(event);
      const bool link =
          mouse->modifiers().testFlag(Qt::ControlModifier) &&
          mouse->buttons() == Qt::NoButton &&
          !typeAt(mouse->position().toPoint()).isEmpty();
      source_->viewport()->setCursor(link ? Qt::PointingHandCursor
                                          : Qt::IBeamCursor);
      break;
    }
    case QEvent::MouseButtonPress: {
      // Ctrl+click on a type's name is a link's click, which selects no
      // text: the press is the link's.
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton &&
          mouse->modifiers().testFlag(Qt::ControlModifier)) {
        pressedType_ = typeAt(mouse->position().toPoint());
        if (!pressedType_.isEmpty())
          return true;
      }
      break;
    }
    case QEvent::MouseButtonRelease: {
      const auto *mouse = static_cast<QMouseEvent *>(event);
      if (mouse->button() == Qt::LeftButton && !pressedType_.isEmpty()) {
        // Released on the name it was pressed on, it opens.
        const QString pressed = std::exchange(pressedType_, QString());
        if (typeAt(mouse->position().toPoint()) == pressed &&
            pressed != current_) {
          source_->viewport()->setCursor(Qt::IBeamCursor);
          open(pressed, true);
        }
        return true;
      }
      break;
    }
    default:
      break;
    }
  }
  if (object == source_->viewport() &&
      event->type() == QEvent::MouseButtonDblClick) {
    auto *mouse = static_cast<QMouseEvent *>(event);
    QTextCursor cursor = source_->cursorForPosition(mouse->pos());
    cursor.select(QTextCursor::WordUnderCursor);
    const QString type = highlighter_->typeOf(cursor.selectedText());
    if (!type.isEmpty() && type != current_) {
      open(type, true);
      return true;
    }
  }
  return QWidget::eventFilter(object, event);
}

} // namespace neverd::gui
