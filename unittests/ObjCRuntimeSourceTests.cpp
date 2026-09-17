#include "gtest/gtest.h"

#include "neverd/sdk/NeverDCAPI.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace {
#ifdef __APPLE__
std::string read(const std::filesystem::path &Path) {
  std::ifstream Input(Path);
  return {std::istreambuf_iterator<char>(Input), {}};
}

void write(const std::filesystem::path &Path, const std::string &Text) {
  std::ofstream Output(Path);
  Output << Text;
  Output.close();
  ASSERT_TRUE(Output) << Path;
}

void run(const std::vector<std::string> &Arguments,
         const std::filesystem::path &Log) {
  std::vector<llvm::StringRef> Refs(Arguments.begin(), Arguments.end());
  const std::string Out = Log.string() + ".out";
  const std::string Err = Log.string() + ".err";
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt, Out, Err};
  std::string Error;
  const int Status = llvm::sys::ExecuteAndWait(Refs.front(), Refs, std::nullopt,
                                               Redirects, 120, 0, &Error);
  ASSERT_EQ(Status, 0) << Error << '\n' << read(Err);
}

enum class RuntimeFixture {
  AtomicARC,
  DispatchOnce,
  MutableConstants,
  CStringStorage,
  ARC,
  Associations,
  SwiftCalls,
  ConstantStrings,
  ConstantObjects,
  UnfairLocks,
  SwiftStrings,
  DiagnosticReports,
  Protocols,
  NativePointers,
  Foundation,
  BlockLifetimes,
  CoreData,
  DynamicProperties,
  ReceiverTypes,
  ReceiverFields,
  ReceiverAliases,
  ReceiverResults,
  SavedScalars,
  SharedFrameworks,
  AggregateRecords,
  WordRecords,
  PredicateFormats,
  CRecords,
  SystemCalls,
  MetadataCalls,
  InvariantLoops,
  SwitchEffects,
  LoopEdges,
  FramePadding,
  FrameSelectors,
  NativeReturnPaths,
  SwiftOnce,
  IncomingResults,
  NativeContext,
  AuxiliaryInputs,
  RuntimeIvars,
  SwiftTypeLookup,
  SwiftIntegerRuntime,
  SwiftRecordRuntime,
  NativeRecords,
  NativeFloating,
  NativeVoid,
  NativeVoidFrames,
  ReadOnlyTables,
  ScalarConstants,
  SwiftLiterals,
  StoredStrings,
  SystemData,
  IndirectFields,
  ProtocolReferences,
  SwiftAllocation,
  FloatingSaves,
  Equality,
  Graphics,
  DarwinDeclarations
};

