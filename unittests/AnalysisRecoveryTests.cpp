//===- AnalysisRecoveryTests.cpp - Display/patch recovery separation ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Independent machine-byte fixtures for bounded tentative CFG recovery.
///
//===----------------------------------------------------------------------===//

#include "../lib/ir/high/lower/AnalysisDispatch.h"
#include "../lib/pipeline/PipelineAnalysis.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/LLVMContext.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

namespace neverd::detail {
struct AnalysisRecoveryTestAccess {
  static std::optional<uint32_t> stack(CFGBuilder &Builder,
                                       const BinaryImage &Image, va_t Entry) {
    Builder.CurrentImg = &Image;
    return Builder.analysisCallStackPop(Entry);
  }
  static size_t work(const CFGBuilder &Builder) {
    return Builder.AnalysisStackWork;
  }
  static bool noReturn(CFGBuilder &Builder, const BinaryImage &Image,
                       Decoder &Decode) {
    return Builder.buildNoReturnProof(Image, Decode, Image.Entry,
                                      "display-fixture");
  }
  static bool used(const CFGBuilder &Builder) {
    return Builder.AnalysisRecoveryUsed;
  }
  static std::optional<int64_t>
  delta(CFGBuilder &Builder, const BinaryImage &Image, const LowOp &Call) {
    Builder.CurrentImg = &Image;
    return Builder.analysisCallStackDelta(Call);
  }
  static void registration(CFGBuilder &Builder, const BinaryImage &Image,
                           LowFunc &Function) {
    Builder.CurrentImg = &Image;
    Builder.linkExceptionalSuccessors(Function);
  }
};
} // namespace neverd::detail

namespace neverd {
namespace {

BinaryImage analysisImage() {
  BinaryImage Image;
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Format = BinaryFormat::COFF;
  Image.Base = 0x400000;
  Image.Entry = 0x401000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.resize(0x100, 0xcc);
  const std::vector<uint8_t> Entry{0x55, 0x89, 0xe5, 0xff, 0x24,
                                   0x8d, 0x00, 0x20, 0x40, 0x00};
  std::copy(Entry.begin(), Entry.end(), Text.Data.begin());
  for (unsigned Offset : {0x20u, 0x30u}) {
    Text.Data[Offset] = 0xb8;
    Text.Data[Offset + 1] = Offset;
    std::fill_n(Text.Data.begin() + Offset + 2, 3, 0);
    Text.Data[Offset + 5] = 0xc3;
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Segment Table;
  Table.VA = 0x402000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data = {0x20, 0x10, 0x40, 0x00, 0x30, 0x10, 0x40, 0x00};
  Table.Size = Table.FileSz = Table.Data.size();
  Image.Segments.push_back(Table);
  for (const auto &Seg : Image.Segments) {
    Section Sec;
    Sec.VA = Seg.VA;
    Sec.Size = Sec.FileSz = Seg.Size;
    Sec.Flags = Seg.Flags;
    Sec.Data = Seg.Data;
    Image.Sections.push_back(Sec);
  }
  Image.CodePtrRelocSlots = {0x402000, 0x402004};
  Image.Symbols.push_back(Symbol::makeFunc(Image.Entry));
  return Image;
}

LowFunc buildAnalysis(const BinaryImage &Image, bool Enabled,
                      bool FrameProof = false) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  Builder.setAnalysisRecoveryEnabled(Enabled);
  std::set<va_t> Entries;
  for (const auto &Symbol : Image.Symbols)
    if (Symbol.IsFunc)
      Entries.insert(Symbol.Addr);
  Builder.setKnownFuncEntries(&Entries);
  return FrameProof
             ? Builder.buildIntegerFrameProof(Image, Decode, Image.Entry,
                                              "display-fixture")
             : Builder.build(Image, Decode, Image.Entry, "display-fixture");
}

TEST(AnalysisRecovery, DisplayRecoversTableWithoutInventingASelectorProof) {
  auto Image = analysisImage();
  const auto Strict = buildAnalysis(Image, false);
  EXPECT_TRUE(Strict.JumpTables.empty());
  EXPECT_FALSE(Strict.AnalysisOnly);
  const auto Display = buildAnalysis(Image, true);
  ASSERT_EQ(Display.JumpTables.size(), 1u);
  EXPECT_TRUE(Display.AnalysisOnly);
  EXPECT_TRUE(Display.hasCompleteInstructionLift());
  EXPECT_EQ(Display.DecodedInstructionCount, 7u);
  const auto &Table = Display.JumpTables.front();
  EXPECT_TRUE(Table.AnalysisOnly);
  EXPECT_EQ(Table.Targets, (std::vector<va_t>{0x401020, 0x401030}));
  EXPECT_TRUE(Table.SuppressibleRelocationSlots.empty());
  EXPECT_TRUE(Table.AuthenticatedTableLoads.empty());
  const auto Proof = buildAnalysis(Image, true, true);
  EXPECT_FALSE(Proof.AnalysisOnly);
  EXPECT_TRUE(Proof.JumpTables.empty());
}

TEST(AnalysisRecovery, ChangedStorageAndForeignTargetsDoNotPublishCandidates) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Image = analysisImage();
    if (Mutation == 0)
      Image.CodePtrRelocSlots.erase(0x402004);
    if (Mutation == 1) {
      Image.Segments[1].Flags =
          Image.Segments[1].Flags | SegmentFlags::Writable;
      Image.Sections[1].Flags =
          Image.Sections[1].Flags | SegmentFlags::Writable;
    }
    if (Mutation == 2)
      Image.Symbols.push_back(Symbol::makeFunc(0x401030));
    if (Mutation == 3) {
      Image.Segments[1].Data[4] = 0x04; // Inside the dispatch instruction.
      Image.Sections[1].Data[4] = 0x04;
    }
    if (Mutation == 4)
      Image.Format = BinaryFormat::ELF;
    SCOPED_TRACE(Mutation);
    EXPECT_TRUE(buildAnalysis(Image, true).JumpTables.empty());
  }
}

