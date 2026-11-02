//===- MedCachedCallTargetTests.cpp - Captured call proofs ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Exact cached-target and Win64 call-interface regression cases.
///
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedCallConvention.h"
#include "neverd/ir/med/MedCallTarget.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/Support/raw_ostream.h"

namespace {
using namespace neverd;

#define CACHED_CALL_STRING(Name, Value) constexpr const char *Name = Value;
#define CACHED_CALL_INTEGER(Name, Value) constexpr uint64_t Name = Value;
#include "MedCachedCallTargetFixtures.def"

MedVar value(int Id, uint16_t Bytes = 8) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Bytes;
  V.TheArch = Arch::X64;
  return V;
}

MedVar reg(int Id, uint64_t Offset, uint16_t Bytes = 8) {
  auto V = value(Id, Bytes);
  V.Kind = MedVar::Reg;
  V.RegOff = Offset;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp O;
  O.Opcode = Opcode;
  O.Output = Output;
  for (const auto &V : Inputs)
    O.addInput(V);
  return O;
}

template <typename Check>
unsigned checkCallAt(const HighFunc &F, va_t Site, const Check &Verify) {
  unsigned Matches = 0;
  walkStmts(F.Body, [&](const HighStmt &S) {
    forEachRhsExpr(S, [&](const ExprPtr &Root) {
      const auto Find = [&](auto &&Self, const ExprPtr &E) -> void {
        if (!E)
          return;
        if (E->Kind == ExprKind::Call && E->CallSiteAddr == Site) {
          ++Matches;
          Verify(*E);
        }
        E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
      };
      Find(Find, Root);
    });
  });
  return Matches;
}

MedBlock cached(bool Import = true) {
  const auto SP = reg(1, x86reg::RSP);
  MedBlock B;
  B.Ops = {op(NdOp::INT_SUB, value(2), {SP, MedVar::makeConst(64, 8)}),
           op(NdOp::LOAD, value(3), {MedVar::makeConst(0x3000, 8)}),
           op(NdOp::STORE, {},
              {value(2), Import ? value(3) : MedVar::makeConst(0x1000, 8)}),
           op(NdOp::INT_ADD, SP, {value(2), MedVar::makeConst(16, 8)}),
           op(NdOp::INT_SUB, value(4), {SP, MedVar::makeConst(16, 8)}),
           op(NdOp::LOAD, value(5), {value(4)})};
  return B;
}

TEST(MedCachedCallTarget, ProvesMovingStackWithoutChangingRuntimeOperands) {
  for (bool Import : {false, true}) {
    auto B = cached(Import);
    const auto Original = B.Ops.back().Inputs[0];
    const auto Target = resolvePreSSAMedCallTarget(B, value(5), Arch::X64);
    ASSERT_TRUE(Target);
    EXPECT_EQ(Target->ThroughSlot, Import);
    EXPECT_EQ(Target->Address, Import ? 0x3000u : 0x1000u);
    if (Import)
      EXPECT_EQ(Target->LoadIndex, 1u);
    EXPECT_EQ(B.Ops.back().Opcode, NdOp::LOAD);
    EXPECT_EQ(B.Ops.back().Inputs[0], Original);
  }
}

TEST(MedCachedCallTarget, RejectsInterveningCallsAndMayAliasWrites) {
  for (bool Call : {false, true}) {
    auto B = cached();
    B.Ops.insert(
        B.Ops.begin() + 3,
        Call ? op(NdOp::CALL, reg(6, x86reg::RAX),
                  {MedVar::makeConst(0x2000, 8)})
             : op(NdOp::STORE, {}, {value(99), MedVar::makeConst(0, 8)}));
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
  }
}

TEST(MedCachedCallTarget, RejectsPartialAndOrderedOverwrites) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    auto B = cached();
    if (Variant == 0)
      B.Ops.insert(B.Ops.begin() + 3,
                   op(NdOp::STORE, {}, {value(2), MedVar::makeConst(0, 4)}));
    else if (Variant == 1)
      B.Ops.back().MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
    else
      B.Ops[2].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
  }
}