void verifyRuntime(bool Chained,
                   RuntimeFixture FixtureKind = RuntimeFixture::ARC,
                   bool Profiled = false, bool ManualBlocks = false,
                   bool CheckSummary = false) {
  const bool DarwinDeclarations =
      FixtureKind == RuntimeFixture::DarwinDeclarations;
  const bool DynamicProperties =
      FixtureKind == RuntimeFixture::DynamicProperties;
  const bool ReceiverTypes = FixtureKind == RuntimeFixture::ReceiverTypes;
  const bool ReceiverFields = FixtureKind == RuntimeFixture::ReceiverFields;
  const bool ReceiverAliases = FixtureKind == RuntimeFixture::ReceiverAliases;
  const bool SavedScalars = FixtureKind == RuntimeFixture::SavedScalars;
  const bool ReceiverResults = FixtureKind == RuntimeFixture::ReceiverResults;
  const bool SharedFrameworks = FixtureKind == RuntimeFixture::SharedFrameworks;
  const bool AggregateRecords = FixtureKind == RuntimeFixture::AggregateRecords;
  const bool WordRecords = FixtureKind == RuntimeFixture::WordRecords;
  const bool PredicateFormats = FixtureKind == RuntimeFixture::PredicateFormats;
  const bool SwitchEffects = FixtureKind == RuntimeFixture::SwitchEffects;
  const bool SwiftTypeLookup = FixtureKind == RuntimeFixture::SwiftTypeLookup;
  const bool NativeVoid = FixtureKind == RuntimeFixture::NativeVoid;
  const bool NativeVoidFrames = FixtureKind == RuntimeFixture::NativeVoidFrames;
  const bool NativeFloating = FixtureKind == RuntimeFixture::NativeFloating;
  const bool NativeRecords = FixtureKind == RuntimeFixture::NativeRecords;
  const bool SwiftRecordRuntime =
      FixtureKind == RuntimeFixture::SwiftRecordRuntime;
  const bool SwiftIntegerRuntime =
      FixtureKind == RuntimeFixture::SwiftIntegerRuntime;
  const bool DispatchOnce = FixtureKind == RuntimeFixture::DispatchOnce;
  const bool MutableConstants = FixtureKind == RuntimeFixture::MutableConstants;
  const bool CStringStorage = FixtureKind == RuntimeFixture::CStringStorage;
  const bool AtomicARC = FixtureKind == RuntimeFixture::AtomicARC;
  const bool SwiftOnce = FixtureKind == RuntimeFixture::SwiftOnce;
  const bool NativeReturnPaths =
      FixtureKind == RuntimeFixture::NativeReturnPaths;
  const bool IncomingResults = FixtureKind == RuntimeFixture::IncomingResults;
  const bool NativeContext = FixtureKind == RuntimeFixture::NativeContext;
  const bool AuxiliaryInputs = FixtureKind == RuntimeFixture::AuxiliaryInputs;
  const bool RuntimeIvars = FixtureKind == RuntimeFixture::RuntimeIvars;
  const bool FramePadding = FixtureKind == RuntimeFixture::FramePadding;
  const bool FrameSelectors = FixtureKind == RuntimeFixture::FrameSelectors;
  const bool LoopEdges = FixtureKind == RuntimeFixture::LoopEdges;
  const bool InvariantLoops = FixtureKind == RuntimeFixture::InvariantLoops;
  const bool MetadataCalls = FixtureKind == RuntimeFixture::MetadataCalls;
  const bool SystemCalls = FixtureKind == RuntimeFixture::SystemCalls;
  const bool CRecords = FixtureKind == RuntimeFixture::CRecords;
  const bool CoreData = FixtureKind == RuntimeFixture::CoreData;
  const bool ReadOnlyTables = FixtureKind == RuntimeFixture::ReadOnlyTables;
  const bool ScalarConstants = FixtureKind == RuntimeFixture::ScalarConstants;
  const bool SwiftLiterals = FixtureKind == RuntimeFixture::SwiftLiterals;
  const bool StoredStrings = FixtureKind == RuntimeFixture::StoredStrings;
  const bool SwiftAllocation = FixtureKind == RuntimeFixture::SwiftAllocation;
  const bool Equality = FixtureKind == RuntimeFixture::Equality;
  const bool FloatingSaves = FixtureKind == RuntimeFixture::FloatingSaves;
  const bool ProtocolReferences =
      FixtureKind == RuntimeFixture::ProtocolReferences;
  const bool IndirectFields = FixtureKind == RuntimeFixture::IndirectFields;
  const bool SystemData = FixtureKind == RuntimeFixture::SystemData;
  const std::vector<std::string> SystemDataFrameworks{
      "-framework", "CoreData", "-framework", "CoreGraphics",
      "-framework", "ImageIO",  "-framework", "CoreSpotlight"};
  const std::vector<std::string> SharedFrameworkFlags{
      "-framework", "QuartzCore",
      "-framework", "CoreLocation",
      "-framework", "CoreSpotlight",
      "-framework", "UserNotifications",
      "-framework", "UniformTypeIdentifiers"};
  const bool Graphics = FixtureKind == RuntimeFixture::Graphics;
  const bool BlockLifetimes = FixtureKind == RuntimeFixture::BlockLifetimes;
  const bool Foundation = FixtureKind == RuntimeFixture::Foundation;
  const bool DiagnosticReports =
      FixtureKind == RuntimeFixture::DiagnosticReports;
  const bool SwiftStrings = FixtureKind == RuntimeFixture::SwiftStrings;
  const bool Associations = FixtureKind == RuntimeFixture::Associations;
  const bool SwiftCalls = FixtureKind == RuntimeFixture::SwiftCalls;
  const bool NativePointers = FixtureKind == RuntimeFixture::NativePointers;
  const bool ConstantStrings =
      FixtureKind == RuntimeFixture::ConstantStrings || NativePointers;
  const bool ConstantObjects = FixtureKind == RuntimeFixture::ConstantObjects;
  const bool UnfairLocks = FixtureKind == RuntimeFixture::UnfairLocks;
  const bool Protocols = FixtureKind == RuntimeFixture::Protocols;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-objc-arc", Directory));
  const std::filesystem::path Work(Directory.str().str());
  const auto Cleanup = llvm::make_scope_exit([&] {
    if (::testing::Test::HasFailure()) {
      llvm::errs() << "Failed source fixture retained at " << Work.string()
                   << "\n";
      return;
    }
    std::error_code Error;
    std::filesystem::remove_all(Work, Error);
  });
  const std::filesystem::path Fixtures(NEVERD_MOBILE_FIXTURE_DIR);
  const char *Fixture = DarwinDeclarations    ? "ObjCDarwinDeclarations.m"
                        : Equality            ? "ObjCEquality.m"
                        : FloatingSaves       ? "ObjCFloatingSaves.m"
                        : SharedFrameworks    ? "ObjCSharedFrameworks.m"
                        : AggregateRecords    ? "ObjCAggregateRecords.m"
                        : WordRecords         ? "ObjCWordRecords.m"
                        : PredicateFormats    ? "ObjCPredicateFormats.m"
                        : CRecords            ? "ObjCCRecordCalls.m"
                        : NativeVoidFrames    ? "ObjCNativeVoidFrames.m"
                        : NativeVoid          ? "ObjCNativeVoid.m"
                        : NativeFloating      ? "ObjCNativeFloating.m"
                        : NativeRecords       ? "ObjCNativeRecordResults.m"
                        : SwiftRecordRuntime  ? "ObjCSwiftRecordRuntime.m"
                        : SwiftIntegerRuntime ? "ObjCSwiftIntegerRuntime.m"
                        : SwiftTypeLookup     ? "ObjCSwiftTypeLookup.m"
                        : DispatchOnce        ? "ObjCDispatchOnce.m"
                        : MutableConstants    ? "ObjCMutableConstants.m"
                        : CStringStorage      ? "ObjCCStringStorage.m"
                        : SwiftOnce           ? "ObjCSwiftOnce.m"
                        : NativeReturnPaths   ? "ObjCNativeReturnPaths.m"
                        : IncomingResults     ? "ObjCIncomingResults.m"
                        : RuntimeIvars        ? "ObjCRuntimeIvars.swift"
                        : AuxiliaryInputs     ? "ObjCNativeAuxiliaryInputs.m"
                        : NativeContext       ? "ObjCNativeContext.m"
                        : FramePadding        ? "ObjCFramePadding.m"
                        : FrameSelectors      ? "ObjCFrameSelectors.m"
                        : LoopEdges           ? "ObjCLoopEdges.m"
                        : SwitchEffects       ? "ObjCSwitchEffects.m"
                        : InvariantLoops      ? "ObjCInvariantLoops.m"
                        : MetadataCalls       ? "ObjCMetadataCalls.m"
                        : SystemCalls         ? "ObjCSystemCalls.m"
                        : SavedScalars        ? "ObjCSavedScalars.m"
                        : ReceiverResults     ? "ObjCReceiverResults.m"
                        : ReceiverAliases     ? "ObjCReceiverAliases.m"
                        : ReceiverFields      ? "ObjCReceiverFields.m"
                        : ReceiverTypes       ? "ObjCReceiverTypes.m"
                        : DynamicProperties   ? "ObjCDynamicProperties.m"
                        : CoreData            ? "ObjCCoreDataCalls.m"
                        : ReadOnlyTables      ? "ObjCReadOnlyTables.m"
                        : ScalarConstants     ? "ObjCScalarConstants.m"
                        : SwiftLiterals       ? "ObjCSwiftLiteralStrings.m"
                        : StoredStrings       ? "ObjCStoredStrings.m"
                        : SwiftAllocation     ? "ObjCSwiftAllocation.m"
                        : ProtocolReferences  ? "ObjCProtocolReferences.m"
                        : IndirectFields      ? "ObjCIndirectFields.m"
                        : SystemData          ? "ObjCSystemData.m"
                        : Graphics            ? "ObjCGraphicsCalls.m"
                        : BlockLifetimes      ? "ObjCBlockLifetimes.m"
                        : Foundation          ? "ObjCFoundationCalls.m"
                        : Protocols           ? "ObjCProtocols.m"
                        : DiagnosticReports   ? "ObjCDiagnosticReports.m"
                        : SwiftStrings        ? "ObjCSwiftString.m"
                        : UnfairLocks         ? "ObjCUnfairLocks.m"
                        : ConstantObjects     ? "ObjCConstantObjects.m"
                        : ConstantStrings     ? "ObjCConstantStrings.m"
                        : SwiftCalls          ? "ObjCSwiftRuntime.m"
                        : Associations        ? "ObjCAssociations.m"
                                              : "ObjCARC.m";
  const char *Harness = DarwinDeclarations ? "ObjCDarwinDeclarationsHarness.m"
                        : Equality         ? "ObjCEqualityHarness.m"
                        : FloatingSaves    ? "ObjCFloatingSavesHarness.m"
                        : SharedFrameworks ? "ObjCSharedFrameworksHarness.m"
                        : AggregateRecords ? "ObjCAggregateRecordsHarness.m"
                        : WordRecords      ? "ObjCWordRecordsHarness.m"
                        : PredicateFormats ? "ObjCPredicateFormatsHarness.m"
                        : CRecords         ? "ObjCCRecordCallsHarness.m"
                        : NativeVoidFrames ? "ObjCNativeVoidFramesHarness.m"
                        : NativeVoid       ? "ObjCNativeVoidHarness.m"
                        : NativeFloating   ? "ObjCNativeFloatingHarness.m"
                        : NativeRecords    ? "ObjCNativeRecordResultsHarness.m"
                        : SwiftRecordRuntime ? "ObjCSwiftRecordRuntimeHarness.m"
                        : SwiftIntegerRuntime
                            ? "ObjCSwiftIntegerRuntimeHarness.m"
                        : SwiftTypeLookup   ? "ObjCSwiftTypeLookupHarness.m"
                        : DispatchOnce      ? "ObjCDispatchOnceHarness.m"
                        : MutableConstants  ? "ObjCMutableConstantsHarness.m"
                        : CStringStorage    ? "ObjCCStringStorageHarness.m"
                        : SwiftOnce         ? "ObjCSwiftOnceHarness.m"
                        : NativeReturnPaths ? "ObjCNativeReturnPathsHarness.m"
                        : IncomingResults   ? "ObjCIncomingResultsHarness.m"
                        : RuntimeIvars      ? "ObjCRuntimeIvarsHarness.m"
                        : AuxiliaryInputs ? "ObjCNativeAuxiliaryInputsHarness.m"
                        : NativeContext   ? "ObjCNativeContextHarness.m"
                        : FramePadding    ? "ObjCFramePaddingHarness.m"
                        : FrameSelectors  ? "ObjCFrameSelectorsHarness.m"
                        : LoopEdges       ? "ObjCLoopEdgesHarness.m"
                        : SwitchEffects   ? "ObjCSwitchEffectsHarness.m"
                        : InvariantLoops  ? "ObjCInvariantLoopsHarness.m"
                        : MetadataCalls   ? "ObjCMetadataCallsHarness.m"
                        : SystemCalls     ? "ObjCSystemCallsHarness.m"
                        : SavedScalars    ? "ObjCSavedScalarsHarness.m"
                        : ReceiverResults ? "ObjCReceiverResultsHarness.m"
                        : ReceiverAliases ? "ObjCReceiverAliasesHarness.m"
                        : ReceiverFields  ? "ObjCReceiverFieldsHarness.m"
                        : ReceiverTypes   ? "ObjCReceiverTypesHarness.m"
                        : DynamicProperties ? "ObjCDynamicPropertiesHarness.m"
                        : CoreData          ? "ObjCCoreDataCallsHarness.m"
                        : ReadOnlyTables    ? "ObjCReadOnlyTablesHarness.m"
                        : ScalarConstants   ? "ObjCScalarConstantsHarness.m"
                        : SwiftLiterals     ? "ObjCSwiftLiteralStringsHarness.m"
                        : StoredStrings     ? "ObjCStoredStringsHarness.m"
                        : SwiftAllocation   ? "ObjCSwiftAllocationHarness.m"
                        : ProtocolReferences ? "ObjCProtocolReferencesHarness.m"
                        : IndirectFields     ? "ObjCIndirectFieldsHarness.m"
                        : SystemData         ? "ObjCSystemDataHarness.m"
                        : Graphics           ? "ObjCGraphicsCallsHarness.m"
                        : BlockLifetimes     ? "ObjCBlockLifetimesHarness.m"
                        : Foundation         ? "ObjCFoundationCallsHarness.m"
                        : Protocols          ? "ObjCProtocolsHarness.m"
                        : DiagnosticReports  ? "ObjCDiagnosticReportsHarness.m"
                        : SwiftStrings       ? "ObjCSwiftStringHarness.m"
                        : UnfairLocks        ? "ObjCUnfairLocksHarness.m"
                        : ConstantObjects    ? "ObjCConstantObjectsHarness.m"
                        : ConstantStrings    ? "ObjCConstantStringsHarness.m"
                        : SwiftCalls         ? "ObjCSwiftRuntimeHarness.m"
                        : Associations       ? "ObjCAssociationsHarness.m"
                                             : "ObjCARCHarness.m";
  const auto Original = (Work / "original.dylib").string();
  const std::string Compiler = NEVERD_TEST_CLANG;
#if defined(__aarch64__) || defined(__arm64__)
  const std::string HostArch = "arm64";
#else
  const std::string HostArch = "x86_64";
#endif
  const unsigned CRecordMethods = HostArch == "arm64" ? 8U : 2U;
  std::vector<std::string> Compile{Compiler,      "-arch",
                                   HostArch,      "-O2",
                                   "-g0",         "-fobjc-arc",
                                   "-dynamiclib", "-framework",
                                   "Foundation",  (Fixtures / Fixture).string(),
                                   "-o",          Original};
  if (AtomicARC)
    Compile.push_back("-DNEVERD_ATOMIC_PROPERTIES");
  if (ReceiverAliases) {
    auto Flag = std::find(Compile.begin(), Compile.end(), "-fobjc-arc");
    ASSERT_NE(Flag, Compile.end());
    *Flag = "-fno-objc-arc";
  }
  if (ManualBlocks) {
    ASSERT_TRUE(BlockLifetimes);
    auto Flag = std::find(Compile.begin(), Compile.end(), "-fobjc-arc");
    ASSERT_NE(Flag, Compile.end());
    *Flag = "-fno-objc-arc";
    Compile.push_back("-DNEVERD_MANUAL_BLOCKS");
  }
  if (ReceiverResults)
    Compile.insert(Compile.end(), {"-framework", "CoreSpotlight"});
  if (SharedFrameworks)
    Compile.insert(Compile.end(), SharedFrameworkFlags.begin(),
                   SharedFrameworkFlags.end());
  if (SystemData)
    Compile.insert(Compile.end(), SystemDataFrameworks.begin(),
                   SystemDataFrameworks.end());
  if (CoreData || DynamicProperties)
    Compile.insert(Compile.end(), {"-framework", "CoreData"});
  if (CRecords)
    Compile.insert(Compile.end(), {"-framework", "CoreGraphics"});
  if (Graphics)
    Compile.insert(Compile.end(),
                   {"-framework", "CoreGraphics", "-framework", "ImageIO"});
  if (NativeVoid)
    Compile.push_back("-fomit-frame-pointer");
  if (NativeReturnPaths)
    Compile.push_back((Fixtures / "ObjCNativeReturnPaths.S").string());
  if (IncomingResults)
    Compile.push_back((Fixtures / "ObjCIncomingResults.S").string());
  if (NativeContext)
    Compile.push_back((Fixtures / "ObjCNativeContext.S").string());
  if (AuxiliaryInputs)
    Compile.push_back((Fixtures / "ObjCNativeAuxiliaryInputs.S").string());
  if (ReceiverFields)
    Compile.push_back((Fixtures / "ObjCReceiverFieldStorage.m").string());
  if (!Chained)
    Compile.push_back("-Wl,-no_fixup_chains");
  if (ConstantStrings)
    Compile.push_back((Fixtures / "ObjCConstantStringStorage.m").string());
  if (NativePointers)
    Compile.push_back("-DNEVERD_NATIVE_POINTERS");
  if (SwiftCalls || SwiftStrings || DiagnosticReports || SwiftAllocation ||
      SwiftTypeLookup || SwiftIntegerRuntime || SwiftRecordRuntime ||
      NativeRecords || NativeVoid || NativeVoidFrames || RuntimeIvars ||
      SwiftOnce)
    Compile.insert(Compile.end(), {"-L/usr/lib/swift", "-lswiftCore",
                                   "-Wl,-rpath,/usr/lib/swift"});
  if (SwiftStrings)
    Compile.push_back("-lswiftFoundation");
  if (DiagnosticReports)
    Compile.insert(Compile.end(), {"-iwithsysroot", "/usr/lib"});
  if (Profiled)
    Compile.push_back("-fprofile-instr-generate=" +
                      (Work / "counters.profraw").string());
  if (SwiftLiterals || SwiftAllocation) {
    const auto Base = (Work / "literal-base.o").string();
    ASSERT_NO_FATAL_FAILURE(
        run({Compiler, "-arch", HostArch, "-O2", "-fobjc-arc", "-c",
             (Fixtures / Fixture).string(), "-o", Base},
            Work / "compile-base"));
    Compile = {"/usr/bin/swiftc",
               "-O",
               "-target",
               HostArch + "-apple-macosx13.0",
               "-emit-library",
               "-import-objc-header",
               (Fixtures / (SwiftAllocation ? "ObjCSwiftAllocation.h"
                                            : "ObjCSwiftLiteralStrings.h"))
                   .string(),
               (Fixtures / (SwiftAllocation ? "ObjCSwiftAllocation.swift"
                                            : "ObjCSwiftLiteralStrings.swift"))
                   .string(),
               Base,
               "-o",
               Original};
    if (!Chained)
      Compile.insert(Compile.end(), {"-Xlinker", "-no_fixup_chains"});
  }
  if (RuntimeIvars) {
    Compile = {"/usr/bin/swiftc",
               "-O",
               "-emit-library",
               "-module-name",
               "RuntimeIvars",
               "-target",
               HostArch + "-apple-macosx13.0",
               (Fixtures / Fixture).string(),
               "-o",
               Original};
    if (!Chained)
      Compile.insert(Compile.end(), {"-Xlinker", "-no_fixup_chains"});
  }
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "compile-original"));

  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_TRUE(Session);
  ASSERT_TRUE(neverd_session_load(Session.get(), Original.c_str()));
  std::unique_ptr<const char, decltype(&neverd_free_string)> Raw(
      neverd_objc_methods_json(Session.get(), 0), neverd_free_string);
  ASSERT_TRUE(Raw);
  auto JSON = llvm::json::parse(Raw.get());
  ASSERT_TRUE(bool(JSON));
  if (CheckSummary) {
    auto Compare = [&](const llvm::json::Value &Full, size_t MaxFunctions) {
      std::unique_ptr<const char, decltype(&neverd_free_string)> SummaryRaw(
          neverd_objc_methods_summary_json(Session.get(), MaxFunctions),
          neverd_free_string);
      ASSERT_TRUE(SummaryRaw);
      auto Summary = llvm::json::parse(SummaryRaw.get());
      ASSERT_TRUE(bool(Summary));
      auto *SummaryObject = Summary->getAsObject();
      ASSERT_NE(SummaryObject, nullptr);
      EXPECT_EQ(SummaryObject->getBoolean("sources_omitted"), true);
      SummaryObject->erase("sources_omitted");
      auto Expected = Full;
      Expected.getAsObject()->erase("native_source");
      for (auto &Method : *Expected.getAsObject()->getArray("methods"))
        Method.getAsObject()->erase("source");
      EXPECT_TRUE(Expected == *Summary);
    };
    ASSERT_NO_FATAL_FAILURE(Compare(*JSON, 0));
    // A restricted inventory must preserve unrecovered rows and incomplete
    // diagnostics, not just parity for methods whose source was emitted.
    std::unique_ptr<const char, decltype(&neverd_free_string)> LimitedRaw(
        neverd_objc_methods_json(Session.get(), 1), neverd_free_string);
    ASSERT_TRUE(LimitedRaw);
    auto Limited = llvm::json::parse(LimitedRaw.get());
    ASSERT_TRUE(bool(Limited));
    ASSERT_GT(Limited->getAsObject()->getInteger("method_count"),
              Limited->getAsObject()->getInteger("recovered_method_count"));
    ASSERT_NO_FATAL_FAILURE(Compare(*Limited, 1));
  }
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *ProjectionGraph = Object->getObject("source_projection_graph");
  ASSERT_NE(ProjectionGraph, nullptr);
  EXPECT_EQ(ProjectionGraph->getString("closure_stage"),
            "before_method_emission_and_text_checks");
  const auto *ProjectionNodes = ProjectionGraph->getArray("nodes");
  ASSERT_NE(ProjectionNodes, nullptr);
  std::map<std::string, const llvm::json::Object *> Nodes;
  for (const auto &Value : *ProjectionNodes) {
    const auto *Node = Value.getAsObject();
    ASSERT_NE(Node, nullptr);
    const auto Address = Node->getString("address");
    ASSERT_TRUE(Address);
    ASSERT_TRUE(Nodes.emplace(Address->str(), Node).second);
    ASSERT_NE(Node->getObject("local_diagnostics"), nullptr);
    if (Node->getBoolean("has_typed_body") == false)
      EXPECT_EQ(
          Node->getObject("local_diagnostics")->getBoolean("checks_complete"),
          false);
  }
  for (const auto &[Address, Node] : Nodes) {
    SCOPED_TRACE(Address);
    const auto *Dependencies = Node->getArray("dependencies");
    ASSERT_NE(Dependencies, nullptr);
    const bool Closed = Node->getBoolean("closure_closed") == true;
    if (Closed)
      EXPECT_EQ(Node->getBoolean("local_gate_passed"), true);
    for (const auto &Value : *Dependencies) {
      const auto Dependency = Value.getAsString();
      ASSERT_TRUE(Dependency);
      ASSERT_TRUE(Nodes.count(Dependency->str()));
      if (Closed)
        EXPECT_EQ(Nodes.at(Dependency->str())->getBoolean("closure_closed"),
                  true);
    }
  }
  const auto *Methods = Object->getArray("methods");
  ASSERT_NE(Methods, nullptr);
  for (const auto &Value : *Methods) {
    const auto *Method = Value.getAsObject();
    ASSERT_NE(Method, nullptr);
    if (Method->getString("status") != "recovered")
      continue;
    const auto Entry = Method->getString("implementation");
    ASSERT_TRUE(Entry);
    ASSERT_TRUE(Nodes.count(Entry->str()));
    EXPECT_EQ(Nodes.at(Entry->str())->getBoolean("closure_closed"), true);
  }
  if (DynamicProperties) {
    const auto *Metadata = Object->getObject("objc_metadata");
    ASSERT_NE(Metadata, nullptr);
    const auto *Properties = Metadata->getArray("properties");
    ASSERT_NE(Properties, nullptr);
    EXPECT_TRUE(std::any_of(
        Properties->begin(), Properties->end(), [](const auto &Value) {
          const auto *Property = Value.getAsObject();
          return Property && Property->getString("name") == "ndOptionalValue" &&
                 Property->getBoolean("optional").value_or(false) &&
                 Property->getString("status") == "supported";
        }));
  }
  if (Equality && HostArch == "x86_64") {
    // This SDK catalog has conflicting x86-64 isEqual: return declarations.
    // Without that call contract the source cannot prove the subsequent
    // register lifetimes. Keep the negative case explicit until those facts
    // can be recovered, instead of accepting a body with unknown calls.
    ASSERT_EQ(Methods->size(), 6U);
    unsigned Rejected = 0;
    for (const auto &Value : *Methods) {
      const auto *Method = Value.getAsObject();
      ASSERT_NE(Method, nullptr);
      if (Method->getString("selector") == "equivalent:") {
        ++Rejected;
        EXPECT_EQ(Method->getString("status"), "unrecovered");
        const auto *Diagnostics = Method->getObject("projection_diagnostics");
        ASSERT_NE(Diagnostics, nullptr);
        const auto *Items = Diagnostics->getArray("items");
        ASSERT_NE(Items, nullptr);
        EXPECT_TRUE(
            std::any_of(Items->begin(), Items->end(), [](const auto &Item) {
              const auto *Diagnostic = Item.getAsObject();
              return Diagnostic &&
                     Diagnostic->getString("code") == "call_binding";
            }));
      } else {
        EXPECT_EQ(Method->getString("status"), "recovered");
      }
    }
    EXPECT_EQ(Rejected, 1U);
    return;
  }
  ASSERT_EQ(Methods->size(), DarwinDeclarations    ? 19U
                             : Equality            ? 6U
                             : FloatingSaves       ? 2U
                             : SharedFrameworks    ? 11U
                             : SavedScalars        ? 13U
                             : ReceiverResults     ? 8U
                             : ReceiverAliases     ? 5U
                             : AggregateRecords    ? 8U
                             : WordRecords         ? 11U
                             : PredicateFormats    ? 8U
                             : CRecords            ? CRecordMethods
                             : NativeVoidFrames    ? 3U
                             : NativeVoid          ? 3U
                             : NativeFloating      ? 3U
                             : NativeRecords       ? 2U
                             : SwiftRecordRuntime  ? 2U
                             : SwiftIntegerRuntime ? 2U
                             : SwiftTypeLookup     ? 1U
                             : DispatchOnce        ? 2U
                             : MutableConstants    ? 5U
                             : CStringStorage      ? 6U
                             : SwiftOnce           ? 1U
                             : NativeReturnPaths   ? 2U
                             : IncomingResults     ? 2U
                             : RuntimeIvars        ? 4U
                             : AuxiliaryInputs     ? 2U
                             : NativeContext       ? 2U
                             : FramePadding        ? 5U
                             : FrameSelectors      ? 1U
                             : LoopEdges           ? 1U
                             : SwitchEffects       ? 1U
                             : InvariantLoops      ? 2U
                             : MetadataCalls       ? 8U
                             : SystemCalls         ? 11U
                             : ReceiverFields      ? 4U
                             : ReceiverTypes       ? 9U
                             : DynamicProperties   ? 6U
                             : CoreData            ? 3U
                             : ReadOnlyTables      ? 4U
                             : ScalarConstants     ? 7U
                             : SwiftLiterals       ? 3U
                             : StoredStrings       ? 4U
                             : SwiftAllocation     ? 4U
                             : ProtocolReferences  ? 4U
                             : IndirectFields      ? 5U
                             : SystemData          ? 9U
                             : Graphics            ? 6U
                             : BlockLifetimes      ? (ManualBlocks ? 6U : 5U)
                             : Foundation          ? 13U
                             : Protocols           ? 6U
                             : DiagnosticReports   ? 5U
                             : SwiftStrings        ? 2U
                             : ConstantObjects     ? 5U
                             : ConstantStrings     ? 13U
                             : SwiftCalls          ? 9U
                                                   : 7U);
  std::set<std::string> Remaining{"item",         "setItem:", "observer",
                                  "setObserver:", "title",    "setTitle:",
                                  ".cxx_destruct"};
  if (ReceiverTypes)
    Remaining = {
        "NDIntegerReceiver-sharedValue", "NDIntegerReceiver-readOwnValue",
        "NDIntegerReceiver+sharedValue", "NDIntegerReceiver+readClassValue",
        "NDPointerReceiver-sharedValue", "NDPointerReceiver-readOwnValue",
        "NDPointerReceiver-code",        "NDPointerReceiver-readOwnCode",
        "NDTypedError-declaredCode"};
  if (SharedFrameworks)
    Remaining = {"makeLayer",      "opacity:",        "opacity:layer:",
                 "position:",      "position:layer:", "distance:from:",
                 "describe:text:", "descriptionOf:",  "request:content:",
                 "type:",          "extensionOf:"};
  if (SavedScalars)
    Remaining = {"flag",          "setFlag:",
                 "byte",          "setByte:",
                 "word",          "setWord:",
                 "integer",       "setInteger:",
                 "promoteByte:",  "promoteSignedByte:",
                 "promoteWord:",  "promoteSignedWord:",
                 "branchForFlag:"};
  if (ReceiverResults)
    Remaining = {"NDResultValue-duration",         "NDResultValue-setDuration:",
                 "NDResultValue+factoryDuration:", "NDResultOwner-error",
                 "NDResultOwner-setError:",        "NDResultOwner-errorCode",
                 "NDResultOwner-.cxx_destruct",    "NDResultOther-code"};
  if (ReceiverAliases)
    Remaining = {"NDAliasError-retainedCode", "NDAliasError-autoreleasedCode",
                 "NDAliasError-retainAutoreleasedCode",
                 "NDAliasError-claimedCode", "NDAliasOther-code"};
  if (AggregateRecords)
    Remaining = {
        "pair:",         "quad:",   "rotate:", "throughCall:",
        "scale:factor:", "floats:", "triple:", "stackA:b:c:d:e:f:g:pair:tail:"};
  if (WordRecords)
    Remaining = {"word:",
                 "pair:",
                 "swap:",
                 "throughCall:",
                 "nested:",
                 "pointer:",
                 "readPointer:",
                 "three:b:c:pair:tail:",
                 "five:b:c:d:e:pair:tail:",
                 "subarray:range:",
                 "find:needle:"};
  if (SwiftTypeLookup)
    Remaining = {"lookup:length:"};
  if (NativeVoidFrames)
    Remaining = {"releaseFirst:second:active:",
                 "releaseFirst:second:active:result:"};
  if (NativeVoid)
    Remaining = {"releaseObject:active:", "releaseObject:active:result:"};
  if (NativeFloating)
    Remaining = {"doubleValue:other:mode:output:",
                 "floatValue:other:mode:output:", "compare:other:bias:"};
  if (NativeRecords)
    Remaining = {"newBox:value:storage:", "metadataState:request:result:"};
  if (SwiftRecordRuntime)
    Remaining = {"newBox:value:storage:", "metadataState:request:result:"};
  if (SwiftIntegerRuntime)
    Remaining = {"retainObject:times:", "object:canCastToClass:"};
  if (SwiftOnce)
    Remaining = {"value"};
  if (DispatchOnce)
    Remaining = {"shared", "initializationCount"};
  if (MutableConstants)
    Remaining = {"value", "independent", "initial",
                 "setValue:", "setIndependent:"};
  if (NativeReturnPaths)
    Remaining = {"adjusted:choose:output:", "wideLeaf:"};
  if (CStringStorage)
    Remaining = {"label",  "suffix", "newQueue",
                 "string", "stored", "setStored:"};
  if (IncomingResults)
    Remaining = {"word:flags:", "word:memory:"};
  if (NativeContext)
    Remaining = {"word:context:", "contextWord:"};
  if (AuxiliaryInputs)
    Remaining = {"fillWithWord:buffer:", "addWord:buffer:"};
  if (RuntimeIvars)
    Remaining = {"word", "setWord:"};
  if (ReadOnlyTables)
    Remaining = {
        "actionForKind:", "shortForKind:", "maskedForKind:", "doubleForKind:"};
  if (FramePadding)
    Remaining = {"key", "horizontal", "vertical",
                 "setHorizontal:", "setVertical:"};
  if (FrameSelectors)
    Remaining = {"removing:from:"};
  if (LoopEdges)
    Remaining = {"fold:seed:output:"};
  if (SwitchEffects)
    Remaining = {"choose:object:output:"};
  if (InvariantLoops)
    Remaining = {"nestedNumbers:", "nestedStrings:"};
  if (MetadataCalls)
    Remaining = {"url:",     "date:",      "characterSet:", "dateComponents:",
                 "request:", "indexPath:", "locale:",       "notification:"};
  if (SystemCalls)
    Remaining = {"setAttribute:name:bytes:length:",
                 "getAttribute:name:bytes:length:",
                 "listAttributes:bytes:length:",
                 "removeAttribute:name:",
                 "logEnabled:type:",
                 "setMessage:key:value:",
                 "messageValue:key:",
                 "digest:length:output:",
                 "initializeDigest:",
                 "updateDigest:bytes:length:",
                 "finishDigest:output:"};
  if (CRecords) {
    Remaining = {"unionRange:with:", "intersection:with:"};
    if (HostArch == "arm64")
      Remaining.insert({"width:", "midY:", "standardize:", "unionRect:with:",
                        "textPosition:", "fill:context:"});
  }
  if (PredicateFormats)
    Remaining = {"object:", "key:value:", "minimum:maximum:", "count:",
                 "score:",  "quoted:",    "always",           "expression:"};
  if (ReceiverFields)
    Remaining = {"NDFieldOwner-errorCode", "NDFieldOwner-nestedErrorCode",
                 "NDFieldOwner-errorCodeAfterCall:", "NDFieldUnrelated-code"};
  if (Associations)
    Remaining = {"objectForKey:",
                 "storeObject:forKey:policy:",
                 "clearAssociatedObjects",
                 "objectForStaticKey",
                 "storeObjectForStaticKey:",
                 "objectForInteriorKey",
                 "storeObjectForInteriorKey:"};
  if (SwiftCalls)
    Remaining = {"keep:",
                 "drop:",
                 "weakInitialize:object:",
                 "weakAssign:object:",
                 "weakRead:",
                 "weakDestroy:",
                 "objectType:",
                 "begin:scratch:flags:",
                 "end:"};
  if (ConstantObjects)
    Remaining = {"words", "nested", "signedNumber", "unsignedNumber",
                 "mapping"};
  if (ConstantStrings)
    Remaining = {"ascii",           "alias",         "unicode",
                 "embedded",        "empty",         "first",
                 "second",          "indirectASCII", "indirectAlias",
                 "indirectUnicode", "mutableValue",  "setMutableValue:"};
  if (UnfairLocks)
    Remaining = {"add:",   "value",       "tryAdd:",       "lock",
                 "unlock", "assertOwner", "assertNotOwner"};
  if (SwiftStrings)
    Remaining = {"bridgeWord:storage:", "roundTrip:"};
  if (DiagnosticReports)
    Remaining = {"initializer", "initializerInFile", "fatal", "fatalInFile",
                 "terminal"};
  if (Protocols) {
    Remaining = {"enumerate:state:objects:count:",
                 "metricOf:",
                 "isNegativeMetric:",
                 "countObjects:",
                 "reportMutation:",
                 "checkRuntimeGuard:"};
    const auto *Metadata = Object->getObject("objc_metadata");
    ASSERT_NE(Metadata, nullptr);
    const auto *Declarations = Metadata->getArray("protocols");
    ASSERT_NE(Declarations, nullptr);
    bool Found = false;
    for (const auto &Value : *Declarations) {
      const auto *Protocol = Value.getAsObject();
      ASSERT_NE(Protocol, nullptr);
      if (Protocol->getString("name") != "NSFastEnumeration")
        continue;
      const auto *Members = Protocol->getArray("methods");
      ASSERT_NE(Members, nullptr);
      for (const auto &Member : *Members) {
        const auto *Method = Member.getAsObject();
        ASSERT_NE(Method, nullptr);
        if (Method->getString("selector") ==
            "countByEnumeratingWithState:objects:count:") {
          EXPECT_EQ(Method->getString("status"), "supported");
          EXPECT_EQ(Method->get("implementation"), nullptr);
          Found = true;
        }
      }
    }
    ASSERT_TRUE(Found);
  }
  if (Foundation)
    Remaining = {"lookup:key:",
                 "lengthOf:",
                 "copyObject:",
                 "append:to:",
                 "numberValue:",
                 "put:forKey:in:",
                 "makeDictionary:keys:count:",
                 "formatObject:number:fraction:",
                 "formatPosition:fraction:",
                 "formatEmpty",
                 "formatWide:small:",
                 "logObject:count:fraction:",
                 "logEmpty"};
  if (DynamicProperties)
    Remaining = {
        "eventCount:", "setEventCount:record:", "weight:", "setWeight:record:",
        "label:",      "setLabel:record:"};
  if (CoreData)
    Remaining = {
        "fetchFromContext:request:error:", "countInContext:request:error:",
        "registeredObjectsInContext:"};
  if (ScalarConstants)
    Remaining = {"finiteDouble", "negativeZeroDouble", "payloadDouble",
                 "finiteFloat",  "negativeZeroFloat",  "payloadFloat",
                 "fillWide:"};
  if (Graphics)
    Remaining = {"widthOfImage:", "heightOfImage:",     "retainImage:",
                 "alphaOfColor:", "componentsInColor:", "imageCountInSource:"};
  if (SystemData)
    Remaining = {"emptyArray",       "emptyArrayAlias",
                 "emptyDictionary",  "trueObject",
                 "falseObject",      "contextSaveName",
                 "gifDictionaryKey", "searchableItemIdentifier",
                 "colorSpaceName"};
  if (SwiftAllocation)
    Remaining = {
        "allocateRaw:alignment:", "freeRaw:size:alignment:",
        "allocateObject:size:alignment:", "freeUninitialized:size:alignment:"};
  if (Equality)
    Remaining = {"equivalent:",   "leftValue",      "rightValue",
                 "setLeftValue:", "setRightValue:", ".cxx_destruct"};
  if (FloatingSaves)
    Remaining = {"sumSine:cosine:", "weighted:bias:"};
  if (ProtocolReferences)
    Remaining = {"valueProtocol", "rootProtocol", "sameValueProtocol",
                 "sameRootProtocol"};
  if (IndirectFields)
    Remaining = {"first", "second", "setFirst:", "setSecond:", ".cxx_destruct"};
  if (StoredStrings)
    Remaining = {"arrayWithValue:", "dictionaryWithValue:", "writeLiteralTo:",
                 "literal"};
  if (SwiftLiterals)
    Remaining = {"asciiLiteral", "sameAsciiLiteral", "unicodeLiteral"};
  if (DarwinDeclarations)
    Remaining = {"nameOfClass:",
                 "classNamed:",
                 "nameOfSelector:",
                 "selectorNamed:",
                 "incrementWithLock:counter:",
                 "incrementWithObject:counter:",
                 "compare:with:",
                 "time:delta:",
                 "lastError",
                 "remainder:divisor:",
                 "defaultMode",
                 "modeStorage",
                 "descriptionKey",
                 "mainQueue",
                 "timerType",
                 "defaultPriority",
                 "foundationVersion",
                 "belongs:to:",
                 "responds:selector:"};
  if (BlockLifetimes)
    Remaining = {"makeCounterForArray:", "makeCounterForArray:other:offset:",
                 "duplicateBlock:", "releaseBlock:",
                 "synchronouslyAppend:toArray:queue:"};
  if (ManualBlocks)
    Remaining.insert("holderForBlock:");
  std::string Declarations;
  std::string Install =
      "static void installRecovered(void) {\n"
      "Class cls = objc_getClass(\"" +
      std::string(DarwinDeclarations    ? "NDDarwinDeclarations"
                  : SavedScalars        ? "NDSavedValues"
                  : SharedFrameworks    ? "NDFrameworkCalls"
                  : AggregateRecords    ? "NDRecords"
                  : WordRecords         ? "NDWordRecords"
                  : PredicateFormats    ? "NDPredicateFormats"
                  : CRecords            ? "NDCCRecords"
                  : NativeVoidFrames    ? "NDVoidFrames"
                  : NativeVoid          ? "NDVoidForwarders"
                  : NativeFloating      ? "NDNativeFloating"
                  : NativeRecords       ? "NDNativeRecordResults"
                  : SwiftRecordRuntime  ? "NDSwiftRecordRuntime"
                  : SwiftIntegerRuntime ? "NDSwiftIntegerRuntime"
                  : SwiftTypeLookup     ? "NDSwiftTypeLookup"
                  : DispatchOnce        ? "NDDispatchOnce"
                  : MutableConstants    ? "NDMutableConstants"
                  : CStringStorage      ? "NDCStringStorage"
                  : SwiftOnce           ? "NDSwiftOnce"
                  : NativeReturnPaths   ? "NDNativeReturnPaths"
                  : IncomingResults     ? "NDIncomingResults"
                  : RuntimeIvars        ? "NDRuntimeIvars"
                  : AuxiliaryInputs     ? "NDNativeAuxiliaryInputs"
                  : NativeContext       ? "NDNativeContext"
                  : FramePadding        ? "NDFramePadding"
                  : FrameSelectors      ? "NDFrameSelectors"
                  : LoopEdges           ? "NDLoopEdges"
                  : SwitchEffects       ? "NDSwitchEffects"
                  : InvariantLoops      ? "NDInvariantLoops"
                  : MetadataCalls       ? "NDMetadataCalls"
                  : SystemCalls         ? "NDSystemCalls"
                  : DynamicProperties   ? "NDPropertyDriver"
                  : CoreData            ? "NDCoreDataCalls"
                  : ReadOnlyTables      ? "NDReadOnlyTables"
                  : ScalarConstants     ? "NDScalarConstants"
                  : Equality            ? "NDEquality"
                  : FloatingSaves       ? "NDFloatingSaves"
                  : SwiftLiterals       ? "NDSwiftLiteralStrings"
                  : StoredStrings       ? "NDStoredStrings"
                  : SwiftAllocation     ? "NDSwiftAllocation"
                  : ProtocolReferences  ? "NDProtocolReferences"
                  : IndirectFields      ? "NDIndirectFields"
                  : SystemData          ? "NDSystemData"
                  : Graphics            ? "NDGraphicsCalls"
                  : BlockLifetimes      ? "NDBlockFactory"
                  : Foundation          ? "NDFoundationCalls"
                  : Protocols           ? "NDProtocolCalls"
                  : DiagnosticReports   ? "NDDiagnosticReports"
                  : SwiftStrings        ? "NDSwiftString"
                  : UnfairLocks         ? "NDUnfairLocks"
                  : ConstantObjects     ? "NDConstantObjects"
                  : ConstantStrings     ? "NDConstantStrings"
                  : SwiftCalls          ? "NDSwiftRuntimeCalls"
                                        : "NDARCBox") +
      "\");\n";
  if (ReceiverTypes || ReceiverFields || ReceiverAliases || ReceiverResults)
    Install = "static void installRecovered(void) {\nClass cls;\n";
  std::vector<std::string> Sources;
  std::map<std::string, std::string> IdentityHelpers;
  std::map<std::string, std::string> BlockHelpers;
  unsigned RepeatedBlockHelpers = 0;
  std::set<std::string> RejectedConstantUses =
      ConstantStrings ? std::set<std::string>{"slotAddress"}
                      : std::set<std::string>{};
  std::string NativeHelper;
  std::set<std::string> StorageNames;
  std::set<std::string> CStringNames;
  for (const auto &Value : *Methods) {
    const auto *Method = Value.getAsObject();
    ASSERT_NE(Method, nullptr);
    auto Selector = Method->getString("selector");
    ASSERT_TRUE(Selector);
    if ((NativeVoid && *Selector == "unprovenResult:active:") ||
        (NativeVoidFrames && *Selector == "unprovenResult:second:active:")) {
      EXPECT_EQ(Method->getString("status"), "unrecovered");
      EXPECT_EQ(Method->getString("reason"),
                "method contains an unresolved value");
      continue;
    }
    if (RuntimeIvars && (*Selector == "init" || *Selector == ".cxx_destruct"))
      continue; // This fixture replaces only the two scalar property methods.
    if (RejectedConstantUses.erase(Selector->str())) {
      EXPECT_EQ(Method->getString("status"), "unrecovered");
      const auto Reason = Method->getString("reason");
      ASSERT_TRUE(Reason);
      EXPECT_NE(Reason->find("image address"), llvm::StringRef::npos)
          << Reason->str();
      continue;
    }
    ASSERT_EQ(Method->getString("status"), "recovered") << Raw.get();
    auto Name = Method->getString("function_name");
    auto Source = Method->getString("source");
    ASSERT_TRUE(Selector && Name && Source);
    if (AtomicARC && *Selector == "item")
      EXPECT_TRUE(Source->contains("objc_getProperty(")) << Source->str();
    std::string Identity = Selector->str();
    if (ReceiverTypes || ReceiverFields || ReceiverAliases || ReceiverResults) {
      const auto Class = Method->getString("class_name");
      const auto ClassMethod = Method->getBoolean("class_method");
      ASSERT_TRUE(Class && ClassMethod);
      Identity = Class->str() + (*ClassMethod ? "+" : "-") + Identity;
      Install += "cls = objc_getClass(\"" + Class->str() + "\");\n";
      if (*ClassMethod)
        Install += "cls = object_getClass(cls);\n";
    }
    ASSERT_EQ(Remaining.erase(Identity), 1U) << Identity;
    std::string MethodSource = Source->str();
    // Each C API unit is independently compilable. Its inventory identifies
    // functions whose address must be shared when units are linked together.
    if (const auto *Helpers = Method->getArray("shared_block_functions"))
      for (const auto &Value : *Helpers) {
        const auto Helper = Value.getAsString();
        ASSERT_TRUE(Helper);
        auto Name = MethodSource.find(" " + Helper->str() + "(");
        ASSERT_NE(Name, std::string::npos);
        auto Begin = MethodSource.rfind('\n', Name);
        ASSERT_NE(Begin, std::string::npos);
        // Skip prototypes and select the emitted top-level definition.
        while (MethodSource.find('{', Begin) > MethodSource.find(';', Begin)) {
          Name = MethodSource.find(" " + Helper->str() + "(", Name + 1);
          ASSERT_NE(Name, std::string::npos);
          Begin = MethodSource.rfind('\n', Name);
        }
        const auto End = MethodSource.find("\n}", Begin);
        ASSERT_NE(End, std::string::npos);
        const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
        const auto [It, Added] =
            BlockHelpers.emplace(Helper->str(), Definition);
        EXPECT_EQ(It->second, Definition);
        if (!Added) {
          ++RepeatedBlockHelpers;
          MethodSource.replace(Begin, End + 2 - Begin,
                               Definition.substr(0, Definition.find('{')) +
                                   ";");
        }
      }
    const char *NativeSignature =
        NativeVoid
            ? "void releaseIfActive(void* native_arg0, int64_t native_arg1)"
        : NativePointers && (*Selector == "ascii" || *Selector == "unicode")
            ? "int64_t NDForwardPointer(void* native_arg0)"
        : IndirectFields && (*Selector == "first" || *Selector == "second")
            ? "int64_t NDReadObjectField(void* native_arg0, void* native_arg1)"
            : nullptr;
    if (NativeSignature) {
      EXPECT_NE(MethodSource.find(NativeSignature), std::string::npos)
          << MethodSource;
      EXPECT_NE(MethodSource.find(NativeVoid ? "swift_unknownObjectRelease"
                                             : "objc_retain"),
                std::string::npos);
      // Each C API method includes its complete native dependency group.
      // Link the common helper once when combining these two independent units.
      const auto Begin =
          MethodSource.find("\n" + std::string(NativeSignature) + " {\n");
      ASSERT_NE(Begin, std::string::npos);
      const auto End = MethodSource.find("\n}", Begin);
      ASSERT_NE(End, std::string::npos);
      const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
      if (NativeHelper.empty()) {
        NativeHelper = Definition;
      } else {
        EXPECT_EQ(NativeHelper, Definition);
        MethodSource.erase(Begin, End + 2 - Begin);
      }
    }
    for (const char *Inventory :
         {"shared_identity_functions", "shared_storage_functions"})
      if (const auto *Helpers = Method->getArray(Inventory)) {
        for (const auto &Value : *Helpers) {
          const auto Helper = Value.getAsString();
          ASSERT_TRUE(Helper);
          if (llvm::StringRef(Inventory) == "shared_storage_functions")
            StorageNames.insert(Helper->str());
          if (Helper->starts_with("neverd_cstring_storage_"))
            CStringNames.insert(Helper->str());
          const auto Begin =
              MethodSource.find("\nuintptr_t " + Helper->str() + "(void) {\n");
          ASSERT_NE(Begin, std::string::npos);
          const auto End = MethodSource.find("\n}", Begin);
          ASSERT_NE(End, std::string::npos);
          const auto Definition = MethodSource.substr(Begin, End + 2 - Begin);
          const auto [It, Added] =
              IdentityHelpers.emplace(Helper->str(), Definition);
          EXPECT_EQ(It->second, Definition);
          MethodSource.erase(Begin, End + 2 - Begin);
        }
      }
    const auto Path =
        Work / ("recovered-" + std::to_string(Sources.size()) + ".c");
    ASSERT_NO_FATAL_FAILURE(write(Path, MethodSource));
    Sources.push_back(Path.string());
    Declarations += "extern void " + Name->str() + "(void);\n";
    Install += "class_replaceMethod(cls, sel_registerName(\"" +
               Selector->str() + "\"), (IMP)" + Name->str() +
               ", method_getTypeEncoding(class_getInstanceMethod(cls, "
               "sel_registerName(\"" +
               Selector->str() + "\"))));\n";
  }
  ASSERT_TRUE(Remaining.empty());
  EXPECT_TRUE(RejectedConstantUses.empty());
  if (BlockLifetimes)
    EXPECT_GE(RepeatedBlockHelpers, 2U);
  EXPECT_EQ(StorageNames.size(), Profiled || ConstantStrings   ? 1U
                                 : (SwiftOnce || DispatchOnce) ? 3U
                                 : MutableConstants            ? 2U
                                                               : 0U);
  if (DiagnosticReports)
    EXPECT_FALSE(IdentityHelpers.empty());
  else
    EXPECT_EQ(IdentityHelpers.size(), (Associations       ? 0U
                                       : ConstantObjects  ? 13U
                                       : ConstantStrings  ? 7U
                                       : MutableConstants ? 1U
                                       : CStringStorage   ? 1U
                                       : Foundation       ? 6U
                                       : SwiftLiterals    ? 2U
                                       : StoredStrings    ? 4U
                                       : ReadOnlyTables   ? 4U
                                       : FramePadding     ? 1U
                                       : FrameSelectors   ? 1U
                                       : PredicateFormats ? 8U
                                                          : 0U) +
                                          StorageNames.size() +
                                          CStringNames.size());
  if (!IdentityHelpers.empty()) {
    std::string Shared = "#include <stdint.h>\n";
    for (const auto &[Name, Definition] : IdentityHelpers)
      Shared += Definition + "\n";
    const auto Path = Work / "shared-identities.c";
    ASSERT_NO_FATAL_FAILURE(write(Path, Shared));
    Sources.push_back(Path.string());
  }
  ASSERT_NO_FATAL_FAILURE(
      write(Work / "replacements.h", Declarations + Install + "}\n"));
  const auto Baseline = (Work / "baseline").string();
  Compile = {Compiler,
             "-arch",
             HostArch,
             "-O2",
             "-fno-objc-arc",
             "-fblocks",
             "-framework",
             "Foundation",
             "-I" + Work.string(),
             (Fixtures / Harness).string(),
             Original,
             "-o",
             Baseline};
  if (AtomicARC)
    Compile.insert(Compile.end() - 2, "-DNEVERD_ATOMIC_PROPERTIES");
  if (ManualBlocks)
    Compile.insert(Compile.end() - 2, "-DNEVERD_MANUAL_BLOCKS");
  if (ReceiverResults)
    Compile.insert(Compile.end() - 2, {"-framework", "CoreSpotlight"});
  if (SharedFrameworks)
    Compile.insert(Compile.end() - 2, SharedFrameworkFlags.begin(),
                   SharedFrameworkFlags.end());
  if (SystemData)
    Compile.insert(Compile.end() - 2, SystemDataFrameworks.begin(),
                   SystemDataFrameworks.end());
  if (CoreData || DynamicProperties)
    Compile.insert(Compile.end() - 2, {"-framework", "CoreData"});
  if (CRecords)
    Compile.insert(Compile.end() - 2, {"-framework", "CoreGraphics"});
  if (Graphics)
    Compile.insert(Compile.end() - 2,
                   {"-framework", "CoreGraphics", "-framework", "ImageIO"});
  if (SwiftCalls || SwiftStrings || DiagnosticReports || SwiftAllocation ||
      SwiftTypeLookup || SwiftIntegerRuntime || SwiftRecordRuntime ||
      NativeRecords || NativeVoid || NativeVoidFrames || RuntimeIvars ||
      SwiftOnce)
    Compile.insert(Compile.end() - 2, {"-L/usr/lib/swift", "-lswiftCore",
                                       "-Wl,-rpath,/usr/lib/swift"});
  if (SwiftStrings) {
    const auto Words = (Work / "words.dylib").string();
    ASSERT_NO_FATAL_FAILURE(
        run({"/usr/bin/swiftc", "-O", "-target", HostArch + "-apple-macosx13.0",
             "-emit-library",
             (Fixtures / "ObjCSwiftStringWords.swift").string(), "-o", Words},
            Work / "compile-swift-words"));
    Compile.insert(Compile.end() - 2, {"-lswiftFoundation", Words});
  }
  if (MetadataCalls) {
    const auto Oracle = (Work / "metadata-oracle.dylib").string();
    ASSERT_NO_FATAL_FAILURE(
        run({"/usr/bin/swiftc", "-O", "-target", HostArch + "-apple-macosx13.0",
             "-emit-library", (Fixtures / "ObjCMetadataOracle.swift").string(),
             "-o", Oracle},
            Work / "compile-metadata-oracle"));
    Compile.insert(Compile.end() - 2, Oracle);
  }
  if (Protocols) {
    const auto Probe = (Work / "stack-failure-probe.dylib").string();
    ASSERT_NO_FATAL_FAILURE(
        run({Compiler, "-arch", HostArch, "-O2", "-dynamiclib",
             (Fixtures / "ObjCStackFailureProbe.c").string(), "-o", Probe},
            Work / "compile-stack-probe"));
    Compile.insert(Compile.end() - 2, Probe);
  }
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-baseline"));
  ASSERT_NO_FATAL_FAILURE(run({Baseline}, Work / "baseline"));
  const auto Recovered = (Work / "recovered").string();
  Compile.back() = Recovered;
  Compile.push_back("-DNEVERD_RECOVERED_ARC");
  Compile.insert(Compile.end(), Sources.begin(), Sources.end());
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-recovered"));
  ASSERT_NO_FATAL_FAILURE(run({Recovered}, Work / "recovered"));
  EXPECT_EQ(read(Work / "baseline.out"), read(Work / "recovered.out"));
  if (Foundation) {
    auto Messages = [](const std::string &Text) {
      std::vector<std::string> Result;
      size_t Position = 0;
      while ((Position = Text.find("ND_FORMAT:", Position)) !=
             std::string::npos) {
        const auto End = Text.find('\n', Position);
        Result.push_back(Text.substr(Position, End - Position));
        Position = End == std::string::npos ? Text.size() : End + 1;
      }
      return Result;
    };
    const auto BaselineMessages = Messages(read(Work / "baseline.err"));
    const auto RecoveredMessages = Messages(read(Work / "recovered.err"));
    ASSERT_EQ(BaselineMessages.size(), 4U);
    EXPECT_EQ(RecoveredMessages, BaselineMessages);
  }
  if (DiagnosticReports) {
    EXPECT_EQ(read(Work / "baseline.err"), read(Work / "recovered.err"));
    EXPECT_NE(
        read(Work / "recovered.err").find("Use of unimplemented initializer"),
        std::string::npos);
  }
  EXPECT_EQ(
      read(Work / "recovered.out"),
      DarwinDeclarations ? "darwin-declarations=2048\nlocked-updates="
                           "8192\nsynchronized-updates=8192\n"
      : Equality ? "equality-cases=4096\nshort-circuit=pass\nownership=pass\n"
      : FloatingSaves    ? "floating-saves=4096\nvalues-across-calls=pass\n"
      : AggregateRecords ? "record-checks=8192\nrecord-bits=pass\nrecord-calls="
                           "pass\nrecord-stack=pass\n"
      : WordRecords
          ? "word-record-checks=45056\ncall-effects=4096\nrecord-bits="
            "pass\nrecord-stack=pass\n"
      : SwiftTypeLookup ? "swift-type-lookup-cases=20480\nmetadata-identity="
                          "pass\nbyte-length=pass\n"
      : NativeVoidFrames
          ? "native-void-frame-cases=16384\nrelease-effects=pass\n"
            "independent-results=pass\n"
      : NativeVoid         ? "native-void-cases=16384\nrelease-effects=pass\n"
                             "independent-results=pass\n"
      : NativeFloating     ? "native-floating-cases=24576\nfloating-bits=pass\n"
                             "memory-effects=pass\n"
      : NativeRecords      ? "native-record-result-cases=16384\nbox-storage="
                             "pass\nmetadata-response=pass\n"
      : SwiftRecordRuntime ? "swift-record-runtime-cases=16384\nbox-storage="
                             "pass\nmetadata-response=pass\n"
      : SwiftIntegerRuntime ? "runtime-integer-cases=4096\ncast-results="
                              "pass\nreference-counts=pass\n"
      : DispatchOnce        ? "dispatch-once-calls=8192\ninitializer-effects="
                              "once\nshared-object=pass\n"
      : MutableConstants    ? "mutable-initializers=1024\nshared-cells=pass\n"
                              "initial-identity=pass\nnull-stores=pass\n"
      : CStringStorage
          ? "cstring-checks=4096\ninterior-aliases=pass\nretained-label=pass\n"
      : SwiftOnce ? "swift-once-calls=8192\ninitializer-effects=once\nshared-"
                    "state=pass\n"
      : NativeReturnPaths ? "native-return-cases=16384\nreturns-and-stores="
                            "pass\nwide-leaf-cases=4096\n"
      : IncomingResults   ? "incoming-result-cases=16384\nbit-patterns=pass\n"
                            "memory-input=pass\n"
      : RuntimeIvars      ? "runtime-ivar-cases=16384\nproperty-bits=pass\n"
                            "resilient-field=pass\n"
      : AuxiliaryInputs ? "native-auxiliary-cases=16384\nresult-buffers=pass\n"
                          "scalar-results=pass\n"
      : NativeContext   ? "native-context-cases=16384\ncontext-bits=pass\n"
                          "memory-input=pass\n"
      : ReadOnlyTables  ? "read-only-table-cases=32768\ninteger-bits=pass\n"
                          "floating-bits=pass\nindex-bounds=pass\n"
      : FramePadding ? "frame-padding-cases=16384\nscalar-values=pass\ngetter-"
                       "effects=pass\n"
      : FrameSelectors
          ? "frame-selector-cases=20480\nstrings-and-identity=pass\n"
      : LoopEdges ? "loop-cases=65536\nreturns-and-stores=pass\n"
      : SwitchEffects
          ? "switch-cases=5632\nhash-effects=2816\nshared-targets=pass\n"
      : InvariantLoops
          ? "loop-method-cases=2048\nprobe-checks=2048\nprobe-reads=2048\n"
      : MetadataCalls ? "metadata-responses=4096\nmetadata-identity="
                        "pass\ncomplete-state=pass\n"
      : SystemCalls
          ? "system-c-cases=512\nextended-attribute-roundtrips=512\n"
            "log-decisions=512\nasl-roundtrips=512\nsha256-vectors=512\n"
      : CRecords
          ? (HostArch == "arm64" ? "c-record-checks=8192\nbitmap-effects=1024\n"
                                 : "c-record-checks=2048\n")
      : PredicateFormats ? "predicate-checks=30720\nquoted-placeholders="
                           "pass\nscalar-widths=pass\n"
      : SharedFrameworks ? "framework-calls=2816\nscalar-record-values="
                           "pass\nobject-identity=pass\n"
      : SavedScalars
          ? "scalar-cases=589824\ncall-effects=589824\nknown-bytes=pass\n"
      : ReceiverResults
          ? "receiver-results=5120\nlifetime-checks=1024\nidentity=pass\n"
      : ReceiverAliases
          ? "receiver-aliases=5120\nlifetime-checks=1024\nidentity=pass\n"
      : ReceiverFields
          ? "receiver-fields=4096\nnested-types=pass\nnil-dispatch=pass\n"
      : ReceiverTypes ? "receiver-checks=5120\nclass-instance-abi=pass\nsdk-"
                        "inheritance=pass\n"
      : DynamicProperties ? "dynamic-property-checks=6144\nscalar-widths="
                            "pass\nobject-identity=pass\n"
      : CoreData
          ? "core-data-fetches=1024\ncontext-identity=pass\nfetch-count=16\n"
            "nil-context=pass\n"
      : ScalarConstants ? "scalar-bit-checks=7168\nsigned-zero=pass\nnan-"
                          "payload=pass\nwide-lanes=pass\n"
      : SwiftLiterals   ? "swift-literals=3072\nutf8=pass\nlifetime="
                          "pass\nidentical-objects=0\n"
      : SystemData ? "system-data=9216\nsingletons=pass\nframework-identity="
                     "pass\nlifetime=pass\n"
      : SwiftAllocation    ? "swift-allocation=4096\nalignment=pass\nmemory="
                             "pass\nmetadata=pass\ndestruction=pass\n"
      : ProtocolReferences ? "protocol-references=8192\nregistered-identity="
                             "pass\nconformance=pass\n"
      : IndirectFields ? "indirect-fields=6144\nobject-identity=pass\nlifetime="
                         "pass\ndestroyed=2048\n"
      : StoredStrings
          ? "stored-strings=4096\nobject-identity=pass\nlifetime=pass\n"
      : Graphics ? "graphics-queries=6144\nimage-identity=pass\npng-count=1\n"
      : BlockLifetimes
          ? "escaping-blocks=1024\ncopy-dispose=pass\nmutated-captures=pass\n"
            "conditional-invokes=1024\nconditional-construction=pass\n"
            "synchronous-mutations=1024\n"
      : Foundation ? "variadic-formats=2276\nframework-iterations=2048\narray-"
                     "dictionaries=17\nnil-"
                     "dispatch=pass\n"
      : Protocols  ? "enumerated=132096\nmutations=2048\nloop-mutations=4\n"
                     "integer-bits=4096\nguard-check=pass\n"
      : DiagnosticReports
          ? "diagnostic-runtime=pass\ncontents=pass\ntrap=pass\n"
      : SwiftStrings    ? "swift-string=pass\ncontents=pass\nlifetime=pass\n"
      : UnfairLocks     ? "unfair-locks=pass\ntrylock=pass\nownership=pass\n"
                          "concurrency=pass\n"
      : ConstantObjects ? "constant-object-checks=32768\ncontents-and-aliases="
                          "pass\nconcurrent-initialization=pass\n"
      : ConstantStrings ? "constant-strings=pass\nunicode=pass\nidentity="
                          "pass\nlifetime=pass\n"
      : SwiftCalls
          ? "swift-runtime=pass\nweak=pass\naccess=pass\ndestroyed=128\n"
      : Associations ? "associations=pass\nretain=pass\ncopy=pass\nstatic-"
                       "keys=pass\nclear="
                       "pass\ndestroyed=1\n"
                     : "strong=pass\nweak=pass\ncopy=pass\ndestructor="
                       "pass\ndestroyed=3\n");
}
#endif

