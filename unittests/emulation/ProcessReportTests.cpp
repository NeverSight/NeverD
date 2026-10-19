//===- ProcessReportTests.cpp - Lossless bounded process wire contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinCreatedAdvisoryLockTestData.h"
#include "DarwinCreatedAttributeTestData.h"
#include "DarwinCreatedSynchronizationTestData.h"
#include "DarwinFileTestData.h"
#include "DarwinFullSynchronizationTestData.h"
#include "DarwinSymbolicLinkFileSystemTestData.h"
#include "DarwinSymbolicOwnershipTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTimeTestData.h"
#include "DarwinTimestampMutationTestData.h"
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Base64.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
TEST(ProcessReport, IoctlTraceHasStrictBoundsAndExplicitCaptureStatus) {
  const char *Good = R"({"linux_files":{"files":[],"ioctl_trace":{
    "event_limit":8,"byte_limit":256,"requests":[{"request":"4294967295",
    "memory":[{"offset":1,"size":8,"pointer_offset":0}]}]}}})";
  auto O = processOptionsFromJSON(Good);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxFiles && O->LinuxFiles->IoctlTrace);
  const auto &T = *O->LinuxFiles->IoctlTrace;
  EXPECT_EQ(T.EventLimit, 8u);
  EXPECT_EQ(T.ByteLimit, 256u);
  ASSERT_EQ(T.Requests.at(UINT32_MAX).size(), 1u);
  EXPECT_EQ(T.Requests.at(UINT32_MAX)[0].PointerOffset, 0u);
  for (
      const char *Bad :
      {"null", "true", "{}",
       R"({"event_limit":0,"byte_limit":1,"requests":[]})",
       R"({"event_limit":4097,"byte_limit":1,"requests":[]})",
       R"({"event_limit":1,"byte_limit":1048577,"requests":[]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[],"extra":0})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":4294967296,"memory":[]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[]},{"request":0,"memory":[]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[{"offset":0,"size":0}]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[{"offset":0,"size":4097}]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[{"offset":"18446744073709551615","size":2}]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[{"offset":0,"size":1,"pointer_offset":null}]}]})",
       R"({"event_limit":1,"byte_limit":1,"requests":[{"request":0,"memory":[{"offset":0,"size":1,"pointer_offset":"18446744073709551615"}]}]})"}) {
    auto R = processOptionsFromJSON(
        std::string(R"({"linux_files":{"files":[],"ioctl_trace":)") + Bad +
        "}}");
    EXPECT_FALSE(bool(R)) << Bad;
    llvm::consumeError(R.takeError());
  }
  ProcessResult R{ProcessProfile::LinuxELF64,
                  GuestArchitecture::X64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  auto Off = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
  EXPECT_FALSE(Off.getAsObject()->get("ioctl_trace"));
  R.IoctlTraceEnabled = true;
  R.IoctlTraceTruncated = true;
  ProcessIoctlEvent E;
  E.PC = 0x1234;
  E.Request = UINT32_MAX;
  E.Result = uint64_t(-13);
  E.ReturnAddress = 0x1238;
  E.Memory.push_back({0x2000, 2, {0, 0xff}, {}, "complete", "byte_limit"});
  R.IoctlTrace.push_back(E);
  auto V = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
  const auto *J =
      V.getAsObject()->getArray("ioctl_trace")->front().getAsObject();
  EXPECT_EQ(J->getString("result"), "fffffffffffffff3");
  EXPECT_EQ(J->getString("return_address"), "1238");
  EXPECT_EQ(
      J->getArray("memory")->front().getAsObject()->getString("before_hex"),
      "00ff");
  EXPECT_EQ(V.getAsObject()->getBoolean("ioctl_trace_truncated"), true);
}
namespace {
namespace field = process_report;
TEST(ProcessReport, DarwinNamedCallsRetainProviderArityFullBitsAndNullResults) {
  for (auto Profile : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                       ProcessProfile::IOSSimulatorMachO64}) {
    ProcessResult R{Profile, GuestArchitecture::AArch64,
                    ExecutionBackendKind::Unicorn, "explicit test"};
    const auto Original = processResultJSON(R);
    auto Raw = llvm::json::parse(Original);
    ASSERT_TRUE(bool(Raw));
    ASSERT_TRUE(Raw->getAsObject());
    EXPECT_FALSE(Raw->getAsObject()->get("darwin"));
    NativeCallEvent Call;
    Call.PC = 0x7fffffbf0008ULL;
    Call.Module = "/usr/lib/libSystem.B.dylib";
    Call.Name = "_write";
    Call.ArgumentCount = 3;
    Call.Arguments = {0x1234567800000001ULL, UINT64_MAX, 0x8000000000000001ULL};
    R.NativeCalls.push_back(Call);
    R.NativeCalls.push_back(Call);
    R.NativeCalls[0].Result = UINT64_MAX;
    auto Parsed = llvm::json::parse(processResultJSON(R));
    ASSERT_TRUE(bool(Parsed));
    auto *Root = Parsed->getAsObject();
    ASSERT_NE(Root, nullptr);
    EXPECT_EQ(Root->getInteger("schema_version"), 1);
    auto *Darwin = Root->getObject("darwin");
    ASSERT_NE(Darwin, nullptr);
    auto *Calls = Darwin->getArray("native_calls");
    ASSERT_NE(Calls, nullptr);
    ASSERT_EQ(Calls->size(), 2u);
    auto *First = (*Calls)[0].getAsObject();
    ASSERT_NE(First, nullptr);
    EXPECT_EQ(First->getString("module"), "/usr/lib/libSystem.B.dylib");
    EXPECT_EQ(First->getString("name"), "_write");
    EXPECT_EQ(First->getString("pc"), "7fffffbf0008");
    EXPECT_EQ(First->getString("result"), "ffffffffffffffff");
    auto *Arguments = First->getArray("arguments");
    ASSERT_NE(Arguments, nullptr);
    ASSERT_EQ(Arguments->size(), 3u);
    EXPECT_EQ((*Arguments)[0].getAsString(), "1234567800000001");
    EXPECT_EQ((*Arguments)[1].getAsString(), "ffffffffffffffff");
    EXPECT_EQ((*Arguments)[2].getAsString(), "8000000000000001");
    EXPECT_EQ((*Calls)[1].getAsObject()->get("result")->kind(),
              llvm::json::Value::Null);
    EXPECT_TRUE(Root->getArray("services")->empty());
    R.NativeCalls.clear();
    EXPECT_EQ(processResultJSON(R), Original);
  }
}
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_FAULT_REPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "FaultReportCases.def"
#undef NEVERD_FAULT_REPORT_TEXT

TEST(ProcessReport,
     DarwinFileSystemSnapshotsKeepAllSixteenFieldsAndZeroValues) {
  for (bool Zero : {false, true}) {
    auto S =
        llvm::cantFail(llvm::json::parse(darwin_test::FileSystemSnapshotJSON));
    if (Zero) {
      for (auto &[K, V] : *S.getAsObject()) {
        if (V.getAsString() &&
            (K == "type_name" || K == "mount_point" || K == "mounted_from"))
          V = "";
        else if (K == "filesystem_id")
          V = llvm::json::Array{0, 0};
        else
          V = 0;
      }
    }
    auto F = llvm::json::Object{
        {"files", llvm::json::Array{llvm::json::Object{{"path", "/data"},
                                                       {"bytes_hex", ""}}}},
        {"filesystem_observations",
         llvm::json::Object{{"objects", llvm::json::Array{llvm::json::Object{
                                            {"path", "/data"},
                                            {"snapshot", std::move(S)}}}}}}};
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(F)}}))
            .str());
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    const auto &E = P->DarwinFiles->FileSystemObservations->Objects.at("/data");
    EXPECT_EQ(E.BlockSize, Zero ? 0u : 0x10203040u);
    EXPECT_EQ(E.IOSize, Zero ? 0 : -4096);
    EXPECT_EQ(E.Blocks, Zero ? 0u : 0xfedcba9876543210ULL);
    EXPECT_EQ(E.FreeBlocks, Zero ? 0u : 0x0102030405060708ULL);
    EXPECT_EQ(E.AvailableBlocks, 0u);
    EXPECT_EQ(E.Files, Zero ? 0u : UINT64_MAX);
    EXPECT_EQ(E.FreeFiles, Zero ? 0u : 9u);
    EXPECT_EQ(E.FileSystemID[0], Zero ? 0 : -1);
    EXPECT_EQ(E.FileSystemID[1], Zero ? 0 : INT32_MIN);
    EXPECT_EQ(E.Owner, Zero ? 0u : 0xfffffffeu);
    EXPECT_EQ(E.Type, Zero ? 0u : 26u);
    EXPECT_EQ(E.Flags, Zero ? 0u : 0x01020304u);
    EXPECT_EQ(E.Subtype, Zero ? 0u : 7u);
    EXPECT_EQ(E.ExtendedFlags, Zero ? 0u : 0x87654321u);
    EXPECT_EQ(E.TypeName, Zero ? "" : "virtfs");
    EXPECT_EQ(E.MountPoint, Zero ? "" : "/虚拟");
    EXPECT_EQ(E.MountedFrom, Zero ? "" : "neverd:declared");
    EXPECT_FALSE(P->DarwinFiles->FileSystemObservations->CreatedRegularObjects);
  }
}
TEST(ProcessReport,
     DarwinFileSystemRejectsIncompleteUnknownWidthsNamesAndDuplicatePaths) {
  for (unsigned I = 0; I != 30; ++I) {
    auto S =
        llvm::cantFail(llvm::json::parse(darwin_test::FileSystemSnapshotJSON));
    auto *Record = S.getAsObject();
    auto Provider = llvm::json::Object{
        {"objects", llvm::json::Array{llvm::json::Object{{"path", "/data"},
                                                         {"snapshot", S}}}}};
    if (I < 16) {
      const char *Keys[] = {"block_size",
                            "io_size",
                            "blocks",
                            "free_blocks",
                            "available_blocks",
                            "files",
                            "free_files",
                            "filesystem_id",
                            "owner",
                            "type",
                            "flags",
                            "subtype",
                            "type_name",
                            "mount_point",
                            "mounted_from",
                            "extended_flags"};
      Record->erase(Keys[I]);
    } else
      switch (I) {
      case 16:
        (*Record)["extra"] = 0;
        break;
      case 17:
        (*Record)["io_size"] = "2147483648";
        break;
      case 18:
        (*Record)["blocks"] = "-1";
        break;
      case 19:
        (*Record)["files"] = "18446744073709551616";
        break;
      case 20:
        (*Record)["filesystem_id"] = llvm::json::Array{0};
        break;
      case 21:
        (*Record)["filesystem_id"] = llvm::json::Array{0, "-2147483649"};
        break;
      case 22:
        (*Record)["type_name"] = std::string(16, 'x');
        break;
      case 23:
        (*Record)["mount_point"] = std::string(1024, 'x');
        break;
      case 24:
        (*Record)["mounted_from"] = std::string("a\0b", 3);
        break;
      case 25:
        (*Record)["owner"] = true;
        break;
      case 26:
        Provider["created_regular_objects"] = 1;
        break;
      case 27:
        Provider["created_regular_objects"] = true;
        break;
      case 28:
        Provider["extra"] = false;
        break;
      case 29:
        Provider.getArray("objects")->push_back(
            llvm::json::Object{{"path", "/data"}, {"snapshot", S}});
        break;
      }
    (*(*Provider.getArray("objects"))[0].getAsObject())["snapshot"] =
        std::move(S);
    auto F = llvm::json::Object{
        {"files", llvm::json::Array{llvm::json::Object{{"path", "/data"},
                                                       {"bytes_hex", ""}}}},
        {"filesystem_observations", std::move(Provider)}};
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(F)}}))
            .str());
    EXPECT_FALSE(bool(P)) << I;
    if (!P)
      llvm::consumeError(P.takeError());
  }
}
TEST(ProcessReport,
     DarwinFileSystemEmptyDeclarationsCloseBothImmutableDomains) {
  for (const char *Domain :
       {"static-owner-queries", "static-ordinary-queries"}) {
    auto P = processOptionsFromJSON(
        llvm::formatv("{{\"darwin_files\":{{\"authorization\":\"{0}\","
                      "\"filesystem_observations\":{{\"objects\":[]}}}}}}",
                      Domain)
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      llvm::consumeError(P.takeError());
  }
}

TEST(ProcessReport,
     DarwinMountEnumerationPreservesOrderDuplicatesAndOptionalRefresh) {
  auto S =
      llvm::cantFail(llvm::json::parse(darwin_test::FileSystemSnapshotJSON));
  auto Z = S;
  for (auto &[K, V] : *Z.getAsObject()) {
    if (K == "type_name" || K == "mount_point" || K == "mounted_from")
      V = "";
    else if (K == "filesystem_id")
      V = llvm::json::Array{0, 0};
    else
      V = 0;
  }
  auto Parse = [&](llvm::json::Array Mounts) {
    return processOptionsFromJSON(
        llvm::formatv(
            "{0}",
            llvm::json::Value(llvm::json::Object{
                {"darwin_files",
                 llvm::json::Object{
                     {"files", llvm::json::Array{}},
                     {"filesystem_enumeration",
                      llvm::json::Object{{"mounts", std::move(Mounts)}}}}}}))
            .str());
  };
  auto P = Parse(
      llvm::json::Array{llvm::json::Object{{"cached", S}, {"refreshed", Z}},
                        llvm::json::Object{{"cached", Z}, {"refreshed", S}},
                        llvm::json::Object{{"cached", S}}});
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  const auto &M = P->DarwinFiles->FileSystemEnumeration->Mounts;
  ASSERT_EQ(M.size(), 3u);
  ASSERT_TRUE(M[0].Refreshed);
  ASSERT_TRUE(M[1].Refreshed);
  EXPECT_FALSE(M[2].Refreshed);
  for (const auto *E : {&M[0].Cached, &*M[1].Refreshed, &M[2].Cached}) {
    EXPECT_EQ(E->BlockSize, 0x10203040u);
    EXPECT_EQ(E->IOSize, -4096);
    EXPECT_EQ(E->Blocks, 0xfedcba9876543210ULL);
    EXPECT_EQ(E->FreeBlocks, 0x0102030405060708ULL);
    EXPECT_EQ(E->AvailableBlocks, 0u);
    EXPECT_EQ(E->Files, UINT64_MAX);
    EXPECT_EQ(E->FreeFiles, 9u);
    EXPECT_EQ(E->FileSystemID, (std::array<int32_t, 2>{-1, INT32_MIN}));
    EXPECT_EQ(E->Owner, 0xfffffffeu);
    EXPECT_EQ(E->Type, 26u);
    EXPECT_EQ(E->Flags, 0x01020304u);
    EXPECT_EQ(E->Subtype, 7u);
    EXPECT_EQ(E->TypeName, "virtfs");
    EXPECT_EQ(E->MountPoint, "/虚拟");
    EXPECT_EQ(E->MountedFrom, "neverd:declared");
    EXPECT_EQ(E->ExtendedFlags, 0x87654321u);
  }
  for (const auto *E : {&*M[0].Refreshed, &M[1].Cached}) {
    EXPECT_EQ(E->BlockSize, 0u);
    EXPECT_EQ(E->Files, 0u);
    EXPECT_EQ(E->TypeName, "");
    EXPECT_EQ(E->FileSystemID, (std::array<int32_t, 2>{0, 0}));
  }
  EXPECT_FALSE(P->DarwinFiles->FileSystemObservations);
  auto Empty = Parse(llvm::json::Array{});
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->DarwinFiles->FileSystemEnumeration->Mounts.empty());
}
TEST(ProcessReport,
     DarwinMountEnumerationRejectsMalformedTablesAndBothRecords) {
  const auto S =
      llvm::cantFail(llvm::json::parse(darwin_test::FileSystemSnapshotJSON));
  auto Reject = [&](llvm::json::Value Enumeration) {
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"darwin_files",
                           llvm::json::Object{{"files", llvm::json::Array{}},
                                              {"filesystem_enumeration",
                                               std::move(Enumeration)}}}}))
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      EXPECT_NE(llvm::toString(P.takeError()),
                "invalid darwin_files option: darwin_files");
  };
  for (const char *Bad : {"null", "[]", "{}", R"({"mounts":null})",
                          R"({"mounts":{}})", R"({"mounts":[],"extra":0})",
                          R"({"mounts":[null]})", R"({"mounts":[{}]})"})
    Reject(llvm::cantFail(llvm::json::parse(Bad)));
  for (auto Entry : {llvm::json::Object{{"refreshed", S}},
                     llvm::json::Object{{"cached", S}, {"extra", 0}},
                     llvm::json::Object{{"cached", S}, {"refreshed", nullptr}}})
    Reject(llvm::json::Object{{"mounts", llvm::json::Array{std::move(Entry)}}});
  const char *Keys[] = {"block_size",
                        "io_size",
                        "blocks",
                        "free_blocks",
                        "available_blocks",
                        "files",
                        "free_files",
                        "filesystem_id",
                        "owner",
                        "type",
                        "flags",
                        "subtype",
                        "type_name",
                        "mount_point",
                        "mounted_from",
                        "extended_flags"};
  for (const char *Which : {"cached", "refreshed"})
    for (unsigned I = 0; I != 26; ++I) {
      SCOPED_TRACE(llvm::formatv("{0}/{1}", Which, I).str());
      auto Record = S;
      auto &O = *Record.getAsObject();
      if (I < 16)
        O.erase(Keys[I]);
      else
        switch (I) {
        case 16:
          O["extra"] = 0;
          break;
        case 17:
          O["block_size"] = "4294967296";
          break;
        case 18:
          O["io_size"] = "-2147483649";
          break;
        case 19:
          O["blocks"] = "-1";
          break;
        case 20:
          O["files"] = "18446744073709551616";
          break;
        case 21:
          O["filesystem_id"] = llvm::json::Array{0};
          break;
        case 22:
          O["filesystem_id"] = llvm::json::Array{0, "2147483648"};
          break;
        case 23:
          O["type_name"] = std::string(16, 'x');
          break;
        case 24:
          O["mounted_from"] = std::string("a\0b", 3);
          break;
        case 25:
          O["owner"] = true;
          break;
        }
      auto Entry = llvm::json::Object{{"cached", S}, {"refreshed", S}};
      Entry[Which] = std::move(Record);
      Reject(
          llvm::json::Object{{"mounts", llvm::json::Array{std::move(Entry)}}});
    }
  llvm::json::Array Excess;
  for (unsigned I = 0; I != 257; ++I)
    Excess.push_back(llvm::json::Object{{"cached", S}});
  Reject(llvm::json::Object{{"mounts", std::move(Excess)}});
}
TEST(ProcessReport,
     DarwinMountEnumerationSharesCatalogueAndClosesStaticQueries) {
  for (const char *Domain :
       {"static-owner-queries", "static-ordinary-queries"}) {
    auto P = processOptionsFromJSON(
        llvm::formatv(
            "{{\"darwin_files\":{{\"files\":[],\"authorization\":\"{0}\","
            "\"filesystem_enumeration\":{{\"mounts\":[]}}}}}}",
            Domain)
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      EXPECT_NE(llvm::toString(P.takeError()),
                "invalid darwin_files option: darwin_files");
  }
  const auto S =
      llvm::cantFail(llvm::json::parse(darwin_test::FileSystemSnapshotJSON));
  llvm::json::Array Mounts;
  for (unsigned I = 0; I != 100; ++I)
    Mounts.push_back(llvm::json::Object{{"cached", S}});
  auto F =
      llvm::json::Object{{"files", llvm::json::Array{}},
                         {"filesystem_enumeration",
                          llvm::json::Object{{"mounts", std::move(Mounts)}}}};
  // Exercise the shared 256-entry catalogue without exceeding the separate
  // 64KiB public JSON envelope with 256 complete sixteen-field records.
  for (unsigned I = 0; I != 156; ++I)
    F.getArray("files")->push_back(llvm::json::Object{
        {"path", "/f" + std::to_string(I)}, {"bytes_hex", ""}});
  for (bool File : {false, true}) {
    if (File)
      F.getArray("files")->push_back(
          llvm::json::Object{{"path", "/extra"}, {"bytes_hex", ""}});
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", llvm::json::Object(F)}}))
            .str());
    EXPECT_EQ(bool(P), !File);
    if (!P)
      EXPECT_NE(llvm::toString(P.takeError()),
                "invalid darwin_files option: darwin_files");
  }
}

TEST(ProcessReport, DarwinTimestampDeclarationUsesStrictSharedTimedObjects) {
  for (bool Text : {false, true}) {
    auto Files =
        llvm::cantFail(llvm::json::parse(darwin_test::TimestampFilesJSON));
    if (Text)
      (*(*Files.getAsObject()
              ->getObject("timestamp_mutations")
              ->getArray("objects"))[0]
            .getAsObject())["time"] =
          llvm::json::Object{{"seconds", "111"}, {"nanoseconds", "112"}};
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
    ASSERT_TRUE(P->DarwinFiles->TimestampMutations);
    EXPECT_EQ(P->DarwinFiles->TimestampMutations->Objects.at("/data").Seconds,
              111);
    EXPECT_EQ(
        P->DarwinFiles->TimestampMutations->Objects.at("/data").Nanoseconds,
        112);
    EXPECT_FALSE(P->DarwinFiles->TimestampMutations->CreatedRegularObjects);
    EXPECT_FALSE(P->DarwinFiles->ModeMutations);
    EXPECT_FALSE(P->DarwinFiles->OwnershipMutations);
  }
}
TEST(ProcessReport, DarwinTimestampRejectsMalformedUnknownSecurityAndClosure) {
  for (
      const char *Bad :
      {R"({})", R"({"objects":null})", R"({"objects":["/data"]})",
       R"({"objects":[{"path":"/data"}]})",
       R"({"objects":[{"path":"/data","time":{"seconds":1,"nanoseconds":-1}}]})",
       R"({"objects":[{"path":"relative","time":{"seconds":1,"nanoseconds":2}}]})",
       R"({"objects":[],"created_regular_objects":1})",
       R"({"objects":[],"created_regular_objects":null})",
       R"({"objects":[],"created_regular_objects":"true"})",
       R"({"objects":[],"created_regular_objects":true})",
       R"({"objects":[],"extra":false})"}) {
    auto Files =
        llvm::cantFail(llvm::json::parse(darwin_test::TimestampFilesJSON));
    (*Files.getAsObject())["timestamp_mutations"] =
        llvm::cantFail(llvm::json::parse(Bad));
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_FALSE(bool(P)) << Bad;
    EXPECT_FALSE(llvm::toString(P.takeError()).empty());
  }
  for (unsigned V = 0; V != 5; ++V) {
    auto Files =
        llvm::cantFail(llvm::json::parse(darwin_test::TimestampFilesJSON));
    auto &F = *Files.getAsObject();
    if (V < 2) {
      F["authorization"] =
          V ? "static-owner-queries" : "static-ordinary-queries";
      F["timestamp_mutations"] =
          llvm::json::Object{{"objects", llvm::json::Array{}}};
    }
    if (V == 2) {
      auto &M = *(*F.getArray("files"))[0].getAsObject()->getObject("metadata");
      M["flags"] = 1;
    }
    if (V == 3) {
      auto Copy = (*F.getObject("timestamp_mutations")->getArray("objects"))[0];
      F.getObject("timestamp_mutations")
          ->getArray("objects")
          ->push_back(std::move(Copy));
    }
    if (V == 4) {
      llvm::json::Array Many;
      for (unsigned I = 0; I != 257; ++I)
        Many.push_back(llvm::json::Object{
            {"path", "/data"},
            {"time", llvm::json::Object{{"seconds", 1}, {"nanoseconds", 2}}}});
      (*F.getObject("timestamp_mutations"))["objects"] = std::move(Many);
    }
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_FALSE(bool(P));
    EXPECT_FALSE(llvm::toString(P.takeError()).empty());
  }
}

