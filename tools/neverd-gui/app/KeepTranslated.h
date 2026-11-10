//===- KeepTranslated.h - Texts that follow the language ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep a view's texts in the interface language after it changes.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_KEEP_TRANSLATED_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_KEEP_TRANSLATED_H

#include <functional>

class QWidget;

namespace neverd::gui {

/// Runs \p apply, which sets \p owner's labels, tooltips and headings, now and
/// again whenever the interface language changes.  A view that writes its
/// texts only while it is built keeps the language it started in until the
/// program restarts.
void keepTranslated(QWidget &owner, std::function<void()> apply);

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_KEEP_TRANSLATED_H
