//===- WindowActivation.h - Handing the focus to another workbench -*- C++ -*-//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A Wayland compositor does not let a process take the focus on its own: a
/// window raised from another process only asks for attention. The window that
/// has the focus asks the compositor for a token, the other process activates
/// its window with it, and the focus moves. Elsewhere there is no token, and
/// these do nothing that matters.
//===----------------------------------------------------------------------===//

#pragma once

#include <QString>
#include <functional>

class QObject;
class QWindow;

namespace neverd::gui {

/// Calls \p done with a token that lets another process take the focus from
/// \p window, or with nothing where the platform has none.  \p context bounds
/// the call, which may come later.
void requestActivationToken(QWindow *window, QObject *context,
                            std::function<void(const QString &token)> done);

/// Lets this process's next window activation use \p token.
void useActivationToken(const QString &token);

} // namespace neverd::gui
