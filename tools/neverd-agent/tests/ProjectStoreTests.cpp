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
