//===- AndroidNativeTests.cpp - Authored Android shared library workloads ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "gtest/gtest.h"

#include "neverd/emulation/ExecutionConfiguration.h"
#include "neverd/emulation/ProcessReport.h"
#include "neverd/emulation/ProcessSession.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/loader/ELF/ELFProgramLinking.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

namespace neverd::emulation {
namespace {
constexpr uint64_t Buffer = 0x20000000;
class AndroidNative : public testing::TestWithParam<const char *> {
protected:
  std::filesystem::path Path;
  ProcessOptions Options;
  void SetUp() override {
#ifndef NEVERD_ANDROID_FIXTURE_DIR
    GTEST_SKIP() << "Clang and ld.lld Android fixtures unavailable";
#else
    Path = std::filesystem::path(NEVERD_ANDROID_FIXTURE_DIR) /
           (std::string(GetParam()) + ".so");
    Options.Backend = ExecutionBackendKind::Unicorn;
    ExecutionConfiguration Configuration;
    Configuration.Backend = Options.Backend;
    Configuration.Architecture = GuestArchitecture::AArch64;
    Configuration.Contract = ExecutionContract::CheckedUserAArch64;
    auto Probe = probeExecutionBackend(Configuration);
    ASSERT_TRUE(bool(Probe)) << llvm::toString(Probe.takeError());
    if (Probe->Availability != BackendAvailability::Available)
      GTEST_SKIP() << Probe->Reason;
    Options.Android.emplace();
    Options.Android->TraceLimit = 4096;
    Options.Android->Memory.push_back({Buffer, 4096, {}, false});
    Options.Android->ReadMemory.push_back({Buffer, 64});
    Options.InstructionQuantum = 3;
#endif
  }
  ProcessResult run(llvm::StringRef Entry,
                    std::vector<uint64_t> Arguments = {}) {
    Options.Android->EntrySymbol = Entry.str();
    Options.Android->Arguments = std::move(Arguments);
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    if (!R) {
      ADD_FAILURE() << llvm::toString(R.takeError());
      return {ProcessProfile::AndroidNativeAArch64,
              GuestArchitecture::AArch64,
              Options.Backend,
              {}};
    }
    return std::move(*R);
  }
  void returned(const ProcessResult &Result, uint64_t Value) {
    ASSERT_EQ(Result.Stop, ProcessStopReason::Returned) << Result.Diagnostic;
    EXPECT_EQ(Result.ReturnValue, Value);
    EXPECT_FALSE(Result.ExitStatus);
    EXPECT_EQ(Result.Trace.size(), Result.Instructions);
    EXPECT_FALSE(Result.TraceTruncated);
  }
  std::vector<uint8_t> original() {
    auto B = llvm::MemoryBuffer::getFile(Path.string());
    EXPECT_TRUE(bool(B));
    llvm::StringRef Bytes = (*B)->getBuffer();
    return {Bytes.bytes_begin(), Bytes.bytes_end()};
  }
  template <class F> void temporary(llvm::ArrayRef<uint8_t> Bytes, F Callback) {
    int FD;
    llvm::SmallString<128> File;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-android", "so", FD, File));
    auto Cleanup = llvm::make_scope_exit([&] { llvm::sys::fs::remove(File); });
    {
      llvm::raw_fd_ostream OS(FD, true);
      OS.write(reinterpret_cast<const char *>(Bytes.data()), Bytes.size());
    }
    auto Saved = Path;
    Path = File.str().str();
    Callback();
    Path = Saved;
  }
};
TEST_P(AndroidNative, ConstructorsStackArgumentsAndUninitializedAnalysis) {
  returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 402);
  Options.Android->Initialize = false;
  returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 390);
}
TEST_P(AndroidNative, PropertiesMemoryAndTLS) {
  Options.Android->Properties["test.device"] = "sample";
  auto R = run("properties", {Buffer});
  returned(R, 28);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(std::string(R.MemorySnapshots[0].Bytes.begin(),
                        R.MemorySnapshots[0].Bytes.begin() + 7),
            std::string("sample\0", 7));
  EXPECT_GE(R.NativeCalls.size(), 5);
  returned(run("tls_slots"), 1);
  R = run("absent_property", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[0], 0);
}
TEST_P(AndroidNative, KernelErrorsAndLibcErrnoHaveDifferentContracts) {
  returned(run("libc_error"), 0);
  auto R = run("raw_error");
  returned(R, 0);
  ASSERT_EQ(R.Services.size(), 1);
  EXPECT_EQ(R.Services[0].Result, uint64_t(0) - 9);
}
TEST_P(AndroidNative, AllocationsPreserveBytesAndReportExhaustion) {
  auto R = run("allocation", {Buffer});
  returned(R, 0);
  ASSERT_EQ(R.MemorySnapshots.size(), 1);
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[4], 'Z');
  EXPECT_EQ(R.MemorySnapshots[0].Bytes[31], 'A');
}
TEST_P(AndroidNative, UnknownImportsAndForgedTrapsFailExplicitly) {
  auto R = run("unknown_call");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService) << R.Diagnostic;
  EXPECT_NE(R.Diagnostic.find("unknown_native_function"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("forged_trap");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("SVC"), std::string::npos);
}
TEST_P(AndroidNative, InvalidPointersAndStackCorruptionAreNotSuccess) {
  for (const char *Entry : {"bad_pointer", "bad_free", "stack_failure"}) {
    SCOPED_TRACE(Entry);
    auto R = run(Entry);
    EXPECT_EQ(R.Stop, ProcessStopReason::RuntimeFailure) << R.Diagnostic;
    EXPECT_FALSE(R.ReturnValue);
  }
}
TEST_P(AndroidNative, OpaqueStdioNeverExposesInventedStructureContents) {
  returned(run("stdio_identity"), 1);
  auto R = run("stdio_content");
  EXPECT_EQ(R.Stop, ProcessStopReason::CPUFailure) << R.Diagnostic;
  EXPECT_FALSE(R.ReturnValue);
}
TEST_P(AndroidNative, CapturesBinaryOutputAndEnforcesOutputLimit) {
  auto R = run("print_bytes");
  returned(R, 4);
  EXPECT_EQ(R.StandardOutput, std::string("A\0\xff\n", 4));
  Options.Android->TraceLimit = 0;
  Options.Android->ReadMemory.clear();
  Options.OutputLimit = 3;
  R = run("print_bytes");
  EXPECT_EQ(R.Stop, ProcessStopReason::OutputLimit);
  EXPECT_TRUE(R.StandardOutput.empty());
}
TEST_P(AndroidNative, BoundedTraceAndSharedInstructionEventBudgets) {
  Options.Limits.Instructions = 37;
  Options.Android->TraceLimit = 5;
  auto R = run("busy_loop");
  EXPECT_EQ(R.Stop, ProcessStopReason::InstructionLimit) << R.Diagnostic;
  EXPECT_EQ(R.Instructions, 37);
  EXPECT_EQ(R.Trace.size(), 5);
  EXPECT_TRUE(R.TraceTruncated);
  Options.Limits.Instructions = 10000;
  Options.Limits.Events = 1; // The constructor consumes the only return event.
  R = run("tls_slots");
  EXPECT_EQ(R.Stop, ProcessStopReason::EventLimit);
  EXPECT_EQ(R.Events, 1);
}
TEST_P(AndroidNative, ProgramMetadataDoesNotRequireSections) {
  auto Bytes = original();
  // ELF64 e_shoff, e_shentsize, e_shnum, e_shstrndx.
  llvm::support::endian::write64le(Bytes.data() + 40, 0);
  llvm::support::endian::write16le(Bytes.data() + 58, 0);
  llvm::support::endian::write16le(Bytes.data() + 60, 0);
  llvm::support::endian::write16le(Bytes.data() + 62, 0);
  temporary(Bytes, [&] {
    returned(run("add_arguments", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}), 402);
  });
}
TEST_P(AndroidNative, InvalidRelocationsFailBeforeAnyGuestInstruction) {
  auto Bytes = original();
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = llvm::cantFail(readELFProgramLinking(Image));
  uint64_t RelVA = 0;
  for (const auto &D : Facts.Dynamic)
    if (D.Tag == llvm::ELF::DT_JMPREL)
      RelVA = D.Value;
  ASSERT_NE(RelVA, 0);
  uint64_t Offset = 0;
  for (const auto &P : Image.ELFMetadata->ProgramHeaders)
    if (P.Type == llvm::ELF::PT_LOAD && RelVA >= P.VirtualAddress &&
        RelVA - P.VirtualAddress < P.FileSize)
      Offset = P.FileOffset + RelVA - P.VirtualAddress;
  ASSERT_NE(Offset, 0);
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Bad = Bytes;
    if (Mode == 0) // Unsupported relocation type, preserving the symbol index.
      llvm::support::endian::write32le(Bad.data() + Offset + 8,
                                       llvm::ELF::R_AARCH64_IRELATIVE);
    else if (Mode == 1)
      llvm::support::endian::write32le(Bad.data() + Offset + 12, 0xffffffff);
    else // Destination overflow, even though the table itself is valid.
      llvm::support::endian::write64le(Bad.data() + Offset, UINT64_MAX - 3);
    temporary(Bad, [&] {
      Options.Android->EntrySymbol = "tls_slots";
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      EXPECT_FALSE(bool(Result));
      llvm::consumeError(Result.takeError());
    });
  }
}
TEST_P(AndroidNative, RejectsContradictoryProgramHeaderMappings) {
  auto Bytes = original();
  uint64_t HeaderOffset = llvm::support::endian::read64le(Bytes.data() + 32);
  uint16_t Count = llvm::support::endian::read16le(Bytes.data() + 56);
  uint64_t Load = 0, Dynamic = 0;
  for (uint16_t I = 0; I < Count; ++I) {
    uint64_t P = HeaderOffset + I * 56;
    uint32_t Type = llvm::support::endian::read32le(Bytes.data() + P);
    if (Type == llvm::ELF::PT_LOAD && !Load)
      Load = P;
    if (Type == llvm::ELF::PT_DYNAMIC)
      Dynamic = P;
  }
  ASSERT_NE(Load, 0);
  ASSERT_NE(Dynamic, 0);
  for (unsigned Mode = 0; Mode < 4; ++Mode) {
    auto Bad = Bytes;
    if (Mode == 0)
      llvm::support::endian::write64le(Bad.data() + Load + 48, 3);
    if (Mode == 1)
      llvm::support::endian::write64le(Bad.data() + Load + 16, 1);
    if (Mode == 2)
      llvm::support::endian::write64le(Bad.data() + Load + 40, 0);
    if (Mode == 3) {
      uint64_t VA = llvm::support::endian::read64le(Bad.data() + Dynamic + 16);
      llvm::support::endian::write64le(Bad.data() + Dynamic + 16, VA + 8);
    }
    temporary(Bad, [&] {
      Options.Android->EntrySymbol = "tls_slots";
      auto Result =
          emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
      EXPECT_FALSE(bool(Result)) << Mode;
      llvm::consumeError(Result.takeError());
    });
  }
}
TEST_P(AndroidNative, InvalidRequestsAreRejectedAndTimeoutIsTyped) {
  Options.Android->EntrySymbol = "tls_slots";
  auto Original = Options;
  auto Rejected = [&] {
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(R));
    llvm::consumeError(R.takeError());
    Options = Original;
  };
  Options.Android->EntryAddress = 0;
  Rejected();
  Options.Android->Properties["invalid"] = std::string(92, 'a');
  Rejected();
  Options.Android->Memory.push_back({Buffer, 4096, {}, false});
  Rejected();
  Options.Android->LoadBias = UINT64_MAX;
  Rejected();
  Options.Android->TraceLimit = Options.OutputLimit;
  Rejected();
  Options.Limits.TimeoutMicroseconds = 1;
  auto R = run("busy_loop");
  EXPECT_EQ(R.Stop, ProcessStopReason::Timeout) << R.Diagnostic;
}
TEST_P(AndroidNative, RejectsMalformedOriginalDynamicTables) {
  ELFLoader Loader;
  auto Image = llvm::cantFail(Loader.load(Path));
  auto Facts = readELFProgramLinking(Image);
  ASSERT_TRUE(bool(Facts)) << llvm::toString(Facts.takeError());
  EXPECT_FALSE(Facts->Relocations.empty());
  uint64_t DynamicOffset = 0;
  for (const auto &P : Image.ELFMetadata->ProgramHeaders)
    if (P.Type == llvm::ELF::PT_DYNAMIC)
      DynamicOffset = P.FileOffset;
  ASSERT_NE(DynamicOffset, 0);
  auto Bytes = Image.Raw;
  // Force the string table span to overflow without touching any sections.
  for (uint64_t P = DynamicOffset; P + 16 <= Image.Raw.size(); P += 16) {
    auto Tag = llvm::support::endian::read64le(Image.Raw.data() + P);
    if (Tag == llvm::ELF::DT_NULL)
      break;
    if (Tag == llvm::ELF::DT_STRSZ) {
      llvm::support::endian::write64le(Image.Raw.data() + P + 8, UINT64_MAX);
      break;
    }
  }
  auto Bad = readELFProgramLinking(Image);
  EXPECT_FALSE(bool(Bad));
  llvm::consumeError(Bad.takeError());
}
TEST_P(AndroidNative, DynamicLookupUsesOnlyExplicitGuestSymbols) {
  returned(run("dynamic_lookup"), 100);
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  auto R = run("dynamic_lookup");
  returned(R, 4);
  auto Lookup = llvm::find_if(R.NativeCalls,
                              [](const auto &E) { return E.Name == "dlsym"; });
  ASSERT_NE(Lookup, R.NativeCalls.end());
  EXPECT_EQ(Lookup->Library, "libfixture.so");
  EXPECT_EQ(Lookup->Symbol, "strlen");
  ASSERT_TRUE(Lookup->Result);
  auto Call = llvm::find_if(R.NativeCalls,
                            [](const auto &E) { return E.Name == "strlen"; });
  ASSERT_NE(Call, R.NativeCalls.end());
  EXPECT_EQ(Call->PC, *Lookup->Result);
  EXPECT_EQ(Call->Library, "libfixture.so");
  EXPECT_EQ(Call->Result, 4);
  auto Report = llvm::json::parse(processResultJSON(R));
  ASSERT_TRUE(bool(Report)) << llvm::toString(Report.takeError());
  const auto *Android = Report->getAsObject()->getObject("android");
  ASSERT_NE(Android, nullptr);
  bool Named = false;
  for (const auto &V : *Android->getArray("native_calls")) {
    const auto *E = V.getAsObject();
    if (E->getString("name") == "dlsym") {
      EXPECT_EQ(E->getString("library"), "libfixture.so");
      EXPECT_EQ(E->getString("symbol"), "strlen");
      Named = true;
    }
  }
  EXPECT_TRUE(Named);
}
TEST_P(AndroidNative, DynamicHandlesFailuresAndConsumeOnceErrors) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  returned(run("dynamic_lifecycle"), 0);
}
TEST_P(AndroidNative, DynamicProvidersKeepSeparateSymbolNamespaces) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  Options.Android->Libraries["libother.so"] = {"strlen", "only_in_other"};
  returned(run("dynamic_providers"), 5);
}
TEST_P(AndroidNative, DynamicUnknownAndClosedFunctionsStopWithTheirNames) {
  Options.Android->Libraries["libfixture.so"] = {"strlen",
                                                 "unmodeled_fixture_export"};
  auto R = run("dynamic_unknown");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("unmodeled_fixture_export"), std::string::npos);
  ASSERT_FALSE(R.NativeCalls.empty());
  EXPECT_EQ(R.NativeCalls.back().Name, "unmodeled_fixture_export");
  EXPECT_EQ(R.NativeCalls.back().Library, "libfixture.so");
  EXPECT_FALSE(R.NativeCalls.back().Result);
  R = run("dynamic_closed");
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  EXPECT_NE(R.Diagnostic.find("inactive dynamic library"), std::string::npos);
  EXPECT_EQ(R.NativeCalls.back().Name, "strlen");
}
TEST_P(AndroidNative, DynamicScopesAndInvalidPointersAreNotGuessed) {
  Options.Android->Libraries["libfixture.so"] = {"strlen"};
  for (uint64_t Scope : {uint64_t(0), UINT64_MAX}) {
    auto R = run("dynamic_scope", {Scope});
    EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
    EXPECT_NE(R.Diagnostic.find("process scopes"), std::string::npos);
  }
  auto R = run("dynamic_open", {0, 2});
  EXPECT_EQ(R.Stop, ProcessStopReason::UnsupportedService);
  R = run("dynamic_open", {1, 2});
  EXPECT_NE(R.Stop, ProcessStopReason::Returned);
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
  R = run("dynamic_bad_name");
  EXPECT_NE(R.Stop, ProcessStopReason::Returned);
  EXPECT_NE(R.Diagnostic.find("invalid guest pointer"), std::string::npos);
}
TEST_P(AndroidNative, DynamicCatalogueRejectsMalformedCppInputs) {
  for (const auto &Names : std::vector<std::vector<std::string>>{
           {""}, {"strlen", "strlen"}, {std::string("bad\0name", 8)}}) {
    Options.Android->EntrySymbol = "dynamic_lookup";
    Options.Android->Libraries["libfixture.so"] = Names;
    auto R =
        emulateProcess(Path, ProcessProfile::AndroidNativeAArch64, Options);
    EXPECT_FALSE(bool(R));
    EXPECT_NE(llvm::toString(R.takeError()).find("modeled Android symbols"),
              std::string::npos);
  }
}
INSTANTIATE_TEST_SUITE_P(Relocations, AndroidNative,
                         testing::Values("none", "android", "relr"));
