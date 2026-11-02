#pragma once

#include "Address.h"

#include <QWidget>
#include <optional>
#include <vector>

class QLabel;
class QStackedWidget;

namespace neverd::gui {

class AddressSpace;
class GraphView;
class ListingView;
class Session;

/// A disassembly view ("NeverD View-A"): the text listing and the function
/// graph over one location, a status line, and the view's navigation history.
class DisassemblyView final : public QWidget {
  Q_OBJECT
public:
  DisassemblyView(Session &session, const AddressSpace &space,
                  QWidget *parent = nullptr);

  ListingView *listing() const { return listing_; }
  GraphView *graph() const { return graph_; }
  bool graphMode() const;
  void setGraphMode(bool graph);
  /// Navigate, recording the previous location for Jump to previous position.
  /// A code window can supply its own origin when it is pinned elsewhere.
  void navigate(Address address, bool record = true,
                std::optional<Address> from = {});
  /// Go \p steps stops back or forward at once, as picking a stop from the
  /// Back or Forward button's list does.
  void goBack(std::size_t steps = 1);
  void goForward(std::size_t steps = 1);
  bool canGoBack() const { return historyIndex_ > 0; }
  bool canGoForward() const { return historyIndex_ + 1 < history_.size(); }
  /// The stops Back, or Forward, reaches, nearest first.
  std::vector<Address> stops(bool forward) const;

  std::optional<Address> currentAddress() const;
  std::optional<Address> currentItem() const;
  std::optional<Address> currentFunction() const;
  QString currentFunctionName() const;
  std::optional<Address> operandTarget() const;
  /// The instruction operand the cursor is on (ListingView::currentOperand);
  /// none in the graph.
  std::optional<int> currentOperand() const;
  /// Whether the cursor is on an instruction's own line, as every graph row
  /// is, or on a data item's, rather than a name or comment line around it.
  bool onInstruction() const;
  bool onData() const;
  QString currentToken() const;
  QString selectedText() const;
  /// What Copy takes in the listing or the graph shown.
  QString copyText() const;
  void setSyncName(const QString &name) {
    syncName_ = name;
    updateStatus();
  }
  /// Show segment names and file offsets of the regions that just arrived.
  void addressSpaceChanged();
  void focusContent();

signals:
  void locationChanged(neverd::gui::Address address);
  void navigateRequested(neverd::gui::Address target);
  void contextMenuRequested(const QPoint &globalPosition);
  void historyChanged();
  /// The view switched between text and graph.
  void graphModeChanged(bool graph);

private:
  void updateStatus();
  void remember(Address address);
  /// Take the current history stop to the cursor, as IDA's history does: a
  /// move since the jump that made the stop does not add one of its own.
  void moveStopToCursor();
  /// Go to the stop at \p index of the history, leaving the stops on both
  /// sides of it.
  void goToStop(std::size_t index);
  Session &session_;
  const AddressSpace &space_;
  QStackedWidget *stack_;
  ListingView *listing_;
  GraphView *graph_;
  QLabel *status_;
  QString syncName_;
  std::vector<Address> history_;
  std::size_t historyIndex_ = 0;
  bool restoring_ = false;
  bool graphAfterJump_ = false;
  QMetaObject::Connection graphNavigation_;
  QMetaObject::Connection graphAfterJumpConnection_;
};

} // namespace neverd::gui
