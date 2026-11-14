//===- AgentWindowFrame.h - Application-painted Quick window ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qt Quick frame adapter using the workbench's native window integration.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_AGENTWINDOWFRAME_H
#define NEVERD_TOOLS_AGENT_PLATFORM_AGENTWINDOWFRAME_H

#include <QColor>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <memory>

class QAbstractNativeEventFilter;
class QWindow;

namespace neverd::agent {
class AgentWindowFrame final : public QObject {
  Q_OBJECT
  Q_PROPERTY(int shadowMargin READ shadowMargin NOTIFY stateChanged)
  Q_PROPERTY(int titleHeight READ titleHeight CONSTANT)
  Q_PROPERTY(int buttonWidth READ buttonWidth CONSTANT)
  Q_PROPERTY(int dragDistance READ dragDistance CONSTANT)
  Q_PROPERTY(bool maximized READ maximized NOTIFY stateChanged)

public:
  explicit AgentWindowFrame(QObject *parent = nullptr);
  ~AgentWindowFrame() override;
  void attach(QWindow &window);
  int shadowMargin() const;
  int titleHeight() const;
  int buttonWidth() const;
  int dragDistance() const;
  bool maximized() const;
  Q_INVOKABLE void minimize();
  Q_INVOKABLE void toggleMaximized();
  Q_INVOKABLE void startMove();
  Q_INVOKABLE QColor chromeColor(const QString &name, bool light) const;
  /// A role of the workbench's code-view colors (ThemeColors.def).
  Q_INVOKABLE QColor themeColor(const QString &name, bool light) const;

signals:
  void stateChanged();

protected:
  bool eventFilter(QObject *object, QEvent *event) override;

private:
  Qt::Edges edgesAt(QPoint point) const;
  void prepareResize();
  void updateState();
  void showEdges(Qt::Edges edges);
  void finishManualGesture();
  QPointer<QWindow> Window;
  std::unique_ptr<QAbstractNativeEventFilter> NativeFilter;
  Qt::Edges ShownEdges;
  Qt::Edges Resizing;
  QPoint GestureOrigin;
  QRect GestureGeometry;
  bool Moving = false;
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_AGENTWINDOWFRAME_H
