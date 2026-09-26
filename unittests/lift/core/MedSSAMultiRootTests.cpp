//===- MedSSAMultiRootTests.cpp - Multiple-root SSA tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include <set>

namespace {

using namespace neverd;

TEST(MedSSAMultiRoot, MergesDefinitionsFromIndependentSourcesAtAJoin) {
  constexpr va_t EntryVA = 0x1000;
  constexpr va_t ResumeVA = 0x1100;
  constexpr va_t JoinVA = 0x1200;
  constexpr uint64_t EntryValue = 0x11;
  constexpr uint64_t ResumeValue = 0x22;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "two_source_join";
  Low.Blocks.resize(3);

  auto BuildSource = [&](LowBlock &Block, int Id, va_t Address,
                         uint64_t Value) {
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 1;
    Block.Succs = {2};

    LowOp Define;
    Define.Opcode = NdOp::COPY;
    Define.Addr = Address;
    Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
    Define.addInput(NdVar::cst(Value, TRI.PointerSize));
    Block.Ops.push_back(Define);

    LowOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Address;
    Branch.addInput(NdVar::cst(JoinVA, TRI.PointerSize));
    Block.Ops.push_back(Branch);
  };

  BuildSource(Low.Blocks[0], 0, EntryVA, EntryValue);
  BuildSource(Low.Blocks[1], 1, ResumeVA, ResumeValue);

  LowBlock &Join = Low.Blocks[2];
  Join.Id = 2;
  Join.StartAddr = JoinVA;
  Join.EndAddr = JoinVA + 1;
  Join.Preds = {0, 1};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = JoinVA;
  Return.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Join.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedJoin = Med.Blocks[2];
  ASSERT_EQ(MedJoin.Phis.size(), 1u);

  const PhiNode &Phi = MedJoin.Phis.front();
  ASSERT_EQ(Phi.Output.Kind, MedVar::Reg);
  EXPECT_EQ(Phi.Output.RegOff, TRI.IntReturnReg);
  ASSERT_EQ(Phi.Args.size(), 2u);
  EXPECT_EQ(Phi.Args[0].first, 0);
  EXPECT_EQ(Phi.Args[1].first, 1);
  EXPECT_EQ(Phi.Args[0].second, MedVar::makeConst(EntryValue, TRI.PointerSize));
  EXPECT_EQ(Phi.Args[1].second,
            MedVar::makeConst(ResumeValue, TRI.PointerSize));

  ASSERT_FALSE(MedJoin.Ops.empty());
  const MedOp &MedReturn = MedJoin.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  EXPECT_EQ(MedReturn.Inputs[0].Id, Phi.Output.Id);
  EXPECT_EQ(MedReturn.Inputs[0].SSAVer, Phi.Output.SSAVer);
  EXPECT_TRUE(verifyMedFunc(Med, "multi-root-join"));
}

TEST(MedSSAMultiRoot, DoesNotInventADownstreamRootFromBlockOrder) {
  constexpr va_t EntryVA = 0x2000;
  constexpr va_t DownstreamVA = 0x2100;
  constexpr va_t SourceVA = 0x2200;
  constexpr uint64_t SourceValue = 0x33;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "source_scc_order";
  Low.Blocks.resize(3);

  LowBlock &Entry = Low.Blocks[0];
  Entry.Id = 0;
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 1;
  LowOp EntryReturn;
  EntryReturn.Opcode = NdOp::RETURN;
  EntryReturn.Addr = EntryVA;
  Entry.Ops.push_back(EntryReturn);

  // This sink deliberately has a lower block id than its disconnected source.
  // Root discovery must follow the condensation graph, not vector order.
  LowBlock &Downstream = Low.Blocks[1];
  Downstream.Id = 1;
  Downstream.StartAddr = DownstreamVA;
  Downstream.EndAddr = DownstreamVA + 1;
  Downstream.Preds = {2};
  LowOp DownstreamReturn;
  DownstreamReturn.Opcode = NdOp::RETURN;
  DownstreamReturn.Addr = DownstreamVA;
  DownstreamReturn.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Downstream.Ops.push_back(DownstreamReturn);

  LowBlock &Source = Low.Blocks[2];
  Source.Id = 2;
  Source.StartAddr = SourceVA;
  Source.EndAddr = SourceVA + 1;
  Source.Succs = {1};
  LowOp Define;
  Define.Opcode = NdOp::COPY;
  Define.Addr = SourceVA;
  Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  Define.addInput(NdVar::cst(SourceValue, TRI.PointerSize));
  Source.Ops.push_back(Define);
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = SourceVA;
  Branch.addInput(NdVar::cst(DownstreamVA, TRI.PointerSize));
  Source.Ops.push_back(Branch);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedDownstream = Med.Blocks[1];
  ASSERT_TRUE(MedDownstream.Phis.empty());
  ASSERT_FALSE(MedDownstream.Ops.empty());
  const MedOp &MedReturn = MedDownstream.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  ASSERT_TRUE(MedReturn.Inputs[0].isConst());
  EXPECT_EQ(MedReturn.Inputs[0].ConstVal, SourceValue);
  EXPECT_TRUE(verifyMedFunc(Med, "source-scc-order"));
}

TEST(MedTempIdentity, SeparatesReusedLowTempSlotsByWidth) {
  constexpr va_t EntryVA = 0x3000;

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "temp_width_identity";
  Low.Blocks.resize(1);

  LowBlock &Block = Low.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = EntryVA;
  Block.EndAddr = EntryVA + 4;

  LowOp ByteDef;
  ByteDef.Opcode = NdOp::POPCOUNT;
  ByteDef.Addr = EntryVA;
  ByteDef.Output = NdVar::tmp(TmpBase, 1);
  ByteDef.addInput(NdVar::scalar(0x5a, 1));
  Block.Ops.push_back(ByteDef);
  LowOp StoreByte;
  StoreByte.Opcode = NdOp::STORE;
  StoreByte.Addr = EntryVA;
  StoreByte.addInput(NdVar::cst(0x4000, 8));
  StoreByte.addInput(ByteDef.Output);
  Block.Ops.push_back(StoreByte);

  LowOp WideDef;
  WideDef.Opcode = NdOp::POPCOUNT;
  WideDef.Addr = EntryVA + 1;
  WideDef.Output = NdVar::tmp(TmpBase, 2);
  WideDef.addInput(NdVar::scalar(0x1234, 2));
  Block.Ops.push_back(WideDef);
  LowOp StoreWideDef;
  StoreWideDef.Opcode = NdOp::STORE;
  StoreWideDef.Addr = EntryVA + 1;
  StoreWideDef.addInput(NdVar::cst(0x4002, 8));
  StoreWideDef.addInput(WideDef.Output);
  Block.Ops.push_back(StoreWideDef);

  LowOp WideUpdate;
  WideUpdate.Opcode = NdOp::POPCOUNT;
  WideUpdate.Addr = EntryVA + 2;
  WideUpdate.Output = NdVar::tmp(TmpBase, 2);
  WideUpdate.addInput(NdVar::scalar(0xabcd, 2));
  Block.Ops.push_back(WideUpdate);
  LowOp StoreWideUpdate = StoreWideDef;
  StoreWideUpdate.Addr = EntryVA + 2;
  StoreWideUpdate.Inputs[1] = WideUpdate.Output;
  Block.Ops.push_back(StoreWideUpdate);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = EntryVA + 3;
  Block.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  auto FindOp = [&](va_t Addr, NdOp Opcode) -> const MedOp * {
    for (const MedOp &Op : Med.Blocks.front().Ops)
      if (Op.Addr == Addr && Op.Opcode == Opcode)
        return &Op;
    return nullptr;
  };
  const MedOp *MedByteDef = FindOp(EntryVA, NdOp::POPCOUNT);
  const MedOp *MedWideDef = FindOp(EntryVA + 1, NdOp::POPCOUNT);
  const MedOp *MedWideUpdate = FindOp(EntryVA + 2, NdOp::POPCOUNT);
  ASSERT_NE(MedByteDef, nullptr);
  ASSERT_NE(MedWideDef, nullptr);
  ASSERT_NE(MedWideUpdate, nullptr);
  EXPECT_NE(MedByteDef->Output.Id, MedWideDef->Output.Id)
      << "a Med SSA identity must have exactly one value width";
  EXPECT_EQ(MedWideDef->Output.Id, MedWideUpdate->Output.Id);
  EXPECT_NE(MedWideDef->Output.SSAVer, MedWideUpdate->Output.SSAVer);
}

BinaryImage makeFixedSEHFrameImage() {
  constexpr va_t Entry = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x3d;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  // push rdi; sub rsp,128; mov dword [rsp+36],1; nop; jmp join.
  const uint8_t Prologue[] = {0x57, 0x48, 0x81, 0xec, 0x80, 0, 0,
                              0,    0xc7, 0x44, 0x24, 0x24, 1, 0,
                              0,    0,    0x90, 0xeb, 0x1d};
  std::copy(std::begin(Prologue), std::end(Prologue), Text.Data.begin());
  // Handler: eax=[rsp+36]; eax+=20; [rsp+36]=eax; jmp join.
  const uint8_t Handler[] = {0x8b, 0x44, 0x24, 0x24, 0x83, 0xc0, 20,
                             0x89, 0x44, 0x24, 0x24, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  const uint8_t Join[] = {0x8b, 0x44, 0x24, 0x24, 0x48, 0x81, 0xc4,
                          0x80, 0,    0,    0,    0x5f, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x3d;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x3d));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x3d};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 8;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateLarge;
  Alloc.CodeOffset = 8;
  Alloc.StackOffset = 128;
  EH.UnwindOperations.push_back(Alloc);
  UnwindOperation Push;
  Push.Kind = UnwindOperationKind::PushNonVolatile;
  Push.CodeOffset = 1;
  Push.Register = 7;
  EH.UnwindOperations.push_back(Push);
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry + 0x10, Entry + 0x11};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 0x20;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(EH);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

