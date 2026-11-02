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
  enum class Mode { Dark, Light };

  static Theme &instance();

  Mode mode() const { return mode_; }
  void setMode(Mode mode);
  static QString modeName(Mode mode);
  static Mode modeFromName(const QString &name);

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
  QFont codeFont_;
  std::array<QColor, static_cast<int>(ColorRole::Count)> colors_;
  QHash<QString, QColor> chrome_;
};

} // namespace neverd::gui