TEST(AnalysisRecovery, DisplayKeepsAnUnprovedCallReturnSlotBody) {
  auto Image = analysisImage();
  auto &Code = Image.Segments[0].Data;
  const std::vector<uint8_t> Entry{
      0x29, 0xc4,                         // sub esp,eax
      0xe8, 0x79, 0x00, 0x00, 0x00,       // call 0x401080
      0x8b, 0x4c, 0x24, 0xfc,             // mov ecx,[esp-4]
      0xff, 0x24, 0x8d, 0x00, 0x20, 0x40, // jmp [ecx*4+0x402000]
      0x00};
  std::copy(Entry.begin(), Entry.end(), Code.begin());
  Code[0x80] = 0xc3;
  Image.Sections[0].Data = Code;
  Image.Symbols.push_back(Symbol::makeFunc(0x401080, 1));

  PipelineOptions Options;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries = {Image.Entry};
  llvm::LLVMContext Context;
  const auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_EQ(Result.LowFuncs.size(), 1u);
  ASSERT_EQ(Result.MedFuncs.size(), 1u);
  EXPECT_TRUE(Result.LowFuncs.front().AnalysisOnly);
  const auto &Med = Result.MedFuncs.front();
  EXPECT_TRUE(Med.AnalysisOnly);
  EXPECT_TRUE(Med.CallReturnReads.empty());
  size_t Calls = 0, Loads = 0;
  for (const auto &Block : Med.Blocks)
    for (const auto &Op : Block.Ops) {
      Calls += Op.Opcode == NdOp::CALL;
      Loads += Op.Opcode == NdOp::LOAD;
    }
  EXPECT_EQ(Calls, 1u);
  EXPECT_GE(Loads, 1u);
  EXPECT_THROW(MedLLVMEmitter().emit(Result.MedFuncs, Context, "display",
                                     Image.Arch, {}, &Image, Image.Format),
               std::runtime_error);
}

TEST(AnalysisRecovery, FloatingNoReturnProofDoesNotEnableDisplayTables) {
  auto Image = analysisImage();
  // FLDZ forces the no-return checker through its floating-stack rebuild.
  auto &Code = Image.Segments[0].Data;
  Code[0] = 0xd9;
  Code[1] = 0xee;
  Code[2] = 0x90;
  Image.Sections[0].Data = Code;
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  Builder.setAnalysisRecoveryEnabled(true);
  EXPECT_FALSE(
      detail::AnalysisRecoveryTestAccess::noReturn(Builder, Image, Decode));
  EXPECT_FALSE(detail::AnalysisRecoveryTestAccess::used(Builder));
  EXPECT_TRUE(Builder.build(Image, Decode, Image.Entry).AnalysisOnly);
}

