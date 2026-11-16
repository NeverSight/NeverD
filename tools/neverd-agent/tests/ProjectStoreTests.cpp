//===- ProjectStoreTests.cpp - Agent persistence regression tests ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify project relocation, isolated memories and atomic failed imports with
/// real temporary files. No binary execution or model service is involved.
//===----------------------------------------------------------------------===//

#include "app/AgentController.h"
#include "platform/AppPaths.h"
#include "project/ProjectStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QUrl>
#include <QtTest>

using namespace neverd::agent;

class ProjectStoreTests : public QObject {
  Q_OBJECT
private slots:
  // Paths a model supplies are judged as text, before any file call could
  // resolve a network path and contact its host.
  void modelPathsNeverReachOtherMachines() {
    for (const auto *remote :
         {"\\\\attacker\\share\\x", "//attacker/share/x", "\\/attacker/x",
          "/\\attacker/x", "\\\\?\\UNC\\host\\s", "file://attacker/share/x",
          " \\\\attacker\\x"})
      QVERIFY2(isRemotePath(QString::fromLatin1(remote)), remote);
    for (const auto *local :
         {"C:\\Windows\\kernel32.dll", "/usr/lib/libc.so", "file:///tmp/x",
          "file://localhost/tmp/x", "notes/a.md", "kernel32.dll"})
      QVERIFY2(!isRemotePath(QString::fromLatin1(local)), local);
    for (const auto *inside : {"notes/a.md", "reports\\r.md", "a/./b.txt"})
      QVERIFY2(isContainedRelativePath(QString::fromLatin1(inside)), inside);
    for (const auto *outside :
         {"", "/etc/passwd", "C:\\x.txt", "C:x.txt", "../secret",
          "notes/../../x", "notes\\..\\..\\x", "//attacker/share/x",
          "\\\\attacker\\share"})
      QVERIFY2(!isContainedRelativePath(QString::fromLatin1(outside)), outside);
  }

  void agentStateIsPrivateInAnyLetterCase() {
    for (const auto *state :
         {".nd-agent", ".nd-agent/project.json", ".ND-Agent/project.json",
          ".nd-agent\\memory.json", "./.Nd-Agent/x", " .nd-agent/x"})
      QVERIFY2(isAgentStatePath(QString::fromLatin1(state)), state);
    for (const auto *other :
         {"notes/.nd-agent/x", ".nd-agent-notes/x", "reports/a.md", "", "."})
      QVERIFY2(!isAgentStatePath(QString::fromLatin1(other)), other);
  }

  void helpersFollowTheConfiguredBundleAndInstallation() {
    QTemporaryDir workspace;
    const QString bundle = workspace.filePath("NeverD with spaces.app");
    const QString programs = QDir(bundle).filePath("Contents/MacOS");
    QVERIFY(QDir().mkpath(programs));
    QCOMPARE(ndProgramDirectory(QUrl::fromLocalFile(bundle).toString()),
             programs);
#ifdef Q_OS_WIN
    const QString suffix = ".exe";
#else
    const QString suffix;
#endif
    const auto mcp = QDir(programs).filePath("neverd-mcp" + suffix);
    const auto worker = QDir(programs).filePath("neverd-worker" + suffix);
    for (const auto &file : {mcp, worker}) {
      QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(), file));
      QVERIFY(QFile::setPermissions(file, QFile::permissions(file) |
                                              QFile::ExeOwner));
    }
    QCOMPARE(findMcpExecutable(bundle, {}), mcp);
    QCOMPARE(findMcpExecutable({}, QUrl::fromLocalFile(mcp).toString()), mcp);
    QCOMPARE(findWorkerExecutable(bundle, {}), worker);
    QCOMPARE(findWorkerExecutable({}, mcp), worker);
    QVERIFY(findMcpExecutable(bundle, workspace.filePath("missing-program"))
                .isEmpty());
    QVERIFY(QFile::remove(worker));
    QVERIFY(findWorkerExecutable(bundle, mcp).isEmpty());
#ifndef Q_OS_WIN
    QVERIFY(QFile::setPermissions(mcp, QFile::ReadOwner | QFile::WriteOwner));
    QVERIFY(findMcpExecutable({}, mcp).isEmpty());
