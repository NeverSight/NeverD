#pragma once

#include "Address.h"
#include "ChooserKind.h"

#include <QAbstractTableModel>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QSet>
#include <QTimer>
#include <QVector>
#include <QWidget>
#include <functional>
#include <list>
#include <optional>

class QLabel;
class QLineEdit;
class QTreeView;

namespace neverd::gui {

class AddressSpace;
class Session;

/// A command of IDA's list windows that a list window can run on its current
/// row by IDA's key (IdaActions.def).
enum class ChooserCommand {
#define NEVERD_IDA_CHOOSER_COMMAND(IdaAction, Shortcut, QtShortcut, Command)   \
  Command,
#include "IdaActions.def"
};

/// Rows of a worker table operation, fetched in pages on demand so a list of
/// a million functions costs only its visible pages.  Filtering and sorting
/// run in the worker.
class ChooserModel final : public QAbstractTableModel {
  Q_OBJECT
public:
  using Located = std::function<void(std::optional<int>, const QString &)>;
  /// How a column shows its field (ChooserFormats.def).
  enum class Format {
#define NEVERD_CHOOSER_FORMAT(Id, Columns) Id,
#include "ChooserFormats.def"
  };
  struct Column {
    QString field;
    const char *title;
    Format format;
    QString sortField;
  };

  ChooserModel(Session &session, const AddressSpace &space, ChooserKind kind,
               QObject *parent = nullptr);

  ChooserKind kind() const { return kind_; }
  const Column &column(int index) const { return columns_.at(index); }
  QString title() const;
  QString iconName() const;
  bool filterable() const;
  void setFilter(const QString &filter);
  QString filter() const { return filter_; }
  /// Extra request fields, such as the address and direction of references.
  void setRequest(const QJsonObject &payload);
  /// Rows supplied locally instead of by the worker (bookmarks).
  void setLocalRows(const QJsonArray &rows);
  void reload();
  /// Redraw the columns that name segments once the regions change.
  void addressSpaceChanged();
  int total() const { return total_; }
  bool loading() const {
    return refresh_.has_value() || !pageRequests_.isEmpty();
  }
  QJsonObject rowObject(int row) const;
  std::optional<Address> addressAt(int row) const;
  /// The item a row stands for when its cross references are listed: a
  /// string reference's string, otherwise the row's address.
  std::optional<Address> referenceAddressAt(int row) const;
  /// Fetch the row at an address, or its containing segment, in table order.
  void locateAddress(Address address, Located done);

  int rowCount(const QModelIndex &parent = {}) const override;
  int columnCount(const QModelIndex &parent = {}) const override;
  QVariant data(const QModelIndex &index, int role) const override;
  QVariant headerData(int section, Qt::Orientation orientation,
                      int role) const override;
  void sort(int column, Qt::SortOrder order) override;

signals:
  void totalChanged(int total);
  void failed(const QString &message);

private:
  /// Background discovery replaces cached pages together, keeping row
  /// identities and selections instead of emptying the table between reads.
  void refreshFunctions();
  void refreshNextPage(quint64 serial);
  void publishFunctionRefresh(quint64 serial);
  void seekRefreshAddress(quint64 serial, Address address,
                          const QSet<Address> &selected);
  struct Refresh {
    QVector<int> pending;
    QHash<int, QJsonArray> pages;
    int total = -1;
    int lookups = 0;
    QSet<Address> missing;
  };
  std::optional<Refresh> refresh_;
  std::optional<Address> itemAddress(const QJsonObject &object) const;
  QJsonObject pagePayload(int page) const;
  void finishLocation(std::optional<int> row, const QString &error = {});
  void requestPage(int page) const;
  void accept(int page, quint64 serial, const QJsonObject &payload);
  QString display(const QJsonObject &row, const Column &column) const;

  Session &session_;
  const AddressSpace &space_;
  ChooserKind kind_;
  QString operation_, filter_, sortField_;
  bool descending_ = false;
  QJsonObject request_;
  QVector<Column> columns_;
  mutable QHash<int, QJsonArray> pages_;
  mutable std::list<int> pageOrder_;
  mutable QHash<int, quint64> pageRequests_;
  mutable std::list<int> requestOrder_;
  QJsonArray local_;
  bool localRows_ = false;
  int total_ = 0;
  quint64 serial_ = 0;
  quint64 locationSerial_ = 0;
  Located locationDone_;
};

/// A list window: table, IDA-style "Line N of M" status and a quick filter.
class ChooserView final : public QWidget {
  Q_OBJECT
public:
  ChooserView(Session &session, const AddressSpace &space, ChooserKind kind,
              QWidget *parent = nullptr);
  ChooserModel &model() { return *model_; }
  QTreeView *table() const { return table_; }
  std::optional<Address> currentAddress() const;
  /// The current row's item for cross references (referenceAddressAt).
  std::optional<Address> referenceTarget() const;
  void focusFilter();
  /// Show the quick filter with \p text applied after the usual debounce.
  void setFilterText(const QString &text);
  /// Select the row whose address equals \p address among loaded rows.
  void revealAddress(Address address);
  /// Locate a link target, clearing a filter that could hide it.
  void locateAddress(Address address, ChooserModel::Located done);
  /// The selected rows, or else the current one, as text: a line a row in
  /// table order, its shown columns in header order separated by tabs.
  QString selectedText() const;
  /// Let the user run \p command on the current row by its key, which then
  /// wins over a window-wide command with the same key.
  void enableCommand(ChooserCommand command, bool enabled);
  /// IDA's key for \p command.
  static QKeySequence commandKey(ChooserCommand command);

signals:
  void activated(neverd::gui::Address address);
  void currentChanged(neverd::gui::Address address);
  /// The user ran \p command on the row of \p address.
  void commandRequested(neverd::gui::ChooserCommand command,
                        neverd::gui::Address address);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void updateStatus();
  /// The enabled command whose key \p key is.
  std::optional<ChooserCommand> commandFor(const QKeySequence &key) const;
  /// The enabled commands, one bit each.
  unsigned commands_ = 0;
  ChooserModel *model_;
  QTreeView *table_;
  QLineEdit *filter_;
  QLabel *status_;
  QTimer filterTimer_;
};

} // namespace neverd::gui
