//===- Credentials.cpp - User-local model credentials
//----------------------===//
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
Credentials::Credentials(QString directory) : Directory(std::move(directory)) {}

QString Credentials::path(const QString &id) const {
  if (QUuid(id).isNull() || QUuid(id).toString(QUuid::WithoutBraces) != id)
    return {};
  return QDir(Directory).filePath("credentials/" + id + ".key");
}

bool Credentials::validateKey(const QString &key, QString &error) {
  if (key.toUtf8().size() > model_limits::kMaxCredentialBytes ||
      key.contains('\r') || key.contains('\n') || key.contains(QChar('\0'))) {
    error = QStringLiteral("API Key 格式无效。");
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
    error = QStringLiteral("Windows 无法保护 API Key（错误 %1）。")
                .arg(GetLastError());
    return false;
  }
  bytes =
      QByteArray(reinterpret_cast<const char *>(output.pbData), output.cbData);
  SecureZeroMemory(output.pbData, output.cbData);
  LocalFree(output.pbData);
  const QString directory = QDir(Directory).filePath("credentials");
  if (!QDir().mkpath(directory)) {
    error = QStringLiteral("无法创建凭据目录。");
    return false;
  }
  id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QSaveFile file(path(id));
  if (!file.open(QIODevice::WriteOnly) ||
      !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
      file.write(bytes) != bytes.size() || !file.commit()) {
    error = QStringLiteral("无法保存 API Key：%1").arg(file.errorString());
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
    error = QStringLiteral("凭据引用无效。");
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
      error = QStringLiteral("系统凭据库中的 API Key 无效，请重新填写。");
      return false;
    }
    return true;
  }
#endif
  QFile file(location);
  if (!file.open(QIODevice::ReadOnly) ||
      file.size() > model_limits::kMaxCredentialBytes * 2) {
    error = QStringLiteral("无法读取已保存的 API Key，请在设置中重新填写。");
    return false;
  }
  QByteArray bytes = file.readAll();
#ifdef Q_OS_WIN
  DATA_BLOB input{static_cast<DWORD>(bytes.size()),
                  reinterpret_cast<BYTE *>(bytes.data())};
  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output)) {
    error = QStringLiteral(
        "无法解密 API Key，请使用保存它的 Windows 用户，或重新填写。");
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
    error = QStringLiteral("已保存的 API Key 格式无效，请重新填写。");
    return false;
  }
#ifndef Q_OS_WIN
  file.close();
  // Keep the legacy file if native storage or removal fails. Never report a
  // migration as complete while a plaintext copy is still present.
  if (!keyring::store(id, bytes, error))
    return false;
  if (!QFile::remove(location)) {
    error = QStringLiteral("API Key 已存入系统凭据库，但无法删除旧凭据文件：%1")
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
