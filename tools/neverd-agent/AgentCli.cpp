//===- AgentCli.cpp - Headless NeverD Agent commands ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Serve both Agent frontends or run a request directly from a terminal.

#include "app/AgentController.h"
#include "bridge/AgentWorkbench.h"
#include "bridge/AgentWorkspace.h"
#include "client/AgentProtocol.h"
#include "platform/AppPaths.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QTextStream>
#include <QTimer>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <thread>
#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace neverd::agent {
namespace {
constexpr int kClosePollMs = 20;

class AgentServer final : public QObject {
public:
  AgentServer(const QString &home, QFile &output)
      : Home(home), Output(output) {}

  void dispatch(const QJsonObject &request) {
    if (ExitCode)
      return;
    if (request.value("command") == "initialize") {
      if (Workspace || Workbench ||
          request.value("protocol") != wire::kVersion) {
        reject(tr("Incompatible or repeated NeverD Agent CLI initialization."));
        return;
      }
      const auto publish = [this](const auto &message) { post(message); };
      const auto mode = request.value("mode");
      if (mode == "workspace") {
        Workspace = std::make_unique<AgentWorkspace>(Home, publish);
        QString error;
        if (!Workspace->initialize(
                request.value("project").toString(),
                request.value("interactiveObjectApprovals") == QJsonValue(true),
                error))
          reject(error);
      } else if (mode == "workbench" && request.value("context").isObject()) {
        Workbench = std::make_unique<AgentWorkbench>(Home, publish);
        Workbench->bind(request.value("context").toObject());
      } else {
        reject(tr("Invalid NeverD Agent CLI session mode."));
      }
      return;
    }
    if (!Workspace && !Workbench) {
      reject(tr("Initialize the NeverD Agent CLI session first."));
      return;
    }
    const auto id = request.value("id").toInteger(-1);
    const auto method = request.value("method").toString();
    const auto arguments = request.value("arguments");
    if (request.value("command") != "invoke" || id <= 0 || method.isEmpty() ||
        !arguments.isArray()) {
      reject(tr("Invalid NeverD Agent CLI action request."));
      return;
    }
    const auto args = arguments.toArray();
    const auto result =
        Workspace ? Workspace->dispatch(method, args,
                                        request.value("sideChat").toString())
                  : Workbench->dispatch(method, args);
    QJsonObject reply{{"type", "response"}, {"id", id}};
    if (result)
      reply["result"] = *result;
    else
      reply["error"] =
          tr("Invalid or unavailable Agent action: %1").arg(method);
    post(reply);
  }

  void reject(const QString &error) {
    post({{"type", "error"}, {"fatal", true}, {"text", error}});
  }

  void close() {
    Closing = true;
    const auto status = Workspace   ? Workspace->prepareShutdown()
                        : Workbench ? Workbench->prepareShutdown()
                                    : ShutdownStatus::Ready;
    if (status == ShutdownStatus::Pending) {
      QTimer::singleShot(kClosePollMs, this, &AgentServer::close);
      return;
    }
    if (status == ShutdownStatus::Failed) {
      qWarning("NeverD Agent CLI could not save the session during shutdown.");
      ExitCode = 1;
    }
    QCoreApplication::exit(ExitCode);
  }

private:
  void post(const QJsonObject &message) {
    if (Closing || ExitCode)
      return;
    auto frame = wire::encode(message);
    if (frame.isEmpty()) {
      frame = wire::encode(
          {{"type", "error"},
           {"fatal", true},
           {"text", tr("NeverD Agent CLI response exceeds the frame limit.")}});
      ExitCode = 1;
    }
    if (Output.write(frame) != frame.size() || !Output.flush() ||
        message.value("fatal").toBool())
      ExitCode = 1;
  }

