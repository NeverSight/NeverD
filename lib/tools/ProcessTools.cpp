//===- ProcessTools.cpp - Process plugin tool catalog ---------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Publish complete components, deriving parameter contracts from NPH actions.
//===----------------------------------------------------------------------===//

#include "neverd/tools/ProcessTools.h"

#include "neverd/plugin/Config.h"

#include <algorithm>
#include <map>
#include <set>

namespace neverd::tools {
using namespace plugin;
namespace {
bool keys(const Json &Object, const std::set<std::string> &Allowed) {
  if (!Object.is_object())
    return false;
  for (auto I = Object.begin(); I != Object.end(); ++I)
    if (!Allowed.contains(I.key()))
      return false;
  return true;
}
bool identifier(const Json &Value) {
  return Value.is_string() &&
         validIdentifier(Value.get_ref<const std::string &>());
}
} // namespace
std::optional<Owner> installProcessTools(PluginCatalog &Installations,
                                         ToolRegistry &Tools,
                                         const Manifest &Plugin,
                                         const Json &Catalog,
                                         Diagnostic &Error) {
  const auto Fail = [&](std::string Why) -> std::optional<Owner> {
    Error = {"invalid_catalog", std::move(Why)};
    return std::nullopt;
  };
  try {
    if (!keys(Catalog, {"schema", "tools"}) ||
        Catalog.at("schema") != "neverd.tools.v1" ||
        !Catalog.at("tools").is_array() || Catalog["tools"].empty() ||
        Catalog["tools"].size() > limits::kMaxActions)
      return Fail("Invalid tools catalog");
    std::map<std::string, std::string> Names;
    std::set<std::string> Actions;
    std::vector<ProcessActionContribution> Contributions;
    for (const auto &Item : Catalog["tools"]) {
      if (!keys(Item, {"id", "action", "description", "output_schema", "effect",
                       "headless", "commands", "continuation_of"}) ||
          !identifier(Item.at("id")) || !identifier(Item.at("action")) ||
          !Item.at("description").is_string() ||
          (Item.contains("headless") && !Item["headless"].is_boolean()))
        return Fail("Invalid tool declaration");
      const auto Id = Item["id"].get<std::string>();
      const auto Action = Item["action"].get<std::string>();
      if (!Names.emplace(Id, Action).second || !Actions.insert(Action).second)
        return Fail("Catalog tool and action IDs must be unique");
      ActionDefinition Definition;
      Definition.Id = Action;
      Definition.Description = Item["description"];
      Definition.OutputSchema = Item.at("output_schema");
      Definition.Headless = Item.value("headless", true);
      const auto Effect = Item.at("effect").get<std::string>();
      if (Effect == "read")
        Definition.Effect = ActionEffect::Read;
      else if (Effect == "artifact")
        Definition.Effect = ActionEffect::Artifact;
      else if (Effect == "project_write")
        Definition.Effect = ActionEffect::ProjectWrite;
      else
        return Fail("Unsupported action effect");
      Definition.Services = Plugin.Queries;
      std::vector<std::string> Commands;
      if (Item.contains("commands")) {
        if (!Item["commands"].is_array())
          return Fail("Commands must be an array");
        for (const auto &Command : Item["commands"]) {
          if (!identifier(Command))
            return Fail("Invalid command name");
          const auto Name = Command.get<std::string>();
          if (std::find(Commands.begin(), Commands.end(), Name) !=
              Commands.end())
            return Fail("Repeated command");
          Commands.push_back(Name);
          Definition.Services.push_back("command:" + Name);
        }
      }
      Contributions.push_back(
          {std::move(Definition), {true, std::move(Commands)}});
    }
    for (const auto &Item : Catalog["tools"])
      if (Item.contains("continuation_of")) {
        const auto Parent = Item["continuation_of"].get<std::string>();
        if (!Names.contains(Parent) || Parent == Item["id"].get<std::string>())
          return Fail("Continuation requires a different declared tool");
      }
    const bool Active = Installations.install(
        Plugin, Error, std::move(Contributions),
        [&Tools, Catalog, Names](Owner, const auto &Bindings,
                                 Diagnostic &Error) {
          for (const auto &Item : Catalog["tools"]) {
            const auto Id = Item["id"].get<std::string>();
            ToolDefinition Definition{Id, Bindings.at(Names.at(Id)), {}};
            if (Item.contains("continuation_of"))
              Definition.ContinuationOf = Bindings.at(
                  Names.at(Item["continuation_of"].get<std::string>()));
            if (!Tools.add(std::move(Definition), Error))
              return false;
          }
          return true;
        });
    return Active ? Installations.component(Plugin.Id) : std::nullopt;
  } catch (const Json::exception &Exception) {
    return Fail(std::string("Malformed tools catalog: ") + Exception.what());
  }
}
} // namespace neverd::tools