TEST(ProcessReport, DarwinFullBarrierDeclarationsRemainIndependentAndStrict) {
  for (const char *Purpose :
       {"full_synchronization", "barrier_synchronization"})
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedSynchronizationFilesJSON));
      Files.getAsObject()->erase("synchronization");
      Files.getAsObject()->erase("data_synchronization");
      llvm::json::Object Provider{{"objects", llvm::json::Array{}}};
      if (Variant)
        Provider["created_regular_objects"] = Variant == 2;
      (*Files.getAsObject())[Purpose] = std::move(Provider);
      if (Variant != 2)
        Files.getAsObject()->erase("creation_policy");
      auto Parsed = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto &O = *Parsed->DarwinFiles;
      EXPECT_EQ(bool(O.FullSynchronization),
                llvm::StringRef(Purpose) == "full_synchronization");
      EXPECT_EQ(bool(O.BarrierSynchronization),
                llvm::StringRef(Purpose) == "barrier_synchronization");
      EXPECT_EQ(O.FullSynchronization
                    ? O.FullSynchronization->CreatedRegularObjects
                    : O.BarrierSynchronization->CreatedRegularObjects,
                Variant == 2);
      EXPECT_FALSE(O.Synchronization);
      EXPECT_FALSE(O.DataSynchronization);
    }
}
TEST(ProcessReport, DarwinFullBarrierRejectMalformedObjectsFlagsAndPolicy) {
  for (const char *Purpose :
       {"full_synchronization", "barrier_synchronization"}) {
    for (const char *Bad :
         {R"({})", R"({"objects":null})", R"({"objects":"/data"})",
          R"({"objects":[3]})", R"({"objects":[{"path":"/data"}]})",
          R"({"objects":["/data","/data"]})", R"({"objects":["relative"]})",
          R"({"objects":["/data/../data"]})", R"({"objects":["/missing"]})",
          R"({"objects":["/data\u0000tail"]})",
          R"({"objects":[],"created_regular_objects":1})",
          R"({"objects":[],"created_regular_objects":null})",
          R"({"objects":[],"created_regular_objects":"true"})",
          R"({"objects":[],"created_regular_objects":[]})",
          R"({"objects":[],"created_regular_objects":{}})",
          R"({"objects":[],"created_regular_objects":true,"extra":false})"}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::FullSynchronizationFilesJSON));
      (*Files.getAsObject())[Purpose] = llvm::cantFail(llvm::json::parse(Bad));
      auto P = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_FALSE(bool(P)) << Bad;
      EXPECT_FALSE(llvm::toString(P.takeError()).empty());
    }
    for (unsigned Variant = 0; Variant != 5; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::FullSynchronizationFilesJSON));
      auto &F = *Files.getAsObject();
      if (Variant == 0)
        (*F.getObject(Purpose))["created_regular_objects"] = true;
      if (Variant == 1)
        F["authorization"] = "static-owner-queries";
      if (Variant == 2)
        F["authorization"] = "static-ordinary-queries";
      if (Variant == 3) {
        llvm::json::Array TooMany;
        for (unsigned I = 0; I != 257; ++I)
          TooMany.push_back("/data");
        (*F.getObject(Purpose))["objects"] = std::move(TooMany);
      }
      if (Variant == 4) {
        auto &M =
            *(*F.getArray("files"))[0].getAsObject()->getObject("metadata");
        M["flags"] = 1;
      }
      auto P = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_FALSE(bool(P));
      EXPECT_FALSE(llvm::toString(P.takeError()).empty());
    }
  }
}

TEST(ProcessReport, DarwinDirectoryFlagIsStrictAndExclusiveToObjectProviders) {
  for (const char *Purpose :
       {"mode_mutations", "ownership_mutations", "timestamp_mutations",
        "filesystem_observations", "synchronization"})
    for (bool Enabled : {false, true}) {
      auto Files =
          llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
      (*Files.getAsObject())[Purpose] =
          llvm::json::Object{{"objects", llvm::json::Array{}},
                             {"created_directory_objects", Enabled}};
      auto Parsed = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      const bool Accepted =
          (llvm::StringRef(Purpose) == "mode_mutations" ||
           llvm::StringRef(Purpose) == "timestamp_mutations" ||
           llvm::StringRef(Purpose) == "ownership_mutations" ||
           llvm::StringRef(Purpose) == "filesystem_observations") &&
          !Enabled;
      EXPECT_EQ(bool(Parsed), Accepted);
      if (Parsed && llvm::StringRef(Purpose) == "timestamp_mutations") {
        EXPECT_FALSE(
            Parsed->DarwinFiles->TimestampMutations->CreatedDirectoryObjects);
        EXPECT_FALSE(
            Parsed->DarwinFiles->TimestampMutations->CreatedRegularObjects);
      } else if (Parsed && llvm::StringRef(Purpose) == "ownership_mutations") {
        EXPECT_FALSE(
            Parsed->DarwinFiles->OwnershipMutations->CreatedDirectoryObjects);
        EXPECT_FALSE(
            Parsed->DarwinFiles->OwnershipMutations->CreatedRegularObjects);
      } else if (Parsed &&
                 llvm::StringRef(Purpose) == "filesystem_observations") {
        EXPECT_FALSE(Parsed->DarwinFiles->FileSystemObservations
                         ->CreatedDirectoryObjects);
        EXPECT_FALSE(
            Parsed->DarwinFiles->FileSystemObservations->CreatedRegularObjects);
      } else if (Parsed) {
        EXPECT_FALSE(
            Parsed->DarwinFiles->ModeMutations->CreatedDirectoryObjects);
        EXPECT_FALSE(Parsed->DarwinFiles->ModeMutations->CreatedRegularObjects);
      } else
        llvm::consumeError(Parsed.takeError());
    }
  for (const char *Bad :
       {R"({"objects":[],"created_directory_objects":null})",
        R"({"objects":[],"created_directory_objects":1})",
        R"({"objects":[],"created_directory_objects":"false"})",
        R"({"created_directory_objects":false})",
        R"({"objects":[],"created_directory_objects":false,"extra":false})"}) {
    for (const char *Purpose :
         {"mode_mutations", "timestamp_mutations", "ownership_mutations",
          "filesystem_observations"}) {
      auto Files =
          llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
      (*Files.getAsObject())[Purpose] = llvm::cantFail(llvm::json::parse(Bad));
      auto Parsed = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      EXPECT_FALSE(bool(Parsed));
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
    }
  }
}

TEST(ProcessReport, DarwinInitialDirectoryModeNeedsNoCreationOrNamespaceGrant) {
  auto Files = llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
  (*Files.getAsObject())["mode_mutations"] = llvm::json::Object{
      {"objects", llvm::json::Array{llvm::json::Object{
                      {"path", "/"},
                      {"time", llvm::json::Object{{"seconds", 71},
                                                  {"nanoseconds", 72}}}}}},
      {"created_directory_objects", false},
      {"created_regular_objects", false}};
  auto Parsed = processOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                               {"darwin_files", std::move(Files)}}))
          .str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->DarwinFiles->ModeMutations->Objects.at("/").Seconds, 71);
  EXPECT_FALSE(Parsed->DarwinFiles->CreationPolicy);
  EXPECT_TRUE(Parsed->DarwinFiles->MutableDirectories.empty());
}
TEST(ProcessReport,
     DarwinInitialDirectoryOwnershipNeedsNoCreationOrNamespaceGrant) {
  auto Files = llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
  (*Files.getAsObject())["ownership_mutations"] = llvm::json::Object{
      {"objects", llvm::json::Array{llvm::json::Object{
                      {"path", "/"},
                      {"time", llvm::json::Object{{"seconds", 71},
                                                  {"nanoseconds", 72}}}}}},
      {"created_directory_objects", false},
      {"created_regular_objects", false}};
  auto Parsed = processOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                               {"darwin_files", std::move(Files)}}))
          .str());
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->DarwinFiles->OwnershipMutations->Objects.at("/").Seconds,
            71);
  EXPECT_FALSE(Parsed->DarwinFiles->CreationPolicy);
  EXPECT_TRUE(Parsed->DarwinFiles->MutableDirectories.empty());
}

TEST(ProcessReport, DarwinCreatedDirectoryModeRequiresItsOwnCompletePolicy) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      auto &F = *Files.getAsObject();
      F.erase("ownership_mutations");
      F["mode_mutations"] =
          llvm::json::Object{{"objects", llvm::json::Array{}},
                             {"created_regular_objects", Regular},
                             {"created_directory_objects", Directory}};
      const auto Parse = [&] {
        return processOptionsFromJSON(
            llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                     {"darwin_files", Files}}))
                .str());
      };
      auto Parsed = Parse();
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->DarwinFiles->ModeMutations->CreatedRegularObjects,
                Regular);
      EXPECT_EQ(Parsed->DarwinFiles->ModeMutations->CreatedDirectoryObjects,
                Directory);
      EXPECT_FALSE(Parsed->DarwinFiles->OwnershipMutations);
      F.getObject("creation_policy")->erase("namespace_policy");
      Parsed = Parse();
      EXPECT_EQ(bool(Parsed), !Directory);
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
    }
}
TEST(ProcessReport,
     DarwinCreatedDirectoryTimestampRequiresItsOwnCompletePolicy) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      auto &F = *Files.getAsObject();
      F.erase("ownership_mutations");
      F.erase("mode_mutations");
      F["timestamp_mutations"] =
          llvm::json::Object{{"objects", llvm::json::Array{}},
                             {"created_regular_objects", Regular},
                             {"created_directory_objects", Directory}};
      const auto Parse = [&] {
        return processOptionsFromJSON(
            llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                     {"darwin_files", Files}}))
                .str());
      };
      auto Parsed = Parse();
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->DarwinFiles->TimestampMutations->CreatedRegularObjects,
                Regular);
      EXPECT_EQ(
          Parsed->DarwinFiles->TimestampMutations->CreatedDirectoryObjects,
          Directory);
      EXPECT_FALSE(Parsed->DarwinFiles->OwnershipMutations);
      F.getObject("creation_policy")->erase("namespace_policy");
      Parsed = Parse();
      EXPECT_EQ(bool(Parsed), !Directory);
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
    }
}
TEST(ProcessReport,
     DarwinCreatedDirectoryOwnershipRequiresItsOwnCompletePolicy) {
  for (bool Regular : {false, true})
    for (bool Directory : {false, true}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      auto &F = *Files.getAsObject();
      F.erase("ownership_mutations");
      F.erase("mode_mutations");
      F["ownership_mutations"] =
          llvm::json::Object{{"objects", llvm::json::Array{}},
                             {"created_regular_objects", Regular},
                             {"created_directory_objects", Directory}};
      const auto Parse = [&] {
        return processOptionsFromJSON(
            llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                     {"darwin_files", Files}}))
                .str());
      };
      auto Parsed = Parse();
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->DarwinFiles->OwnershipMutations->CreatedRegularObjects,
                Regular);
      EXPECT_EQ(
          Parsed->DarwinFiles->OwnershipMutations->CreatedDirectoryObjects,
          Directory);
      EXPECT_FALSE(Parsed->DarwinFiles->ModeMutations);
      F.getObject("creation_policy")->erase("namespace_policy");
      Parsed = Parse();
      EXPECT_EQ(bool(Parsed), !Directory);
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
    }
}

TEST(ProcessReport, DarwinCreatedDirectoryFileSystemNeedsItsOwnCompletePolicy) {
  for (bool Directory : {false, true})
    for (bool Regular : {false, true}) {
      auto Files =
          darwin_test::createdDirectoryFileSystemFilesJSON(Directory, Regular);
      auto &F = *Files.getAsObject();
      const auto Parse = [&] {
        return processOptionsFromJSON(
            llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                     {"darwin_files", Files}}))
                .str());
      };
      auto Parsed = Parse();
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(
          Parsed->DarwinFiles->FileSystemObservations->CreatedDirectoryObjects,
          Directory);
      EXPECT_EQ(
          Parsed->DarwinFiles->FileSystemObservations->CreatedRegularObjects,
          Regular);
      EXPECT_FALSE(Parsed->DarwinFiles->ModeMutations);
      EXPECT_FALSE(Parsed->DarwinFiles->OwnershipMutations);
      EXPECT_FALSE(Parsed->DarwinFiles->TimestampMutations);
      EXPECT_EQ(
          Parsed->DarwinFiles->FileSystemObservations->Objects.at("/").Blocks,
          0xfedcba9876543210ULL);
      F.getObject("creation_policy")->erase("namespace_policy");
      Parsed = Parse();
      EXPECT_EQ(bool(Parsed), !Directory);
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
      F.erase("creation_policy");
      Parsed = Parse();
      EXPECT_EQ(bool(Parsed), !Directory && !Regular);
      if (!Parsed)
        llvm::consumeError(Parsed.takeError());
    }
}

TEST(ProcessReport, DarwinSymbolicCreationFlagsAreStrictAndPurposeSpecific) {
  for (const char *Purpose :
       {"filesystem_observations", "mode_mutations", "ownership_mutations",
        "timestamp_mutations", "synchronization"})
    for (const char *Flag : {"false", "true", "null", "1", "\"false\""}) {
      auto V = llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
      (*V.getAsObject())[Purpose] = llvm::cantFail(llvm::json::parse(
          std::string("{\"objects\":[],\"created_symbolic_link_objects\":") +
          Flag + "}"));
      auto P = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(V)}}))
              .str());
      const bool Filesystem =
          llvm::StringRef(Purpose) == "filesystem_observations";
      const bool Ownership = llvm::StringRef(Purpose) == "ownership_mutations";
      const bool Good =
          (Filesystem || Ownership) && llvm::StringRef(Flag) == "false";
      EXPECT_EQ(bool(P), Good);
      if (P && Filesystem)
        EXPECT_FALSE(
            P->DarwinFiles->FileSystemObservations->CreatedSymbolicLinkObjects);
      else if (P && Ownership)
        EXPECT_FALSE(
            P->DarwinFiles->OwnershipMutations->CreatedSymbolicLinkObjects);
      else
        llvm::consumeError(P.takeError());
    }
  for (
      const char *Bad :
      {R"({"created_symbolic_link_objects":false})",
       R"({"objects":[],"created_symbolic_link_objects":false,"extra":false})"}) {
    auto V = llvm::cantFail(llvm::json::parse(darwin_test::ModeFilesJSON));
    (*V.getAsObject())["filesystem_observations"] =
        llvm::cantFail(llvm::json::parse(Bad));
    auto P = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(V)}}))
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      llvm::consumeError(P.takeError());
  }
}
TEST(ProcessReport,
     DarwinSymbolicOwnershipCreationRequiresIndependentCompletePolicy) {
  for (bool Symbolic : {false, true})
    for (bool Directory : {false, true})
      for (bool Regular : {false, true}) {
        auto V = darwin_test::symbolicOwnershipFilesJSON(Symbolic);
        auto &F = *V.getAsObject(), &Env = *F.getObject("ownership_mutations");
        Env["created_directory_objects"] = Directory;
        Env["created_regular_objects"] = Regular;
        const auto Parse = [&] {
          return processOptionsFromJSON(
              llvm::formatv("{0}", llvm::json::Value(
                                       llvm::json::Object{{"darwin_files", V}}))
                  .str());
        };
        auto P = Parse();
        ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
        const auto &O = *P->DarwinFiles->OwnershipMutations;
        EXPECT_EQ(O.CreatedSymbolicLinkObjects, Symbolic);
        EXPECT_EQ(O.CreatedDirectoryObjects, Directory);
        EXPECT_EQ(O.CreatedRegularObjects, Regular);
        EXPECT_EQ(O.Objects.at("/dir/link").Seconds, 181);
        EXPECT_EQ(P->DarwinFiles->SymbolicLinks.at("/dir/link"),
                  (std::vector<uint8_t>{'.', '.', '/', 'd', 'a', 't', 'a'}));
        EXPECT_FALSE(P->DarwinFiles->ModeMutations);
        EXPECT_FALSE(P->DarwinFiles->TimestampMutations);
        EXPECT_FALSE(P->DarwinFiles->FileSystemObservations);
        F.getObject("creation_policy")->erase("namespace_policy");
        P = Parse();
        EXPECT_EQ(bool(P), !Symbolic && !Directory);
        if (!P)
          llvm::consumeError(P.takeError());
        F.erase("creation_policy");
        P = Parse();
        EXPECT_EQ(bool(P), !Symbolic && !Directory && !Regular);
        if (!P)
          llvm::consumeError(P.takeError());
      }
}
TEST(ProcessReport,
     DarwinInitialSymbolicOwnershipNeedsExactCompleteGrantAndLosslessTime) {
  auto V = darwin_test::symbolicOwnershipFilesJSON();
  auto &F = *V.getAsObject();
  F.erase("creation_policy");
  const auto Parse = [&] {
    return processOptionsFromJSON(
        llvm::formatv(
            "{0}", llvm::json::Value(llvm::json::Object{{"darwin_files", V}}))
            .str());
  };
  auto &Objects = *F.getObject("ownership_mutations")->getArray("objects");
  auto &Time = *Objects[1].getAsObject()->getObject("time");
  Time["seconds"] = "-9223372036854775808";
  Time["nanoseconds"] = 999999999;
  auto P = Parse();
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->DarwinFiles->OwnershipMutations->Objects.at("/dir/link").Seconds,
            INT64_MIN);
  EXPECT_EQ(
      P->DarwinFiles->OwnershipMutations->Objects.at("/dir/link").Nanoseconds,
      999999999);
  EXPECT_FALSE(P->DarwinFiles->OwnershipMutations->CreatedSymbolicLinkObjects);
  auto Saved =
      *(*F.getArray("symbolic_links"))[0].getAsObject()->get("metadata");
  for (const char *Field :
       {"metadata", "flags", "mode", "link_count", "inode"}) {
    auto &Link = *(*F.getArray("symbolic_links"))[0].getAsObject();
    Link["metadata"] = Saved;
    if (llvm::StringRef(Field) == "metadata")
      Link.erase("metadata");
    else
      (*Link.getObject("metadata"))[Field] =
          llvm::StringRef(Field) == "mode"         ? 0100755
          : llvm::StringRef(Field) == "inode"      ? 7
          : llvm::StringRef(Field) == "link_count" ? 2
                                                   : 1;
    P = Parse();
    EXPECT_FALSE(bool(P)) << Field;
    if (!P)
      llvm::consumeError(P.takeError());
  }
}
TEST(ProcessReport,
     DarwinSymbolicOwnershipRejectsMalformedTimesAndForeignPurposeGrants) {
  for (const char *Purpose : {"mode_mutations", "timestamp_mutations"}) {
    auto V = darwin_test::symbolicOwnershipFilesJSON();
    auto &F = *V.getAsObject();
    F[Purpose] = llvm::json::Object{
        {"objects", llvm::json::Array{llvm::json::Object{
                        {"path", "/dir/link"},
                        {"time", llvm::json::Object{{"seconds", 1},
                                                    {"nanoseconds", 2}}}}}}};
    auto P = processOptionsFromJSON(
        llvm::formatv(
            "{0}", llvm::json::Value(llvm::json::Object{{"darwin_files", V}}))
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      llvm::consumeError(P.takeError());
  }
  for (const char *Bad :
       {R"({"seconds":1,"nanoseconds":-1})",
        R"({"seconds":1,"nanoseconds":1000000000})",
        R"({"seconds":1.5,"nanoseconds":2})",
        R"({"seconds":9007199254740992,"nanoseconds":2})",
        R"({"seconds":-9223372036854775808,"nanoseconds":2})",
        R"({"seconds":"-9223372036854775809","nanoseconds":2})",
        R"({"seconds":true,"nanoseconds":2})",
        R"({"seconds":1,"nanoseconds":2,"extra":0})"}) {
    auto V = darwin_test::symbolicOwnershipFilesJSON();
    auto &F = *V.getAsObject();
    (*(*F.getObject("ownership_mutations")->getArray("objects"))[1]
          .getAsObject())["time"] = llvm::cantFail(llvm::json::parse(Bad));
    auto P = processOptionsFromJSON(
        llvm::formatv(
            "{0}", llvm::json::Value(llvm::json::Object{{"darwin_files", V}}))
            .str());
    EXPECT_FALSE(bool(P));
    if (!P)
      llvm::consumeError(P.takeError());
  }
}

TEST(ProcessReport, DarwinInitialSymbolicFilesystemDoesNotConsultTargetOrStat) {
  auto V = darwin_test::symbolicLinkFileSystemFilesJSON();
  auto &F = *V.getAsObject();
  F.erase("creation_policy");
  auto &Root = *(*F.getArray("directories"))[0].getAsObject();
  Root.erase("mutable");
  Root.erase("mutation_policy");
  auto &Link = *(*F.getArray("symbolic_links"))[0].getAsObject();
  Link.erase("metadata");
  Link.erase("mutable");
  Link.erase("mutation_policy");
  Link["target_hex"] = "ff";
  (*F.getObject("filesystem_observations"))["created_symbolic_link_objects"] =
      false;
  auto P = processOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                               {"darwin_files", std::move(V)}}))
          .str());
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->DarwinFiles->SymbolicLinks.at("/link"),
            (std::vector<uint8_t>{255}));
  EXPECT_FALSE(P->DarwinFiles->Metadata.contains("/link"));
  EXPECT_EQ(P->DarwinFiles->FileSystemObservations->Objects.at("/link").Blocks,
            0xfedcba9876543210ULL);
}
TEST(ProcessReport, DarwinSymbolicFilesystemCreationNeedsItsOwnCompletePolicy) {
  for (bool Symbolic : {false, true})
    for (bool Directory : {false, true})
      for (bool Regular : {false, true}) {
        auto V = darwin_test::symbolicLinkFileSystemFilesJSON(true);
        auto &F = *V.getAsObject(),
             &Env = *F.getObject("filesystem_observations");
        Env["created_symbolic_link_objects"] = Symbolic;
        Env["created_directory_objects"] = Directory;
        Env["created_regular_objects"] = Regular;
        const auto Parse = [&] {
          return processOptionsFromJSON(
              llvm::formatv("{0}", llvm::json::Value(
                                       llvm::json::Object{{"darwin_files", V}}))
                  .str());
        };
        auto P = Parse();
        ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
        EXPECT_EQ(
            P->DarwinFiles->FileSystemObservations->CreatedSymbolicLinkObjects,
            Symbolic);
        EXPECT_EQ(
            P->DarwinFiles->FileSystemObservations->CreatedDirectoryObjects,
            Directory);
        EXPECT_EQ(P->DarwinFiles->FileSystemObservations->CreatedRegularObjects,
                  Regular);
        F.getObject("creation_policy")->erase("namespace_policy");
        P = Parse();
        EXPECT_EQ(bool(P), !Symbolic && !Directory);
        if (!P)
          llvm::consumeError(P.takeError());
        F.erase("creation_policy");
        P = Parse();
        EXPECT_EQ(bool(P), !Symbolic && !Directory && !Regular);
        if (!P)
          llvm::consumeError(P.takeError());
      }
}