TEST(AnalysisRecovery, StackEquationsAgreeAcrossExitsAndStayOutOfProofs) {
  for (unsigned Case = 0; Case != 5; ++Case) {
    auto Image = analysisImage();
    // An opaque call followed by two physical near returns. Display recovery
    // may assume an ordinary returning callee; it may not choose one of two
    // different return pops or invent a return for a loop/indirect tail.
    std::vector<uint8_t> Code{0xff, 0xd0, 0x85, 0xc0, 0x74, 0x03,
                              0xc2, 0x08, 0x00, 0xc2, 0x08, 0x00};
    if (Case == 1)
      Code[10] = 4;
    if (Case == 2)
      Code = {0xff, 0xe0};
    if (Case == 3)
      Code = {0xeb, 0xfe};
    if (Case == 4)
      Code = {0xc3};
    Image.Segments[0].Data = Code;
    Image.Sections[0].Data = Code;
    CFGBuilder Builder;
    EXPECT_FALSE(
        detail::AnalysisRecoveryTestAccess::stack(Builder, Image, Image.Entry));
    Builder.setAnalysisRecoveryEnabled(true);
    const auto Pop =
        detail::AnalysisRecoveryTestAccess::stack(Builder, Image, Image.Entry);
    if (Case == 0 || Case == 4)
      EXPECT_EQ(Pop, Case == 0 ? 8u : 0u);
    else
      EXPECT_FALSE(Pop);
    const size_t Work = detail::AnalysisRecoveryTestAccess::work(Builder);
    EXPECT_EQ(
        detail::AnalysisRecoveryTestAccess::stack(Builder, Image, Image.Entry),
        Pop);
    EXPECT_EQ(detail::AnalysisRecoveryTestAccess::work(Builder), Work);
    EXPECT_LE(Work, limits::ir::kAnalysisStackInstructionsPerCallee + 1);
  }
}

TEST(AnalysisRecovery, BinaryOutputRejectsCopiedTentativeBodies) {
  const auto Image = analysisImage();
  const auto Low = buildAnalysis(Image, true);
  auto Med = LowToMedConverter().convert(Low, Image.Arch, Image.abiFormat());
  ASSERT_TRUE(Med.AnalysisOnly);
  EXPECT_TRUE(MedToHighConverter().convert(Med, Image.Arch).AnalysisOnly);
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  EXPECT_THROW(Emitter.emit({Med}, Context, "display", Image.Arch, {}, &Image,
                            Image.Format),
               std::runtime_error);
  Med.AnalysisOnly = false;
  ASSERT_TRUE(Med.JumpTables.front().AnalysisOnly);
  EXPECT_THROW(Emitter.emit({Med}, Context, "display", Image.Arch, {}, &Image,
                            Image.Format),
               std::runtime_error);
}

TEST(AnalysisRecovery, StackScanHasPerCalleeAndAggregateCeilings) {
  auto Image = analysisImage();
  auto &Text = Image.Segments.front();
  Text.Data.assign(limits::ir::kAnalysisStackInstructionsPerCallee + 32, 0x90);
  Text.Data.back() = 0xc3;
  Text.Size = Text.FileSz = Text.Data.size();
  CFGBuilder Builder;
  Builder.setAnalysisRecoveryEnabled(true);
  for (unsigned Offset = 0; Offset != 24; ++Offset)
    EXPECT_FALSE(detail::AnalysisRecoveryTestAccess::stack(
        Builder, Image, Image.Entry + Offset));
  EXPECT_LE(detail::AnalysisRecoveryTestAccess::work(Builder),
            limits::ir::kAnalysisStackInstructionsPerFunction + 1);
}

