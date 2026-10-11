//===- ProcessReportTests.cpp - Lossless bounded process wire contract ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "DarwinFileTestData.h"
#include "DarwinSystemTestData.h"
#include "DarwinTimeTestData.h"
#include "LinuxFileTestMetadata.h"
#include "gtest/gtest.h"

#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessReportFields.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/JSON.h"

namespace neverd::emulation {
namespace {
namespace field = process_report;
#define NEVERD_PROCESS_TEST_TEXT(Name, Text) constexpr char Name[] = Text;
#include "ProcessReportCases.def"
#undef NEVERD_PROCESS_TEST_TEXT
#define NEVERD_FAULT_REPORT_TEXT(Name, Text) constexpr char Name[] = Text;
#include "FaultReportCases.def"
#undef NEVERD_FAULT_REPORT_TEXT

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
