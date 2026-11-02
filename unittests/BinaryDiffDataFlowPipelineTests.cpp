//===- BinaryDiffDataFlowPipelineTests.cpp - Native SSA diff evidence -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Owned native bytes through the real decoder, strict lifter and SSA owner.
/// These fixtures are analyzed without executing the target instructions.
//===----------------------------------------------------------------------===//

#include "neverd/backend/llvm/LLVMSourceMap.h"
#include "neverd/diff/MedDataFlow.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

#include <gtest/gtest.h>
#include <set>

using namespace neverd;
using namespace neverd::diff;
namespace {
DataFlowGraph nativeGraph(Arch Architecture, std::vector<uint8_t> Code,
                          uint64_t Base, bool ImportStub = false) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Base = Image.Entry = Base;
  Segment Segment;
  Segment.VA = Base;
  Segment.Size = Code.size();
  Segment.Data = std::move(Code);
  Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Image.Segments.push_back(std::move(Segment));
  Image.Symbols.push_back(Symbol::makeFunc(Base));
  if (ImportStub)
    Image.ImportStubRanges.emplace_back(Base, Base + Image.Segments[0].Size);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.OnlyFunctionEntries.insert(Base);
  Options.MedAnalysisOnly = true;
  Options.EmitDumpOutput = false;
  auto Result = Pipeline().run(Image, Context, Options);
  EXPECT_TRUE(Result.Success) << Result.Error;
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_FALSE(Result.HighIRDeferred);
  EXPECT_EQ(Result.LlvmModule, nullptr);
  const auto Audit = llvm::find_if(
      Result.FunctionAudits, [&](const auto &A) { return A.Entry == Base; });
  EXPECT_NE(Audit, Result.FunctionAudits.end());
  if (Audit != Result.FunctionAudits.end()) {
    EXPECT_TRUE(Audit->MedIRVerified);
    EXPECT_TRUE(Audit->DecodeFailures.empty());
    EXPECT_TRUE(Audit->UnsupportedInstructions.empty());
  }
  const auto F = llvm::find_if(Result.MedFuncs,
                               [&](const auto &F) { return F.Entry == Base; });
  if (F == Result.MedFuncs.end()) {
    ADD_FAILURE() << "pipeline did not retain the selected native function";
    return {};
  }
  return buildMedDataFlow(*F, "owned-fixture-v1",
                          Architecture == Arch::X64 ? "sysv-x64" : "aapcs64",
                          true);
}
std::vector<uint8_t> x64(uint8_t Add = 3) {
  // return (((unsigned)x + Add) ^ 23) * 7 + 9;
  return {0x89, 0xf8, 0x83, 0xc0, Add,  0x83, 0xf0, 0x17,
          0x6b, 0xc0, 0x07, 0x83, 0xc0, 0x09, 0xc3};
}
uint32_t operation(const DataFlowGraph &G, llvm::StringRef Opcode) {
  for (uint32_t N = 0; N < G.Nodes.size(); ++N)
    if (G.Nodes[N].Opcode == Opcode)
      return N;
  ADD_FAILURE() << "missing operation " << Opcode.str();
  return NoIndex;
}
} // namespace

TEST(BinaryDiffDataFlowPipeline, X64RebaseKeepsSSAKeysAndScalarChangesSurvive) {
  const auto A = nativeGraph(Arch::X64, x64(), 0x1000);
  const auto B = nativeGraph(Arch::X64, x64(), 0x8000);
  const auto C = nativeGraph(Arch::X64, x64(5), 0x8000);
  const auto AMul = operation(A, "INT_MULT"), BMul = operation(B, "INT_MULT"),
             CMul = operation(C, "INT_MULT");
  ASSERT_NE(AMul, NoIndex);
  ASSERT_NE(BMul, NoIndex);
  ASSERT_NE(CMul, NoIndex);
  EXPECT_EQ(A.Nodes[AMul].Address, 0x1008u);
  EXPECT_EQ(B.Nodes[BMul].Address, 0x8008u);
  EXPECT_EQ(fingerprintDataFlow(A).Keys[AMul],
            fingerprintDataFlow(B).Keys[BMul]);
  DataFlowOptions Deep;
  Deep.Rounds = 16;
  EXPECT_NE(fingerprintDataFlow(A, Deep).Keys[AMul],
            fingerprintDataFlow(C, Deep).Keys[CMul]);
  EXPECT_TRUE(fingerprintDataFlow(A).OpenDataFrontiers[AMul]);
  EXPECT_FALSE(fingerprintDataFlow(A, Deep).OpenDataFrontiers[AMul]);
  const auto AAdd = operation(A, "INT_ADD"), CAdd = operation(C, "INT_ADD");
  ASSERT_NE(AAdd, NoIndex);
  ASSERT_NE(CAdd, NoIndex);
  EXPECT_NE(fingerprintDataFlow(A).Keys[AAdd],
            fingerprintDataFlow(C).Keys[CAdd]);
  for (uint32_t N = 0; N < A.Nodes.size(); ++N)
    for (const auto &Input : A.Nodes[N].Inputs)
      if (Input.Type == FlowOperand::Kind::Node)
        EXPECT_NE(Input.Node, N); // An entry declaration is not a data cycle.
  const auto Slice = sliceDataFlow(C, {CMul}, SliceDirection::Backward, true);
  EXPECT_EQ(Slice.State, ComparisonState::Complete);
  EXPECT_TRUE(llvm::any_of(Slice.Nodes, [&](auto N) {
    return C.Nodes[N].Address == 0x8002 &&
           llvm::any_of(C.Nodes[N].Inputs, [](const auto &I) {
             return I.Type == FlowOperand::Kind::Constant && I.Value == 5;
           });
  }));
}