TEST(AnalysisRecovery, AllocatingProbeUsesASignedEquationAndKeepsTheCall) {
  auto Image = analysisImage();
  // mov eax,0x1000; call 0x401080. This helper relocates its own return
  // word: its ordinary RET immediate cannot describe the caller's ESP.
  const std::vector<uint8_t> Prefix{0xb8, 0, 0x10, 0, 0, 0xe8, 0x76, 0, 0, 0};
  const std::vector<uint8_t> Probe{
      0x51, 0x8d, 0x4c, 0x24, 0x04, 0x2b, 0xc8, 0x1b, 0xc0, 0xf7, 0xd0,
      0x23, 0xc8, 0x8b, 0xc4, 0x25, 0x00, 0xf0, 0xff, 0xff, 0x3b, 0xc8,
      0x72, 0x0a, 0x8b, 0xc1, 0x59, 0x94, 0x8b, 0x00, 0x89, 0x04, 0x24,
      0xc3, 0x2d, 0x00, 0x10, 0x00, 0x00, 0x85, 0x00, 0xeb, 0xe9};
  auto &Code = Image.Segments[0].Data;
  std::copy(Prefix.begin(), Prefix.end(), Code.begin());
  std::copy(Probe.begin(), Probe.end(), Code.begin() + 0x80);
  Image.Sections[0].Data = Code;
  const auto Original = Code;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = Image.Entry + 5;
  Call.NumInputs = 1;
  Call.Inputs[0] = NdVar::scalar(Image.Entry + 0x80, 4);
  CFGBuilder Builder;
  EXPECT_FALSE(detail::AnalysisRecoveryTestAccess::delta(Builder, Image, Call));
  Builder.setAnalysisRecoveryEnabled(true);
  EXPECT_EQ(detail::AnalysisRecoveryTestAccess::delta(Builder, Image, Call),
            -4096);
  EXPECT_FALSE(detail::AnalysisRecoveryTestAccess::stack(Builder, Image,
                                                         Image.Entry + 0x80));
  EXPECT_EQ(Call.Opcode, NdOp::CALL);
  EXPECT_EQ(Code, Original);
  Code[0x80] = 0x50;
  Image.Sections[0].Data = Code;
  EXPECT_FALSE(detail::AnalysisRecoveryTestAccess::delta(Builder, Image, Call));
}

TEST(AnalysisRecovery, ReusingBuilderDoesNotKeepTentativeEdges) {
  auto Image = analysisImage();
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Image));
  CFGBuilder Builder;
  Builder.setAnalysisRecoveryEnabled(true);
  EXPECT_EQ(Builder.build(Image, Decode, Image.Entry).JumpTables.size(), 1u);
  Builder.setAnalysisRecoveryEnabled(false);
  const auto Strict = Builder.build(Image, Decode, Image.Entry);
  EXPECT_FALSE(Strict.AnalysisOnly);
  EXPECT_TRUE(Strict.JumpTables.empty());
  Builder.setAnalysisRecoveryEnabled(true);
  Image.CodePtrRelocSlots.clear();
  EXPECT_TRUE(Builder.build(Image, Decode, Image.Entry).JumpTables.empty());
}

TEST(AnalysisRecovery, OversizedTablesDoNotPublishAnArbitraryPrefix) {
  auto Image = analysisImage();
  auto &Table = Image.Segments[1];
  Table.Data.clear();
  Image.CodePtrRelocSlots.clear();
  for (unsigned I = 0; I <= limits::ir::kAnalysisTableSlots; ++I) {
    Table.Data.insert(Table.Data.end(), {0x20, 0x10, 0x40, 0x00});
    Image.CodePtrRelocSlots.insert(Table.VA + I * 4);
  }
  Table.Size = Table.FileSz = Table.Data.size();
  Image.Sections[1].Data = Table.Data;
  Image.Sections[1].Size = Image.Sections[1].FileSz = Table.Data.size();
  EXPECT_TRUE(buildAnalysis(Image, true).JumpTables.empty());
}

TEST(AnalysisRecovery, OrdinaryBodiesRemainIdenticalAcrossFormats) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto Image = analysisImage();
      Image.Arch = Architecture;
      Image.Bits =
          Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
      Image.Format = Format;
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << ':' << unsigned(Format));
      Image.Segments[0].Data = {0x31, 0xc0, 0xc3};
      Image.Sections[0].Data = Image.Segments[0].Data;
      if (Format == BinaryFormat::MachO)
        Image.Sections[0].Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
      Image.CodePtrRelocSlots.clear();
      const auto Strict = buildAnalysis(Image, false);
      const auto Display = buildAnalysis(Image, true);
      EXPECT_FALSE(Display.AnalysisOnly);
      ASSERT_EQ(Display.Blocks.size(), Strict.Blocks.size());
      EXPECT_EQ(Display.DecodedInstructionCount, 2u);
      for (size_t I = 0; I < Strict.Blocks.size(); ++I) {
        EXPECT_EQ(Display.Blocks[I].Ops, Strict.Blocks[I].Ops);
        EXPECT_EQ(Display.Blocks[I].Succs, Strict.Blocks[I].Succs);
      }
    }
}

