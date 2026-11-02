#pragma once

#include "Address.h"

#include <QStringList>
#include <QWidget>
#include <functional>

class QLineEdit;
class QMenu;
class QPlainTextEdit;
class QToolButton;

namespace neverd::gui {

class Session;

/// The output window: engine and workbench messages above a command line
/// that evaluates expressions and runs workbench commands.
class OutputWindow final : public QWidget {
  Q_OBJECT
public:
  explicit OutputWindow(Session &session, QWidget *parent = nullptr);

  void append(const QString &text, int level = 0);
  /// The log's context menu, which the caller owns: Copy, Select all and
  /// Clear.
  QMenu *createLogMenu();
  void focusCommandLine();
  /// Run one command line as if typed (echoed to the log).
  void run(const QString &line);
  /// Supplies the current address for `.`/`here` and command defaults.
  void setLocationProvider(std::function<std::optional<Address>()> provider) {
    location_ = std::move(provider);
  }

signals:
  void navigateRequested(neverd::gui::Address address);
  void commandRequested(const QString &command, const QString &argument);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void execute(const QString &line);
  void evaluate(const QString &text,
                std::function<void(std::optional<Address>, QString)> done);
  void printHelp();
  /// Color every line again for its level in the current theme.
  void recolor();
  Session &session_;
  QPlainTextEdit *log_;
  QLineEdit *command_;
  QToolButton *language_;
  QStringList history_;
  int historyIndex_ = 0;
  /// Whether the log's lines are colored for the dark theme.
  bool coloredDark_ = false;
  std::function<std::optional<Address>()> location_;
};

} // namespace neverd::gui