LowFunc decodeFixedSEHFrame(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  return CFGBuilder().build(Img, Dec, Img.Entry, "fixed_seh_frame");
}

// Resolve only copies, constant SP arithmetic and equal-input PHIs. This
// asserts the actual memory operand's entry-SP-relative identity on both
// paths, rather than looking for a particular initializer spelling.
std::optional<int64_t> entrySPOffset(const MedFunc &F, const MedVar &V,
                                     std::set<std::pair<int, int>> Seen = {}) {
  if (V.Kind == MedVar::Reg &&
      V.RegOff == getTargetRegInfo(Arch::X64).StackPointer && V.SSAVer == 0)
    return 0;
  if (!Seen.emplace(V.Id, V.SSAVer).second)
    return std::nullopt;
  for (const MedBlock &B : F.Blocks) {
    for (const PhiNode &Phi : B.Phis) {
      if (Phi.Output.Id != V.Id || Phi.Output.SSAVer != V.SSAVer)
        continue;
      std::optional<int64_t> Result;
      for (const auto &[Pred, Arg] : Phi.Args) {
        auto Offset = entrySPOffset(F, Arg, Seen);
        if (!Offset || (Result && Result != Offset))
          return std::nullopt;
        Result = Offset;
      }
      return Result;
    }
    for (const MedOp &Op : B.Ops) {
      if (Op.Output.Id != V.Id || Op.Output.SSAVer != V.SSAVer)
        continue;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        return entrySPOffset(F, Op.Inputs[0], Seen);
      if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
          Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
        auto Base = entrySPOffset(F, Op.Inputs[0], Seen);
        if (Base)
          return *Base + (Op.Opcode == NdOp::INT_ADD ? 1 : -1) *
                             static_cast<int64_t>(Op.Inputs[1].ConstVal);
      }
    }
  }
  return std::nullopt;
}

