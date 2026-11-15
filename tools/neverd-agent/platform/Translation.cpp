//===- Translation.cpp - Agent interface translations --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Catalogs are compiled into each executable under :/i18n, named
/// neverd-agent_<code>.qm with the workbench's code spelling ("zh_CN").
///
//===----------------------------------------------------------------------===//

#include "Translation.h"

#include "WorkbenchSettings.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
#include <atomic>
#include <iterator>

namespace neverd::agent {
namespace {

QTranslator &agentTranslator() {
  static QTranslator translator;
  return translator;
}
QTranslator &qtTranslator() {
  static QTranslator translator;
  return translator;
}

struct LanguageName {
  const char *Code;
  const char *EnglishName;
};
constexpr LanguageName EnglishNames[] = {
    {"en", "English"},
    {"zh-CN", "Simplified Chinese"},
    {"zh-TW", "Traditional Chinese"},
    {"ja", "Japanese"},
    {"ko", "Korean"},
    {"fr", "French"},
    {"de", "German"},
    {"es", "Spanish"},
    {"it", "Italian"},
    {"ru", "Russian"},
    {"ar", "Arabic"},
};
// The installed entry of EnglishNames. Model requests read it from worker
// threads while the event loop may install another language.
std::atomic<int> Installed{0};

int entryOf(const QString &code) {
  for (int index = 0; index < int(std::size(EnglishNames)); ++index)
    if (code == QLatin1String(EnglishNames[index].Code))
      return index;
  return 0;
}
} // namespace

QString installTranslations(const QString &code) {
  const QString language = knownLanguage(code);
  QCoreApplication::removeTranslator(&agentTranslator());
  QCoreApplication::removeTranslator(&qtTranslator());
  QString resource = language;
  resource.replace(QLatin1Char('-'), QLatin1Char('_'));
  if (language != QLatin1String("en")) {
    if (agentTranslator().load(QStringLiteral(":/i18n/neverd-agent_") +
                               resource + QStringLiteral(".qm")))
      QCoreApplication::installTranslator(&agentTranslator());
    if (qtTranslator().load(QLocale(resource), QStringLiteral("qtbase"),
                            QStringLiteral("_"),
                            QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
      QCoreApplication::installTranslator(&qtTranslator());
  }
  Installed.store(entryOf(language));
  return language;
}

QString interfaceLanguage() {
  return QLatin1String(EnglishNames[Installed.load()].Code);
}

bool isRightToLeft(const QString &code) {
  QString resource = knownLanguage(code);
  resource.replace(QLatin1Char('-'), QLatin1Char('_'));
  return QLocale(resource).textDirection() == Qt::RightToLeft;
}

QString interfaceLanguageEnglishName() {
  return QLatin1String(EnglishNames[Installed.load()].EnglishName);
}

} // namespace neverd::agent