TEST(ProcessReport, DarwinCreatedAttributeFlagsAreIndependentAndCompatible) {
  for (const char *Purpose : {"mode_mutations", "ownership_mutations"})
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      Files.getAsObject()->erase("mode_mutations");
      Files.getAsObject()->erase("ownership_mutations");
      llvm::json::Object Provider{
          {"objects", llvm::json::Array{llvm::json::Object{
                          {"path", "/data"},
                          {"time", llvm::json::Object{{"seconds", 71},
                                                      {"nanoseconds", 72}}}}}}};
      if (Variant)
        Provider["created_regular_objects"] = Variant == 2;
      (*Files.getAsObject())[Purpose] = std::move(Provider);
      if (Variant != 2)
        Files.getAsObject()->erase("creation_policy");
      auto Parsed = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto &O = *Parsed->DarwinFiles;
      EXPECT_EQ(bool(O.ModeMutations),
                llvm::StringRef(Purpose) == "mode_mutations");
      EXPECT_EQ(bool(O.OwnershipMutations),
                llvm::StringRef(Purpose) == "ownership_mutations");
      const auto &Map = O.ModeMutations ? O.ModeMutations->Objects
                                        : O.OwnershipMutations->Objects;
      ASSERT_EQ(Map.size(), 1u);
      EXPECT_EQ(Map.at("/data").Seconds, 71);
      EXPECT_EQ(Map.at("/data").Nanoseconds, 72);
      EXPECT_EQ(O.ModeMutations ? O.ModeMutations->CreatedRegularObjects
                                : O.OwnershipMutations->CreatedRegularObjects,
                Variant == 2);
      EXPECT_FALSE(O.Synchronization);
      EXPECT_FALSE(O.DataSynchronization);
      EXPECT_FALSE(O.AdvisoryLocks);
    }
}
TEST(ProcessReport, DarwinCreatedAttributeRejectsMalformedMapsFlagsAndPolicy) {
  for (const char *Purpose : {"mode_mutations", "ownership_mutations"}) {
    for (
        const char *Bad :
        {R"({"objects":[],"created_regular_objects":null})",
         R"({"objects":[],"created_regular_objects":1})",
         R"({"objects":[],"created_regular_objects":"true"})",
         R"({"objects":[],"created_regular_objects":[]})",
         R"({"objects":[],"created_regular_objects":{}})",
         R"({"created_regular_objects":true})",
         R"({"objects":[],"created_regular_objects":true,"extra":false})",
         R"({"objects":["/data"],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data"}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data","time":{"seconds":1,"nanoseconds":2},"extra":0}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data","time":null}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data","time":{"seconds":1,"nanoseconds":2}},{"path":"/data","time":{"seconds":1,"nanoseconds":2}}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/missing","time":{"seconds":1,"nanoseconds":2}}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data\u0000","time":{"seconds":1,"nanoseconds":2}}],"created_regular_objects":true})",
         R"({"objects":[{"path":"data","time":{"seconds":1,"nanoseconds":2}}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data","time":{"seconds":1,"nanoseconds":-1}}],"created_regular_objects":true})",
         R"({"objects":[{"path":"/data","time":{"seconds":1,"nanoseconds":1000000000}}],"created_regular_objects":true})"}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      (*Files.getAsObject())[Purpose] = llvm::cantFail(llvm::json::parse(Bad));
      auto Rejected = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_FALSE(bool(Rejected)) << Purpose << ":" << Bad;
      EXPECT_FALSE(llvm::toString(Rejected.takeError()).empty());
    }
    for (unsigned Variant = 0; Variant != 7; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedAttributeFilesJSON));
      Files.getAsObject()->erase(llvm::StringRef(Purpose) == "mode_mutations"
                                     ? "ownership_mutations"
                                     : "mode_mutations");
      if (!Variant)
        Files.getAsObject()->erase("creation_policy");
      else if (Variant == 1) {
        llvm::json::Array Objects;
        for (unsigned I = 0; I != 257; ++I)
          Objects.push_back(llvm::json::Object{
              {"path", "/a" + std::to_string(I)},
              {"time",
               llvm::json::Object{{"seconds", 1}, {"nanoseconds", 2}}}});
        (*Files.getAsObject()->getObject(Purpose))["objects"] =
            std::move(Objects);
      } else if (Variant < 4) {
        (*Files.getAsObject()->getObject(Purpose))["created_regular_objects"] =
            false;
        (*Files.getAsObject())["authorization"] =
            Variant == 2 ? "static_owner_queries" : "static_ordinary_queries";
      } else {
        auto &Entry = *(*Files.getAsObject()->getArray("files"))[0]
                           .getAsObject()
                           ->getObject("metadata");
        if (Variant == 4)
          Entry["flags"] = 1;
        if (Variant == 5)
          Entry["mode"] = 0104600;
        if (Variant == 6)
          Entry["link_count"] = 2;
        (*Files.getAsObject()->getObject(Purpose))["objects"] =
            llvm::json::Array{llvm::json::Object{
                {"path", "/data"},
                {"time",
                 llvm::json::Object{{"seconds", 71}, {"nanoseconds", 72}}}}};
      }
      auto Rejected = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_FALSE(bool(Rejected));
      const auto Reason = llvm::toString(Rejected.takeError());
      EXPECT_FALSE(Reason.empty());
      if (!Variant)
        EXPECT_EQ(Reason, "invalid Darwin virtual file creation policy");
    }
  }
}

TEST(ProcessReport, DarwinCreatedAdvisoryFlagsAreStrictAndCompatible) {
  for (unsigned Variant = 0; Variant != 3; ++Variant) {
    auto Files = llvm::cantFail(
        llvm::json::parse(darwin_test::CreatedAdvisoryFilesJSON));
    auto &Provider = *Files.getAsObject()->getObject("advisory_locks");
    if (!Variant)
      Provider.erase("created_regular_objects");
    else
      Provider["created_regular_objects"] = Variant == 2;
    if (Variant != 2)
      Files.getAsObject()->erase("creation_policy");
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_TRUE(Parsed->DarwinFiles->AdvisoryLocks);
    EXPECT_EQ(Parsed->DarwinFiles->AdvisoryLocks->CreatedRegularObjects,
              Variant == 2);
    EXPECT_TRUE(Parsed->DarwinFiles->AdvisoryLocks->Objects.empty());
    EXPECT_FALSE(Parsed->DarwinFiles->Synchronization);
    EXPECT_FALSE(Parsed->DarwinFiles->DataSynchronization);
  }
}
TEST(ProcessReport,
     DarwinCreatedAdvisoryRejectsMalformedFlagsObjectsAndPolicy) {
  for (const char *Bad :
       {R"({"objects":[],"created_regular_objects":null})",
        R"({"objects":[],"created_regular_objects":1})",
        R"({"objects":[],"created_regular_objects":"true"})",
        R"({"objects":[],"created_regular_objects":[]})",
        R"({"objects":[],"created_regular_objects":{}})",
        R"({"created_regular_objects":true})",
        R"({"objects":[],"created_regular_objects":true,"extra":false})",
        R"({"objects":["/data","/data"],"created_regular_objects":true})",
        R"({"objects":["/missing"],"created_regular_objects":true})",
        R"({"objects":["/data\u0000"],"created_regular_objects":true})",
        R"({"objects":["data"],"created_regular_objects":true})",
        R"({"objects":["/data/../data"],"created_regular_objects":true})"}) {
    auto Files = llvm::cantFail(
        llvm::json::parse(darwin_test::CreatedAdvisoryFilesJSON));
    (*Files.getAsObject())["advisory_locks"] =
        llvm::cantFail(llvm::json::parse(Bad));
    auto Rejected = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_FALSE(bool(Rejected)) << Bad;
    const auto Reason = llvm::toString(Rejected.takeError());
    EXPECT_FALSE(Reason.empty()) << Bad;
    if (llvm::StringRef(Bad).contains("/missing") ||
        llvm::StringRef(Bad).contains("\\u0000") ||
        llvm::StringRef(Bad).contains("\"data\"") ||
        llvm::StringRef(Bad).contains(".."))
      EXPECT_EQ(Reason, "Darwin advisory lock grants require complete ordinary "
                        "initial regular objects");
    else
      EXPECT_EQ(Reason,
                std::string(field::DarwinFilesOptions) + "advisory_locks");
  }
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    auto Files = llvm::cantFail(
        llvm::json::parse(darwin_test::CreatedAdvisoryFilesJSON));
    if (!Variant)
      Files.getAsObject()->erase("creation_policy");
    else if (Variant == 1) {
      llvm::json::Array Objects;
      for (unsigned I = 0; I != 257; ++I)
        Objects.push_back("/a" + std::to_string(I));
      (*Files.getAsObject()->getObject("advisory_locks"))["objects"] =
          std::move(Objects);
    } else
      (*Files.getAsObject())["authorization"] =
          Variant == 2 ? "static_owner_queries" : "static_ordinary_queries";
    auto Rejected = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_FALSE(bool(Rejected));
    const auto Reason = llvm::toString(Rejected.takeError());
    if (!Variant)
      EXPECT_EQ(Reason, "invalid Darwin virtual file creation policy");
    else if (Variant == 1)
      EXPECT_EQ(Reason,
                std::string(field::DarwinFilesOptions) + "advisory_locks");
    else
      EXPECT_FALSE(Reason.empty());
  }
}

TEST(ProcessReport, DarwinCreatedCompletionFlagsAreIndependentAndCompatible) {
  for (const char *Purpose : {"synchronization", "data_synchronization"})
    for (unsigned Variant = 0; Variant != 3; ++Variant) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedSynchronizationFilesJSON));
      Files.getAsObject()->erase("synchronization");
      Files.getAsObject()->erase("data_synchronization");
      llvm::json::Object Provider{{"objects", llvm::json::Array{}}};
      if (Variant)
        Provider["created_regular_objects"] = Variant == 2;
      (*Files.getAsObject())[Purpose] = std::move(Provider);
      if (Variant != 2)
        Files.getAsObject()->erase("creation_policy");
      auto Parsed = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      const auto &O = *Parsed->DarwinFiles;
      EXPECT_EQ(bool(O.Synchronization),
                llvm::StringRef(Purpose) == "synchronization");
      EXPECT_EQ(bool(O.DataSynchronization),
                llvm::StringRef(Purpose) == "data_synchronization");
      EXPECT_EQ(O.Synchronization
                    ? O.Synchronization->CreatedRegularObjects
                    : O.DataSynchronization->CreatedRegularObjects,
                Variant == 2);
    }
}
TEST(ProcessReport,
     DarwinCreatedCompletionRejectsMalformedFlagsAndMissingPolicy) {
  for (const char *Purpose : {"synchronization", "data_synchronization"}) {
    for (const char *Bad :
         {R"({"objects":[],"created_regular_objects":null})",
          R"({"objects":[],"created_regular_objects":1})",
          R"({"objects":[],"created_regular_objects":"true"})",
          R"({"objects":[],"created_regular_objects":[]})",
          R"({"objects":[],"created_regular_objects":{}})",
          R"({"created_regular_objects":true})",
          R"({"objects":[],"created_regular_objects":true,"extra":false})",
          R"({"objects":["/data","/data"],"created_regular_objects":true})",
          R"({"objects":["/missing"],"created_regular_objects":true})",
          R"({"objects":["/data\u0000"],"created_regular_objects":true})"}) {
      auto Files = llvm::cantFail(
          llvm::json::parse(darwin_test::CreatedSynchronizationFilesJSON));
      (*Files.getAsObject())[Purpose] = llvm::cantFail(llvm::json::parse(Bad));
      auto Rejected = processOptionsFromJSON(
          llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                   {"darwin_files", std::move(Files)}}))
              .str());
      ASSERT_FALSE(bool(Rejected)) << Purpose << ':' << Bad;
      const auto Expected =
          llvm::StringRef(Bad).contains("/missing") ||
                  llvm::StringRef(Bad).contains("\\u0000")
              ? std::string(llvm::StringRef(Purpose) == "synchronization"
                                ? "Darwin file synchronization"
                                : "Darwin data synchronization") +
                    " grants require complete ordinary initial regular objects"
              : std::string(field::DarwinFilesOptions) + Purpose;
      EXPECT_EQ(llvm::toString(Rejected.takeError()), Expected)
          << Purpose << ':' << Bad;
    }
    auto Files = llvm::cantFail(
        llvm::json::parse(darwin_test::CreatedSynchronizationFilesJSON));
    Files.getAsObject()->erase("creation_policy");
    auto Rejected = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_files", std::move(Files)}}))
            .str());
    ASSERT_FALSE(bool(Rejected));
    EXPECT_EQ(llvm::toString(Rejected.takeError()),
              "invalid Darwin virtual file creation policy");
  }
}

TEST(ProcessReport, RetainsOptionalFaultCauseAndFullWidthProcessorCode) {
  ProcessResult Result{ProcessProfile::WindowsPE64,
                       GuestArchitecture::X64,
                       ExecutionBackendKind::KVM,
                       {}};
  Result.LastCPUExit = ExecutionExit{ExecutionExitKind::GuestTrap,
                                     BackendFault{BackendFaultKind::Interrupt},
                                     {}};
  auto Check = [&](std::optional<uint64_t> Code, const char *Expected) {
    for (bool Classified : {false, true}) {
      auto &Fault = *Result.LastCPUExit->Fault;
      Fault.ErrorCode = Code;
      Fault.Cause = Classified
                        ? std::optional(BackendFaultCause::OperandAlignment)
                        : std::nullopt;
      auto JSON = llvm::cantFail(llvm::json::parse(processResultJSON(Result)));
      const auto *Record = JSON.getAsObject()
                               ->getObject(field::CPUExit)
                               ->getObject(field::Fault);
      ASSERT_NE(Record, nullptr);
      if (Classified)
        EXPECT_EQ(Record->getString(field::Cause), AlignmentCause);
      else
        EXPECT_TRUE(Record->get(field::Cause)->getAsNull());
      if (Code)
        EXPECT_EQ(Record->getString(field::ErrorCode), Expected);
      else
        EXPECT_TRUE(Record->get(field::ErrorCode)->getAsNull());
    }
  };
#define NEVERD_FAULT_REPORT_CASE(Name, Code, ProcessHex, DriverHex)            \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    Check(Code, ProcessHex);                                                   \
  }
#include "FaultReportCases.def"
#undef NEVERD_FAULT_REPORT_CASE
}

TEST(ProcessReport, JNIClassLoaderDeclarationsAndRetainedNativeBindings) {
  auto O = processOptionsFromJSON(R"({"android":{"jni":true,
    "jni_class_lookup_failures":{"fixture/Absent":"java/lang/ClassNotFoundException"}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  EXPECT_EQ(O->Android->JNIClassLookupFailures.at("fixture/Absent"),
            "java/lang/ClassNotFoundException");
  for (const char *Invalid : {"true", "[]", "null", R"({"C":7})"}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"android\":{\"jni_class_lookup_failures\":") + Invalid +
        "}}");
    ASSERT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  ProcessResult R{ProcessProfile::AndroidNativeAArch64,
                  GuestArchitecture::AArch64,
                  ExecutionBackendKind::Unicorn,
                  {}};
  R.JNIRegistrations = {{"fixture/Native", "call", "(I)J", UINT64_MAX}};
  auto J = llvm::cantFail(llvm::json::parse(processResultJSON(R)));
  const auto *Bindings =
      J.getAsObject()->getObject("android")->getArray("jni_registrations");
  ASSERT_NE(Bindings, nullptr);
  ASSERT_EQ(Bindings->size(), 1u);
  const auto *B = Bindings->front().getAsObject();
  EXPECT_EQ(B->getString("class"), "fixture/Native");
  EXPECT_EQ(B->getString("name"), "call");
  EXPECT_EQ(B->getString("signature"), "(I)J");
  EXPECT_EQ(B->getString("address"), "ffffffffffffffff");
}

TEST(ProcessReport, DirectoryCreationRequiresCompleteBoundedMetadata) {
  auto M = llvm::cantFail(llvm::json::parse(FileTestMetadataJSON));
  auto &O = *M.getAsObject();
  O["mode"] = 0042775;
  O["inode"] = 91;
  O["uid"] = 1000;
  O["gid"] = 1234;
  O["size"] = 40;
  O["blocks"] = 0;
  O["block_size"] = 4096;
  O["link_count"] = 2;
  auto Parse = [&](const llvm::json::Value &V) {
    return processOptionsFromJSON(
        R"({"linux_files":{"files":[],"directories":["/work"],"directory_creation":)" +
        llvm::formatv("{0}", V).str() + "}}");
  };
  llvm::json::Value Good =
      llvm::json::Object{{"umask", 18},
                         {"limit", 2},
                         {"parents", llvm::json::Array{llvm::json::Object{
                                         {"path", "/work"}, {"metadata", M}}}}};
  auto P = Parse(Good);
  ASSERT_TRUE(bool(P)) << llvm::toString(P.takeError());
  EXPECT_EQ(P->LinuxFiles->DirectoryCreation->Umask, 0022u);
  EXPECT_EQ(P->LinuxFiles->DirectoryCreation->Parents.at("/work").Inode, 91u);
  auto Refused = [&](llvm::json::Value V) {
    auto R = Parse(V);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  };
  for (const char *Key : {"umask", "limit", "parents"}) {
    auto V = Good;
    V.getAsObject()->erase(Key);
    Refused(V);
    V = Good;
    (*V.getAsObject())[Key] = nullptr;
    Refused(V);
  }
  for (auto [Key, Value] : {std::pair{"umask", -1},
                            {"umask", 512},
                            {"limit", 0},
                            {"limit", 256},
                            {"limit", -1}}) {
    auto V = Good;
    (*V.getAsObject())[Key] = Value;
    Refused(V);
  }
  for (const char *Path : {"/missing", "/work/", "relative", "/proc"}) {
    auto V = Good;
    (*(*V.getAsObject()->getArray("parents"))[0].getAsObject())["path"] = Path;
    Refused(V);
  }
  for (auto [Key, Value] : {std::pair{"mode", 0100755},
                            {"mode", 0044755},
                            {"inode", 0},
                            {"blocks", 1},
                            {"block_size", 512},
                            {"size", 60},
                            {"link_count", 3}}) {
    auto V = Good;
    (*(*V.getAsObject()->getArray("parents"))[0].getAsObject()->getObject(
        "metadata"))[Key] = Value;
    Refused(V);
  }
  auto V = Good;
  auto &Rows = *V.getAsObject()->getArray("parents");
  Rows.push_back(Rows.front());
  Refused(V);
  V = Good;
  (*V.getAsObject())["extra"] = 0;
  Refused(V);
}

TEST(ProcessReport, GuestDirectoriesAreExplicitAndTyped) {
  auto Parsed = processOptionsFromJSON(R"({"linux_files":{"files":[],
    "directories":["/work","/app/files"],"working_directory":"/work"},
    "android":{"jni":true,"application_snapshot":{"phase":"application",
    "files_directory":"/app/files"}}})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->LinuxFiles->WorkingDirectory, "/work");
  EXPECT_EQ(Parsed->Android->Application->files_directory, "/app/files");
  for (const char *Value : {"null", "12", R"("")", R"("relative")",
                            R"("/absent")", R"("//work")"}) {
    auto Bad = processOptionsFromJSON(
        std::string(
            R"({"linux_files":{"files":[],"directories":["/work"],"working_directory":)") +
        Value + "}}");
    EXPECT_FALSE(bool(Bad)) << Value;
    llvm::consumeError(Bad.takeError());
  }
  for (const char *Input :
       {R"({"phase":"attached","files_directory":"/work"})",
        R"({"phase":"application","files_directory":null})",
        R"({"phase":"application","files_directory":""})",
        R"({"phase":"application","files_directory":"relative"})",
        R"({"phase":"application","files_directory":"/a\u0000b"})"}) {
    auto Bad = processOptionsFromJSON(
        std::string(R"({"android":{"application_snapshot":)") + Input + "}}");
    EXPECT_FALSE(bool(Bad)) << Input;
    llvm::consumeError(Bad.takeError());
  }
}

TEST(ProcessReport, AndroidInstrumentationInputIsExplicitAndStrict) {
  using Snapshot = mobile::dalvik::AndroidApplicationSnapshot;
  for (const char *Kind : {"uninitialized", "platform"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(
            R"({"android":{"jni":true,"application_snapshot":{"phase":"attached","instrumentation":")") +
        Kind + "\"}}}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->Android->Application->instrumentation,
              std::string_view(Kind) == "platform"
                  ? Snapshot::Instrumentation::Platform
                  : Snapshot::Instrumentation::Uninitialized);
  }
  for (const char *Bad :
       {R"({"phase":"unattached","instrumentation":"platform"})",
        R"({"phase":"attached","instrumentation":"custom"})",
        R"({"phase":"attached","instrumentation":null})",
        R"({"phase":"attached","instrumentation":false})"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"android":{"application_snapshot":)") + Bad + "}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, AndroidApplicationSnapshotIsExplicitAndStrict) {
  auto Parsed = processOptionsFromJSON(R"({"android":{"jni":true,
    "application_snapshot":{"phase":"application","package_name":"test.app"}}})");
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  ASSERT_TRUE(Parsed->Android->Application);
  EXPECT_EQ(Parsed->Android->Application->phase,
            mobile::dalvik::AndroidApplicationSnapshot::Phase::Application);
  EXPECT_EQ(Parsed->Android->Application->package_name, "test.app");
  for (const char *Invalid :
       {"null", "[]", "true", "{}", R"({"phase":1})", R"({"phase":"ready"})",
        R"({"package_name":"test.app"})",
        R"({"phase":"attached","package_name":"test.app"})",
        R"({"phase":"application","package_name":""})",
        R"({"phase":"application","package_name":"a\u0000b"})",
        R"({"phase":"unattached","invent":true})"}) {
    auto Bad = processOptionsFromJSON(
        std::string("{\"android\":{\"application_snapshot\":") + Invalid +
        "}}");
    ASSERT_FALSE(bool(Bad)) << Invalid;
    llvm::consumeError(Bad.takeError());
  }
  for (const char *Phase : {"unattached", "attached", "application"}) {
    auto Input = processOptionsFromJSON(
        std::string("{\"android\":{\"application_snapshot\":{\"phase\":\"") +
        Phase + "\"}}}");
    ASSERT_TRUE(bool(Input)) << llvm::toString(Input.takeError());
    EXPECT_FALSE(Input->Android->Application->package_name.has_value());
  }
}