TEST(AnalysisRecovery, AdjacentTablesUseTheirOwnRelocatedDispatchBoundaries) {
  for (bool Canonical : {false, true})
    for (unsigned Provenance = 0; Provenance != 4; ++Provenance) {
      SCOPED_TRACE(::testing::Message() << Canonical << ':' << Provenance);
      auto Image = analysisImage();
      auto &Table = Image.Segments[1];
      for (unsigned I = 2; I != 300; ++I) {
        Table.Data.insert(Table.Data.end(), {0x20, 0x10, 0x40, 0x00});
        Image.CodePtrRelocSlots.insert(Table.VA + I * 4);
      }
      Table.Size = Table.FileSz = Table.Data.size();
      Image.Sections[1].Data = Table.Data;
      Image.Sections[1].Size = Image.Sections[1].FileSz = Table.Data.size();
      // A second physical dispatch names the neighboring table. It need not
      // yet be reachable, but a generic data reference cannot split the run.
      const std::vector<uint8_t> Branch{
          uint8_t(Canonical ? 0xff : 0x90), 0x24, 0x8d, 0x08, 0x20, 0x40, 0x00};
      std::copy(Branch.begin(), Branch.end(),
                Image.Segments[0].Data.begin() + 0x60);
      Image.Sections[0].Data = Image.Segments[0].Data;
      if (Provenance == 0)
        Image.CodePtrRelocSlots.insert(Image.Entry + 0x63);
      else {
        RelocatedAddressField Field;
        Field.Width = 4;
        Field.TargetVA = Field.EncodedValue = Table.VA + 8;
        if (Provenance == 3)
          ++Field.EncodedValue;
        auto &Fields = Provenance == 2 ? Image.DataAddressRelocOperands
                                       : Image.CodeAddressRelocOperands;
        Fields.emplace(Image.Entry + 0x63, Field);
      }
      const auto Display = buildAnalysis(Image, true);
      if (!Canonical || Provenance == 3) {
        EXPECT_TRUE(Display.JumpTables.empty());
        continue;
      }
      ASSERT_EQ(Display.JumpTables.size(), 1u);
      EXPECT_EQ(Display.JumpTables.front().Targets.size(), 2u);
      EXPECT_TRUE(Display.JumpTables.front().AnalysisOnly);
      EXPECT_EQ(Display.DecodedInstructionCount, 7u);
    }
}

TEST(AnalysisRecovery, PipelineModeOwnsTheOptIn) {
  PipelineOptions Options;
  EXPECT_TRUE(Options.allowsAnalysisRecovery());
  Options.PatchMode = true;
  EXPECT_FALSE(Options.allowsAnalysisRecovery());
  Options.PatchMode = false;
  Options.LiftMode = true;
  Options.SourceProjection = true;
  EXPECT_FALSE(Options.allowsAnalysisRecovery());
  Options.LiftMode = false;
  Options.DumpLlvm = true;
  EXPECT_FALSE(Options.allowsAnalysisRecovery());
  Options.DumpLlvm = false;
  Options.MedAnalysisOnly = true;
  EXPECT_FALSE(Options.allowsAnalysisRecovery());
}

TEST(AnalysisRecovery, DisplayRetainsMetadataWithoutNativeFrameClaims) {
  const auto Image = analysisImage();
  LowFunc Function;
  Function.Entry = Image.Entry;
  Function.ExceptionMetadata.emplace();
  Function.ExceptionMetadata->CodeRange = {Image.Entry, Image.Entry + 0x40};
  Function.ExceptionMetadata->Registration.emplace();
  Function.ExceptionMetadata->Cxx.emplace();
  Function.Blocks.resize(2);
  CFGBuilder Builder;
  Builder.setAnalysisRecoveryEnabled(true);
  detail::AnalysisRecoveryTestAccess::registration(Builder, Image, Function);
  ASSERT_TRUE(Function.RegistrationStates);
  EXPECT_TRUE(Function.AnalysisOnly);
  EXPECT_EQ(Function.Blocks.size(), 2u);
  EXPECT_TRUE(Function.ExceptionMetadata->Registration);
  EXPECT_TRUE(Function.ExceptionMetadata->Cxx);
  EXPECT_FALSE(Function.RegistrationStates->Complete);
  EXPECT_FALSE(Function.RegistrationStates->RegistrationLifetimeComplete);
  EXPECT_FALSE(Function.RegistrationStates->CallFrameEffectsComplete);
  EXPECT_FALSE(Function.RegistrationStates->Diagnostics.empty());
}

