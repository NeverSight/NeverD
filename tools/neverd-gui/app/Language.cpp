#include "Language.h"

#include "Theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

namespace neverd::gui {
namespace {
constexpr char LanguageKey[] = "ui/language";
constexpr char DefaultLanguage[] = "en";

struct LanguageEntry {
  const char *code;
  const char *selfName;
};
constexpr LanguageEntry LanguageTable[] = {
#define NEVERD_LANGUAGE(Code, SelfName) {Code, SelfName},
#include "Languages.def"
};

QTranslator &applicationTranslator() {
  static QTranslator translator;
  return translator;
}
QTranslator &qtTranslator() {
  static QTranslator translator;
  return translator;
}
} // namespace

const QVector<Language> &languages() {
  static const QVector<Language> list = [] {
    QVector<Language> result;
    for (const auto &entry : LanguageTable)
      result.append(
          {QString::fromLatin1(entry.code), QString::fromUtf8(entry.selfName)});
    return result;
  }();
  return list;
}

QString currentLanguage() {
  const QString saved = QSettings().value(LanguageKey).toString();
  for (const auto &language : languages())
    if (language.code == saved)
      return saved;
  // First start is English until the user picks a language.
  return QString::fromLatin1(DefaultLanguage);
}

void applyLanguage(const QString &code) {
  bool known = false;
  for (const auto &language : languages())
    known = known || language.code == code;
  if (!known)
    return;
  QCoreApplication::removeTranslator(&applicationTranslator());
  QCoreApplication::removeTranslator(&qtTranslator());
  QString resource = code;
  resource.replace(QLatin1Char('-'), QLatin1Char('_'));
  if (code != QLatin1String(DefaultLanguage)) {
    if (applicationTranslator().load(QStringLiteral(":/i18n/neverd_") +
                                     resource + QStringLiteral(".qm")))
      QCoreApplication::installTranslator(&applicationTranslator());
    if (qtTranslator().load(QLocale(resource), QStringLiteral("qtbase"),
                            QStringLiteral("_"),
                            QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
      QCoreApplication::installTranslator(&qtTranslator());
  }
  if (qobject_cast<QApplication *>(QCoreApplication::instance()))
    QApplication::setLayoutDirection(QLocale(resource).textDirection());
  QSettings().setValue(LanguageKey, code);
  if (qobject_cast<QApplication *>(QCoreApplication::instance()))
    Theme::instance().apply();
}

} // namespace neverd::gui
