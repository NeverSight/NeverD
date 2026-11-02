//===- LinkOpener.h - Native URL events -----------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Route operating-system URL events through the same instance selection as
/// the command line, keeping the receiving window's event loop responsive.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_LINK_OPENER_H
#define NEVERD_GUI_APP_LINK_OPENER_H

#include <QObject>

namespace neverd::gui {

class MainWindow;
class Session;
struct DeepLink;

class LinkOpener final : public QObject {
public:
  LinkOpener(MainWindow &window, Session &session);

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  void openRecent(const QString &text, const DeepLink &link);
  MainWindow &window_;
  Session &session_;
};

} // namespace neverd::gui

#endif // NEVERD_GUI_APP_LINK_OPENER_H