TEST(AnalysisRecovery, PatchAfterDisplayUsesAnIndependentStrictBody) {
  auto Image = analysisImage();
  llvm::LLVMContext Context;
  PipelineAnalysisCache Cache;
  PipelineOptions Options;
  Options.AnalysisCache = &Cache;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Image.Entry);
  auto Display = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Display.Success) << Display.Error;
  ASSERT_EQ(Display.LowFuncs.size(), 1u);
  EXPECT_TRUE(Display.LowFuncs.front().AnalysisOnly);
  const auto Roots = Image.CodeRefTargets;
  Options.PatchMode = true;
  Options.NoOpt = true;
  auto Patch = Pipeline().run(Image, Context, Options);
  ASSERT_EQ(Patch.LowFuncs.size(), 1u);
  EXPECT_FALSE(Patch.LowFuncs.front().AnalysisOnly);
  EXPECT_TRUE(Patch.LowFuncs.front().JumpTables.empty());
  EXPECT_EQ(Image.CodeRefTargets, Roots);
}

TEST(AnalysisRecovery, NativeStateRefusalRetainsDecodedAnalysisOperations) {
  auto Image = analysisImage();
  // A state restore supplies unknown x87 tags. The following FILD is decoded,
  // but its empty push destination is not a native-state proof.
  Image.Segments[0].Data = {0xdd, 0x20, 0xdb, 0x00, 0xc3};
  Image.Sections[0].Data = Image.Segments[0].Data;
  const auto Strict = buildAnalysis(Image, false);
  ASSERT_FALSE(Strict.hasCompleteInstructionLift());
  EXPECT_EQ(Strict.DecodedInstructionCount, Strict.LiftedInstructionCount);
  EXPECT_TRUE(pipeline_detail::hasAnalysisInstructionBody(Strict));
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Image.Entry);
  const auto Display = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Display.Success) << Display.Error;
  ASSERT_EQ(Display.LowFuncs.size(), 1u);
  EXPECT_TRUE(Display.LowFuncs.front().AnalysisOnly);
  EXPECT_FALSE(Display.LowFuncs.front().hasCompleteInstructionLift());
  EXPECT_EQ(Display.LowFuncs.front().UnsupportedInstructionAddresses,
            Strict.UnsupportedInstructionAddresses);
  ASSERT_EQ(Display.MedFuncs.size(), 1u);
  EXPECT_TRUE(Display.MedFuncs.front().AnalysisOnly);
  EXPECT_EQ(Display.MedIRVerifierFailures, 0u);
  Options.PatchMode = true;
  const auto Patch = Pipeline().run(Image, Context, Options);
  EXPECT_TRUE(Patch.LowFuncs.empty());
  ASSERT_EQ(Patch.FunctionAudits.size(), 1u);
  EXPECT_EQ(Patch.FunctionAudits.front().Disposition,
            PipelineFunctionDisposition::RejectedIncomplete);

  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    auto Broken = Strict;
    if (Mutation == 0)
      --Broken.LiftedInstructionCount;
    if (Mutation == 1)
      Broken.DecodeFailureAddresses.push_back(Image.Entry);
    if (Mutation == 2)
      Broken.UnprovenReturnAddresses.push_back(Image.Entry);
    if (Mutation == 3)
      Broken.Blocks.clear();
    if (Mutation == 4)
      for (auto &Block : Broken.Blocks)
        for (auto &Op : Block.Ops)
          Op.Opcode = NdOp::NOP;
    SCOPED_TRACE(Mutation);
    EXPECT_FALSE(pipeline_detail::hasAnalysisInstructionBody(Broken));
  }
}