TEST(ProcessReport, RejectsMalformedRequestsBeforeWorkloadConstruction) {
#define NEVERD_PROCESS_INVALID_JSON(Name, Text)                                \
  {                                                                            \
    SCOPED_TRACE(#Name);                                                       \
    auto Result = processOptionsFromJSON(Text);                                \
    EXPECT_FALSE(bool(Result));                                                \
    llvm::consumeError(Result.takeError());                                    \
  }
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_INVALID_JSON
  auto Large = processOptionsFromJSON(std::string(field::JSONLimit + 1, ' '));
  EXPECT_FALSE(bool(Large));
  EXPECT_EQ(llvm::toString(Large.takeError()), field::TooLarge);
}
TEST(ProcessReport, DexWireBudgetPreservesTheMetadataAndDecodedByteBounds) {
  const std::string Prefix = R"({"android":{"jni":true,"dex_images":[")";
  const std::string Suffix = R"("]}})";
  auto check = [&](std::string Text, bool Accepted) {
    auto Options = processOptionsFromJSON(Text);
    EXPECT_EQ(bool(Options), Accepted);
    if (!Options) {
      llvm::consumeError(Options.takeError());
      return;
    }
    ASSERT_TRUE(Options->Android);
    size_t Bytes = 0;
    for (const auto &Image : Options->Android->DexImages) {
      Bytes += Image.size();
      EXPECT_TRUE(
          llvm::all_of(Image, [](uint8_t Byte) { return Byte == 0xab; }));
    }
    EXPECT_EQ(Bytes, 1024u * 1024u);
  };
  std::string Hex;
  for (unsigned I = 0; I < 1024 * 1024; ++I)
    Hex += "ab";
  const std::string Request = Prefix + Hex + Suffix;
  ASSERT_GT(Request.size(), 65536u);
  check(Request, true);
  check(Request + std::string(field::JSONLimit - Request.size(), ' '), true);
  check(Request + std::string(field::JSONLimit - Request.size() + 1, ' '),
        false);
  check(Prefix + Hex + "ab" + Suffix, false);
  check(Prefix + Hex.substr(0, Hex.size() / 2) + "\",\"" +
            Hex.substr(Hex.size() / 2) + Suffix,
        true);
  check(Prefix + Hex + "\",\"ab" + Suffix, false);
  check(Prefix + "not-hex" + Hex + Suffix, false);
  // Enlarging the outer parser allocation does not enlarge other input fields.
  auto Exact = processOptionsFromJSON(
      "{}" + std::string(field::JSONMetadataLimit - 2, ' '));
  ASSERT_TRUE(bool(Exact)) << llvm::toString(Exact.takeError());
  for (auto Text :
       {"{}" + std::string(field::JSONMetadataLimit - 1, ' '),
        std::string(R"({"arguments":[")") + std::string(65536, 'a') + "\"]}"}) {
    auto Rejected = processOptionsFromJSON(Text);
    ASSERT_FALSE(bool(Rejected));
    EXPECT_EQ(llvm::toString(Rejected.takeError()), field::TooLarge);
  }
}
TEST(ProcessReport, DarwinFileInputsAreLosslessAndRequireDarwinProfiles) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"00FF78"}],"stdin_hex":"00ff",
    "descriptor_limit":32}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->DarwinFiles);
  EXPECT_EQ(O->DarwinFiles->Files.at("/data"),
            (std::vector<uint8_t>{0, 255, 'x'}));
  ASSERT_TRUE(O->DarwinFiles->StandardInput);
  EXPECT_EQ(*O->DarwinFiles->StandardInput, (std::vector<uint8_t>{0, 255}));
  EXPECT_EQ(O->DarwinFiles->DescriptorLimit, 32u);
  for (auto P : {ProcessProfile::LinuxELF64, ProcessProfile::WindowsPE64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinFilesProfile);
  }
  O->DarwinFiles->Files["/data/child"] = {};
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("catalogue path"),
              std::string::npos);
  }
  auto Empty = processOptionsFromJSON(R"({"darwin_files":{"files":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_FALSE(Empty->DarwinFiles->StandardInput);
  auto EOFInput =
      processOptionsFromJSON(R"({"darwin_files":{"files":[],"stdin_hex":""}})");
  ASSERT_TRUE(bool(EOFInput)) << llvm::toString(EOFInput.takeError());
  ASSERT_TRUE(EOFInput->DarwinFiles->StandardInput);
  EXPECT_TRUE(EOFInput->DarwinFiles->StandardInput->empty());
}
TEST(ProcessReport, DarwinXattrInputKeepsOpaqueBytesOrderAndKnownEmpty) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"00ff","extended_attributes":[
      {"name":"z/slash","bytes_hex":"00FF80"},
      {"name":"α","bytes_hex":""},
      {"name":"a","bytes_hex":"78"}]},
    {"path":"/unknown","bytes_hex":""}],
    "directories":[{"path":"/dir","extended_attributes":[]}],
    "symbolic_links":[{"path":"/link","target_hex":"2f64617461",
      "extended_attributes":[{"name":"user.link","bytes_hex":"42"}]}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  const auto &A = O->DarwinFiles->ExtendedAttributes.at("/data");
  ASSERT_EQ(A.size(), 3u);
  EXPECT_EQ(A[0].Name, "z/slash");
  EXPECT_EQ(A[0].Bytes, (std::vector<uint8_t>{0, 255, 128}));
  EXPECT_EQ(A[1].Name, "α");
  EXPECT_TRUE(A[1].Bytes.empty());
  EXPECT_EQ(A[2].Name, "a");
  EXPECT_FALSE(O->DarwinFiles->ExtendedAttributes.contains("/unknown"));
  EXPECT_TRUE(O->DarwinFiles->ExtendedAttributes.at("/dir").empty());
  EXPECT_EQ(O->DarwinFiles->ExtendedAttributes.at("/link")[0].Bytes,
            (std::vector<uint8_t>{66}));
}
TEST(ProcessReport, DarwinXattrInputRejectsMalformedAndUnknownFields) {
  for (const char *Attributes :
       {"null", "{}", "[null]", "[{\"name\":\"a\"}]",
        "[{\"name\":\"a\",\"bytes_hex\":\"\",\"extra\":0}]",
        "[{\"name\":\"\",\"bytes_hex\":\"\"}]",
        "[{\"name\":\"a\\u0000b\",\"bytes_hex\":\"\"}]",
        "[{\"name\":\"a\",\"bytes_hex\":\"0\"}]",
        "[{\"name\":\"a\",\"bytes_hex\":\"gg\"}]",
        "[{\"name\":\"a\",\"bytes_hex\":1}]",
        "[{\"name\":1,\"bytes_hex\":\"\"}]",
        "[{\"name\":\"a\",\"bytes_hex\":\"\"},{\"name\":\"a\",\"bytes_hex\":"
        "\"\"}]",
        "[{\"name\":\"com.apple.ResourceFork\",\"bytes_hex\":\"\"}]"})
    for (unsigned Kind = 0; Kind != 3; ++Kind) {
      std::string Entry = "{\"path\":\"/data\",\"extended_attributes\":";
      Entry += Attributes;
      if (Kind == 0)
        Entry += ",\"bytes_hex\":\"\"";
      if (Kind == 2)
        Entry += ",\"target_hex\":\"78\"";
      Entry += '}';
      const auto Text =
          Kind == 0
              ? "{\"darwin_files\":{\"files\":[" + Entry + "]}}"
              : "{\"darwin_files\":{\"files\":[],\"" +
                    std::string(Kind == 1 ? "directories" : "symbolic_links") +
                    "\":[" + Entry + "]}}";
      auto O = processOptionsFromJSON(Text);
      ASSERT_FALSE(bool(O)) << Text;
      llvm::consumeError(O.takeError());
    }
}
TEST(ProcessReport, DarwinXattrMutationGrantsAreStrictAndObjectSpecific) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"00ff","extended_attributes":[],
     "mutable_extended_attributes":true},
    {"path":"/readonly","bytes_hex":"","extended_attributes":[],
     "mutable_extended_attributes":false}],
    "directories":[{"path":"/dir","extended_attributes":[],
      "mutable_extended_attributes":true}],
    "symbolic_links":[{"path":"/link","target_hex":"2f64617461",
      "extended_attributes":[],"mutable_extended_attributes":true}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->DarwinFiles->MutableExtendedAttributes,
            (std::set<std::string>{"/data", "/dir", "/link"}));
  for (const char *Value : {"null", "0", "1", "\"true\"", "[]", "{}"})
    for (const char *Kind : {"files", "directories", "symbolic_links"}) {
      std::string Entry = "{\"path\":\"/data\",\"extended_attributes\":[],"
                          "\"mutable_extended_attributes\":";
      Entry += Value;
      if (llvm::StringRef(Kind) == "files")
        Entry += ",\"bytes_hex\":\"\"";
      if (llvm::StringRef(Kind) == "symbolic_links")
        Entry += ",\"target_hex\":\"78\"";
      Entry += '}';
      auto Bad = processOptionsFromJSON(
          "{\"darwin_files\":{\"" + std::string(Kind) + "\":[" + Entry +
          (llvm::StringRef(Kind) == "files" ? "]}}" : "] ,\"files\":[]}}"));
      ASSERT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
  auto Unknown = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"","mutable_extended_attributes":true}]}})");
  ASSERT_FALSE(bool(Unknown));
  llvm::consumeError(Unknown.takeError());
  auto FalseUnknown = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/data","bytes_hex":"","mutable_extended_attributes":false}]}})");
  ASSERT_TRUE(bool(FalseUnknown));
  EXPECT_TRUE(FalseUnknown->DarwinFiles->MutableExtendedAttributes.empty());
}
TEST(ProcessReport,
     DarwinSymbolicLinkTargetsAreLosslessAndRequireDarwinProfiles) {
  auto Good =
      processOptionsFromJSON(R"({"darwin_files":{"files":[],"symbolic_links":[
    {"path":"/link","target_hex":"2E2F2FFF2F"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_TRUE(Good->DarwinFiles);
  EXPECT_EQ(Good->DarwinFiles->SymbolicLinks.at("/link"),
            (std::vector<uint8_t>{'.', '/', '/', 0xff, '/'}));
  for (auto Profile : {ProcessProfile::LinuxELF64, ProcessProfile::WindowsPE64,
                       ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing-symbolic-link-fixture", Profile, *Good);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinFilesProfile);
  }
  Good->DarwinFiles->SymbolicLinks["/link"].push_back(0);
  for (auto Profile : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                       ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing-symbolic-link-fixture", Profile, *Good);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("symbolic link targets"),
              std::string::npos);
  }
  auto Empty = processOptionsFromJSON(
      R"({"darwin_files":{"files":[],"symbolic_links":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->DarwinFiles->SymbolicLinks.empty());
}

TEST(ProcessReport,
     DarwinSymbolicLinksRejectMalformedTargetsAndNamespaceInput) {
  for (
      auto Bad :
      {R"({"symbolic_links":null})", R"({"symbolic_links":{}})",
       R"({"symbolic_links":[{}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":null}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":""}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61F"}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"zz"}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"610062"}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61","writable":true}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61"},{"path":"/link","target_hex":"62"}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61"}],"files":[{"path":"/link","bytes_hex":""}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61"}],"directories":[{"path":"/link/child"}]})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61"}],"working_directory":"/link"})",
       R"({"symbolic_links":[{"path":"/link","target_hex":"61"}],"directories":[{"path":"/","mutable":true}]})"}) {
    SCOPED_TRACE(Bad);
    auto Files = llvm::cantFail(llvm::json::parse(Bad));
    if (!Files.getAsObject()->get(field::Files))
      (*Files.getAsObject())[field::Files] = llvm::json::Array{};
    auto R = processOptionsFromJSON(R"({"darwin_files":)" +
                                    llvm::formatv("{0}", Files).str() + '}');
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (unsigned Size : {1023u, 1024u}) {
    auto R = processOptionsFromJSON(
        R"({"darwin_files":{"files":[],"symbolic_links":[{"path":"/link","target_hex":")" +
        std::string(Size * 2, 'f') + R"("}]}})");
    if (Size == 1023)
      ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    else {
      EXPECT_FALSE(bool(R));
      llvm::consumeError(R.takeError());
    }
  }
}

TEST(ProcessReport, DarwinWritableFilesRequireExplicitBooleanAdmission) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[
    {"path":"/a","bytes_hex":"00ff","writable":true},
    {"path":"/b","bytes_hex":"","writable":false},
    {"path":"/c","bytes_hex":""}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->WritableFiles, (std::set<std::string>{"/a"}));
  for (auto Value : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Bad = processOptionsFromJSON(
        std::string(
            R"({"darwin_files":{"files":[{"path":"/a","bytes_hex":"","writable":)") +
        Value + "}]}}");
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
}

TEST(ProcessReport, DarwinMutationPolicyIsExplicitStrictAndLossless) {
  auto M = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  (*M.getAsObject())["flags"] = 0;
  (*M.getAsObject())["link_count"] = 1;
  auto Parse = [&](llvm::StringRef Policy) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","writable":true,"metadata":)" +
        llvm::formatv("{0}", M).str() + R"(,"mutation_policy":)" +
        Policy.str() + "}]}}");
  };
  auto Good = Parse(darwin_test::MutationPolicyJSON);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &P = Good->DarwinFiles->MutationPolicies.at("/data");
  EXPECT_EQ(P.AllocationUnit, 4096u);
  EXPECT_EQ(P.Time.Seconds, -7);
  EXPECT_EQ(P.Time.Nanoseconds, 123456789);
  for (auto Seconds : {INT64_MIN, int64_t(0), INT64_MAX}) {
    auto Boundary =
        Parse(R"({"allocation_unit":"4096","mutation_time":{"seconds":")" +
              std::to_string(Seconds) + R"(","nanoseconds":999999999}})");
    ASSERT_TRUE(bool(Boundary)) << llvm::toString(Boundary.takeError());
    EXPECT_EQ(Boundary->DarwinFiles->MutationPolicies.at("/data").Time.Seconds,
              Seconds);
  }
  for (
      auto Bad :
      {"null", "[]", "true", "{}", R"({"allocation_unit":4096})",
       R"({"allocation_unit":4096,"mutation_time":null})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0,"nanoseconds":0,"extra":0}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0,"nanoseconds":0},"extra":0})",
       R"({"allocation_unit":4096.5,"mutation_time":{"seconds":0,"nanoseconds":0}})",
       R"({"allocation_unit":true,"mutation_time":{"seconds":0,"nanoseconds":0}})",
       R"({"allocation_unit":4294967296,"mutation_time":{"seconds":0,"nanoseconds":0}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":"9223372036854775808","nanoseconds":0}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0,"nanoseconds":1000000000}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0,"nanoseconds":-1}})",
       R"({"allocation_unit":4096,"mutation_time":{"seconds":0.5,"nanoseconds":0}})",
       R"({"allocation_unit":0,"mutation_time":{"seconds":0,"nanoseconds":0}})"}) {
    auto Rejected = Parse(Bad);
    EXPECT_FALSE(bool(Rejected)) << Bad;
    llvm::consumeError(Rejected.takeError());
  }
  (*M.getAsObject())["blocks"] = 0;
  auto Incoherent = Parse(darwin_test::MutationPolicyJSON);
  EXPECT_FALSE(bool(Incoherent));
  llvm::consumeError(Incoherent.takeError());
}

TEST(ProcessReport, DarwinUmaskIsIndependentExplicitAndStrict) {
  for (auto Mask : {"0", "4095", "\"4095\""}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"umask":)") + Mask + "}}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_TRUE(Parsed->DarwinFiles->InitialUmask);
    EXPECT_FALSE(Parsed->DarwinFiles->CreationPolicy);
    EXPECT_TRUE(Parsed->DarwinFiles->MutableDirectories.empty());
  }
  for (auto Mask : {"null", "true", "[]", "{}", "4096", "65536", "-1", "0.5",
                    "\"4095x\""}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"umask":)") + Mask + "}}");
    EXPECT_FALSE(bool(Parsed)) << Mask;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinCreationPolicyIsStrictAndPreservesUnsignedInodes) {
  auto Parent = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  auto *M = Parent.getAsObject();
  (*M)["inode"] = 41;
  (*M)["mode"] = 0040755;
  (*M)["flags"] = 0;
  (*M)["size"] = 0;
  (*M)["blocks"] = 0;
  auto Parse = [&](llvm::StringRef Policy) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[],"umask":23,"directories":[{"path":"/","mutable":true,"metadata":)" +
        llvm::formatv("{0}", Parent).str() + R"(}],"creation_policy":)" +
        Policy.str() + "}}");
  };
  auto Good = Parse(darwin_test::CreationPolicyJSON);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &P = *Good->DarwinFiles->CreationPolicy;
  EXPECT_EQ(P.FirstInode, 0xfedcba9876543211ULL);
  EXPECT_EQ(P.BlockSize, 8192u);
  EXPECT_EQ(P.Generation, 0x89abcdefu);
  EXPECT_EQ(P.Time.Seconds, -19);
  EXPECT_EQ(P.Time.Nanoseconds, 987654321);
  EXPECT_EQ(P.Mutation.AllocationUnit, 4096u);
  EXPECT_EQ(P.Mutation.Time.Seconds, -7);
  for (auto Key : {"first_inode", "block_size", "generation", "creation_time",
                   "mutation_policy"}) {
    auto Value =
        llvm::cantFail(llvm::json::parse(darwin_test::CreationPolicyJSON));
    Value.getAsObject()->erase(Key);
    auto Missing = Parse(llvm::formatv("{0}", Value).str());
    EXPECT_FALSE(bool(Missing)) << Key;
    llvm::consumeError(Missing.takeError());
    (*Value.getAsObject())[Key] = nullptr;
    auto Null = Parse(llvm::formatv("{0}", Value).str());
    EXPECT_FALSE(bool(Null)) << Key;
    llvm::consumeError(Null.takeError());
  }
  for (auto Key : {"first_inode", "block_size", "generation"}) {
    for (auto Bad : {"true", "1.5", "-1", "\"18446744073709551616\""}) {
      auto Value =
          llvm::cantFail(llvm::json::parse(darwin_test::CreationPolicyJSON));
      (*Value.getAsObject())[Key] = llvm::cantFail(llvm::json::parse(Bad));
      auto Parsed = Parse(llvm::formatv("{0}", Value).str());
      EXPECT_FALSE(bool(Parsed)) << Key << ':' << Bad;
      llvm::consumeError(Parsed.takeError());
    }
  }
  auto Value =
      llvm::cantFail(llvm::json::parse(darwin_test::CreationPolicyJSON));
  (*Value.getAsObject())["first_inode"] = "18446744073709551615";
  auto Last = Parse(llvm::formatv("{0}", Value).str());
  ASSERT_TRUE(bool(Last)) << llvm::toString(Last.takeError());
  EXPECT_EQ(Last->DarwinFiles->CreationPolicy->FirstInode, UINT64_MAX);
  (*Value.getAsObject())["unknown"] = 0;
  auto Extra = Parse(llvm::formatv("{0}", Value).str());
  EXPECT_FALSE(bool(Extra));
  llvm::consumeError(Extra.takeError());
}

TEST(ProcessReport, DarwinNamespaceCreationPolicyIsStrictAndLossless) {
  auto Parent = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  auto *M = Parent.getAsObject();
  (*M)["inode"] = 41;
  (*M)["mode"] = 0040755;
  (*M)["flags"] = 0;
  (*M)["size"] = 0;
  (*M)["blocks"] = 0;
  auto Parse = [&](const llvm::json::Value &Policy) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[],"umask":23,"directories":[{"path":"/","mutable":true,"metadata":)" +
        llvm::formatv("{0}", Parent).str() + R"(}],"creation_policy":)" +
        llvm::formatv("{0}", Policy).str() + "}}");
  };
  const auto Good = llvm::cantFail(
      llvm::json::parse(darwin_test::NamespaceCreationPolicyJSON));
  auto Parsed = Parse(Good);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto &P = *Parsed->DarwinFiles->CreationPolicy->Namespace;
  EXPECT_EQ(P.SymbolicLinkAllocationUnit, 512u);
  EXPECT_EQ(P.DirectoryEntrySize, 32u);
  EXPECT_EQ(P.DirectoryBlocks, 7u);
  auto Refused = [&](const llvm::json::Value &V) {
    auto Out = Parse(V);
    EXPECT_FALSE(bool(Out)) << llvm::formatv("{0}", V).str();
    llvm::consumeError(Out.takeError());
  };
  for (const char *Key : {"symbolic_link_allocation_unit",
                          "directory_entry_size", "directory_blocks"}) {
    auto V = Good;
    auto *N = V.getAsObject()->getObject("namespace_policy");
    N->erase(Key);
    Refused(V);
    for (const char *Bad :
         {"null", "true", "-1", "1.5", "\"18446744073709551616\""}) {
      (*N)[Key] = llvm::cantFail(llvm::json::parse(Bad));
      Refused(V);
    }
  }
  for (const char *Bad : {"null", "[]", "{}", "false", "1"}) {
    auto V = Good;
    (*V.getAsObject())["namespace_policy"] =
        llvm::cantFail(llvm::json::parse(Bad));
    Refused(V);
  }
  auto V = Good;
  (*V.getAsObject()->getObject("namespace_policy"))["extra"] = 1;
  Refused(V);
  V = Good;
  (*V.getAsObject())["extra"] = 1;
  Refused(V);
  V = Good;
  auto *N = V.getAsObject()->getObject("namespace_policy");
  (*N)["symbolic_link_allocation_unit"] = "16777216";
  (*N)["directory_entry_size"] = "16777216";
  (*N)["directory_blocks"] = "9223372036854775807";
  auto Max = Parse(V);
  ASSERT_TRUE(bool(Max)) << llvm::toString(Max.takeError());
  EXPECT_EQ(Max->DarwinFiles->CreationPolicy->Namespace->DirectoryBlocks,
            uint64_t(INT64_MAX));
  (*N)["directory_blocks"] = "9223372036854775808";
  Refused(V);
}

TEST(ProcessReport, DarwinInitialLinkMutationIsExplicitStrictAndLossless) {
  auto M = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  (*M.getAsObject())["inode"] = 57;
  (*M.getAsObject())["mode"] = 0120777;
  (*M.getAsObject())["size"] = 1;
  (*M.getAsObject())["flags"] = 0;
  (*M.getAsObject())["link_count"] = 1;
  const auto Policy = llvm::cantFail(llvm::json::parse(
      R"({"mutation_time":{"seconds":"-9223372036854775808","nanoseconds":999999999}})"));
  llvm::json::Object Link{{"path", "/l"},
                          {"target_hex", "78"},
                          {"mutable", true},
                          {"metadata", M},
                          {"mutation_policy", Policy}};
  auto Parse = [](const llvm::json::Object &Input) {
    llvm::json::Object O{
        {"darwin_files",
         llvm::json::Object{
             {"files", llvm::json::Array{}},
             {"directories", llvm::json::Array{llvm::json::Object{
                                 {"path", "/"}, {"mutable", true}}}},
             {"symbolic_links", llvm::json::Array{llvm::json::Value(
                                    llvm::json::Object(Input))}}}}};
    return processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(std::move(O))).str());
  };
  auto Good = Parse(Link);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_TRUE(Good->DarwinFiles->MutableSymbolicLinks.contains("/l"));
  const auto &P = Good->DarwinFiles->SymbolicLinkMutationPolicies.at("/l");
  EXPECT_EQ(P.Time.Seconds, INT64_MIN);
  EXPECT_EQ(P.Time.Nanoseconds, 999999999);
  auto Refused = [&](const llvm::json::Object &O) {
    auto Out = Parse(O);
    EXPECT_FALSE(bool(Out));
    llvm::consumeError(Out.takeError());
  };
  for (const char *Bad : {"false", "null", "0", "[]", "\"true\""}) {
    auto O = Link;
    O["mutable"] = llvm::cantFail(llvm::json::parse(Bad));
    Refused(O);
  }
  auto O = Link;
  O.erase("mutable");
  Refused(O);
  O = Link;
  O.erase("metadata");
  Refused(O);
  O = Link;
  O["extra"] = 1;
  Refused(O);
  for (
      const char *Bad :
      {"null", "true", "[]", "{}", R"({"mutation_time":null})",
       R"({"mutation_time":{"seconds":0,"nanoseconds":0},"extra":1})",
       R"({"mutation_time":{"seconds":"9223372036854775808","nanoseconds":0}})",
       R"({"mutation_time":{"seconds":0,"nanoseconds":-1}})",
       R"({"mutation_time":{"seconds":0,"nanoseconds":1000000000}})",
       R"({"mutation_time":{"seconds":1.5,"nanoseconds":0}})"}) {
    O = Link;
    O["mutation_policy"] = llvm::cantFail(llvm::json::parse(Bad));
    Refused(O);
  }
  O = Link;
  O.erase("mutation_policy");
  O.erase("metadata");
  auto WithoutStat = Parse(O);
  ASSERT_TRUE(bool(WithoutStat)) << llvm::toString(WithoutStat.takeError());
  EXPECT_TRUE(WithoutStat->DarwinFiles->SymbolicLinkMutationPolicies.empty());
}

TEST(ProcessReport, DarwinInitialDirectoryMutationPolicyIsStrictAndLossless) {
  auto Parent = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  (*Parent.getAsObject())["inode"] = 41;
  (*Parent.getAsObject())["mode"] = 0040755;
  (*Parent.getAsObject())["size"] = 0;
  auto Parse = [&](const llvm::json::Value &Policy) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[],"directories":[{"path":"/","metadata":)" +
        llvm::formatv("{0}", Parent).str() + R"(,"mutation_policy":)" +
        llvm::formatv("{0}", Policy).str() + "}]}}");
  };
  const auto Good = llvm::cantFail(llvm::json::parse(
      R"({"directory_entry_size":"17","mutation_time":{"seconds":"-9223372036854775808","nanoseconds":999999999}})"));
  auto Out = Parse(Good);
  ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
  const auto &P = Out->DarwinFiles->DirectoryMutationPolicies.at("/");
  EXPECT_EQ(P.DirectoryEntrySize, 17u);
  EXPECT_EQ(P.Time.Seconds, INT64_MIN);
  EXPECT_EQ(P.Time.Nanoseconds, 999999999);
  auto Refused = [&](const llvm::json::Value &V) {
    auto Bad = Parse(V);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  };
  for (const char *Key : {"directory_entry_size", "mutation_time"}) {
    auto V = Good;
    V.getAsObject()->erase(Key);
    Refused(V);
    for (const char *Bad : {"null", "true", "[]", "-1", "1.5"}) {
      (*V.getAsObject())[Key] = llvm::cantFail(llvm::json::parse(Bad));
      Refused(V);
    }
  }
  auto V = Good;
  (*V.getAsObject())["extra"] = 1;
  Refused(V);
  V = Good;
  (*V.getAsObject())["directory_entry_size"] = 16777217;
  Refused(V);
  V = Good;
  (*V.getAsObject()->getObject("mutation_time"))["nanoseconds"] = 1000000000;
  Refused(V);
  V = Good;
  (*V.getAsObject()->getObject("mutation_time"))["seconds"] =
      "-9223372036854775809";
  Refused(V);
}

