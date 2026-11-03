//===- PluginManifest.cpp - External plugin declarations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// External plugin declarations.
///
//===----------------------------------------------------------------------===//

#include "PluginManifest.h"

#include "neverd/plugin/Manifest.h"

#include <QDir>
#include <QJsonDocument>

namespace neverd::gui {
namespace {
neverd::plugin::Json native(const QJsonDocument &Document) {
  const auto Bytes = Document.toJson(QJsonDocument::Compact);
  return neverd::plugin::Json::parse(Bytes.constData(),
                                     Bytes.constData() + Bytes.size());
}
} // namespace

std::optional<GuiPlugin> readGuiPlugin(const QString &Path, QString &Error) {
  std::string Diagnostic;
  auto M = neverd::plugin::loadManifest(
      std::filesystem::u8path(Path.toUtf8().toStdString()), Diagnostic);
  Error = QString::fromStdString(Diagnostic);
  if (!M)
    return std::nullopt;
  return guiPlugin(*M);
}

GuiPlugin guiPlugin(const neverd::plugin::Manifest &M) {
  GuiPlugin Result;
  Result.id = QString::fromStdString(M.Id);
  Result.name = QString::fromStdString(M.Name);
  Result.version = QString::fromStdString(M.Version);
  Result.icon = QString::fromStdString(M.Icon);
  const auto ManifestPath = M.Path.u8string();
  const auto ProgramPath = M.Program.u8string();
  Result.path = QString::fromUtf8(
      reinterpret_cast<const char *>(ManifestPath.data()), ManifestPath.size());
  Result.path = QDir::fromNativeSeparators(Result.path);
  Result.program = QString::fromUtf8(
      reinterpret_cast<const char *>(ProgramPath.data()), ProgramPath.size());
  for (const auto &Argument : M.Arguments)
    Result.arguments.append(QString::fromStdString(Argument));
  for (const auto &Query : M.Queries)
    Result.queries.append(QString::fromStdString(Query));
  Result.actions =
      QJsonDocument::fromJson(QByteArray::fromStdString(M.Actions.dump()))
          .array();
  return Result;
}

bool validatePluginParameters(const QJsonArray &Fields,
                              const QJsonObject &Values, QString &Error) {
  std::string Diagnostic;
  const bool Valid = neverd::plugin::validateParameters(
      native(QJsonDocument(Fields)), native(QJsonDocument(Values)), Diagnostic);
  Error = QString::fromStdString(Diagnostic);
  return Valid;
}

std::optional<QJsonObject> resolvePluginParameters(const QJsonArray &Fields,
                                                   const QJsonObject &User,
                                                   const QJsonObject &Project,
                                                   const QJsonObject &Explicit,
                                                   QString &Error) {
  std::string Diagnostic;
  const auto Values = neverd::plugin::resolveParameters(
      native(QJsonDocument(Fields)), native(QJsonDocument(User)),
      native(QJsonDocument(Project)), native(QJsonDocument(Explicit)),
      Diagnostic);
  Error = QString::fromStdString(Diagnostic);
  if (!Values)
    return std::nullopt;
  return QJsonDocument::fromJson(QByteArray::fromStdString(Values->dump()))
      .object();
}

bool validatePluginResult(const QJsonObject &Result, QString &Error) {
  std::string Diagnostic;
  const bool Valid =
      neverd::plugin::validateResult(native(QJsonDocument(Result)), Diagnostic);
  Error = QString::fromStdString(Diagnostic);
  return Valid;
}
} // namespace neverd::gui
