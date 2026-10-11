//===- Backend.cpp - Shared offline web C API client ----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Shared offline analysis adapter for native C++ transports.
///
//===----------------------------------------------------------------------===//

#include "Backend.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <memory>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace neverd::web_client {
using transport::Error;
using transport::parseJson;
using transport::sizeField;
using transport::stringField;
namespace {
// Optional additive API: a worker linked to an older engine must still run
// native requests. Resolve only already-loaded engine symbols, never a path.
void *symbol(const char *name) {
#ifdef _WIN32
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&neverd_session_create),
                          &module))
    return nullptr;
  return reinterpret_cast<void *>(GetProcAddress(module, name));
#else
  return dlsym(RTLD_DEFAULT, name);
#endif
}

struct API {
#define WEB_API(NAME)                                                          \
  decltype(&::NAME) NAME = reinterpret_cast<decltype(&::NAME)>(symbol(#NAME));
  WEB_API(neverd_web_session_create)
  WEB_API(neverd_web_session_destroy)
  WEB_API(neverd_web_capabilities_json)
  WEB_API(neverd_web_native_open_json)
  WEB_API(neverd_web_native_metadata_json)
  WEB_API(neverd_web_native_analyze_json)
  WEB_API(neverd_web_import_preview_json)
  WEB_API(neverd_web_import_commit_json)
  WEB_API(neverd_web_metadata_json)
  WEB_API(neverd_web_artifacts_json)
  WEB_API(neverd_web_bun_extract_json)
  WEB_API(neverd_web_bun_records_json)
  WEB_API(neverd_web_sea_extract_json)
  WEB_API(neverd_web_sea_records_json)
  WEB_API(neverd_web_packages_analyze_json)
  WEB_API(neverd_web_package_archive_extract_json)
  WEB_API(neverd_web_package_archive_records_json)
  WEB_API(neverd_web_package_integrity_verify_json)
  WEB_API(neverd_web_har_preview_json)
  WEB_API(neverd_web_stream_preview_json)
  WEB_API(neverd_web_stream_commit_json)
  WEB_API(neverd_web_stream_records_json)
  WEB_API(neverd_web_har_commit_json)
  WEB_API(neverd_web_har_records_json)
  WEB_API(neverd_web_interfaces_analyze_json)
  WEB_API(neverd_web_interface_records_json)
  WEB_API(neverd_web_interfaces_compare_json)
  WEB_API(neverd_web_interface_correlation_records_json)
  WEB_API(neverd_web_package_records_json)
  WEB_API(neverd_web_packages_compare_json)
  WEB_API(neverd_web_package_diff_records_json)
  WEB_API(neverd_web_asar_extract_json)
  WEB_API(neverd_web_electron_manifest_analyze_json)
  WEB_API(neverd_web_electron_source_analyze_json)
  WEB_API(neverd_web_electron_source_records_json)
  WEB_API(neverd_web_electron_ipc_analyze_json)
  WEB_API(neverd_web_electron_ipc_records_json)
  WEB_API(neverd_web_electron_entries_analyze_json)
  WEB_API(neverd_web_electron_entry_records_json)
  WEB_API(neverd_web_html_analyze_json)
  WEB_API(neverd_web_html_records_json)
  WEB_API(neverd_web_asar_records_json)
  WEB_API(neverd_web_source_analyze_json)
  WEB_API(neverd_web_source_nodes_json)
  WEB_API(neverd_web_source_bindings_analyze_json)
  WEB_API(neverd_web_source_binding_records_json)
  WEB_API(neverd_web_source_semantics_analyze_json)
  WEB_API(neverd_web_source_semantic_records_json)
  WEB_API(neverd_web_source_modules_analyze_json)
  WEB_API(neverd_web_source_module_records_json)
  WEB_API(neverd_web_source_bundles_analyze_json)
  WEB_API(neverd_web_source_bundle_records_json)
  WEB_API(neverd_web_source_view_preview_json)
  WEB_API(neverd_web_source_view_commit_json)
  WEB_API(neverd_web_source_view_records_json)
  WEB_API(neverd_web_source_view_chunk_json)
  WEB_API(neverd_web_source_navigation_analyze_json)
  WEB_API(neverd_web_source_navigation_records_json)
  WEB_API(neverd_web_source_anchor_json)
  WEB_API(neverd_web_source_map_analyze_json)
  WEB_API(neverd_web_source_map_sources_json)
  WEB_API(neverd_web_source_map_segments_json)
  WEB_API(neverd_web_source_map_lookup_json)
#undef WEB_API
  bool complete() const {
    return neverd_web_session_create && neverd_web_session_destroy &&
           neverd_web_capabilities_json && neverd_web_import_preview_json &&
           neverd_web_import_commit_json && neverd_web_metadata_json &&
           neverd_web_artifacts_json && neverd_web_source_analyze_json &&
           neverd_web_source_nodes_json && neverd_web_source_map_analyze_json &&
           neverd_web_source_map_sources_json &&
           neverd_web_source_map_segments_json &&
           neverd_web_source_map_lookup_json;
  }
};

const API &api() {
  static const API functions;
  return functions;
}

Json result(const char *text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> owned(
      text, neverd_free_string);
  if (!text)
    throw Error("allocation_failed", "Web response allocation failed");
  const auto length = strnlen(text, transport::MaxJsonBytes + 1);
  if (length > transport::MaxJsonBytes)
    throw Error("budget_exceeded", "Web response exceeds the transport budget");
  auto value = parseJson({text, length});
  if (!value.is_object() || value.value("schema_version", 0) != 1)
    throw Error("invalid_backend_response", "Invalid web response schema");
  if (value.value("status", "") != "ok") {
    auto code = std::string("invalid_backend_response");
    if (value.contains("error") && value["error"].is_object())
      code = stringField(value["error"], "code", code, 128);
    throw Error(code, "Web operation failed");
  }
  return value;
}

std::string required(const Json &payload, const char *key, size_t maximum) {
  auto value = stringField(payload, key, {}, maximum);
  if (value.empty())
    throw Error("invalid_request", "Missing required web field");
  return value;
}

void fields(const Json &payload,
            std::initializer_list<std::string_view> allowed) {
  if (!payload.is_object() || !payload.contains("schema_version") ||
      !payload["schema_version"].is_number_integer() ||
      payload["schema_version"] != 1)
    throw Error("unsupported_schema", "Web payload requires schema_version 1");
  for (auto it = payload.begin(); it != payload.end(); ++it) {
    if (it.key() == "schema_version")
      continue;
    if (std::find(allowed.begin(), allowed.end(), it.key()) == allowed.end())
      throw Error("invalid_request", "Unknown web payload field");
  }
}

uint64_t decimal(const Json &payload, const char *key) {
  const auto text = required(payload, key, 20);
  uint64_t value = 0;
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      std::to_string(value) != text)
    throw Error("invalid_request",
                "Web byte offset requires a canonical decimal string");
  return value;
}
} // namespace

