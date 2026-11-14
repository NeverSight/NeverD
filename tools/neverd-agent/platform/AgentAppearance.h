//===- AgentAppearance.h - The workbench's language and theme ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The Agent window shows the workbench's interface language and theme.
/// refresh() reads them again, so a change made in the workbench reaches the
/// Agent the next time its window becomes active.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_AGENTAPPEARANCE_H
#define NEVERD_TOOLS_AGENT_PLATFORM_AGENTAPPEARANCE_H

#include <QColor>
#include <QObject>
#include <QString>

namespace neverd::agent {

class AgentAppearance final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString language READ language NOTIFY languageChanged)
  Q_PROPERTY(bool rightToLeft READ rightToLeft NOTIFY languageChanged)
  Q_PROPERTY(int themeMode READ themeMode NOTIFY themeChanged)
  Q_PROPERTY(bool light READ light NOTIFY themeChanged)
  Q_PROPERTY(bool highContrast READ highContrast NOTIFY themeChanged)

public:
  /// The columns of the workbench's color tables.
  enum Mode { Dark, Light, HighContrastDark, HighContrastLight };
  Q_ENUM(Mode)

  explicit AgentAppearance(QObject *parent = nullptr);
  /// Read the workbench's language and theme; signal what changed.
  Q_INVOKABLE void refresh();

  QString language() const { return Language; }
  bool rightToLeft() const;
  int themeMode() const { return Shown; }
  bool light() const { return Shown == Light || Shown == HighContrastLight; }
  bool highContrast() const {
    return Shown == HighContrastDark || Shown == HighContrastLight;
  }
  /// A workbench chrome role (ChromeColors.def) in the shown theme.
  Q_INVOKABLE QColor chromeColor(const QString &name, int mode) const;
  /// A workbench code-view role (ThemeColors.def) in the shown theme.
  Q_INVOKABLE QColor themeColor(const QString &name, int mode) const;

signals:
  void languageChanged();
  void themeChanged();

private:
  void showTheme();
  QString Language;
  QString Choice;
  Mode Shown = Dark;
};

} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_AGENTAPPEARANCE_H
