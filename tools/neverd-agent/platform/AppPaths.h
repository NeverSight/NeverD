//===- AppPaths.h - Portable Agent paths ------------------------*- C++ -*-===//
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
/// Whether \p path names a file on another machine: a UNC or device path
/// ("\\host\share", "//host/share", "\\?\UNC\...") or a file URL with a
/// host. Windows authenticates to that machine as soon as such a path is
/// resolved, so a path a model supplied is checked before any file call.
bool isRemotePath(const QString &path);
/// Whether \p relative stays inside the directory it is resolved against:
/// not empty, not absolute, no drive letter, no ".." step and not remote.
bool isContainedRelativePath(const QString &relative);
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PLATFORM_APPPATHS_H
