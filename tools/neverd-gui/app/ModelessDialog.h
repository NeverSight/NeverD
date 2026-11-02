//===- ModelessDialog.h - Browsers owned by their source pane --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep modeless browsers usable as their source pane floats, docks or closes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_MODELESSDIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_MODELESSDIALOG_H

class QDialog;
class QWidget;

namespace neverd::gui {
class Session;

/// Show a heap-allocated dialog owned by a stable pane, not its temporary
/// floating window. Close it with the pane (or session, when supplied), and
/// replace a previous browser with the same object name in that pane.
void showModelessDialog(QDialog *dialog, QWidget *source,
                        Session *session = nullptr);
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_MODELESSDIALOG_H
