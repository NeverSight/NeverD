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
#include "neverd/lift/X86Regs.h"
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

TEST(MedSEHEstablisherFrame, HandlerSharedWithTheNormalPathKeepsTheFrame) {
  // NtLockVirtualMemory: an empty __except body resumes at the code after
  // its __try, which ordinary flow reaches too, and that join reads the
  // frame.  Ordinary flow brings the certified establisher frame, so both
  // entries address one slot.
  auto Img = makeFixedSEHFrameImage();
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().HandlerVA =
      Img.Entry + 0x30;
  Img.ExceptionMetadata.rebuildIndex();
  auto Low = decodeFixedSEHFrame(Img);
  MedFunc Med;
  ASSERT_NO_THROW(
      Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF));
  ASSERT_TRUE(verifyMedFunc(Med, "seh-shared-frame"));
  bool Seen = false;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Addr == Img.Entry + 0x30) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
        Seen = true;
      }
  EXPECT_TRUE(Seen);
}

BinaryImage makeFramePointerSEHFrameImage() {
  BinaryImage Img = makeFixedSEHFrameImage();
  const va_t Entry = Img.Entry;
  Segment &Text = Img.Segments.front();
  Text.Size = 0x39;
  Text.Data.assign(Text.Size, 0xcc);
  // push rbp; sub rsp,64; lea rbp,[rsp+64]; mov dword [rbp-4],1; nop; jmp join.
  const uint8_t Prologue[] = {0x55, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d,
                              0x6c, 0x24, 0x40, 0xc7, 0x45, 0xfc, 1,
                              0,    0,    0,    0x90, 0xeb, 0x1c};
  std::copy(std::begin(Prologue), std::end(Prologue), Text.Data.begin());
  // Handler: eax=[rbp-4]; eax+=20; [rbp-4]=eax; jmp join.
  const uint8_t Handler[] = {0x8b, 0x45, 0xfc, 0x83, 0xc0, 20,
                             0x89, 0x45, 0xfc, 0xeb, 5};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  // Join: eax=[rbp-4]; add rsp,64; pop rbp; ret.
  const uint8_t Join[] = {0x8b, 0x45, 0xfc, 0x48, 0x83, 0xc4, 0x40, 0x5d, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.Sections.front().Size = Text.Size;
  Img.Symbols.front() = Symbol::makeFunc(Entry, Text.Size);

  ExceptionFunction &EH = Img.ExceptionMetadata.Functions.front();
  EH.CodeRange = {Entry, Entry + Text.Size};
  EH.PrologueSize = 10;
  EH.FrameRegister = 5;
  EH.FrameOffset = 0x40;
  EH.UnwindOperations.clear();
  UnwindOperation SetFP;
  SetFP.Kind = UnwindOperationKind::SetFramePointer;
  SetFP.CodeOffset = 10;
  EH.UnwindOperations.push_back(SetFP);
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 5;
  Alloc.StackOffset = 0x40;
  EH.UnwindOperations.push_back(Alloc);
  UnwindOperation Push;
  Push.Kind = UnwindOperationKind::PushNonVolatile;
  Push.CodeOffset = 1;
  Push.Register = 5;
  EH.UnwindOperations.push_back(Push);
  EH.SEH->Scopes.front().GuardedRange = {Entry + 0x11, Entry + 0x12};
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(MedSEHEstablisherFrame, FramePointerHandlerAndJoinUseTheSameLocalSlot) {
  // clang-cl keeps locals behind `lea rbp,[rsp+N]`.  The unwinder resumes the
  // __except body with the fixed-frame SP and restores RBP as a nonvolatile,
  // so every `[rbp-4]` names one slot, 12 bytes below the entry SP.
  auto Img = makeFramePointerSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher-frame-pointer"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 0x0a, Img.Entry + 0x20,
                                   Img.Entry + 0x26, Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -12) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
}

TEST(MedSEHEstablisherFrame, RejectsAFramePointerTheUnwindInfoDoesNotName) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFramePointerSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    if (Case == 0)
      EH.FrameOffset = 0x20;
    if (Case == 1)
      EH.UnwindOperations.front().CodeOffset = 5;
    if (Case == 2) {
      // A second frame-register write before the protected range.
      for (auto &Op : Low.Blocks.front().Ops)
        if (Op.Opcode == NdOp::STORE && Op.Addr == Low.Entry + 0x0a) {
          Op.Opcode = NdOp::COPY;
          Op.Output = NdVar::reg(x86reg::RBP, 8);
          Op.NumInputs = 1;
          Op.Inputs[0] =
              NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
        }
    }
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, AcceptsVersion2EpilogDescriptors) {
  // Version 2 unwind info lists epilog descriptors before the prologue codes.
  // They locate epilogs for the unwinder and do not move the frame.
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto &EH = *Low.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X64UnwindV2;
  EH.UnwindVersion = 2;
  UnwindOperation Epilog;
  Epilog.Kind = UnwindOperationKind::Epilog;
  Epilog.CodeOffset = 13;
  Epilog.StackOffset = 13;
  EH.UnwindOperations.insert(EH.UnwindOperations.begin(), Epilog);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher-v2"));
  bool SawHandlerLoad = false;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Addr == Img.Entry + 0x20) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
        SawHandlerLoad = true;
      }
  EXPECT_TRUE(SawHandlerLoad);
}

