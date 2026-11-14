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
  /// Whether the view reads ahead of a rename's answer where its pages say
  /// what the rename makes of them.  Without that it makes the lines ready
  /// and shows nothing before the answer.
  static void setReadsAhead(CodeText &view, bool ahead) {
    view.showsAhead_ = ahead;
  }
  /// The pixels the view has asked its viewport to paint, all told.
  static quint64 pixelsAsked(const CodeText &view) { return view.pixelsAsked_; }
  /// The lines as the view shows them: what its rows paint.
  static QStringList shownLines(const CodeText &view) {
    QStringList lines;
    for (const auto &line : view.lines_)
      lines.append(line.styled.text);
    return lines;
  }
  /// The pages of the shown text, as the view took them.
  static QVector<QJsonObject> pages(const CodeText &view) {
    return view.pages_;
  }
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_TESTS_CODETEXTTESTACCESS_H