TEST(ObjCRuntimeSource, RecompiledARCMethodsPreserveActualObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, AtomicPropertyGettersPreserveObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::AtomicARC));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SummaryPreservesCoverageDiagnosticsAndSharedHelpers) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(
        Chained, RuntimeFixture::DiagnosticReports, false, false, true));
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Associations,
                                          false, false, true));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SummaryRejectsMissingSessionsAndUnloadedImages) {
  EXPECT_EQ(neverd_objc_methods_summary_json(nullptr, 0), nullptr);
  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_TRUE(Session);
  EXPECT_EQ(neverd_objc_methods_json(Session.get(), 0), nullptr);
  std::unique_ptr<const char, decltype(&neverd_free_string)> FullError(
      neverd_last_error(Session.get()), neverd_free_string);
  EXPECT_EQ(neverd_objc_methods_summary_json(Session.get(), 0), nullptr);
  std::unique_ptr<const char, decltype(&neverd_free_string)> SummaryError(
      neverd_last_error(Session.get()), neverd_free_string);
  ASSERT_TRUE(FullError);
  ASSERT_TRUE(SummaryError);
  EXPECT_STREQ(FullError.get(), SummaryError.get());
}

TEST(ObjCRuntimeSource, RecompiledAssociatedObjectsPreserveLifetimeAndPolicy) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::Associations));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledProfiledMethodsKeepSharedCounterStorage) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::Associations, true));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledSwiftRuntimeCallsPreserveWeakObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::SwiftCalls));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledConstantObjectsPreserveGraphsAndConcurrentIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ConstantObjects));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledConstantStringsPreserveContentsAndIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ConstantStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativePointerForwardingPreservesStringsAndOwnership) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativePointers));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledUnfairLocksPreserveOwnershipAndConcurrentUpdates) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::UnfairLocks));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and os_unfair_lock";
#endif
}
TEST(ObjCRuntimeSource,
     RecompiledSwiftStringBridgePreservesContentsAndLifetime) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}
