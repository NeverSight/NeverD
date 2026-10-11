//===- ProcessTests.cpp - Real native MCP stdio contracts -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Launch only NeverD's C++ server with an unusable PATH and inert input bytes.
///
//===----------------------------------------------------------------------===//

#include "../../../unittests/web/SEAFixture.h"
#include "TestSupport.h"

#include <cstdlib>
#include <iostream>
#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

using namespace neverd::mcp;
using namespace neverd::mcp::test;
namespace transport = neverd::transport;
namespace {
#ifndef _WIN32
class Process {
  int Input = -1, Output = -1;
  pid_t PID = -1;
  std::string Pending;
  unsigned Sequence = 0;

public:
  Process(const char *Binary, const std::vector<std::string> &Options,
          const std::filesystem::path &Errors) {
    int In[2], Out[2];
    require(pipe(In) == 0, "Cannot create test input pipe");
    if (pipe(Out) != 0) {
      close(In[0]);
      close(In[1]);
      throw std::runtime_error("Cannot create test output pipe");
    }
    const auto Log = open(Errors.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (Log < 0) {
      for (const auto FD : {In[0], In[1], Out[0], Out[1]})
        close(FD);
      throw std::runtime_error("Cannot capture test diagnostics");
    }
    posix_spawn_file_actions_t Actions;
    posix_spawn_file_actions_init(&Actions);
    posix_spawn_file_actions_adddup2(&Actions, In[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&Actions, Out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&Actions, Log, STDERR_FILENO);
    for (const auto FD : {In[0], In[1], Out[0], Out[1], Log})
      posix_spawn_file_actions_addclose(&Actions, FD);
    char Path[] = "PATH=/neverd-no-external-tools";
    char Cache[] = "NEVERD_SIGNATURE_CACHE=off";
    char *Environment[] = {Path, Cache, nullptr};
    std::vector<char *> Arguments{const_cast<char *>(Binary)};
    for (const auto &Option : Options)
      Arguments.push_back(const_cast<char *>(Option.c_str()));
    Arguments.push_back(nullptr);
    const auto Status = posix_spawn(&PID, Binary, &Actions, nullptr,
                                    Arguments.data(), Environment);
    posix_spawn_file_actions_destroy(&Actions);
    close(In[0]);
    close(Out[1]);
    close(Log);
    if (Status != 0) {
      close(In[1]);
      close(Out[0]);
      PID = -1;
      throw std::runtime_error("Cannot launch native MCP server");
    }
    Input = In[1];
    Output = Out[0];
  }
  ~Process() {
    if (Input >= 0)
      close(Input);
    if (Output >= 0)
      close(Output);
    if (PID > 0) {
      kill(PID, SIGKILL);
      while (waitpid(PID, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
  void send(std::string_view Bytes) {
    while (!Bytes.empty()) {
      const auto Written = write(Input, Bytes.data(), Bytes.size());
      if (Written < 0 && errno == EINTR)
        continue;
      require(Written > 0, "Cannot write MCP test request");
      Bytes.remove_prefix(static_cast<std::size_t>(Written));
    }
  }
  void eof() {
    close(Input);
    Input = -1;
  }
  Json receive() {
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      const auto LF = Pending.find('\n');
      if (LF != std::string::npos) {
        auto Message = Pending.substr(0, LF);
        Pending.erase(0, LF + 1);
        const auto Reply = transport::parseJson(Message, MaxResponseBytes);
        redacted(Reply);
        return Reply;
      }
      const auto Remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              Deadline - std::chrono::steady_clock::now())
              .count();
      require(Remaining > 0, "MCP response timed out");
      pollfd FD{Output, POLLIN, 0};
      const auto Ready = poll(&FD, 1, static_cast<int>(Remaining));
      if (Ready < 0 && errno == EINTR)
        continue;
      require(Ready > 0, "MCP output timed out");
      char Buffer[4096];
      const auto Count = read(Output, Buffer, sizeof(Buffer));
      if (Count < 0 && errno == EINTR)
        continue;
      require(Count > 0, "MCP closed an incomplete response");
      Pending.append(Buffer, Count);
      require(Pending.size() <= MaxResponseBytes + 1,
              "MCP exceeded response bound");
    }
  }
  Json call(const char *Method, Json Parameters = Json::object()) {
    const auto ID = ++Sequence;
    send(request(Method, std::move(Parameters), ID).dump() + "\n");
    auto Reply = receive();
    require(Reply.at("id") == ID,
            "Notification replied or response order changed");
    return Reply;
  }
  Json tool(const char *Name, Json Args = Json::object()) {
    const auto Started = std::chrono::steady_clock::now();
    try {
      auto Result =
          evidence(call("tools/call", {{"name", "neverd_" + std::string(Name)},
                                       {"arguments", std::move(Args)}})
                       .at("result"));
      const auto Elapsed =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - Started);
      std::cout << Name << ": " << Elapsed.count() << " ms\n";
      return Result;
    } catch (const std::exception &E) {
      throw std::runtime_error(std::string(Name) + ": " + E.what());
    }
  }
  void ready() {
    const auto Init = initialize();
    require(call("initialize", Init.at("params"))
                    .at("result")
                    .at("protocolVersion") == ProtocolVersion,
            "MCP initialization failed");
    send("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n");
  }
  void finish(int Expected = 0) {
    if (Input >= 0)
      eof();
    const auto Deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(15);
    for (;;) {
      int Status = 0;
      const auto Reaped = waitpid(PID, &Status, WNOHANG);
      if (Reaped == PID) {
        PID = -1;
        require(WIFEXITED(Status) && WEXITSTATUS(Status) == Expected,
                "MCP exited unsuccessfully");
        break;
      }
      require(Reaped == 0 || errno == EINTR, "Cannot reap MCP server");
      require(std::chrono::steady_clock::now() < Deadline,
              "MCP did not exit after EOF");
      poll(nullptr, 0, 10);
    }
    char Extra;
    require(Pending.empty() && read(Output, &Extra, 1) == 0,
            "MCP wrote unsolicited stdout output");
  }
};

void fullSession(const char *Binary, Fixture &F) {
  Process P(Binary, {"--stdio", "--input", F.path()}, F.Root / "session.log");
  P.ready();
  const auto Caps = P.tool("web_capabilities");
  require(Caps.at("mcp").at("input_count") == 1,
          "Configured input count changed");
  require(P.call("tools/list").contains("result"),
          "Real tool discovery failed");
  require(
      P.call("tools/call", {{"name", "neverd_web_import_preview"},
                            {"arguments", {{"path", "CANARY_UNCONFIGURED"}}}})
              .at("error")
              .at("code") == -32602,
      "Wire protocol admitted an unconfigured input path");
  const auto &Backend = Caps.at("backend");
  if (Backend.value("status", "") == "ok" &&
      Backend.value("input_reader", "") != "unavailable") {
    // Launch preceded creation: import must read only when explicitly called.
    F.write("input", neverd::web::sea_test::blob(12));
    const auto Preview = P.tool("web_import_preview", {{"input_index", 0}});
    P.send(
        R"({"jsonrpc":"2.0","method":"tools/call","params":{"name":"neverd_web_import_commit","arguments":{"preview_token":"CANARY"}}})"
        "\n");
    const auto Commit = P.tool(
        "web_import_commit", {{"preview_token", Preview.at("preview_token")}});
    const auto Revision = Commit.at("revision");
    const auto Items = P.tool("web_artifacts", {{"revision", Revision}});
    const auto Artifact = Items.at("items")[0].at("artifact_id");
    const auto SEA = P.tool("web_sea_extract",
                            {{"revision", Revision},
                             {"artifact_id", Artifact},
                             {"profile", "node-sea-22.15.0-blob-le64-v1"}});
    require(SEA.at("asset_count") == 2 &&
                SEA.at("runtime_activation") == "not_checked",
            "Real MCP lost SEA evidence");
    P.tool("web_sea_records", {{"revision", Revision},
                               {"extraction_id", SEA.at("extraction_id")}});
    const auto &Ops = Backend.at("operations");
    if (std::find(Ops.begin(), Ops.end(), "source_analyze") != Ops.end()) {
      const auto Source = P.tool("web_source_analyze",
                                 {{"revision", Revision},
                                  {"artifact_id", SEA.at("source_artifact_id")},
                                  {"source_type", "commonjs"}});
      P.tool("web_source_anchor", {{"revision", Revision},
                                   {"source_id", Source.at("source_id")},
                                   {"byte_offset", "0"},
                                   {"byte_length", "6"}});
    }
  }
  require(
      P.call("initialize", initialize().at("params")).at("error").at("code") ==
          -32600,
      "Repeated initialize was admitted");
  require(P.call("ping").at("result").empty(),
          "MCP ping failed after requests");
  P.finish();
  require(std::filesystem::file_size(F.Root / "session.log") == 0,
          "MCP logged unexpected analysis diagnostics");
}

void framing(const char *Binary, Fixture &F) {
  {
    Process P(Binary, {}, F.Root / "framing.log");
    P.send(request("ping").dump() + "\r\n");
    require(P.receive().at("result").empty(), "CRLF request failed");
    P.send(request("ping", Json::object(), 2).dump());
    P.eof();
    require(P.receive().at("error").at("code") == -32700,
            "Truncated input executed");
    P.finish(2);
  }
  {
    Process P(Binary, {}, F.Root / "oversize.log");
    P.send(std::string(MaxRequestBytes + 1, 'x'));
    require(P.receive().at("error").at("code") == -32700,
            "Oversized input executed");
    P.finish(2);
  }
  {
    std::vector<std::string> Arguments;
    for (unsigned I = 0; I < 9; ++I) {
      Arguments.emplace_back("--input");
      Arguments.emplace_back("CANARY_PRIVATE_PATH");
    }
    Process P(Binary, Arguments, F.Root / "arguments.log");
    P.finish(2);
    std::ifstream Log(F.Root / "arguments.log");
    std::string Text((std::istreambuf_iterator<char>(Log)), {});
    require(Text.find("CANARY") == std::string::npos && Text.size() < 256,
            "Launch errors exposed private paths");
  }
}

int claudeSession(const char *Binary, Fixture &F) {
  const auto *Path = std::getenv("NEVERD_CLAUDE_CODE_21296_ELF");
  if (!Path) {
    std::cout << "Optional pinned Claude Code input not supplied\n";
    return 77;
  }
  Process P(Binary, {"--input", Path}, F.Root / "claude.log");
  P.ready();
  const auto Caps = P.tool("web_capabilities");
  if (Caps.at("backend").value("status", "") != "ok" ||
      Caps.at("backend").value("input_reader", "") == "unavailable") {
    P.finish();
    return 77;
  }
  const auto Preview = P.tool("web_import_preview", {{"input_index", 0}});
  const auto Commit = P.tool("web_import_commit",
                             {{"preview_token", Preview.at("preview_token")}});
  const auto Revision = Commit.at("revision");
  const auto Artifacts = P.tool("web_artifacts", {{"revision", Revision}});
  require(Artifacts.at("items").size() == 1,
          "Pinned input is not one artifact");
  const auto &Artifact = Artifacts.at("items")[0];
  require(Artifact.at("size") == "257068216" &&
              Artifact.at("blob_sha256") == "24972e3bc859fab2b46ed4c1e51f7d6130"
                                            "f06d3bd550811a114640de3370d0de",
          "Supplied input differs from the pinned Claude Code artifact");
  const auto Bun =
      P.tool("web_bun_extract", {{"revision", Revision},
                                 {"artifact_id", Artifact.at("artifact_id")}});
  require(Bun.at("module_count") == 2589 && Bun.at("region_count") == 10005,
          "MCP lost pinned Bun module or region evidence");
  unsigned Rows = 0, Sources = 0, Caches = 0, Maps = 0;
  for (unsigned Offset = 0; Offset < 2589; Offset += 128) {
    const auto Page =
        P.tool("web_bun_records", {{"revision", Revision},
                                   {"extraction_id", Bun.at("extraction_id")},
                                   {"record_kind", "modules"},
                                   {"offset", Offset},
                                   {"limit", 128}});
    require(!Page.at("items").empty(), "MCP module page ended early");
    for (const auto &M : Page.at("items")) {
      require(M.at("module_index") == Rows++,
              "MCP module pages lost source order");
      Sources += !M.at("source_artifact_id").is_null();
      Caches += !M.at("bytecode_region_id").is_null();
      Maps += !M.at("source_map_region_id").is_null();
      require(M.at("name_redacted") == true, "MCP disclosed a module name");
    }
  }
  require(Rows == 2589 && Sources == 2345 && Caches == 2343 && Maps == 0,
          "MCP did not preserve the complete pinned module inventory");
  P.finish();
  require(std::filesystem::file_size(F.Root / "claude.log") == 0,
          "MCP logged private sample diagnostics");
  std::cout
      << "Pinned Claude Code MCP extraction: 2345 JS modules, 244 assets, "
         "2343 opaque caches; no sample execution\n";
  return 0;
}
#endif
} // namespace

int main(int Count, char **Arguments) {
#ifdef _WIN32
  std::cout << "POSIX subprocess qualification is unavailable on this host\n";
  return 77;
#else
  try {
    require(Count == 2 ||
                (Count == 3 && std::string_view(Arguments[2]) == "--claude"),
            "Pass the native MCP executable and optional --claude");
    std::signal(SIGPIPE, SIG_IGN);
    Fixture F;
    if (Count == 3)
      return claudeSession(Arguments[1], F);
    fullSession(Arguments[1], F);
    framing(Arguments[1], F);
    std::cout << "MCP native stdio, EOF, redaction and unusable-PATH contracts "
                 "passed\n";
    return 0;
  } catch (const std::exception &E) {
    std::cerr << E.what() << '\n';
    return 1;
  }
#endif
}
