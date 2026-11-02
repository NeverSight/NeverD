//===- ExportDataDialog.h - Selected byte export ------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Export a checked byte range from the current worker session.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_NEVERD_GUI_APP_EXPORTDATADIALOG_H
#define NEVERD_TOOLS_NEVERD_GUI_APP_EXPORTDATADIALOG_H

#include "Address.h"

#include <QByteArray>
#include <QDialog>
#include <QTimer>

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;

namespace neverd::gui {
class Session;

class ExportDataDialog final : public QDialog {
  Q_OBJECT
public:
  ExportDataDialog(Session &session, Address first, Address last,
                   QWidget *parent = nullptr);
  /// Writes only a completely read range, atomically replacing the target.
  bool saveToFile(const QString &path);

private:
  void scheduleRead();
  void readRange();
  void readNext();
  void fail(const QString &message);
  void updatePreview();
  QByteArray formatted(const QByteArray &data) const;

  Session &SessionRef;
  QLineEdit *Start, *End;
  QComboBox *Format;
  QPlainTextEdit *Preview;
  QLabel *Status;
  QPushButton *Copy, *Save;
  QTimer Debounce;
  QByteArray Data;
  Address First = 0;
  qsizetype Size = 0;
  quint64 Serial = 0, Epoch = 0;
  QString Revision;
  bool Ready = false;
};
} // namespace neverd::gui

#endif // NEVERD_TOOLS_NEVERD_GUI_APP_EXPORTDATADIALOG_H