TEST(MedSEHEstablisherFrame, RejectsEpilogCodesOutsideVersion2) {
  for (unsigned Case = 0; Case != 2; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFixedSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    UnwindOperation Extra;
    Extra.CodeOffset = 13;
    if (Case == 0) {
      // Version 1 has no epilog descriptors.
      Extra.Kind = UnwindOperationKind::Epilog;
    } else {
      // A version 2 spare code has no certified SP effect.
      EH.Encoding = ExceptionEncoding::X64UnwindV2;
      EH.UnwindVersion = 2;
      Extra.Kind = UnwindOperationKind::Spare;
    }
    EH.UnwindOperations.insert(EH.UnwindOperations.begin(), Extra);
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
  }
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

// The normal path writes ECX before the guarded nop; the handler stores EAX
// and ECX without writing either.
BinaryImage makeSEHHandlerRegisterImage() {
  BinaryImage Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  // mov ecx,5; nop; nop; nop in place of the slot initializer.
  const uint8_t Normal[] = {0xb9, 5, 0, 0, 0, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  // Handler: [rsp+36]=eax; [rsp+40]=ecx; nop; nop; nop; jmp join.
  const uint8_t Handler[] = {0x89, 0x44, 0x24, 0x24, 0x89, 0x4c, 0x24,
                             0x28, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  return Img;
}

// Follow copies and register-view extensions to the value a use reads.
MedVar sourceValue(const MedFunc &F, MedVar V) {
  for (unsigned Step = 0; Step != 16; ++Step) {
    const MedOp *Def = nullptr;
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer)
          Def = &Op;
    if (!Def || Def->NumInputs == 0 ||
        (Def->Opcode != NdOp::COPY && Def->Opcode != NdOp::INT_ZEXT))
      return V;
    V = Def->Inputs[0];
  }
  return V;
}

std::optional<MedVar> storedValueAt(const MedFunc &F, va_t Addr) {
  for (const MedBlock &B : F.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::STORE && Op.Addr == Addr && Op.NumInputs > 1)
        return sourceValue(F, Op.Inputs[1]);
  return std::nullopt;
}

// push rbp; sub rsp,64; lea rbp,[rsp+64]; sub rsp,rcx; jmp next;
// next: mov dword [rbp-4],1; mov [rsp+32],ecx (protected); nop; jmp join.
// The handler stores [rbp-4] to [rsp+32] too; the join leaves through
// lea rsp,[rbp].
BinaryImage makeDynamicFramePointerSEHImage() {
  BinaryImage Img = makeFramePointerSEHFrameImage();
  Segment &Text = Img.Segments.front();
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Body[] = {0x55, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d, 0x6c,
                          0x24, 0x40, 0x48, 0x29, 0xcc, 0xeb, 0,    0xc7,
                          0x45, 0xfc, 1,    0,    0,    0,    0x89, 0x4c,
                          0x24, 0x20, 0x90, 0xeb, 0x13};
  std::copy(std::begin(Body), std::end(Body), Text.Data.begin());
  const uint8_t Handler[] = {0x8b, 0x45, 0xfc, 0x89, 0x44, 0x24, 0x20, 0xeb, 7};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  const uint8_t Join[] = {0x8b, 0x45, 0xfc, 0x48, 0x8d, 0x65, 0, 0x5d, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange = {
      Img.Entry + 0x16, Img.Entry + 0x1a};
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

// The value the `[rsp+disp]` store at Addr adds its displacement to.
std::optional<MedVar> storeAddressBase(const MedFunc &F, va_t Addr) {
  for (const MedBlock &B : F.Blocks)
    for (const MedOp &Op : B.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.Addr != Addr || Op.NumInputs < 2)
        continue;
      const MedVar Address = sourceValue(F, Op.Inputs[0]);
      for (const MedBlock &DefBlock : F.Blocks)
        for (const MedOp &Def : DefBlock.Ops)
          if (Def.Opcode == NdOp::INT_ADD && Def.NumInputs == 2 &&
              Def.Output.Kind == Address.Kind && Def.Output.Id == Address.Id &&
              Def.Output.SSAVer == Address.SSAVer && Def.Inputs[1].isConst())
            return sourceValue(F, Def.Inputs[0]);
      return std::nullopt;
    }
  return std::nullopt;
}

TEST(MedSEHEstablisherFrame, DynamicFramePointerHandlerResumesWithTheRangeSP) {
  // An alloca after a frame-pointer prologue moves SP before the __try.  The
  // unwinder resumes the __except body with the faulting point's SP, which
  // the protected range never moves, and restores RBP as a nonvolatile.
  auto Img = makeDynamicFramePointerSEHImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-dynamic-frame-pointer"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 0x0f, Img.Entry + 0x20,
                                   Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -12) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
  auto RangeBase = storeAddressBase(Med, Img.Entry + 0x16);
  auto HandlerBase = storeAddressBase(Med, Img.Entry + 0x23);
  ASSERT_TRUE(RangeBase);
  ASSERT_TRUE(HandlerBase);
  EXPECT_EQ(RangeBase->Kind, MedVar::Reg) << RangeBase->display();
  EXPECT_EQ(HandlerBase->Id, RangeBase->Id) << HandlerBase->display();
  EXPECT_EQ(HandlerBase->SSAVer, RangeBase->SSAVer) << HandlerBase->display();
  // The alloca leaves that SP at no fixed offset from the entry SP.
  EXPECT_FALSE(entrySPOffset(Med, *RangeBase));
}

TEST(MedSEHEstablisherFrame, RejectsADynamicSPWriteInAProtectedBlock) {
  // An alloca in a block the scope covers can leave the faulting point more
  // than one SP.
  auto Img = makeDynamicFramePointerSEHImage();
  auto Low = decodeFixedSEHFrame(Img);
  Low.ExceptionMetadata->SEH->Scopes.front().GuardedRange = {Img.Entry + 0x0a,
                                                             Img.Entry + 0x1a};
  EXPECT_THROW(LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
               LowToMedConversionError);
}

TEST(MedSEHHandlerEntry, EAXHoldsTheExceptionCode) {
  auto Img = makeSEHHandlerRegisterImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  // __C_specific_handler resumes the handler with the code in EAX.
  auto Code = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Code);
  EXPECT_EQ(Code->Kind, MedVar::SEHExceptionCode) << Code->display();
  EXPECT_EQ(Code->SSAVer, 1);
  EXPECT_EQ(Code->ConstVal, Img.Entry + 0x20);
}

// A second scope guards the nop before the first one and resumes at 0x13,
// before the first handler.
BinaryImage makeTwoSEHHandlerImage() {
  BinaryImage Img = makeSEHHandlerRegisterImage();
  auto &Data = Img.Segments.front().Data;
  // Second handler: [rsp+40]=eax; jmp join.
  const uint8_t Handler[] = {0x89, 0x44, 0x24, 0x28, 0xeb, 0x17};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x13);
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Img.Entry + 0xf, Img.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Img.Entry + 0x13;
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(MedSEHHandlerEntry, EachHandlerEntryHasItsOwnExceptionCode) {
  auto Img = makeTwoSEHHandlerImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-two-handlers"));
  auto Early = storedValueAt(Med, Img.Entry + 0x13);
  auto Late = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Early && Late);
  ASSERT_EQ(Early->Kind, MedVar::SEHExceptionCode) << Early->display();
  ASSERT_EQ(Late->Kind, MedVar::SEHExceptionCode) << Late->display();
  // Numbered in address order and keyed by the entry each handler starts at.
  EXPECT_EQ(Early->SSAVer, 1);
  EXPECT_EQ(Late->SSAVer, 2);
  EXPECT_EQ(Early->ConstVal, Img.Entry + 0x13);
  EXPECT_EQ(Late->ConstVal, Img.Entry + 0x20);
}