TEST(MedCachedCallTarget, DoesNotReuseAnOverwrittenRegisterAddress) {
  auto B = cached();
  B.Ops.insert(B.Ops.begin() + 4, op(NdOp::COPY, reg(1, x86reg::RSP, 4),
                                     {MedVar::makeConst(0x9000, 4)}));
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64));
}

TEST(MedCachedCallTarget, AuxiliaryOutputsInvalidatePreviousTemporaryValues) {
  MedBlock B;
  B.Ops = {op(NdOp::COPY, value(2), {MedVar::makeConst(0x1000, 8)})};
  auto Effect = op(NdOp::INTRINSIC, {}, {});
  Effect.IntrinsicOutputs = {value(2)};
  B.Ops.push_back(Effect);
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(2), Arch::X64));
}

TEST(MedCachedCallTarget, ExhaustionPublishesNoPartialTarget) {
  auto B = cached();
  for (size_t Budget : {size_t(0), size_t(1)})
    EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64, &Budget));
  size_t Budget = 10000;
  ASSERT_TRUE(resolvePreSSAMedCallTarget(B, value(5), Arch::X64, &Budget));
  EXPECT_LT(Budget, 10000u);
}

TEST(MedCachedCallTarget, IncrementalQueriesMatchWholePrefixProofs) {
  // A second captured callable follows the first call. The call retires the
  // registers and ends the earlier frame cell's chain; a new store starts one.
  const auto SP = reg(1, x86reg::RSP);
  auto Full = cached();
  Full.Ops.push_back(op(NdOp::INDIR_CALL, reg(6, x86reg::RAX), {value(5)}));
  Full.Ops.push_back(
      op(NdOp::INT_SUB, value(7), {SP, MedVar::makeConst(32, 8)}));
  Full.Ops.push_back(
      op(NdOp::STORE, {}, {value(7), MedVar::makeConst(0x1000, 8)}));
  Full.Ops.push_back(op(NdOp::LOAD, value(8), {value(7)}));
  Full.Ops.push_back(op(NdOp::LOAD, value(9), {value(4)}));
  PreSSAMedCallTargets Incremental(Arch::X64);
  size_t Work = 100000;
  MedBlock Growing;
  for (const auto &Op : Full.Ops) {
    Growing.Ops.push_back(Op);
    for (int Id : {5, 8, 9}) {
      const auto Whole =
          resolvePreSSAMedCallTarget(Growing, value(Id), Arch::X64);
      const auto Shared = Incremental.resolve(Growing, value(Id), &Work);
      ASSERT_EQ(Whole.has_value(), Shared.has_value())
          << Growing.Ops.size() << " " << Id;
      if (Whole) {
        EXPECT_EQ(Whole->Address, Shared->Address);
        EXPECT_EQ(Whole->ThroughSlot, Shared->ThroughSlot);
      }
    }
  }
  EXPECT_FALSE(resolvePreSSAMedCallTarget(Growing, value(9), Arch::X64));
  const auto Second = resolvePreSSAMedCallTarget(Growing, value(8), Arch::X64);
  ASSERT_TRUE(Second);
  EXPECT_EQ(Second->Address, 0x1000u);
  EXPECT_FALSE(Second->ThroughSlot);
  // The incremental proof versions each operation once, where whole-prefix
  // queries version the prefix again for every call.
  size_t WholeWork = 100000;
  MedBlock Prefix;
  for (const auto &Op : Full.Ops) {
    Prefix.Ops.push_back(Op);
    for (int Id : {5, 8, 9})
      resolvePreSSAMedCallTarget(Prefix, value(Id), Arch::X64, &WholeWork);
  }
  EXPECT_GT(Work, WholeWork);
}

