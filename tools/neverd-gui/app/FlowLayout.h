#pragma once

#include <QLayout>
#include <QList>

namespace neverd::gui {

/// Lays widgets out in the reading direction at their own sizes, and starts a
/// new row where the next would not fit, as the options of a narrow pane wrap.
/// A box layout squeezes a row it cannot fit, so a widget that keeps its
/// width overlaps the next one.  A hidden widget takes no place.
class FlowLayout final : public QLayout {
public:
  explicit FlowLayout(QWidget *parent = nullptr);
  ~FlowLayout() override;

  void addItem(QLayoutItem *item) override;
  int count() const override;
  QLayoutItem *itemAt(int index) const override;
  QLayoutItem *takeAt(int index) override;
  Qt::Orientations expandingDirections() const override;
  bool hasHeightForWidth() const override;
  int heightForWidth(int width) const override;
  QSize minimumSize() const override;
  QSize sizeHint() const override;
  void setGeometry(const QRect &rect) override;

private:
  /// The height the items take in \p rect, placing them when \p place.
  int arrange(const QRect &rect, bool place) const;
  int horizontalSpacing() const;
  int verticalSpacing() const;

  QList<QLayoutItem *> items_;
};

} // namespace neverd::gui