TEST(BinaryDiffDataFlowPipeline, AArch64KeepsNativeOriginsAndItsOwnABI) {
  std::vector<uint8_t> Code;
  for (uint32_t Word : {0x11000c00u, 0x528002e1u, 0x4a010000u, 0x528000e1u,
                        0x1b017c00u, 0x11002400u, 0xd65f03c0u})
    for (unsigned I = 0; I < 4; ++I)
      Code.push_back(uint8_t(Word >> (I * 8)));
  const auto G = nativeGraph(Arch::AArch64, Code, 0x4000);
  const auto Mul = operation(G, "INT_MULT");
  ASSERT_NE(Mul, NoIndex);
  EXPECT_EQ(G.ABI, "aapcs64");
  EXPECT_EQ(G.Nodes[Mul].Address, 0x4010u);
  const auto Slice = sliceDataFlow(G, {Mul}, SliceDirection::Backward, false);
  EXPECT_TRUE(llvm::any_of(
      Slice.Nodes, [&](auto N) { return G.Nodes[N].Address == 0x4000; }));
  EXPECT_EQ(fingerprintDataFlow(G).State, ComparisonState::Complete);
}

TEST(BinaryDiffDataFlowPipeline, SystemVCallArgumentsRetainSetupDependencies) {
  // push rax; lea edi,[rdi+3]; call helper; pop rcx; ret;
  // helper: lea eax,[rdi+5]; ret. Only the caller is selected; the pipeline's
  // existing omitted-callee recovery supplies the helper's argument contract.
  const auto G = nativeGraph(
      Arch::X64, {0x50, 0x8d, 0x7f, 0x03, 0xe8, 0x07, 0x00, 0x00, 0x00, 0x59,
                  0xc3, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0x8d, 0x47, 0x05, 0xc3},
      0x1000);
  const auto Call = operation(G, "CALL");
  ASSERT_NE(Call, NoIndex);
  EXPECT_EQ(G.Nodes[Call].Address, 0x1004u);
  ASSERT_NE(G.Nodes[Call].CallArgumentBegin, NoIndex);
  ASSERT_EQ(G.Nodes[Call].CallArgumentCount, 1u);
  const auto &Argument = G.Nodes[Call].Inputs[G.Nodes[Call].CallArgumentBegin];
  ASSERT_EQ(Argument.Type, FlowOperand::Kind::Node);
  const auto Slice =
      sliceDataFlow(G, {Argument.Node}, SliceDirection::Backward, false);
  EXPECT_EQ(Slice.State, ComparisonState::Complete);
  EXPECT_TRUE(llvm::any_of(Slice.Nodes, [&](auto N) {
    return G.Nodes[N].Address == 0x1001 &&
           llvm::any_of(G.Nodes[N].Inputs, [](const auto &I) {
             return I.Type == FlowOperand::Kind::Constant && I.Value == 3;
           });
  }));
  EXPECT_TRUE(G.Nodes[Call].Gaps & FlowCallEffects);
  EXPECT_TRUE(G.Nodes[Call].Gaps & FlowMemoryAlias);
}

TEST(BinaryDiffDataFlowPipeline, AnalysisRetainsLoaderMarkedImportStubBodies) {
  // Import-stub exclusion belongs to native replacement output. A read-only
  // analysis still retains verified instructions in the loader-marked range.
  const auto G = nativeGraph(Arch::X64, x64(), 0x1000, true);
  EXPECT_NE(operation(G, "INT_MULT"), NoIndex);
}

