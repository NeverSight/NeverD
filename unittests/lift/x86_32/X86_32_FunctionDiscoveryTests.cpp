//===- X86_32_FunctionDiscoveryTests.cpp - PE gap function recovery ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/LLVMContext.h"

#include <algorithm>

using namespace neverd;

namespace {

BinaryImage makePeText(std::vector<uint8_t> Bytes, va_t Base = 0x401000) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = Base;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Base;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Img.Segments.push_back(Text);

  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Base;
  Sec.Size = Img.Segments[0].Size;
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Sec.Type = 0x60000020;
  Img.Sections.push_back(Sec);
  Img.KnownCodeRanges.emplace_back(Base, Base + Img.Segments[0].Size);
  return Img;
}

} // namespace

TEST(X86_32_FunctionDiscovery, RecoversAlignedUnsymbolizedStackPrologue) {
  // Entry at 0x401000 is a tiny EBP frame.  An unsymbolized callee sits at
  // the next 16-byte boundary with `sub esp, imm32` and is not called.
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x81, 0xEC, 0x20, 0x00, 0x00,
                            0x00, 0x83, 0xC4, 0x20, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  const bool FoundEntry =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401000; });
  const bool FoundHidden =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401020; });
  EXPECT_TRUE(FoundEntry);
  EXPECT_TRUE(FoundHidden) << "expected 16-byte-aligned sub-esp prologue";
}

TEST(X86_32_FunctionDiscovery, DoesNotPromoteInt3PaddingAsAFunction) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == 0x401010; }),
            0);
}

static bool hasEntry(const std::vector<std::pair<va_t, std::string>> &Functions,
                     va_t Addr) {
  return std::any_of(Functions.begin(), Functions.end(),
                     [Addr](const auto &F) { return F.first == Addr; });
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedFramePrologue) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x09);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401009))
      << "expected unaligned push-ebp frame prologue";
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedPushRegAndSehPrologues) {
  std::vector<uint8_t> Bytes(0x50, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t PushReg[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24,
                             0x10, 0x5F, 0x5E, 0x5B, 0xC3};
  std::copy(PushReg, PushReg + sizeof(PushReg), Bytes.begin() + 0x20);
  const uint8_t Seh[] = {0x6A, 0xFF, 0x58, 0xC3};
  std::copy(Seh, Seh + sizeof(Seh), Bytes.begin() + 0x40);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned push-ebx/esi/edi prologue";
  EXPECT_TRUE(hasEntry(Functions, 0x401040))
      << "expected 16-byte-aligned SEH push -1 prologue";
}

TEST(X86_32_FunctionDiscovery,
     RecoversLargeDirectCallWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  Bytes[0] = 0xE8;
  writeLE<uint32_t>(Bytes.data() + 1, 3); // call 0x401008
  Bytes[5] = 0xC3;
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "direct CALL target must survive a verify walk that exhausts "
         "kMaxVerifyInsns";
}

TEST(X86_32_FunctionDiscovery,
     RecoversRelocBackedLeafWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Segment Data;
  Data.Name = ".rdata";
  Data.VA = 0x402000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x401008);
  Img.Segments.push_back(std::move(Data));
  Img.CodePtrRelocSlots.insert(0x402000);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "PE HIGHLOW code-pointer target must survive verify budget exhaustion";
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedStackAdjustAfterStdcallRet) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0xC2, 0x08, 0x00};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x83, 0xEC, 0x08, 0x33, 0xC0, 0x83,
                            0xC4, 0x08, 0xC2, 0x08, 0x00};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x06);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401006))
      << "expected packed unaligned sub-esp prologue after ret imm16";
}

TEST(X86_32_FunctionDiscovery,
     SeedsPrimaryExceptionFunctionInsideCoarseKnownRange) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  Bytes[0] = 0xC3;
  Bytes[0x10] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges = {{0x401000, 0x401020}};
  ExceptionFunction EH;
  EH.CodeRange = {0x401010, 0x401012};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401010))
      << "pdata/registration primary starts are functions even inside a coarse "
         "KnownCodeRange";
}

class SizedFuncDebugContext : public DebugContext {
public:
  FunctionSym Function;
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    if (Addr == Function.Addr)
      return Function;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {Function}; }
  bool hasInfo() const override { return true; }
};