TEST(MedCachedCallTarget, Win64ConventionOwnsWdkOperandsAndSingleResults) {
  const auto *Win64 = callArgumentConvention(Arch::X64, BinaryFormat::COFF);
  const auto *SysV = callArgumentConvention(Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(Win64 && SysV && Win64->PrototypeIntegerArguments);
  EXPECT_TRUE(Win64->SingleRegisterReturns);
  EXPECT_FALSE(SysV->SingleRegisterReturns);
  EXPECT_FALSE(SysV->PrototypeIntegerArguments);
  const auto Arguments = Win64->PrototypeIntegerArguments("IoCreateDevice");
  ASSERT_TRUE(Arguments);
  ASSERT_EQ(Arguments->Parameters.size(), 7u);
  for (size_t I = 0; I < 7; ++I)
    EXPECT_EQ(Arguments->Parameters[I].Kind,
              I < 4 ? SourceABICarrierKind::IntegerRegister
                    : SourceABICarrierKind::Stack)
        << I;
  EXPECT_EQ(Arguments->Parameters[4].Bytes, 4u);
  EXPECT_EQ(Arguments->Parameters[5].Bytes, 1u);
  EXPECT_EQ(Arguments->Parameters[6].Bytes, 8u);
  EXPECT_FALSE(Win64->PrototypeIntegerArguments("NotAKernelRoutine"));
}

TEST(MedCachedCallTarget, RetainsTheOriginalMutableImportObservation) {
  auto B = cached();
  // Updating the IAT does not change the already captured callable. This
  // observation must neither turn the call into a new load nor mutate IR.
  B.Ops.insert(B.Ops.begin() + 2, op(NdOp::STORE, {},
                                     {MedVar::makeConst(0x3000, 8),
                                      MedVar::makeConst(0x4000, 8)}));
  const auto Target = resolvePreSSAMedCallTarget(B, value(5), Arch::X64);
  ASSERT_TRUE(Target);
  EXPECT_EQ(Target->Address, 0x3000u);
  EXPECT_TRUE(Target->ThroughSlot);
  EXPECT_EQ(B.Ops.size(), 7u);
}

TEST(MedCachedCallTarget, WindowsVolatileTransportIsNotAnAggregateResult) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
    LowFunc F;
    F.Name = "transport_after_call";
    F.Entry = 0x1000;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    const auto Add = [&](NdOp Opcode, NdVar Out,
                         std::initializer_list<NdVar> Inputs) {
      LowOp O;
      O.Opcode = Opcode;
      O.Addr = F.Entry + B.Ops.size();
      O.Output = Out;
      for (const auto &V : Inputs)
        O.Inputs[O.NumInputs++] = V;
      B.Ops.push_back(O);
    };
    Add(NdOp::CALL, NdVar::reg(x86reg::RAX, 8), {NdVar::cst(0x2000, 8)});
    Add(NdOp::STORE, {}, {NdVar::cst(0x4000, 8), NdVar::reg(x86reg::RAX, 8)});
    Add(NdOp::STORE, {}, {NdVar::cst(0x4008, 8), NdVar::reg(x86reg::RDX, 8)});
    Add(NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)});
    F.Blocks.push_back(B);
    LowToMedConverter Converter;
    const auto M = Converter.convert(F, Arch::X64, Format);
    bool Found = false;
    for (const auto &Block : M.Blocks)
      for (const auto &O : Block.Ops)
        if (O.Opcode == NdOp::CALL) {
          Found = true;
          EXPECT_EQ(O.Output.Size, Format == BinaryFormat::COFF ? 8 : 16);
        }
    EXPECT_TRUE(Found);
  }
}

