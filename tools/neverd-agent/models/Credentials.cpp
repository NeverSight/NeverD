//===- Credentials.cpp - User-local model credentials ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// DPAPI protects Windows credentials. Other desktops use their native keyring.
/// Credentials never enter settings JSON, project memories or run logs.
//===----------------------------------------------------------------------===//

#include "Credentials.h"

#include "ModelProfile.h"
#ifndef Q_OS_WIN
#include "platform/Keyring.h"
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <QStringDecoder>
#include <QUuid>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows SDK partition declarations must precede dpapi.h.
// clang-format off
#include <windows.h>
#include <dpapi.h>
// clang-format on
#endif

namespace neverd::agent {
namespace {
struct Text {
  Q_DECLARE_TR_FUNCTIONS(neverd::agent::Credentials)
};
} // namespace

Credentials::Credentials(QString directory) : Directory(std::move(directory)) {}

QString Credentials::path(const QString &id) const {
  if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
    return {};
  return QDir(Directory).filePath("credentials/" + id + ".key");
}

bool Credentials::validateKey(const QString &key, QString &error) {
  if (key.toUtf8().size() > model_limits::kMaxCredentialBytes ||
      key.contains('\r') || key.contains('\n') || key.contains(QChar('\0'))) {
    error = Text::tr("Invalid API key format.");
    return false;
  }
  return true;
}

bool Credentials::store(const QString &key, QString &id, QString &error) const {
  if (!validateKey(key, error) || key.isEmpty())
    return false;
  id.clear();
  QByteArray bytes = key.toUtf8();
#ifdef Q_OS_WIN
  DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                  reinterpret_cast<BYTE *>(bytes.data())};
  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"NeverD Agent API credential", nullptr,
                        nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    error = Text::tr("Windows could not protect the API key (error %1).")
                .arg(GetLastError());
    return false;
  }
  bytes =
      QByteArray(reinterpret_cast<const char *>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  const QString directory = QDir(Directory).filePath("credentials");
  if (!QDir().mkpath(directory)) {
    error = Text::tr("Cannot create the credential directory.");
    return false;
  }
  id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QSaveFile file(path(id));
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      file.write(bytes) != bytes.size() || !file.commit()) {
    error = Text::tr("Cannot save the API key: %1").arg(file.errorString());
    id.clear();
    return false;
  }
  return true;
#else
  const QString candidate = QUuid::createUuid().toString(QUuid::WithoutBraces);
  if (!keyring::store(candidate, bytes, error))
    return false;
  id = candidate;
  return true;
#endif
}

bool Credentials::read(const QString &id, QString &key, QString &error) const {
  key.clear();
  if (id.isEmpty())
    return true;
  const QString location = path(id);
  if (location.isEmpty()) {
    error = Text::tr("Invalid credential reference.");
    return false;
  }
#ifndef Q_OS_WIN
  if (!QFile::exists(location)) {
    QByteArray bytes;
    if (!keyring::read(id, bytes, error))
      return false;
    QStringDecoder decoder(QStringDecoder::Utf8);
    key = decoder(bytes);
    if (decoder.hasError() || key.isEmpty() || !validateKey(key, error)) {
      key.clear();
      error = Text::tr("The API key in the system credential store is invalid. "
                       "Enter it again.");
      return false;
    }
    return true;
  }
#endif
  QFile file(location);
  if (!file.open(QIODevice::ReadOnly) ||
      file.size() > model_limits::kMaxCredentialBytes * 2) {
    error =
        Text::tr("Cannot read the saved API key. Enter it again in settings.");
    return false;
  }
  QByteArray bytes = file.readAll();
#ifdef Q_OS_WIN
  DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                  reinterpret_cast<BYTE *>(bytes.data())};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    error = Text::tr("Cannot decrypt the API key. Sign in as the Windows user "
                     "who saved it, or enter the key again.");
    return false;
  }
  bytes =
      QByteArray(reinterpret_cast<const char *>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
#endif
  QStringDecoder decoder(QStringDecoder::Utf8);
  const QString candidate = decoder(bytes);
  if (decoder.hasError() || candidate.isEmpty() ||
      !validateKey(candidate, error)) {
    error =
        Text::tr("The saved API key has an invalid format. Enter it again.");
    return false;
  }
#ifndef Q_OS_WIN
  file.close();
  // Keep the legacy file if native storage or removal fails. Never report a
  // migration as complete while a plaintext copy is still present.
  if (!keyring::store(id, bytes, error))
    return false;
  if (!QFile::remove(location)) {
    error = Text::tr("The API key was stored in the system credential store, "
                     "but the old credential file could not be deleted: %1")
                .arg(location);
    return false;
  }
#endif
  key = candidate;
  return true;
}

bool Credentials::remove(const QString &id) const {
  const QString location = path(id);
  if (location.isEmpty())
    return false;
#ifndef Q_OS_WIN
  if (!keyring::remove(id))
    return false;
#endif
  return !QFile::exists(location) || QFile::remove(location);
}
} // namespace neverd::agent
