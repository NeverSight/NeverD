//===- ExportDataDialog.cpp - Selected byte export -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read complete, revision-consistent ranges before copying or saving bytes.
//===----------------------------------------------------------------------===//

#include "ExportDataDialog.h"

#include "../Config.h"
#include "../ResourceLimits.h"
#include "Session.h"
#include "Theme.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QVBoxLayout>
#include <algorithm>

namespace neverd::gui {
namespace {
enum Format { Hex, SpacedHex, String, HexArray, DecimalArray, Raw };
} // namespace

ExportDataDialog::ExportDataDialog(Session &session, Address first,
                                   Address last, QWidget *parent)
    : QDialog(parent), SessionRef(session) {
  setObjectName(QStringLiteral("exportDataDialog"));
  setWindowTitle(tr("Export data"));
  resize(::neverd::limits::gui_config::kExportDataDialogSize);
  setSizeGripEnabled(true);
  auto *layout = new QVBoxLayout(this);
  auto *form = new QFormLayout;
  Start = new QLineEdit(hexAddress(first), this);
  Start->setObjectName(QStringLiteral("exportStart"));
  End = new QLineEdit(hexAddress(last), this);
  End->setObjectName(QStringLiteral("exportEnd"));
  Format = new QComboBox(this);
  Format->setObjectName(QStringLiteral("exportFormat"));
  Format->addItems({tr("Hex string"), tr("Spaced hex string"),
                    tr("C string literal"), tr("C byte array (hex)"),
                    tr("C byte array (decimal)"), tr("Raw bytes (file only)")});
  form->addRow(tr("Start address:"), Start);
  form->addRow(tr("End address (inclusive):"), End);
  form->addRow(tr("Format:"), Format);
  layout->addLayout(form);
  Preview = new QPlainTextEdit(this);
  Preview->setObjectName(QStringLiteral("exportPreview"));
  Preview->setReadOnly(true);
  Preview->setFont(Theme::instance().codeFont());
  layout->addWidget(Preview, 1);
  Status = new QLabel(this);
  Status->setObjectName(QStringLiteral("exportStatus"));
  Status->setWordWrap(true);
  layout->addWidget(Status);
  auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
  Copy = buttons->addButton(tr("Copy"), QDialogButtonBox::ActionRole);
  Copy->setObjectName(QStringLiteral("exportCopy"));
  Save = buttons->addButton(tr("Save as..."), QDialogButtonBox::ActionRole);
  Save->setObjectName(QStringLiteral("exportSave"));
  layout->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(Copy, &QPushButton::clicked, this, [this] {
    if (Ready && Format->currentIndex() != Raw)
      QApplication::clipboard()->setText(QString::fromLatin1(formatted(Data)));
  });
  connect(Save, &QPushButton::clicked, this, [this] {
    const auto path = QFileDialog::getSaveFileName(
        this, tr("Export data"),
        Format->currentIndex() == Raw ? QStringLiteral("data.bin")
                                      : QStringLiteral("data.txt"));
    if (!path.isEmpty())
      saveToFile(path);
  });
  Debounce.setSingleShot(true);
  Debounce.setInterval(::neverd::limits::gui::kExportDataDebounceMs);
  connect(&Debounce, &QTimer::timeout, this, &ExportDataDialog::readRange);
  connect(Start, &QLineEdit::textChanged, this,
          &ExportDataDialog::scheduleRead);
  connect(End, &QLineEdit::textChanged, this, &ExportDataDialog::scheduleRead);
  connect(Format, &QComboBox::currentIndexChanged, this,
          &ExportDataDialog::updatePreview);
  connect(&session, &Session::unloaded, this, &QDialog::reject);
  connect(&session, &Session::revisionChanged, this,
          &ExportDataDialog::scheduleRead);
  scheduleRead();
}

void ExportDataDialog::scheduleRead() {
  ++Serial;
  SessionRef.queries().unsubscribeOwner(this);
  Ready = false;
  Data.clear();
  Copy->setEnabled(false);
  Save->setEnabled(false);
  Preview->clear();
  Status->setText(tr("Reading bytes..."));
  Debounce.start();
}

void ExportDataDialog::fail(const QString &message) {
  Ready = false;
  Data.clear();
  Preview->clear();
  Copy->setEnabled(false);
  Save->setEnabled(false);
  Status->setText(message);
}

void ExportDataDialog::readRange() {
  const auto first = parseAddress(Start->text());
  const auto last = parseAddress(End->text());
  if (!first || !last || *last < *first) {
    fail(tr("Enter a valid address range."));
    return;
  }
  if (*last - *first >= ::neverd::limits::gui::kExportDataMaxBytes) {
    fail(tr("Export takes at most %1 bytes; select a smaller range.")
             .arg(::neverd::limits::gui::kExportDataMaxBytes));
    return;
  }
  First = *first;
  Size = qsizetype(*last - *first + 1);
  Epoch = SessionRef.epoch();
  Revision = SessionRef.revision();
  readNext();
}

void ExportDataDialog::readNext() {
  const auto serial = Serial;
  const Address address = First + Address(Data.size());
  const int count = int(std::min(Size - Data.size(),
                                 ::neverd::limits::gui::kExportDataChunkBytes));
  SessionRef.read(
      QStringLiteral("bytes"),
      {{"address", hexAddress(address)}, {"size", count}}, this,
      [this, serial, address, count](const QJsonObject &result) {
        if (serial != Serial || Epoch != SessionRef.epoch() ||
            Revision != SessionRef.revision())
          return;
        const QByteArray hex = result.value("data").toString().toLatin1();
        const bool validHex = std::all_of(hex.begin(), hex.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                 (c >= 'A' && c <= 'F');
        });
        if (addressValue(result.value("address")) != address ||
            result.value("bytes_read").toInt() != count ||
            hex.size() != count * 2 || !validHex ||
            result.value("encoding").toString() != QLatin1String("hex") ||
            result.value("mapping_status").toString() !=
                QLatin1String("mapped")) {
          fail(tr("Cannot read the complete range at %1. Nothing was exported.")
                   .arg(hexAddress(address)));
          return;
        }
        Data.append(QByteArray::fromHex(hex));
        if (Data.size() < Size) {
          Status->setText(
              tr("Reading bytes: %1 / %2").arg(Data.size()).arg(Size));
          readNext();
        } else {
          Ready = true;
          updatePreview();
        }
      },
      [this, serial](const QString &, const QString &message) {
        if (serial == Serial)
          fail(message);
      });
}

