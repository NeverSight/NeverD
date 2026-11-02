//===- ViewSearch.h - Literal view search ---------------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared literal matching for the find dialog and both text renderers.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_VIEWSEARCH_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_VIEWSEARCH_H

#include <QString>
#include <QStringView>
#include <QVector>

namespace neverd::gui {

struct ViewSearchQuery {
  QString Text;
  Qt::CaseSensitivity Case = Qt::CaseInsensitive;
  bool WholeWords = false;

  QVector<int> matches(QStringView text) const;
};

} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_VIEWSEARCH_H