TEST(X86_32_FunctionDiscovery, PipelineDropsInteriorsInsideDebugExtents) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55, 0x8B, 0xEC,
                          0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD, 0xFF, 0xFF, 0xFF,
                            0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  SizedFuncDebugContext Dbg;
  Dbg.Function.Name = "covering";
  Dbg.Function.Addr = 0x401000;
  Dbg.Function.Size = sizeof(Leaf);

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  PipelineResult Result = Pipeline().run(Img, Ctx, Opts, &Dbg);
  EXPECT_TRUE(std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                          [](const HighFunc &F) { return F.Entry == 0x401000; }));
  EXPECT_FALSE(std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                           [](const HighFunc &F) { return F.Entry == 0x401005; }))
      << "debug-sized function must keep interior CALL targets as blocks";
  const auto Covering = std::find_if(
      Result.HighFuncs.begin(), Result.HighFuncs.end(),
      [](const HighFunc &F) { return F.Entry == 0x401000; });
  ASSERT_NE(Covering, Result.HighFuncs.end());
  EXPECT_FALSE(Covering->Body.empty());
}

TEST(X86_32_FunctionDiscovery, DropsCallTargetsInsideSizedFunctionSymbol) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  // 11-byte leaf: push imm32 then a nested-looking EBP frame.  A later caller
  // targets the interior frame; the sized symbol still owns the bytes.
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55,
                          0x8B, 0xEC, 0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  // push ebp; mov ebp, esp; call 0x401005; pop ebp; ret
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD,
                            0xFF, 0xFF, 0xFF, 0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Symbol Covering = Symbol::makeFunc(0x401000, sizeof(Leaf));
  Covering.Name = "covering";
  Img.Symbols.push_back(std::move(Covering));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401020));
  EXPECT_FALSE(hasEntry(Functions, 0x401005))
      << "direct-call target inside a sized function symbol is an interior "
         "block";

  std::set<va_t> Entries;
  for (const auto &F : Functions)
    Entries.insert(F.first);
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto LeafIR = CFG.build(Img, Dec, 0x401000, "covering");
  EXPECT_GE(LeafIR.DecodedInstructionCount, 2u)
      << "CFG must not stop at a dropped interior start 5 bytes in";
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedThiscallVtableStoreThunk) {
  std::vector<uint8_t> Bytes(0x30, 0x90);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Thunk[] = {0xC7, 0x01, 0x00, 0x20, 0x40, 0x00, 0xC3};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned mov [ecx], imm32 destructor thunk";
}

TEST(X86_32_FunctionDiscovery, DecompileIncludesImportStubs) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // jmp dword ptr [0x402000]
  const uint8_t Thunk[] = {0xFF, 0x25, 0x00, 0x20, 0x40, 0x00};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = 0x402000;
  Img.Imports.push_back(Imp);
  ASSERT_TRUE(Img.recordImportStub(0x401000, 0));
  ASSERT_TRUE(Img.isImportStubAt(0x401000));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  const bool Found =
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401000; });
  EXPECT_TRUE(Found) << "decompile must emit import thunks, not skip them";
}

TEST(X86_32_FunctionDiscovery, SkipsSsaWhenInstructionCountExceedsCap) {
  LowFunc Low;
  Low.Name = "wide_ssa";
  Low.Entry = 0x401000;
  Low.DecodedInstructionCount =
      static_cast<uint64_t>(limits::kMaxSSANodes) + 1;
  LowBlock B0;
  B0.Id = 0;
  B0.StartAddr = 0x401000;
  B0.Succs = {1};
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = 0x401000;
  B0.Ops.push_back(Nop);
  LowBlock B1;
  B1.Id = 1;
  B1.StartAddr = 0x401010;
  B1.Preds = {0};
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x401010;
  B1.Ops.push_back(Ret);
  Low.Blocks = {B0, B1};

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  ASSERT_EQ(Med.Blocks.size(), 2u);
  EXPECT_TRUE(Med.Blocks[0].Phis.empty());
  EXPECT_TRUE(Med.Blocks[1].Phis.empty());
  EXPECT_EQ(Med.Entry, 0x401000u);
}

