//===- ChatTranscript.h - Floating Agent messages -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_PLUGINS_AGENT_UI_CHATTRANSCRIPT_H
#define NEVERD_PLUGINS_AGENT_UI_CHATTRANSCRIPT_H

#include <QList>
#include <QScrollArea>
#include <functional>

class QJsonArray;
class QUrl;
class QVBoxLayout;

namespace neverd::gui::pluginui {

class ChatTranscript final : public QScrollArea {
public:
  using OpenLink = std::function<void(const QUrl &)>;
  using RequestExecution = std::function<void(const QString &)>;

  ChatTranscript(OpenLink openLink, RequestExecution requestExecution,
                 QWidget *parent);
  void replaceTail(qsizetype first, const QJsonArray &entries);
  void updateExecution(const QString &runId, qsizetype first,
                       const QJsonArray &records);

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
