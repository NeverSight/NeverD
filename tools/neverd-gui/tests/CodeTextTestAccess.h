//===- CodeTextTestAccess.h - Code view changes for tests ------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Same-build changes of a code view's state, as its own code makes them or
/// as none of it does.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_TESTS_CODETEXTTESTACCESS_H
#define NEVERD_TOOLS_NEVERD_GUI_TESTS_CODETEXTTESTACCESS_H

#include "CodeView.h"

namespace neverd::gui {
// The workbench never includes this test header.
struct CodeTextTestAccess {
  /// Highlights \p word without telling the view that what it shows
  /// changed, as none of the view's own code does: a change the view has to
  /// find by itself.
  static void highlightUnsaid(CodeText &view, const QString &word) {
    view.highlight_ = word;
  }
  /// Says that what the view shows changed, as the view's own code does
  /// after it changed something.
  static void changed(CodeText &view) { view.updateView(); }
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_CODETEXTTESTACCESS_H
