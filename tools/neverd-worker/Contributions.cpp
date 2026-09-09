#include "Contributions.h"

#include <filesystem>
#include <fstream>
#include <set>

namespace neverd::worker {
namespace {
void fields(const Json &object, std::initializer_list<const char *> allowed) {
  if (!object.is_object())
    throw Error("invalid_manifest", "Manifest component must be an object");
  for (auto it = object.begin(); it != object.end(); ++it) {
    bool found = false;
    for (const auto *name : allowed)
      if (it.key() == name)
        found = true;
    if (!found)
      throw Error("invalid_manifest",
                  "Unsupported manifest field: " + it.key());
  }
}
bool identifier(const std::string &text) {
  return !text.empty() && text.size() <= 64 && text[0] >= 'a' &&
         text[0] <= 'z' &&
         text.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_.-") ==
             std::string::npos;
}
Json validateQuery(Json query) {
  fields(query, {"operation", "payload"});
  const auto operation = stringField(query, "operation", {}, 32);
  auto payload = query.value("payload", Json::object());
  if (operation == "metadata")
    fields(payload, {});
  else if (operation == "functions" || operation == "strings")
    fields(payload, {"filter", "offset", "limit"});
  else if (operation == "disasm")
    fields(payload, {"address", "limit"});
  else if (operation == "decompile")
    fields(payload, {"address", "representation", "offset", "limit"});
  else if (operation == "xrefs")
    fields(payload, {"address", "direction", "offset", "limit"});
  else
    throw Error(
        "invalid_manifest",
        "Contributions may only use whitelisted read-only query templates");
  if (operation == "disasm" || operation == "decompile" ||
      operation == "xrefs") {
    const auto address = stringField(payload, "address", "${address}", 32);
    if (address != "${address}")
      parseAddress(address);
    payload["address"] = address;
  }
  if (payload.contains("limit") &&
      sizeField(payload, "limit", 128, operation == "decompile" ? 2048 : 512) ==
          0)
    throw Error("invalid_manifest", "Query limit must be positive");
  if (payload.contains("offset"))
    (void)sizeField(payload, "offset", 0, 1000000000);
  if (payload.contains("filter"))
    (void)stringField(payload, "filter", {}, 256);
  if (payload.contains("representation")) {
    const auto rep = stringField(payload, "representation", "c", 16);
    if (rep != "c" && rep != "low" && rep != "med" && rep != "high" &&
        rep != "llvm")
      throw Error("invalid_manifest",
                  "Unsupported representation in query template");
  }
  if (payload.contains("direction")) {
    const auto direction = stringField(payload, "direction", "to", 8);
    if (direction != "to" && direction != "from")
      throw Error("invalid_manifest", "Invalid xref direction");
  }
  return {{"operation", operation}, {"payload", payload}};
}
} // namespace

Contributions::Contributions() {
  manifests_["neverd"] = {
      {"schema_version", 1},
      {"namespace", "neverd"},
      {"version", "1.0"},
      {"contributions",
       Json::array(
           {{{"id", "neverd:metadata"},
             {"title", "Image metadata"},
             {"kind", "panel"},
             {"query",
              {{"operation", "metadata"}, {"payload", Json::object()}}}},
            {{"id", "neverd:functions"},
             {"title", "Functions"},
             {"kind", "table"},
             {"query",
              {{"operation", "functions"}, {"payload", {{"limit", 128}}}}}},
            {{"id", "neverd:strings"},
             {"title", "Strings"},
             {"kind", "table"},
             {"query",
              {{"operation", "strings"}, {"payload", {{"limit", 128}}}}}}})}};
}

Json Contributions::listing() const {
  Json items = Json::array();
  for (const auto &[nameSpace, manifest] : manifests_) {
    for (const auto &descriptor : manifest.at("contributions")) {
      auto item = descriptor;
      item["namespace"] = nameSpace;
      item["version"] = manifest.at("version");
      items.push_back(std::move(item));
    }
  }
  return {{"schema_version", 1},
          {"registry_revision", std::to_string(revision_)},
          {"revision", std::to_string(revision_)},
          {"items", items},
          {"complete", true}};
}

Json Contributions::registerFile(const std::string &pathText) {
  try {
    if (pathText.empty())
      throw Error("invalid_manifest", "Manifest path is required");
    const auto path = std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t *>(pathText.data()), pathText.size()));
    std::error_code ec;
    const auto bytes = std::filesystem::file_size(path, ec);
    if (ec || bytes > 65536)
      throw Error(
          "invalid_manifest",
          "Manifest must be an accessible JSON file no larger than 64 KiB");
    std::ifstream input(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(bytes) + 1, '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(input.gcount()));
    if (text.size() > bytes)
      throw Error("invalid_manifest",
                  "Manifest changed while it was being read");
    auto manifest = parseJson(text);
    fields(manifest,
           {"schema_version", "namespace", "version", "contributions"});
    if (manifest.value("schema_version", 0) != 1)
      throw Error("invalid_manifest",
                  "Only contribution schema version 1 is supported");
    const auto nameSpace = stringField(manifest, "namespace", {}, 64);
    if (!identifier(nameSpace) || nameSpace == "neverd")
      throw Error("invalid_manifest",
                  "Namespace must be a non-reserved lowercase identifier");
    if (stringField(manifest, "version", {}, 32).empty())
      throw Error("invalid_manifest", "Manifest version is required");
    if (!manifest.contains("contributions") ||
        !manifest["contributions"].is_array() ||
        manifest["contributions"].empty() ||
        manifest["contributions"].size() > 32)
      throw Error("invalid_manifest",
                  "A manifest must contain 1 to 32 contributions");
    std::set<std::string> ids;
    for (auto &item : manifest["contributions"]) {
      fields(item, {"id", "kind", "title", "query", "columns"});
      const auto id = stringField(item, "id", {}, 129);
      if (!id.starts_with(nameSpace + ":") ||
          !identifier(id.substr(nameSpace.size() + 1)) ||
          !ids.insert(id).second)
        throw Error(
            "invalid_manifest",
            "Contribution IDs must be unique and prefixed by their namespace");
      const auto kind = stringField(item, "kind", {}, 16);
      if (kind != "command" && kind != "panel" && kind != "table")
        throw Error("invalid_manifest",
                    "Contribution kind must be command, panel or table");
      if (stringField(item, "title", {}, 256).empty())
        throw Error("invalid_manifest", "Contribution title is required");
      if (!item.contains("query"))
        throw Error("invalid_manifest",
                    "Each contribution requires a read-only query");
      item["query"] = validateQuery(item["query"]);
      if (item.contains("columns")) {
        if (kind != "table" || !item["columns"].is_array() ||
            item["columns"].size() > 16)
          throw Error("invalid_manifest",
                      "Only tables may declare up to 16 columns");
        for (const auto &column : item["columns"]) {
          fields(column, {"key", "title"});
          if (!identifier(stringField(column, "key", {}, 64)) ||
              stringField(column, "title", {}, 128).empty())
            throw Error("invalid_manifest", "Invalid table column");
        }
      }
    }
    std::size_t count = manifest["contributions"].size();
    for (const auto &[otherNamespace, other] : manifests_)
      if (otherNamespace != nameSpace)
        count += other.at("contributions").size();
    if (count > 128 ||
        (!manifests_.contains(nameSpace) && manifests_.size() >= 16))
      throw Error(
          "budget_exceeded",
          "Contribution registry is limited to 16 namespaces / 128 items");
    manifests_[nameSpace] = std::move(manifest);
    ++revision_;
    return listing();
  } catch (const Json::exception &) {
    throw Error("invalid_manifest",
                "Manifest fields have invalid types or missing values");
  }
}

Json Contributions::remove(const std::string &nameSpace) {
  if (nameSpace == "neverd")
    throw Error("invalid_manifest", "Built-in contributions cannot be removed");
  if (manifests_.erase(nameSpace) == 0)
    throw Error("not_found", "Contribution namespace is not registered");
  ++revision_;
  return listing();
}

Json Contributions::query(const std::string &id,
                          const std::string &address) const {
  for (const auto &[nameSpace, manifest] : manifests_) {
    for (const auto &item : manifest.at("contributions")) {
      if (item.at("id") != id)
        continue;
      auto query = item.at("query");
      if (query["payload"].value("address", "") == "${address}")
        query["payload"]["address"] = hexAddress(parseAddress(address));
      return query;
    }
  }
  throw Error("not_found", "Contribution ID is not registered");
}
} // namespace neverd::worker