TEST(BinaryDiffDataFlowPipeline, AnalysisRejectsVMsAndConflictingOutputModes) {
  llvm::LLVMContext Context;
  BinaryImage Image;
  PipelineOptions Options;
  Options.MedAnalysisOnly = true;
  for (auto Architecture : {Arch::EVM, Arch::SBF}) {
    Image.Arch = Architecture;
    const auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_FALSE(Result.Error.empty());
  }
  Image.Arch = Arch::X64;
  for (auto Mode : {&PipelineOptions::PatchMode, &PipelineOptions::LiftMode,
                    &PipelineOptions::DeferHighIR, &PipelineOptions::DeferMedIR,
                    &PipelineOptions::DumpHigh, &PipelineOptions::DumpLlvm,
                    &PipelineOptions::SourceProjection}) {
    auto Conflict = Options;
    Conflict.*Mode = true;
    const auto Result = Pipeline().run(Image, Context, Conflict);
    EXPECT_FALSE(Result.Success);
    EXPECT_FALSE(Result.Error.empty());
  }
}

TEST(BinaryDiffDataFlowPipeline, LLVMRetentionPreservesSerialAndShardedOutput) {
  for (unsigned Count : {1u, 8u}) {
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Base = Image.Entry = 0x1000;
    Segment Segment;
    Segment.VA = Image.Base;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    PipelineOptions Options;
    Options.LiftMode = true;
    Options.EmitDumpOutput = false;
    for (unsigned N = 0; N < Count; ++N) {
      const auto Entry = Image.Base + Segment.Data.size();
      auto Code = x64();
      Image.Symbols.push_back(Symbol::makeFunc(Entry));
      Options.OnlyFunctionEntries.insert(Entry);
      Segment.Data.insert(Segment.Data.end(), Code.begin(), Code.end());
      Segment.Data.resize(Segment.Data.size() + 17, 0xcc);
    }
    Segment.Size = Segment.Data.size();
    Image.Segments.push_back(std::move(Segment));
    llvm::LLVMContext Context;
    const auto Plain = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Plain.Success) << Plain.Error;
    ASSERT_TRUE(Plain.LlvmModule);
    EXPECT_FALSE(Plain.LLVMSources);
    Options.RetainLLVMSourceObservations = true;
    const auto Observed = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Observed.Success) << Observed.Error;
    ASSERT_TRUE(Observed.LlvmModule);
    ASSERT_TRUE(Observed.LLVMSources);
    std::string Before, After;
    llvm::raw_string_ostream B(Before), A(After);
    Plain.LlvmModule->print(B, nullptr);
    Observed.LlvmModule->print(A, nullptr);
    EXPECT_EQ(Before, After);
    ASSERT_EQ(Observed.LLVMSources->Functions.size(), Count);
    std::set<va_t> Retained;
    for (const auto &O : Observed.LLVMSources->Observations) {
      EXPECT_FALSE(O.ExpressionDerived);
      const auto *I = llvm::dyn_cast_or_null<llvm::Instruction>(O.Value);
      if (!I)
        continue;
      const auto Owner = Observed.LLVMSources->Functions.find(O.Function);
      ASSERT_NE(Owner, Observed.LLVMSources->Functions.end());
      EXPECT_EQ(static_cast<llvm::Value *>(Owner->second), I->getFunction());
      EXPECT_EQ(I->getModule(), Observed.LlvmModule.get());
      EXPECT_GE(O.Occurrence.Address, O.Function);
      EXPECT_LT(O.Occurrence.Address, O.Function + x64().size());
      Retained.insert(O.Function);
    }
    EXPECT_EQ(Retained.size(), Count);
  }
}

