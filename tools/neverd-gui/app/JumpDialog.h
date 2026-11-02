#pragma once

#include "Address.h"

#include <QDialog>
#include <QTimer>
#include <optional>

class QLineEdit;
class QListWidget;
class QLabel;

namespace neverd::gui {

class AddressSpace;
class ListingView;
class Session;

/// "Jump anywhere": an address, expression or name with live name matches,
/// the recent jump history and a preview of the place Enter goes to.
class JumpDialog final : public QDialog {
  Q_OBJECT
public:
  /// \p here is the address `.` and `here` stand for.
  JumpDialog(Session &session, const AddressSpace &space,
             std::optional<Address> here, QWidget *parent = nullptr);
  /// The expression to evaluate, or a chosen match's address.
  QString result() const { return result_; }
  static void remember(const QString &text);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void showRecent();
  void search();
  /// Show where Enter goes: the chosen match, else the typed expression.
  void updatePreview();
  void accept() override;
  Session &session_;
  QLineEdit *input_;
  QListWidget *matches_;
  QLabel *hint_;
  ListingView *preview_;
  std::optional<Address> here_;
  QTimer debounce_;
  QString result_;
  quint64 serial_ = 0;
  quint64 previewSerial_ = 0;
};

} // namespace neverd::gui
