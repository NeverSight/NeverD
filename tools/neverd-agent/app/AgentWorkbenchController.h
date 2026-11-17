//===- AgentWorkbenchController.h - Workbench conversations ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_APP_AGENTWORKBENCHCONTROLLER_H
#define NEVERD_TOOLS_AGENT_APP_AGENTWORKBENCHCONTROLLER_H

#include "project/ProjectStore.h"

namespace neverd::agent {

struct WorkbenchConversation {
  QString Directory, Name;
  FingerprintResult Input;
};

std::optional<WorkbenchConversation>
resolveWorkbenchConversation(const QString &dataDirectory,
                             const QJsonObject &binding, QString &error);

bool recoverWorkbenchInputs(QJsonObject &conversation, QString &error);

} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_APP_AGENTWORKBENCHCONTROLLER_H