TEST(BinaryDiffDataFlowPipeline, LLVMAncestryKeepsItsDistinctOriginKind) {
  llvm::LLVMContext Context;
  llvm::Module Module("source-observations", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Type = llvm::FunctionType::get(Builder.getInt32Ty(),
                                       {Builder.getInt32Ty()}, false);
  auto *Function = llvm::Function::Create(
      Type, llvm::GlobalValue::ExternalLinkage, "fixture", Module);
  Builder.SetInsertPoint(llvm::BasicBlock::Create(Context, "entry", Function));
  auto *Add = Builder.CreateAdd(Function->getArg(0), Builder.getInt32(3));
  auto *Mul = Builder.CreateMul(Add, Builder.getInt32(7));
  auto *Xor = Builder.CreateXor(Mul, Builder.getInt32(23));
  Builder.CreateRet(Xor);
  LLVMSourceMap Sources;
  Sources.Functions[0x1000] = Function;
  Sources.Observations = {{0x1000, {0x1000, 0}, Add},
                          {0x1000, {0x1004, 0}, Mul},
                          {0x1000, {0x1008, 0}, Xor}};
  sigs::LibraryRecognition Match;
  Match.Function = 0x1000;
  Match.Isolated = true;
  Match.Occurrences = {{0x1000, 0}, {0x1004, 0}, {0x1008, 0}};
  Sources.preserveExpressionOrigins({Match});
  ASSERT_GT(Sources.Observations.size(), 3u);
  for (size_t N = 0; N < Sources.Observations.size(); ++N)
    EXPECT_EQ(Sources.Observations[N].ExpressionDerived, N >= 3);
  // A real replacement keeps the original observation kind. Derived ancestry
  // does not become direct evidence merely because a clone changes pointers.
  Add->replaceAllUsesWith(Function->getArg(0));
  EXPECT_EQ(static_cast<llvm::Value *>(Sources.Observations[0].Value),
            Function->getArg(0));
  EXPECT_FALSE(Sources.Observations[0].ExpressionDerived);
  Sources.remap([](llvm::Value *V) { return V; });
  for (size_t N = 0; N < Sources.Observations.size(); ++N)
    EXPECT_EQ(Sources.Observations[N].ExpressionDerived, N >= 3);
}

TEST(BinaryDiffDataFlowPipeline,
     AArch64OptimizedLLVMRetainsDirectArithmeticOrigins) {
  // compiler_regions_fixture.c, Clang 23 aarch64-unknown-linux-gnu O0/O2.
  // These owned instructions are lifted, never executed.
  const std::vector<std::vector<uint32_t>> Variants = {
      {0xd10043ff, 0xb9000fe0, 0xb9000be1, 0xb9400fe8, 0xb9400be9, 0x0b090108,
       0x528002e9, 0x4a090108, 0xb90007e8, 0xb94007e8, 0x528000e9, 0x1b097d08,
       0x11002508, 0xb90007e8, 0xb94007e9, 0xb94007e8, 0x53051108, 0x2a491508,
       0xb90007e8, 0xb94007e8, 0xb9400be9, 0x528001aa, 0x1b0a7d29, 0x11004529,
       0x4a090108, 0x11000d00, 0x910043ff, 0xd65f03c0},
      {0x528002e8, 0x0b000029, 0x528001aa, 0x4a080128, 0x5280022b, 0x1b0a2c2a,
       0x531d7109, 0x4b080128, 0x11002508, 0x4ac81548, 0x11000d00, 0xd65f03c0}};
  for (const auto &Words : Variants) {
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.Base = Image.Entry = 0x1000;
    Segment Segment;
    Segment.VA = Image.Base;
    Segment.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    for (auto Word : Words)
      for (unsigned Byte = 0; Byte < 4; ++Byte)
        Segment.Data.push_back(uint8_t(Word >> (Byte * 8)));
    Segment.Size = Segment.Data.size();
    Image.Segments.push_back(std::move(Segment));
    Image.Symbols.push_back(Symbol::makeFunc(Image.Entry));
    PipelineOptions Options;
    Options.OnlyFunctionEntries.insert(Image.Entry);
    Options.LiftMode = true;
    Options.RetainLLVMSourceObservations = true;
    Options.EmitDumpOutput = false;
    llvm::LLVMContext Context;
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.LLVMSources);
    ASSERT_TRUE(Result.LlvmModule);
    std::set<uint64_t> Addresses;
    std::set<const llvm::Instruction *> Values;
    for (const auto &O : Result.LLVMSources->Observations) {
      EXPECT_FALSE(O.ExpressionDerived);
      const auto *I = llvm::dyn_cast_or_null<llvm::Instruction>(O.Value);
      if (!I || !llvm::isa<llvm::BinaryOperator>(I))
        continue;
      EXPECT_EQ(O.Function, Image.Entry);
      EXPECT_EQ(I->getModule(), Result.LlvmModule.get());
      EXPECT_GE(O.Occurrence.Address, Image.Entry);
      EXPECT_LT(O.Occurrence.Address, Image.Entry + Words.size() * 4);
      EXPECT_EQ((O.Occurrence.Address - Image.Entry) % 4, 0u);
      Addresses.insert(O.Occurrence.Address);
      Values.insert(I);
    }
    EXPECT_GE(Addresses.size(), 3u);
    EXPECT_GE(Values.size(), 3u);
    RecordProperty("native_words_" + std::to_string(Words.size()),
                   std::to_string(Addresses.size()) + " direct origins / " +
                       std::to_string(Values.size()) + " arithmetic values");
  }
}
