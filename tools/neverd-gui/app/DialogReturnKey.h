#pragma once

class QApplication;
class QDialog;
class QObject;

namespace neverd::gui {

/// Return and Enter on a check box, radio button or list of choices (a combo
/// box that takes no typing) of a dialog press the dialog's default button,
/// as in IDA and on Windows, macOS and KDE; Space still toggles the focused
/// button or opens the list.  Qt's GNOME and other Linux platform themes make
/// Return press the focused control instead, so after a mnemonic checked a
/// box (Alt+O for "Does not return"), Return cleared it again and left the
/// dialog open, and Return on a chosen theme opened its list again.  Returns
/// the filter, which \p app owns.
QObject *acceptDialogsOnReturn(QApplication &app);

/// Leaves \p dialog without a default button: a window of tools, in which no
/// one step comes next.  Qt otherwise makes the first push button the default
/// as the dialog shows, and each one the keyboard reaches after that, draws
/// it in the accent and presses it for Return in any field: "Enable sharing"
/// from the server path of MCP connections, the file chooser behind "Import
/// plugin…" from the plugin filter, Close from the shortcut filter.  Return
/// then presses the button that holds the focus and otherwise stays with the
/// control it was typed in, which acts on it or not; the dialog swallows
/// what is left, where QDialog would press the default button of a child
/// window that happens to be open.  Covers the push buttons the dialog has
/// each time it shows.
void leaveWithoutDefaultButton(QDialog &dialog);

} // namespace neverd::gui
