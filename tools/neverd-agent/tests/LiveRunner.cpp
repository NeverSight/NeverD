//===- LiveRunner.cpp - Explicit model integration runner -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the same controller as the GUI. Secrets enter only through stdin.
//===----------------------------------------------------------------------===//
#include "app/AgentController.h"
#include "project/MemoryStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTextStream>
#include <QTimer>
#include <cstdio>

using namespace neverd::agent;
int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  QFile input;
  input.open(stdin, QIODevice::ReadOnly);
  QJsonParseError parse;
  const auto document = QJsonDocument::fromJson(input.readAll(), &parse);
  if (parse.error != QJsonParseError::NoError || !document.isObject())
    return 2;
  const auto options = document.object();
  AgentController controller(options["dataDir"].toString());
  QString error;
  bool failed = false;
  QObject::connect(&controller, &AgentController::operationError, &app,
                   [&](const QString &text) {
                     failed = true;
                     QTextStream(stderr) << text << '\n';
                   });
  const auto waitForCredentials = [&] {
    QEventLoop loop;
    const auto connection = QObject::connect(
        &controller, &AgentController::credentialChanged, &loop, [&] {
          if (!controller.credentialBusy())
            loop.quit();
        });
    if (controller.credentialBusy())
      loop.exec();
    QObject::disconnect(connection);
  };
  if (!controller.initialize(error)) {
    QTextStream(stderr) << error;
    return 2;
  }
  waitForCredentials();
  if (options["model"].isObject() &&
      !controller.updateModelSettings(
          options["model"].toObject().toVariantMap(),
          options["apiKey"].toString(), false, options["mcp"].toString()))
    return 2;
  waitForCredentials();
  if (failed || !controller.credentialStatus().isEmpty()) {
    QTextStream(stderr) << controller.credentialStatus();
    return 2;
  }
  if (options["configureOnly"].toBool()) {
    QTextStream(stdout) << "Configuration saved; credential omitted.\n";
    return 0;
  }
  const QString projectPath = options["project"].toString();
  if (QFileInfo(QDir(projectPath).filePath(".nd-agent/project.json"))
          .exists()) {
    if (!controller.openProject(projectPath))
      return 2;
  } else if (!controller.createProject("Agent live validation", projectPath))
    return 2;
  QJsonObject completed;
  bool awaitingMemory = false;
  auto finish = [&] {
    completed["memoryStatus"] = controller.memoryStatus();
    completed["projectMemory"] =
        QJsonArray::fromVariantList(controller.projectMemory());
    QJsonArray memories;
    const auto project = ProjectStore::open(projectPath, error);
    if (project)
      for (const auto &object : project->Objects) {
        QJsonArray records;
        if (MemoryStore::read(*project, object.Id, records, error, true))
          memories.append(QJsonObject{{"objectId", object.Id},
                                      {"name", object.Name},
                                      {"records", records}});
      }
    completed["objectMemories"] = memories;
    QString writeError;
    if (!ProjectStore::writeJson(options["output"].toString(), completed,
                                 writeError)) {
      QTextStream(stderr) << writeError;
      app.exit(2);
      return;
    }
    const auto issue = completed["error"].toString();
    QTextStream(stdout) << (issue.isEmpty() ? "Run succeeded.\n"
                                            : "Run failed: " + issue + "\n");
    app.exit(issue.isEmpty() ? 0 : 1);
  };
  auto maintain = [&] {
    const auto project = ProjectStore::open(projectPath, error);
    if (!controller.generateMemory() || !project ||
        MemoryLearner::pending(*project, error).isEmpty()) {
      finish();
      return;
    }
    controller.organizeMemory(false);
  };
  QObject::connect(&controller, &AgentController::memoryChanged, &app, [&] {
    if (awaitingMemory && !controller.memoryWorking())
      QTimer::singleShot(0, &app, maintain);
  });
  auto start = [&] {
    if (options["memoryOnly"].toBool()) {
      awaitingMemory = true;
      maintain();
      return;
    }
    controller.setDraft(options["prompt"].toString());
    if (!controller.sendRequest(""))
      app.exit(2);
  };
  QObject::connect(&controller, &AgentController::runFinished, &app,
                   [&](const QString &, const QString &text,
                       const QString &issue, bool cancelled) {
                     completed = {{"text", text},
                                  {"error", issue},
                                  {"cancelled", cancelled},
                                  {"progress", QJsonArray::fromVariantList(
                                                   controller.progress())}};
                     if (options["waitForMemory"].toBool() && issue.isEmpty() &&
                         controller.generateMemory()) {
                       awaitingMemory = true;
                       QTimer::singleShot(0, &app, maintain);
                     } else
                       finish();
                   });
  QObject::connect(&controller, &AgentController::runChanged, &app, [&] {
    for (const auto &entry : controller.progress()) {
      const auto row = entry.toMap();
      QTextStream(stdout) << row["label"].toString() << " | "
                          << row["state"].toString() << " | "
                          << row["activity"].toString() << '\n';
    }
  });
  if (controller.activeProject()["objects"].toList().isEmpty() &&
      !options["binary"].toString().isEmpty()) {
    QObject::connect(&controller, &AgentController::busyChanged, &app, [&] {
      if (!controller.busy())
        QTimer::singleShot(0, &app, start);
    });
    controller.addObject(options["binary"].toString());
  } else
    QTimer::singleShot(0, &app, start);
  return app.exec();
}