TEST(MedSEHHandlerEntry, ScratchRegisterIsNotTheNormalPathValue) {
  auto Img = makeSEHHandlerRegisterImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  // The unwinder leaves ECX unspecified: neither the normal path's
  // `mov ecx,5` nor the value the function was entered with.
  auto Scratch = storedValueAt(Med, Img.Entry + 0x24);
  ASSERT_TRUE(Scratch);
  EXPECT_EQ(Scratch->Kind, MedVar::Unspecified) << Scratch->display();
}

TEST(MedSEHHandlerEntry, NonvolatileRegisterHoldsItsProtectedValue) {
  // mov rdi,rcx before the protected nop; the handler stores rdi. The
  // unwinder restores the protected frame's rdi, which the range never
  // writes: the handler sees rcx's value, not rdi's value at entry.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0xcf, 0x90, 0x90, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x7c, 0x24, 0x28, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  auto Saved = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Saved);
  EXPECT_EQ(Saved->Kind, MedVar::Reg) << Saved->display();
  EXPECT_EQ(Saved->RegOff, x86reg::RCX) << Saved->display();
  EXPECT_EQ(Saved->SSAVer, 0) << Saved->display();
}

namespace {

// xor eax,eax; mov [rcx],dl (protected); jmp H; H: ret -- the handler is the
// normal path's return block too, as for `status = 0; __try {..}
// __except (1) { status = GetExceptionCode(); } return status;`.
BinaryImage makeSharedHandlerImage() {
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
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Code[] = {0x31, 0xc0, // xor eax, eax
                          0x88, 0x11, // mov [rcx], dl
                          0xeb, 0x00, // jmp +0
                          0xc3};      // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x10;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Code)));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + sizeof(Code)};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry + 2, Entry + 6};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 6;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(EH);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

} // namespace

TEST(MedSEHHandlerEntry, ArgumentLiveIntoTheHandlerStaysAParameter) {
  // mov [rsp+40],rdx before the protected nop; the handler stores rdx too.
  // rdx is then live into both roots, so its entry copy takes a new SSA
  // version, yet it is still the incoming second argument.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0x54, 0x24, 0x28, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x54, 0x24, 0x30, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-argument"));
  auto Param =
      std::find_if(Med.Params.begin(), Med.Params.end(),
                   [](const MedVar &P) { return P.RegOff == x86reg::RDX; });
  ASSERT_NE(Param, Med.Params.end());
  EXPECT_GE(Param->Id, 0) << "rdx is a placeholder, not the read argument";
}

TEST(MedSEHHandlerEntry, NonvolatileRegisterWrittenInTheRangeIsUnspecified) {
  // mov rdi,rcx inside the protected range; the handler stores rdi. Where
  // the exception struck decides rdi, so the handler cannot know it.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0xcf, 0x90, 0x90, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x7c, 0x24, 0x28, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange = {
      Img.Entry + 8, Img.Entry + 0x11};
  Img.ExceptionMetadata.rebuildIndex();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  auto Saved = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Saved);
  EXPECT_EQ(Saved->Kind, MedVar::Unspecified) << Saved->display();
}

