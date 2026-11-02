//===- LineageGraph.h - Source comparison page diagram ----------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A navigable diagram of one explicit source-report page. Placement conveys
/// no chronology; the companion table preserves full keyboard access.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_GUI_LINEAGEGRAPH_H
#define NEVERD_GUI_LINEAGEGRAPH_H

#include <QGraphicsView>
#include <QJsonArray>
#include <QMap>

namespace neverd::gui {
class LineageGraph final : public QGraphicsView {
  Q_OBJECT
public:
  explicit LineageGraph(QWidget *Parent = nullptr);
  void setReports(QJsonArray Reports);
  void setLabels(const QMap<QString, QString> &Labels);
  void fit();
  void zoom(bool In);

signals:
  void versionActivated(const QString &ID);
  void reportActivated(const QString &ID);

protected:
  void mouseDoubleClickEvent(QMouseEvent *Event) override;
  void wheelEvent(QWheelEvent *Event) override;
  void resizeEvent(QResizeEvent *Event) override;
  void changeEvent(QEvent *Event) override;

private:
  bool AutoFit = true;
  QJsonArray Reports;
  QMap<QString, QString> Labels;
  void rebuild();
};
} // namespace neverd::gui

#endif // NEVERD_GUI_LINEAGEGRAPH_H
