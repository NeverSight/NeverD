//===- AgentProcess.cpp - Agent CLI subprocess client ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Read Agent protocol output independently of subprocess diagnostics.

#include "AgentProcess.h"

#include "AgentProtocol.h"
#include "platform/AppPaths.h"

#include <QDebug>
#include <QScopedValueRollback>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neverd::agent {
namespace {
constexpr int kProcessCloseGraceMs = 1000;
}

AgentProcess::AgentProcess(QObject *parent) : QObject(parent) {
#ifdef Q_OS_WIN
  Process.setCreateProcessArgumentsModifier(
      [](auto *args) { args->flags |= CREATE_NO_WINDOW; });
#endif
  connect(&Process, &QProcess::readyReadStandardOutput, this,
          &AgentProcess::readOutput);
  connect(&Process, &QProcess::readyReadStandardError, this, [this] {
    const auto diagnostic = Process.readAllStandardError().trimmed();
    if (!diagnostic.isEmpty())
      qWarning().noquote() << QString::fromUtf8(diagnostic);
  });
  connect(&Process, &QProcess::errorOccurred, this, [this](auto error) {
    if (error == QProcess::FailedToStart || error == QProcess::WriteError ||
        error == QProcess::ReadError)
      fail(tr("NeverD Agent CLI: %1").arg(Process.errorString()));
  });
  connect(&Process, &QProcess::finished, this, [this](int, auto) {
    readOutput();
    fail(tr("NeverD Agent CLI disconnected."));
  });
}

AgentProcess::~AgentProcess() { close(); }

bool AgentProcess::start(const QString &directory, const QString &language,
                         QString &error) {
  if (active())
    return false;
  const auto program = findAgentCliExecutable();
  if (program.isEmpty()) {
    error = tr("The installed NeverD Agent CLI is missing.");
    return false;
  }
  Failed = false;
  Input.clear();
  QStringList arguments{"serve", "--stdio"};
  if (!directory.isEmpty())
    arguments.append({"--data-dir", directory});
  if (!language.isEmpty())
    arguments.append({"--language", language});
  Process.start(program, arguments);
  return true;
}

bool AgentProcess::active() const {
  return Process.state() != QProcess::NotRunning;
}

bool AgentProcess::send(const QJsonObject &message) {
  const auto bytes = wire::encode(message);
  return !Failed && active() && !bytes.isEmpty() &&
         Process.write(bytes) == bytes.size();
}

bool AgentProcess::waitForOutput(int milliseconds) {
  return Process.waitForReadyRead(milliseconds);
}

void AgentProcess::readOutput() {
  Input.append(Process.readAllStandardOutput());
  while (!Closing && !Failed) {
    const auto end = Input.indexOf('\n');
    if (end < 0) {
      if (Input.size() >= wire::kMaxFrameBytes)
        fail(tr("NeverD Agent CLI output exceeds the protocol frame limit."));
      return;
    }
    const auto document = wire::decode(Input.first(end + 1));
    Input.remove(0, end + 1);
    if (!document.isObject()) {
      fail(tr("Invalid NeverD Agent CLI response."));
      return;
    }
    emit received(document.object());
  }
}

void AgentProcess::fail(const QString &error) {
  if (Closing || Failed)
    return;
  Failed = true;
  emit failed(error);
}

void AgentProcess::close() {
  if (!active())
    return;
  const QScopedValueRollback closing(Closing, true);
  Process.closeWriteChannel();
  if (!Process.waitForFinished(kProcessCloseGraceMs)) {
    Process.kill();
    Process.waitForFinished(kProcessCloseGraceMs);
  }
  Input.clear();
}
} // namespace neverd::agent
