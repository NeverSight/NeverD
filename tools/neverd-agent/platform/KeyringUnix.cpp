//===- KeyringUnix.cpp - Secret Service credentials ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Use libsecret and the desktop's Secret Service; never fall back to
/// plaintext.
//===----------------------------------------------------------------------===//

#include "Keyring.h"

#pragma push_macro("signals")
#undef signals
#include <libsecret/secret.h>
#pragma pop_macro("signals")

namespace neverd::agent::keyring {
namespace {
const SecretSchema Schema = {"org.neversight.neverd-agent",
                             SECRET_SCHEMA_NONE,
                             {{"id", SECRET_SCHEMA_ATTRIBUTE_STRING},
                              {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING}}};

bool checked(GError *failure, QString &error) {
  if (!failure)
    return true;
  error = QStringLiteral("无法访问系统凭据库：%1。请启动并解锁 Secret "
                         "Service（GNOME Keyring 或兼容服务）。")
              .arg(QString::fromUtf8(failure->message));
  g_error_free(failure);
  return false;
}
} // namespace

bool store(const QString &id, const QByteArray &secret, QString &error) {
  GError *failure = nullptr;
  const auto account = id.toUtf8();
  const bool stored = secret_password_store_sync(
      &Schema, SECRET_COLLECTION_DEFAULT, "NeverD Agent API Key",
      secret.constData(), nullptr, &failure, "id", account.constData(),
      nullptr);
  if (!checked(failure, error))
    return false;
  if (!stored)
    error =
        QStringLiteral("系统凭据库未保存 API Key；请确认默认钥匙环已解锁。");
  return stored;
}

bool read(const QString &id, QByteArray &secret, QString &error) {
  GError *failure = nullptr;
  const auto account = id.toUtf8();
  auto *password = secret_password_lookup_sync(&Schema, nullptr, &failure, "id",
                                               account.constData(), nullptr);
  if (!checked(failure, error))
    return false;
  if (!password) {
    error = QStringLiteral(
        "系统凭据库中没有可用的 API Key；请解锁钥匙环，或在设置中重新填写。");
    return false;
  }
  secret = password;
  secret_password_free(password);
  return true;
}

bool remove(const QString &id) {
  GError *failure = nullptr;
  auto *service =
      secret_service_get_sync(SECRET_SERVICE_NONE, nullptr, &failure);
  if (!service) {
    g_clear_error(&failure);
    return false;
  }
  const auto account = id.toUtf8();
  auto *attributes =
      secret_attributes_build(&Schema, "id", account.constData(), nullptr);
  auto *items = secret_service_search_sync(
      service, &Schema, attributes, SECRET_SEARCH_ALL, nullptr, &failure);
  bool removed = !failure;
  for (auto *item = items; item; item = item->next) {
    GError *itemError = nullptr;
    if (!secret_item_delete_sync(SECRET_ITEM(item->data), nullptr, &itemError))
      removed = false;
    g_clear_error(&itemError);
  }
  g_clear_error(&failure);
  g_list_free_full(items, g_object_unref);
  g_hash_table_unref(attributes);
  g_object_unref(service);
  return removed;
}
} // namespace neverd::agent::keyring
