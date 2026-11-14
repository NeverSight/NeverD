//===- AgentAppearance.cpp - The workbench's language and theme ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The theme choice "system" follows the desktop as the workbench does: its
/// high contrast setting first, then its light or dark color scheme.
///
//===----------------------------------------------------------------------===//

#include "AgentAppearance.h"

#include "DesktopContrast.h"
#include "Translation.h"
#include "WorkbenchSettings.h"

#include <QGuiApplication>
#include <QStyleHints>

namespace neverd::agent {
namespace {
namespace platform = ::neverd::gui::platform;

struct ColorEntry {
  const char *Name;
  const char *Values[4]; // Dark, Light, HighContrastDark, HighContrastLight.
};
constexpr ColorEntry ChromeColors[] = {
#define NEVERD_CHROME_COLOR(Name, Light, Dark, LightHighContrast,              \
                            DarkHighContrast)                                  \
  {#Name, {Dark, Light, DarkHighContrast, LightHighContrast}},
#include "ChromeColors.def"
};
constexpr ColorEntry ThemeColors[] = {
#define NEVERD_THEME_COLOR(Name, Light, Dark, LightHighContrast,               \
                           DarkHighContrast)                                   \
  {#Name, {Dark, Light, DarkHighContrast, LightHighContrast}},
#include "ThemeColors.def"
};

template <size_t Size>
QColor lookup(const ColorEntry (&table)[Size], const QString &name, int mode,
              const char *kind) {
  const int column = mode >= AgentAppearance::Dark &&
                             mode <= AgentAppearance::HighContrastLight
                         ? mode
                         : AgentAppearance::Dark;
  for (const auto &entry : table)
    if (name == QLatin1String(entry.Name))
      return QColor(QLatin1String(entry.Values[column]));
  qWarning("Unknown Agent %s color: %s", kind, qPrintable(name));
  return {};
}
} // namespace

AgentAppearance::AgentAppearance(QObject *parent) : QObject(parent) {
  connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, this,
          [this] {
            if (Choice == QLatin1String("system"))
              showTheme();
          });
}

void AgentAppearance::refresh() {
  const QString language = workbenchLanguage();
  if (language != Language) {
    Language = installTranslations(language);
    QGuiApplication::setLayoutDirection(
        isRightToLeft(Language) ? Qt::RightToLeft : Qt::LeftToRight);
    emit languageChanged();
  }
  const QString choice = workbenchTheme();
  if (choice != Choice) {
    Choice = choice;
    // An explicit scheme also gives native dialogs the shown theme; the
    // system choice leaves the scheme to the desktop.
    auto *hints = QGuiApplication::styleHints();
    if (Choice == QLatin1String("system"))
      hints->unsetColorScheme();
    else
      hints->setColorScheme(Choice == QLatin1String("light") ||
                                    Choice == QLatin1String("hc-light")
                                ? Qt::ColorScheme::Light
                                : Qt::ColorScheme::Dark);
    showTheme();
  }
}

bool AgentAppearance::rightToLeft() const { return isRightToLeft(Language); }

void AgentAppearance::showTheme() {
  Mode mode = Dark;
  if (Choice == QLatin1String("light"))
    mode = Light;
  else if (Choice == QLatin1String("hc-dark"))
    mode = HighContrastDark;
  else if (Choice == QLatin1String("hc-light"))
    mode = HighContrastLight;
  else if (Choice == QLatin1String("system")) {
    const auto contrast = platform::desktopContrast();
    const auto scheme = QGuiApplication::styleHints()->colorScheme();
    if (contrast == platform::DesktopContrast::HighDark)
      mode = HighContrastDark;
    else if (contrast == platform::DesktopContrast::HighLight)
      mode = HighContrastLight;
    else if (scheme == Qt::ColorScheme::Unknown)
      mode = Shown;
    else
      mode = scheme == Qt::ColorScheme::Light ? Light : Dark;
  }
  if (mode == Shown && !Language.isEmpty() && property("shownOnce").toBool())
    return;
  setProperty("shownOnce", true);
  Shown = mode;
  emit themeChanged();
}

QColor AgentAppearance::chromeColor(const QString &name, int mode) const {
  return lookup(ChromeColors, name, mode, "chrome");
}

QColor AgentAppearance::themeColor(const QString &name, int mode) const {
  return lookup(ThemeColors, name, mode, "theme");
}

} // namespace neverd::agent
