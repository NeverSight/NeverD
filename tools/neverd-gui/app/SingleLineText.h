//===- SingleLineText.h - Single-line display copies ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Normalize explicit line breaks before width elision. Original metadata,
/// copied evidence and accessible text retain their complete values.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_APP_SINGLELINETEXT_H
#define NEVERD_GUI_APP_SINGLELINETEXT_H

#include <QChar>
#include <QString>
#include <initializer_list>

namespace neverd::gui {
inline QString singleLineDisplayText(QString Text) {
  for (const auto Break :
       {QChar('\r'), QChar('\n'), QChar(QChar::LineSeparator),
        QChar(QChar::ParagraphSeparator)})
    Text.replace(Break, QLatin1Char(' '));
  return Text;
}
} // namespace neverd::gui

#endif // NEVERD_GUI_APP_SINGLELINETEXT_H
