//===- BinaryDiffLineageSDKTests.cpp - Offline graph ABI transactions
//------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native-produced reports exercise retained handles, paging, retry, typed
/// stops and byte-exact export through the public C ABI.
//===----------------------------------------------------------------------===//

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {
std::string take(const char *Text) {
  const std::unique_ptr<const char, decltype(&neverd_free_string)> Owned(
      Text, neverd_free_string);
  return Text ? Text : "";
}
std::string render(llvm::json::Value V) {
  std::string S;
  llvm::raw_string_ostream OS(S);
  OS << V;
  return S;
}
llvm::json::Object object(std::string Text) {
  auto V = llvm::json::parse(Text);
  if (!V) {
    ADD_FAILURE() << llvm::toString(V.takeError());
    return {};
  }
  if (!V->getAsObject()) {
    ADD_FAILURE() << "Expected a JSON object";
    return {};
  }
  return *V->getAsObject();
}

class BinaryDiffLineageSDK : public ::testing::Test {
protected:
  neverd_lineage_t Handle = nullptr;
  std::string Report, Document;
  llvm::SmallString<256> Directory;

  void SetUp() override {
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-lineage-sdk", Directory));
    using Session = std::unique_ptr<void, decltype(&neverd_session_destroy)>;
    Session A(neverd_session_create(), neverd_session_destroy);
    Session B(neverd_session_create(), neverd_session_destroy);
    ASSERT_TRUE(A && B);
    unsigned I = 0;
    for (const auto S : {A.get(), B.get()}) {
      const auto Path = (Directory + (I++ ? "/b.raw" : "/a.raw")).str();
      std::error_code Error;
      llvm::raw_fd_ostream OS(Path, Error);
      ASSERT_FALSE(Error);
      const char Bytes[] = {char(0xb8), char(I), 0, 0, 0, char(0xc3)};
      OS.write(Bytes, sizeof(Bytes));
      OS.close();
      ASSERT_FALSE(OS.has_error());
      ASSERT_EQ(neverd_session_set_load_options(
                    S, "{\"loader\":\"binary\",\"processor\":\"x86_64\","
                       "\"platform\":\"sysv\",\"base\":4096}"),
                0);
      ASSERT_EQ(neverd_session_load(S, Path.c_str()), 1);
    }
    std::unique_ptr<void, decltype(&neverd_comparison_destroy)> Comparison(
        neverd_comparison_create(), neverd_comparison_destroy);
    ASSERT_TRUE(Comparison);
    ASSERT_EQ(neverd_comparison_run(Comparison.get(), A.get(), B.get(),
                                    "{\"discover\":false}", nullptr, nullptr),
              1);
    Report = take(neverd_comparison_export_json(Comparison.get()));
    ASSERT_FALSE(Report.empty());
    Document =
        render(llvm::json::Object{{"schema", "neverd.bindiff.lineage"},
                                  {"schema_version", 1},
                                  {"reports", llvm::json::Array{Report}}});
    Handle = neverd_lineage_create();
    ASSERT_TRUE(Handle);
  }
  void TearDown() override {
    neverd_lineage_destroy(Handle);
    if (!Directory.empty())
      (void)llvm::sys::fs::remove_directories(Directory);
  }
  llvm::json::Object status() {
    return object(take(neverd_lineage_status_json(Handle)));
  }
  llvm::json::Object summary() {
    return object(take(neverd_lineage_summary_json(Handle)));
  }
  std::string saved() {
    return take(neverd_lineage_export_json(Handle, nullptr, nullptr));
  }
  std::string query(llvm::json::Object Q,
                    neverd_lineage_progress_t Progress = nullptr,
                    void *Context = nullptr) {
    const auto Text = render(std::move(Q));
    return take(
        neverd_lineage_query_json(Handle, Text.c_str(), Progress, Context));
  }
};
} // namespace