TEST(ObjCRuntimeSource,
     RecompiledFrameworkCallsPreserveValuesAndObjectIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "chained" : "classic");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Foundation));
  }
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledProtocolCallsPreserveEnumerationState) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Protocols));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}
} // namespace

TEST(ObjCRuntimeSource, RecompiledDiagnosticsPreserveMessagesAndTerminalTrap) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DiagnosticReports));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

namespace {
#ifdef __APPLE__
void verifySwiftStorage(bool Chained) {
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-swift-storage", Directory));
  const std::filesystem::path Work(Directory.str().str());
  const auto Cleanup = llvm::make_scope_exit([&] {
    std::error_code Error;
    std::filesystem::remove_all(Work, Error);
  });
  const std::filesystem::path Fixtures(NEVERD_MOBILE_FIXTURE_DIR);
#if defined(__aarch64__) || defined(__arm64__)
  const std::string HostArch = "arm64";
#else
  const std::string HostArch = "x86_64";
#endif
  const auto Original = (Work / "original.dylib").string();
  std::vector<std::string> Compile{
      "/usr/bin/swiftc",
      "-O",
      "-emit-library",
      "-enable-library-evolution",
      "-module-name",
      "NDStorage",
      "-target",
      HostArch + "-apple-macosx13.0",
      (Fixtures / "ObjCSwiftStorage.swift").string(),
      "-o",
      Original};
  if (!Chained)
    Compile.insert(Compile.end(), {"-Xlinker", "-no_fixup_chains"});
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "compile-original"));
  std::unique_ptr<void, decltype(&neverd_session_destroy)> Session(
      neverd_session_create(), neverd_session_destroy);
  ASSERT_TRUE(Session);
  ASSERT_TRUE(neverd_session_load(Session.get(), Original.c_str()));
  std::unique_ptr<const char, decltype(&neverd_free_string)> Raw(
      neverd_objc_methods_json(Session.get(), 0), neverd_free_string);
  ASSERT_TRUE(Raw);
  auto JSON = llvm::json::parse(Raw.get());
  ASSERT_TRUE(bool(JSON));
  const auto *Object = JSON->getAsObject();
  ASSERT_NE(Object, nullptr);
  const auto *Methods = Object->getArray("methods");
  ASSERT_NE(Methods, nullptr);
  std::set<std::string> Remaining{"value", "count"};
  std::vector<std::string> Sources;
  std::string Declarations,
      Install = "static void installRecovered(void) {\n"
                "Class cls = objc_getClass(\"NDSwiftStorage\");\n";
  for (const auto &Value : *Methods) {
    const auto *Method = Value.getAsObject();
    ASSERT_NE(Method, nullptr);
    const auto Selector = Method->getString("selector");
    ASSERT_TRUE(Selector);
    if (!Remaining.count(Selector->str()))
      continue;
    ASSERT_EQ(Method->getString("status"), "recovered") << Raw.get();
    const auto Name = Method->getString("function_name");
    const auto Source = Method->getString("source");
    ASSERT_TRUE(Name && Source);
    EXPECT_TRUE(Source->contains("ivar_getOffset"));
    const auto Path = Work / (Selector->str() + ".c");
    ASSERT_NO_FATAL_FAILURE(write(Path, Source->str()));
    Sources.push_back(Path.string());
    Declarations += "extern void " + Name->str() + "(void);\n";
    Install += "class_replaceMethod(cls, sel_registerName(\"" +
               Selector->str() + "\"), (IMP)" + Name->str() +
               ", method_getTypeEncoding(class_getInstanceMethod(cls, "
               "sel_registerName(\"" +
               Selector->str() + "\"))));\n";
    Remaining.erase(Selector->str());
  }
  ASSERT_TRUE(Remaining.empty());
  ASSERT_NO_FATAL_FAILURE(
      write(Work / "replacements.h", Declarations + Install + "}\n"));
  Compile = {NEVERD_TEST_CLANG,
             "-arch",
             HostArch,
             "-O2",
             "-fno-objc-arc",
             "-framework",
             "Foundation",
             "-I" + Work.string(),
             (Fixtures / "ObjCSwiftStorageHarness.m").string(),
             Original,
             "-o",
             (Work / "baseline").string()};
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-baseline"));
  ASSERT_NO_FATAL_FAILURE(run({Compile.back()}, Work / "baseline"));
  Compile.back() = (Work / "recovered").string();
  Compile.push_back("-DNEVERD_RECOVERED_STORAGE");
  Compile.insert(Compile.end(), Sources.begin(), Sources.end());
  ASSERT_NO_FATAL_FAILURE(run(Compile, Work / "link-recovered"));
  ASSERT_NO_FATAL_FAILURE(
      run({(Work / "recovered").string()}, Work / "recovered"));
  EXPECT_EQ(read(Work / "baseline.out"), read(Work / "recovered.out"));
  EXPECT_EQ(read(Work / "recovered.out"),
            "swift-storage=pass\ngetter-calls=2048\n");
}
#endif
} // namespace