TEST(AndroidOptions, StrictWireTypesAndProfiles) {
  for (
      const char *Text :
      {R"({"android":{"typo":1}})", R"({"android":{"arguments":[-1]}})",
       R"({"android":{"entry_address":"0xzz"}})",
       R"({"android":{"memory":[{"address":4096,"size":4096,"bytes_hex":"gg"}]}})",
       R"({"android":{"read_memory":[{"address":4096,"size":8,"executable":true}]}})",
       R"({"android":{"libraries":[]}})",
       R"({"android":{"libraries":{"x":1}}})",
       R"({"android":{"libraries":{"x":["a","a"]}}})",
       R"({"android":{"libraries":{"x":[""]}}})",
       R"({"android":{"libraries":{"x":["bad\u0000name"]}}})",
       R"({"android":{"libraries":{"":[]}}})"}) {
    auto O = processOptionsFromJSON(Text);
    EXPECT_FALSE(bool(O)) << Text;
    llvm::consumeError(O.takeError());
  }
  auto O = processOptionsFromJSON(
      R"({"android":{"entry_address":"0x1000","arguments":["0xffffffffffffffff",0],"initialize":false,"trace_limit":0,"libraries":{"libfixture.so":["strlen"]}}})");
  ASSERT_TRUE(bool(O)) << llvm::toString(O.takeError());
  ASSERT_TRUE(O->Android);
  EXPECT_EQ(O->Android->Arguments[0], UINT64_MAX);
  EXPECT_EQ(O->Android->EntryAddress, 4096);
  EXPECT_EQ(O->Android->Libraries.at("libfixture.so"),
            std::vector<std::string>{"strlen"});
  auto Bad = emulateProcess("missing", ProcessProfile::LinuxELF64, *O);
  EXPECT_FALSE(bool(Bad));
  EXPECT_NE(llvm::toString(Bad.takeError()).find("Android"), std::string::npos);
}
} // namespace
} // namespace neverd::emulation