TEST_F(BinaryDiffLineageSDK,
       NativeReportsOutliveTheirSessionsAndRefusalsRetainData) {
  EXPECT_EQ(status().getString("operation_state"), "idle");
  EXPECT_EQ(neverd_lineage_summary_json(Handle), nullptr);
  ASSERT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  const auto S = summary();
  EXPECT_EQ(S.getInteger("versions"), 2);
  EXPECT_EQ(S.getInteger("nodes"), 2);
  EXPECT_EQ(S.getBoolean("native_authenticated"), false);
  const auto Revision = S.getString("revision")->str();
  const auto Before = saved();
  auto Page = object(
      query(llvm::json::Object{{"kind", "reports"}, {"revision", Revision}}));
  const auto ID =
      Page.getArray("items")->front().getAsObject()->getString("id")->str();
  EXPECT_EQ(
      take(neverd_lineage_report_json(Handle, ID.c_str(), nullptr, nullptr)),
      Report);
  EXPECT_TRUE(
      query(llvm::json::Object{{"kind", "versions"}, {"revision", "stale"}})
          .empty());
  EXPECT_EQ(status().getString("operation_state"), "failed");
  EXPECT_EQ(
      neverd_lineage_load_json(Handle, "{invalid", nullptr, nullptr, nullptr),
      0);
  EXPECT_EQ(status().getString("loaded_revision"), Revision);
  EXPECT_EQ(saved(), Before);
  for (int I = 0; I < 2; ++I) {
    auto Row = object(query(llvm::json::Object{{"kind", "versions"},
                                               {"revision", Revision},
                                               {"offset", I},
                                               {"limit", 1}}));
    ASSERT_EQ(Row.getArray("items")->size(), 1u);
    EXPECT_EQ(Row.getInteger("total"), 2);
  }
  EXPECT_EQ(neverd_lineage_load_json(Handle, Before.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  EXPECT_EQ(saved(), Before);
}

TEST_F(BinaryDiffLineageSDK,
       CancellationAtEveryPublicationBoundaryIsRetryable) {
  ASSERT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  const auto Before = saved();
  const auto Revision = summary().getString("revision")->str();
  struct Probe {
    neverd_lineage_t Handle;
    std::string Phase;
    std::thread::id Owner = std::this_thread::get_id();
    bool Reached = false;
  };
  const auto Progress =
      +[](void *Context, const char *Phase, neverd_va_t, neverd_va_t) {
        auto &P = *static_cast<Probe *>(Context);
        EXPECT_EQ(P.Owner, std::this_thread::get_id());
        const auto Status = object(take(neverd_lineage_status_json(P.Handle)));
        EXPECT_EQ(Status.getString("operation_state"), "running");
        if (P.Phase == Phase) {
          P.Reached = true;
          neverd_lineage_cancel(P.Handle);
        }
      };
  for (const auto *Phase : {"lineage:parse", "lineage:reports", "lineage:graph",
                            "lineage:publish", "lineage:commit"}) {
    Probe P{Handle, Phase};
    EXPECT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr,
                                       Progress, &P),
              0);
    EXPECT_TRUE(P.Reached) << Phase;
    const auto Status = status();
    EXPECT_EQ(Status.getString("operation_state"), "cancelled");
    EXPECT_EQ(Status.getString("loaded_revision"), Revision);
    EXPECT_FALSE(*Status.getBoolean("running"));
    EXPECT_EQ(saved(), Before);
  }
  EXPECT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  neverd_lineage_cancel(Handle);
  EXPECT_EQ(status().getString("operation_state"), "complete");
  const auto Delay = +[](void *, const char *Phase, neverd_va_t, neverd_va_t) {
    if (llvm::StringRef(Phase) == "lineage:start")
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  EXPECT_EQ(neverd_lineage_load_json(Handle, Document.c_str(),
                                     "{\"timeout_ms\":1}", Delay, nullptr),
            0);
  EXPECT_EQ(status().getString("operation_state"), "timed_out");
  EXPECT_EQ(saved(), Before);
}

TEST_F(BinaryDiffLineageSDK, TracePagesReuseWorkButHonorCurrentAdmission) {
  ASSERT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  const auto Revision = summary().getString("revision")->str();
  const auto Versions = object(
      query(llvm::json::Object{{"kind", "versions"}, {"revision", Revision}}));
  const auto V =
      Versions.getArray("items")->front().getAsObject()->getString("id")->str();
  llvm::json::Object Q{{"kind", "trace_nodes"},
                       {"revision", Revision},
                       {"version", V},
                       {"address", "0x1000"},
                       {"limit", 1}};
  unsigned Walks = 0;
  const auto Progress =
      +[](void *Context, const char *Phase, neverd_va_t, neverd_va_t) {
        if (llvm::StringRef(Phase) == "lineage:trace")
          ++*static_cast<unsigned *>(Context);
      };
  auto First = object(query(Q, Progress, &Walks));
  ASSERT_GT(Walks, 0u);
  EXPECT_EQ(First.getInteger("total"), 2);
  Walks = 0;
  Q["offset"] = 1;
  EXPECT_FALSE(query(Q, Progress, &Walks).empty());
  EXPECT_EQ(Walks, 0u);
  Q["max_work"] = 1;
  EXPECT_TRUE(query(Q).empty());
  EXPECT_EQ(status().getString("operation_state"), "resource_limited");
  EXPECT_EQ(neverd_lineage_load_json(Handle, Document.c_str(),
                                     "{\"max_nodes\":5}", nullptr, nullptr),
            0);
  EXPECT_EQ(status().getString("operation_state"), "resource_limited");
  EXPECT_EQ(status().getString("loaded_revision"), Revision);
  EXPECT_EQ(neverd_lineage_load_json(Handle, Document.c_str(),
                                     "{\"max_nodes\":6}", nullptr, nullptr),
            1);
  EXPECT_EQ(summary().getString("revision"), Revision);
}

TEST_F(BinaryDiffLineageSDK,
       ReleaseEditsPublishAtomicallyAndPreserveTraceOnStop) {
  ASSERT_EQ(neverd_lineage_load_json(Handle, Document.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  const auto Revision = summary().getString("revision")->str();
  const auto Versions = object(
      query(llvm::json::Object{{"kind", "versions"}, {"revision", Revision}}));
  const auto Version =
      Versions.getArray("items")->front().getAsObject()->getString("id")->str();
  const auto Trace = query(llvm::json::Object{{"kind", "trace_nodes"},
                                              {"revision", Revision},
                                              {"version", Version},
                                              {"address", "0x1000"}});
  ASSERT_FALSE(Trace.empty());
  const auto Before = saved();
  const auto Edit = render(
      llvm::json::Object{{"revision", Revision},
                         {"upsert", llvm::json::Array{llvm::json::Object{
                                        {"id", "release-1"},
                                        {"label", "Stable 1"},
                                        {"branch", "stable"},
                                        {"version", Version},
                                        {"published_at", nullptr},
                                        {"parents", llvm::json::Array{}}}}}});
  struct Probe {
    neverd_lineage_t Handle;
    const char *Phase;
    bool Reached = false;
  };
  const auto Cancel =
      +[](void *Context, const char *Phase, neverd_va_t, neverd_va_t) {
        auto &P = *static_cast<Probe *>(Context);
        if (llvm::StringRef(Phase) == P.Phase) {
          P.Reached = true;
          neverd_lineage_cancel(P.Handle);
        }
      };
  for (const auto Phase : {"lineage:release_edit", "lineage:reuse_graph",
                           "lineage:publish", "lineage:commit"}) {
    SCOPED_TRACE(Phase);
    Probe P{Handle, Phase};
    ASSERT_EQ(neverd_lineage_edit_releases_json(Handle, Edit.c_str(), nullptr,
                                                Cancel, &P),
              0);
    ASSERT_TRUE(P.Reached);
    EXPECT_EQ(status().getString("operation_state"), "cancelled");
    EXPECT_EQ(status().getString("loaded_revision"), Revision);
    EXPECT_EQ(saved(), Before);
    EXPECT_EQ(query(llvm::json::Object{{"kind", "trace_nodes"},
                                       {"revision", Revision},
                                       {"version", Version},
                                       {"address", "0x1000"}}),
              Trace);
  }
  EXPECT_EQ(neverd_lineage_edit_releases_json(
                Handle, Edit.c_str(), "{\"max_work\":1}", nullptr, nullptr),
            0);
  EXPECT_EQ(status().getString("operation_state"), "resource_limited");
  EXPECT_EQ(saved(), Before);
  const auto Delay = +[](void *, const char *Phase, neverd_va_t, neverd_va_t) {
    if (llvm::StringRef(Phase) == "lineage:start")
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
  };
  EXPECT_EQ(neverd_lineage_edit_releases_json(
                Handle, Edit.c_str(), "{\"timeout_ms\":1}", Delay, nullptr),
            0);
  EXPECT_EQ(status().getString("operation_state"), "timed_out");
  EXPECT_EQ(saved(), Before);
  ASSERT_EQ(neverd_lineage_edit_releases_json(Handle, Edit.c_str(), nullptr,
                                              nullptr, nullptr),
            1)
      << take(neverd_lineage_last_error(Handle));
  const auto Current = summary().getString("revision")->str();
  EXPECT_NE(Current, Revision);
  EXPECT_EQ(summary().getInteger("document_schema_version"), 2);
  EXPECT_EQ(summary().getInteger("releases"), 1);
  EXPECT_EQ(summary().getInteger("schema_version"), 1);
  const auto After = saved();
  EXPECT_EQ(neverd_lineage_edit_releases_json(Handle, Edit.c_str(), nullptr,
                                              nullptr, nullptr),
            0);
  EXPECT_NE(take(neverd_lineage_last_error(Handle)).find("stale"),
            std::string::npos);
  EXPECT_EQ(saved(), After);
  EXPECT_TRUE(query(llvm::json::Object{{"kind", "trace_nodes"},
                                       {"revision", Revision},
                                       {"version", Version},
                                       {"address", "0x1000"}})
                  .empty());
  EXPECT_FALSE(query(llvm::json::Object{{"kind", "trace_nodes"},
                                        {"revision", Current},
                                        {"version", Version},
                                        {"address", "0x1000"}})
                   .empty());
  const auto Sources = object(
      query(llvm::json::Object{{"kind", "reports"}, {"revision", Current}}));
  const auto Source =
      Sources.getArray("items")->front().getAsObject()->getString("id")->str();
  EXPECT_EQ(take(neverd_lineage_report_json(Handle, Source.c_str(), nullptr,
                                            nullptr)),
            Report);
  EXPECT_EQ(neverd_lineage_load_json(Handle, After.c_str(), nullptr, nullptr,
                                     nullptr),
            1);
  EXPECT_EQ(saved(), After);
}