TEST(MedSEHHandlerEntry, HandlerSharedWithTheNormalPathMergesBothValues) {
  // The normal path returns 0 and the dispatcher's entry the exception code;
  // the handler block is both, so RAX there must merge the two.
  auto Img = makeSharedHandlerImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-shared-handler"));
  bool Merged = false;
  for (const MedBlock &Block : Med.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      Merged |= Block.StartAddr == Img.Entry + 6 && Phi.ExceptionalEntry &&
                Phi.ExceptionalEntry->Kind == MedVar::SEHExceptionCode;
  EXPECT_TRUE(Merged);
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  // The normal path keeps its zero; the arm assigns the code.
  EXPECT_NE(Source.substr(0, Handler).find(" = 0;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.substr(Handler).find("exception_code;"), std::string::npos)
      << Source;
  // The handler block follows the __try: the arm simply ends there.
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
}

TEST(MedSEHHandlerEntry, HighCCapturesTheExceptionCodeInTheExceptArm) {
  auto Img = makeSEHHandlerRegisterImage();
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
  const std::string Arm = Source.substr(Handler);
  EXPECT_NE(Arm.find("exception_code = GetExceptionCode();"), std::string::npos)
      << Source;
  EXPECT_NE(Arm.find("var_m64 = exception_code;"), std::string::npos) << Source;
  EXPECT_NE(Source.substr(0, Handler).find(" exception_code;"),
            std::string::npos)
      << Source;
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
      // Ordinary flow entering the handler from the epilogue, after the
      // frame is gone, does not bring the establisher frame.
      for (auto &B : Low.Blocks)
        if (B.StartAddr == Low.Entry + 0x20)
          Low.Blocks.back().Succs.push_back(B.Id);
    }
    if (Case == 10)
      Low.DecodedInstructionCount = uint64_t(limits::kMaxSSAFunctionOps) + 1;
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

namespace {

// sub rsp,0x38; mov rax,[rsp+0x60]; mov [rsp+0x20],rax; call callee;
// add rsp,0x38; ret -- RCX, RDX, R8 and R9 are passed through untouched.
BinaryImage makeWin64ForwarderImage() {
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Callee = 0x140001020;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x30;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const int32_t Rel = static_cast<int32_t>(Callee - (Entry + 0x13 + 5));
  const uint8_t Code[] = {0x48,
                          0x83,
                          0xec,
                          0x38, // sub
                          0x48,
                          0x8b,
                          0x44,
                          0x24,
                          0x60, // mov rax
                          0x48,
                          0x89,
                          0x44,
                          0x24,
                          0x20, // mov [rsp]
                          0x90,
                          0x90,
                          0x90,
                          0x90,
                          0x90, // pad
                          0xe8,
                          static_cast<uint8_t>(Rel), // call
                          static_cast<uint8_t>(Rel >> 8),
                          static_cast<uint8_t>(Rel >> 16),
                          static_cast<uint8_t>(Rel >> 24),
                          0x48,
                          0x83,
                          0xc4,
                          0x38,  // add
                          0xc3}; // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  // The callee reads its fifth argument, so the forwarder's outgoing slot is
  // a real stack argument: mov rax, [rsp+28h]; ret.
  const uint8_t CalleeCode[] = {0x48, 0x8b, 0x44, 0x24, 0x28, 0xc3};
  std::copy(std::begin(CalleeCode), std::end(CalleeCode),
            Text.Data.begin() + (Callee - Entry));
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x30;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x20));
  Img.Symbols.push_back(Symbol::makeFunc(Callee, sizeof(CalleeCode)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, UntouchedRegisterArgumentsPassTheParameters) {
  auto Img = makeWin64ForwarderImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const HighFunc *Forwarder = nullptr;
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Entry == Img.Entry)
      Forwarder = &Func;
  ASSERT_NE(Forwarder, nullptr);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({*Forwarder}, Stream, Options));
  Stream.flush();
  // The outgoing stack argument means all four register slots are arguments;
  // the function never writes them, so they carry its own parameters.
  EXPECT_NE(Source.find("(arg0, arg1, arg2, arg3, arg4)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(0, 0, 0, 0,"), std::string::npos) << Source;
}

namespace {

// xor r11d,r11d; lea r8,[rsp+8]; test rdx,rdx; mov [rsp+8],r11w;
// cmovne r8,rdx; mov word [r8],0x14; movzx eax,word [r8]; ret
BinaryImage makeMaskedFrameSelectImage() {
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
  Text.Size = 0x30;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Code[] = {0x45, 0x31, 0xdb,                   // xor
                          0x4c, 0x8d, 0x44, 0x24, 0x08,       // lea
                          0x48, 0x85, 0xd2,                   // test
                          0x66, 0x44, 0x89, 0x5c, 0x24, 0x08, // mov
                          0x4c, 0x0f, 0x45, 0xc2,             // cmovne
                          0x66, 0x41, 0xc7, 0x00, 0x14, 0x00, // mov
                          0x41, 0x0f, 0xb7, 0x00,             // movzx
                          0xc3};                              // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x30;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Code)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, MaskedSelectOfAFrameAddressUsesItsIntegerValue) {
  // `cmovne` over a frame address lifts to `(p & m) | (&slot & ~m)`; C has
  // no bitwise operator on a pointer, so the address is an integer there.
  auto Img = makeMaskedFrameSelectImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Result.HighFuncs.front()}, Stream, Options));
  Stream.flush();
  for (size_t At = Source.find("&var_"); At != std::string::npos;
       At = Source.find("&var_", At + 1)) {
    const size_t End = Source.find_first_of(" ),;", At);
    if (End == std::string::npos || Source.compare(End, 3, " & ") != 0)
      continue;
    EXPECT_EQ(Source.compare(At - 11, 11, "(uintptr_t)"), 0) << Source;
  }
  EXPECT_NE(Source.find("(uintptr_t)&var_"), std::string::npos) << Source;
}

