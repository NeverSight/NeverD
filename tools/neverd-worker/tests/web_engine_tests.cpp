#include "../../../unittests/web/AsarEnvelopeFixture.h"
#include "../../../unittests/web/BunFixture.h"
#include "../../../unittests/web/BunSourceMapFixture.h"
#include "../../../unittests/web/NativeFixture.h"
#include "WebEngine.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace neverd::worker;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const char *message) {
  if (!condition)
    throw std::runtime_error(message);
}
template <typename F> void rejects(F call, const char *code) {
  try {
    (void)call();
  } catch (const Error &error) {
    check(error.code == code, "Unexpected web error code");
    check(std::string(error.what()).find("SECRET_WEB") == std::string::npos,
          "Web error exposed private evidence");
    return;
  }
  throw std::runtime_error("Invalid web request was admitted");
}
Json take(const char *text) {
  std::unique_ptr<const char, decltype(&neverd_free_string)> owned(
      text, neverd_free_string);
  check(text != nullptr, "Missing owned C API result");
  return parseJson(text);
}
struct Fixture {
  fs::path root;
  Fixture() {
    const auto stamp =
        std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; i != 100; ++i) {
      root = fs::temp_directory_path() /
             ("neverd-web-worker-" + std::to_string(stamp) + "-" +
              std::to_string(i));
      if (fs::create_directory(root))
        return;
    }
    throw std::runtime_error("Cannot create private worker test directory");
  }
  ~Fixture() {
    std::error_code error;
    fs::remove_all(root, error);
  }
  void write(std::string_view bytes) const {
    std::ofstream out(root / "input", std::ios::binary);
    out.write(bytes.data(), bytes.size());
    check(out.good(), "Cannot write worker test fixture");
  }
  std::string path() const { return (root / "input").string(); }
};

#ifndef _WIN32
class Process {
  int input_ = -1, output_ = -1;
  pid_t pid_ = -1;
  uint64_t sequence_ = 0;
  void readExact(char *bytes, size_t count) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (count) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now())
              .count();
      check(remaining > 0, "Worker frame timed out");
      pollfd descriptor{output_, POLLIN, 0};
      const auto ready = poll(&descriptor, 1, int(remaining));
      if (ready < 0 && errno == EINTR)
        continue;
      check(ready > 0, "Worker output timed out");
      const auto received = read(output_, bytes, count);
      if (received < 0 && errno == EINTR)
        continue;
      check(received > 0, "Worker closed an incomplete frame");
      bytes += received;
      count -= size_t(received);
    }
  }

