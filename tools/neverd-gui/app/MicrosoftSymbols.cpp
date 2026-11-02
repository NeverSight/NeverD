//===- MicrosoftSymbols.cpp - Load the PDB an open PE lacks ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Offer the PDB an open PE lacks. HTTP failures leave analysis intact;
/// no downloaded file is applied without the engine's strict identity check.
//===----------------------------------------------------------------------===//

#include "MicrosoftSymbols.h"

#include "../ResourceLimits.h"
#include "PdbPromptDialog.h"
#include "Session.h"

#include <QApplication>
#include <QNetworkAccessManager>
#include <QTimer>

namespace neverd::gui {
namespace {} // namespace

MicrosoftSymbols::MicrosoftSymbols(Session &session, QWidget *window,
                                   QNetworkAccessManager *network)
    : QObject(window), session_(session), window_(window),
      network_(network ? network : new QNetworkAccessManager(this)) {
  connect(&session_, &Session::opened, this, &MicrosoftSymbols::opened);
  connect(&session_, &Session::unloaded, this, &MicrosoftSymbols::cancel);
  connect(&session_, &Session::stateChanged, this, [this] {
    if (!request_.isEmpty() && !current(serial_))
      cancel();
  });
}

MicrosoftSymbols::~MicrosoftSymbols() { cancel(); }

bool MicrosoftSymbols::current(quint64 serial) const {
  return serial == serial_ && !request_.isEmpty() && session_.loaded() &&
         !session_.opening() && session_.epoch() == epoch_ &&
         session_.metadata().value("pdb_download").toObject() == request_ &&
         !session_.metadata().value("pdb").isObject();
}

void MicrosoftSymbols::cancel() {
  ++serial_;
  request_ = {};
  if (prompt_) {
    auto *prompt = prompt_.data();
    prompt_ = nullptr;
    disconnect(prompt, nullptr, this, nullptr);
    prompt->reject();
    prompt->deleteLater();
  }
}

void MicrosoftSymbols::setOffers(bool offers) {
  offers_ = offers;
  if (!offers)
    cancel();
}

void MicrosoftSymbols::opened() {
  cancel();
  const auto request = session_.metadata().value("pdb_download").toObject();
  if (!offers_ || request.isEmpty() || !session_.loadsPdbFiles() ||
      !session_.findsPdbFiles())
    return;
  request_ = request;
  epoch_ = session_.epoch();
  const auto serial = serial_;
  // The opened signal runs inside query delivery. Ask only after that
  // barrier is released, and discard the answer on a new file.
  QTimer::singleShot(0, this, [this, serial] {
    if (!current(serial))
      return;
    session_.read(QStringLiteral("pdb_reference"),
                  {{"path", session_.filePath()}}, this,
                  [this, serial](const QJsonObject &payload) {
                    const auto reference = payload.value("pdb").toObject();
                    if (!current(serial) ||
                        reference.value("key") != request_.value("key"))
                      return;
                    // A copy that reached a store since the file loaded is the
                    // one a load would have found.
                    const auto found = reference.value("found").toObject();
                    if (!found.isEmpty())
                      apply(found.value("path").toString(), serial);
                    else
                      offer(serial, reference);
                  });
  });
}

void MicrosoftSymbols::offer(quint64 serial, const QJsonObject &reference) {
  if (!current(serial))
    return;
  if (QApplication::activeModalWidget()) {
    QTimer::singleShot(::neverd::limits::gui::kMicrosoftSymbolsModalRetryMs,
                       this,
                       [this, serial, reference] { offer(serial, reference); });
    return;
  }
  auto *prompt = new PdbPromptDialog(session_, session_.filePath(), reference,
                                     network_, window_);
  prompt_ = prompt;
  prompt->setAttribute(Qt::WA_DeleteOnClose);
  connect(prompt, &QDialog::finished, this, [this, serial, prompt] {
    if (!current(serial))
      return;
    prompt_ = nullptr;
    const auto path = prompt->choice().value("path").toString();
    if (path.isEmpty()) {
      cancel();
      session_.declinePdb();
    } else
      apply(path, serial);
  });
  prompt->open();
}

void MicrosoftSymbols::apply(const QString &path, quint64 serial) {
  if (!current(serial))
    return;
  // pdb_load repeats the strict identity check transactionally and returns
  // the final path's stamp. No unverified-analysis fallback is used here.
  const QJsonObject selection{
      {"path", path}, {"mode", "verified"}, {"download", request_}};
  cancel();
  session_.loadPdb(selection);
}
} // namespace neverd::gui