TEST(MedSEHEstablisherFrame, NormalHandlerAndJoinUseTheSameLocalSlot) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 8, Img.Entry + 0x20,
                                   Img.Entry + 0x27, Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
}

TEST(MedSEHEstablisherFrame, HighCPreservesTheCertifiedHandlerSlot) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m64 = 1;"), std::string::npos) << Source;
  EXPECT_NE(Source.substr(Handler).find("var_m64 += 20;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.substr(Handler).find("return var_m64;"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("var_mEC"), std::string::npos) << Source;
}

TEST(MedSEHEstablisherFrame, RejectsUncertifiedFramesWithoutAborting) {
  for (unsigned Case = 0; Case != 11; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFixedSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    if (Case == 0)
      EH.UnwindOperations[0].StackOffset = 64;
    if (Case == 1)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Case == 2)
      EH.FrameRegister = 5;
    if (Case == 3)
      EH.ChainedUnwindInfoRVA = 0x500;
    if (Case == 4)
      EH.SEH->Scopes[0].GuardedRange.Begin = Low.Entry + 1;
    if (Case == 5)
      Low.OrdinaryModuleAnalysisRoots.insert(Low.Entry + 0x20);
    if (Case == 6)
      Low.Blocks.back().Succs.push_back(0);
    if (Case == 7)
      Low.Blocks.front().InstructionBoundaries.clear();
    if (Case == 8) {
      // Dynamic SP update in the ordinary prefix before the protected range.
      for (auto &Op : Low.Blocks.front().Ops)
        if (Op.Opcode == NdOp::STORE && Op.Addr == Low.Entry + 8) {
          Op.Opcode = NdOp::COPY;
          Op.Output = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
          Op.NumInputs = 1;
          Op.Inputs[0] =
              NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
        }
    }
    if (Case == 9) {
      for (auto &B : Low.Blocks)
        if (B.StartAddr == Low.Entry + 0x20)
          Low.Blocks.front().Succs.push_back(B.Id);
    }
    if (Case == 10)
      Low.DecodedInstructionCount = uint64_t(limits::kMaxSSANodes) + 1;
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, PipelineReturnsExplicitFailureForUnknownFrame) {
  auto Img = makeFixedSEHFrameImage();
  Img.ExceptionMetadata.Functions.front().UnwindOperations.front().StackOffset =
      64;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.DumpMed = true;
  Opts.OnlyFunctionEntries.insert(Img.Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(Img, Context, Opts);
  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(Result.MedFuncs.empty());
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_NE(Result.Error.find("Windows SEH establisher frame"),
            std::string::npos);
  // The same session/process can still perform a supported conversion.
  auto Valid = makeFixedSEHFrameImage();
  Result = Pipeline().run(Valid, Context, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
}

TEST(MedSEHEstablisherFrame, RejectsConvertedCalleePopEffects) {
  for (bool CallInPrologue : {false, true}) {
    SCOPED_TRACE(CallInPrologue);
    auto Img = makeFixedSEHFrameImage();
    auto &Bytes = Img.Segments.front().Data;
    // CALL entry+0x1000 at +0x10; its ret 8 contract adjusts SP only in
    // converted MedIR. Exercise both a protected prefix and the prologue.
    const uint8_t CallAndJump[] = {0xe8, 0xeb, 0x0f, 0, 0, 0x90, 0xeb, 0x18};
    std::copy(std::begin(CallAndJump), std::end(CallAndJump),
              Bytes.begin() + 0x10);
    auto &EH = Img.ExceptionMetadata.Functions.front();
    EH.SEH->Scopes.front().GuardedRange = {Img.Entry + 0x15, Img.Entry + 0x16};
    if (CallInPrologue)
      EH.PrologueSize = 0x15;
    auto Low = decodeFixedSEHFrame(Img);
    std::map<va_t, int> Pop{{Img.Entry + 0x1000, 8}};
    LowToMedConverter Converter;
    Converter.setCalleePopMap(&Pop);
    EXPECT_THROW(Converter.convert(Low, Arch::X64, BinaryFormat::COFF),
                 LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, PublicNestedSEHUsesOneSlotInMedAndHighC) {
#ifndef NEVERD_BINARY_CORPUS_ROOT
  GTEST_SKIP() << "windows-eh corpus root is not configured";
#else
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "seh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << "public SEH corpus fixture is missing";
  constexpr va_t Entry = 0x1400010b0;
  BinaryLoadOptions LoadOptions;
  LoadOptions.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, LoadOptions);
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(*Img, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto Main = std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                           [&](const MedFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(Main, Result.MedFuncs.end());
  const MedFunc &Med = *Main;
  const std::set<va_t> Expected = {Entry + 0x1d, Entry + 0x81, Entry + 0x88,
                                   Entry + 0x8c};
  std::set<va_t> Seen;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions COptions;
  COptions.TheArch = Arch::X64;
  auto MainHigh =
      std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                   [&](const HighFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(MainHigh, Result.HighFuncs.end());
  ASSERT_TRUE(HighCEmitter().emit({*MainHigh}, Stream, COptions));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_NE(Source.substr(Handler).find("frame_base - 100"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("frame_base - 236"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("frame_base - 272"), std::string::npos) << Source;
#endif
}

} // namespace