TEST(ObjCRuntimeSource, RecompiledSwiftStoredPropertiesUseRuntimeIvarOffsets) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifySwiftStorage(Chained));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledBlocksPreserveEscapingCapturesAndOwnershipHelpers) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    for (bool Manual : {false, true}) {
      SCOPED_TRACE(Manual ? "manual reference counting" : "ARC");
      for (bool Profiled : {false, true}) {
        SCOPED_TRACE(Profiled ? "instrumented counters" : "uninstrumented");
        ASSERT_NO_FATAL_FAILURE(verifyRuntime(
            Chained, RuntimeFixture::BlockLifetimes, Profiled, Manual));
      }
    }
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledDarwinDeclarationsPreserveIdentitySynchronizationAndScalars) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DarwinDeclarations));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledFrameworkFetchesPreserveContextObjects) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained ? "default fixups" : "classic fixups");
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::CoreData));
  }
#else
  GTEST_SKIP() << "Requires macOS CoreData and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, RecompiledImmutableScalarsPreserveFloatingBitPatterns) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ScalarConstants));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}

TEST(ObjCRuntimeSource, RecompiledGraphicsCallsPreserveValuesAndImageIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Graphics));
  }
#else
  GTEST_SKIP() << "Requires macOS CoreGraphics and ImageIO";
