#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include <QStringList>
#include <QWidget>
#include <optional>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSortFilterProxyModel;
class QStackedWidget;
class QStandardItemModel;
class QTabBar;
class QToolButton;
class QTreeView;
class QTreeWidget;
class QTreeWidgetItem;

namespace neverd::gui {

class Session;
class TypeSourceHighlighter;

/// A command of IDA's Local Types window, by IDA's key (IdaActions.def).
enum class TypesCommand {
#define NEVERD_IDA_TYPES_COMMAND(IdaAction, Shortcut, QtShortcut, Command)     \
  Command,
#include "IdaActions.def"
};

/// IDA's Local Types window: every named type of the program, filtered by
/// name, each with an icon of its kind, and the selected one laid out
/// member by member -- offset, size, bit range and the padding between,
/// each with a bar of where it lies in the type -- or as the C, C++, Rust or
/// Go that declares it, to copy or export.  A breadcrumb above names the
/// records that hold the member selected, and the source shows its line.
/// Hovering a type's name shows what it is; a member's type, or a type
/// named in the source, opens on a double click or a Ctrl+click, as an
/// editor follows a definition; Back returns to the type before.  Types are
/// added and edited as C, C++, Rust or Go declarations, and deleted, by
/// IDA's keys.
class LocalTypesView final : public QWidget {
  Q_OBJECT
public:
  explicit LocalTypesView(Session &session, QWidget *parent = nullptr);
  void focusFilter();
  /// Shows the type named \p name, as following a member's type does.
  void showType(const QString &name);
  /// The type shown, empty for none.
  QString currentType() const { return current_; }
  /// The tab shown: 0 the layout, then C, C++, Rust and Go.
  int page() const;
  void setPage(int page);
  /// Add opens the editor empty, Edit with the type shown declared in the
  /// language shown (C from the layout), and Delete asks before removing
  /// the selected types.
  void run(TypesCommand command);
  static QKeySequence commandKey(TypesCommand command);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void reload();
  void listed(const QJsonObject &payload);
  void selectionChanged();
  void requestLayout();
  void requestSource();
  void laidOut(const QJsonObject &type);
  /// Sizes the layout's columns to what they hold, the member's within
  /// what the view leaves the types.
  void fitLayoutColumns();
  void addMembers(QTreeWidgetItem *parent, const QJsonArray &members,
                  const QString &path = QString());
  /// The breadcrumb of the member selected in the layout: the type, then
  /// each record that holds the member, each a link that selects it.
  void showPath();
  /// Highlights the source's current line.
  void highlightSourceLine();
  /// Puts the source's cursor on the line that declares the member selected
  /// in the layout, where the source shows one.
  void showMemberInSource();
  /// The listed type named at \p position of the source, empty for none.
  QString typeAt(const QPoint &position) const;
  void exportTypes(const QStringList &names);
  void copy();
  void back();
  void open(const QString &name, bool remember);
  QString language() const;
  /// How \p language names builtins as chosen: C's spelling serves the
  /// layout as well as C/C++.
  QString spelling(const QString &language) const;
  /// The spellings the tab shown chooses from, the chosen one current.
  void showSpellings();
  QString offsetText(qint64 offset, qint64 bit = -1) const;
  QStringList selectedNames() const;
  void updateActions();
  std::optional<TypesCommand> commandFor(const QKeySequence &key) const;
  void openEditor(const QString &language, const QString &source,
                  const QString &title);
  void deleteTypes(const QStringList &names);

  Session &session_;
  // Null until the constructor makes them: the event filter sees events of
  // the first ones before the last exist.
  QLineEdit *filter_ = nullptr;
  QComboBox *origin_ = nullptr;
  QLabel *summary_ = nullptr;
  QStandardItemModel *model_ = nullptr;
  QSortFilterProxyModel *proxy_ = nullptr;
  QTreeView *list_ = nullptr;
  QToolButton *back_ = nullptr;
  QLabel *heading_ = nullptr;
  QLabel *path_ = nullptr;
  QTabBar *tabs_ = nullptr;
  QStackedWidget *pages_ = nullptr;
  QTreeWidget *layout_ = nullptr;
  QPlainTextEdit *source_ = nullptr;
  TypeSourceHighlighter *highlighter_ = nullptr;
  QComboBox *spelling_ = nullptr;
  /// The spelling chosen for each language, by its id.
  QHash<QString, QString> spellings_;
  QCheckBox *hex_ = nullptr, *dependencies_ = nullptr, *asserts_ = nullptr;
  QToolButton *copy_ = nullptr, *export_ = nullptr;
  QToolButton *add_ = nullptr, *edit_ = nullptr, *delete_ = nullptr;
  QString current_;
  QJsonObject type_;
  /// The list the worker gave last, which the origin shown filters.
  QJsonObject lastList_;
  QStringList history_;
  QSet<QString> names_;
  /// What hovering each listed type shows, by its name.
  QHash<QString, QString> cards_;
  /// The member selected in the layout, from the type down, as the source
  /// names each; empty for none.
  QStringList memberPath_;
  /// The type named where Ctrl+click pressed in the source, which its
  /// release there opens.
  QString pressedType_;
  /// Serials of the last list, layout and source requests; older replies
  /// are dropped.
  quint64 listSerial_ = 0, layoutSerial_ = 0, sourceSerial_ = 0;
  /// Whether the next type shown puts the one before on the history.
  bool rememberNext_ = true;
  /// The type the next list shows, one just declared; and what the summary
  /// says of the last change.
  QString showNext_, note_;
};

} // namespace neverd::gui
