//===- ProjectStore.h - Agent project persistence ----------------*- C++
//-*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Project-local objects, conversations and portable object memory records.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_TOOLS_AGENT_PROJECT_PROJECTSTORE_H
#define NEVERD_TOOLS_AGENT_PROJECT_PROJECTSTORE_H

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace neverd::agent {
struct AnalysisObject {
  QString Id;
  QString Name;
  QString Path;
  QString Sha256;
};

struct Project {
  QString Id;
  QString Name;
  QString Directory;
  QList<AnalysisObject> Objects;
};

struct FingerprintResult {
  QString Path;
  QString Sha256;
  QString Error;
};

class ProjectStore {
public:
  static QString stateDirectory(const Project &project);
  static QString sessionDirectory(const Project &project,
                                  const QString &sessionId);
  static QString objectDirectory(const Project &project,
                                 const AnalysisObject &object);
  static std::optional<Project> create(const QString &directory,
                                       const QString &name, QString &error);
  static std::optional<Project> open(const QString &directory, QString &error);
  static bool save(const Project &project, QString &error);
  static FingerprintResult fingerprint(const QString &path);
  static bool addObject(Project &project, const FingerprintResult &input,
                        QString &error);
  static bool relocateObject(Project &project, const QString &objectId,
                             const FingerprintResult &input, QString &error);
  static bool readJson(const QString &path, QJsonObject &value, QString &error,
                       bool optional = false);
  static bool writeJson(const QString &path, const QJsonObject &value,
                        QString &error);
  static bool readConversation(const Project &project, QJsonObject &value,
                               QString &error, const QString &sessionId = {});
  static bool saveConversation(const Project &project, const QJsonObject &value,
                               QString &error, const QString &sessionId = {});
  static bool readMemory(const Project &project, const AnalysisObject &object,
                         QJsonArray &records, QString &error);
  static bool appendMemory(const Project &project, const AnalysisObject &object,
                           const QString &text, QString &error);
  static bool exportMemory(const Project &project, const AnalysisObject &object,
                           const QString &destination, QString &error);
  static bool importMemory(const Project &project, const AnalysisObject &object,
                           const QString &source, QString &error);
};
} // namespace neverd::agent

#endif // NEVERD_TOOLS_AGENT_PROJECT_PROJECTSTORE_H
