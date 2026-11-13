//===- ResearchPlan.h - Conversation research steps ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#ifndef NEVERD_TOOLS_AGENT_PROJECT_RESEARCHPLAN_H
#define NEVERD_TOOLS_AGENT_PROJECT_RESEARCHPLAN_H
#include "ProjectStore.h"

namespace neverd::agent {
class ResearchPlan {
public:
  static bool read(const Project &project, const QString &session,
                   QJsonObject &plan, QString &error);
  static bool save(const Project &project, const QString &session,
                   QJsonObject plan, const QString &runId, QString &error);
};
} // namespace neverd::agent
#endif