TEST(AnalysisRecovery, CheckedExternalCallbacksStayInTheAnalysisSelection) {
  auto Image = analysisImage();
  Image.Segments[0].Data[0] = 0xc3;
  Image.Sections[0].Data = Image.Segments[0].Data;
  auto &Table = Image.Segments[1];
  Table.Data.assign(0x40, 0);
  const auto Put = [&](size_t Offset, uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      Table.Data[Offset + I] = uint8_t(Value >> (I * 8));
  };
  Put(0, 0x19930520);
  Put(4, 1);
  Put(8, uint32_t(Table.VA + 0x30));
  Put(0x30, UINT32_MAX);
  Put(0x34, uint32_t(Image.Entry + 0x20));
  Table.Size = Table.FileSz = Table.Data.size();
  Image.Sections[1].Data = Table.Data;
  Image.Sections[1].Size = Image.Sections[1].FileSz = Table.Size;
  Image.CodePtrRelocSlots.clear();
  const auto Records =
      coff_loader::getCheckedX86CxxFuncInfoRecords(Image, Table.VA);
  ASSERT_TRUE(Records);
  ExceptionFunction Owner;
  Owner.CodeRange = {Image.Entry, Image.Entry + 0x10};
  Owner.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  Owner.Personality = ExceptionPersonality::CxxFrameHandlerX86;
  Owner.HandlerDataVA = Table.VA;
  Owner.Cxx = Records->Cxx;
  Owner.Registration.emplace();
  Owner.Registration->ScopeTableVA = Table.VA;
  // This unvalidated handler address is not an additional analysis root.
  Owner.Registration->HandlerVA = Image.Entry + 0x30;
  Image.ExceptionMetadata.Functions.push_back(Owner);
  Image.ExceptionMetadata.rebuildIndex();
  PipelineOptions Options;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Image.Entry);
  ASSERT_EQ(pipeline_detail::analysisCallbackEntries(Image, Options),
            (std::set<va_t>{Image.Entry + 0x20}));
  llvm::LLVMContext Context;
  const auto SymbolCount = Image.Symbols.size();
  const auto Roots = Image.CodeRefTargets;
  auto Display = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Display.Success) << Display.Error;
  ASSERT_EQ(Display.LowFuncs.size(), 2u);
  const auto &Callback = Display.LowFuncs.back();
  EXPECT_EQ(Callback.Entry, Image.Entry + 0x20);
  EXPECT_TRUE(Callback.AnalysisOnly);
  EXPECT_EQ(Callback.DecodedInstructionCount, 2u);
  EXPECT_EQ(Image.Symbols.size(), SymbolCount);
  EXPECT_EQ(Image.CodeRefTargets, Roots);
  Options.PatchMode = true;
  EXPECT_TRUE(pipeline_detail::analysisCallbackEntries(Image, Options).empty());
  const auto Patch = Pipeline().run(Image, Context, Options);
  ASSERT_EQ(Patch.LowFuncs.size(), 1u);
  EXPECT_EQ(Patch.LowFuncs.front().Entry, Image.Entry);
  EXPECT_FALSE(Patch.LowFuncs.front().AnalysisOnly);
  Options.PatchMode = false;
  Options.OnlyFunctionEntries = {Image.Entry + 1};
  EXPECT_TRUE(pipeline_detail::analysisCallbackEntries(Image, Options).empty());
  Options.OnlyFunctionEntries = {Image.Entry};
  Put(0x34, uint32_t(Image.Entry + 0x30));
  Image.Sections[1].Data = Table.Data;
  EXPECT_TRUE(pipeline_detail::analysisCallbackEntries(Image, Options).empty());
}

TEST(AnalysisRecovery, X87DisplayDoesNotAcceptMalformedCallOperands) {
  MedFunc Function;
  Function.AnalysisOnly = true;
  Function.Blocks.resize(1);
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.X87ImportArguments = 1;
  Call.X87ArgumentInput = 1;
  Call.NumInputs = 2;
  Call.Inputs[0].Size = 4;
  Call.Inputs[1].Size = x86reg::FPURegSize;
  Function.Blocks[0].Ops.push_back(Call);
  EXPECT_NO_THROW(MedToHighConverter::validateCallContracts(Function));
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    auto Broken = Function;
    auto &Operation = Broken.Blocks[0].Ops.front();
    if (Mutation == 0)
      Broken.AnalysisOnly = false;
    if (Mutation == 1)
      Operation.Opcode = NdOp::NOP;
    if (Mutation == 2)
      Operation.X87ArgumentInput = 0;
    if (Mutation == 3)
      Operation.NumInputs = 1;
    if (Mutation == 4)
      Operation.Inputs[1].Size = 8;
    if (Mutation == 5)
      Operation.Output.Size = 4;
    SCOPED_TRACE(Mutation);
    EXPECT_THROW(MedToHighConverter::validateCallContracts(Broken),
                 std::runtime_error);
  }
}

