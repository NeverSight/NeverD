#pragma once

#include <QProxyStyle>

namespace neverd::gui {

/// The workbench's widget style: Fusion, which renders the palette faithfully
/// on every platform, with keyboard cues as on Windows and GNOME.  Menus,
/// buttons and labels underline their mnemonic letters only while Alt is held,
/// and a menu that Alt and a letter opened keeps them until it closes.  The
/// mnemonics work either way; always drawn, they make every menu title and
/// item look misprinted.  A combo box's list is drawn like a menu, which the
/// theme style sheet reaches through a styled item delegate, and message
/// boxes show the workbench's own icons.
class WorkbenchStyle final : public QProxyStyle {
  Q_OBJECT
public:
  WorkbenchStyle();

  int styleHint(StyleHint hint, const QStyleOption *option = nullptr,
                const QWidget *widget = nullptr,
                QStyleHintReturn *returnData = nullptr) const override;
  int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr) const override;
  QIcon standardIcon(StandardPixmap pixmap,
                     const QStyleOption *option = nullptr,
                     const QWidget *widget = nullptr) const override;
  QPixmap standardPixmap(StandardPixmap pixmap,
                         const QStyleOption *option = nullptr,
                         const QWidget *widget = nullptr) const override;
  using QProxyStyle::polish;
  using QProxyStyle::unpolish;
  void polish(QApplication *application) override;
  void polish(QWidget *widget) override;
  void unpolish(QApplication *application) override;

  /// Whether mnemonic letters are underlined now.
  bool keyboardCues() const { return cues_; }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override;

private:
  void setKeyboardCues(bool on);
  void releaseAfterMenu();

  bool cues_ = false;
  // Alt was released while the menu it opened still showed.
  bool heldByMenu_ = false;
};

} // namespace neverd::gui