TEST(MedCachedCallTarget, HighIRCallsTheCapturedValueAfterTheSlotChanges) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    MedFunc F;
    F.Entry = 0x1000;
    F.Name = "captured_call";
    F.FrameSize = 64;
    MedBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    const auto SP = reg(1, x86reg::RSP);
    B.Ops = {op(NdOp::INT_SUB, value(2), {SP, MedVar::makeConst(16, 8)}),
             op(NdOp::LOAD, value(3), {MedVar::makeConst(0x3000, 8)}),
             op(NdOp::STORE, {},
                {MedVar::makeConst(0x3000, 8), MedVar::makeConst(0x4000, 8)}),
             op(NdOp::STORE, {}, {value(2), value(3)}),
             op(NdOp::LOAD, value(5), {value(2)}),
             op(NdOp::INDIR_CALL, value(6), {Spilled ? value(5) : value(3)}),
             op(NdOp::RETURN, {}, {value(6)})};
    for (size_t I = 0; I < B.Ops.size(); ++I)
      B.Ops[I].Addr = F.Entry + I;
    F.Blocks = {B};
    const std::map<va_t, std::string> Names{{0x3000, "observed_import"}};
    MedToHighConverter Converter;
    Converter.setFuncNames(&Names);
    auto High = Converter.convert(F, Arch::X64);
    bool Found = false;
    walkStmts(High.Body, [&](const HighStmt &S) {
      forEachRhsExpr(S, [&](const ExprPtr &Root) {
        const auto Find = [&](auto &&Self, const ExprPtr &E) -> void {
          if (!E)
            return;
          if (E->Kind == ExprKind::Call) {
            Found = true;
            EXPECT_TRUE(E->IsIndirectCall);
            EXPECT_TRUE(E->IndirectTarget);
            EXPECT_EQ(E->CallAddr, 0x3000u);
          }
          E->forEachChildExpr([&](const ExprPtr &Child) { Self(Self, Child); });
        };
        Find(Find, Root);
      });
    });
    EXPECT_TRUE(Found);
  }
}

TEST(MedCachedCallTarget,
     LiveImportLoadsThroughConstantAddressCopiesKeepNames) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (unsigned Boundary = 0; Boundary < 3; ++Boundary) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Boundary);
      MedFunc F;
      F.Entry = 0x1000;
      F.Name = "live_import";
      MedBlock B;
      B.Id = 0;
      B.StartAddr = F.Entry;
      B.Ops = {op(NdOp::COPY, value(2), {MedVar::makeConst(0x3000, 8)}),
               op(NdOp::LOAD, value(3), {value(2)})};
      if (Boundary == 1)
        B.Ops.push_back(
            op(NdOp::STORE, {},
               {MedVar::makeConst(0x3000, 8), MedVar::makeConst(0x4000, 8)}));
      else if (Boundary == 2)
        B.Ops.push_back(
            op(NdOp::CALL, value(4), {MedVar::makeConst(0x2000, 8)}));
      B.Ops.push_back(op(NdOp::INDIR_CALL, value(6), {value(3)}));
      B.Ops.push_back(op(NdOp::RETURN, {}, {value(6)}));
      for (size_t I = 0; I < B.Ops.size(); ++I) {
        B.Ops[I].Addr = F.Entry + I;
        B.Ops[I].Output.TheArch = Architecture;
        for (unsigned K = 0; K < B.Ops[I].NumInputs; ++K)
          B.Ops[I].Inputs[K].TheArch = Architecture;
      }
      F.Blocks = {B};
      const std::map<va_t, std::string> Names{{0x3000, "observed_import"}};
      MedToHighConverter Converter;
      Converter.setFuncNames(&Names);
      const auto High = Converter.convert(F, Architecture);
      bool Found = false;
      walkStmts(High.Body, [&](const HighStmt &S) {
        forEachRhsExpr(S, [&](const ExprPtr &Root) {
          const auto Find = [&](auto &&Self, const ExprPtr &E) -> void {
            if (!E)
              return;
            if (E->Kind == ExprKind::Call && E->CallAddr == 0x3000) {
              Found = true;
              EXPECT_EQ(E->IsIndirectCall, Boundary != 0);
              EXPECT_EQ(bool(E->IndirectTarget), Boundary != 0);
              if (!Boundary)
                EXPECT_EQ(E->CallTarget, "observed_import");
            }
            E->forEachChildExpr(
                [&](const ExprPtr &Child) { Self(Self, Child); });
          };
          Find(Find, Root);
        });
      });
      EXPECT_TRUE(Found);
    }
}