QByteArray ExportDataDialog::formatted(const QByteArray &data) const {
  const auto format = Format->currentIndex();
  if (format == Raw)
    return data;
  if (format == Hex || format == SpacedHex)
    return data.toHex(format == SpacedHex ? ' ' : '\0').toUpper();
  QByteArray text = format == String ? "\"" : "unsigned char data[] = { ";
  for (qsizetype i = 0; i < data.size(); ++i) {
    const auto byte = static_cast<unsigned char>(data[i]);
    if (format == String) {
      if (byte == '"' || byte == '\\')
        text += QByteArray("\\") + char(byte);
      else if (byte >= 0x20 && byte <= 0x7e)
        text += char(byte);
      else
        text += QByteArray("\\") +
                QByteArray::number(byte, 8).rightJustified(3, '0');
    } else {
      if (i)
        text += ", ";
      text += format == HexArray
                  ? QByteArray("0x") + QByteArray::number(byte, 16)
                                           .rightJustified(2, '0')
                                           .toUpper()
                  : QByteArray::number(byte);
    }
  }
  text += format == String ? "\"" : " };";
  return text;
}

void ExportDataDialog::updatePreview() {
  if (!Ready)
    return;
  const bool raw = Format->currentIndex() == Raw;
  const auto shown = Data.first(
      std::min(Data.size(), ::neverd::limits::gui::kExportDataPreviewBytes));
  Preview->setPlainText(
      QString::fromLatin1(raw ? shown.toHex(' ').toUpper() : formatted(shown)));
  Status->setText(Data.size() > shown.size()
                      ? tr("%1 bytes ready. Preview shows the first %2; copy "
                           "and save use the complete range.")
                            .arg(Data.size())
                            .arg(shown.size())
                      : tr("%1 bytes ready.").arg(Data.size()));
  Copy->setEnabled(!raw);
  Save->setEnabled(true);
}

bool ExportDataDialog::saveToFile(const QString &path) {
  if (!Ready || Epoch != SessionRef.epoch() ||
      Revision != SessionRef.revision())
    return false;
  QSaveFile file(path);
  const QByteArray output = formatted(Data);
  if (!file.open(QIODevice::WriteOnly) || file.write(output) != output.size() ||
      !file.commit()) {
    Status->setText(tr("Could not save %1: %2").arg(path, file.errorString()));
    return false;
  }
  Status->setText(tr("Saved %1 bytes to %2").arg(Data.size()).arg(path));
  return true;
}
} // namespace neverd::gui
