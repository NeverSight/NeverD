#pragma once

#include <QColor>
#include <QFont>
#include <QHash>
#include <QObject>
#include <array>

namespace neverd::gui {

enum class ColorRole : int {
#define NEVERD_THEME_COLOR(Role, Light, Dark) Role,
#include "ThemeColors.def"
  Count
};

/// Workbench appearance: the Visual Studio Code Dark+ (default) or Light+
/// palettes for widget chrome and analysis views, and the code font.
class Theme final : public QObject {
  Q_OBJECT
public:
  /// The palette the workbench shows.
  enum class Mode { Dark, Light };
  /// The theme a person chose: one of the workbench's, or the desktop's light
  /// or dark mode, which the workbench follows as it changes.
  enum class Choice { Dark, Light, System };

  static Theme &instance();

  Mode mode() const { return mode_; }
  /// Chooses the theme of \p mode.
  void setMode(Mode mode);
  static QString modeName(Mode mode);
  Choice choice() const { return choice_; }
  void setChoice(Choice choice);
  static QString choiceName(Choice choice);
  static Choice choiceFromName(const QString &name);
  /// Follows the desktop's color scheme, \p scheme, while the person chose
  /// the system's theme; an unknown scheme keeps the theme shown.
  void followColorScheme(Qt::ColorScheme scheme);

  bool dark() const { return mode_ == Mode::Dark; }
  QColor color(ColorRole role) const { return colors_[static_cast<int>(role)]; }
  /// Chrome color by ChromeColors.def name.
  QColor chrome(const QString &name) const { return chrome_.value(name); }
  /// Color of a worker listing span role code (ListingRoles.def).
  QColor listingRole(int code) const;
  /// Listing prefix color of a worker address class (AddressClasses.def).
  QColor prefixColor(int addressClass) const;
  /// Navigation band color of a worker address class.
  QColor navigationColor(int addressClass) const;

  QFont codeFont() const { return codeFont_; }
  void setCodeFont(const QFont &font);
  /// Bundled code face at the platform's default interface reading size.
  static QFont defaultCodeFont();
  /// Grow or shrink the code font by whole points, within sane bounds.
  void zoomCodeFont(int steps);
  void resetCodeFontSize();

  /// Apply the palette and style sheet to the running application.
  void apply();

signals:
  void changed();

private:
  Theme();
  void loadColors();
  Mode mode_ = Mode::Dark;
  Choice choice_ = Choice::Dark;
  QFont codeFont_;
  std::array<QColor, static_cast<int>(ColorRole::Count)> colors_;
  QHash<QString, QColor> chrome_;
};

} // namespace neverd::gui