TEST(ProcessReport, DarwinVirtualEnumerationPolicyIsStrictAndLossless) {
  auto Parent = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  auto *M = Parent.getAsObject();
  (*M)["inode"] = 41;
  (*M)["mode"] = 0040755;
  (*M)["size"] = 0;
  auto Parse = [&](const llvm::json::Value &Policy) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[],"directories":[{"path":"/","metadata":)" +
        llvm::formatv("{0}", Parent).str() + R"(,"enumeration_policy":)" +
        llvm::formatv("{0}", Policy).str() + "}]}}");
  };
  const auto Good = llvm::cantFail(llvm::json::parse(
      R"({"minimum_buffer_size":1,"initial_minimum_buffer_size":64,"seek_offset":"18446744073709551615"})"));
  auto Parsed = Parse(Good);
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  const auto &P = Parsed->DarwinFiles->DirectoryEnumerationPolicies.at("/");
  EXPECT_EQ(P.MinimumBufferSize, 1u);
  EXPECT_EQ(P.InitialMinimumBufferSize, 64u);
  EXPECT_EQ(P.SeekOffset, UINT64_MAX);
  EXPECT_FALSE(P.BulkAttributes);
  auto Refused = [&](const llvm::json::Value &V) {
    auto Out = Parse(V);
    EXPECT_FALSE(bool(Out)) << llvm::formatv("{0}", V).str();
    llvm::consumeError(Out.takeError());
  };
  for (const char *Key :
       {"minimum_buffer_size", "initial_minimum_buffer_size", "seek_offset"}) {
    auto V = Good;
    V.getAsObject()->erase(Key);
    Refused(V);
    for (const char *Bad :
         {"null", "true", "-1", "1.5", "\"18446744073709551616\""}) {
      (*V.getAsObject())[Key] = llvm::cantFail(llvm::json::parse(Bad));
      Refused(V);
    }
  }
  for (const char *Bad : {"null", "[]", "{}", "false", "1"})
    Refused(llvm::cantFail(llvm::json::parse(Bad)));
  for (bool Enabled : {false, true}) {
    auto V = Good;
    (*V.getAsObject())["bulk_attributes"] = Enabled;
    auto Out = Parse(V);
    ASSERT_TRUE(bool(Out)) << llvm::toString(Out.takeError());
    EXPECT_EQ(
        Out->DarwinFiles->DirectoryEnumerationPolicies.at("/").BulkAttributes,
        Enabled);
  }
  for (const char *Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto V = Good;
    (*V.getAsObject())["bulk_attributes"] =
        llvm::cantFail(llvm::json::parse(Bad));
    Refused(V);
  }
  auto V = Good;
  (*V.getAsObject())["extra"] = 1;
  Refused(V);
  V = Good;
  (*V.getAsObject())["minimum_buffer_size"] = "134217728";
  (*V.getAsObject())["initial_minimum_buffer_size"] = "134217728";
  auto Max = Parse(V);
  ASSERT_TRUE(bool(Max)) << llvm::toString(Max.takeError());
  (*V.getAsObject())["minimum_buffer_size"] = "134217729";
  Refused(V);
}

TEST(ProcessReport, DarwinDirectoriesAndWorkingDirectoryAreExplicitAndStrict) {
  auto O = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/empty/deep"}],"working_directory":"/empty"}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  EXPECT_EQ(O->DarwinFiles->WorkingDirectory, "/empty");
  EXPECT_EQ(O->DarwinFiles->Directories,
            (std::set<std::string>{"/empty/deep"}));
  auto M = llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  (*M.getAsObject())["mode"] = 0040755;
  (*M.getAsObject())["size"] = "9223372036854775807";
  auto Directory = processOptionsFromJSON(
      R"({"darwin_files":{"files":[],"directories":[{"path":"/","metadata":)" +
      llvm::formatv("{0}", M).str() + R"(}],"working_directory":"/"}})");
  ASSERT_TRUE(bool(Directory)) << llvm::toString(Directory.takeError());
  EXPECT_EQ(Directory->DarwinFiles->Metadata.at("/").Size, uint64_t(INT64_MAX));
  for (
      auto Bad :
      {R"({"files":[],"directories":null})",
       R"({"files":[],"directories":["/empty"]})",
       R"({"files":[],"directories":[{"path":"/empty","extra":1}]})",
       R"({"files":[],"directories":[{"path":"/empty"},{"path":"/empty"}]})",
       R"({"files":[],"directories":[{"path":"/empty/"}]})",
       R"({"files":[{"path":"/a","bytes_hex":""}],"directories":[{"path":"/a"}]})",
       R"({"files":[{"path":"/a","bytes_hex":""}],"directories":[{"path":"/a/b"}]})",
       R"({"files":[],"working_directory":null})",
       R"({"files":[],"working_directory":""})",
       R"({"files":[],"working_directory":"relative"})",
       R"({"files":[],"working_directory":"/missing"})"}) {
    SCOPED_TRACE(Bad);
    auto Parsed =
        processOptionsFromJSON(std::string("{\"darwin_files\":") + Bad + '}');
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinNamespaceAuthorityRequiresAnExplicitBoolean) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true},
    {"path":"/no","mutable":false},{"path":"/default"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->MutableDirectories,
            (std::set<std::string>{"/"}));
  EXPECT_TRUE(Good->DarwinFiles->WritableFiles.empty());
  for (auto Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(
            R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  auto Extra = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true,"unknown":0}]}})");
  EXPECT_FALSE(bool(Extra));
  llvm::consumeError(Extra.takeError());
}

TEST(ProcessReport, DarwinSwapRenameRequiresExplicitMutableDirectoryBoolean) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true,"swap_rename":true},
    {"path":"/no","swap_rename":false},{"path":"/default"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->SwapRenameDirectories,
            (std::set<std::string>{"/"}));
  EXPECT_EQ(Good->DarwinFiles->MutableDirectories,
            (std::set<std::string>{"/"}));
  EXPECT_TRUE(Good->DarwinFiles->RemovableDirectories.empty());
  for (auto Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"directories":[
          {"path":"/","mutable":true,"swap_rename":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  for (
      auto Bad :
      {R"({"darwin_files":{"files":[],"directories":[{"path":"/","swap_rename":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":false,"swap_rename":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true,"unknown":0}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"swap_rename":true},{"path":"/","mutable":true,"swap_rename":true}]}})"}) {
    auto Parsed = processOptionsFromJSON(Bad);
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinInitialDirectoryRemovalRequiresExplicitBoolean) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true},
    {"path":"/yes","removable":true},{"path":"/no","removable":false},
    {"path":"/default"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->RemovableDirectories,
            (std::set<std::string>{"/yes"}));
  EXPECT_EQ(Good->DarwinFiles->MutableDirectories,
            (std::set<std::string>{"/"}));
  for (auto Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"directories":[
          {"path":"/","mutable":true},{"path":"/yes","removable":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  for (
      auto Bad :
      {R"({"darwin_files":{"files":[],"directories":[{"path":"/yes","removable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"removable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/yes","removable":true,"unknown":0}]}})"}) {
    auto Parsed = processOptionsFromJSON(Bad);
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinInitialDirectoryMoveRequiresExplicitBoolean) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true},
    {"path":"/yes","movable":true},{"path":"/no","movable":false},
    {"path":"/default"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->MovableDirectories,
            (std::set<std::string>{"/yes"}));
  EXPECT_TRUE(Good->DarwinFiles->RemovableDirectories.empty());
  EXPECT_TRUE(Good->DarwinFiles->SwapRenameDirectories.empty());
  for (auto Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"directories":[
          {"path":"/","mutable":true},{"path":"/yes","movable":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  for (
      auto Bad :
      {R"({"darwin_files":{"files":[],"directories":[{"path":"/yes","movable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"movable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/yes","movable":true,"unknown":0}]}})"}) {
    auto Parsed = processOptionsFromJSON(Bad);
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinInitialDirectoryExchangeRequiresExplicitBoolean) {
  auto Good = processOptionsFromJSON(R"({"darwin_files":{"files":[],
    "directories":[{"path":"/","mutable":true},
    {"path":"/yes","exchangeable":true},{"path":"/no","exchangeable":false},
    {"path":"/default"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->ExchangeableDirectories,
            (std::set<std::string>{"/yes"}));
  EXPECT_TRUE(Good->DarwinFiles->RemovableDirectories.empty());
  EXPECT_TRUE(Good->DarwinFiles->MovableDirectories.empty());
  EXPECT_TRUE(Good->DarwinFiles->SwapRenameDirectories.empty());
  for (auto Bad : {"null", "0", "1", "\"true\"", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_files":{"files":[],"directories":[
          {"path":"/","mutable":true},{"path":"/yes","exchangeable":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  for (
      auto Bad :
      {R"({"darwin_files":{"files":[],"directories":[{"path":"/yes","exchangeable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true,"exchangeable":true}]}})",
       R"({"darwin_files":{"files":[],"directories":[{"path":"/","mutable":true},{"path":"/yes","exchangeable":true,"unknown":0}]}})"}) {
    auto Parsed = processOptionsFromJSON(Bad);
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinDirectorySnapshotsHaveStrictLosslessWireFields) {
  const auto Original =
      llvm::cantFail(llvm::json::parse(darwin_test::DirectoryContentsJSON));
  auto Parse = [&](const llvm::json::Value &Contents) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[{"path":"/data","bytes_hex":""}],"directories":[{"path":"/empty"},{"path":"/","contents":)" +
        llvm::formatv("{0}", Contents).str() + "}]}}");
  };
  auto Good = Parse(Original);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->DarwinFiles->DirectoryContents.at("/").Entries[3].Inode,
            0xfedcba9876543210ULL);
  for (auto Field : {"inode", "type", "next_offset", "seek_offset", "name"}) {
    auto V = Original;
    auto *Entries = V.getAsObject()->getArray("entries");
    (*Entries)[0].getAsObject()->erase(Field);
    auto Bad = Parse(V);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (const char *BadValue : {"null", "[]", "-1", "1.5", "9007199254740992",
                               "\"18446744073709551616\""}) {
    auto V = Original;
    (*V.getAsObject()->getArray("entries"))[3].getAsObject()->operator[](
        "inode") = llvm::cantFail(llvm::json::parse(BadValue));
    auto Bad = Parse(V);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (auto Text :
       {"null", "[]", "{}", R"({"entries":[],"minimum_buffer_size":0})",
        R"({"entries":[],"minimum_buffer_size":1,"unknown":0})"}) {
    auto Bad = Parse(llvm::cantFail(llvm::json::parse(Text)));
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
}

TEST(ProcessReport, MalformedDarwinFilesFailBeforeExecution) {
  for (
      const char *Bad :
      {"null", "[]", "{}", R"({"files":{}})", R"({"files":[],"unknown":1})",
       R"({"files":[],"descriptor_limit":2})",
       R"({"files":[],"descriptor_limit":4097})",
       R"({"files":[],"descriptor_limit":4294967296})",
       R"({"files":[],"descriptor_limit":3.5})",
       R"({"files":[],"stdin_hex":null})", R"({"files":[],"stdin_hex":"0"})",
       R"({"files":[],"stdin_hex":"zz"})",
       R"({"files":[{"path":"/x","bytes_hex":"00","extra":1}]})",
       R"({"files":[{"path":"x","bytes_hex":""}]})",
       R"({"files":[{"path":"/x\u0000","bytes_hex":""}]})",
       R"({"files":[{"path":"/x/../y","bytes_hex":""}]})",
       R"({"files":[{"path":"/x/","bytes_hex":""}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x","bytes_hex":"00"}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x/y","bytes_hex":""}]})"}) {
    SCOPED_TRACE(Bad);
    auto O =
        processOptionsFromJSON(std::string("{\"darwin_files\":") + Bad + '}');
    EXPECT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  ProcessOptions O;
  O.DarwinFiles.emplace();
  O.DarwinFiles->StandardInput =
      std::vector<uint8_t>(darwin_file_limits::Bytes + 1);
  auto R = emulateProcess("missing", ProcessProfile::MacOSMachO64, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("file input limits"),
            std::string::npos);
}

TEST(ProcessReport, DarwinMetadataIntegersAreLosslessAndStrictlyAdmitted) {
  const auto Original =
      llvm::cantFail(llvm::json::parse(darwin_test::MetadataJSON));
  auto Parse = [&](const llvm::json::Value &Metadata) {
    return processOptionsFromJSON(
        R"({"darwin_files":{"files":[{"path":"/data","bytes_hex":"30313233343536373839","metadata":)" +
        llvm::formatv("{0}", Metadata).str() + "}]}}");
  };
  auto Good = Parse(Original);
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &M = Good->DarwinFiles->Metadata.at("/data");
  EXPECT_EQ(M.Device, -123);
  EXPECT_EQ(M.Inode, 0xfedcba9876543210ULL);
  EXPECT_EQ(M.Mode, 0100644);
  EXPECT_EQ(M.UID, 0x89abcdefu);
  EXPECT_EQ(M.GID, 0xfedcba98u);
  EXPECT_EQ(M.Generation, 0x89abcdefu);
  EXPECT_EQ(M.AccessTime.Seconds, INT64_MIN + 1);
  EXPECT_EQ(M.ModificationTime.Seconds, INT64_MAX);
  EXPECT_EQ(M.ModificationTime.Nanoseconds, 999999999);
  EXPECT_EQ(M.BirthTime.Seconds, -5);
  EXPECT_EQ(M.BirthTime.Nanoseconds, 6);
  const std::pair<llvm::StringRef, llvm::json::Value> Invalid[] = {
      {"device", uint64_t(2147483648)},
      {"mode", 65536},
      {"mode", 0040644},
      {"link_count", 65536},
      {"uid", -1},
      {"gid", uint64_t(4294967296)},
      {"inode", 9007199254740992.0},
      {"inode", "18446744073709551616"},
      {"size", 9},
      {"blocks", "9223372036854775808"},
      {"block_size", uint64_t(2147483648)},
      {"flags", true},
      {"generation", 1.25},
      {"birth_time", nullptr},
      {"unknown", 0}};
  for (const auto &[Name, Value] : Invalid) {
    SCOPED_TRACE(Name.str());
    auto Changed = Original;
    (*Changed.getAsObject())[Name] = Value;
    auto Bad = Parse(Changed);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (const auto &[Name, Value] : *Original.getAsObject()) {
    auto Missing = Original;
    Missing.getAsObject()->erase(Name);
    auto Bad = Parse(Missing);
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (auto Time :
       {"access_time", "modification_time", "change_time", "birth_time"})
    for (auto Number : {-1, 1000000000}) {
      auto Changed = Original;
      (*Changed.getAsObject()->getObject(Time))["nanoseconds"] = Number;
      auto Bad = Parse(Changed);
      EXPECT_FALSE(bool(Bad));
      llvm::consumeError(Bad.takeError());
    }
}

TEST(ProcessReport, PreservesBinaryOutputRawRegisterBitsAndNullableStatus) {
  ProcessResult Result{ProcessProfile::LinuxELF64,
                       GuestArchitecture::AArch64,
                       ExecutionBackendKind::Unicorn,
                       {}};
  Result.Entry = UINT64_MAX;
  Result.StandardOutput = llvm::fromHex(BinaryHex);
  Result.Stop = ProcessStopReason::CPUFailure;
  ProcessServiceEvent Event{UINT64_MAX, UINT64_MAX, {}, UINT64_MAX};
  Event.Arguments.fill(UINT64_MAX);
  Result.Services.push_back(Event);
  Result.LastCPUExit = ExecutionExit{
      ExecutionExitKind::GuestFault,
      BackendFault{BackendFaultKind::Protection, UINT64_MAX, UINT64_MAX, 8,
                   BackendAccessKind::Write, std::nullopt},
      {}};
  auto Parsed = llvm::json::parse(processResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  auto *Object = Parsed->getAsObject();
  ASSERT_NE(Object, nullptr);
  EXPECT_TRUE(Object->get(field::ExitStatus)->getAsNull());
  EXPECT_EQ(Object->getString(field::Entry), AllBits);
  EXPECT_EQ(Object->getString(field::Stdout), BinaryHex);
  const auto *Service =
      Object->getArray(field::Services)->front().getAsObject();
  EXPECT_EQ(Service->getString(field::PC), AllBits);
  EXPECT_EQ(Service->getString(field::Number), AllBits);
  EXPECT_EQ(Service->getString(field::Result), AllBits);
  for (const auto &Argument : *Service->getArray(field::Arguments))
    EXPECT_EQ(Argument.getAsString(), AllBits);
  const auto *Exit = Object->getObject(field::CPUExit);
  EXPECT_EQ(Exit->getString(field::Kind), FaultExit);
  EXPECT_EQ(Exit->getObject(field::Fault)->getString(field::Address), AllBits);
  Result.ExitStatus = 0;
  Parsed = llvm::json::parse(processResultJSON(Result));
  ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
  EXPECT_EQ(Parsed->getAsObject()->getInteger(field::ExitStatus), 0);
}
TEST(ProcessReport, PreservesExplicitWindowsCatalogueAndRejectsOtherProfiles) {
  auto O = processOptionsFromJSON(WindowsModuleOptions);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Windows);
  ASSERT_EQ(O->Windows->Modules.size(), 1u);
  EXPECT_EQ(O->Windows->Modules.front().Name, WindowsModuleName);
  EXPECT_EQ(O->Windows->Modules.front().Path.generic_string(),
            WindowsModulePath);
  for (auto Profile :
       {ProcessProfile::LinuxELF64, ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess(WindowsModulePath, Profile, *O);
    EXPECT_FALSE(bool(R));
    if (!R)
      EXPECT_EQ(llvm::toString(R.takeError()), field::WindowsProfile);
  }
}
TEST(ProcessReport, WindowsPEBVersionRequiresEveryFieldAtItsNativeWidth) {
  for (
      const char *Text :
      {R"({"major":10,"minor":0,"build":19043,"platform":2})",
       R"({"major":4294967295,"minor":4294967295,"build":65535,"platform":4294967295})"}) {
    auto O = processOptionsFromJSON(
        std::string(R"({"windows":{"peb_version":)") + Text + "}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->Windows && O->Windows->PEBVersion);
    EXPECT_GE(O->Windows->PEBVersion->Build, 19043u);
  }
  for (const char *Bad :
       {"null", "[]", "{}", R"({"major":10,"minor":0,"build":19043})",
        R"({"major":10,"minor":0,"build":19043,"platform":2,"extra":0})",
        R"({"major":-1,"minor":0,"build":19043,"platform":2})",
        R"({"major":4294967296,"minor":0,"build":19043,"platform":2})",
        R"({"major":10,"minor":4294967296,"build":19043,"platform":2})",
        R"({"major":10,"minor":0,"build":65536,"platform":2})",
        R"({"major":10,"minor":0,"build":-1,"platform":2})",
        R"({"major":10,"minor":0,"build":1.5,"platform":2})",
        R"({"major":10,"minor":0,"build":true,"platform":2})",
        R"({"major":10,"minor":0,"build":19043,"platform":4294967296})"}) {
    auto O = processOptionsFromJSON(
        std::string(R"({"windows":{"peb_version":)") + Bad + "}}");
    EXPECT_FALSE(bool(O)) << Bad;
    llvm::consumeError(O.takeError());
  }
}
TEST(ProcessReport, DarwinUsageIsLosslessAndEachSnapshotIsIndependent) {
  auto Good = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                     darwin_test::ResourceUsageJSON + "}");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_TRUE(Good->DarwinSystem);
  ASSERT_TRUE(Good->DarwinSystem->ResourceUsageSelf);
  ASSERT_TRUE(Good->DarwinSystem->ResourceUsageChildren);
  const auto &Self = *Good->DarwinSystem->ResourceUsageSelf;
  const auto &Children = *Good->DarwinSystem->ResourceUsageChildren;
  EXPECT_EQ(Self.UserSeconds, INT64_MIN);
  EXPECT_EQ(Self.UserMicroseconds, 999999u);
  EXPECT_EQ(Self.SystemSeconds, 0x0123456789abcdefLL);
  EXPECT_EQ(Self.SystemMicroseconds, 123456u);
  EXPECT_EQ(Self.Counters[0], INT64_MAX);
  EXPECT_EQ(Self.Counters[1], INT64_MIN);
  EXPECT_EQ(Self.Counters[13], -14);
  EXPECT_EQ(Children.UserSeconds, INT64_MAX);
  EXPECT_EQ(Children.SystemSeconds, -3);
  EXPECT_EQ(Children.Counters[13], INT64_MAX);
  for (const char *Empty : {R"({"darwin_system":{}})",
                            R"({"darwin_system":{"resource_usage":{}}})"}) {
    auto Parsed = processOptionsFromJSON(Empty);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_FALSE(Parsed->DarwinSystem->ResourceUsageSelf);
    EXPECT_FALSE(Parsed->DarwinSystem->ResourceUsageChildren);
  }
  const std::string Zero = R"({"user_seconds":"0","user_microseconds":"0",
    "system_seconds":"0","system_microseconds":"0",
    "counters":[0,0,0,0,0,0,0,0,0,0,0,0,0,0]})";
  for (bool SelfOnly : {true, false}) {
    auto Parsed = processOptionsFromJSON(
        std::string("{\"darwin_system\":{\"resource_usage\":{\"") +
        (SelfOnly ? "self" : "children") + "\":" + Zero + "}}}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->DarwinSystem->ResourceUsageSelf.has_value(), SelfOnly);
    EXPECT_EQ(Parsed->DarwinSystem->ResourceUsageChildren.has_value(),
              !SelfOnly);
    const auto &Usage = SelfOnly ? *Parsed->DarwinSystem->ResourceUsageSelf
                                 : *Parsed->DarwinSystem->ResourceUsageChildren;
    EXPECT_EQ(Usage.UserSeconds, 0);
    EXPECT_EQ(Usage.SystemSeconds, 0);
    EXPECT_EQ(Usage.Counters, (std::array<int64_t, 14>{}));
  }
}

TEST(ProcessReport, MalformedDarwinUsageFailsBeforeImageLoading) {
  const std::string Zero = R"({"user_seconds":0,"user_microseconds":0,
    "system_seconds":0,"system_microseconds":0,
    "counters":[0,0,0,0,0,0,0,0,0,0,0,0,0,0]})";
  for (const char *Outer : {"null", "[]", "true", "0", R"({"self":null})",
                            R"({"self":{}})", R"({"children":false})"}) {
    auto Parsed = processOptionsFromJSON(
        std::string("{\"darwin_system\":{\"resource_usage\":") + Outer + "}}");
    EXPECT_FALSE(bool(Parsed)) << Outer;
    llvm::consumeError(Parsed.takeError());
  }
  auto Reject = [&](llvm::json::Value Snapshot, llvm::StringRef Key = "self") {
    llvm::json::Object System;
    System["resource_usage"] = llvm::json::Object{{Key, std::move(Snapshot)}};
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_system", std::move(System)}}))
            .str());
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  };
  auto Seed = [&] { return llvm::cantFail(llvm::json::parse(Zero)); };
  Reject(Seed(), "unknown");
  for (const char *Key : {"user_seconds", "user_microseconds", "system_seconds",
                          "system_microseconds", "counters"}) {
    auto Snapshot = Seed();
    Snapshot.getAsObject()->erase(Key);
    Reject(std::move(Snapshot));
  }
  auto Unknown = Seed();
  (*Unknown.getAsObject())["unknown"] = 0;
  Reject(std::move(Unknown));
  for (const char *Key : {"user_seconds", "system_seconds"}) {
    for (const char *Bad : {"null", "true", "0.5", "9223372036854775807",
                            R"("9223372036854775808")",
                            R"("-9223372036854775809")", R"("1.0")"}) {
      auto Snapshot = Seed();
      (*Snapshot.getAsObject())[Key] = llvm::cantFail(llvm::json::parse(Bad));
      Reject(std::move(Snapshot));
    }
  }
  for (const char *Key : {"user_microseconds", "system_microseconds"}) {
    for (const char *Bad :
         {"null", "true", "0.5", "-1", "1000000", R"("4294967296")"}) {
      auto Snapshot = Seed();
      (*Snapshot.getAsObject())[Key] = llvm::cantFail(llvm::json::parse(Bad));
      Reject(std::move(Snapshot));
    }
  }
  for (const char *Bad :
       {"null", "{}", "[]", "[0]", "[0,0,0,0,0,0,0,0,0,0,0,0,0]",
        "[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]", "[true,0,0,0,0,0,0,0,0,0,0,0,0,0]",
        "[0.5,0,0,0,0,0,0,0,0,0,0,0,0,0]",
        R"(["-9223372036854775809",0,0,0,0,0,0,0,0,0,0,0,0,0])"}) {
    auto Snapshot = Seed();
    (*Snapshot.getAsObject())["counters"] =
        llvm::cantFail(llvm::json::parse(Bad));
    Reject(std::move(Snapshot));
  }
}

TEST(ProcessReport, DarwinUsageRequiresDarwinAndTypedTimesFailBeforeLoading) {
  auto O = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                  darwin_test::ResourceUsageJSON + "}");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (bool Self : {true, false}) {
    auto Bad = *O;
    auto &Usage = Self ? *Bad.DarwinSystem->ResourceUsageSelf
                       : *Bad.DarwinSystem->ResourceUsageChildren;
    Usage.SystemMicroseconds = 1000000;
    for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                   ProcessProfile::IOSSimulatorMachO64}) {
      auto R = emulateProcess("missing.macho", P, Bad);
      ASSERT_FALSE(bool(R));
      EXPECT_EQ(llvm::toString(R.takeError()),
                "Darwin resource usage microseconds must be below 1000000");
    }
  }
}