public:
  Process(const char *binary, const fs::path &errors) {
    int inputs[2], outputs[2];
    check(pipe(inputs) == 0, "Cannot create worker input pipe");
    if (pipe(outputs) != 0) {
      close(inputs[0]);
      close(inputs[1]);
      throw std::runtime_error("Cannot create worker output pipe");
    }
    const auto error = open(errors.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (error < 0) {
      for (const auto fd : {inputs[0], inputs[1], outputs[0], outputs[1]})
        close(fd);
      throw std::runtime_error("Cannot capture worker diagnostics");
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, inputs[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, outputs[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, error, STDERR_FILENO);
    for (const auto fd : {inputs[0], inputs[1], outputs[0], outputs[1], error})
      posix_spawn_file_actions_addclose(&actions, fd);
    char path[] = "PATH=/neverd-no-external-tools",
         cache[] = "NEVERD_SIGNATURE_CACHE=off";
    char *environment[] = {path, cache, nullptr};
    char *arguments[] = {const_cast<char *>(binary), nullptr};
    const auto status =
        posix_spawn(&pid_, binary, &actions, nullptr, arguments, environment);
    posix_spawn_file_actions_destroy(&actions);
    close(inputs[0]);
    close(outputs[1]);
    close(error);
    if (status != 0) {
      close(inputs[1]);
      close(outputs[0]);
      pid_ = -1;
      throw std::runtime_error("Cannot launch C++ worker");
    }
    input_ = inputs[1];
    output_ = outputs[0];
  }
  ~Process() {
    if (input_ >= 0)
      close(input_);
    if (output_ >= 0)
      close(output_);
    if (pid_ > 0) {
      kill(pid_, SIGKILL);
      while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
  Json receive() {
    unsigned char header[4];
    readExact(reinterpret_cast<char *>(header), 4);
    std::string body(frameSize(header), '\0');
    readExact(body.data(), body.size());
    check(body.find("SECRET_WEB") == std::string::npos,
          "Worker frame exposed evidence");
    return parseJson(body);
  }
  Json call(std::string operation, Json payload,
            std::string expectedRevision = {}, std::string project = {}) {
    const auto id = std::to_string(++sequence_);
    Json request{{"protocol_major", 1},
                 {"request_id", id},
                 {"operation", operation},
                 {"payload", payload}};
    if (!expectedRevision.empty())
      request["expected_revision"] = expectedRevision;
    if (!project.empty())
      request["project_id"] = project;
    const auto bytes = frame(request);
    size_t cursor = 0;
    while (cursor < bytes.size()) {
      const auto sent =
          write(input_, bytes.data() + cursor, bytes.size() - cursor);
      if (sent < 0 && errno == EINTR)
        continue;
      check(sent > 0, "Cannot send worker request");
      cursor += size_t(sent);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    do {
      auto value = receive();
      if (value.value("type", "") == "heartbeat")
        continue;
      check(value.value("request_id", "") == id,
            "Unexpected worker response correlation");
      return value;
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Worker response timed out");
  }
  void stop() {
    check(call("shutdown", Json::object()).at("status") == "ok",
          "Worker shutdown failed");
    close(input_);
    input_ = -1;
    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    do {
      const auto result = waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        pid_ = -1;
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "Worker exited unsuccessfully");
        return;
      }
      check(result == 0 || errno == EINTR, "Cannot reap worker process");
      poll(nullptr, 0, 10);
    } while (std::chrono::steady_clock::now() < deadline);
    throw std::runtime_error("Worker did not exit after shutdown");
  }
};
#endif
} // namespace

int main(int argc, char **argv) {
  try {
#ifndef _WIN32
    std::signal(SIGPIPE, SIG_IGN);
#endif
    auto caps = WebEngine::capabilities();
    if (caps.value("status", "") != "ok" ||
        caps.value("input_reader", "") == "unavailable")
      return 77;
    caps.erase("operation_prefix");
    caps.erase("payload_schema_version");
    check(caps == take(neverd_web_capabilities_json()),
          "Worker capabilities differ from the C API");
    Fixture fixture;
    auto input = Json::parse(
        R"({"version":3,"sources":["https://SECRET_WEB.invalid"],"sourcesContent":["while(true){}; require('SECRET_WEB');"],"mappings":"AAAA"})");
    input["sources"].push_back("SECRET_WEB_BUNDLE.js");
    input["sourcesContent"].push_back(R"JS((() => {
      var table = {'SECRET_WEB_KEY': (m,e,r) => { r('SECRET_WEB_KEY'); }};
      var cache = {};
      function load(id) {
        var cached = cache[id];
        if (cached !== undefined) { return cached.exports; }
        var module = cache[id] = {exports:{}};
        table[id](module, module.exports, load);
        return module.exports;
      }
      load('SECRET_WEB_KEY');
    })(); )JS");
    fixture.write(input.dump());
    WebEngine web;
    const Json empty{{"schema_version", 1}};
    rejects([&] { return web.execute("web_metadata", {}); },
            "unsupported_schema");
    rejects(
        [&] {
          return web.execute("web_metadata",
                             {{"schema_version", 1}, {"SECRET_WEB", true}});
        },
        "invalid_request");
    rejects([&] { return web.execute("web_metadata", empty); }, "no_project");
    auto preview =
        web.execute("web_import_preview",
                    {{"schema_version", 1}, {"path", fixture.path()}});
    auto committed = web.execute(
        "web_import_commit", {{"schema_version", 1},
                              {"preview_token", preview.at("preview_token")}});
    check(web.revision() == "1" &&
              web.projectId() == committed.at("project_id").get<std::string>(),
          "Worker import identity drifted");
    const Json page{{"schema_version", 1}, {"revision", "1"}, {"limit", 512}};
    auto artifacts = web.execute("web_artifacts", page);
    const auto artifact = artifacts.at("items").at(0).at("artifact_id");
    auto map = web.execute(
        "web_source_map_analyze",
        {{"schema_version", 1}, {"revision", "1"}, {"artifact_id", artifact}});
    check(web.analysisState() == "partial",
          "Worker promoted inspection to complete analysis");
    const auto mapPage =
        web.execute("web_source_map_sources", {{"schema_version", 1},
                                               {"revision", "1"},
                                               {"map_id", map.at("map_id")}});
    check(mapPage.dump().find("SECRET_WEB") == std::string::npos,
          "Worker exposed private map evidence");
    const bool hasBindings =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_bindings_analyze") != caps["operations"].end();
    const bool hasSemantics =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_semantics_analyze") != caps["operations"].end();
    const bool hasModules =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_modules_analyze") != caps["operations"].end();
    const bool hasBundles =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_bundles_analyze") != caps["operations"].end();
    const bool hasViews =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_view_preview") != caps["operations"].end();
    const bool hasNavigation =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "source_navigation_analyze") != caps["operations"].end();
    Json navigationSummary, navigationRecords, navigationRequest, anchorResult,
        anchorRequest;
    Json viewPreview, viewCommit, viewRecords, viewChunk, viewQuery,
        viewCommitRequest;
    Json bundleSourceRequest, bundleRequest, bundleRecordsRequest,
        bundleSummary, bundleRecords;
    Json sourceRequest, bindingRequest, recordsRequest, bindingSummary,
        bindingRecords, semanticSummary, semanticRecords, moduleSummary,
        moduleRequest, moduleRecords;
    if (hasBindings) {
      sourceRequest = {{"schema_version", 1},
                       {"revision", "1"},
                       {"artifact_id", mapPage["items"][0]["artifact_id"]},
                       {"source_type", "commonjs"}};
      const auto source = web.execute("web_source_analyze", sourceRequest);
      bindingRequest = {{"schema_version", 1},
                        {"revision", "1"},
                        {"source_id", source.at("source_id")}};
      bindingSummary =
          web.execute("web_source_bindings_analyze", bindingRequest);
      check(bindingSummary.at("binding_status") == "ok",
            "Binding analysis failed");
      recordsRequest = bindingRequest;
      recordsRequest["record_kind"] = "references";
      bindingRecords =
          web.execute("web_source_binding_records", recordsRequest);
      check(bindingRecords.at("items").size() == 1 &&
                bindingRecords["items"][0]["resolution"] == "lexical_binding",
            "CommonJS reference was not resolved by the backend");
      check(bindingRecords.dump().find("SECRET_WEB") == std::string::npos,
            "Binding query exposed private evidence");
      if (hasModules) {
        moduleSummary =
            web.execute("web_source_modules_analyze", bindingRequest);
        check(moduleSummary.at("module_status") == "ok" &&
                  moduleSummary.at("resolves_runtime_modules") == false,
              "Module evidence was promoted to runtime resolution");
        moduleRequest = bindingRequest;
        moduleRequest["record_kind"] = "requests";
        moduleRecords = web.execute("web_source_module_records", moduleRequest);
        check(moduleRecords.at("items").size() == 1 &&
                  moduleRecords["items"][0]["callee_evidence"] ==
                      "caller_selected_commonjs_parameter" &&
                  moduleRecords["items"][0]["link_status"] ==
                      "unverified_callee",
              "CommonJS evidence was changed by the worker");
        check(moduleRecords.dump().find("SECRET_WEB") == std::string::npos,
              "Module query exposed a private specifier");
      }
      if (hasSemantics) {
        semanticSummary =
            web.execute("web_source_semantics_analyze", bindingRequest);
        check(semanticSummary.at("value_status") == "ok" &&
                  semanticSummary.at("authorizes_source_rewrites") == false,
              "Semantic summary promoted finite values to rewrite permission");
        semanticRecords =
            web.execute("web_source_semantic_records", bindingRequest);
        check(semanticRecords.dump().find("SECRET_WEB") == std::string::npos,
              "Semantic query exposed private values");
      }
    }
    if (hasBundles) {
      bundleSourceRequest = {
          {"schema_version", 1},
          {"revision", "1"},
          {"artifact_id", mapPage["items"][1]["artifact_id"]},
          {"source_type", "script"}};
      const auto source =
          web.execute("web_source_analyze", bundleSourceRequest);
      bundleRequest = {{"schema_version", 1},
                       {"revision", "1"},
                       {"source_id", source.at("source_id")}};
      bundleSummary = web.execute("web_source_bundles_analyze", bundleRequest);
      check(bundleSummary.at("bundle_status") == "ok" &&
                bundleSummary.at("module_count") == 1 &&
                bundleSummary.at("producer_verified") == false,
            "Bundle layout was not preserved as unverified producer evidence");
      bundleRecordsRequest = bundleRequest;
      bundleRecordsRequest["record_kind"] = "modules";
      bundleRecords =
          web.execute("web_source_bundle_records", bundleRecordsRequest);
      check(bundleRecords.at("items").size() == 1 &&
                bundleRecords["items"][0]["module_key_redacted"] == true &&
                bundleRecords.dump().find("SECRET_WEB") == std::string::npos,
            "Bundle partition query exposed private input");
    }
    if (hasViews) {
      viewPreview = web.execute("web_source_view_preview", bindingRequest);
      check(!viewPreview.contains("text") &&
                viewPreview.at("publication_status") == "preview",
            "Source preview exposed text or published without commit");
      viewQuery = {{"schema_version", 1},
                   {"revision", "1"},
                   {"view_id", viewPreview.at("view_id")}};
      viewRecords = web.execute("web_source_view_records", viewQuery);
      auto chunkQuery = viewQuery;
      chunkQuery["byte_offset"] = "0";
      rejects([&] { return web.execute("web_source_view_chunk", chunkQuery); },
              "source_view_not_committed");
      viewCommitRequest = {{"schema_version", 1},
                           {"revision", "1"},
                           {"preview_token", viewPreview.at("preview_token")}};
      viewCommit = web.execute("web_source_view_commit", viewCommitRequest);
      viewChunk = web.execute("web_source_view_chunk", chunkQuery);
      check(viewChunk.dump().find("SECRET_WEB") == std::string::npos &&
                viewChunk.at("reviewed_bytes") == "0",
            "Default source view exposed private source data");
      if (hasNavigation) {
        navigationSummary =
            web.execute("web_source_navigation_analyze", bindingRequest);
        navigationRequest = bindingRequest;
        navigationRequest["record_kind"] = "calls";
        navigationRecords =
            web.execute("web_source_navigation_records", navigationRequest);
        check(navigationRecords.at("runtime_call_graph") == "not_analyzed" &&
                  navigationRecords.dump().find("SECRET_WEB") ==
                      std::string::npos,
              "Navigation promoted lexical evidence or exposed names");
        anchorRequest = bindingRequest;
        anchorRequest["byte_offset"] = "0";
        anchorRequest["byte_length"] = "1";
        anchorRequest["view_id"] = viewPreview.at("view_id");
        anchorResult = web.execute("web_source_anchor", anchorRequest);
        check(anchorResult.at("storage").at("mapping") ==
                  "encoded_member_not_located",
              "Embedded JSON source invented original byte positions");
      }
      auto invalid = bindingRequest;
      invalid["options"] = {
          {"schema_version", 1},
          {"reviewed_ranges",
           Json::array({{{"byte_offset", "0"}, {"byte_length", "1"}}})}};
      rejects([&] { return web.execute("web_source_view_preview", invalid); },
              "source_view_local_review_required");
    }
    rejects(
        [&] {
          return web.execute("web_artifacts",
                             {{"schema_version", 1}, {"revision", "0"}});
        },
        "stale_revision");
    rejects(
        [&] {
          return web.execute("web_source_map_lookup",
                             {{"schema_version", 1},
                              {"revision", "1"},
                              {"map_id", map.at("map_id")},
                              {"generated_source_id", "abc"},
                              {"byte_offset", "00"}});
        },
        "invalid_request");
#ifndef _WIN32
    check(argc == 2, "Worker executable path is required");
    Process process(argv[1], fixture.root / "stderr");
    const auto hello = process.receive();
    check(hello.at("web").at("status") == "ok",
          "Worker did not advertise the actual web backend");
    auto reply = process.call("web_import_preview", {{"schema_version", 1},
                                                     {"path", fixture.path()}});
    check(reply.at("domain") == "web" && reply.at("revision") == "0",
          "Wrong preview envelope domain");
    reply = process.call(
        "web_import_commit",
        {{"schema_version", 1},
         {"preview_token", reply.at("payload").at("preview_token")}},
        "0");
    check(reply.at("payload") == committed,
          "Worker and direct adapter import results differ");
    check(reply.at("revision") == "1",
          "Worker envelope used the native revision");
    const auto project = reply.at("project_id").get<std::string>();
    reply = process.call("web_artifacts", page, "1", project);
    check(reply.at("payload") == artifacts,
          "Worker artifact result differs from direct query");
    reply = process.call(
        "web_source_map_analyze",
        {{"schema_version", 1}, {"revision", "1"}, {"artifact_id", artifact}},
        "1", project);
    check(reply.at("payload") == map && reply.at("analysis_state") == "partial",
          "Worker changed map evidence or coverage");
    if (hasBindings) {
      reply = process.call("web_source_analyze", sourceRequest, "1", project);
      check(reply.at("status") == "ok", "Worker source analysis failed");
      reply = process.call("web_source_bindings_analyze", bindingRequest, "1",
                           project);
      check(reply.at("payload") == bindingSummary &&
                reply.at("analysis_state") == "partial",
            "Worker binding summary differs from direct analysis");
      reply = process.call("web_source_binding_records", recordsRequest, "1",
                           project);
      check(reply.at("payload") == bindingRecords,
            "Worker binding records differ from direct analysis");
      if (hasModules) {
        reply = process.call("web_source_modules_analyze", bindingRequest, "1",
                             project);
        check(reply.at("payload") == moduleSummary &&
                  reply.at("analysis_state") == "partial",
              "Worker module summary differs from direct analysis");
        reply = process.call("web_source_module_records", moduleRequest, "1",
                             project);
        check(reply.at("payload") == moduleRecords,
              "Worker module records differ from direct analysis");
        auto badModuleRequest = moduleRequest;
        badModuleRequest["fetch"] = true;
        reply = process.call("web_source_module_records", badModuleRequest, "1",
                             project);
        check(reply.at("error").at("code") == "invalid_request",
              "Worker accepted an extra module request field");
      }
      if (hasSemantics) {
        reply = process.call("web_source_semantics_analyze", bindingRequest,
                             "1", project);
        check(reply.at("payload") == semanticSummary &&
                  reply.at("analysis_state") == "partial",
              "Worker semantic summary differs from direct analysis");
        reply = process.call("web_source_semantic_records", bindingRequest, "1",
                             project);
        check(reply.at("payload") == semanticRecords,
              "Worker semantic records differ from direct analysis");
      }
    }
    if (hasBundles) {
      reply =
          process.call("web_source_analyze", bundleSourceRequest, "1", project);
      check(reply.at("status") == "ok",
            "Worker bundle source admission failed");
      reply = process.call("web_source_bundles_analyze", bundleRequest, "1",
                           project);
      check(reply.at("payload") == bundleSummary &&
                reply.at("analysis_state") == "partial",
            "Worker bundle summary differs from direct analysis");
      reply = process.call("web_source_bundle_records", bundleRecordsRequest,
                           "1", project);
      check(reply.at("payload") == bundleRecords,
            "Worker bundle partitions differ from direct analysis");
      auto unsafe = bundleRecordsRequest;
      unsafe["execute"] = true;
      reply = process.call("web_source_bundle_records", unsafe, "1", project);
      check(reply.at("error").at("code") == "invalid_request",
            "Worker accepted an unknown bundle query field");
    }
    if (hasViews) {
      reply =
          process.call("web_source_view_preview", bindingRequest, "1", project);
      check(reply.at("payload") == viewPreview,
            "Worker source preview differs from direct query");
      reply = process.call("web_source_view_records", viewQuery, "1", project);
      check(reply.at("payload") == viewRecords,
            "Worker source range mapping differs");
      auto chunkQuery = viewQuery;
      chunkQuery["byte_offset"] = "0";
      reply = process.call("web_source_view_chunk", chunkQuery, "1", project);
      check(reply.at("error").at("code") == "source_view_not_committed",
            "Worker exposed uncommitted source text");
      reply = process.call("web_source_view_commit", viewCommitRequest, "1",
                           project);
      check(reply.at("payload") == viewCommit,
            "Worker source publication differs");
      reply = process.call("web_source_view_chunk", chunkQuery, "1", project);
      check(reply.at("payload") == viewChunk,
            "Worker source text differs from direct query");
      if (hasNavigation) {
        reply = process.call("web_source_navigation_analyze", bindingRequest,
                             "1", project);
        check(reply.at("payload") == navigationSummary,
              "Worker navigation summary differs");
        reply = process.call("web_source_navigation_records", navigationRequest,
                             "1", project);
        check(reply.at("payload") == navigationRecords,
              "Worker navigation records differ");
        reply = process.call("web_source_anchor", anchorRequest, "1", project);
        check(reply.at("payload") == anchorResult,
              "Worker source/storage/view anchor differs");
        auto invalid = anchorRequest;
        invalid["byte_offset"] = "00";
        reply = process.call("web_source_anchor", invalid, "1", project);
        check(reply.at("error").at("code") == "invalid_request",
              "Worker accepted a noncanonical anchor offset");
        invalid = navigationRequest;
        invalid["execute"] = true;
        reply = process.call("web_source_navigation_records", invalid, "1",
                             project);
        check(reply.at("error").at("code") == "invalid_request",
              "Worker accepted an undeclared navigation field");
      }
      chunkQuery["unredacted"] = true;
      reply = process.call("web_source_view_chunk", chunkQuery, "1", project);
      check(reply.at("error").at("code") == "invalid_request",
            "Worker accepted an undeclared source disclosure field");
    }
    reply = process.call("web_artifacts", page, "0", project);
    check(reply.at("error").at("code") == "stale_revision",
          "Worker accepted a stale web envelope");
    reply = process.call("web_artifacts", page, "1", "wrong-project");
    check(reply.at("error").at("code") == "stale_project",
          "Worker accepted a mismatched web project");
    reply = process.call("metadata", Json::object(), "0");
    check(reply.at("status") == "error" && reply.at("revision") == "0",
          "Web import changed native session state");
    reply = process.call("web_metadata", empty, "1", project);
    check(reply.at("status") == "ok" &&
              reply.at("payload").at("revision") == "1",
          "Native failure changed web session state");
    // Replace the project with inert native-container bytes and exercise the
    // same extraction API through both direct and framed worker calls.
    fixture.write(
        neverd::web::test::BunFixture(true, neverd::web::test::simpleBunMap(),
                                      neverd::web::test::nativeELF())
            .Bytes);
    preview = web.execute("web_import_preview",
                          {{"schema_version", 1}, {"path", fixture.path()}});
    const Json bunCommit{{"schema_version", 1},
                         {"preview_token", preview.at("preview_token")}};
    const auto bunProject = web.execute("web_import_commit", bunCommit)
                                .at("project_id")
                                .get<std::string>();
    const auto bunRoot = web.execute(
        "web_artifacts",
        {{"schema_version", 1}, {"revision", "2"}})["items"][0]["artifact_id"];
    const Json bunRequest{
        {"schema_version", 1}, {"revision", "2"}, {"artifact_id", bunRoot}};
    const auto bunSummary = web.execute("web_bun_extract", bunRequest);
    check(bunSummary.at("module_count") == 3 &&
              bunSummary.at("producer_version_verified") == false,
          "Bun extraction profile mismatch");
    const Json bunPage{{"schema_version", 1},
                       {"revision", "2"},
                       {"extraction_id", bunSummary.at("extraction_id")},
                       {"record_kind", "modules"}};
    const auto bunModules = web.execute("web_bun_records", bunPage);
    check(bunModules.dump().find("CANARY") == std::string::npos,
          "Bun module metadata exposed source bytes");
    reply = process.call("web_import_preview",
                         {{"schema_version", 1}, {"path", fixture.path()}}, "1",
                         project);
    reply = process.call("web_import_commit",
                         {{"schema_version", 1},
                          {"preview_token", reply["payload"]["preview_token"]}},
                         "1", project);
    check(reply.at("project_id") == bunProject, "Bun project identity differs");
    reply = process.call("web_bun_extract", bunRequest, "2", bunProject);
    check(reply.at("payload") == bunSummary, "Worker Bun extraction differs");
    reply = process.call("web_bun_records", bunPage, "2", bunProject);
    check(reply.at("payload") == bunModules, "Worker Bun modules differ");
    if (bunSummary.at("source_map_decoding") == "available_on_request") {
      const Json request{
          {"schema_version", 1},
          {"revision", "2"},
          {"artifact_id", bunModules["items"][0]["source_map_region_id"]}};
      const auto decoded = web.execute("web_source_map_analyze", request);
      reply = process.call("web_source_map_analyze", request, "2", bunProject);
      check(reply.at("payload") == decoded &&
                decoded.at("mapping_coverage") ==
                    "retained_mapped_anchors_only" &&
                decoded.at("association_status") == "container_assertion" &&
                decoded.at("provenance_verified") == false,
            "Worker lost serialized-map identity, association or fidelity");
      const Json page{{"schema_version", 1},
                      {"revision", "2"},
                      {"map_id", decoded.at("map_id")}};
      const auto sources = web.execute("web_source_map_sources", page);
      reply = process.call("web_source_map_sources", page, "2", bunProject);
      check(reply.at("payload") == sources &&
                sources["items"][0]["storage"]["encoding"] == "zstd-utf8" &&
                sources.dump().find("CANARY") == std::string::npos &&
                sources.dump().find("SECRET_MAP") == std::string::npos,
            "Worker changed compressed source evidence or exposed private "
            "content");
      const auto segments = web.execute("web_source_map_segments", page);
      reply = process.call("web_source_map_segments", page, "2", bunProject);
      check(reply.at("payload") == segments && segments["items"].size() == 2,
            "Worker changed mapped anchors");
    }
    auto unsafeBun = bunRequest;
    unsafeBun["execute"] = true;
    reply = process.call("web_bun_extract", unsafeBun, "2", bunProject);
    check(reply.at("error").at("code") == "invalid_request",
          "Worker accepted Bun execution option");
    if (hasBindings) {
      const Json source{
          {"schema_version", 1},
          {"revision", "2"},
          {"artifact_id", bunModules["items"][1]["source_artifact_id"]},
          {"source_type", "module"}};
      const auto direct = web.execute("web_source_analyze", source);
      reply = process.call("web_source_analyze", source, "2", bunProject);
      check(reply.at("payload") == direct &&
                direct.at("parse_status") == "parsed",
            "Worker did not preserve Bun source projection identity");
    }
    const Json nativeRequest{
        {"schema_version", 1},
        {"revision", "2"},
        {"selection_id", bunModules["items"][2]["content_region_id"]}};
    const auto handoff = web.execute("web_native_open", nativeRequest);
    reply = process.call("web_native_open", nativeRequest, "2", bunProject);
    check(reply.at("payload") == handoff &&
              handoff["pipeline_status"] == "not_run",
          "Worker native handoff differs from the shared backend");
    check(handoff.dump().find("CANARY") == std::string::npos,
          "Native metadata exposed original strings");
    const Json nativeQuery{{"schema_version", 1},
                           {"revision", "2"},
                           {"handoff_id", handoff.at("handoff_id")}};
    auto badNative = nativeRequest;
    badNative["selection_id"] = bunModules["items"][0]["bytecode_region_id"];
    rejects([&] { return web.execute("web_native_open", badNative); },
            "native_selection_unavailable");
    reply = process.call("web_native_open", badNative, "2", bunProject);
    check(reply["error"]["code"] == "native_selection_unavailable",
          "Worker handed opaque bytecode to a native loader");
    reply = process.call("web_native_metadata", nativeQuery, "2", bunProject);
    check(reply.at("payload") == handoff &&
              web.execute("web_native_metadata", nativeQuery) == handoff,
          "Failed native replacement destroyed the existing handoff");
    badNative = nativeRequest;
    badNative["execute"] = true;
    reply = process.call("web_native_open", badNative, "2", bunProject);
    check(reply["error"]["code"] == "invalid_request",
          "Worker accepted native execution through a web handoff");
    const auto analyzed = web.execute("web_native_analyze", nativeQuery);
    reply = process.call("web_native_analyze", nativeQuery, "2", bunProject);
    check(
        reply.at("payload") == analyzed &&
            analyzed["pipeline_status"] == "succeeded",
        "Native static pipeline query lost provenance or direct/framed parity");
    reply = process.call("metadata", Json::object(), "0");
    check(reply.at("status") == "error" && reply.at("revision") == "0",
          "Web native handoff replaced the ordinary native project");
    fixture.write("replacement web project");
    preview = web.execute("web_import_preview",
                          {{"schema_version", 1}, {"path", fixture.path()}});
    const auto nextProject =
        web.execute("web_import_commit",
                    {{"schema_version", 1},
                     {"preview_token", preview["preview_token"]}})["project_id"]
            .get<std::string>();
    reply = process.call("web_import_preview",
                         {{"schema_version", 1}, {"path", fixture.path()}}, "2",
                         bunProject);
    reply = process.call("web_import_commit",
                         {{"schema_version", 1},
                          {"preview_token", reply["payload"]["preview_token"]}},
                         "2", bunProject);
    auto oldHandoff = nativeQuery;
    oldHandoff["revision"] = "3";
    rejects([&] { return web.execute("web_native_metadata", oldHandoff); },
            "native_handoff_not_open");
    reply = process.call("web_native_metadata", oldHandoff, "3", nextProject);
    check(reply["error"]["code"] == "native_handoff_not_open",
          "Worker retained a handoff after replacing the web project");
    const bool hasAsar =
        std::find(caps["operations"].begin(), caps["operations"].end(),
                  "asar_extract") != caps["operations"].end();
    if (hasAsar) {
      const std::string source = "export const SECRET_WEB=7;";
      const auto nativeBytes = neverd::web::test::nativeELF();
      const auto asarRoot = fixture.root / "asar";
      fs::create_directories(asarRoot / "chosen");
      const auto archive = neverd::web::test::asarBytes(
          Json{
              {"files",
               {{"a_SECRET_WEB.js", {{"size", source.size()}, {"offset", "0"}}},
                {"b_SECRET_WEB.node",
                 {{"size", nativeBytes.size()}, {"unpacked", true}}}}}}
              .dump(),
          source);
      for (const auto &[path, bytes] :
           std::initializer_list<std::pair<fs::path, std::string_view>>{
               {asarRoot / "a.asar", archive},
               {asarRoot / "chosen/b_SECRET_WEB.node", nativeBytes}}) {
        std::ofstream out(path, std::ios::binary);
        out.write(bytes.data(), bytes.size());
        check(out.good(), "Cannot write ASAR worker fixture");
      }
      const Json previewRequest{{"schema_version", 1},
                                {"path", asarRoot.string()}};
      preview = web.execute("web_import_preview", previewRequest);
      const auto committed = web.execute(
          "web_import_commit",
          {{"schema_version", 1}, {"preview_token", preview["preview_token"]}});
      const auto asarProject = committed["project_id"].get<std::string>();
      reply =
          process.call("web_import_preview", previewRequest, "3", nextProject);
      reply =
          process.call("web_import_commit",
                       {{"schema_version", 1},
                        {"preview_token", reply["payload"]["preview_token"]}},
                       "3", nextProject);
      check(reply["payload"] == committed && committed["revision"] == "4",
            "ASAR directory capture differs across worker transport");
      const Json artifactRequest{{"schema_version", 1},
                                 {"revision", "4"},
                                 {"offset", 0},
                                 {"limit", 16}};
      const auto artifacts = web.execute("web_artifacts", artifactRequest);
      reply = process.call("web_artifacts", artifactRequest, "4", asarProject);
      check(reply["payload"] == artifacts && artifacts["items"].size() == 4 &&
                artifacts["items"][2]["kind"] == "directory",
            "ASAR fixture capture lost deterministic occurrences");
      Json extractionRequest{
          {"schema_version", 1},
          {"revision", "4"},
          {"artifact_id", artifacts["items"][1]["artifact_id"]}};
      auto extracted = web.execute("web_asar_extract", extractionRequest);
      reply =
          process.call("web_asar_extract", extractionRequest, "4", asarProject);
      check(reply["payload"] == extracted &&
                extracted["extraction_status"] == "partial",
            "Unselected ASAR unpacked directory was silently discovered");
      extractionRequest["unpacked_directory_id"] =
          artifacts["items"][2]["artifact_id"];
      extracted = web.execute("web_asar_extract", extractionRequest);
      reply =
          process.call("web_asar_extract", extractionRequest, "4", asarProject);
      check(
          reply["payload"] == extracted &&
              extracted["extraction_status"] == "complete" &&
              reply["analysis_state"] == "partial",
          "ASAR shared backend or analysis coverage differs through transport");
      Json recordsRequest{{"schema_version", 1},
                          {"revision", "4"},
                          {"extraction_id", extracted["extraction_id"]},
                          {"offset", 0},
                          {"limit", 16}};
      const auto members = web.execute("web_asar_records", recordsRequest);
      reply =
          process.call("web_asar_records", recordsRequest, "4", asarProject);
      check(reply["payload"] == members && members["items"].size() == 2,
            "ASAR member pages differ through transport");
      check(reply.dump().find("SECRET_WEB") == std::string::npos,
            "ASAR member page exposed private names");
      auto invalid = recordsRequest;
      invalid["limit"] = 0;
      rejects([&] { return web.execute("web_asar_records", invalid); },
              "invalid_page");
      reply = process.call("web_asar_records", invalid, "4", asarProject);
      check(reply["error"]["code"] == "invalid_page",
            "Worker bypassed ASAR page limits");
      invalid = extractionRequest;
      invalid["execute"] = true;
      reply = process.call("web_asar_extract", invalid, "4", asarProject);
      check(reply["error"]["code"] == "invalid_request",
            "Worker accepted unknown ASAR action fields");
      if (hasBindings) {
        const Json request{{"schema_version", 1},
                           {"revision", "4"},
                           {"artifact_id", members["items"][0]["member_id"]},
                           {"source_type", "module"}};
        const auto analyzedSource = web.execute("web_source_analyze", request);
        reply = process.call("web_source_analyze", request, "4", asarProject);
        check(reply["payload"] == analyzedSource,
              "ASAR source identity differs through transport");
        const Json anchorRequest{{"schema_version", 1},
                                 {"revision", "4"},
                                 {"source_id", analyzedSource["source_id"]},
                                 {"byte_offset", "7"},
                                 {"byte_length", "3"}};
        const auto anchor = web.execute("web_source_anchor", anchorRequest);
        reply =
            process.call("web_source_anchor", anchorRequest, "4", asarProject);
        check(reply["payload"] == anchor &&
                  anchor["storage"]["kind"] == "asar_packed_member",
              "ASAR source lost exact packed storage through transport");
      }
      const Json request{{"schema_version", 1},
                         {"revision", "4"},
                         {"selection_id", members["items"][1]["member_id"]}};
      const auto asarHandoff = web.execute("web_native_open", request);
      reply = process.call("web_native_open", request, "4", asarProject);
      check(reply["payload"] == asarHandoff &&
                asarHandoff["origin"]["kind"] == "asar_unpacked_member" &&
                asarHandoff["origin"]["storage_artifact_id"] ==
                    artifacts["items"][3]["artifact_id"],
            "Unpacked ASAR native handoff lost its captured origin");
      invalid = recordsRequest;
      invalid["revision"] = "3";
      rejects([&] { return web.execute("web_asar_records", invalid); },
              "stale_revision");
      reply = process.call("web_asar_records", invalid, "4", asarProject);
      check(reply["error"]["code"] == "stale_revision",
            "Worker accepted a stale ASAR result request");
    } else {
      const auto artifacts =
          web.execute("web_artifacts", {{"schema_version", 1},
                                        {"revision", "3"},
                                        {"offset", 0},
                                        {"limit", 1}});
      const Json request{{"schema_version", 1},
                         {"revision", "3"},
                         {"artifact_id", artifacts["items"][0]["artifact_id"]}};
      rejects([&] { return web.execute("web_asar_extract", request); },
              "archive_path_policy_unavailable");
      reply = process.call("web_asar_extract", request, "3", nextProject);
      check(reply["error"]["code"] == "archive_path_policy_unavailable",
            "Missing ASAR policy used an undeclared fallback");
    }
    const auto beforeElectron = web.execute("web_metadata", empty);
    const auto beforeRevision = beforeElectron["revision"].get<std::string>();
    const auto beforeProject = beforeElectron["project_id"].get<std::string>();
    const auto electronRoot = fixture.root / "electron";
    fs::create_directory(electronRoot);
    for (const auto &[path, bytes] :
         std::initializer_list<std::pair<fs::path, std::string_view>>{
             {electronRoot / "main.js",
              "const {ipcMain,BrowserWindow} = require('electron'); "
              "ipcMain.handle('SECRET_WEB', handler); "
              "const path=require('path'); const win=new BrowserWindow({"
              "webPreferences:{preload:path.join(__dirname,'preload.js')}}); "
              "win.loadFile('index.html'); "
              "win.loadURL('https://example.invalid/SECRET_WEB');"},
             {electronRoot / "package.json",
              R"({"name":"SECRET_WEB","main":"main.js"})"},
             {electronRoot / "preload.js",
              "const {ipcRenderer} = require('electron'); "
              "ipcRenderer.invoke('SECRET_WEB');"}}) {
      std::ofstream out(path, std::ios::binary);
      out.write(bytes.data(), bytes.size());
      check(out.good(), "Cannot write Electron fixture");
    }
    const Json electronPreview{{"schema_version", 1},
                               {"path", electronRoot.string()}};
    preview = web.execute("web_import_preview", electronPreview);
    const auto electronCommit = web.execute(
        "web_import_commit",
        {{"schema_version", 1}, {"preview_token", preview["preview_token"]}});
    reply = process.call("web_import_preview", electronPreview, beforeRevision,
                         beforeProject);
    reply = process.call("web_import_commit",
                         {{"schema_version", 1},
                          {"preview_token", reply["payload"]["preview_token"]}},
                         beforeRevision, beforeProject);
    check(reply["payload"] == electronCommit,
          "Electron snapshot identity differs through transport");
    const auto electronRevision = electronCommit["revision"].get<std::string>();
    const auto electronProject =
        electronCommit["project_id"].get<std::string>();
    const auto electronArtifacts =
        web.execute("web_artifacts", {{"schema_version", 1},
                                      {"revision", electronRevision},
                                      {"offset", 0},
                                      {"limit", 8}});
    check(electronArtifacts["items"].size() == 4,
          "Electron fixture lost captured members");
    Json manifestRequest{
        {"schema_version", 1},
        {"revision", electronRevision},
        {"artifact_id", electronArtifacts["items"][2]["artifact_id"]}};
    const auto manifestResult =
        web.execute("web_electron_manifest_analyze", manifestRequest);
    reply = process.call("web_electron_manifest_analyze", manifestRequest,
                         electronRevision, electronProject);
    check(reply["payload"] == manifestResult &&
              reply["analysis_state"] == "partial" &&
              manifestResult["main_artifact_id"] ==
                  electronArtifacts["items"][1]["artifact_id"],
          "Electron manifest entry differs through transport");
    check(reply.dump().find("SECRET_WEB") == std::string::npos,
          "Electron manifest exposed a declaration");
    auto badManifestRequest = manifestRequest;
    badManifestRequest["execute"] = true;
    reply = process.call("web_electron_manifest_analyze", badManifestRequest,
                         electronRevision, electronProject);
    check(reply["error"]["code"] == "invalid_request",
          "Worker accepted an undeclared Electron action");
    Json electronSourceRequest{{"schema_version", 1},
                               {"revision", electronRevision},
                               {"source_id", "not-yet-analyzed"}};
    if (hasBindings) {
      const Json sourceRequest{
          {"schema_version", 1},
          {"revision", electronRevision},
          {"artifact_id", manifestResult["main_artifact_id"]},
          {"source_type", "commonjs"}};
      const auto sourceResult =
          web.execute("web_source_analyze", sourceRequest);
      reply = process.call("web_source_analyze", sourceRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == sourceResult,
            "Electron source identity differs through transport");
      electronSourceRequest["source_id"] = sourceResult["source_id"];
      const auto electron =
          web.execute("web_electron_source_analyze", electronSourceRequest);
      reply = process.call("web_electron_source_analyze", electronSourceRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == electron && electron["boundary_count"] == 5 &&
                electron["runtime_targets_verified"] == false,
            "Electron boundary evidence differs through transport");
      auto recordsRequest = electronSourceRequest;
      recordsRequest["offset"] = 0;
      recordsRequest["limit"] = 4;
      const auto records =
          web.execute("web_electron_source_records", recordsRequest);
      reply = process.call("web_electron_source_records", recordsRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == records &&
                records["items"][0]["kind"] == "ipc_main_handle",
            "Electron records differ through transport");
      check(reply.dump().find("SECRET_WEB") == std::string::npos,
            "Electron channel leaked through transport");
      recordsRequest["limit"] = 0;
      reply = process.call("web_electron_source_records", recordsRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "invalid_page",
            "Worker bypassed Electron page limits");
      auto preloadRequest = sourceRequest;
      preloadRequest["artifact_id"] =
          electronArtifacts["items"][3]["artifact_id"];
      const auto preloadSource =
          web.execute("web_source_analyze", preloadRequest);
      reply = process.call("web_source_analyze", preloadRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == preloadSource,
            "Preload source differs through transport");
      auto preloadEvidenceRequest = electronSourceRequest;
      preloadEvidenceRequest["source_id"] = preloadSource["source_id"];
      const auto preloadEvidence =
          web.execute("web_electron_source_analyze", preloadEvidenceRequest);
      reply =
          process.call("web_electron_source_analyze", preloadEvidenceRequest,
                       electronRevision, electronProject);
      check(reply["payload"] == preloadEvidence,
            "Preload evidence differs through transport");
      Json ipcRequest{
          {"schema_version", 1},
          {"revision", electronRevision},
          {"manifest_artifact_id", manifestRequest["artifact_id"]},
          {"source_ids", Json::array({sourceResult["source_id"],
                                      preloadSource["source_id"]})}};
      const auto ipc = web.execute("web_electron_ipc_analyze", ipcRequest);
      reply = process.call("web_electron_ipc_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == ipc && ipc["endpoint_count"] == 2 &&
                ipc["channel_count"] == 1 &&
                ipc["runtime_routing_verified"] == false,
            "IPC correlation differs through transport");
      for (const auto kind : {"sources", "channels", "endpoints"}) {
        const Json request{{"schema_version", 1},
                           {"revision", electronRevision},
                           {"electron_ipc_id", ipc["electron_ipc_id"]},
                           {"record_kind", kind},
                           {"offset", 0},
                           {"limit", 4}};
        const auto page = web.execute("web_electron_ipc_records", request);
        reply = process.call("web_electron_ipc_records", request,
                             electronRevision, electronProject);
        check(reply["payload"] == page, "IPC page differs through transport");
        check(reply.dump().find("SECRET_WEB") == std::string::npos,
              "IPC page exposed a channel value");
        if (std::string_view(kind) == "channels")
          check(page["items"][0]["invoke_handle_candidate_pairs"] == 1,
                "IPC candidate pair was lost");
      }
      const auto entries =
          web.execute("web_electron_entries_analyze", ipcRequest);
      reply = process.call("web_electron_entries_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["payload"] == entries && entries["entry_count"] == 3 &&
                entries["linked_file_candidate_count"] == 1 &&
                entries["runtime_entries_verified"] == false &&
                entries["runtime_path_bases_verified"] == false &&
                entries["html_analysis"] == "not_analyzed",
            "Entry association differs through transport");
      for (const auto kind : {"sources", "entries"}) {
        Json request{{"schema_version", 1},
                     {"revision", electronRevision},
                     {"electron_entries_id", entries["electron_entries_id"]},
                     {"record_kind", kind},
                     {"offset", 0},
                     {"limit", 4}};
        const auto page = web.execute("web_electron_entry_records", request);
        reply = process.call("web_electron_entry_records", request,
                             electronRevision, electronProject);
        check(reply["payload"] == page, "Entry page differs through transport");
        check(reply.dump().find("SECRET_WEB") == std::string::npos &&
                  reply.dump().find("preload.js") == std::string::npos &&
                  reply.dump().find("index.html") == std::string::npos,
              "Entry page exposed a private path");
        if (std::string_view(kind) == "entries") {
          check(page["items"][0]["target_artifact_id"] ==
                        electronArtifacts["items"][3]["artifact_id"] &&
                    page["items"][0]["selected_target_source_id"] ==
                        preloadSource["source_id"] &&
                    page["items"][1]["link_status"] ==
                        "no_exact_admitted_file" &&
                    page["items"][2]["link_status"] == "url_base_not_captured",
                "Entry candidate or unresolved reason was lost");
          request["limit"] = 0;
          reply = process.call("web_electron_entry_records", request,
                               electronRevision, electronProject);
          check(reply["error"]["code"] == "invalid_page",
                "Worker bypassed entry page limits");
        }
      }
      ipcRequest["execute"] = true;
      reply = process.call("web_electron_entries_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "invalid_request",
            "Worker accepted an entry execution field");
      reply = process.call("web_electron_ipc_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "invalid_request",
            "Worker accepted an IPC execution field");
    } else {
      rejects(
          [&] {
            return web.execute("web_electron_source_analyze",
                               electronSourceRequest);
          },
          "capability_unavailable");
      reply = process.call("web_electron_source_analyze", electronSourceRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "capability_unavailable",
            "Missing parser used an Electron fallback");
      const Json ipcRequest{
          {"schema_version", 1},
          {"revision", electronRevision},
          {"manifest_artifact_id", manifestRequest["artifact_id"]},
          {"source_ids", Json::array({"unavailable-source"})}};
      rejects(
          [&] { return web.execute("web_electron_ipc_analyze", ipcRequest); },
          "capability_unavailable");
      reply = process.call("web_electron_ipc_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "capability_unavailable",
            "Missing parser used an IPC fallback");
      rejects(
          [&] {
            return web.execute("web_electron_entries_analyze", ipcRequest);
          },
          "capability_unavailable");
      reply = process.call("web_electron_entries_analyze", ipcRequest,
                           electronRevision, electronProject);
      check(reply["error"]["code"] == "capability_unavailable",
            "Missing parser used an entry fallback");
    }
    manifestRequest["revision"] = beforeRevision;
    reply = process.call("web_electron_manifest_analyze", manifestRequest,
                         beforeRevision, electronProject);
    check(reply["error"]["code"] == "stale_revision",
          "Worker accepted an old Electron capture revision");
    const auto htmlRoot = fixture.root / "html";
    fs::create_directory(htmlRoot);
    for (const auto &[path, bytes] :
         std::initializer_list<std::pair<fs::path, std::string_view>>{
             {htmlRoot / "a.html",
              "<base href='./'><script type=importmap>{\"imports\":{"
              "\"SECRET_WEB\":\"./b.js?SECRET_WEB#f\"}}</script><script>const "
              "x='SECRET_WEB';import('SECRET_WEB');</script><script "
              "src='b.js?SECRET_WEB'></script>"},
             {htmlRoot / "b.js", "throw Error('SECRET_WEB');"}}) {
      std::ofstream out(path, std::ios::binary);
      out.write(bytes.data(), bytes.size());
      check(out.good(), "Cannot write HTML fixture");
    }
    const Json htmlPreview{{"schema_version", 1}, {"path", htmlRoot.string()}};
    preview = web.execute("web_import_preview", htmlPreview);
    const auto htmlCommit = web.execute(
        "web_import_commit",
        {{"schema_version", 1}, {"preview_token", preview["preview_token"]}});
    reply = process.call("web_import_preview", htmlPreview, electronRevision,
                         electronProject);
    reply = process.call("web_import_commit",
                         {{"schema_version", 1},
                          {"preview_token", reply["payload"]["preview_token"]}},
                         electronRevision, electronProject);
    check(reply["payload"] == htmlCommit,
          "HTML snapshot identity differs through transport");
    const auto htmlRevision = htmlCommit["revision"].get<std::string>();
    const auto htmlProject = htmlCommit["project_id"].get<std::string>();
    const auto htmlArtifacts =
        web.execute("web_artifacts", {{"schema_version", 1},
                                      {"revision", htmlRevision},
                                      {"offset", 0},
                                      {"limit", 8}});
    check(htmlArtifacts["items"].size() == 3, "HTML fixture lost members");
    Json htmlRequest{{"schema_version", 1},
                     {"revision", htmlRevision},
                     {"artifact_id", htmlArtifacts["items"][1]["artifact_id"]}};
    const auto html = web.execute("web_html_analyze", htmlRequest);
    reply = process.call("web_html_analyze", htmlRequest, htmlRevision,
                         htmlProject);
    check(reply["payload"] == html && html["script_count"] == 3 &&
              html["import_map_count"] == 1 && html["base_count"] == 1 &&
              html["runtime_entries_verified"] == false,
          "HTML summary differs through transport");
    Json htmlScripts;
    for (const auto kind : {"scripts", "bases", "import_maps"}) {
      Json request{{"schema_version", 1},
                   {"revision", htmlRevision},
                   {"html_id", html["html_id"]},
                   {"record_kind", kind},
                   {"offset", 0},
                   {"limit", 3}};
      const auto page = web.execute("web_html_records", request);
      reply =
          process.call("web_html_records", request, htmlRevision, htmlProject);
      check(reply["payload"] == page &&
                reply.dump().find("SECRET_WEB") == std::string::npos,
            "HTML page differs or exposes private content");
      if (std::string_view(kind) == "scripts")
        htmlScripts = page;
      request["fetch"] = true;
      reply =
          process.call("web_html_records", request, htmlRevision, htmlProject);
      check(reply["error"]["code"] == "invalid_request",
            "Worker accepted HTML fetch field");
    }
    check(htmlScripts["items"][2]["candidate_artifact_id"] ==
              htmlArtifacts["items"][2]["artifact_id"],
          "HTML external candidate lost occurrence");
    const Json htmlSourceRequest{
        {"schema_version", 1},
        {"revision", htmlRevision},
        {"artifact_id", htmlScripts["items"][1]["inline_artifact_id"]},
        {"source_type", "script"}};
    if (hasBindings) {
      const auto source = web.execute("web_source_analyze", htmlSourceRequest);
      reply = process.call("web_source_analyze", htmlSourceRequest,
                           htmlRevision, htmlProject);
      check(reply["payload"] == source && source["parse_status"] == "parsed",
            "HTML inline source differs through transport");
      const Json position{{"schema_version", 1},
                          {"revision", htmlRevision},
                          {"source_id", source["source_id"]},
                          {"byte_offset", "0"},
                          {"byte_length", "5"}};
      const auto location = web.execute("web_source_anchor", position);
      reply = process.call("web_source_anchor", position, htmlRevision,
                           htmlProject);
      check(reply["payload"] == location &&
                location["storage"]["kind"] == "html_inline_script",
            "HTML origin differs through transport");
      const Json modulesRequest{{"schema_version", 1},
                                {"revision", htmlRevision},
                                {"source_id", source["source_id"]}};
      const auto modules =
          web.execute("web_source_modules_analyze", modulesRequest);
      reply = process.call("web_source_modules_analyze", modulesRequest,
                           htmlRevision, htmlProject);
      check(reply["payload"] == modules &&
                modules["link_context"]["html_id"] == html["html_id"],
            "HTML module context differs through transport");
      auto requestsQuery = modulesRequest;
      requestsQuery["record_kind"] = "requests";
      const auto requests =
          web.execute("web_source_module_records", requestsQuery);
      reply = process.call("web_source_module_records", requestsQuery,
                           htmlRevision, htmlProject);
      check(reply["payload"] == requests && requests["items"].size() == 1 &&
                requests["items"][0]["candidate_artifact_id"] ==
                    htmlArtifacts["items"][2]["artifact_id"] &&
                requests["items"][0]["query_present"] == true &&
                requests["items"][0]["fragment_present"] == true &&
                requests["items"][0]["import_map_match"] == "exact" &&
                requests["items"][0]["module_url_candidate_id"].is_string() &&
                reply.dump().find("SECRET_WEB") == std::string::npos,
            "HTML module links differ or expose a private reference");
    } else {
      reply = process.call("web_source_analyze", htmlSourceRequest,
                           htmlRevision, htmlProject);
      check(reply["error"]["code"] == "capability_unavailable",
            "HTML bypassed omitted JS parser");
    }
    htmlRequest["execute"] = true;
    reply = process.call("web_html_analyze", htmlRequest, htmlRevision,
                         htmlProject);
    check(reply["error"]["code"] == "invalid_request",
          "Worker accepted HTML execution field");
    process.stop();
    std::ifstream errors(fixture.root / "stderr");
    const std::string diagnostics{std::istreambuf_iterator<char>(errors), {}};
    check(diagnostics.find("SECRET_WEB") == std::string::npos,
          "Worker stderr exposed private evidence");
#endif
    std::cout << "web worker adapter, transport and domain isolation passed\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