#endif
  }

  void foreignAbsolutePathsSurviveOpenAndSave() {
    QTemporaryDir workspace;
    QString error;
    auto project =
        ProjectStore::create(workspace.filePath("project"), "portable", error);
    QVERIFY(project);
    QFile file(workspace.filePath("object.bin"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("object identity");
    file.close();
    QVERIFY(ProjectStore::addObject(
        *project, ProjectStore::fingerprint(file.fileName()), error));
#ifdef Q_OS_WIN
    const QString foreign = "/opt/research/external object.bin";
#else
    const QString foreign = "E:/research/external object.bin";
#endif
    project->Objects[0].Path = foreign;
    QVERIFY(ProjectStore::save(*project, error));
    auto reopened = ProjectStore::open(project->Directory, error);
    QVERIFY(reopened);
    QCOMPARE(reopened->Objects[0].Path, foreign);
    QVERIFY(ProjectStore::save(*reopened, error));
    QJsonObject document;
    QVERIFY(ProjectStore::readJson(
        QDir(ProjectStore::stateDirectory(*project)).filePath("project.json"),
        document, error));
    QCOMPARE(
        document["objects"].toArray().first().toObject()["path"].toString(),
        foreign);
  }

  void relocatedObjectKeepsMemoryAndRejectsDifferentBytes() {
    QTemporaryDir workspace;
    QString error;
    auto project =
        ProjectStore::create(workspace.filePath("project"), "project", error);
    QVERIFY(project);
    QFile source(workspace.filePath("original.bin"));
    QVERIFY(source.open(QIODevice::WriteOnly));
    source.write("same binary");
    source.close();
    QVERIFY(ProjectStore::addObject(
        *project, ProjectStore::fingerprint(source.fileName()), error));
    const auto original = project->Objects[0];
    QVERIFY(ProjectStore::appendMemory(*project, original, "retained evidence",
                                       error));
    const auto contextPath =
        QDir(ProjectStore::objectDirectory(*project, original))
            .filePath("context.json");
    const QJsonObject context{{"checkpoint", "same object context"}};
    QVERIFY(ProjectStore::writeJson(contextPath, context, error));
    const auto relocated = workspace.filePath("移动后的文件.bin");
    QVERIFY(QFile::rename(source.fileName(), relocated));
    AgentController controller(workspace.filePath("global"));
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.openProject(project->Directory));
    controller.selectObject(original.Id);
    controller.relocateObject(project->Id, original.Id, relocated);
    QVERIFY(controller.createProject("other", workspace.filePath("other")));
    const auto other = controller.activeId();
    QTRY_VERIFY(!controller.busy());
    QCOMPARE(controller.activeId(), other);
    QVERIFY(controller.selectProject(project->Id));
    controller.selectObject(original.Id);
    QCOMPARE(controller.selectedObject()["path"].toString(), relocated);
    QCOMPARE(controller.memory().size(), 1);
    auto reopened = ProjectStore::open(project->Directory, error);
    QVERIFY(reopened);
    QCOMPARE(reopened->Objects[0].Id, original.Id);
    QJsonObject after;
    QVERIFY(ProjectStore::readJson(contextPath, after, error));
    QCOMPARE(after, context);
    QFile different(workspace.filePath("different.bin"));
    QVERIFY(different.open(QIODevice::WriteOnly));
    different.write("different bytes");
    different.close();
    QVERIFY(!ProjectStore::relocateObject(
        *reopened, original.Id, ProjectStore::fingerprint(different.fileName()),
        error));
    QCOMPARE(reopened->Objects[0].Path, relocated);
    QCOMPARE(ProjectStore::open(project->Directory, error)->Objects[0].Path,
             relocated);
  }

  void emptyExistingConfigurationIsNotReset() {
    QTemporaryDir workspace;
    QString error;
    const QString path = workspace.filePath("config.json");
    QVERIFY(ProjectStore::writeJson(path, {}, error));
    AgentController controller(workspace.path());
    QVERIFY(!controller.initialize(error));
    QJsonObject after;
    QVERIFY(ProjectStore::readJson(path, after, error));
    QVERIFY(after.isEmpty());
  }

  void switchingProjectsKeepsDraftsAndAsyncImportsScoped() {
    QTemporaryDir workspace;
    QString error;
    const QString global = workspace.filePath("global");
    QString firstId;
    QString secondId;
    {
      AgentController controller(global);
      QVERIFY(controller.initialize(error));
      QVERIFY(controller.createProject("first", workspace.filePath("first")));
      firstId = controller.activeId();
      controller.setDraft("keep this project draft");
      QFile binary(workspace.filePath("with space.bin"));
      QVERIFY(binary.open(QIODevice::WriteOnly));
      binary.write("asynchronous fingerprint input");
      binary.close();
      controller.addObject(binary.fileName());
      QVERIFY(controller.createProject("second", workspace.filePath("second")));
      secondId = controller.activeId();
      QVERIFY(controller.draft().isEmpty());
      controller.setDraft("second project request");
      QVERIFY(controller.saveRequest(""));
      QTRY_VERIFY(!controller.busy());
      QCOMPARE(controller.activeId(), secondId);
      QCOMPARE(controller.activeProject()["objects"].toList().size(), 0);
      QVERIFY(controller.selectProject(firstId));
      QCOMPARE(controller.draft(), QString("keep this project draft"));
      QCOMPARE(controller.activeProject()["objects"].toList().size(), 1);
      QCOMPARE(controller.messages().size(), 0);
      QVERIFY(controller.prepareClose());
    }
    AgentController reopened(global);
    QVERIFY(reopened.initialize(error));
    QCOMPARE(reopened.activeId(), firstId);
    QCOMPARE(reopened.draft(), QString("keep this project draft"));
    QVERIFY(reopened.selectProject(secondId));
    QCOMPARE(reopened.messages().size(), 1);
    QCOMPARE(reopened.messages()[0].toMap()["text"].toString(),
             QString("second project request"));
  }

  // Removing a project forgets it and deletes nothing: the folder keeps what
  // was typed, and opens again in the same session because its lock went too.
  void removedProjectLeavesTheListAndKeepsItsFolder() {
    QTemporaryDir workspace;
    QString error;
    const QString global = workspace.filePath("global");
    const QString first = workspace.filePath("first");
    QString firstId;
    {
      AgentController controller(global);
      QVERIFY(controller.initialize(error));
      QVERIFY(controller.createProject("first", first));
      firstId = controller.activeId();
      QVERIFY(controller.createProject("second", workspace.filePath("second")));
      QVERIFY(controller.createProject("third", workspace.filePath("third")));
      QVERIFY(controller.selectProject(firstId));
      controller.setDraft("typed before the project was removed");
      QSignalSpy errors(&controller, &AgentController::operationError);
      QVERIFY(controller.removeProject(first));
      QCOMPARE(errors.count(), 0);
      QCOMPARE(controller.projects().size(), 2);
      // The project that followed it is the open one.
      QCOMPARE(controller.activeProject()["name"].toString(),
               QString("second"));
      QVERIFY(controller.draft().isEmpty());
      QVERIFY(
          QFileInfo(QDir(first).filePath(".nd-agent/project.json")).isFile());
      QVERIFY(!controller.removeProject(first));
      QVERIFY(controller.openProject(first));
      QCOMPARE(errors.count(), 0);
      QCOMPARE(controller.activeId(), firstId);
      QCOMPARE(controller.draft(),
               QString("typed before the project was removed"));
      // One that is not open leaves without changing which one is.
      QVERIFY(controller.removeProject(workspace.filePath("third")));
      QCOMPARE(controller.activeId(), firstId);
      QVERIFY(controller.removeProject(first));
      QCOMPARE(controller.activeProject()["name"].toString(),
               QString("second"));
      QCOMPARE(errors.count(), 0);
      QVERIFY(controller.prepareClose());
    }
    AgentController reopened(global);
    QVERIFY(reopened.initialize(error));
    QCOMPARE(reopened.projects().size(), 1);
    QCOMPARE(reopened.unavailableProjects().size(), 0);
    QCOMPARE(reopened.activeProject()["name"].toString(), QString("second"));
  }

  void removingTheLastProjectLeavesNoneOpen() {
    QTemporaryDir workspace;
    QString error;
    const QString global = workspace.filePath("global");
    const QString only = workspace.filePath("only");
    {
      AgentController controller(global);
      QVERIFY(controller.initialize(error));
      QVERIFY(controller.createProject("only", only));
      controller.setDraft("a saved request");
      QVERIFY(controller.saveRequest(""));
      QFile note(QDir(only).filePath("note.txt"));
      QVERIFY(note.open(QIODevice::WriteOnly));
      note.close();
      controller.refreshFiles();
      QCOMPARE(controller.files().size(), 1);
      QVERIFY(controller.removeProject(only));
      QVERIFY(controller.activeId().isEmpty());
      QVERIFY(controller.activeProject().isEmpty());
      QCOMPARE(controller.projects().size(), 0);
      QCOMPARE(controller.messages().size(), 0);
      QCOMPARE(controller.files().size(), 0);
      // Nothing is open to take a draft, and nothing is written for one.
      controller.setDraft("typed with no project open");
      QVERIFY(controller.draft().isEmpty());
      QVERIFY(controller.prepareClose());
    }
    QJsonObject config;
    QVERIFY(ProjectStore::readJson(QDir(global).filePath("config.json"), config,
                                   error));
    QCOMPARE(config["projects"].toArray().size(), 0);
    AgentController reopened(global);
    QVERIFY(reopened.initialize(error));
    QVERIFY(reopened.activeId().isEmpty());
    QVERIFY(reopened.openProject(only));
    QCOMPARE(reopened.messages().size(), 1);
  }

  // A folder deleted outside the Agent: the project leaves the list all the
  // same, and its folder is not made again to hold a draft.
  void aProjectWhoseFolderIsGoneIsRemovedWithoutMakingItAgain() {
#ifdef Q_OS_WIN
    // Windows keeps a folder in place while a file in it is open, and an
    // open project holds its lock file.
    QSKIP("An open project's folder cannot be deleted on Windows.");
#endif
    QTemporaryDir workspace;
    QString error;
    const QString lost = workspace.filePath("lost");
    AgentController controller(workspace.filePath("global"));
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.createProject("kept", workspace.filePath("kept")));
    QVERIFY(controller.createProject("lost", lost));
    controller.setDraft("typed before the folder was deleted");
    QVERIFY(QDir(lost).removeRecursively());
    QSignalSpy errors(&controller, &AgentController::operationError);
    QVERIFY(controller.removeProject(lost));
    QCOMPARE(errors.count(), 0);
    QVERIFY(!QFileInfo::exists(lost));
    QCOMPARE(controller.projects().size(), 1);
    QCOMPARE(controller.activeProject()["name"].toString(), QString("kept"));
    QVERIFY(controller.prepareClose());
    QVERIFY(!QFileInfo::exists(lost));
  }

  // Folders that are gone when the Agent starts stay listed, each on its own
  // row with the reason, until the person removes them or they come back.
  void foldersGoneAtStartStayListedUntilRemoved() {
    QTemporaryDir workspace;
    QString error;
    const QString global = workspace.filePath("global");
    const QString away = workspace.filePath("away");
    const QString gone = workspace.filePath("gone");
    const QString parked = workspace.filePath("parked");
    {
      AgentController controller(global);
      QVERIFY(controller.initialize(error));
      QVERIFY(controller.createProject("kept", workspace.filePath("kept")));
      QVERIFY(controller.createProject("away", away));
      QVERIFY(controller.createProject("gone", gone));
      QVERIFY(controller.prepareClose());
    }
    QVERIFY(QDir().rename(away, parked));
    QVERIFY(QDir(gone).removeRecursively());
    {
      AgentController controller(global);
      QSignalSpy errors(&controller, &AgentController::operationError);
      QVERIFY(controller.initialize(error));
      QCoreApplication::processEvents();
      // The rows say it; no sheet interrupts the start.
      QCOMPARE(errors.count(), 0);
      QCOMPARE(controller.projects().size(), 1);
      const auto unavailable = controller.unavailableProjects();
      QCOMPARE(unavailable.size(), 2);
      QCOMPARE(unavailable[0].toMap()["name"].toString(), QString("away"));
      QCOMPARE(unavailable[0].toMap()["directory"].toString(), away);
      QVERIFY(!unavailable[0].toMap()["issue"].toString().isEmpty());
      // Two missing folders are two entries: removing one keeps the other.
      QVERIFY(controller.removeProject(gone));
      QCOMPARE(controller.unavailableProjects().size(), 1);
      QCOMPARE(controller.unavailableProjects()[0].toMap()["name"].toString(),
               QString("away"));
      QVERIFY(!controller.removeProject(gone));
      QVERIFY(!QFileInfo::exists(gone));
      // Trying one that is still away says why and keeps its row.
      QVERIFY(!controller.openProject(away));
      QCOMPARE(errors.count(), 1);
      QCOMPARE(controller.unavailableProjects().size(), 1);
      QVERIFY(controller.prepareClose());
    }
    QVERIFY(QDir().rename(parked, away));
    {
      AgentController controller(global);
      QVERIFY(controller.initialize(error));
      QCOMPARE(controller.projects().size(), 2);
      QCOMPARE(controller.unavailableProjects().size(), 0);
      QVERIFY(controller.prepareClose());
    }
    QVERIFY(QDir().rename(away, parked));
    AgentController controller(global);
    QVERIFY(controller.initialize(error));
    QCOMPARE(controller.unavailableProjects().size(), 1);
    // It comes back while the Agent runs: a click on its row opens it.
    QVERIFY(QDir().rename(parked, away));
    QVERIFY(controller.openProject(away));
    QCOMPARE(controller.unavailableProjects().size(), 0);
    QCOMPARE(controller.projects().size(), 2);
    QCOMPARE(controller.activeProject()["name"].toString(), QString("away"));
    QVERIFY(controller.prepareClose());
    QJsonObject config;
    QVERIFY(ProjectStore::readJson(QDir(global).filePath("config.json"), config,
                                   error));
    QCOMPARE(config["projects"].toArray().size(), 2);
  }

  void sharedMemoryFollowsToggleWithoutCopyingConversations() {
    QTemporaryDir workspace;
    QString error;
    auto first =
        ProjectStore::create(workspace.filePath("first"), "first", error);
    auto second =
        ProjectStore::create(workspace.filePath("second"), "second", error);
    QVERIFY(first && second);
    QFile binary(workspace.filePath("shared.bin"));
    QVERIFY(binary.open(QIODevice::WriteOnly));
    binary.write("shared identity");
    binary.close();
    const auto fingerprint = ProjectStore::fingerprint(binary.fileName());
    QVERIFY(ProjectStore::addObject(*first, fingerprint, error));
    QVERIFY(ProjectStore::addObject(*second, fingerprint, error));
    QVERIFY(ProjectStore::appendMemory(*first, first->Objects[0],
                                       "source finding", error));
    AgentController controller(workspace.filePath("global"));
    QVERIFY(controller.initialize(error));
    QVERIFY(controller.openProject(first->Directory));
    controller.setDraft("private conversation");
    QVERIFY(controller.saveRequest(""));
    QVERIFY(controller.openProject(second->Directory));
    controller.selectObject(second->Objects[0].Id);
    QCOMPARE(controller.sharedMemory().size(), 0);
    QVERIFY(controller.updateSettings(true, false, ""));
    QCOMPARE(controller.sharedMemory().size(), 1);
    QCOMPARE(controller.memory().size(), 0);
    QCOMPARE(controller.messages().size(), 0);
    QVERIFY(controller.updateSettings(false, false, ""));
    QCOMPARE(controller.sharedMemory().size(), 0);
    QJsonArray original;
    QVERIFY(
        ProjectStore::readMemory(*first, first->Objects[0], original, error));
    QCOMPARE(original.size(), 1);
  }

  void projectReopensAndRelocates() {
    QTemporaryDir workspace;
    QVERIFY(workspace.isValid());
    QString error;
    const auto original = ProjectStore::create(workspace.filePath("研究项目 A"),
                                               "API 研究", error);
    QVERIFY2(original.has_value(), qPrintable(error));
    auto project = *original;
    QFile binary(QDir(project.Directory).filePath("测试 object.bin"));
    QVERIFY(binary.open(QIODevice::WriteOnly));
    binary.write("stable binary bytes");
    binary.close();
    QVERIFY(ProjectStore::addObject(
        project, ProjectStore::fingerprint(binary.fileName()), error));
    auto reopened = ProjectStore::open(project.Directory, error);
    QVERIFY2(reopened.has_value(), qPrintable(error));
    QCOMPARE(reopened->Objects.size(), 1);
    QCOMPARE(reopened->Id, project.Id);
    QVERIFY(!ProjectStore::create(project.Directory, "overwrite", error));
    const QString moved = workspace.filePath("搬迁后的目录");
    QVERIFY(QDir().rename(project.Directory, moved));
    reopened = ProjectStore::open(moved, error);
    QVERIFY2(reopened.has_value(), qPrintable(error));
    QCOMPARE(reopened->Objects[0].Path,
             QDir(moved).absoluteFilePath("测试 object.bin"));
    QCOMPARE(reopened->Objects[0].Sha256, project.Objects[0].Sha256);
  }

  void memoryIsIsolatedPortableAndDeduplicated() {
    QTemporaryDir workspace;
    QString error;
    auto first =
        ProjectStore::create(workspace.filePath("first"), "first", error);
    auto second =
        ProjectStore::create(workspace.filePath("second"), "second", error);
    QVERIFY(first && second);
    QFile binary(workspace.filePath("same.dll"));
    QVERIFY(binary.open(QIODevice::WriteOnly));
    binary.write("one binary, two independent contexts");
    binary.close();
    const auto fingerprint = ProjectStore::fingerprint(binary.fileName());
    QVERIFY(ProjectStore::addObject(*first, fingerprint, error));
    QVERIFY(ProjectStore::addObject(*second, fingerprint, error));
    QVERIFY(first->Objects[0].Id != second->Objects[0].Id);
    QVERIFY(ProjectStore::appendMemory(*first, first->Objects[0],
                                       "Evidence: manual note.", error));
    QJsonArray records;
    QVERIFY(
        ProjectStore::readMemory(*second, second->Objects[0], records, error));
    QCOMPARE(records.size(), 0);
    const QString package = workspace.filePath("记忆.ndmemory.json");
    QVERIFY(
        ProjectStore::exportMemory(*first, first->Objects[0], package, error));
    QVERIFY(ProjectStore::importMemory(*second, second->Objects[0], package,
                                       error));
    QVERIFY(ProjectStore::importMemory(*second, second->Objects[0], package,
                                       error));
    QVERIFY(
        ProjectStore::readMemory(*second, second->Objects[0], records, error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records[0].toObject().value("originProjectId").toString(),
             first->Id);

    QJsonObject conflicting;
    QVERIFY(ProjectStore::readJson(package, conflicting, error));
    auto changed = conflicting["records"].toArray();
    auto record = changed[0].toObject();
    record["text"] = "Conflicting claim under the same provenance ID";
    changed[0] = record;
    conflicting["records"] = changed;
    QVERIFY(ProjectStore::writeJson(package, conflicting, error));
    QVERIFY(!ProjectStore::importMemory(*second, second->Objects[0], package,
                                        error));
    QJsonArray after;
    QVERIFY(
        ProjectStore::readMemory(*second, second->Objects[0], after, error));
    QCOMPARE(after, records);
    conflicting["sha256"] = QString(64, '0');
    QVERIFY(ProjectStore::writeJson(package, conflicting, error));
    QVERIFY(!ProjectStore::importMemory(*second, second->Objects[0], package,
                                        error));
    QVERIFY(
        ProjectStore::readMemory(*second, second->Objects[0], after, error));
    QCOMPARE(after, records);
  }

  void malformedProjectIsRejectedWithoutRewrite() {
    QTemporaryDir workspace;
    QString error;
    auto project =
        ProjectStore::create(workspace.filePath("project"), "project", error);
    QVERIFY(project.has_value());
    const QString path =
        QDir(ProjectStore::stateDirectory(*project)).filePath("project.json");
    QJsonObject invalid;
    QVERIFY(ProjectStore::readJson(path, invalid, error));
    invalid["id"] = "../../outside";
    QVERIFY(ProjectStore::writeJson(path, invalid, error));
    QVERIFY(!ProjectStore::open(project->Directory, error));
    QJsonObject after;
    QVERIFY(ProjectStore::readJson(path, after, error));
    QCOMPARE(after, invalid);
  }
};

QTEST_GUILESS_MAIN(ProjectStoreTests)
#include "ProjectStoreTests.moc"