static MedFunc makeWideChainMed() {
  MedFunc Med;
  Med.Name = "wide_cfg";
  Med.Entry = 0x401000;
  for (int I = 0; I < 1025; ++I) {
    MedBlock Block;
    Block.Id = I;
    Block.StartAddr = 0x401000 + static_cast<va_t>(I);
    if (I + 1 < 1025)
      Block.Succs = {I + 1};
    Med.Blocks.push_back(Block);
  }
  return Med;
}

TEST(X86_32_FunctionDiscovery, LargeSkippedSSAMedIREmitsGotoSkeleton) {
  MedFunc Med = makeWideChainMed();
  Med.SkippedSSA = true;
  MedToHighConverter Conv;
  HighFunc HF = Conv.convert(Med, Arch::X86);
  ASSERT_FALSE(HF.Body.empty());
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_TRUE(HasGoto);
}

TEST(X86_32_FunctionDiscovery, LargeSSAMedIRIsLoweredBlockByBlock) {
  // Past the structuring limits an SSA function still gets its statements;
  // a straight chain needs no goto at all.
  MedFunc Med = makeWideChainMed();
  MedToHighConverter Conv;
  HighFunc HF = Conv.convert(Med, Arch::X86);
  ASSERT_FALSE(HF.Body.empty());
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_FALSE(HasGoto);
  EXPECT_EQ(HF.Body.back().Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, FillUnstructuredSkeletonFromTwoBlocks) {
  MedFunc Med;
  Med.Name = "two_block";
  Med.Entry = 0x401000;
  MedBlock First;
  First.Id = 0;
  First.StartAddr = 0x401000;
  First.Succs = {1};
  MedBlock Second;
  Second.Id = 1;
  Second.StartAddr = 0x401010;
  Med.Blocks = {First, Second};

  HighFunc HF;
  HF.Name = Med.Name;
  HF.Entry = Med.Entry;
  fillUnstructuredGotoSkeleton(HF, Med);
  ASSERT_EQ(HF.Body.size(), 4u);
  EXPECT_EQ(HF.Body[0].Kind, StmtKind::Nop);
  EXPECT_EQ(HF.Body[1].Kind, StmtKind::Goto);
  EXPECT_EQ(HF.Body[1].GotoTarget, 0x401010u);
  EXPECT_EQ(HF.Body[3].Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, CfgDoesNotFollowPastNextFunctionStart) {
  std::vector<uint8_t> Bytes(0x20, 0x90);
  Bytes[0x1F] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401010};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LE(First.DecodedInstructionCount, 16u)
      << "jump/fallthrough must not decode past the next function start";
}

TEST(X86_32_FunctionDiscovery, CfgStopsAtNextFunctionEntry) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // Two packed functions: NOPs then a RET that belongs to the second start.
  Bytes[0] = 0x90;
  Bytes[1] = 0x90;
  Bytes[2] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401002};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LT(First.DecodedInstructionCount, 3u)
      << "must not decode the next function's RET as part of the first body";
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldq) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Body[] = {0x0F, 0x62, 0xC1, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck");
  EXPECT_TRUE(LF.hasCompleteInstructionLift())
      << "unsupported=" << LF.UnsupportedInstructionAddresses.size()
      << " decode_fail=" << LF.DecodeFailureAddresses.size();
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldqFromMemory) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  // punpckldq mm0, qword ptr [0x401020]; emms; ret
  const uint8_t Body[] = {0x0F, 0x62, 0x05, 0x20, 0x10,
                          0x40, 0x00, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck_mem");
  EXPECT_TRUE(LF.hasCompleteInstructionLift())
      << "unsupported=" << LF.UnsupportedInstructionAddresses.size()
      << (LF.UnsupportedInstructionAddresses.empty()
              ? ""
              : " at " + std::to_string(LF.UnsupportedInstructionAddresses[0]));
}

TEST(X86_32_FunctionDiscovery,
     DecompileKeepsFirstPassWhenJumpTableBudgetExhausts) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.JumpTableEvidenceBudgetForTesting = 0;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  ASSERT_FALSE(Result.HighFuncs.empty())
      << "decompile must keep first-pass CFGs when table evidence is exhausted";
  EXPECT_FALSE(Result.HighFuncs.front().Name.empty());
}
