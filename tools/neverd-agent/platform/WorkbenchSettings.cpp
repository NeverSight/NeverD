//===- WorkbenchSettings.cpp - Settings shared with the workbench --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Read the workbench's settings store. The workbench names itself
/// NeverSight/NeverD (domain neversight.dev) and keeps Qt's native format, so
/// these values come from the same file, registry key or property list.
///
//===----------------------------------------------------------------------===//

#include "WorkbenchSettings.h"

#include <QSettings>
#include <QVariant>

namespace neverd::agent {
namespace {
constexpr char LanguageKey[] = "ui/language";
constexpr char ThemeKey[] = "appearance/theme";
constexpr char DefaultLanguage[] = "en";
constexpr char DefaultTheme[] = "dark";

/// QSettings() in the workbench passes its organization domain on Apple
/// platforms and its organization name elsewhere; use the same identity.
QString workbenchOrganization() {
#ifdef Q_OS_DARWIN
  return QStringLiteral("neversight.dev");
#else
  return QStringLiteral("NeverSight");
#endif
}

QVariant workbenchValue(const char *key) {
  const QSettings settings(workbenchOrganization(), QStringLiteral("NeverD"));
  return settings.value(QLatin1String(key));
}
} // namespace

const QStringList &interfaceLanguages() {
  static const QStringList codes = [] {
    QStringList result;
#define NEVERD_LANGUAGE(Code, SelfName) result.append(QStringLiteral(Code));
#include "Languages.def"
    return result;
  }();
  return codes;
}

QString knownLanguage(const QString &code) {
  return interfaceLanguages().contains(code) ? code
                                             : QLatin1String(DefaultLanguage);
}

QString workbenchLanguage() {
  return knownLanguage(workbenchValue(LanguageKey).toString());
}

QString workbenchTheme() {
  const QString saved = workbenchValue(ThemeKey).toString();
  for (const char *choice : {"dark", "light", "hc-dark", "hc-light", "system"})
    if (saved == QLatin1String(choice))
      return saved;
  return QLatin1String(DefaultTheme);
}

} // namespace neverd::agent
