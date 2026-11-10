//===- Theme.cpp - Workbench colors and bundled fonts ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Apply the owned palette and font families across desktop platforms.
//===----------------------------------------------------------------------===//

#include "Theme.h"

#include "../Config.h"
#include "Language.h"
#include "WorkbenchStyle.h"

#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QPalette>
#include <QRegularExpression>
#include <QSettings>
#include <QStringList>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>
#include <algorithm>

namespace neverd::gui {
namespace {
/// A color's value in each theme, in the order of Theme::Mode.
struct ThemedColor {
  const char *name;
  std::array<const char *, 4> values;
};
constexpr ThemedColor ViewColors[] = {
#define NEVERD_THEME_COLOR(Role, Light, Dark, LightHighContrast,               \
                           DarkHighContrast)                                   \
  {#Role, {Dark, Light, DarkHighContrast, LightHighContrast}},
#include "ThemeColors.def"
};
static_assert(std::size(ViewColors) ==
              static_cast<std::size_t>(ColorRole::Count));
constexpr ThemedColor ChromeColors[] = {
#define NEVERD_CHROME_COLOR(Name, Light, Dark, LightHighContrast,              \
                            DarkHighContrast)                                  \
  {#Name, {Dark, Light, DarkHighContrast, LightHighContrast}},
#include "ChromeColors.def"
};

constexpr int ListingRoleCount = 0
#define NEVERD_LISTING_ROLE(Name, Code) +1
#include "ListingRoles.def"
    ;
constexpr int AddressClassCount = 0
#define NEVERD_ADDRESS_CLASS(Name, Code) +1
#include "AddressClasses.def"
    ;
// Worker protocol codes map to view colors by name; a role without a color is
// a compile error.
constexpr auto listingColors() {
  std::array<ColorRole, ListingRoleCount> map{};
#define NEVERD_LISTING_ROLE(Name, Code) map[Code] = ColorRole::Listing##Name;
#include "ListingRoles.def"
  return map;
}
constexpr auto prefixColors() {
  std::array<ColorRole, AddressClassCount> map{};
#define NEVERD_ADDRESS_CLASS(Name, Code) map[Code] = ColorRole::Prefix##Name;
#include "AddressClasses.def"
  return map;
}
constexpr auto navigationColors() {
  std::array<ColorRole, AddressClassCount> map{};
#define NEVERD_ADDRESS_CLASS(Name, Code) map[Code] = ColorRole::Nav##Name;
#include "AddressClasses.def"
  return map;
}

constexpr char ModeKey[] = "appearance/theme";

/// The mode a theme chosen by name shows.
Theme::Mode chosenMode(Theme::Choice choice) {
  switch (choice) {
  case Theme::Choice::Light:
    return Theme::Mode::Light;
  case Theme::Choice::HighContrastDark:
    return Theme::Mode::HighContrastDark;
  case Theme::Choice::HighContrastLight:
    return Theme::Mode::HighContrastLight;
  case Theme::Choice::Dark:
  case Theme::Choice::System:
    break;
  }
  return Theme::Mode::Dark;
}

/// The mode the system's theme shows for the desktop's \p scheme and
/// \p contrast, or \p shown where the desktop does not say.
Theme::Mode desktopMode(Qt::ColorScheme scheme,
                        platform::DesktopContrast contrast, Theme::Mode shown) {
  if (contrast == platform::DesktopContrast::HighDark)
    return Theme::Mode::HighContrastDark;
  if (contrast == platform::DesktopContrast::HighLight)
    return Theme::Mode::HighContrastLight;
  if (scheme == Qt::ColorScheme::Unknown)
    return shown;
  return scheme == Qt::ColorScheme::Light ? Theme::Mode::Light
                                          : Theme::Mode::Dark;
}

constexpr char FontKey[] = "appearance/codeFont";
constexpr char StyleSheetResource[] = ":/neverd/theme.qss";
/// Rules a high contrast theme adds over the style sheet: outlines where the
/// other themes fill (theme-high-contrast.qss).
constexpr char HighContrastStyleSheetResource[] =
    ":/neverd/theme-high-contrast.qss";
// ${Mode} in the style sheet stands for the theme's name, and ${CodeFont} for
// the code font at its default size, in which text fields type.
constexpr char ModePlaceholder[] = "Mode";
constexpr char CodeFontPlaceholder[] = "CodeFont";

QFont withWorkbenchMetrics(QFont font) {
  // Horizontal grid fitting can give variable monospace fonts different
  // advances for W and i. Keep fractional widths and vertical hinting.
  font.setHintingPreference(QFont::PreferVerticalHinting);
  return font;
}

void loadFonts() {
  static const bool loaded = [] {
    for (const auto *file :
         {"NotoSans.ttf", "JetBrainsMonoNL-Regular.ttf", "NotoSansArabic.ttf",
          "NotoSansSymbols2.ttf", "NotoSansCJK.ttc"})
      if (QFontDatabase::addApplicationFont(QStringLiteral(":/neverd/fonts/") +
                                            QLatin1String(file)) < 0)
        qFatal("Cannot load bundled workbench font %s", file);
    return true;
  }();
  Q_UNUSED(loaded);
}

QStringList configureFontFallbacks() {
  loadFonts();
  const QString language = currentLanguage();
  const auto cjk = language == QLatin1String("ja")      ? "Noto Sans CJK JP"
                   : language == QLatin1String("ko")    ? "Noto Sans CJK KR"
                   : language == QLatin1String("zh-TW") ? "Noto Sans CJK TC"
                                                        : "Noto Sans CJK SC";
  // Punctuation and symbols can inherit the adjacent text's script. Keep
  // their bundled fallbacks on every script, not just Common; otherwise an
  // arrow after Cyrillic or Arabic can pick up a desktop font.
  const QStringList symbols{QStringLiteral("Noto Sans Symbols 2"),
                            QStringLiteral("JetBrains Mono NL"),
                            QLatin1String(cjk)};
  const auto fallback = [&](QChar::Script script, const char *family) {
    QStringList families{QLatin1String(family)};
    families.append(symbols);
    families.removeDuplicates();
    // Qt clears the font cache even when the fallback list is unchanged.
    if (QFontDatabase::applicationFallbackFontFamilies(script) != families)
      QFontDatabase::setApplicationFallbackFontFamilies(script, families);
  };
  fallback(QChar::Script_Han, cjk);
  fallback(QChar::Script_Hiragana, "Noto Sans CJK JP");
  fallback(QChar::Script_Katakana, "Noto Sans CJK JP");
  fallback(QChar::Script_Hangul, "Noto Sans CJK KR");
  fallback(QChar::Script_Arabic, "Noto Sans Arabic");
  fallback(QChar::Script_Cyrillic, "Noto Sans");
  fallback(QChar::Script_Greek, "Noto Sans");
  if (QFontDatabase::applicationFallbackFontFamilies(QChar::Script_Common) !=
      symbols)
    QFontDatabase::setApplicationFallbackFontFamilies(QChar::Script_Common,
                                                      symbols);
  // CoreText shaping can also use the font's own fallback list.
  return {QStringLiteral("Noto Sans"), QLatin1String(cjk),
          QStringLiteral("Noto Sans Arabic"),
          QStringLiteral("Noto Sans Symbols 2"),
          QStringLiteral("JetBrains Mono NL")};
}

struct PaletteRole {
  const char *name;
  QPalette::ColorRole role;
};
constexpr PaletteRole PaletteRoles[] = {
    {"Window", QPalette::Window},
    {"WindowText", QPalette::WindowText},
    {"Base", QPalette::Base},
    {"AlternateBase", QPalette::AlternateBase},
    {"Text", QPalette::Text},
    {"Button", QPalette::Button},
    {"ButtonText", QPalette::ButtonText},
    {"Highlight", QPalette::Highlight},
    {"HighlightedText", QPalette::HighlightedText},
    {"ToolTipBase", QPalette::ToolTipBase},
    {"ToolTipText", QPalette::ToolTipText},
    {"PlaceholderText", QPalette::PlaceholderText},
    {"Link", QPalette::Link},
    {"LinkVisited", QPalette::LinkVisited},
    {"BrightText", QPalette::BrightText},
    {"Accent", QPalette::Accent},
    {"Light", QPalette::Light},
    {"Midlight", QPalette::Midlight},
    {"Mid", QPalette::Mid},
    {"Dark", QPalette::Dark},
    {"Shadow", QPalette::Shadow},
};
static_assert(std::size(PaletteRoles) == QPalette::NColorRoles - 1);
} // namespace

Theme &Theme::instance() {
  static Theme theme;
  return theme;
}

Theme::Theme() {
  QSettings settings;
  choice_ = choiceFromName(settings.value(ModeKey).toString());
  // The desktop's scheme is known once the application starts (apply()).
  mode_ = chosenMode(choice_);
  codeFont_ = defaultCodeFont();
  if (const auto saved = settings.value(FontKey).toString(); !saved.isEmpty()) {
    QFont font;
    if (font.fromString(saved))
      codeFont_ = withWorkbenchMetrics(font);
  }
  loadColors();
}

QString Theme::modeName(Mode mode) {
  return mode == Mode::Light || mode == Mode::HighContrastLight
             ? QStringLiteral("light")
             : QStringLiteral("dark");
}

void Theme::setMode(Mode mode) {
  switch (mode) {
  case Mode::Dark:
    setChoice(Choice::Dark);
    return;
  case Mode::Light:
    setChoice(Choice::Light);
    return;
  case Mode::HighContrastDark:
    setChoice(Choice::HighContrastDark);
    return;
  case Mode::HighContrastLight:
    setChoice(Choice::HighContrastLight);
    return;
  }
}

QString Theme::choiceName(Choice choice) {
  switch (choice) {
  case Choice::Light:
    return QStringLiteral("light");
  case Choice::HighContrastDark:
    return QStringLiteral("hc-dark");
  case Choice::HighContrastLight:
    return QStringLiteral("hc-light");
  case Choice::System:
    return QStringLiteral("system");
  case Choice::Dark:
    break;
  }
  return QStringLiteral("dark");
}

Theme::Choice Theme::choiceFromName(const QString &name) {
  for (const auto choice : {Choice::Light, Choice::HighContrastDark,
                            Choice::HighContrastLight, Choice::System})
    if (name == choiceName(choice))
      return choice;
  return Choice::Dark;
}

void Theme::setChoice(Choice choice) {
  if (choice == choice_)
    return;
  choice_ = choice;
  QSettings().setValue(ModeKey, choiceName(choice));
  const Mode shown = mode_;
  if (choice == Choice::System) {
    followColorScheme(QGuiApplication::styleHints()->colorScheme());
    return;
  }
  mode_ = chosenMode(choice);
  if (mode_ != shown)
    apply();
}

void Theme::followColorScheme(Qt::ColorScheme scheme) {
  if (choice_ == Choice::System)
    followDesktop(scheme, platform::desktopContrast());
}

void Theme::followDesktop(Qt::ColorScheme scheme,
                          platform::DesktopContrast contrast) {
  if (choice_ != Choice::System)
    return;
  const Mode mode = desktopMode(scheme, contrast, mode_);
  if (mode == mode_)
    return;
  mode_ = mode;
  apply();
}

bool Theme::eventFilter(QObject *watched, QEvent *event) {
  if ((event->type() == QEvent::ThemeChange && watched->isWidgetType() &&
       static_cast<QWidget *>(watched)->isWindow()) ||
      event->type() == QEvent::ApplicationStateChange)
    followColorScheme(QGuiApplication::styleHints()->colorScheme());
  return QObject::eventFilter(watched, event);
}

QFont Theme::defaultCodeFont() {
  loadFonts();
  // A point has different logical-pixel sizes on Cocoa and other desktops.
  // Use the platform's normal reading size for code too: Cocoa's FixedFont
  // defaults to 10 pt even when its interface font is 13 pt.
  QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  font.setFamily(QStringLiteral("JetBrains Mono NL"));
  font.setStyleHint(QFont::Monospace, QFont::PreferAntialias);
  font.setFixedPitch(true);
  return withWorkbenchMetrics(font);
}

void Theme::setCodeFont(const QFont &font) {
  const bool family = font.family() != codeFont_.family();
  codeFont_ = withWorkbenchMetrics(font);
  QSettings().setValue(FontKey, font.toString());
  // Text fields take the family, but not the zoomed size, from the style
  // sheet.
  if (family)
    apply();
  else
    emit changed();
}

void Theme::zoomCodeFont(int steps) {
  QFont font = codeFont_;
  const qreal points =
      font.pointSizeF() > 0 ? font.pointSizeF() : QFontInfo(font).pointSizeF();
  font.setPointSizeF(std::clamp<qreal>(
      points + steps, ::neverd::limits::gui_config::kThemeMinimumCodePointSize,
      ::neverd::limits::gui_config::kThemeMaximumCodePointSize));
  setCodeFont(font);
}

void Theme::resetCodeFontSize() {
  QFont font = codeFont_;
  const auto defaults = defaultCodeFont();
  if (defaults.pixelSize() > 0)
    font.setPixelSize(defaults.pixelSize());
  else
    font.setPointSizeF(defaults.pointSizeF());
  setCodeFont(font);
}

void Theme::loadColors() {
  const auto column = static_cast<std::size_t>(mode_);
  for (std::size_t i = 0; i < std::size(ViewColors); ++i)
    colors_[i] = QColor(ViewColors[i].values[column]);
  chrome_.clear();
  for (const auto &entry : ChromeColors)
    chrome_.insert(QString::fromLatin1(entry.name),
                   QColor(entry.values[column]));
}

void Theme::apply() {
  auto *application =
      qobject_cast<QApplication *>(QCoreApplication::instance());
  // The system's theme takes the desktop's light or dark mode now and
  // whenever it changes.
  if (application) {
    if (!followsDesktop_) {
      followsDesktop_ = true;
      connect(application->styleHints(), &QStyleHints::colorSchemeChanged, this,
              &Theme::followColorScheme);
      application->installEventFilter(this);
    }
    if (choice_ == Choice::System)
      mode_ = desktopMode(application->styleHints()->colorScheme(),
                          platform::desktopContrast(), mode_);
  }
  loadColors();
  if (!application) {
    emit changed();
    return;
  }
  // Qt's macOS default hides action icons even in application-painted menus.
  // Apply after QApplication initializes the platform's defaults.
  QCoreApplication::setAttribute(Qt::AA_DontShowIconsInMenus, false);
  // Under the application style sheet, the workbench style is the base of the
  // style sheet's style.
  if (QStyle *style = application->style();
      !qobject_cast<WorkbenchStyle *>(style) &&
      !style->findChild<WorkbenchStyle *>(Qt::FindDirectChildrenOnly))
    application->setStyle(new WorkbenchStyle);
  QFont uiFont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
  uiFont.setFamilies(configureFontFallbacks());
  application->setFont(withWorkbenchMetrics(uiFont));
  QPalette palette;
  for (const auto &entry : PaletteRoles)
    palette.setColor(entry.role,
                     chrome_.value(QString::fromLatin1(entry.name)));
  const QColor disabled = chrome_.value("DisabledText");
  for (const auto role : {QPalette::WindowText, QPalette::Text,
                          QPalette::ButtonText, QPalette::HighlightedText})
    palette.setColor(QPalette::Disabled, role, disabled);
  application->setPalette(palette);

  QFile file(StyleSheetResource);
  if (file.open(QIODevice::ReadOnly)) {
    QString sheet = QString::fromUtf8(file.readAll());
    if (highContrast()) {
      QFile overlay(HighContrastStyleSheetResource);
      if (overlay.open(QIODevice::ReadOnly))
        sheet += QLatin1Char('\n') + QString::fromUtf8(overlay.readAll());
    }
    static const QRegularExpression placeholder(R"(\$\{([A-Za-z]+)\})");
    QString resolved;
    resolved.reserve(sheet.size());
    qsizetype last = 0;
    for (auto it = placeholder.globalMatch(sheet); it.hasNext();) {
      const auto match = it.next();
      resolved += QStringView(sheet).mid(last, match.capturedStart() - last);
      const QString name = match.captured(1);
      if (name == QLatin1String(ModePlaceholder))
        resolved += modeName(mode_);
      else if (name == QLatin1String(CodeFontPlaceholder)) {
        // Zooming the code views leaves text fields alone.
        const auto font = defaultCodeFont();
        resolved += QStringLiteral("%1%2 \"%3\"")
                        .arg(font.pixelSize() > 0 ? font.pixelSize()
                                                  : font.pointSizeF())
                        .arg(font.pixelSize() > 0 ? "px" : "pt")
                        .arg(codeFont_.family());
      } else if (const auto color = chrome_.constFind(name);
                 color != chrome_.cend())
        // A translucent color keeps its alpha.
        resolved += color->alpha() == 255
                        ? color->name()
                        : QStringLiteral("rgba(%1, %2, %3, %4)")
                              .arg(color->red())
                              .arg(color->green())
                              .arg(color->blue())
                              .arg(color->alpha());
      else
        qWarning("Theme style sheet names unknown color ${%s}",
                 qPrintable(name));
      last = match.capturedEnd();
    }
    resolved += QStringView(sheet).mid(last);
    application->setStyleSheet(resolved);
  }
  emit changed();
}

QColor Theme::listingRole(int code) const {
  static constexpr auto map = listingColors();
  return color(map[code >= 0 && code < ListingRoleCount ? code : 0]);
}

QColor Theme::prefixColor(int addressClass) const {
  static constexpr auto map = prefixColors();
  return color(
      map[addressClass >= 0 && addressClass < AddressClassCount ? addressClass
                                                                : 0]);
}

QColor Theme::navigationColor(int addressClass) const {
  static constexpr auto map = navigationColors();
  return color(
      map[addressClass >= 0 && addressClass < AddressClassCount ? addressClass
                                                                : 0]);
}

} // namespace neverd::gui