Backend::~Backend() {
  neverd_session_destroy(native_);
  if (session_)
    api().neverd_web_session_destroy(session_);
}

Json Backend::capabilities() {
  if (!api().complete())
    return {{"schema_version", 1},
            {"status", "error"},
            {"error", {{"code", "capability_unavailable"}}}};
  try {
    auto value = result(api().neverd_web_capabilities_json());
    value["operation_prefix"] = "web_";
    value["payload_schema_version"] = 1;
    return value;
  } catch (const Error &) {
    return {{"schema_version", 1},
            {"status", "error"},
            {"error", {{"code", "capability_unavailable"}}}};
  }
}

Json Backend::execute(const std::string &operation, const Json &p) {
  if (operation == "web_capabilities") {
    fields(p, {});
    return capabilities();
  }
  if (!api().complete())
    throw Error("capability_unavailable", "Web analysis is unavailable");
  if (!session_)
    session_ = api().neverd_web_session_create();
  if (!session_)
    throw Error("capability_unavailable", "Web analysis is unavailable");
  if (operation == "web_import_preview") {
    fields(p, {"path", "options"});
    const auto path = required(p, "path", 32768);
    std::string options;
    if (p.contains("options")) {
      if (!p["options"].is_object())
        throw Error("invalid_request", "Web import options require an object");
      options = p["options"].dump();
    }
    return result(api().neverd_web_import_preview_json(
        session_, path.data(), path.size(), options.data(), options.size()));
  }
  if (operation == "web_import_commit") {
    fields(p, {"preview_token"});
    const auto token = required(p, "preview_token", 64);
    auto value = result(api().neverd_web_import_commit_json(
        session_, token.data(), token.size()));
    revision_ = value.at("revision").get<std::string>();
    projectId_ = value.at("project_id").get<std::string>();
    analysisState_ = "not_analyzed";
    neverd_session_destroy(native_);
    native_ = nullptr;
    handoffId_.clear();
    return value;
  }
  if (operation == "web_metadata") {
    fields(p, {});
    auto value = result(api().neverd_web_metadata_json(session_));
    // A C ABI response allocation may fail after a commit was published.
    // Explicit refresh must recover the backend's authoritative state.
    auto revision = value.at("revision").get<std::string>();
    auto project = value.at("project_id").get<std::string>();
    auto analysis = value.at("analysis_status").get<std::string>();
    if (revision != revision_ || project != projectId_) {
      neverd_session_destroy(native_);
      native_ = nullptr;
      handoffId_.clear();
    }
    revision_ = std::move(revision);
    projectId_ = std::move(project);
    analysisState_ = std::move(analysis);
    return value;
  }
  if (operation == "web_native_open") {
    fields(p, {"revision", "selection_id"});
    if (!api().neverd_web_native_open_json)
      throw Error("capability_unavailable", "Native handoff is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto selection = required(p, "selection_id", 64);
    neverd_session_t candidate = nullptr;
    const auto reply = api().neverd_web_native_open_json(
        session_, revision.data(), revision.size(), selection.data(),
        selection.size(), &candidate);
    std::unique_ptr<void, decltype(&neverd_session_destroy)> owner(
        candidate, neverd_session_destroy);
    auto value = result(reply);
    if (!owner)
      throw Error("invalid_backend_response", "Missing native handoff");
    auto id = value.at("handoff_id").get<std::string>();
    neverd_session_destroy(native_);
    native_ = owner.release();
    handoffId_ = std::move(id);
    return value;
  }
  if (operation == "web_native_metadata" || operation == "web_native_analyze") {
    fields(p, {"revision", "handoff_id"});
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "handoff_id", 64);
    if (revision != revision_)
      throw Error("stale_revision", "Web revision changed");
    if (!native_ || id != handoffId_)
      throw Error("native_handoff_not_open", "Native handoff is not open");
    const auto query = operation == "web_native_analyze"
                           ? api().neverd_web_native_analyze_json
                           : api().neverd_web_native_metadata_json;
    if (!query)
      throw Error("capability_unavailable", "Native handoff is unavailable");
    return result(query(native_));
  }
  if (operation == "web_stream_preview") {
    fields(p, {"revision", "artifact_id", "profile"});
    const auto call = api().neverd_web_stream_preview_json;
    if (!call)
      throw Error("capability_unavailable", "Passive streams are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    const auto profile = required(p, "profile", 64);
    return result(call(session_, revision.data(), revision.size(), id.data(),
                       id.size(), profile.data(), profile.size()));
  }
  if (operation == "web_stream_commit") {
    fields(p, {"revision", "preview_token"});
    const auto call = api().neverd_web_stream_commit_json;
    if (!call)
      throw Error("capability_unavailable", "Passive streams are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto token = required(p, "preview_token", 64);
    auto value = result(call(session_, revision.data(), revision.size(),
                             token.data(), token.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_stream_records") {
    fields(p, {"revision", "capture_id", "offset", "limit"});
    const auto call = api().neverd_web_stream_records_json;
    if (!call)
      throw Error("capability_unavailable", "Passive streams are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "capture_id", 64);
    return result(
        call(session_, revision.data(), revision.size(), id.data(), id.size(),
             sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
             sizeField(p, "limit", 128, 128)));
  }
  if (operation == "web_har_preview" || operation == "web_har_commit" ||
      operation == "web_interfaces_analyze") {
    const auto *key = operation == "web_har_preview"  ? "artifact_id"
                      : operation == "web_har_commit" ? "preview_token"
                                                      : "source_id";
    fields(p, {"revision", key});
    const auto call = operation == "web_har_preview"
                          ? api().neverd_web_har_preview_json
                      : operation == "web_har_commit"
                          ? api().neverd_web_har_commit_json
                          : api().neverd_web_interfaces_analyze_json;
    if (!call)
      throw Error("capability_unavailable",
                  "Passive interfaces are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, key, 64);
    auto value = result(
        call(session_, revision.data(), revision.size(), id.data(), id.size()));
    if (operation != "web_har_preview")
      analysisState_ = "partial";
    return value;
  }
  if (operation == "web_har_records" || operation == "web_interface_records" ||
      operation == "web_interface_correlation_records") {
    const auto *key = operation == "web_har_records"         ? "capture_id"
                      : operation == "web_interface_records" ? "analysis_id"
                                                             : "correlation_id";
    fields(p, {"revision", key, "offset", "limit"});
    const auto call = operation == "web_har_records"
                          ? api().neverd_web_har_records_json
                      : operation == "web_interface_records"
                          ? api().neverd_web_interface_records_json
                          : api().neverd_web_interface_correlation_records_json;
    if (!call)
      throw Error("capability_unavailable",
                  "Passive interfaces are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, key, 64);
    return result(
        call(session_, revision.data(), revision.size(), id.data(), id.size(),
             sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
             sizeField(p, "limit", 128, 128)));
  }
  if (operation == "web_interfaces_compare") {
    fields(p, {"revision", "analysis_id", "capture_id"});
    const auto call = api().neverd_web_interfaces_compare_json;
    if (!call)
      throw Error("capability_unavailable",
                  "Passive interfaces are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "analysis_id", 64);
    const auto capture = required(p, "capture_id", 64);
    return result(call(session_, revision.data(), revision.size(),
                       source.data(), source.size(), capture.data(),
                       capture.size()));
  }
  if (operation == "web_package_archive_extract") {
    fields(p, {"revision", "artifact_id", "format"});
    if (!api().neverd_web_package_archive_extract_json)
      throw Error("capability_unavailable", "Package archives are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto artifact = required(p, "artifact_id", 64);
    const auto format = required(p, "format", 16);
    auto value = result(api().neverd_web_package_archive_extract_json(
        session_, revision.data(), revision.size(), artifact.data(),
        artifact.size(), format.data(), format.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_package_integrity_verify") {
    fields(p, {"revision", "artifact_id", "declaration_id", "package_id"});
    if (!api().neverd_web_package_integrity_verify_json)
      throw Error("capability_unavailable", "Package integrity is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto artifact = required(p, "artifact_id", 64);
    const auto declaration = required(p, "declaration_id", 64);
    const auto package = p.contains("package_id")
                             ? required(p, "package_id", 64)
                             : std::string();
    auto value = result(api().neverd_web_package_integrity_verify_json(
        session_, revision.data(), revision.size(), artifact.data(),
        artifact.size(), declaration.data(), declaration.size(), package.data(),
        package.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_package_archive_records") {
    fields(p, {"revision", "archive_id", "offset", "limit"});
    if (!api().neverd_web_package_archive_records_json)
      throw Error("capability_unavailable", "Package archives are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto archive = required(p, "archive_id", 64);
    return result(api().neverd_web_package_archive_records_json(
        session_, revision.data(), revision.size(), archive.data(),
        archive.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_packages_analyze") {
    fields(p, {"revision", "artifact_id", "input_kind"});
    if (!api().neverd_web_packages_analyze_json)
      throw Error("capability_unavailable", "Package evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    const auto kind = required(p, "input_kind", 32);
    auto value = result(api().neverd_web_packages_analyze_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        kind.data(), kind.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_package_records") {
    fields(p, {"revision", "analysis_id", "record_kind", "offset", "limit"});
    if (!api().neverd_web_package_records_json)
      throw Error("capability_unavailable", "Package evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "analysis_id", 64);
    const auto kind = required(p, "record_kind", 32);
    return result(api().neverd_web_package_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        kind.data(), kind.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_packages_compare") {
    fields(p, {"revision", "before_id", "after_id"});
    if (!api().neverd_web_packages_compare_json)
      throw Error("capability_unavailable",
                  "Package comparison is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto before = required(p, "before_id", 64),
               after = required(p, "after_id", 64);
    return result(api().neverd_web_packages_compare_json(
        session_, revision.data(), revision.size(), before.data(),
        before.size(), after.data(), after.size()));
  }
  if (operation == "web_package_diff_records") {
    fields(p, {"revision", "diff_id", "offset", "limit"});
    if (!api().neverd_web_package_diff_records_json)
      throw Error("capability_unavailable",
                  "Package comparison is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "diff_id", 64);
    return result(api().neverd_web_package_diff_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_electron_manifest_analyze" ||
      operation == "web_electron_source_analyze") {
    const bool Manifest = operation == "web_electron_manifest_analyze";
    const auto Key = Manifest ? "artifact_id" : "source_id";
    fields(p, {"revision", Key});
    const auto analyze = Manifest
                             ? api().neverd_web_electron_manifest_analyze_json
                             : api().neverd_web_electron_source_analyze_json;
    if (!analyze)
      throw Error("capability_unavailable", "Electron evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, Key, 64);
    auto value = result(analyze(session_, revision.data(), revision.size(),
                                id.data(), id.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_electron_source_records") {
    fields(p, {"revision", "source_id", "offset", "limit"});
    if (!api().neverd_web_electron_source_records_json)
      throw Error("capability_unavailable", "Electron evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "source_id", 64);
    return result(api().neverd_web_electron_source_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_electron_ipc_analyze" ||
      operation == "web_electron_entries_analyze") {
    fields(p, {"revision", "manifest_artifact_id", "source_ids"});
    const auto analyze = operation == "web_electron_ipc_analyze"
                             ? api().neverd_web_electron_ipc_analyze_json
                             : api().neverd_web_electron_entries_analyze_json;
    if (!analyze)
      throw Error("capability_unavailable", "Electron evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "manifest_artifact_id", 64);
    const auto selected = p.find("source_ids");
    if (selected == p.end() || !selected->is_array() || selected->empty() ||
        selected->size() > 16)
      throw Error("invalid_request", "Invalid source selection");
    for (const auto &source : *selected)
      if (!source.is_string() ||
          source.get_ref<const std::string &>().empty() ||
          source.get_ref<const std::string &>().size() > 64)
        throw Error("invalid_request", "Invalid source selection");
    const auto options =
        Json{{"schema_version", 1}, {"source_ids", *selected}}.dump();
    auto value =
        result(analyze(session_, revision.data(), revision.size(), id.data(),
                       id.size(), options.data(), options.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_electron_ipc_records" ||
      operation == "web_electron_entry_records") {
    const bool ipc = operation == "web_electron_ipc_records";
    const auto idField = ipc ? "electron_ipc_id" : "electron_entries_id";
    fields(p, {"revision", idField, "record_kind", "offset", "limit"});
    const auto records = ipc ? api().neverd_web_electron_ipc_records_json
                             : api().neverd_web_electron_entry_records_json;
    if (!records)
      throw Error("capability_unavailable", "Electron evidence is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, idField, 64);
    const auto kind = required(p, "record_kind", 16);
    return result(
        records(session_, revision.data(), revision.size(), id.data(),
                id.size(), kind.data(), kind.size(),
                sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
                sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_html_analyze") {
    fields(p, {"revision", "artifact_id"});
    if (!api().neverd_web_html_analyze_json)
      throw Error("capability_unavailable", "HTML analysis is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    auto value = result(api().neverd_web_html_analyze_json(
        session_, revision.data(), revision.size(), id.data(), id.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_html_records") {
    fields(p, {"revision", "html_id", "record_kind", "offset", "limit"});
    if (!api().neverd_web_html_records_json)
      throw Error("capability_unavailable", "HTML analysis is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "html_id", 64);
    const auto kind = required(p, "record_kind", 16);
    return result(api().neverd_web_html_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        kind.data(), kind.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_asar_extract") {
    fields(p, {"revision", "artifact_id", "unpacked_directory_id"});
    if (!api().neverd_web_asar_extract_json)
      throw Error("capability_unavailable", "ASAR extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    const auto unpacked = stringField(p, "unpacked_directory_id", {}, 64);
    auto value = result(api().neverd_web_asar_extract_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        unpacked.data(), unpacked.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_asar_records") {
    fields(p, {"revision", "extraction_id", "offset", "limit"});
    if (!api().neverd_web_asar_records_json)
      throw Error("capability_unavailable", "ASAR extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "extraction_id", 64);
    return result(api().neverd_web_asar_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_sea_extract") {
    fields(p, {"revision", "artifact_id", "profile"});
    if (!api().neverd_web_sea_extract_json)
      throw Error("capability_unavailable", "SEA extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    const auto profile = required(p, "profile", 64);
    auto value = result(api().neverd_web_sea_extract_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        profile.data(), profile.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_sea_records") {
    fields(p, {"revision", "extraction_id", "offset", "limit"});
    if (!api().neverd_web_sea_records_json)
      throw Error("capability_unavailable", "SEA extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "extraction_id", 64);
    return result(api().neverd_web_sea_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 128)));
  }
  if (operation == "web_bun_extract") {
    fields(p, {"revision", "artifact_id"});
    if (!api().neverd_web_bun_extract_json)
      throw Error("capability_unavailable", "Bun extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    auto value = result(api().neverd_web_bun_extract_json(
        session_, revision.data(), revision.size(), id.data(), id.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_bun_records") {
    fields(p, {"revision", "extraction_id", "record_kind", "offset", "limit"});
    if (!api().neverd_web_bun_records_json)
      throw Error("capability_unavailable", "Bun extraction is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "extraction_id", 64);
    const auto kind = required(p, "record_kind", 16);
    return result(api().neverd_web_bun_records_json(
        session_, revision.data(), revision.size(), id.data(), id.size(),
        kind.data(), kind.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_source_analyze" ||
      operation == "web_source_map_analyze") {
    const bool source = operation == "web_source_analyze";
    if (source)
      fields(p, {"revision", "artifact_id", "source_type"});
    else
      fields(p, {"revision", "artifact_id"});
    const auto revision = required(p, "revision", 20);
    const auto id = required(p, "artifact_id", 64);
    const auto type = source ? required(p, "source_type", 16) : std::string();
    auto value = source ? result(api().neverd_web_source_analyze_json(
                              session_, revision.data(), revision.size(),
                              id.data(), id.size(), type.data(), type.size()))
                        : result(api().neverd_web_source_map_analyze_json(
                              session_, revision.data(), revision.size(),
                              id.data(), id.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_source_semantics_analyze") {
    fields(p, {"revision", "source_id"});
    if (!api().neverd_web_source_semantics_analyze_json)
      throw Error("capability_unavailable", "Source semantics are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    auto value = result(api().neverd_web_source_semantics_analyze_json(
        session_, revision.data(), revision.size(), source.data(),
        source.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_source_semantic_records") {
    fields(p, {"revision", "source_id", "offset", "limit"});
    if (!api().neverd_web_source_semantic_records_json)
      throw Error("capability_unavailable", "Source semantics are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    return result(api().neverd_web_source_semantic_records_json(
        session_, revision.data(), revision.size(), source.data(),
        source.size(),
        sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
        sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_source_bindings_analyze" ||
      operation == "web_source_modules_analyze" ||
      operation == "web_source_bundles_analyze" ||
      operation == "web_source_navigation_analyze") {
    fields(p, {"revision", "source_id"});
    const auto analyze = operation == "web_source_navigation_analyze"
                             ? api().neverd_web_source_navigation_analyze_json
                         : operation == "web_source_bundles_analyze"
                             ? api().neverd_web_source_bundles_analyze_json
                         : operation == "web_source_modules_analyze"
                             ? api().neverd_web_source_modules_analyze_json
                             : api().neverd_web_source_bindings_analyze_json;
    if (!analyze)
      throw Error("capability_unavailable", "Source analysis is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    auto value = result(analyze(session_, revision.data(), revision.size(),
                                source.data(), source.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_source_binding_records" ||
      operation == "web_source_module_records" ||
      operation == "web_source_bundle_records" ||
      operation == "web_source_navigation_records") {
    fields(p, {"revision", "source_id", "record_kind", "offset", "limit"});
    const auto records = operation == "web_source_navigation_records"
                             ? api().neverd_web_source_navigation_records_json
                         : operation == "web_source_bundle_records"
                             ? api().neverd_web_source_bundle_records_json
                         : operation == "web_source_module_records"
                             ? api().neverd_web_source_module_records_json
                             : api().neverd_web_source_binding_records_json;
    if (!records)
      throw Error("capability_unavailable", "Source analysis is unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    const auto kind = required(p, "record_kind", 16);
    return result(
        records(session_, revision.data(), revision.size(), source.data(),
                source.size(), kind.data(), kind.size(),
                sizeField(p, "offset", 0, std::numeric_limits<size_t>::max()),
                sizeField(p, "limit", 128, 512)));
  }
  if (operation == "web_source_anchor") {
    fields(p,
           {"revision", "source_id", "byte_offset", "byte_length", "view_id"});
    if (!api().neverd_web_source_anchor_json)
      throw Error("capability_unavailable", "Source anchors are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    const auto view = stringField(p, "view_id", {}, 64);
    return result(api().neverd_web_source_anchor_json(
        session_, revision.data(), revision.size(), source.data(),
        source.size(), decimal(p, "byte_offset"), decimal(p, "byte_length"),
        view.data(), view.size()));
  }
  if (operation == "web_source_view_preview") {
    fields(p, {"revision", "source_id", "options"});
    if (!api().neverd_web_source_view_preview_json)
      throw Error("capability_unavailable", "Source views are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto source = required(p, "source_id", 64);
    std::string options;
    if (p.contains("options")) {
      if (!p["options"].is_object())
        throw Error("invalid_request", "Source view options require an object");
      options = p["options"].dump();
    }
    auto value = result(api().neverd_web_source_view_preview_json(
        session_, revision.data(), revision.size(), source.data(),
        source.size(), options.data(), options.size()));
    analysisState_ = "partial";
    return value;
  }
  if (operation == "web_source_view_commit") {
    fields(p, {"revision", "preview_token"});
    if (!api().neverd_web_source_view_commit_json)
      throw Error("capability_unavailable", "Source views are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto token = required(p, "preview_token", 64);
    return result(api().neverd_web_source_view_commit_json(
        session_, revision.data(), revision.size(), token.data(),
        token.size()));
  }
  if (operation == "web_source_view_records" ||
      operation == "web_source_view_chunk") {
    const bool chunk = operation == "web_source_view_chunk";
    if (chunk)
      fields(p, {"revision", "view_id", "byte_offset", "byte_limit"});
    else
      fields(p, {"revision", "view_id", "offset", "limit"});
    const auto query = chunk ? api().neverd_web_source_view_chunk_json
                             : api().neverd_web_source_view_records_json;
    if (!query)
      throw Error("capability_unavailable", "Source views are unavailable");
    const auto revision = required(p, "revision", 20);
    const auto view = required(p, "view_id", 64);
    const auto offset =
        chunk ? decimal(p, "byte_offset")
              : sizeField(p, "offset", 0, std::numeric_limits<size_t>::max());
    const auto limit = chunk ? sizeField(p, "byte_limit", 65536, 65536)
                             : sizeField(p, "limit", 128, 512);
    return result(query(session_, revision.data(), revision.size(), view.data(),
                        view.size(), offset, limit));
  }
  if (operation == "web_source_map_lookup") {
    fields(p, {"revision", "map_id", "generated_source_id", "byte_offset"});
    const auto revision = required(p, "revision", 20);
    const auto map = required(p, "map_id", 64);
    const auto source = required(p, "generated_source_id", 64);
    return result(api().neverd_web_source_map_lookup_json(
        session_, revision.data(), revision.size(), map.data(), map.size(),
        source.data(), source.size(), decimal(p, "byte_offset")));
  }
  const bool artifacts = operation == "web_artifacts";
  const bool nodes = operation == "web_source_nodes";
  const bool sources = operation == "web_source_map_sources";
  const bool segments = operation == "web_source_map_segments";
  if (!artifacts && !nodes && !sources && !segments)
    throw Error("unsupported_operation", "Unsupported web operation");
  if (artifacts)
    fields(p, {"revision", "offset", "limit"});
  else if (nodes)
    fields(p, {"revision", "source_id", "offset", "limit"});
  else
    fields(p, {"revision", "map_id", "offset", "limit"});
  const auto revision = required(p, "revision", 20);
  const auto offset =
      sizeField(p, "offset", 0, std::numeric_limits<size_t>::max());
  const auto limit = sizeField(p, "limit", 128, 512);
  if (artifacts)
    return result(api().neverd_web_artifacts_json(
        session_, revision.data(), revision.size(), offset, limit));
  const auto id = required(p, nodes ? "source_id" : "map_id", 64);
  const auto query = nodes     ? api().neverd_web_source_nodes_json
                     : sources ? api().neverd_web_source_map_sources_json
                               : api().neverd_web_source_map_segments_json;
  return result(query(session_, revision.data(), revision.size(), id.data(),
                      id.size(), offset, limit));
}
} // namespace neverd::web_client