TEST(MedCachedCallTarget, HighIRNamesOnlyImportsWhoseReadStillReachesTheCall) {
  enum class Transport { Direct, Copy, Spill };
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (Transport Via : {Transport::Direct, Transport::Copy, Transport::Spill})
      for (bool SlotChanges : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Via));
        SCOPED_TRACE(SlotChanges);
        const auto &Registers = getTargetRegInfo(Architecture);
        const auto PointerBytes = Registers.PointerSize;
        int Next = 0;
        const auto Temp = [&] {
          auto V = value(Next++, PointerBytes);
          V.TheArch = Architecture;
          V.SSAVer = 1;
          return V;
        };
        auto SP = Temp();
        SP.Kind = MedVar::Reg;
        SP.RegOff = Registers.StackPointer;
        SP.SSAVer = 0;
        const auto Address = Temp();
        const auto Observed = Temp();
        const auto Result = Temp();
        MedBlock B;
        B.Id = 0;
        B.StartAddr = kObservationEntry;
        B.Ops = {op(NdOp::INT_ADD, Address,
                    {MedVar::makeConst(kObservationSlotBase, PointerBytes),
                     MedVar::makeConst(kObservationSlotOffset, PointerBytes)}),
                 op(NdOp::LOAD, Observed, {Address})};
        const auto ObservationIndex = B.Ops.size() - 1;
        if (SlotChanges)
          B.Ops.push_back(op(NdOp::STORE, {},
                             {Address, MedVar::makeConst(kReplacementCallable,
                                                         PointerBytes)}));
        MedVar Target = Observed;
        if (Via == Transport::Copy) {
          Target = Temp();
          B.Ops.push_back(op(NdOp::COPY, Target, {Observed}));
        } else if (Via == Transport::Spill) {
          const auto Frame = Temp();
          Target = Temp();
          B.Ops.push_back(op(
              NdOp::INT_SUB, Frame,
              {SP, MedVar::makeConst(kObservationSpillOffset, PointerBytes)}));
          B.Ops.push_back(op(NdOp::STORE, {}, {Frame, Observed}));
          B.Ops.push_back(op(NdOp::LOAD, Target, {Frame}));
        }
        const auto CallIndex = B.Ops.size();
        const auto CallSite = kObservationEntry + CallIndex;
        const auto Observation = resolveMedCallTarget(B, CallIndex, Target);
        ASSERT_TRUE(Observation);
        EXPECT_TRUE(Observation->ThroughSlot);
        EXPECT_EQ(Observation->LoadIndex, ObservationIndex);
        const auto PreSSA = resolvePreSSAMedCallTarget(B, Target, Architecture);
        ASSERT_TRUE(PreSSA);
        EXPECT_EQ(PreSSA->LoadIndex, ObservationIndex);
        B.Ops.push_back(op(NdOp::INDIR_CALL, Result, {Target}));
        B.Ops.push_back(op(NdOp::RETURN, {}, {Result}));
        for (size_t I = 0; I < B.Ops.size(); ++I)
          B.Ops[I].Addr = kObservationEntry + I;
        MedFunc F;
        F.Entry = kObservationEntry;
        F.Name = kObservationFunction;
        F.FrameSize = kObservationFrameBytes;
        F.Blocks = {B};
        const auto Slot = kObservationSlotBase + kObservationSlotOffset;
        const std::map<va_t, std::string> Names{{Slot, kObservedImport}};
        MedToHighConverter Converter;
        Converter.setFuncNames(&Names);
        const auto High = Converter.convert(F, Architecture);
        const unsigned Calls =
            checkCallAt(High, CallSite, [&](const HighExpr &E) {
              EXPECT_EQ(E.CallAddr, Slot);
              EXPECT_EQ(E.IsIndirectCall, SlotChanges);
              if (SlotChanges)
                EXPECT_TRUE(E.IndirectTarget);
              else
                EXPECT_EQ(E.CallTarget, kObservedImport);
            });
        EXPECT_EQ(Calls, 1u);
      }
}