#endif
}

TEST(ObjCRuntimeSource, RecompiledSwiftLiteralsPreserveUTF8AndObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftLiterals));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift compiler";
#endif
}

TEST(ObjCRuntimeSource, RecompiledStoredStringsPreserveCollectionsAndIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::StoredStrings));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation";
#endif
}

TEST(ObjCRuntimeSource, RecompiledSystemDataPreservesRuntimeSingletonIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::SystemData));
  }
#else
  GTEST_SKIP() << "Requires macOS Foundation and public data frameworks";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledIndirectFieldsPreserveRuntimeOffsetsAndOwnership) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::IndirectFields));
  }
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledProtocolReferencesPreserveRegisteredIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ProtocolReferences));
  }
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RecompiledSwiftAllocationPreservesAlignmentMetadataAndDestruction) {
#ifdef __APPLE__
  for (bool Chained : {false, true}) {
    SCOPED_TRACE(Chained);
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftAllocation));
  }
#else
  GTEST_SKIP() << "requires the Darwin Objective-C and Swift runtimes";
#endif
}

TEST(ObjCRuntimeSource, RecompiledFloatingValuesSurviveRuntimeCalls) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::FloatingSaves));
#else
  GTEST_SKIP() << "Objective-C runtime source execution requires macOS";
