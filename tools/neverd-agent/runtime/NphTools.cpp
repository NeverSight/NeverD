//===- NphTools.cpp - Agent consumer of shared ND tools -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep action validation, cancellation and receipts in NPH, not the model SDK.
//===----------------------------------------------------------------------===//
#include "NphTools.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>

namespace neverd::agent {
plugin::Json NphTools::json(const QJsonObject &object) {
  return plugin::Json::parse(
      QJsonDocument(object).toJson(QJsonDocument::Compact).toStdString());
}
QJsonObject NphTools::object(const plugin::Json &value) {
  return QJsonDocument::fromJson(QByteArray::fromStdString(value.dump()))
      .object();
}
void NphTools::succeed(const std::shared_ptr<plugin::ActionCall> &call,
                       const QJsonObject &result) {
  call->succeed(json(result));
}
QJsonObject NphTools::stringParameter(const QString &id, const QString &title,
                                      int maxLength) {
  return {{"id", id},
          {"title", title},
          {"type", "string"},
          {"default", ""},
          {"maxLength", maxLength}};
}
NphTools::NphTools(QString project, QString session, QObject *parent)
    : QObject(parent), Host([](auto work) {
        QTimer::singleShot(0, QCoreApplication::instance(), std::move(work));
      }),
      Actions(Host),
      Registry(
          Host, Actions, [this] { return Environment; }, {},
          [this](const tools::Operation &operation) {
            if (!plugin::terminal(operation.Result.Phase))
              return;
            const QString id = QString::fromStdString(operation.CallId);
            auto done = Pending.take(id);
            if (!done)
              return;
            Operations.removeAll(QString::fromStdString(operation.Id));
            auto receipt = object(tools::describeOperation(operation));
            if (Receipt)
              Receipt(receipt);
            if (operation.Result.Phase == plugin::ActionPhase::Succeeded)
              done(object(operation.Result.Data));
            else
              done({{"error",
                     QString::fromStdString(operation.Result.Error.Message)},
                    {"cancelled", operation.Result.Phase ==
                                      plugin::ActionPhase::Cancelled}});
          }),
      Session(std::move(session)) {
  plugin::Diagnostic error;
  auto owner = Host.createOwner(Host.root(), plugin::OwnerKind::Project,
                                project.toStdString(), error);
  if (!owner || !Host.activate(*owner, error)) {
    SetupError = QString::fromStdString(error.Message);
    return;
  }
  Environment.Project = *owner;
  Environment.Context = {{"loaded", true},
                         {"project_id", project.toStdString()},
                         {"epoch", Session.toStdString()},
                         {"revision", "0"},
                         {"profile", "agent"}};
  auto component =
      Host.createOwner(*owner, plugin::OwnerKind::Component, "agent", error);
  if (!component)
    SetupError = QString::fromStdString(error.Message);
  else
    Component = *component;
}
bool NphTools::add(const QString &name, const QString &description,
                   const QJsonArray &parameters, Handler handler,
                   QString &error, plugin::ActionEffect effect,
                   bool concurrent) {
  if (!SetupError.isEmpty()) {
    error = SetupError;
    return false;
  }
  plugin::ActionDefinition definition;
  definition.PluginId = "neverd-agent";
  definition.Id = name.toStdString();
  definition.Title = definition.Id;
  definition.Version = "1";
  definition.Description = description.toStdString();
  definition.Parameters = plugin::Json::parse(
      QJsonDocument(parameters).toJson(QJsonDocument::Compact).toStdString());
  // ND owns the nested payload contract. The adapter declares openness
  // explicitly rather than pretending all discovered results share a schema.
  definition.OutputSchema = {{"type", "object"},
                             {"properties", plugin::Json::object()},
                             {"additionalProperties", true}};
  definition.Effect = effect;
  plugin::Diagnostic diagnostic;
  auto action = Actions.add(Component, std::move(definition),
                            std::move(handler), diagnostic);
  auto tool = action
                  ? Registry.add({name.toStdString(), *action, {}}, diagnostic)
                  : std::nullopt;
  if (!tool) {
    error = QString::fromStdString(diagnostic.Message);
    SetupError = name + ": " + error;
    Host.failActivation(Component, diagnostic.Message);
    return false;
  }
  if (concurrent)
    Concurrent.insert(name);
  return true;
}
bool NphTools::activate(QString &error) {
  if (!SetupError.isEmpty()) {
    error = SetupError;
    return false;
  }
  plugin::Diagnostic diagnostic;
  if (Host.activate(Component, diagnostic))
    return true;
  error = QString::fromStdString(diagnostic.Message);
  return false;
}
QJsonArray NphTools::resolve(int step, QString &error) {
  Set.release();
  Tokens.clear();
  plugin::Diagnostic diagnostic;
  auto set = Registry.resolve(Session.toStdString(), std::to_string(step),
                              plugin::Json::object(), plugin::Json::object(),
                              diagnostic);
  if (!set) {
    error = QString::fromStdString(diagnostic.Message);
    return {};
  }
  Set = *set;
  QJsonArray result;
  for (const auto &tool : Set.tools()) {
    if (!tool.Unavailable.Code.empty())
      continue;
    auto value = object(tools::describeTool(tool));
    const QString name = QString::fromStdString(tool.Tool.Id);
    value["name"] = name;
    Tokens.insert(name, tool.Token);
    result.append(value);
  }
  return result;
}
void NphTools::call(const ModelToolCall &call, Done done) {
  if (!Tokens.contains(call.Name)) {
    done({{"error", "Tool is absent from this model step."}});
    return;
  }
  Pending.insert(call.Id, std::move(done));
  plugin::Diagnostic error;
  const auto operation =
      Registry.call(Set, Tokens[call.Name], call.Id.toStdString(),
                    json(call.Arguments), "", error);
  if (!operation) {
    auto completion = Pending.take(call.Id);
    if (completion)
      completion({{"error", QString::fromStdString(error.Message)}});
  } else if (Pending.contains(call.Id))
    Operations.append(QString::fromStdString(*operation));
}
void NphTools::cancel() {
  const auto operations = Operations;
  for (const auto &operation : operations)
    Registry.cancel(operation.toStdString());
}
void NphTools::close(std::function<void()> done) {
  cancel();
  Set.release();
  Host.close(Host.root(), [done = std::move(done)](const auto &) { done(); });
}
} // namespace neverd::agent