TEST(ProcessReport,
     DarwinResourceLimitsAreLosslessCanonicalAndDistinctFromMissing) {
  auto Good = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                     darwin_test::ResourceLimitsJSON + "}");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_TRUE(Good->DarwinSystem);
  const auto &Limits = Good->DarwinSystem->ResourceLimits;
  ASSERT_EQ(Limits.size(), 9u);
  EXPECT_EQ(Limits.at(0).Current, 0u);
  EXPECT_EQ(Limits.at(0).Maximum, 0u);
  EXPECT_EQ(Limits.at(1).Current, uint64_t(INT64_MAX));
  EXPECT_EQ(Limits.at(1).Maximum, uint64_t(INT64_MAX));
  EXPECT_EQ(Limits.at(2).Current, 0x0123456789abcdefULL);
  EXPECT_EQ(Limits.at(5).Current, uint64_t(INT64_MAX) - 1);
  EXPECT_EQ(Limits.at(8).Current, 256u);
  EXPECT_EQ(Limits.at(8).Maximum, 1024u);
  for (const char *Empty : {R"({"darwin_system":{}})",
                            R"({"darwin_system":{"resource_limits":[]}})"}) {
    auto Parsed = processOptionsFromJSON(Empty);
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_TRUE(Parsed->DarwinSystem);
    EXPECT_TRUE(Parsed->DarwinSystem->ResourceLimits.empty());
  }
  auto Text = processOptionsFromJSON(R"({"darwin_system":{"resource_limits":[
      {"resource":"8","current":"0","maximum":"9223372036854775807"}]}})");
  ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
  EXPECT_EQ(Text->DarwinSystem->ResourceLimits.at(8).Current, 0u);
  EXPECT_EQ(Text->DarwinSystem->ResourceLimits.at(8).Maximum,
            uint64_t(INT64_MAX));
}

TEST(ProcessReport, MalformedDarwinResourceLimitsFailBeforeImageLoading) {
  for (
      const char *Bad :
      {"null",
       "{}",
       "true",
       "0",
       "[null]",
       "[{}]",
       R"([{"resource":0,"current":0}])",
       R"([{"resource":0,"current":0,"maximum":0,"unknown":0}])",
       R"([{"resource":0,"current":0,"unknown":0}])",
       R"([{"resource":0,"current":0,"maximum":0},{"resource":"0","current":0,"maximum":0}])",
       R"([{"resource":9,"current":0,"maximum":0}])",
       R"([{"resource":4096,"current":0,"maximum":0}])",
       R"([{"resource":"4294967296","current":0,"maximum":0}])",
       R"([{"resource":-1,"current":0,"maximum":0}])",
       R"([{"resource":0.5,"current":0,"maximum":0}])",
       R"([{"resource":true,"current":0,"maximum":0}])",
       R"([{"resource":0,"current":1,"maximum":0}])",
       R"([{"resource":0,"current":-1,"maximum":0}])",
       R"([{"resource":0,"current":0.5,"maximum":1}])",
       R"([{"resource":0,"current":true,"maximum":1}])",
       R"([{"resource":0,"current":0,"maximum":null}])",
       R"([{"resource":0,"current":0,"maximum":"9223372036854775808"}])",
       R"([{"resource":0,"current":0,"maximum":"18446744073709551615"}])",
       R"([{"resource":0,"current":0,"maximum":9223372036854775807}])",
       R"([{"resource":0,"current":0,"maximum":"1.0"}])"}) {
    auto Parsed = processOptionsFromJSON(
        std::string("{\"darwin_system\":{\"resource_limits\":") + Bad + "}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  auto TooMany = std::string("{\"darwin_system\":{\"resource_limits\":[");
  for (unsigned I = 0; I != 10; ++I) {
    if (I)
      TooMany += ',';
    TooMany +=
        "{\"resource\":" + std::to_string(I) + ",\"current\":0,\"maximum\":0}";
  }
  auto Parsed = processOptionsFromJSON(TooMany + "]}}");
  EXPECT_FALSE(bool(Parsed));
  llvm::consumeError(Parsed.takeError());
}

TEST(ProcessReport,
     ResourceLimitsRequireDarwinAndRejectTypedKeysBeforeLoading) {
  auto O = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                  darwin_test::ResourceLimitsJSON + "}");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  O->DarwinSystem->ResourceLimits[4096] = {0, 0};
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(
        llvm::toString(R.takeError()),
        "Darwin resource limits require canonical keys 0..8 and current <= "
        "maximum <= INT64_MAX");
  }
}

TEST(ProcessReport, DarwinCredentialsRetainDistinctIDsAndOptionalGroupOrder) {
  auto Good = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                     darwin_test::CredentialsJSON + "}");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  ASSERT_TRUE(Good->DarwinSystem->Credentials);
  const auto &C = *Good->DarwinSystem->Credentials;
  EXPECT_EQ(C.RealUID, 101u);
  EXPECT_EQ(C.EffectiveUID, 202u);
  EXPECT_EQ(C.RealGID, 303u);
  EXPECT_EQ(C.EffectiveGID, 404u);
  EXPECT_EQ(*C.GroupAccessList,
            (std::vector<uint32_t>{404, 0, INT32_MAX, 7, 7}));
  auto Missing = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Missing)) << llvm::toString(Missing.takeError());
  EXPECT_FALSE(Missing->DarwinSystem->Credentials);
  auto NoGroups = processOptionsFromJSON(R"({"darwin_system":{"credentials":{
      "real_uid":0,"effective_uid":0,"real_gid":0,"effective_gid":0}}})");
  ASSERT_TRUE(bool(NoGroups)) << llvm::toString(NoGroups.takeError());
  EXPECT_EQ(NoGroups->DarwinSystem->Credentials->EffectiveUID, 0u);
  EXPECT_FALSE(NoGroups->DarwinSystem->Credentials->GroupAccessList);
  auto Root = processOptionsFromJSON(R"({"darwin_system":{"credentials":{
      "real_uid":"0","effective_uid":"0","real_gid":"0",
      "effective_gid":"0","groups":["0",0]}}})");
  ASSERT_TRUE(bool(Root)) << llvm::toString(Root.takeError());
  EXPECT_EQ(*Root->DarwinSystem->Credentials->GroupAccessList,
            (std::vector<uint32_t>{0, 0}));
}

TEST(ProcessReport, DarwinMembershipUIDKeepsLosslessAndIndependentAdmission) {
  for (auto ID : {0u, uint32_t(INT32_MAX), 4294967195u})
    for (bool String : {false, true})
      for (bool Groups : {false, true}) {
        auto Input =
            llvm::cantFail(llvm::json::parse(darwin_test::CredentialsJSON));
        auto *C = Input.getAsObject()->getObject("credentials");
        (*C)["group_membership_uid"] =
            String ? llvm::json::Value(std::to_string(ID))
                   : llvm::json::Value(ID);
        if (!Groups)
          C->erase("groups");
        auto Parsed = processOptionsFromJSON(
            llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                     {"darwin_system", std::move(Input)}}))
                .str());
        ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
        const auto &Credential = *Parsed->DarwinSystem->Credentials;
        EXPECT_EQ(Credential.GroupMembershipUID, ID);
        EXPECT_EQ(Credential.RealUID, 101u);
        EXPECT_EQ(Credential.EffectiveUID, 202u);
        EXPECT_EQ(Credential.RealGID, 303u);
        EXPECT_EQ(Credential.EffectiveGID, 404u);
        EXPECT_EQ(bool(Credential.GroupAccessList), Groups);
      }
  auto Omitted = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                        darwin_test::CredentialsJSON + "}");
  ASSERT_TRUE(bool(Omitted));
  EXPECT_FALSE(Omitted->DarwinSystem->Credentials->GroupMembershipUID);
}
TEST(ProcessReport, MalformedDarwinMembershipUIDFailsBeforeImageLoading) {
  auto Reject = [](llvm::json::Value System) {
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_system", std::move(System)}}))
            .str());
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  };
  for (const char *Bad :
       {"null", "true", "[]", "{}", "0.5", "-1", "2147483648", "4294967194",
        "4294967196", "4294967295", "4294967296", R"("1.0")", R"("-1")",
        R"("0xFFFFFF9B")", R"("4294967296")", R"("4294967295")", R"("")",
        R"(" 0")"}) {
    auto S = llvm::cantFail(llvm::json::parse(darwin_test::CredentialsJSON));
    (*S.getAsObject()->getObject("credentials"))["group_membership_uid"] =
        llvm::cantFail(llvm::json::parse(Bad));
    Reject(std::move(S));
  }
  for (const auto &Name :
       {std::string("GroupMembershipUID"), std::string("groupMembershipUID"),
        std::string("gmuid"), std::string("group_membership_uid") + '\0'}) {
    auto S = llvm::cantFail(llvm::json::parse(darwin_test::CredentialsJSON));
    (*S.getAsObject()->getObject("credentials"))[Name] = 4294967195ULL;
    Reject(std::move(S));
  }
}
TEST(ProcessReport, MalformedDarwinCredentialsFailBeforeImageLoading) {
  auto Seed = [] {
    return llvm::cantFail(llvm::json::parse(darwin_test::CredentialsJSON));
  };
  auto Reject = [](llvm::json::Value System) {
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_system", std::move(System)}}))
            .str());
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  };
  for (const char *Bad : {"null", "[]", "true", "0", "{}"}) {
    auto S = Seed();
    (*S.getAsObject())["credentials"] = llvm::cantFail(llvm::json::parse(Bad));
    Reject(std::move(S));
  }
  for (const char *Key :
       {"real_uid", "effective_uid", "real_gid", "effective_gid"}) {
    auto S = Seed();
    S.getAsObject()->getObject("credentials")->erase(Key);
    Reject(std::move(S));
    for (const char *Bad : {"null", "true", "0.5", "-1", "2147483648",
                            R"("4294967296")", R"("1.0")"}) {
      S = Seed();
      (*S.getAsObject()->getObject("credentials"))[Key] =
          llvm::cantFail(llvm::json::parse(Bad));
      Reject(std::move(S));
    }
  }
  auto Unknown = Seed();
  (*Unknown.getAsObject()->getObject("credentials"))["unknown"] = 0;
  Reject(std::move(Unknown));
  Unknown = Seed();
  Unknown.getAsObject()->getObject("credentials")->erase("groups");
  (*Unknown.getAsObject()->getObject("credentials"))["unknown"] = 0;
  Reject(std::move(Unknown));
  for (const char *Bad :
       {"null", "{}", "true", "0", "[]", "[0]", "[404,true]", "[404,0.5]",
        "[404,-1]", "[404,2147483648]", R"([404,"4294967296"])",
        "[404,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0]"}) {
    auto S = Seed();
    (*S.getAsObject()->getObject("credentials"))["groups"] =
        llvm::cantFail(llvm::json::parse(Bad));
    Reject(std::move(S));
  }
}

TEST(ProcessReport, DarwinNiceKeepsEverySignedValueLosslessAndIndependent) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->DarwinSystem->ProcessNice);
  for (int Nice = -20; Nice <= 20; ++Nice) {
    for (bool String : {false, true}) {
      const auto Value =
          String ? "\"" + std::to_string(Nice) + "\"" : std::to_string(Nice);
      auto Parsed = processOptionsFromJSON(
          std::string(R"({"darwin_system":{"nice":)") + Value + "}}");
      ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
      EXPECT_EQ(Parsed->DarwinSystem->ProcessNice, Nice);
      EXPECT_FALSE(Parsed->DarwinSystem->Credentials);
      EXPECT_FALSE(Parsed->DarwinSystem->ProcessGroupID);
      EXPECT_FALSE(Parsed->DarwinSystem->SessionID);
      EXPECT_FALSE(Parsed->DarwinSystem->ProcessTainted);
      EXPECT_FALSE(Parsed->DarwinSystem->LoginNameBytes);
      EXPECT_FALSE(Parsed->DarwinSystem->CPUCount);
      EXPECT_TRUE(Parsed->DarwinSystem->ResourceLimits.empty());
    }
  }
  // Exact integer numeric JSON remains lossless through the existing helper.
  auto Exponent = processOptionsFromJSON(R"({"darwin_system":{"nice":1e1}})");
  ASSERT_TRUE(bool(Exponent));
  EXPECT_EQ(Exponent->DarwinSystem->ProcessNice, 10);
  for (const char *Bad : {"null",
                          "true",
                          "false",
                          "0.5",
                          "[]",
                          "{}",
                          "21",
                          "-21",
                          "2147483647",
                          "-2147483648",
                          "2147483648",
                          "-2147483649",
                          "18446744073709551615",
                          R"("")",
                          R"("+1")",
                          R"(" 0")",
                          R"("0 ")",
                          R"("1e1")",
                          R"("0.5")",
                          R"("0x0")",
                          R"("-21")",
                          R"("21")",
                          R"("-2147483649")",
                          R"("0\u0000")"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_system":{"nice":)") + Bad + "}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
  auto Extra =
      processOptionsFromJSON(R"({"darwin_system":{"nice":0,"ncie":0}})");
  EXPECT_FALSE(bool(Extra));
  llvm::consumeError(Extra.takeError());
}

TEST(ProcessReport, DarwinNiceAdmissionPrecedesImageLoading) {
  ProcessOptions O;
  O.DarwinSystem = darwin_test::priorityOptions();
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    for (int32_t Bad : {-21, 21, INT32_MIN, INT32_MAX}) {
      O.DarwinSystem->ProcessNice = Bad;
      auto R = emulateProcess("missing.macho", P, O);
      ASSERT_FALSE(bool(R));
      EXPECT_EQ(llvm::toString(R.takeError()),
                "Darwin system nice must be between -20 and 20");
    }
  }
}

TEST(ProcessReport,
     DarwinLoginBufferRequiresCompleteStrictHexAndKeepsZeroKnown) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->DarwinSystem->LoginNameBytes);
  auto Typed = darwin_test::loginBufferOptions();
  const std::string Payload = darwin_test::LoginNameHex;
  for (const std::string &Hex :
       {Payload, llvm::StringRef(Payload).upper(), std::string(510, '0')}) {
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"darwin_system",
                           llvm::json::Object{{"login_name_hex", Hex}}}}))
            .str());
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    ASSERT_TRUE(Parsed->DarwinSystem->LoginNameBytes);
    EXPECT_EQ(*Parsed->DarwinSystem->LoginNameBytes,
              Hex == std::string(510, '0') ? std::vector<uint8_t>(255, 0)
                                           : *Typed.LoginNameBytes);
    EXPECT_FALSE(Parsed->DarwinSystem->Credentials);
    EXPECT_FALSE(Parsed->DarwinSystem->ProcessGroupID);
    EXPECT_FALSE(Parsed->DarwinSystem->SessionID);
    EXPECT_FALSE(Parsed->DarwinSystem->ProcessTainted);
    EXPECT_FALSE(Parsed->DarwinSystem->HostName);
  }
  for (const char *Value :
       {"null", "false", "true", "0", "255", "[]", "{}", "[0]"}) {
    auto Bad = processOptionsFromJSON(
        std::string(R"({"darwin_system":{"login_name_hex":)") + Value + "}}");
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
  for (const auto &Hex :
       {std::string(), std::string(508, '0'), std::string(509, '0'),
        std::string(511, '0'), std::string(512, '0'),
        "0x" + std::string(508, '0'), std::string(509, '0') + "g",
        std::string(509, '0') + " ", std::string(509, '0') + '\0',
        std::string(508, '0') + "\xc3\xa9"}) {
    auto Bad = processOptionsFromJSON(
        llvm::formatv("{0}",
                      llvm::json::Value(llvm::json::Object{
                          {"darwin_system",
                           llvm::json::Object{{"login_name_hex", Hex}}}}))
            .str());
    EXPECT_FALSE(bool(Bad));
    llvm::consumeError(Bad.takeError());
  }
}

TEST(ProcessReport, DarwinLoginBufferAdmissionPrecedesImageLoading) {
  ProcessOptions O;
  O.DarwinSystem = darwin_test::loginBufferOptions();
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    for (size_t Size : {0u, 1u, 254u, 256u}) {
      O.DarwinSystem->LoginNameBytes->assign(Size, 0);
      auto R = emulateProcess("missing.macho", P, O);
      ASSERT_FALSE(bool(R));
      EXPECT_EQ(llvm::toString(R.takeError()),
                "Darwin system login_name_hex must encode exactly 255 bytes");
    }
  }
}

TEST(ProcessReport,
     DarwinProcessObservationsKeepIDsAndStrictBooleanIndependent) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->DarwinSystem->ProcessGroupID);
  EXPECT_FALSE(Empty->DarwinSystem->SessionID);
  EXPECT_FALSE(Empty->DarwinSystem->ProcessTainted);
  for (const char *Name : {"process_group_id", "session_id"}) {
    for (const auto &[Value, Expected] :
         {std::pair{"1", 1u},
          std::pair{R"("2147483647")", uint32_t(INT32_MAX)}}) {
      auto O = processOptionsFromJSON(std::string(R"({"darwin_system":{")") +
                                      Name + "\":" + Value + "}}");
      ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
      EXPECT_EQ(llvm::StringRef(Name) == "session_id"
                    ? O->DarwinSystem->SessionID
                    : O->DarwinSystem->ProcessGroupID,
                Expected);
      EXPECT_FALSE(llvm::StringRef(Name) == "session_id"
                       ? O->DarwinSystem->ProcessGroupID
                       : O->DarwinSystem->SessionID);
      EXPECT_FALSE(O->DarwinSystem->ProcessTainted);
      EXPECT_FALSE(O->DarwinSystem->Credentials);
    }
    for (const char *Bad :
         {"null", "true", "false", "0", "-1", "0.5", "[]", "{}", "2147483648",
          "4294967296", R"("4294967296")", R"("18446744073709551615")"}) {
      auto O = processOptionsFromJSON(std::string(R"({"darwin_system":{")") +
                                      Name + "\":" + Bad + "}}");
      EXPECT_FALSE(bool(O)) << Name << ' ' << Bad;
      llvm::consumeError(O.takeError());
    }
  }
  for (bool Tainted : {false, true}) {
    auto O = processOptionsFromJSON(
        std::string(R"({"darwin_system":{"process_tainted":)") +
        (Tainted ? "true" : "false") + "}}");
    ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
    ASSERT_TRUE(O->DarwinSystem->ProcessTainted.has_value());
    EXPECT_EQ(*O->DarwinSystem->ProcessTainted, Tainted);
    EXPECT_FALSE(O->DarwinSystem->ProcessGroupID);
    EXPECT_FALSE(O->DarwinSystem->SessionID);
    EXPECT_FALSE(O->DarwinSystem->Credentials);
  }
  for (const char *Bad :
       {"null", "0", "1", "-1", "0.0", R"("false")", R"("true")", "[]", "{}"}) {
    auto O = processOptionsFromJSON(
        std::string(R"({"darwin_system":{"process_tainted":)") + Bad + "}}");
    EXPECT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
  for (const char *Alias : {"pgid", "sid", "tainted", "processGroupID"}) {
    auto O = processOptionsFromJSON(std::string(R"({"darwin_system":{")") +
                                    Alias + "\":1}}");
    EXPECT_FALSE(bool(O));
    llvm::consumeError(O.takeError());
  }
}

TEST(ProcessReport, DarwinProcessObservationAdmissionPrecedesImageLoading) {
  auto O =
      processOptionsFromJSON(R"({"darwin_system":{"process_tainted":false}})");
  ASSERT_TRUE(bool(O));
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64})
    for (auto Member : {&DarwinSystemOptions::ProcessGroupID,
                        &DarwinSystemOptions::SessionID})
      for (uint32_t Bad : {0u, uint32_t(INT32_MAX) + 1, UINT32_MAX}) {
        O->DarwinSystem.emplace().*Member = Bad;
        auto R = emulateProcess("missing.macho", P, *O);
        ASSERT_FALSE(bool(R));
        EXPECT_EQ(
            llvm::toString(R.takeError()),
            "Darwin system process_group_id and session_id must be between "
            "1 and INT32_MAX");
      }
}

TEST(ProcessReport, DarwinHostNameKeepsMissingEmptyAndStrictByteBounds) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty));
  EXPECT_FALSE(Empty->DarwinSystem->HostName);
  std::string UTF8;
  for (unsigned I = 0; I != 127; ++I)
    UTF8 += "\xc3\xa9";
  for (const auto &Name : {std::string(), std::string(255, 'x'), UTF8 + "a"}) {
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_system",
                                  llvm::json::Object{{"hostname", Name}}}}))
            .str());
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->DarwinSystem->HostName, Name);
    EXPECT_FALSE(Parsed->DarwinSystem->Machine);
    EXPECT_FALSE(Parsed->DarwinSystem->Credentials);
  }
  for (const auto &Bad :
       {std::string(256, 'x'), UTF8 + "\xc3\xa9", std::string("x\0y", 3)}) {
    auto Parsed = processOptionsFromJSON(
        llvm::formatv("{0}", llvm::json::Value(llvm::json::Object{
                                 {"darwin_system",
                                  llvm::json::Object{{"hostname", Bad}}}}))
            .str());
    ASSERT_FALSE(bool(Parsed));
    EXPECT_EQ(llvm::toString(Parsed.takeError()),
              "Darwin system hostname must be at most 255 bytes without NUL");
  }
  for (const char *Bad : {"null", "true", "1", "[]", "{}"}) {
    auto Parsed = processOptionsFromJSON(
        std::string(R"({"darwin_system":{"hostname":)") + Bad + "}}");
    EXPECT_FALSE(bool(Parsed));
    llvm::consumeError(Parsed.takeError());
  }
  auto Alias = processOptionsFromJSON(R"({"darwin_system":{"host_name":"x"}})");
  EXPECT_FALSE(bool(Alias));
  llvm::consumeError(Alias.takeError());
}
TEST(ProcessReport, DarwinHostNameAdmissionPrecedesLoadingOnAllProfiles) {
  auto O = processOptionsFromJSON(R"({"darwin_system":{"hostname":""}})");
  ASSERT_TRUE(bool(O));
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    O->DarwinSystem->HostName = std::string(256, 'x');
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()),
              "Darwin system hostname must be at most 255 bytes without NUL");
  }
}
TEST(ProcessReport, DarwinDescriptorCapKeepsMissingZeroAndIntegerBounds) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_FALSE(Empty->DarwinSystem->MaxFilesPerProcess);
  for (const auto &[Literal, Expected] :
       {std::pair{"0", 0u}, std::pair{"1", 1u},
        std::pair{R"("2147483647")", uint32_t(INT32_MAX)}}) {
    auto Parsed = processOptionsFromJSON(
        std::string("{\"darwin_system\":{\"max_files_per_process\":") +
        Literal + "}}");
    ASSERT_TRUE(bool(Parsed)) << llvm::toString(Parsed.takeError());
    EXPECT_EQ(Parsed->DarwinSystem->MaxFilesPerProcess, Expected);
    EXPECT_TRUE(Parsed->DarwinSystem->ResourceLimits.empty());
  }
  for (const char *Bad : {"null", "true", "[]", "{}", "-1", "0.5", "2147483648",
                          "4294967295", R"("4294967296")", R"("1.0")"}) {
    auto Parsed = processOptionsFromJSON(
        std::string("{\"darwin_system\":{\"max_files_per_process\":") + Bad +
        "}}");
    EXPECT_FALSE(bool(Parsed)) << Bad;
    llvm::consumeError(Parsed.takeError());
  }
}

TEST(ProcessReport, DarwinDescriptorCapAdmissionPrecedesImageLoading) {
  auto O = processOptionsFromJSON(
      R"({"darwin_system":{"max_files_per_process":0}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto Bad : {0x80000000u, UINT32_MAX}) {
    O->DarwinSystem->MaxFilesPerProcess = Bad;
    for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                   ProcessProfile::IOSSimulatorMachO64}) {
      auto R = emulateProcess("missing.macho", P, *O);
      ASSERT_FALSE(bool(R));
      EXPECT_EQ(llvm::toString(R.takeError()),
                "Darwin system max_files_per_process must be between 0 and "
                "INT32_MAX");
    }
  }
}