#endif
}

TEST(ObjCRuntimeSource,
     EqualityProjectionPreservesOwnershipOrRejectsMissingCallEvidence) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::Equality));
#else
  GTEST_SKIP() << "Objective-C runtime source execution requires macOS";
#endif
}

TEST(ObjCRuntimeSource, DynamicPropertyAccessorsExecuteAgainstCoreData) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DynamicProperties));
#else
  GTEST_SKIP() << "Requires macOS CoreData and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     ReceiverDeclarationsPreserveClassInstanceAndInheritedABI) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ReceiverTypes));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, TypedReceiverFieldsPreserveNestedAndNilObjectCalls) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ReceiverFields));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     HomogeneousRecordsPreserveFieldsCallsAndStackArguments) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::AggregateRecords));
#else
  GTEST_SKIP() << "Requires the Darwin ARM64 homogeneous record ABI";
#endif
}

TEST(ObjCRuntimeSource,
     RuntimeReceiverAliasesPreserveValuesAndObjectLifetimes) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ReceiverAliases));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, WordRecordsPreserveBitsPointersCallsAndStackArguments) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::WordRecords));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     InvariantLoopValuesPreserveClassChecksAndGetterEffects) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::InvariantLoops));
#else
  GTEST_SKIP() << "Requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SwiftMetadataCallsPreserveIdentityAndResponseState) {
