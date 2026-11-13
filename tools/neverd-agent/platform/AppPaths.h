//===- AppPaths.h - Portable Agent paths -------------------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// User storage and desktop executable discovery without shell commands.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PLATFORM_APPPATHS_H
#define NEVERD_TOOLS_AGENT_PLATFORM_APPPATHS_H

#include <QString>

namespace neverd::agent {
QString agentHome(const QString &overridePath);
QString findDesktopExecutable(const QString &configuredPath);
QString findAgentExecutable();
QString findAgentCliExecutable();
QString ndProgramDirectory(const QString &location);
QString findMcpExecutable(const QString &guiPath, const QString &mcpPath);
QString findWorkerExecutable(const QString &guiPath, const QString &mcpPath);
QString localPath(const QString &pathOrUrl);
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_APPPATHS_H