TEST(ProcessReport, CredentialsRequireDarwinAndTypedAdmissionBeforeLoading) {
  auto O = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                  darwin_test::CredentialsJSON + "}");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (bool Groups : {false, true}) {
    auto Bad = *O;
    if (Groups)
      Bad.DarwinSystem->Credentials->GroupAccessList->front() = 0;
    else
      Bad.DarwinSystem->Credentials->RealUID = 0x80000000u;
    for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                   ProcessProfile::IOSSimulatorMachO64}) {
      auto R = emulateProcess("missing.macho", P, Bad);
      ASSERT_FALSE(bool(R));
      EXPECT_EQ(
          llvm::toString(R.takeError()),
          "Darwin credentials require bounded IDs and coherent 1..16 groups");
    }
  }
}

TEST(ProcessReport, DarwinSystemInputsAreLosslessAndRequireDarwinProfiles) {
  auto O = processOptionsFromJSON(std::string("{\"darwin_system\":") +
                                  darwin_test::SystemJSON + "}");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->DarwinSystem);
  const auto &S = *O->DarwinSystem;
  EXPECT_EQ(S.OSType, "Darwin");
  EXPECT_EQ(S.OSRelease, "24.test");
  EXPECT_EQ(S.OSRevision, INT32_MIN);
  EXPECT_EQ(S.OSVersion, "V42");
  EXPECT_EQ(S.KernelVersion, "NeverD virtual kernel");
  EXPECT_EQ(S.Machine, "virtual64");
  EXPECT_EQ(S.Model, "VirtualModel");
  EXPECT_EQ(S.CPUCount, 7u);
  EXPECT_EQ(S.MemorySize, 0xfedcba9876543210ULL);
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinSystemProfile);
  }
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    O->DarwinSystem->CPUCount = 0;
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("cpu_count"),
              std::string::npos);
    O->DarwinSystem->CPUCount.reset();
    O->DarwinSystem->Model = std::string("x\0y", 3);
    R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("strings"), std::string::npos);
    O->DarwinSystem->Model.reset();
  }
}

TEST(ProcessReport, DarwinSystemMissingEmptyZeroAndLimitsStayDistinct) {
  auto Empty = processOptionsFromJSON(R"({"darwin_system":{}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->DarwinSystem);
  EXPECT_FALSE(Empty->DarwinSystem->OSType);
  EXPECT_FALSE(Empty->DarwinSystem->CPUCount);
  EXPECT_FALSE(Empty->DarwinSystem->MemorySize);
  auto Zero = processOptionsFromJSON(R"({"darwin_system":{
      "os_type":"","os_revision":0,"memory_size":0,"cpu_count":1}})");
  ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
  EXPECT_EQ(Zero->DarwinSystem->OSType, "");
  EXPECT_EQ(Zero->DarwinSystem->OSRevision, 0);
  EXPECT_EQ(Zero->DarwinSystem->MemorySize, 0u);
  EXPECT_EQ(Zero->DarwinSystem->CPUCount, 1u);
  auto Limits = processOptionsFromJSON(R"({"darwin_system":{
      "os_revision":"2147483647","memory_size":"18446744073709551615",
      "cpu_count":"2147483647"}})");
  ASSERT_TRUE(bool(Limits)) << llvm::toString(Limits.takeError());
  EXPECT_EQ(Limits->DarwinSystem->OSRevision, INT32_MAX);
  EXPECT_EQ(Limits->DarwinSystem->MemorySize, UINT64_MAX);
  EXPECT_EQ(Limits->DarwinSystem->CPUCount, INT32_MAX);
}

TEST(ProcessReport, MalformedDarwinSystemFailsBeforeExecution) {
  for (const char *Bad : {"null",
                          "[]",
                          "true",
                          R"({"unknown":0})",
                          R"({"os_type":null})",
                          R"({"os_type":3})",
                          R"({"os_type":true})",
                          R"({"os_release":"x\u0000y"})",
                          R"({"os_revision":null})",
                          R"({"os_revision":-2147483649})",
                          R"({"os_revision":2147483648})",
                          R"({"os_revision":1.5})",
                          R"({"os_revision":true})",
                          R"({"cpu_count":0})",
                          R"({"cpu_count":-1})",
                          R"({"cpu_count":2147483648})",
                          R"({"cpu_count":null})",
                          R"({"cpu_count":true})",
                          R"({"cpu_count":1.5})",
                          R"({"memory_size":-1})",
                          R"({"memory_size":9007199254740992})",
                          R"({"memory_size":"18446744073709551616"})",
                          R"({"memory_size":true})",
                          R"({"memory_size":"0x1"})",
                          R"({"memory_size":null})",
                          R"({"memory_size":1.5})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"darwin_system\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (const char *Name : {"os_type", "os_release", "os_version",
                           "kernel_version", "machine", "model"}) {
    for (unsigned Size : {1023, 1024}) {
      auto R =
          processOptionsFromJSON(std::string("{\"darwin_system\":{\"") + Name +
                                 "\":\"" + std::string(Size, 'x') + "\"}}");
      EXPECT_EQ(bool(R), Size == 1023);
      llvm::consumeError(R.takeError());
    }
  }
}

TEST(ProcessReport, DarwinTimeInputIsLosslessAndRestrictedToDarwinProfiles) {
  auto O = processOptionsFromJSON(std::string("{\"darwin_time\":") +
                                  darwin_test::TimeJSON + "}");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->DarwinTime);
  EXPECT_EQ(O->DarwinTime->TimeOfDay->Seconds, 0xf1234567u);
  EXPECT_EQ(O->DarwinTime->TimeOfDay->Microseconds, 654321u);
  EXPECT_EQ(O->DarwinTime->Timezone->MinutesWest, -480);
  EXPECT_EQ(O->DarwinTime->Timezone->DSTTime, -1);
  EXPECT_EQ(O->DarwinTime->MachAbsoluteTime, 0xfedcba9876543210ULL);
  EXPECT_EQ(O->DarwinTime->MachContinuousTime, UINT64_MAX);
  ASSERT_TRUE(O->DarwinTime->Timebase);
  EXPECT_EQ(O->DarwinTime->Timebase->Numerator, 0xf1234567u);
  EXPECT_EQ(O->DarwinTime->Timebase->Denominator, 0xe2345679u);
  for (auto P : {ProcessProfile::WindowsPE64, ProcessProfile::LinuxELF64,
                 ProcessProfile::AndroidNativeAArch64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::DarwinTimeProfile);
  }
  O->DarwinTime->TimeOfDay->Microseconds = 1000000;
  for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                 ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.macho", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("microseconds"),
              std::string::npos);
  }
  O->DarwinTime->TimeOfDay.reset();
  for (auto Ratio : {DarwinTimebase{0, 1}, DarwinTimebase{1, 0}}) {
    O->DarwinTime->Timebase = Ratio;
    for (auto P : {ProcessProfile::MacOSMachO64, ProcessProfile::IOSMachO64,
                   ProcessProfile::IOSSimulatorMachO64}) {
      auto R = emulateProcess("missing.macho", P, *O);
      ASSERT_FALSE(bool(R));
      EXPECT_NE(llvm::toString(R.takeError()).find("timebase"),
                std::string::npos);
    }
  }
}

TEST(ProcessReport, DarwinTimeDistinguishesMissingZeroAndIntegerBoundaries) {
  auto Empty = processOptionsFromJSON(R"({"darwin_time":{}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  ASSERT_TRUE(Empty->DarwinTime);
  EXPECT_FALSE(Empty->DarwinTime->TimeOfDay);
  EXPECT_FALSE(Empty->DarwinTime->Timezone);
  EXPECT_FALSE(Empty->DarwinTime->MachAbsoluteTime);
  EXPECT_FALSE(Empty->DarwinTime->Timebase);
  EXPECT_FALSE(Empty->DarwinTime->MachContinuousTime);
  auto Zero = processOptionsFromJSON(R"({"darwin_time":{
    "time_of_day":{"seconds":0,"microseconds":0},
    "timezone":{"minutes_west":0,"dst_time":0},"mach_absolute_time":0,
    "mach_continuous_time":0}})");
  ASSERT_TRUE(bool(Zero)) << llvm::toString(Zero.takeError());
  EXPECT_EQ(Zero->DarwinTime->TimeOfDay->Seconds, 0u);
  EXPECT_EQ(Zero->DarwinTime->TimeOfDay->Microseconds, 0u);
  EXPECT_EQ(Zero->DarwinTime->Timezone->MinutesWest, 0);
  EXPECT_EQ(Zero->DarwinTime->Timezone->DSTTime, 0);
  ASSERT_TRUE(Zero->DarwinTime->MachAbsoluteTime);
  EXPECT_EQ(*Zero->DarwinTime->MachAbsoluteTime, 0u);
  ASSERT_TRUE(Zero->DarwinTime->MachContinuousTime);
  EXPECT_EQ(*Zero->DarwinTime->MachContinuousTime, 0u);
  auto Limits = processOptionsFromJSON(R"({"darwin_time":{
    "time_of_day":{"seconds":"4294967295","microseconds":999999},
    "timezone":{"minutes_west":-2147483648,"dst_time":2147483647},
    "mach_absolute_time":"18446744073709551615",
    "mach_continuous_time":"18446744073709551615",
    "timebase":{"numerator":"4294967295","denominator":4294967295}}})");
  ASSERT_TRUE(bool(Limits)) << llvm::toString(Limits.takeError());
  EXPECT_EQ(Limits->DarwinTime->TimeOfDay->Seconds, UINT32_MAX);
  EXPECT_EQ(Limits->DarwinTime->TimeOfDay->Microseconds, 999999u);
  EXPECT_EQ(Limits->DarwinTime->Timezone->MinutesWest, INT32_MIN);
  EXPECT_EQ(Limits->DarwinTime->Timezone->DSTTime, INT32_MAX);
  EXPECT_EQ(Limits->DarwinTime->MachAbsoluteTime, UINT64_MAX);
  EXPECT_EQ(Limits->DarwinTime->MachContinuousTime, UINT64_MAX);
  EXPECT_EQ(Limits->DarwinTime->Timebase->Numerator, UINT32_MAX);
  EXPECT_EQ(Limits->DarwinTime->Timebase->Denominator, UINT32_MAX);
}

TEST(ProcessReport, MalformedDarwinTimeIsRejectedBeforeExecution) {
  for (const char *Bad :
       {"null",
        "[]",
        "true",
        R"({"unknown":0})",
        R"({"time_of_day":null})",
        R"({"time_of_day":[]})",
        R"({"time_of_day":{}})",
        R"({"time_of_day":{"seconds":0}})",
        R"({"time_of_day":{"microseconds":0}})",
        R"({"time_of_day":{"seconds":0,"microseconds":0,"extra":0}})",
        R"({"time_of_day":{"seconds":-1,"microseconds":0}})",
        R"({"time_of_day":{"seconds":4294967296,"microseconds":0}})",
        R"({"time_of_day":{"seconds":0,"microseconds":-1}})",
        R"({"time_of_day":{"seconds":0,"microseconds":1000000}})",
        R"({"time_of_day":{"seconds":0,"microseconds":1.5}})",
        R"({"timezone":null})",
        R"({"timezone":{}})",
        R"({"timezone":{"minutes_west":0}})",
        R"({"timezone":{"minutes_west":0,"dst_time":0,"extra":0}})",
        R"({"timezone":{"minutes_west":-2147483649,"dst_time":0}})",
        R"({"timezone":{"minutes_west":0,"dst_time":2147483648}})",
        R"({"mach_absolute_time":null})",
        R"({"mach_absolute_time":true})",
        R"({"mach_absolute_time":-1})",
        R"({"mach_absolute_time":9007199254740992})",
        R"({"mach_absolute_time":"18446744073709551616"})",
        R"({"mach_absolute_time":"0x1"})",
        R"({"mach_absolute_time":1.5})",
        R"({"mach_continuous_time":null})",
        R"({"mach_continuous_time":true})",
        R"({"mach_continuous_time":-1})",
        R"({"mach_continuous_time":9007199254740992})",
        R"({"mach_continuous_time":"18446744073709551616"})",
        R"({"mach_continuous_time":"0x1"})",
        R"({"mach_continuous_time":1.5})",
        R"({"timebase":null})",
        R"({"timebase":[]})",
        R"({"timebase":{}})",
        R"({"timebase":{"numerator":1}})",
        R"({"timebase":{"denominator":1}})",
        R"({"timebase":{"numerator":1,"denominator":1,"extra":0}})",
        R"({"timebase":{"numerator":0,"denominator":1}})",
        R"({"timebase":{"numerator":1,"denominator":0}})",
        R"({"timebase":{"numerator":-1,"denominator":1}})",
        R"({"timebase":{"numerator":1,"denominator":-1}})",
        R"({"timebase":{"numerator":4294967296,"denominator":1}})",
        R"({"timebase":{"numerator":1,"denominator":"4294967296"}})",
        R"({"timebase":{"numerator":true,"denominator":1}})",
        R"({"timebase":{"numerator":1,"denominator":1.5}})",
        R"({"timebase":{"numerator":"0x1","denominator":1}})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"darwin_time\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(ProcessReport, LinuxClockInputIsLosslessAndRestrictedToLinuxProfiles) {
  auto O = processOptionsFromJSON(R"({"linux_time":{"clocks":[
    {"id":0,"seconds":"-9223372036854775808","nanoseconds":999999999},
    {"id":1,"seconds":"9223372036854775807","nanoseconds":0}],
    "timezone":{"minutes_west":-2147483648,"dst_time":2147483647}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxTime);
  EXPECT_EQ(O->LinuxTime->Clocks.at(0).Seconds, INT64_MIN);
  EXPECT_EQ(O->LinuxTime->Clocks.at(1).Seconds, INT64_MAX);
  EXPECT_EQ(O->LinuxTime->Clocks.at(0).Nanoseconds, 999999999);
  EXPECT_EQ(O->LinuxTime->Timezone->MinutesWest, INT32_MIN);
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxTimeProfile);
  }
  for (auto Bad : {LinuxTimespec{1, -1}, LinuxTimespec{1, 1000000000}}) {
    O->LinuxTime->Clocks[0] = Bad;
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("nanoseconds"),
              std::string::npos);
  }
  O->LinuxTime->Clocks = {{10, {1, 0}}};
  auto R =
      emulateProcess("missing.elf", ProcessProfile::AndroidNativeAArch64, *O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("clock ID"), std::string::npos);
}
TEST(ProcessReport, MalformedClockInputsFailBeforeExecution) {
  for (
      const char *Bad :
      {R"({"advance_on_idle":1})",
       R"({"nanoseconds_per_instruction":true})",
       R"({"nanoseconds_per_instruction":-1})",
       R"({"nanoseconds_per_instruction":"18446744073709551616"})",
       R"({"nanoseconds_per_instruction":1,"clocks":[{"id":3,"seconds":0,"nanoseconds":0}]})",
       R"({"advance_on_idle":"true"})",
       R"({"advance_on_idle":null})",
       R"({"advance_on_idle":true,"clocks":[{"id":3,"seconds":0,"nanoseconds":0}]})",
       "null",
       "[]",
       "true",
       R"({"unknown":0})",
       R"({"clocks":{}})",
       R"({"clocks":[{"id":0,"seconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":0,"extra":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":0},{"id":0,"seconds":1,"nanoseconds":1}]})",
       R"({"clocks":[{"id":10,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":-3,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":4294967296,"seconds":0,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":-1}]})",
       R"({"clocks":[{"id":0,"seconds":0,"nanoseconds":1000000000}]})",
       R"({"clocks":[{"id":0,"seconds":9007199254740992,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":1e99,"nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"9223372036854775808","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"-9223372036854775809","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":"0x1","nanoseconds":0}]})",
       R"({"clocks":[{"id":0,"seconds":1.5,"nanoseconds":0}]})",
       R"({"timezone":{"minutes_west":0}})",
       R"({"timezone":{"minutes_west":2147483648,"dst_time":0}})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_time\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(ProcessReport, AndroidSnapshotsRequireInitialMappingsUnlessExplicit) {
  auto O = processOptionsFromJSON(R"({"android":{"entry_symbol":"inspect",
    "read_memory":[{"address":4096,"size":64},
      {"address":8192,"size":64,"require_mapped_at_entry":false},
      {"address":12288,"size":64,"require_mapped_at_entry":true}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  const auto &Reads = O->Android->ReadMemory;
  ASSERT_EQ(Reads.size(), 3u);
  EXPECT_TRUE(Reads[0].RequireMappedAtEntry);
  EXPECT_FALSE(Reads[1].RequireMappedAtEntry);
  EXPECT_EQ(Reads[1].Address, 8192u);
  EXPECT_EQ(Reads[1].Size, 64u);
  EXPECT_TRUE(Reads[2].RequireMappedAtEntry);
  for (const char *Bad : {"null", "0", "\"false\"", "[]", "{}"}) {
    auto R = processOptionsFromJSON(
        std::string(R"({"android":{"entry_symbol":"inspect","read_memory":[
          {"address":4096,"size":64,"require_mapped_at_entry":)") +
        Bad + "}]}}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  auto R = processOptionsFromJSON(R"({"android":{"entry_symbol":"inspect",
    "memory":[{"address":4096,"size":4096,"require_mapped_at_entry":false}]}})");
  EXPECT_FALSE(bool(R));
  llvm::consumeError(R.takeError());
}

TEST(ProcessReport, ExplicitSignalActionsAreLosslessAndLinuxOnly) {
  auto O = processOptionsFromJSON(R"({"linux_signals":{"actions":[
    {"signal":11,"handler":"18446744073709551615","flags":"4294967296",
     "restorer":"9223372036854775808","mask":"9223372036854775809"}]}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxSignals);
  const auto &Action = O->LinuxSignals->Actions.at(11);
  EXPECT_EQ(Action.Handler, UINT64_MAX);
  EXPECT_EQ(Action.Flags, 0x100000000u);
  EXPECT_EQ(Action.Restorer, 0x8000000000000000);
  EXPECT_EQ(Action.Mask, 0x8000000000000001);
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxSignalsProfile);
  }
  for (const auto &[Signal, Invalid] :
       std::map<int32_t, LinuxSignalAction>{{0, {}},
                                            {65, {}},
                                            {-1, {}},
                                            {11, {0, 0, 0, 0x100}},
                                            {9, {1, 0, 0, 0}}}) {
    O->LinuxSignals->Actions = {{Signal, Invalid}};
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("signal disposition"),
              std::string::npos);
  }
  auto Empty = processOptionsFromJSON(R"({"linux_signals":{"actions":[]}})");
  ASSERT_TRUE(bool(Empty));
  ASSERT_TRUE(Empty->LinuxSignals);
  EXPECT_TRUE(Empty->LinuxSignals->Actions.empty());
}

TEST(ProcessReport, MalformedSignalActionsFailBeforeExecution) {
  for (
      const char *Bad :
      {"null", "[]", "true", "{}", R"({"actions":{}})",
       R"({"actions":[],"unknown":0})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0}]})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0,"restorer":0,"unknown":0}]})",
       R"({"actions":[{"signal":11,"handler":0,"flags":0,"mask":0,"restorer":0},{"signal":11,"handler":1,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":4294967296,"handler":0,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":-1,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":9007199254740992,"flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":"18446744073709551616","flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":"0x1","flags":0,"mask":0,"restorer":0}]})",
       R"({"actions":[{"signal":11,"handler":1.5,"flags":0,"mask":0,"restorer":0}]})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_signals\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(ProcessReport, MemoryFileBytesAreExplicitAndRestrictedToLinuxProfiles) {
  auto O = processOptionsFromJSON(R"({"linux_files":{"files":[
    {"path":"/fixture/data","bytes_hex":"00ff410a805A"},
    {"path":"/empty","bytes_hex":""}],"descriptor_limit":4}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->LinuxFiles);
  EXPECT_EQ(O->LinuxFiles->DescriptorLimit, 4u);
  EXPECT_EQ(O->LinuxFiles->Files.at("/fixture/data"),
            (std::vector<uint8_t>{0, 0xff, 0x41, 0x0a, 0x80, 0x5a}));
  EXPECT_TRUE(O->LinuxFiles->Files.at("/empty").empty());
  for (auto P :
       {ProcessProfile::WindowsPE64, ProcessProfile::MacOSMachO64,
        ProcessProfile::IOSMachO64, ProcessProfile::IOSSimulatorMachO64}) {
    auto R = emulateProcess("missing.elf", P, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_EQ(llvm::toString(R.takeError()), field::LinuxFilesProfile);
  }
  for (unsigned Mode = 0; Mode < 5; ++Mode) {
    SCOPED_TRACE(Mode);
    O->LinuxFiles.emplace();
    auto &F = *O->LinuxFiles;
    if (Mode == 0)
      F.DescriptorLimit = 2;
    if (Mode == 1)
      F.DescriptorLimit = 4097;
    if (Mode == 2)
      F.Files["/data"].resize(16 * 1024 * 1024);
    if (Mode == 3)
      F.Files["/relative/../bad"] = {};
    if (Mode == 4)
      for (unsigned I = 0; I < 257; ++I)
        F.Files["/file" + std::to_string(I)] = {};
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, *O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}

TEST(ProcessReport, MalformedMemoryFileCataloguesFailBeforeExecution) {
  for (
      const char *Bad :
      {"null",
       "[]",
       "{}",
       R"({"files":null})",
       R"({"files":[],"host":true})",
       R"({"files":[],"descriptor_limit":-1})",
       R"({"files":[],"descriptor_limit":2})",
       R"({"files":[],"descriptor_limit":4097})",
       R"({"files":[],"descriptor_limit":4294967296})",
       R"({"files":[],"descriptor_limit":3.5})",
       R"({"files":[{"path":"/x"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"g0"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"0"}]})",
       R"({"files":[{"path":"/x","bytes_hex":"","extra":1}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x","bytes_hex":""}]})",
       R"({"files":[{"path":"/x","bytes_hex":""},{"path":"/x/y","bytes_hex":""}]})",
       R"({"files":[{"path":"relative","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/../b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a//b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/./b","bytes_hex":""}]})",
       R"({"files":[{"path":"/a/","bytes_hex":""}]})",
       R"({"files":[{"path":"/","bytes_hex":""}]})",
       R"({"files":[{"path":"/a\u0000b","bytes_hex":""}]})"}) {
    SCOPED_TRACE(Bad);
    auto R =
        processOptionsFromJSON(std::string("{\"linux_files\":") + Bad + "}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  auto Empty = processOptionsFromJSON(R"({"linux_files":{"files":[]}})");
  ASSERT_TRUE(bool(Empty)) << llvm::toString(Empty.takeError());
  EXPECT_TRUE(Empty->LinuxFiles->Files.empty());
  EXPECT_EQ(Empty->LinuxFiles->DescriptorLimit, 256u);
}

TEST(ProcessReport, LinuxLinkObservationsAreBoundedAndShareTheFileNamespace) {
  auto Good = processOptionsFromJSON(R"({"linux_files":{"files":[],
    "symbolic_links":[{"path":"/links/raw","target_hex":"2f78ff79"}]}})");
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  EXPECT_EQ(Good->LinuxFiles->SymbolicLinks.at("/links/raw"),
            (std::vector<uint8_t>{'/', 'x', 0xff, 'y'}));
  for (
      const char *Links :
      {"null", "{}", "[null]", R"([{"path":"/x","target_hex":""}])",
       R"([{"path":"/x","target_hex":"6100"}])",
       R"([{"path":"/x","target_hex":"0"}])",
       R"([{"path":"/x","target_hex":"gg"}])",
       R"([{"path":"x","target_hex":"61"}])",
       R"([{"path":"/x/","target_hex":"61"}])",
       R"([{"path":"/x","target_hex":"61","extra":1}])",
       R"([{"path":"/x","target_hex":"61"},{"path":"/x","target_hex":"62"}])",
       R"([{"path":"/x","target_hex":"61"},{"path":"/x/y","target_hex":"62"}])",
       R"([{"path":"/fixture/data","target_hex":"61"}])",
       R"([{"path":"/fixture","target_hex":"61"}])",
       R"([{"path":"/fixture/data/child","target_hex":"61"}])"}) {
    SCOPED_TRACE(Links);
    auto R = processOptionsFromJSON(
        std::string(
            R"({"linux_files":{"files":[{"path":"/fixture/data","bytes_hex":""}],"symbolic_links":)") +
        Links + "}}");
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    auto O = *Good;
    auto &F = *O.LinuxFiles;
    if (Mode == 0)
      F.SymbolicLinks["relative"] = {'x'};
    if (Mode == 1)
      F.SymbolicLinks["/bad"] = {0};
    if (Mode == 2)
      F.SymbolicLinks["/bad"].resize(16 * 1024 * 1024, 'x');
    if (Mode == 3)
      for (unsigned I = 0; I < 256; ++I)
        F.Files["/file" + std::to_string(I)] = {};
    if (Mode == 4)
      F.Files["/links/raw/child"] = {};
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}