#if defined(__APPLE__)
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::MetadataCalls));
#else
  GTEST_SKIP() << "Requires the Darwin Objective-C and Swift runtimes";
#endif
}

TEST(ObjCRuntimeSource, SystemCallsPreserveFileAttributesObjectsAndDigests) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SystemCalls));
#else
  GTEST_SKIP() << "Requires Darwin system libraries";
#endif
}

TEST(ObjCRuntimeSource, FixedCRecordsPreserveValuesAndBitmapEffects) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::CRecords));
#else
  GTEST_SKIP() << "Requires Darwin Foundation and CoreGraphics";
#endif
}

TEST(ObjCRuntimeSource, PredicateFormatsPreserveValuesAndQuotedPlaceholders) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::PredicateFormats));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SharedFrameworkCallsPreserveScalarsRecordsAndObjects) {
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SharedFrameworks));
#else
  GTEST_SKIP() << "Requires macOS frameworks and Darwin ARM64 record ABI";
#endif
}

TEST(ObjCRuntimeSource, ReceiverResultTypesPreserveFactoryAndPropertyBehavior) {
#if defined(__APPLE__)
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ReceiverResults));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NarrowSavedParametersPreserveValuesAndCallEffects) {
#if defined(__APPLE__)
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SavedScalars));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SwitchSuccessorsPreserveStoresCallsAndSharedExits) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwitchEffects));
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, LoopContinuationsPreserveExitCopiesAndStores) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::LoopEdges));
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, PrivateFrameSelectorsPreserveStringsAndIdentity) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::FrameSelectors));
#else
  GTEST_SKIP() << "requires the Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativeReturnPathsPreserveBranchResultsAndStores) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativeReturnPaths));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     SwiftFixedRuntimeCallsPreserveMetadataIdentityAndByteLength) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftTypeLookup));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C and Swift runtimes";
#endif
}

TEST(ObjCRuntimeSource,
     PrivateFramePaddingPreservesScalarValuesAndGetterEffects) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::FramePadding));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     ReadOnlyTablesPreserveIntegerFloatingBitsAndIndexBounds) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::ReadOnlyTables));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, IncomingNativeResultsPreserveWordBitsAndMemoryReads) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::IncomingResults));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativeContextParametersPreserveRegisterInputsAndBits) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativeContext));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SwiftIntegerRuntimeCallsPreserveCastsAndRetainCounts) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftIntegerRuntime));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, AuxiliaryNativeInputsPreserveBuffersAndScalarResults) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::AuxiliaryInputs));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     RuntimeIvarOffsetsPreserveSwiftPropertiesAndOpaqueFields) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::RuntimeIvars));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SwiftRuntimeRecordResultsPreserveBoxesAndMetadata) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::SwiftRecordRuntime));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativeRecordMembersPreserveScalarResultsAndEffects) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativeRecords));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, NativeFloatingHelpersPreserveLanesAndMemoryEffects) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativeFloating));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     VoidNativeTailCallsPreserveLifetimesAndRejectResultUses) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::NativeVoid));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     VoidNativeFramesPreserveReleasesAndRejectUnknownResults) {
#ifdef __APPLE__
  for (const bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::NativeVoidFrames));
#else
  GTEST_SKIP() << "requires the actual Darwin Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource, SwiftOncePreservesInitializationAndSharedStorage) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(verifyRuntime(Chained, RuntimeFixture::SwiftOnce));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Swift runtime";
#endif
}

TEST(ObjCRuntimeSource, DispatchOncePreservesCapturedClassAndSharedObject) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::DispatchOnce));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}

TEST(ObjCRuntimeSource,
     CStringStoragePreservesInteriorAliasesAndRetainedLabels) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::CStringStorage));
#else
  GTEST_SKIP() << "Requires macOS Foundation and libdispatch";
#endif
}

TEST(ObjCRuntimeSource,
     MutableConstantInitializersPreserveIdentityAndLaterStores) {
#ifdef __APPLE__
  for (bool Chained : {false, true})
    ASSERT_NO_FATAL_FAILURE(
        verifyRuntime(Chained, RuntimeFixture::MutableConstants));
#else
  GTEST_SKIP() << "Requires macOS Foundation and Objective-C runtime";
#endif
}
