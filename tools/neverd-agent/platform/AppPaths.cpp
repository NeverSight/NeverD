//===- AppPaths.cpp - Portable Agent paths --------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Resolve local data and application paths using Qt's platform facilities.
//===----------------------------------------------------------------------===//

#include "AppPaths.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QStringList>
#include <QUrl>

namespace neverd::agent {
QString localPath(const QString &pathOrUrl) {
  const QUrl url(pathOrUrl);
  return QDir::cleanPath(url.isLocalFile() ? url.toLocalFile() : pathOrUrl);
}

bool isRemotePath(const QString &path) {
  const QString text = path.trimmed();
  // Both separators: Windows reads "\\host", "//host", "\\/host" and
  // "/\\host" alike.
  const auto separator = [&text](int index) {
    return text.size() > index && (text[index] == QLatin1Char('/') ||
                                   text[index] == QLatin1Char('\\'));
  };
  if (separator(0) && separator(1))
    return true;
  const QUrl url(text);
  return url.scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) ==
             0 &&
         !url.host().isEmpty() &&
         url.host().compare(QLatin1String("localhost"), Qt::CaseInsensitive) !=
             0;
}

bool isContainedRelativePath(const QString &relative) {
  QString path = relative.trimmed();
  path.replace(QLatin1Char('\\'), QLatin1Char('/'));
  if (path.isEmpty() || isRemotePath(relative) ||
      path.startsWith(QLatin1Char('/')) || QDir::isAbsolutePath(path) ||
      (path.size() >= 2 && path[1] == QLatin1Char(':')))
    return false;
  for (const auto &step : path.split(QLatin1Char('/')))
    if (step == QLatin1String(".."))
      return false;
  return true;
}

QString agentHome(const QString &overridePath) {
  const auto configured = overridePath.isEmpty()
                              ? qEnvironmentVariable("NEVERD_AGENT_HOME")
                              : overridePath;
  return QFileInfo(configured.isEmpty()
                       ? QDir(QDir::homePath()).filePath(".nd_agent")
                       : localPath(configured))
      .absoluteFilePath();
}

namespace {
QString executableName(const QString &name) {
#ifdef Q_OS_WIN
  return name + ".exe";
#else
  return name;
#endif
}
QString executableFile(const QString &path) {
  const QFileInfo info(path);
  return info.isFile() && info.isExecutable() ? info.absoluteFilePath()
                                              : QString{};
}
QString configuredExecutable(const QString &location, const QString &name) {
  const QString path = localPath(location);
  if (QFileInfo(path).isDir())
    return executableFile(
        QDir(ndProgramDirectory(path))
            .filePath(path.endsWith(".app", Qt::CaseInsensitive)
                          ? name
                          : executableName(name)));
  return executableFile(path);
}
QStringList installationDirectories() {
  const QString app = QCoreApplication::applicationDirPath();
  QStringList directories{app};
#ifdef Q_OS_MACOS
  for (const auto &relative :
       {"../../../NeverD.app/Contents/MacOS",
        "../../../neverd-gui.app/Contents/MacOS",
        "../../../neverd-agent.app/Contents/MacOS", "../../..", "../../../bin",
        "../NeverD.app/Contents/MacOS", "../neverd-gui.app/Contents/MacOS",
        "../neverd-agent.app/Contents/MacOS"})
    directories.append(QDir(app).absoluteFilePath(relative));
#endif
  return directories;
}
QString findProgram(const QString &name, QStringList directories) {
  directories.append(installationDirectories());
  directories.removeDuplicates();
  for (const auto &directory : directories) {
    const auto path =
        executableFile(QDir(directory).filePath(executableName(name)));
    if (!path.isEmpty())
      return path;
  }
  return QStandardPaths::findExecutable(executableName(name));
}
} // namespace

QString ndProgramDirectory(const QString &location) {
  const QString path = localPath(location);
  if (path.endsWith(".app", Qt::CaseInsensitive))
    return QDir(path).absoluteFilePath("Contents/MacOS");
  const QFileInfo info(path);
  return info.isDir() ? info.absoluteFilePath() : info.absolutePath();
}

QString findDesktopExecutable(const QString &configuredPath) {
  if (!configuredPath.isEmpty())
    return configuredExecutable(configuredPath, "neverd-gui");
  return findProgram("neverd-gui", {});
}

QString findAgentExecutable() { return findProgram("neverd-agent", {}); }

QString findAgentCliExecutable() { return findProgram("neverd-agent-cli", {}); }

QString findMcpExecutable(const QString &guiPath, const QString &mcpPath) {
  if (!mcpPath.isEmpty())
    return configuredExecutable(mcpPath, "neverd-mcp");
  QStringList directories;
  const auto gui = guiPath.isEmpty() ? findDesktopExecutable({}) : guiPath;
  if (!gui.isEmpty())
    directories.append(ndProgramDirectory(gui));
  return findProgram("neverd-mcp", directories);
}

QString findWorkerExecutable(const QString &guiPath, const QString &mcpPath) {
  const auto mcp = findMcpExecutable(guiPath, mcpPath);
  // A selected MCP installation owns the matching write worker as well.
  if (!mcp.isEmpty())
    return executableFile(QDir(ndProgramDirectory(mcp))
                              .filePath(executableName("neverd-worker")));
  if (!mcpPath.isEmpty())
    return {};
  QStringList directories;
  const auto gui = guiPath.isEmpty() ? findDesktopExecutable({}) : guiPath;
  if (!gui.isEmpty())
    directories.append(ndProgramDirectory(gui));
  return findProgram("neverd-worker", directories);
}
} // namespace neverd::agent