TEST(ProcessReport, LinuxDeviceTransactionsAreExplicitLosslessAndBounded) {
  const char *Device = R"({"path":"/fixture/control","access_mode":2,
    "open_errno":0,"ioctls":[{"request":4294967295,"result":-13,
      "argument":"18446744073709551615","memory":[
        {"offset":1,"pointer_offset":0,"expected_hex":"00fF",
         "output_hex":"8001"}]}]})";
  auto Parse = [](const std::string &D) {
    return processOptionsFromJSON(
        std::string(R"({"linux_files":{"files":[],"devices":[)") + D + "]}}");
  };
  auto O = Parse(Device);
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  const auto &D = O->LinuxFiles->Devices.at("/fixture/control");
  EXPECT_EQ(D.AccessMode, 2u);
  EXPECT_EQ(D.OpenErrno, 0u);
  ASSERT_EQ(D.Ioctls.size(), 1u);
  EXPECT_EQ(D.Ioctls[0].Request, UINT32_MAX);
  EXPECT_EQ(D.Ioctls[0].Result, -13);
  EXPECT_EQ(D.Ioctls[0].Argument, UINT64_MAX);
  ASSERT_EQ(D.Ioctls[0].Memory.size(), 1u);
  EXPECT_EQ(D.Ioctls[0].Memory[0].PointerOffset, 0u);
  EXPECT_EQ(D.Ioctls[0].Memory[0].Expected, (std::vector<uint8_t>{0, 255}));
  EXPECT_EQ(D.Ioctls[0].Memory[0].Output, (std::vector<uint8_t>{128, 1}));

  for (unsigned Mode = 0; Mode != 18; ++Mode) {
    SCOPED_TRACE(Mode);
    auto J = llvm::cantFail(llvm::json::parse(Device));
    auto &Object = *J.getAsObject();
    auto &Call = *Object.getArray("ioctls")->front().getAsObject();
    auto &Memory = *Call.getArray("memory")->front().getAsObject();
    switch (Mode) {
    case 0:
      Object["access_mode"] = 3;
      break;
    case 1:
      Object["open_errno"] = 4096;
      break;
    case 2:
      Object.erase("open_errno");
      break;
    case 3:
      Object["path"] = "relative";
      break;
    case 4:
      Object["extra"] = true;
      break;
    case 5:
      Call["request"] = uint64_t(1) << 32;
      break;
    case 6:
      Call["result"] = -4096;
      break;
    case 7:
      Call["argument"] = -1;
      break;
    case 8:
      Call["memory"] = nullptr;
      break;
    case 9:
      Call["extra"] = 0;
      break;
    case 10:
      Memory["offset"] = "18446744073709551615";
      break;
    case 11:
      Memory["pointer_offset"] = "18446744073709551608";
      break;
    case 12:
      Memory["expected_hex"] = "0";
      break;
    case 13:
      Memory["output_hex"] = "gg";
      break;
    case 14:
      Memory["expected_hex"] = "";
      Memory["output_hex"] = "";
      break;
    case 15:
      Memory["offset"] = -1;
      break;
    case 16:
      Memory["extra"] = 0;
      break;
    case 17:
      Call["result"] = 1.5;
      break;
    }
    auto R = Parse(llvm::formatv("{0}", J).str());
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Input = *O;
    auto &Files = *Input.LinuxFiles;
    auto &Device = Files.Devices.at("/fixture/control");
    if (Mode == 0)
      Files.Files["/fixture/control"] = {};
    if (Mode == 1)
      Files.SymbolicLinks["/fixture/control/child"] = {'x'};
    if (Mode == 2)
      Files.Devices["/fixture/control/child"] = {};
    if (Mode == 3)
      Device.Ioctls.resize(1025);
    if (Mode == 4)
      Device.Ioctls[0].Memory.resize(17);
    if (Mode == 5)
      Device.Ioctls[0].Memory[0].Output.resize(16 * 1024 * 1024);
    if (Mode == 6)
      Device.AccessMode = 3;
    if (Mode == 7)
      Device.OpenErrno = 4096;
    if (Mode == 8)
      Device.Ioctls[0].Result = -4096;
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, Input);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
  auto Duplicate = Parse(std::string(Device) + ',' + Device);
  EXPECT_FALSE(bool(Duplicate));
  llvm::consumeError(Duplicate.takeError());
}

TEST(ProcessReport, LinuxDeviceMappingBase64PreservesCompleteBinaryPages) {
  std::vector<uint8_t> Bytes(32768);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = uint8_t(I);
  const auto Encoded = llvm::encodeBase64(Bytes);
  auto Parse = [](llvm::json::Object Mapping) {
    llvm::json::Value V = llvm::json::Object{
        {"linux_files",
         llvm::json::Object{
             {"files", llvm::json::Array{}},
             {"devices", llvm::json::Array{llvm::json::Object{
                             {"path", "/fixture/pages"},
                             {"access_mode", 2},
                             {"open_errno", 0},
                             {"ioctls", llvm::json::Array{}},
                             {"read_only_mappings",
                              llvm::json::Array{std::move(Mapping)}}}}}}}};
    return processOptionsFromJSON(llvm::formatv("{0}", V).str());
  };
  auto Valid =
      Parse(llvm::json::Object{{"offset", 4096}, {"bytes_base64", Encoded}});
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  const auto &Mapping =
      Valid->LinuxFiles->Devices.at("/fixture/pages").ReadOnlyMappings;
  ASSERT_EQ(Mapping.size(), 1u);
  EXPECT_EQ(Mapping[0].Offset, 4096u);
  EXPECT_EQ(Mapping[0].Bytes, Bytes);
  for (size_t Size : {4096u, 8192u, 12288u}) {
    std::vector<uint8_t> Pages(Bytes.begin(), Bytes.begin() + Size);
    auto R = Parse(llvm::json::Object{
        {"offset", 0}, {"bytes_base64", llvm::encodeBase64(Pages)}});
    ASSERT_TRUE(bool(R)) << llvm::toString(R.takeError());
    EXPECT_EQ(
        R->LinuxFiles->Devices.at("/fixture/pages").ReadOnlyMappings[0].Bytes,
        Pages);
  }
  // The same bytes in hex exceed the unchanged transport limit.
  auto Hex = Parse(
      llvm::json::Object{{"offset", 4096}, {"bytes_hex", llvm::toHex(Bytes)}});
  ASSERT_FALSE(bool(Hex));
  EXPECT_NE(llvm::toString(Hex.takeError()).find("64 KiB"), std::string::npos);
  for (unsigned Mode = 0; Mode != 9; ++Mode) {
    SCOPED_TRACE(Mode);
    llvm::json::Object M{{"offset", 4096}, {"bytes_base64", Encoded}};
    if (Mode == 0)
      M["bytes_hex"] = "";
    if (Mode == 1)
      M["bytes_base64"] = nullptr;
    if (Mode == 2)
      M["bytes_base64"] = Encoded.substr(0, Encoded.size() - 1);
    if (Mode == 3)
      M["bytes_base64"] = " " + Encoded.substr(1);
    if (Mode == 4)
      M["bytes_base64"] = "-" + Encoded.substr(1);
    if (Mode == 5)
      M["bytes_base64"] = Encoded.substr(0, Encoded.size() - 2) + "9=";
    if (Mode == 6)
      M["bytes_base64"] = llvm::encodeBase64(std::vector<uint8_t>(4095));
    if (Mode == 7)
      M["offset"] = 1;
    if (Mode == 8)
      M["bytes_base64"] = "====";
    auto R = Parse(std::move(M));
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
}

TEST(ProcessReport, LinuxDeviceMappingsRequireCompleteDisjointBoundedPages) {
  auto Input = [] {
    return llvm::json::Object{
        {"path", "/fixture/control"},
        {"access_mode", 2},
        {"open_errno", 0},
        {"ioctls", llvm::json::Array{}},
        {"read_only_mappings",
         llvm::json::Array{llvm::json::Object{
             {"offset", "4096"}, {"bytes_hex", std::string(8192, 'a')}}}}};
  };
  auto Parse = [](llvm::json::Object Device) {
    llvm::json::Value V = llvm::json::Object{
        {"linux_files",
         llvm::json::Object{
             {"files", llvm::json::Array{}},
             {"devices", llvm::json::Array{std::move(Device)}}}}};
    return processOptionsFromJSON(llvm::formatv("{0}", V).str());
  };
  auto Valid = Parse(Input());
  ASSERT_TRUE(bool(Valid)) << llvm::toString(Valid.takeError());
  const auto &Mapping =
      Valid->LinuxFiles->Devices.at("/fixture/control").ReadOnlyMappings;
  ASSERT_EQ(Mapping.size(), 1u);
  EXPECT_EQ(Mapping[0].Offset, 4096u);
  EXPECT_EQ(Mapping[0].Bytes, std::vector<uint8_t>(4096, 0xaa));
  for (unsigned Mode = 0; Mode != 11; ++Mode) {
    SCOPED_TRACE(Mode);
    auto D = Input();
    auto &M = *D.getArray("read_only_mappings")->front().getAsObject();
    if (Mode == 0)
      M["offset"] = 1;
    if (Mode == 1)
      M["offset"] = "18446744073709547520";
    if (Mode == 2)
      M["bytes_hex"] = "";
    if (Mode == 3)
      M["bytes_hex"] = "aa";
    if (Mode == 4)
      M["bytes_hex"] = std::string(8192, 'g');
    if (Mode == 5)
      M["extra"] = true;
    if (Mode == 6)
      M.erase("offset");
    if (Mode == 7)
      D["read_only_mappings"] = nullptr;
    if (Mode == 8)
      D.getArray("read_only_mappings")->push_back(llvm::json::Object(M));
    if (Mode == 9) {
      llvm::json::Array TooMany;
      for (unsigned I = 0; I < 17; ++I)
        TooMany.push_back(llvm::json::Object(M));
      D["read_only_mappings"] = std::move(TooMany);
    }
    if (Mode == 10)
      M["offset"] = -4096;
    auto R = Parse(std::move(D));
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  for (unsigned Mode = 0; Mode != 5; ++Mode) {
    SCOPED_TRACE(Mode);
    auto O = *Valid;
    auto &M = O.LinuxFiles->Devices.at("/fixture/control").ReadOnlyMappings;
    if (Mode == 0)
      M[0].Bytes.resize(4095);
    if (Mode == 1)
      M.push_back(M.front());
    if (Mode == 2)
      M.resize(17);
    if (Mode == 3)
      M[0].Bytes.resize(16 * 1024 * 1024);
    if (Mode == 4)
      M[0].Offset = UINT64_MAX - 4095;
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}

TEST(ProcessReport, StatefulDevicesValidateProducerAndContextBoundaries) {
  auto Input = [] {
    return llvm::json::Object{
        {"path", "/fixture/control"},
        {"access_mode", 2},
        {"open_errno", 0},
        {"ioctls", llvm::json::Array{}},
        {"kgsl_contexts",
         llvm::json::Object{{"memstore_offset", 4096}, {"context_limit", 3}}},
        {"read_only_mappings",
         llvm::json::Array{llvm::json::Object{
             {"offset", 4096},
             {"bytes_hex", std::string(8192, 'a')},
             {"updates", llvm::json::Array{
                             llvm::json::Object{{"after_nanoseconds", "0"},
                                                {"offset", 4095},
                                                {"bytes_hex", "ab"}},
                             llvm::json::Object{
                                 {"after_nanoseconds", "18446744073709551615"},
                                 {"offset", 40},
                                 {"bytes_hex", "01000000"}}}}}}}};
  };
  auto Parse = [](llvm::json::Object D) {
    llvm::json::Value V = llvm::json::Object{
        {"linux_files",
         llvm::json::Object{{"files", llvm::json::Array{}},
                            {"devices", llvm::json::Array{std::move(D)}}}}};
    return processOptionsFromJSON(llvm::formatv("{0}", V).str());
  };
  auto Good = Parse(Input());
  ASSERT_TRUE(bool(Good)) << llvm::toString(Good.takeError());
  const auto &D = Good->LinuxFiles->Devices.at("/fixture/control");
  ASSERT_TRUE(D.KGSL);
  EXPECT_EQ(D.KGSL->ContextLimit, 3u);
  ASSERT_EQ(D.ReadOnlyMappings[0].Updates.size(), 2u);
  EXPECT_EQ(D.ReadOnlyMappings[0].Updates[1].AfterNanoseconds, UINT64_MAX);
  for (unsigned Mode = 0; Mode != 23; ++Mode) {
    SCOPED_TRACE(Mode);
    auto D = Input();
    auto &K = *D.getObject("kgsl_contexts");
    auto &M = *D.getArray("read_only_mappings")->front().getAsObject();
    auto &U = *M.getArray("updates")->front().getAsObject();
    if (Mode == 0)
      K["context_limit"] = 0;
    if (Mode == 1)
      K["context_limit"] = 103; // last slot exceeds the mapping
    if (Mode == 2)
      K["context_limit"] = 1025;
    if (Mode == 3)
      K["memstore_offset"] = 8192;
    if (Mode == 4)
      K["extra"] = true;
    if (Mode == 5)
      D["kgsl_contexts"] = nullptr;
    if (Mode == 6)
      U["offset"] = 4096;
    if (Mode == 7)
      U["bytes_hex"] = "";
    if (Mode == 8)
      U["bytes_hex"] = std::string(8194, 'a');
    if (Mode == 9)
      U["after_nanoseconds"] = -1;
    if (Mode == 10)
      U["after_nanoseconds"] = true;
    if (Mode == 11)
      U["extra"] = 0;
    if (Mode == 12)
      U.erase("bytes_hex");
    if (Mode == 13)
      M["updates"] = nullptr;
    if (Mode == 14) {
      U["after_nanoseconds"] = 1;
      M.getArray("updates")->back().getAsObject()->operator[](
          "after_nanoseconds") = 0;
    }
    if (Mode == 15) {
      D.getArray("ioctls")->push_back(
          llvm::json::Object{{"request", "3221752083"},
                             {"result", 0},
                             {"memory", llvm::json::Array{}}});
    }
    if (Mode == 16)
      U["offset"] = "18446744073709551615";
    if (Mode == 17) {
      llvm::json::Array Updates;
      for (unsigned I = 0; I != 1025; ++I)
        Updates.push_back(llvm::json::Object(U));
      M["updates"] = std::move(Updates);
    }
    if (Mode == 18)
      K["context_limit"] = true;
    if (Mode == 19)
      K["synchronous_a3xx"] = 1;
    if (Mode == 20)
      K["synchronous_a3xx"] = "true";
    if (Mode == 21)
      K["synchronous_a3xx"] = nullptr;
    if (Mode == 22)
      K["synchronous_a3xx"] = true; // conflicts with producer
    auto R = Parse(std::move(D));
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
  }
  // The C++ entry point uses the same validator as JSON.
  for (unsigned Mode = 0; Mode != 6; ++Mode) {
    SCOPED_TRACE(Mode);
    auto O = *Good;
    auto &D = O.LinuxFiles->Devices.at("/fixture/control");
    if (Mode == 0)
      D.KGSL->ContextLimit = 0;
    if (Mode == 1)
      D.ReadOnlyMappings[0].Updates[0].Bytes.clear();
    if (Mode == 2)
      D.ReadOnlyMappings[0].Updates[0].Offset = UINT64_MAX;
    if (Mode == 3)
      D.ReadOnlyMappings[0].Updates.resize(1025);
    if (Mode == 4)
      D.Ioctls.push_back({0xc00c0916, 0, {}, {}});
    if (Mode == 5)
      D.KGSL->SynchronousA3xx = true;
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
  auto Commands = Input();
  (*Commands.getObject("kgsl_contexts"))["synchronous_a3xx"] = true;
  (*Commands.getArray("read_only_mappings"))[0].getAsObject()->erase("updates");
  auto ValidCommands = Parse(std::move(Commands));
  ASSERT_TRUE(bool(ValidCommands)) << llvm::toString(ValidCommands.takeError());
  EXPECT_TRUE(ValidCommands->LinuxFiles->Devices.at("/fixture/control")
                  .KGSL->SynchronousA3xx);
  auto Objects = Input();
  (*Objects.getObject("kgsl_contexts"))["memory_objects"] = true;
  auto ValidObjects = Parse(std::move(Objects));
  ASSERT_TRUE(bool(ValidObjects)) << llvm::toString(ValidObjects.takeError());
  EXPECT_TRUE(ValidObjects->LinuxFiles->Devices.at("/fixture/control")
                  .KGSL->MemoryObjects);
  for (auto Value : {llvm::json::Value(1), llvm::json::Value("true"),
                     llvm::json::Value(nullptr)}) {
    auto Invalid = Input();
    (*Invalid.getObject("kgsl_contexts"))["memory_objects"] = std::move(Value);
    auto Parsed = Parse(std::move(Invalid));
    EXPECT_FALSE(bool(Parsed));
    if (!Parsed)
      llvm::consumeError(Parsed.takeError());
  }
  for (uint32_t Request : {0xc0300945u, 0xc0300947u, 0x40200946u, 0x40080921u,
                           0xc0300934u, 0xc0080935u, 0xc0480936u}) {
    auto O = *ValidObjects;
    O.LinuxFiles->Devices.at("/fixture/control")
        .Ioctls.push_back({Request, 0, {}, {}});
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
  auto O = *ValidObjects;
  O.LinuxFiles->Devices.at("/fixture/control").AccessMode = 0;
  auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, O);
  ASSERT_FALSE(bool(R));
  EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
            std::string::npos);
}

llvm::json::Value fileMetadata() {
  return llvm::cantFail(llvm::json::parse(R"({
    "device":4294967295,"inode":"18446744073709551615","mode":33279,
    "link_count":4294967295,"uid":4294967295,"gid":4294967295,
    "size":"9223372036854775807","block_size":2147483647,
    "blocks":"9223372036854775807",
    "access_time":{"seconds":"-9223372036854775808","nanoseconds":0},
    "modification_time":{"seconds":"9223372036854775807","nanoseconds":999999999},
    "change_time":{"seconds":-1,"nanoseconds":1}})"));
}
llvm::Expected<ProcessOptions> fileOptions(llvm::json::Value Metadata) {
  llvm::json::Object File{{"path", "/fixture/data"},
                          {"bytes_hex", "00ff"},
                          {"metadata", std::move(Metadata)}};
  llvm::json::Object Options{
      {"linux_files",
       llvm::json::Object{{"files", llvm::json::Array{std::move(File)}}}}};
  return processOptionsFromJSON(
      llvm::formatv("{0}", llvm::json::Value(std::move(Options))).str());
}

TEST(ProcessReport, FileMetadataPreservesFullWidthObservations) {
  auto O = fileOptions(fileMetadata());
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  const auto &M = O->LinuxFiles->Metadata.at("/fixture/data");
  EXPECT_EQ(M.Device, UINT32_MAX);
  EXPECT_EQ(M.Inode, UINT64_MAX);
  EXPECT_EQ(M.Mode, 0100777u);
  EXPECT_EQ(M.LinkCount, UINT32_MAX);
  EXPECT_EQ(M.UID, UINT32_MAX);
  EXPECT_EQ(M.GID, UINT32_MAX);
  EXPECT_EQ(M.Size, uint64_t(INT64_MAX));
  EXPECT_EQ(M.BlockSize, uint32_t(INT32_MAX));
  EXPECT_EQ(M.Blocks, uint64_t(INT64_MAX));
  EXPECT_EQ(M.AccessTime.Seconds, INT64_MIN);
  EXPECT_EQ(M.AccessTime.Nanoseconds, 0);
  EXPECT_EQ(M.ModificationTime.Seconds, INT64_MAX);
  EXPECT_EQ(M.ModificationTime.Nanoseconds, 999999999);
  EXPECT_EQ(M.ChangeTime.Seconds, -1);
  EXPECT_EQ(M.ChangeTime.Nanoseconds, 1);
}

TEST(ProcessReport, IncompleteAndMalformedFileMetadataFailsBeforeExecution) {
  auto Reject = [](llvm::json::Value Value) {
    auto O = fileOptions(std::move(Value));
    EXPECT_FALSE(bool(O));
    EXPECT_NE(llvm::toString(O.takeError()).find("linux_files"),
              std::string::npos);
  };
  for (const char *Text : {"null", "[]", "{}", "false", "0"})
    Reject(llvm::cantFail(llvm::json::parse(Text)));
  auto Good = fileMetadata();
  for (const auto &[Name, Value] : *Good.getAsObject()) {
    SCOPED_TRACE(Name.str());
    auto Missing = Good;
    Missing.getAsObject()->erase(Name);
    Reject(std::move(Missing));
    auto Bad = Good;
    (*Bad.getAsObject())[Name] = nullptr;
    Reject(std::move(Bad));
  }
  auto Extra = Good;
  (*Extra.getAsObject())["unknown"] = 0;
  Reject(std::move(Extra));
  for (const char *Name : {"device", "inode", "mode", "link_count", "uid",
                           "gid", "size", "block_size", "blocks"}) {
    for (const char *Text :
         {"-1", "1.5", "true", "\"\"", "\"+1\"", "\"-1\"", "\" 1\"", "\"0x1\"",
          "\"18446744073709551616\"", "9007199254740992"}) {
      SCOPED_TRACE(testing::Message() << Name << ':' << Text);
      auto Bad = Good;
      (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
      Reject(std::move(Bad));
    }
  }
  for (const auto &[Name, Text] : {std::pair{"device", "4294967296"},
                                   {"link_count", "4294967296"},
                                   {"uid", "4294967296"},
                                   {"gid", "4294967296"},
                                   {"mode", "16877"},
                                   {"mode", "98304"},
                                   {"block_size", "2147483648"},
                                   {"size", "\"9223372036854775808\""},
                                   {"blocks", "\"9223372036854775808\""}}) {
    SCOPED_TRACE(testing::Message() << Name << ':' << Text);
    auto Bad = Good;
    (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
    Reject(std::move(Bad));
  }
  for (const char *Name : {"access_time", "modification_time", "change_time"})
    for (const char *Text :
         {R"({"seconds":0})", R"({"seconds":0,"nanoseconds":0,"extra":0})",
          R"({"seconds":0,"nanoseconds":-1})",
          R"({"seconds":0,"nanoseconds":1000000000})",
          R"({"seconds":"9223372036854775808","nanoseconds":0})",
          R"({"seconds":"-9223372036854775809","nanoseconds":0})",
          R"({"seconds":1.5,"nanoseconds":0})",
          R"({"seconds":9007199254740992,"nanoseconds":0})"}) {
      SCOPED_TRACE(testing::Message() << Name << ':' << Text);
      auto Bad = Good;
      (*Bad.getAsObject())[Name] = llvm::cantFail(llvm::json::parse(Text));
      Reject(std::move(Bad));
    }
}

TEST(ProcessReport, DirectFileMetadataUsesTheSameValidationBoundary) {
  for (unsigned Mode = 0; Mode < 12; ++Mode) {
    SCOPED_TRACE(Mode);
    ProcessOptions Options;
    Options.LinuxFiles.emplace();
    auto &Files = *Options.LinuxFiles;
    Files.Files["/fixture/data"] = {1};
    auto &M = Files.Metadata[Mode ? "/fixture/data" : "/absent"];
    M = fileTestMetadata();
    if (Mode == 1)
      M.Mode = 0040755;
    if (Mode == 2)
      M.Mode |= 0x10000;
    if (Mode == 3)
      M.Size = UINT64_MAX;
    if (Mode == 4)
      M.Blocks = UINT64_MAX;
    if (Mode == 5)
      M.BlockSize = uint32_t(INT32_MAX) + 1;
    if (Mode >= 6) {
      LinuxTimespec *Times[] = {&M.AccessTime, &M.ModificationTime,
                                &M.ChangeTime};
      Times[(Mode - 6) / 2]->Nanoseconds = Mode % 2 ? 1000000000 : -1;
    }
    auto R = emulateProcess("missing.elf", ProcessProfile::LinuxELF64, Options);
    ASSERT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("linux_files"),
              std::string::npos);
  }
}
} // namespace
} // namespace neverd::emulation