  QString Home;
  QFile &Output;
  std::unique_ptr<AgentWorkspace> Workspace;
  std::unique_ptr<AgentWorkbench> Workbench;
  bool Closing = false;
  int ExitCode = 0;
};

// Only the reader thread blocks on stdin. Each admitted frame is dispatched
// in the event loop before the reader queues another one.
void readRequests(AgentServer &server) {
  QByteArray pending;
  char buffer[4096];
  bool invalid = false;
  while (!invalid) {
#ifdef Q_OS_WIN
    const auto count = _read(_fileno(stdin), buffer, sizeof(buffer));
#else
    const auto count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
#endif
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      invalid = count < 0 || !pending.isEmpty();
      break;
    }
    pending.append(buffer, count);
    while (true) {
      const auto end = pending.indexOf('\n');
      if (end < 0) {
        invalid = pending.size() >= wire::kMaxFrameBytes;
        break;
      }
      const auto document = wire::decode(pending.first(end + 1));
      if (!document.isObject()) {
        invalid = true;
        break;
      }
      pending.remove(0, end + 1);
      QMetaObject::invokeMethod(
          &server,
          [&server, request = document.object()] { server.dispatch(request); },
          Qt::BlockingQueuedConnection);
    }
  }
  QMetaObject::invokeMethod(
      &server,
      [&server, invalid] {
        if (invalid)
          server.reject(QObject::tr("Invalid NeverD Agent CLI input frame."));
        server.close();
      },
      Qt::QueuedConnection);
}

int serve(const QString &home) {
#ifdef Q_OS_WIN
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#else
  std::signal(SIGPIPE, SIG_IGN);
#endif
  QFile output;
  if (!output.open(stdout, QIODevice::WriteOnly)) {
    qCritical("Cannot open NeverD Agent CLI output.");
    return 1;
  }
  AgentServer server(home, output);
  std::thread reader([&server] { readRequests(server); });
  const auto result = QCoreApplication::exec();
  reader.join();
  return result;
}

int run(const QString &home, const QString &project, const QString &object,
        const QString &prompt) {
  QLockFile lock(QDir(home).filePath("agent.lock"));
  AgentController controller(home);
  QString error;
  if (!initializeWorkspace(controller, lock, project, error)) {
    qCritical().noquote() << error;
    return 1;
  }
  QObject::connect(
      &controller, &AgentController::operationError, &controller,
      [](const auto &message) { qCritical().noquote() << message; });
  QObject::connect(&controller, &AgentController::runFinished, &controller,
                   [](const QString &, const QString &text,
                      const QString &error, bool cancelled) {
                     if (!text.isEmpty())
                       QTextStream(stdout) << text << Qt::endl;
                     if (!error.isEmpty())
                       qCritical().noquote() << error;
                     QCoreApplication::exit(cancelled || !error.isEmpty() ? 1
                                                                          : 0);
                   });
  bool started = false;
  const auto start = [&] {
    if (started || controller.credentialBusy())
      return;
    started = true;
    controller.setDraft(prompt);
    if (!controller.sendRequest(object))
      QCoreApplication::exit(1);
  };
  QObject::connect(&controller, &AgentController::credentialChanged,
                   &controller, start);
  QTimer::singleShot(0, &controller, start);
  return QCoreApplication::exec();
}
} // namespace
} // namespace neverd::agent

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QCoreApplication::setApplicationName("neverd-agent-cli");
  QCoreApplication::setApplicationVersion("0.1.0");
  QCoreApplication::setOrganizationName("NeverSight");
  QCommandLineParser parser;
  parser.setApplicationDescription("NeverD Agent headless runtime");
  parser.addHelpOption();
  parser.addVersionOption();
  parser.addPositionalArgument("command", "serve --stdio or run");
  parser.addOption({"stdio", "Serve the Agent protocol on standard streams."});
  parser.addOption({"data-dir", "Agent data directory.", "directory"});
  parser.addOption({"project", "Existing Agent project for run.", "directory"});
  parser.addOption({"object", "Limit run to this analysis object ID.", "id"});
  parser.addOption({"prompt", "Question to analyze with run.", "text"});
  parser.process(app);
  const auto commands = parser.positionalArguments();
  const auto home = neverd::agent::agentHome(parser.value("data-dir"));
  if (commands.size() == 1 && commands.front() == "serve" &&
      parser.isSet("stdio") && !parser.isSet("project") &&
      !parser.isSet("object") && !parser.isSet("prompt"))
    return neverd::agent::serve(home);
  if (commands.size() == 1 && commands.front() == "run" &&
      !parser.isSet("stdio") && !parser.value("project").isEmpty() &&
      !parser.value("prompt").trimmed().isEmpty())
    return neverd::agent::run(home, parser.value("project"),
                              parser.value("object"), parser.value("prompt"));
  parser.showHelp(2);
}