TEST(MedCachedCallTarget,
     CrossBlockObservationsKeepTheSignatureAndCapturedValue) {
  enum class Barrier { None, Store, Call, OrderedRead, Join };
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (Barrier Between : {Barrier::None, Barrier::Store, Barrier::Call,
                            Barrier::OrderedRead, Barrier::Join}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Between));
      const auto PointerBytes = getTargetRegInfo(Architecture).PointerSize;
      auto Observed = value(0, PointerBytes);
      Observed.TheArch = Architecture;
      Observed.SSAVer = 1;
      auto Result = value(1, PointerBytes);
      Result.TheArch = Architecture;
      Result.SSAVer = 1;
      auto Auxiliary = value(2, PointerBytes);
      Auxiliary.TheArch = Architecture;
      Auxiliary.SSAVer = 1;
      const auto Slot = kObservationSlotBase + kObservationSlotOffset;
      MedBlock Read;
      Read.Id = 0;
      Read.StartAddr = kObservationEntry;
      Read.Succs = {1};
      Read.Ops = {
          op(NdOp::LOAD, Observed, {MedVar::makeConst(Slot, PointerBytes)}),
          op(NdOp::BRANCH, {},
             {MedVar::makeConst(kObservationSuccessor, PointerBytes)})};
      MedBlock Call;
      Call.Id = 1;
      Call.StartAddr = kObservationSuccessor;
      Call.Preds = {0};
      if (Between == Barrier::Store)
        Call.Ops.push_back(
            op(NdOp::STORE, {},
               {MedVar::makeConst(Slot, PointerBytes),
                MedVar::makeConst(kReplacementCallable, PointerBytes)}));
      else if (Between == Barrier::Call)
        Call.Ops.push_back(
            op(NdOp::CALL, Auxiliary,
               {MedVar::makeConst(kReplacementCallable, PointerBytes)}));
      else if (Between == Barrier::OrderedRead) {
        auto Ordered =
            op(NdOp::LOAD, Auxiliary, {MedVar::makeConst(Slot, PointerBytes)});
        Ordered.MemoryOrdering = NdMemoryOrdering::Acquire;
        Call.Ops.push_back(Ordered);
      }
      const auto CallSite = Call.StartAddr + Call.Ops.size();
      Call.Ops.push_back(op(NdOp::INDIR_CALL, Result, {Observed}));
      Call.Ops.push_back(op(NdOp::RETURN, {}, {Result}));
      MedFunc F;
      F.Entry = kObservationEntry;
      F.Name = kObservationFunction;
      F.Blocks = {Read, Call};
      if (Between == Barrier::Join) {
        auto &Entry = F.Blocks.front();
        Entry.Succs = {1, 2};
        const auto OtherEntry = kObservationSuccessor + kObservationBlockStride;
        Entry.Ops.back() =
            op(NdOp::COND_BR, {},
               {MedVar::makeConst(OtherEntry, PointerBytes), Observed});
        MedBlock Other;
        Other.Id = 2;
        Other.StartAddr = OtherEntry;
        Other.Preds = {0};
        Other.Succs = {1};
        Other.Ops = {
            op(NdOp::BRANCH, {},
               {MedVar::makeConst(kObservationSuccessor, PointerBytes)})};
        F.Blocks[1].Preds.push_back(Other.Id);
        F.Blocks.push_back(Other);
      }
      for (auto &B : F.Blocks) {
        for (size_t I = 0; I < B.Ops.size(); ++I)
          B.Ops[I].Addr = B.StartAddr + I;
        B.EndAddr = B.StartAddr + B.Ops.size();
      }
      const std::map<va_t, std::string> Names{{Slot, kObservedImport}};
      MedToHighConverter Converter;
      Converter.setFuncNames(&Names);
      const auto High = Converter.convert(F, Architecture);
      const unsigned Calls =
          checkCallAt(High, CallSite, [&](const HighExpr &E) {
            EXPECT_EQ(E.CallAddr, Slot);
            EXPECT_EQ(E.IsIndirectCall, Between != Barrier::None);
            if (Between == Barrier::None)
              EXPECT_EQ(E.CallTarget, kObservedImport);
            else {
              ASSERT_TRUE(E.IndirectTarget);
              ASSERT_EQ(E.IndirectTarget->Kind, ExprKind::Var);
              EXPECT_EQ(E.IndirectTarget->Var, Observed);
            }
          });
      EXPECT_EQ(Calls, 1u);
    }
}