namespace {

// caller: sub rsp,28h; call clobber; mov edx,1; call reads_dl; add rsp,28h;
// ret. clobber: xor ecx,ecx; ret. reads_dl: movzx eax,dl; ret.
BinaryImage makeUnreadSlotImage() {
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
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Caller[] = {0x48, 0x83, 0xec, 0x28,       // sub rsp, 28h
                            0xe8, 0x17, 0x00, 0x00, 0x00, // call clobber
                            0xba, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
                            0xe8, 0x1d, 0x00, 0x00, 0x00, // call reads_dl
                            0x48, 0x83, 0xc4, 0x28,       // add rsp, 28h
                            0xc3};                        // ret
  const uint8_t Clobber[] = {0x31, 0xc9, 0xc3};
  const uint8_t ReadsDl[] = {0x0f, 0xb6, 0xc2, 0xc3};
  std::copy(std::begin(Caller), std::end(Caller), Text.Data.begin());
  std::copy(std::begin(Clobber), std::end(Clobber), Text.Data.begin() + 0x20);
  std::copy(std::begin(ReadsDl), std::end(ReadsDl), Text.Data.begin() + 0x30);
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x40;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Caller)));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x20, sizeof(Clobber)));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x30, sizeof(ReadsDl)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, ArgumentSlotTheCalleeNeverReadsIsZero) {
  // The callee reads only DL, so RCX is a positional slot it ignores. The
  // caller left nothing defined there; the call passes zero, not an
  // unknown value.
  auto Img = makeUnreadSlotImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const HighFunc *Caller = nullptr;
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Entry == Img.Entry)
      Caller = &Func;
  ASSERT_NE(Caller, nullptr);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({*Caller}, Stream, Options));
  Stream.flush();
  EXPECT_NE(Source.find("sub_140001030(0, 1)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}
