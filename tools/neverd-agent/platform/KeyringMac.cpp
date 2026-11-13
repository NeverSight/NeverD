//===- KeyringMac.cpp - macOS Keychain credentials -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep API keys in the user's Keychain under immutable credential UUIDs.
//===----------------------------------------------------------------------===//

#include "Keyring.h"

#include <QScopeGuard>
#include <Security/Security.h>

namespace neverd::agent::keyring {
namespace {
CFMutableDictionaryRef query(const QString &id) {
  auto result = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
                                          &kCFTypeDictionaryKeyCallBacks,
                                          &kCFTypeDictionaryValueCallBacks);
  const auto bytes = id.toUtf8();
  const auto account = CFStringCreateWithBytes(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(bytes.constData()),
      bytes.size(), kCFStringEncodingUTF8, false);
  CFDictionarySetValue(result, kSecClass, kSecClassGenericPassword);
  CFDictionarySetValue(result, kSecAttrService,
                       CFSTR("org.neversight.neverd-agent"));
  CFDictionarySetValue(result, kSecAttrAccount, account);
  CFRelease(account);
  return result;
}
bool checked(OSStatus status, QString &error) {
  if (status == errSecSuccess)
    return true;
  error =
      QStringLiteral("无法访问 macOS 钥匙串（错误 %1）。请解锁登录钥匙串并允许 "
                     "NeverD Agent 访问，或重新填写 API Key。")
          .arg(status);
  return false;
}
} // namespace

bool store(const QString &id, const QByteArray &secret, QString &error) {
  const auto attributes = query(id);
  const auto data = CFDataCreate(
      kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(secret.constData()),
      secret.size());
  const auto cleanup = qScopeGuard([&] {
    CFRelease(data);
    CFRelease(attributes);
  });
  CFDictionarySetValue(attributes, kSecValueData, data);
  CFDictionarySetValue(attributes, kSecAttrLabel,
                       CFSTR("NeverD Agent API Key"));
  auto status = SecItemAdd(attributes, nullptr);
  if (status == errSecDuplicateItem) {
    // A legacy-file migration can be resumed after the native write succeeded.
    CFDictionaryRemoveValue(attributes, kSecValueData);
    CFDictionaryRemoveValue(attributes, kSecAttrLabel);
    const void *keys[] = {kSecValueData};
    const void *values[] = {data};
    const auto changes = CFDictionaryCreate(kCFAllocatorDefault, keys, values,
                                            1, &kCFTypeDictionaryKeyCallBacks,
                                            &kCFTypeDictionaryValueCallBacks);
    status = SecItemUpdate(attributes, changes);
    CFRelease(changes);
  }
  return checked(status, error);
}

bool read(const QString &id, QByteArray &secret, QString &error) {
  const auto attributes = query(id);
  const auto cleanup = qScopeGuard([&] { CFRelease(attributes); });
  CFDictionarySetValue(attributes, kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(attributes, kSecMatchLimit, kSecMatchLimitOne);
  CFTypeRef result = nullptr;
  if (!checked(SecItemCopyMatching(attributes, &result), error))
    return false;
  const auto data = static_cast<CFDataRef>(result);
  secret = QByteArray(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                      CFDataGetLength(data));
  CFRelease(result);
  return true;
}

bool remove(const QString &id) {
  const auto attributes = query(id);
  const auto status = SecItemDelete(attributes);
  CFRelease(attributes);
  return status == errSecSuccess || status == errSecItemNotFound;
}
} // namespace neverd::agent::keyring