TEST(MedCachedCallTarget, MachineSummaryAdapterKeepsTheOriginalCallOperand) {
  for (bool Import : {false, true}) {
    const auto M = cached(Import);
    LowBlock B;
    const auto Convert = [](const MedVar &V) {
      return V.isConst()             ? NdVar::cst(V.ConstVal, V.Size)
             : V.Kind == MedVar::Reg ? NdVar::reg(V.RegOff, V.Size)
                                     : NdVar::tmp(V.Id, V.Size);
    };
    for (const auto &Op : M.Ops) {
      LowOp O;
      O.Opcode = Op.Opcode;
      O.Output = Convert(Op.Output);
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        O.addInput(Convert(Op.Inputs[I]));
      B.Ops.push_back(O);
    }
    LowOp Call;
    Call.Opcode = NdOp::INDIR_CALL;
    Call.addInput(NdVar::tmp(5, 8));
    B.Ops.push_back(Call);
    const auto Targets = resolveLowBlockCallTargets(B, Arch::X64);
    ASSERT_EQ(Targets.size(), 1u);
    EXPECT_EQ(Targets.begin()->first, B.Ops.size() - 1);
    EXPECT_EQ(Targets.begin()->second.Address, Import ? 0x3000u : 0x1000u);
    EXPECT_EQ(Targets.begin()->second.ThroughSlot, Import);
    EXPECT_EQ(B.Ops.back(), Call);
    size_t Work = 1;
    EXPECT_TRUE(resolveLowBlockCallTargets(B, Arch::X64, &Work).empty());
    LowOp Clobber;
    Clobber.Opcode = NdOp::STORE;
    Clobber.addInput(NdVar::tmp(2, 8));
    Clobber.addInput(NdVar::cst(0, 4));
    B.Ops.insert(B.Ops.begin() + 3, Clobber);
    EXPECT_TRUE(resolveLowBlockCallTargets(B, Arch::X64).empty());
  }
}

TEST(MedCachedCallTarget, CapturedScalarImportsKeepTheirDeclaredPointerType) {
  class Header : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != 0x3000 && Address != 0x2000)
        return std::nullopt;
      FunctionSym F;
      F.Addr = Address;
      F.Name = Address == 0x3000 ? "Observed" : "replace_pointer";
      F.ReturnType =
          Address == 0x3000 ? NdType::makeInt(4) : NdType::makeVoid();
      if (Address == 0x3000)
        F.Params = {{"code", NdType::makeInt(4, false)}};
      F.PlatformConvention = true;
      F.PrototypeFromHeader = true;
      F.CallConv = DebugCallConv::Cdecl;
      return F;
    }
  } Debug;
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (bool Replace : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Replace);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::COFF;
      Segment Data;
      Data.VA = 0x3000;
      Data.Size = Data.FileSz = 8;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      Data.Data.resize(8);
      Image.Segments.push_back(std::move(Data));
      Import Imported;
      Imported.Name = "Observed";
      Imported.IATAddr = 0x3000;
      Image.Imports.push_back(std::move(Imported));
      ASSERT_TRUE(Image.recordImportStorageSlot(
          0x3000, "Observed", 0, ImportStorageEvidence::LoaderBind));
      const auto Word = NdType::makeInt(8, false);
      const auto Code = NdType::makeInt(4, false);
      HighFunc F;
      F.Entry = 0x1000;
      F.Name = "captured_scalar";
      F.ReturnType = NdType::makeInt(4);
      F.Params = {{"code", Code}};
      auto Target = HighExpr::makeLoad(HighExpr::makeConst(0x3000, 8), Word);
      if (Replace) {
        auto Saved = value(17);
        Saved.TheArch = Architecture;
        HighStmt Capture;
        Capture.Kind = StmtKind::Assign;
        Capture.Dst = HighExpr::makeVar(Saved, Word);
        Capture.Val = Target;
        F.Body.push_back(Capture);
        Target = Capture.Dst;
        HighStmt Change;
        Change.Kind = StmtKind::Call;
        Change.CallExpr = HighExpr::makeCall("replace_pointer", 0x2000, {});
        Change.CallExpr->Type = NdType::makeVoid();
        F.Body.push_back(Change);
      }
      auto Parameter = value(0, 4);
      Parameter.Kind = MedVar::Param;
      Parameter.TheArch = Architecture;
      auto Call = HighExpr::makeCall("indirect", 0x3000,
                                     {HighExpr::makeVar(Parameter, Code)});
      Call->IsIndirectCall = true;
      Call->IndirectTarget = Target;
      Call->Type = F.ReturnType;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = Call;
      F.Body.push_back(Return);
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      Options.Format = BinaryFormat::COFF;
      Options.Image = &Image;
      Options.EmitIncludes = false;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      ASSERT_TRUE(HighCEmitter().emit({F}, Output, Options, &Debug));
      Source = "#include <stdint.h>\n" + Source;
      EXPECT_NE(Source.find("(*__imp_Observed)(uint32_t)"), std::string::npos)
          << Source;
      source_call_execution_test::checkSyntax(
          Source,
          {Architecture == Arch::X64 ? "--target=x86_64-pc-windows-msvc"
                                     : "--target=aarch64-pc-windows-msvc",
           "-ffreestanding"});
      const std::string Oracle = R"c(