TEST(AnalysisRecovery, UnprovedSelectorKeepsCandidateBodiesAndUnknownTarget) {
  auto Image = analysisImage();
  for (unsigned Offset : {0x20u, 0x30u}) {
    const std::vector<uint8_t> Store{
        0xc7, 0x05, 0x00, 0x30, 0x40, 0x00, uint8_t(Offset), 0, 0, 0, 0xc3};
    std::copy(Store.begin(), Store.end(),
              Image.Segments[0].Data.begin() + Offset);
  }
  Image.Sections[0].Data = Image.Segments[0].Data;
  const auto Low = buildAnalysis(Image, true);
  auto Med = LowToMedConverter().convert(Low, Image.Arch, Image.abiFormat());
  ASSERT_EQ(Med.JumpTables.size(), 1u);
  const va_t Dispatch = Med.JumpTables.front().InsnAddr;
  Med.UnsafeIndirectBranchAddresses.insert(Dispatch);
  MedToHighConverter Convert;
  Convert.setBinaryImage(&Image);
  const auto High = Convert.convert(Med, Image.Arch);
  std::set<uint64_t> Stored, Cases;
  unsigned Unknown = 0;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    if (Statement.Kind == StmtKind::Store && Statement.StoreVal &&
        Statement.StoreVal->Kind == ExprKind::Const)
      Stored.insert(Statement.StoreVal->ConstVal);
    if (Statement.Kind == StmtKind::Switch)
      for (const auto &Case : Statement.Cases)
        Cases.insert(Case.Value);
    if (Statement.Kind == StmtKind::Call && Statement.CallExpr &&
        Statement.CallExpr->CallTarget == "__neverd_analysis_indirect_jump") {
      ++Unknown;
      EXPECT_TRUE(Statement.CallExpr->DoesNotReturn);
    }
    if (Statement.Kind == StmtKind::Goto)
      EXPECT_NE(Statement.GotoTarget, InvalidVA);
  });
  EXPECT_EQ(Stored, (std::set<uint64_t>{0x20, 0x30}));
  EXPECT_EQ(Cases, (std::set<uint64_t>{0x401020, 0x401030}));
  EXPECT_EQ(Unknown, 1u);
  EXPECT_TRUE(Med.UnsafeIndirectBranchAddresses.count(Dispatch));
  EXPECT_TRUE(High.SwitchLabelsByJump.empty());
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  Options.Image = &Image;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  ASSERT_TRUE(HighCEmitter().emit({High}, Output, Options));
  EXPECT_NE(Source.find("; /* 0x401003: jmp dword ptr [ecx*4 + 0x402000]; "
                        "unresolved target */"),
            std::string::npos);
  Options.EmitComments = false;
  Source.clear();
  ASSERT_TRUE(HighCEmitter().emit({High}, Output, Options));
  EXPECT_EQ(Source.find("unresolved target"), std::string::npos);
  llvm::LLVMContext Context;
  EXPECT_THROW(MedLLVMEmitter().emit({Med}, Context, "display", Image.Arch, {},
                                     &Image, Image.Format),
               std::runtime_error);
}

TEST(AnalysisRecovery, DisplayDispatchDoesNotClearDamagedTableEvidence) {
  HighFunc Function;
  Function.AnalysisOnly = true;
  MedOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = 0x401010;
  Branch.NumInputs = 1;
  Branch.Inputs[0].Size = 4;
  MedVar Snapshot;
  Snapshot.Kind = MedVar::Temp;
  Snapshot.Id = 10;
  Snapshot.Size = 4;
  JumpTable Table;
  Table.InsnAddr = Branch.Addr;
  Table.AnalysisOnly = true;
  Table.Targets = {0x401020, 0x401030, 0x401020};
  EXPECT_TRUE(high_detail::appendAnalysisDispatch(
      Function, Branch, Table, HighExpr::makeVar(Branch.Inputs[0]), Snapshot));
  ASSERT_EQ(Function.Body.size(), 2u);
  EXPECT_EQ(Function.Body.back().Cases.size(), 2u);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    HighFunc Output;
    Output.AnalysisOnly = Mutation != 0;
    auto Changed = Table;
    if (Mutation == 1)
      Changed.AnalysisOnly = false;
    if (Mutation == 2)
      Changed.MutatedUnsafe = true;
    if (Mutation == 3)
      ++Changed.InsnAddr;
    if (Mutation == 4)
      Changed.Targets.push_back(InvalidVA);
    if (Mutation == 5)
      Changed.Targets.resize(limits::ir::kAnalysisTableSlots + 1, 0x401020);
    EXPECT_FALSE(high_detail::appendAnalysisDispatch(
        Output, Branch, Changed, HighExpr::makeVar(Branch.Inputs[0]),
        Snapshot));
    EXPECT_TRUE(Output.Body.empty());
  }
}

} // namespace
} // namespace neverd
