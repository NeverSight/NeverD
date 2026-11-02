#pragma once

class QApplication;

namespace neverd::gui::gnome {

/// Whether the workbench runs on a GNOME desktop (XDG_CURRENT_DESKTOP).
bool onGnome();

/// GNOME attaches a modal dialog to its parent window: the dialog cannot move
/// on its own, and dragging it drags the whole workbench.  These keep modal
/// dialogs free-standing on GNOME while Qt still blocks input to their
/// parents.  Elsewhere they do nothing.

/// Before the application object exists: a Wayland compositor is not told
/// which dialogs are modal (xdg-dialog-v1), so it places them over their
/// parent as plain child windows. WindowFrame selects non-native dialogs
/// on every desktop.
void prepareModalDialogs();

/// Once the application exists: under X11, modal dialogs take the utility
/// window type, which the window manager keeps above their parent without
/// attaching them.  Qt centers them over their parent.
void detachModalDialogs(QApplication &app);

} // namespace neverd::gui::gnome
