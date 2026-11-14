//===- ChatTranscript.h - Floating Agent messages -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGINS_AGENT_UI_CHATTRANSCRIPT_H
#define NEVERD_PLUGINS_AGENT_UI_CHATTRANSCRIPT_H

#include <QCoreApplication>
#include <QList>
#include <QScrollArea>
#include <functional>

class QJsonArray;
class QUrl;
class QVBoxLayout;

namespace neverd::gui::pluginui {

/// The outline of a user message, which the panel's style sheet draws in the
/// workbench's border color (an outline the high contrast themes need).
inline constexpr int BubbleBorderWidth = 1;

class ChatTranscript final : public QScrollArea {
  Q_DECLARE_TR_FUNCTIONS(neverd::gui::pluginui::ChatTranscript)

public:
  using OpenLink = std::function<void(const QUrl &)>;
  using RequestExecution = std::function<void(const QString &)>;

  ChatTranscript(OpenLink openLink, RequestExecution requestExecution,
                 QWidget *parent);
  void replaceTail(qsizetype first, const QJsonArray &entries);
  void updateExecution(const QString &runId, qsizetype first,
                       const QJsonArray &records);
  bool empty() const { return Bubbles.isEmpty(); }

protected:
  void changeEvent(QEvent *event) override;

private:
  class Bubble;
  bool viewportEvent(QEvent *event) override;
  int messageWidth() const;

  OpenLink Navigate;
  RequestExecution RequestLog;
  QVBoxLayout *Messages = nullptr;
  QList<Bubble *> Bubbles;
  bool FollowingTail = true;
};
} // namespace neverd::gui::pluginui

#endif // NEVERD_PLUGINS_AGENT_UI_CHATTRANSCRIPT_H
