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
  /// The first line whose spans are not what coloring the text afresh gives
  /// it, from the first line on with the comments its lines open and close,
  /// or -1: a line that took the spans of the one that read the same in its
  /// place is colored as one read afresh.  A line that shows a fold's
  /// summary is passed over where it says that its spans are not its text's
  /// alone.  For a text whose source names all came before its first line
  /// was colored.
  static int firstMiscolored(const CodeText &view) {
    bool comment = false;
    for (int at = 0; at < view.lines_.size(); ++at) {
      const auto &line = view.lines_[at];
      CodeText::Line again;
      again.styled.text = line.styled.text;
      view.highlightLine(again, comment);
      if (line.folded && line.coloredIn == CodeText::Line::ColoredOtherwise)
        continue;
      if (again.styled.spans != line.styled.spans)
        return at;
    }
    return -1;
  }
  /// Colors the lines of the shown text again, as a fold does or a page read
  /// again, the text having \p sourceName for its one source name, or none,
  /// and \p language for the language its page names.
  static void colorAgain(CodeText &view, const QString &sourceName,
                         const QString &language) {
    view.sourceNames_.clear();
    view.sourceNameIndex_.clear();
    if (!sourceName.isEmpty()) {
      view.sourceNames_.insert(sourceName,
                               {sourceName, sourceName, Address(0x1000)});
      view.sourceNameIndex_[sourceName.at(0)].append(sourceName);
    }
    view.pageLanguage_ = language;
    view.rebuildLines(false);
  }
  /// Says that what the view shows changed, as the view's own code does
  /// after it changed something.
  static void changed(CodeText &view) { view.updateView(); }
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_CODETEXTTESTACCESS_H