static int old_calls, new_calls;
static int32_t original(uint32_t code) {
  ++old_calls;
  return code == UINT32_MAX ? -37 : -41;
}
static int32_t replacement(uint32_t code) { ++new_calls; return 53; }
int32_t (*__imp_Observed)(uint32_t) = original;
void replace_pointer(void) { __imp_Observed = replacement; }
int main(void) {
  return captured_scalar(UINT32_MAX) != -37 || old_calls != 1 || new_calls;
}
)c";
      for (llvm::StringRef Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Source + Oracle,
            {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
    }
}

TEST(MedCachedCallTarget, AffineCodeValuesKeepTheirAddressRole) {
  for (uint16_t Width : {uint16_t(4), uint16_t(8)})
    for (NdOp Opcode : {NdOp::INT_ADD, NdOp::INT_SUB}) {
      MedBlock B;
      auto Result = value(1, Width);
      const uint64_t Base = Width == 4 ? UINT32_MAX : UINT64_MAX;
      B.Ops = {
          op(Opcode, Result,
             {MedVar::makeConst(Base, Width), MedVar::makeConst(17, Width)})};
      const auto Target = resolvePreSSAMedCallTarget(
          B, Result, Width == 4 ? Arch::X86 : Arch::X64);
      ASSERT_TRUE(Target);
      EXPECT_FALSE(Target->ThroughSlot);
      EXPECT_EQ(Target->Address, Opcode == NdOp::INT_ADD ? 16u : Base - 17);
      B.Ops.push_back(op(NdOp::LOAD, value(2, Width), {Result}));
      const auto Slot = resolvePreSSAMedCallTarget(
          B, value(2, Width), Width == 4 ? Arch::X86 : Arch::X64);
      ASSERT_TRUE(Slot);
      EXPECT_TRUE(Slot->ThroughSlot);
      EXPECT_EQ(Slot->Address, Target->Address);
    }
}

TEST(MedCachedCallTarget, AffineCodeProofRejectsUnknownOrFutureInputs) {
  MedBlock B;
  B.Ops = {op(NdOp::INT_ADD, value(1), {value(2), MedVar::makeConst(17, 8)})};
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(1), Arch::X64));
  // A later definition cannot become evidence for a value at this call.
  B.Ops.push_back(op(NdOp::COPY, value(2), {MedVar::makeConst(0x1000, 8)}));
  EXPECT_FALSE(resolveMedCallTarget(B, 1, value(1)));
  B.Ops.front().Inputs[0] = MedVar::makeConst(0x1000, 4);
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(1), Arch::X64));
  B.Ops.front().Inputs[0] = MedVar::makeConst(0x1000, 8);
  B.Ops.front().MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(1), Arch::X64));
  B.Ops.front().MemoryOrdering = NdMemoryOrdering::None;
  size_t Work = 0;
  EXPECT_FALSE(resolvePreSSAMedCallTarget(B, value(1), Arch::X64, &Work));
}
} // namespace
