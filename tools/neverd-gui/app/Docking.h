#pragma once

#include <QMargins>

class QWidget;

namespace neverd::gui {

/// Selects the Qt Widgets docking front end and sets the workbench's docking
/// behavior and chrome.  Call once, after the application object exists and
/// before any dock or main window is created.
void configureDocking();

/// Add commands before the floating caption's drag area and window controls.
/// The caption takes ownership; reparent the toolbar to remove it again.
bool addFloatingWindowToolbar(QWidget &window, QWidget &toolbar);

/// The margins around a main window's dock area.
QMargins dockAreaMargins();

} // namespace neverd::gui
