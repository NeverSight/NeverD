//===- ModuleJumpTableBudgetTests.cpp - Bounded module inventory ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Separates retained evidence from repeated CFG work, whose allowance grows
/// with the code an inventory reads.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "PipelineLowIRDetail.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/Parallel.h"

#include "llvm/Support/Error.h"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <vector>

using namespace neverd;

namespace {

class ModuleJumpTableBudget : public NeverDLiftTest {
protected:
  struct Inputs {
    BinaryImage Image;
    std::vector<LowFunc> Functions;
  };

  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  std::optional<Inputs> fixture(unsigned CodeReferences = 0,
                                bool PureAfterCall = false,
                                const std::string &ExtraAssembly = {}) {
    const auto Source = tmpFile("inventory.s");
    const auto Object = tmpFile("inventory.o");
    {
      std::ifstream Original(fs::path(TEST_SOURCE_DIR) /
                             "x86_64/test_module_address_owner_induction.s");
      if (!Original)
        return std::nullopt;
      std::ofstream Out(Source);
      if (PureAfterCall) {
        std::string Assembly((std::istreambuf_iterator<char>(Original)), {});
        const std::string Entry = "module_owner_safe_dispatch:\n";
        const auto At = Assembly.find(Entry);
        if (At == std::string::npos)
          return std::nullopt;
        Assembly.insert(At + Entry.size(),
                        "callq module_pure_intrinsic_callee\n"
                        "pdepq %rdx, %rcx, %r8\n");
        Out << Assembly;
        Out << "\n.text\n.globl module_pure_intrinsic_callee\n"
               ".type module_pure_intrinsic_callee,@function\n"
               "module_pure_intrinsic_callee:\nxorl %eax,%eax\nretq\n"
               ".size module_pure_intrinsic_callee,"
               ".-module_pure_intrinsic_callee\n";
      } else {
        Out << Original.rdbuf();
      }
      if (CodeReferences) {
        Out << "\n.section .text.module_inventory,\"ax\",@progbits\n"
               ".globl module_inventory\n"
               ".type module_inventory,@function\nmodule_inventory:\n";
        for (unsigned I = 0; I < CodeReferences; ++I)
          Out << "leaq module_owner_safe_dispatch(%rip), %rax\n";
        Out << "xorl %eax,%eax\nretq\n"
               ".size module_inventory,.-module_inventory\n";
      }
      Out << ExtraAssembly;
    }
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    if (!Built.ok())
      return std::nullopt;
    auto Image = loadBinary(Object);
    EXPECT_TRUE(static_cast<bool>(Image));
    if (!Image) {
      llvm::consumeError(Image.takeError());
      return std::nullopt;
    }
    Inputs Result{std::move(*Image), {}};
    std::vector<const char *> Names{"module_owner_safe_dispatch",
                                    "module_owner_mutated_dispatch",
                                    "module_owner_mutated_loop_writer",
                                    "module_owner_unrelated_pointer_induction"};
    if (CodeReferences)
      Names.push_back("module_inventory");
    if (PureAfterCall)
      Names.push_back("module_pure_intrinsic_callee");
    std::set<va_t> Entries;
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      EXPECT_NE(S, nullptr) << Name;
      if (!S)
        return std::nullopt;
      Entries.insert(S->Addr);
    }
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Result.Image.Arch));
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    for (const char *Name : Names) {
      const Symbol *S = Result.Image.findSymbol(Name);
      Result.Functions.push_back(
          Builder.build(Result.Image, Dec, S->Addr, Name));
    }
    return Result;
  }

  // Insert a consumer after a genuine loader-backed table materialization.
  // Retain the original table certificate and source occurrence; these tests
  // exercise ordinary-use arbitration, not manufactured relocation evidence.
  static bool insertTableConsumer(Inputs &I, std::vector<LowOp> Uses) {
    LowFunc &F = I.Functions.front();
    if (F.JumpTables.size() != 1)
      return false;
    const auto Source = std::find_if(
        F.RelocatedInstructionAddressOccurrences.begin(),
        F.RelocatedInstructionAddressOccurrences.end(), [&](const auto &O) {
          return O.TargetVA == F.JumpTables.front().BaseAddr;
        });
    if (Source == F.RelocatedInstructionAddressOccurrences.end())
      return false;
    for (LowBlock &B : F.Blocks) {
      for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
        const LowOp &Materialization = B.Ops[Index];
        if (Materialization.Addr != Source->InstructionAddr ||
            Materialization.Seq != Source->OpSeq ||
            Materialization.Output.Size != 8 ||
            (Source->DefinesOutput &&
             Materialization.Output != Source->OutputWitness))
          continue;
        LowOp Input;
        Input.Opcode = NdOp::COPY;
        Input.Output = NdVar::tmp(TmpBase + 0x40000, 8);
        Input.addInput(Materialization.Output);
        Uses.insert(Uses.begin(), Input);
        for (size_t U = 0; U < Uses.size(); ++U) {
          Uses[U].Addr = Materialization.Addr;
          Uses[U].Seq = 1000 + U;
        }
        B.Ops.insert(B.Ops.begin() + Index + 1, Uses.begin(), Uses.end());
        for (auto &Boundary : B.InstructionBoundaries) {
          if (Boundary.FirstOp > Index)
            Boundary.FirstOp += Uses.size();
          else if (Boundary.FirstOp + Boundary.OpCount > Index)
            Boundary.OpCount += Uses.size();
        }
        return true;
      }
    }
    return false;
  }

  static LowOp valueIntrinsic(Intrinsic Id = Intrinsic::Pdep) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::tmp(TmpBase + 0x40008, 8);
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
    Op.addInput(NdVar::tmp(TmpBase + 0x40000, 8));
    Op.addInput(NdVar::cst(UINT64_MAX, 8));
    return Op;
  }

  static size_t opCount(const std::vector<LowFunc> &Functions) {
    size_t Count = 0;
    for (const LowFunc &F : Functions)
      for (const LowBlock &B : F.Blocks)
        Count += B.Ops.size();
    return Count;
  }

  static std::set<va_t> requested(const std::vector<LowFunc> &Functions) {
    std::set<va_t> Slots;
    for (const LowFunc &F : Functions)
      for (const JumpTable &JT : F.JumpTables)
        Slots.insert(JT.SuppressibleRelocationSlots.begin(),
                     JT.SuppressibleRelocationSlots.end());
    return Slots;
  }

  static LowFunc ordinaryPadding(size_t Ops) {
    LowFunc F;
    F.Entry = 0x70000000;
    F.Name = "ordinary_padding";
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.EndAddr = F.Entry + Ops;
    // Hand-built LowIR exercises inventory capacity without manufacturing any
    // relocation, address, call-result or table certificate.
    B.Ops.resize(Ops);
    for (LowOp &Op : B.Ops)
      Op.Opcode = NdOp::NOP;
    F.Blocks.push_back(std::move(B));
    return F;
  }

  // The visits every inventory may make, whatever it reads.  Passed as the
  // test budget it gives the transfer account its fixed sixteen times too.
  static constexpr size_t FixedAllowance =
      size_t{limits::kMaxJumpTableEvidenceWork} * limits::kMaxMultiStageRetries;

  // One block that advances the stack pointer and loops to itself: its frame
  // offsets gain one alternative a round until they widen.
  static LowFunc frameWalk(va_t Entry, const BinaryImage &Image) {
    LowFunc F = ordinaryPadding(1);
    F.Entry = Entry;
    LowBlock &B = F.Blocks.front();
    B.StartAddr = Entry;
    B.EndAddr = Entry + 1;
    B.Preds = {0};
    B.Succs = {0};
    LowOp &Advance = B.Ops.front();
    Advance.Opcode = NdOp::INT_ADD;
    Advance.Output = NdVar::reg(getTargetRegInfo(Image.Arch).StackPointer, 8);
    Advance.addInput(Advance.Output);
    Advance.addInput(NdVar::cst(1, 8));
    return F;
  }

  // A register that holds the frame address on the path from the entry and
  // nothing known on the path from a second root, walked round a loop: an
  // imprecise value that may lie outside the frame.
  static LowFunc impreciseFrameWalk(va_t Entry, const BinaryImage &Image) {
    const TargetRegInfo &TRI = getTargetRegInfo(Image.Arch);
    const NdVar Walker = NdVar::reg(TRI.IntReturnReg, 8);
    LowFunc F = ordinaryPadding(0);
    F.Entry = Entry;
    F.Blocks.resize(3);
    for (size_t Index = 0; Index < F.Blocks.size(); ++Index) {
      LowBlock &B = F.Blocks[Index];
      B.Id = static_cast<int>(Index);
      B.StartAddr = Entry + Index * 16;
      B.EndAddr = B.StartAddr + 16;
    }
    LowOp Copy;
    Copy.Opcode = NdOp::COPY;
    Copy.Output = Walker;
    Copy.addInput(NdVar::reg(TRI.StackPointer, 8));
    LowOp Advance;
    Advance.Opcode = NdOp::INT_ADD;
    Advance.Output = Walker;
    Advance.addInput(Walker);
    Advance.addInput(NdVar::cst(8, 8));
    F.Blocks[0].Ops = {Copy};
    F.Blocks[0].Succs = {2};
    F.Blocks[1].Succs = {2};
    F.Blocks[2].Ops = {Advance};
    F.Blocks[2].Preds = {0, 1, 2};
    F.Blocks[2].Succs = {2};
    for (LowBlock &B : F.Blocks)
      for (size_t Index = 0; Index < B.Ops.size(); ++Index) {
        B.Ops[Index].Addr = B.StartAddr + Index;
        B.Ops[Index].Seq = 0;
      }
    F.ModuleAnalysisRoots = {F.Blocks[0].StartAddr, F.Blocks[1].StartAddr};
    return F;
  }

  // The least test budget the arbitration of I completes with.
  static size_t leastCompletingBudget(const Inputs &I) {
    auto completes = [&](size_t Budget) {
      return pipeline_detail::arbitrateModuleJumpTablesForTesting(
                 I.Image, I.Functions, Budget)
          .AnalysisComplete;
    };
    size_t Low = 0, High = FixedAllowance;
    if (!completes(High))
      return SIZE_MAX;
    while (Low < High) {
      const size_t Mid = Low + (High - Low) / 2;
      if (completes(Mid))
        High = Mid;
      else
        Low = Mid + 1;
    }
    return Low;
  }

  // Builds one more function of the fixture's image, as fixture() builds its
  // own.
  static bool appendFunction(Inputs &I, const char *Name) {
    const Symbol *S = I.Image.findSymbol(Name);
    if (!S)
      return false;
    std::set<va_t> Entries{S->Addr};
    for (const LowFunc &F : I.Functions)
      Entries.insert(F.Entry);
    Decoder Dec;
    if (!Dec.init(I.Image.Arch))
      return false;
    CFGBuilder Builder;
    Builder.setKnownFuncEntries(&Entries);
    I.Functions.push_back(Builder.build(I.Image, Dec, S->Addr, Name));
    return true;
  }

  // Assembles a module of its own and builds the named functions.
  std::optional<Inputs> assembled(const std::string &Assembly,
                                  const std::vector<const char *> &Names) {
    const auto Source = tmpFile("assembled.s");
    const auto Object = tmpFile("assembled.o");
    {
      std::ofstream Out(Source);
      Out << Assembly;
    }
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "x86_64-linux-gnu", "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    if (!Built.ok())
      return std::nullopt;
    auto Image = loadBinary(Object);
    EXPECT_TRUE(static_cast<bool>(Image));
    if (!Image) {
      llvm::consumeError(Image.takeError());
      return std::nullopt;
    }
    Inputs Result{std::move(*Image), {}};
    for (const char *Name : Names)
      if (!appendFunction(Result, Name)) {
        ADD_FAILURE() << Name;
        return std::nullopt;
      }
    return Result;
  }

  static void expectAbandoned(const Inputs &I,
                              std::optional<size_t> Budget = std::nullopt) {
    const auto Slots = requested(I.Functions);
    ASSERT_FALSE(Slots.empty());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I.Image, I.Functions, Budget);
    EXPECT_FALSE(Result.AnalysisComplete);
    EXPECT_EQ(Result.ProtectedRelocationSlots, Slots);
    for (const LowFunc &F : I.Functions)
      for (const JumpTable &JT : F.JumpTables)
        EXPECT_EQ(Result.UnsafeBranches.count(JT.InsnAddr), 1u);
  }
};

TEST_F(ModuleJumpTableBudget,
       PureIntrinsicAfterCallDoesNotDemandClobberedLanes) {
  // RCX/RDX are unknown after CALL. Resolving them merely because unused PDEP
  // reads them used to abandon the entire table-consumer completeness proof.
  auto I = fixture(0, true);
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto Slots = requested({I->Functions.front()});
  ASSERT_FALSE(Slots.empty());
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  for (va_t Slot : Slots)
    EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
}

TEST(ModuleIntrinsicEffects, ExplicitValueContractsValidateCompleteShapes) {
  const auto Unary = [](Intrinsic Id, uint16_t Size) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::tmp(TmpBase + 8, Size);
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
    Op.addInput(NdVar::tmp(TmpBase, Size));
    return Op;
  };
  for (Intrinsic Id : {Intrinsic::ArmRbit, Intrinsic::A64_Rbit}) {
    LowOp Op = Unary(Id, 4);
    EXPECT_TRUE(lowIntrinsicHasOnlyValueEffects(Op));
    Op.Inputs[1].Size = 2;
    EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Op));
  }
  for (uint16_t Size : {uint16_t{1}, uint16_t{4}, uint16_t{8}})
    EXPECT_TRUE(
        lowIntrinsicHasOnlyValueEffects(Unary(Intrinsic::A64_Rbit, Size)));
  LowOp Fxam = Unary(Intrinsic::X87Fxam, 10);
  Fxam.Output = NdVar::tmp(TmpBase + 16, 2);
  Fxam.addInput(Fxam.Output);
  EXPECT_TRUE(lowIntrinsicHasOnlyValueEffects(Fxam));
  Fxam.Inputs[1].Size = 8;
  EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Fxam));
  EXPECT_FALSE(lowIntrinsicHasOnlyValueEffects(Unary(Intrinsic::Ldg, 8)));
}

TEST_F(ModuleJumpTableBudget, PureIntrinsicDoesNotEscapeItsUnusedInput) {
  for (Intrinsic Id : {Intrinsic::Pdep, Intrinsic::Pext}) {
    auto I = fixture();
    ASSERT_TRUE(I);
    const auto Slots = requested({I->Functions.front()});
    ASSERT_FALSE(Slots.empty());
    // RDX is an ABI argument register but is unrelated to the switch index.
    // A pure intrinsic must neither publish it nor erase its reaching value.
    LowOp Argument;
    Argument.Opcode = NdOp::COPY;
    Argument.Output =
        NdVar::reg(getTargetRegInfo(I->Image.Arch).IntParamRegs[2], 8);
    Argument.addInput(NdVar::tmp(TmpBase + 0x40000, 8));
    ASSERT_TRUE(insertTableConsumer(*I, {Argument, valueIntrinsic(Id)}));
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
  }
}

TEST_F(ModuleJumpTableBudget, PureIntrinsicResultKeepsLaterEscapeEvidence) {
  for (unsigned UseKind : {0u, 1u, 2u}) {
    SCOPED_TRACE(UseKind);
    auto I = fixture();
    ASSERT_TRUE(I);
    const auto Slots = requested({I->Functions.front()});
    ASSERT_FALSE(Slots.empty());
    LowOp Transform = valueIntrinsic();
    std::vector<LowOp> Uses{Transform};
    if (UseKind == 1) {
      LowOp Argument;
      Argument.Opcode = NdOp::COPY;
      Argument.Output =
          NdVar::reg(getTargetRegInfo(I->Image.Arch).IntParamRegs[2], 8);
      Argument.addInput(Transform.Output);
      Uses.push_back(Argument);
      LowOp Call;
      Call.Opcode = NdOp::CALL;
      Call.addInput(NdVar::codeAddress(I->Functions.back().Entry, 8));
      Uses.push_back(Call);
    } else {
      LowOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(
          NdVar::reg(getTargetRegInfo(I->Image.Arch).IntParamRegs[3], 8));
      // The second form checks that a pure operation is not a call barrier
      // which destroys the original address held in an instruction temporary.
      Store.addInput(UseKind == 0 ? Transform.Output : Transform.Inputs[1]);
      Uses.push_back(Store);
    }
    ASSERT_TRUE(insertTableConsumer(*I, std::move(Uses)));
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 1u);
  }
}

TEST_F(ModuleJumpTableBudget,
       EffectfulAndUnknownIntrinsicsRetainTheirOperands) {
  for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto I = fixture();
    ASSERT_TRUE(I);
    const auto Slots = requested({I->Functions.front()});
    ASSERT_FALSE(Slots.empty());
    LowOp Op = valueIntrinsic();
    if (Mutation == 0) {
      Op.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::Clflush);
      Op.Output = {};
      Op.NumInputs = 2;
    } else if (Mutation == 1) {
      Op.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::MaskedLoadD);
      Op.Output.Size = 16;
      Op.Inputs[2] = NdVar::tmp(TmpBase + 0x50000, 16);
    } else if (Mutation == 2)
      Op.Inputs[0].Offset = static_cast<uint64_t>(Intrinsic::_Count);
    else if (Mutation == 3)
      Op.Inputs[0].Offset += UINT64_C(1) << 32;
    else if (Mutation == 4)
      Op.Inputs[0].Size = 8;
    else if (Mutation == 5)
      Op.Output.Size = 2;
    else if (Mutation == 6)
      Op.Inputs[2].Size = 4;
    else
      Op.Inputs[0] = NdVar::tmp(TmpBase + 0x50000, 2);
    // Only the explicit TEMP operand carries the table address: the test may
    // not pass merely because an unrelated ABI argument still holds its root.
    ASSERT_TRUE(insertTableConsumer(*I, {Op}));
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 1u);
  }
}

TEST_F(ModuleJumpTableBudget, ModuleSizeAloneDoesNotAbandonTheInventory) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto Baseline = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Baseline.AnalysisComplete);
  const auto SafeSlots =
      I->Functions.front().JumpTables.front().SuppressibleRelocationSlots;
  ASSERT_FALSE(SafeSlots.empty());
  for (va_t Slot : SafeSlots)
    ASSERT_EQ(Baseline.ProtectedRelocationSlots.count(Slot), 0u);
  const size_t BaseOps = opCount(I->Functions);
  // Padding spends no retained evidence, and its size is no reason to give
  // up: the allowance for repeated work grows with the code read.
  for (size_t Total : {size_t{65537}, limits::kMaxSSAFunctionOps - 1,
                       limits::kMaxSSAFunctionOps,
                       limits::kMaxSSAFunctionOps + 1, size_t{1000000}}) {
    SCOPED_TRACE(Total);
    I->Functions.push_back(ordinaryPadding(Total - BaseOps));
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      EXPECT_TRUE(Result.AnalysisComplete);
      EXPECT_EQ(Result.ProtectedRelocationSlots,
                Baseline.ProtectedRelocationSlots);
      EXPECT_EQ(Result.UnsafeBranches, Baseline.UnsafeBranches);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
    }
    I->Functions.pop_back();
  }
}

TEST_F(ModuleJumpTableBudget, UnrelatedCodeOccurrencesUseTheScanAccount) {
  auto I = fixture(1100);
  ASSERT_TRUE(I);
  ASSERT_GE(I->Functions.back().RelocatedInstructionAddressOccurrences.size(),
            1100u);
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto Slots =
      I->Functions.front().JumpTables.front().SuppressibleRelocationSlots;
  ASSERT_FALSE(Slots.empty());
  for (va_t Slot : Slots)
    EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 0u);
}

TEST_F(ModuleJumpTableBudget, UnrelatedAbsoluteFieldsUseTheScanAccount) {
  const std::string Inventory =
      "\n.section .text.unexplored_inventory,\"ax\",@progbits\n"
      ".rept 12000\n"
      "movabsq $.Lmodule_owner_unrelated_data+8, %rax\n"
      "movabsq $module_owner_safe_dispatch, %rax\n"
      ".endr\n";
  for (bool ExposesTable : {false, true}) {
    SCOPED_TRACE(ExposesTable);
    auto I = fixture(
        0, false,
        Inventory +
            (ExposesTable ? "movabsq $.Lmodule_owner_safe_table, %rax\n" : ""));
    ASSERT_TRUE(I);
    ASSERT_GE(I->Image.DataAddressRelocOperands.size(), 12000u);
    ASSERT_GE(I->Image.CodeAddressRelocOperands.size(), 12000u);
    ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
    const auto &Table = I->Functions.front().JumpTables.front();
    const auto Slots = Table.SuppressibleRelocationSlots;
    ASSERT_FALSE(Slots.empty());
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      ASSERT_TRUE(Result.AnalysisComplete);
      for (va_t Slot : Slots)
        EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot),
                  ExposesTable ? 1u : 0u);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
    }
  }
}

TEST_F(ModuleJumpTableBudget, UnrelatedLoaderInputsRemainBounded) {
  for (bool Pointers : {false, true}) {
    SCOPED_TRACE(Pointers);
    auto I = fixture(
        0, false,
        Pointers ? "\n.section .data.unexplored_inventory,\"aw\",@progbits\n"
                   ".rept 65537\n.quad .Lmodule_owner_unrelated_data+8\n.endr\n"
                 : "\n.section .text.unexplored_inventory,\"ax\",@progbits\n"
                   ".rept 65537\n"
                   "movabsq $.Lmodule_owner_unrelated_data+8, %rax\n.endr\n");
    ASSERT_TRUE(I);
    expectAbandoned(*I);
  }
}

TEST_F(ModuleJumpTableBudget, DisjointOwnerGapsDoNotSpendSourceScanWork) {
  // These addresses lie between two complete physical table envelopes, in
  // the same section. A single module-wide min/max envelope cannot reject
  // them and used to exhaust the scan account before reaching a real use.
  const std::string Inventory =
      "\n.section .text.unexplored_inventory,\"ax\",@progbits\n"
      ".rept 15000\n"
      "movabsq $.Lmodule_owner_safe_table+17, %rax\n"
      "movabsq $.Lmodule_owner_safe_table+23, %rax\n"
      ".endr\n";
  for (int ExposedOffset : {-1, 8, 16}) {
    SCOPED_TRACE(ExposedOffset);
    auto I = fixture(1000, false,
                     Inventory + (ExposedOffset < 0
                                      ? ""
                                      : "movabsq $.Lmodule_owner_safe_table+" +
                                            std::to_string(ExposedOffset) +
                                            ", %rax\n"));
    ASSERT_TRUE(I);
    ASSERT_GE(I->Image.DataAddressRelocOperands.size(), 30000u);
    ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
    ASSERT_EQ(I->Functions[1].JumpTables.size(), 1u);
    const auto &Table = I->Functions.front().JumpTables.front();
    va_t End = Table.BaseAddr;
    for (const auto &Range : Table.StorageRanges) {
      ASSERT_TRUE(Range.storageEnd());
      End = std::max(End, *Range.storageEnd());
    }
    ASSERT_EQ(End, Table.BaseAddr + 16);
    ASSERT_EQ(I->Functions[1].JumpTables.front().BaseAddr, Table.BaseAddr + 24);
    const auto Slots = Table.SuppressibleRelocationSlots;
    ASSERT_FALSE(Slots.empty());
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      ASSERT_TRUE(Result.AnalysisComplete);
      for (va_t Slot : Slots)
        EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot),
                  ExposedOffset >= 0 ? 1u : 0u);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
    }
  }
}

TEST_F(ModuleJumpTableBudget, UnrelatedDataPointersDoNotRetainEvidence) {
  const std::string Inventory =
      "\n.section .data.unexplored_inventory,\"aw\",@progbits\n"
      ".rept 5000\n"
      ".quad .Lmodule_owner_unrelated_data+8\n"
      ".endr\n";
  for (bool ExposesTable : {false, true}) {
    SCOPED_TRACE(ExposesTable);
    auto I = fixture(
        0, false,
        Inventory + (ExposesTable ? ".quad .Lmodule_owner_safe_table\n" : ""));
    ASSERT_TRUE(I);
    ASSERT_GE(I->Image.DataPtrRelocSlots.size(), 5000u);
    ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
    const auto &Table = I->Functions.front().JumpTables.front();
    const auto Branch = Table.InsnAddr;
    const auto Slots = Table.SuppressibleRelocationSlots;
    ASSERT_FALSE(Slots.empty());
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Reverse);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions);
      ASSERT_TRUE(Result.AnalysisComplete);
      for (va_t Slot : Slots)
        EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot),
                  ExposesTable ? 1u : 0u);
      EXPECT_EQ(Result.UnsafeBranches.count(Branch), ExposesTable ? 1u : 0u);
      if (Reverse)
        std::reverse(I->Functions.begin(), I->Functions.end());
    }
    // Even an unrelated inventory must be read completely. A malformed last
    // field cannot be discarded merely because earlier pointers were disjoint.
    I->Image.DataPtrRelocSlots.insert(InvalidVA - 8);
    expectAbandoned(*I);
  }
}

TEST_F(ModuleJumpTableBudget, SharedFieldKeepsEveryIndependentSource) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto &JT = I->Functions.front().JumpTables.front();
  const auto Slots = JT.SuppressibleRelocationSlots;
  ASSERT_FALSE(Slots.empty());
  LowFunc Independent = I->Functions.front();
  const auto SourceIt =
      std::find_if(Independent.RelocatedInstructionAddressOccurrences.begin(),
                   Independent.RelocatedInstructionAddressOccurrences.end(),
                   [&](const auto &Occurrence) {
                     return Occurrence.FieldVA != InvalidVA &&
                            Occurrence.TargetVA == JT.BaseAddr;
                   });
  ASSERT_NE(SourceIt, Independent.RelocatedInstructionAddressOccurrences.end());
  const auto Source = *SourceIt;
  const auto BlockIt =
      std::find_if(Independent.Blocks.begin(), Independent.Blocks.end(),
                   [&](const LowBlock &B) {
                     return B.StartAddr <= Source.InstructionAddr &&
                            Source.InstructionAddr < B.EndAddr;
                   });
  ASSERT_NE(BlockIt, Independent.Blocks.end());
  const auto BoundaryIt = std::find_if(
      BlockIt->InstructionBoundaries.begin(),
      BlockIt->InstructionBoundaries.end(), [&](const auto &Boundary) {
        return Boundary.Address == Source.InstructionAddr;
      });
  ASSERT_NE(BoundaryIt, BlockIt->InstructionBoundaries.end());
  LowBlock Prefix = *BlockIt;
  const size_t PrefixOps = BoundaryIt->FirstOp + BoundaryIt->OpCount;
  ASSERT_LE(PrefixOps, Prefix.Ops.size());
  Prefix.Ops.resize(PrefixOps);
  Prefix.InstructionBoundaries.resize(
      BoundaryIt - BlockIt->InstructionBoundaries.begin() + 1);
  Prefix.Preds.clear();
  Prefix.Succs.clear();
  Prefix.ExceptionalPreds.clear();
  Prefix.ExceptionalSuccs.clear();
  Prefix.EndAddr = BoundaryIt->Address + BoundaryIt->Size + 1;
  LowOp Overwrite;
  Overwrite.Opcode = NdOp::COPY;
  Overwrite.Addr = Prefix.EndAddr - 1;
  Overwrite.Output =
      NdVar::reg(getTargetRegInfo(I->Image.Arch).IntReturnReg, 8);
  Overwrite.addInput(NdVar::cst(0, 8));
  Prefix.Ops.push_back(Overwrite);
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Overwrite.Addr;
  Return.Seq = 1;
  Return.addInput(Overwrite.Output);
  Prefix.Ops.push_back(Return);
  for (const LowOp &Op : Prefix.Ops) {
    ASSERT_NE(Op.Opcode, NdOp::LOAD);
    ASSERT_NE(Op.Opcode, NdOp::STORE);
    ASSERT_NE(Op.Opcode, NdOp::CALL);
    ASSERT_NE(Op.Opcode, NdOp::INDIR_CALL);
  }
  // This context retains the exact materialization prefix, then overwrites
  // the pointer and returns a scalar. No ordinary use can independently veto
  // suppression; only the matching source inventory can protect the owner.
  Independent.Name = "independent_context_without_table_witness";
  Independent.JumpTables.clear();
  Independent.Blocks = {std::move(Prefix)};
  Independent.ModuleAnalysisRoots = {Independent.Entry};
  Independent.OrdinaryModuleAnalysisRoots = {Independent.Entry};
  Independent.RelocatedInstructionAddressOccurrences.clear();
  I->Functions.push_back(std::move(Independent));
  const auto WithoutSource =
      pipeline_detail::arbitrateModuleJumpTablesForTesting(I->Image,
                                                           I->Functions);
  ASSERT_TRUE(WithoutSource.AnalysisComplete);
  for (va_t Slot : Slots)
    ASSERT_EQ(WithoutSource.ProtectedRelocationSlots.count(Slot), 0u);
  I->Functions.back().RelocatedInstructionAddressOccurrences = {Source};
  // A published context is not a loader field. Keeping an unmatched index
  // key must neither bind it to a numerically related field nor grant the
  // independent context another source's occurrence permission.
  auto &IndependentSource =
      I->Functions.back().RelocatedInstructionAddressOccurrences.front();
  IndependentSource.FieldVA = InvalidVA - 1;
  const auto UnmatchedSource =
      pipeline_detail::arbitrateModuleJumpTablesForTesting(I->Image,
                                                           I->Functions);
  ASSERT_TRUE(UnmatchedSource.AnalysisComplete);
  for (va_t Slot : Slots)
    ASSERT_EQ(UnmatchedSource.ProtectedRelocationSlots.count(Slot), 0u);
  IndependentSource = Source;
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    for (va_t Slot : Slots)
      EXPECT_EQ(Result.ProtectedRelocationSlots.count(Slot), 1u);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
  }
}

TEST_F(ModuleJumpTableBudget, MissingPCRelativeSourcesProtectTheirContainer) {
  auto I = fixture();
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const auto &JT = I->Functions.front().JumpTables.front();
  const auto SourceIt = std::find_if(
      I->Functions.front().RelocatedInstructionAddressOccurrences.begin(),
      I->Functions.front().RelocatedInstructionAddressOccurrences.end(),
      [&](const auto &Source) { return Source.TargetVA == JT.BaseAddr; });
  ASSERT_NE(SourceIt,
            I->Functions.front().RelocatedInstructionAddressOccurrences.end());
  const va_t FieldVA = SourceIt->FieldVA;
  ASSERT_NE(FieldVA, InvalidVA);
  ASSERT_NE(I->Image.DataAddressRelocOperands.count(FieldVA), 0u);
  ASSERT_TRUE(I->Image.DataAddressRelocOperands.at(FieldVA)
                  .PCRelativeFromInstructionEnd);
  // This loader approximation lies outside every table envelope. Without a
  // published instruction end, only its container is authoritative.
  I->Image.DataAddressRelocOperands.at(FieldVA).TargetVA = 0;
  for (LowFunc &F : I->Functions)
    std::erase_if(
        F.RelocatedInstructionAddressOccurrences,
        [&](const auto &Source) { return Source.FieldVA == FieldVA; });
  const auto Slots = requested(I->Functions);
  ASSERT_FALSE(Slots.empty());
  for (bool Reverse : {false, true}) {
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    EXPECT_EQ(Result.ProtectedRelocationSlots, Slots);
  }
}

TEST_F(ModuleJumpTableBudget,
       ScanAndSourceRetentionExhaustionProtectEverySlot) {
  for (unsigned Count : {5000u, 16400u}) {
    SCOPED_TRACE(Count);
    auto I = fixture(1);
    ASSERT_TRUE(I);
    auto &Sources = I->Functions.back().RelocatedInstructionAddressOccurrences;
    ASSERT_FALSE(Sources.empty());
    const auto Source = Sources.front();
    ASSERT_EQ(Source.Provenance, ConstantAddressProvenance::CodeAddress);
    Sources.assign(Count, Source);
    expectAbandoned(*I);
    std::reverse(I->Functions.begin(), I->Functions.end());
    expectAbandoned(*I);
  }
}

TEST_F(ModuleJumpTableBudget, QueueAndRepeatedLongBlockWorkRemainBounded) {
  auto I = fixture();
  ASSERT_TRUE(I);
  LowFunc Chain = ordinaryPadding(1);
  Chain.Blocks.clear();
  for (size_t Index = 0; Index < FixedAllowance / 2 + 1; ++Index) {
    LowBlock B;
    B.Id = static_cast<int>(Index);
    B.StartAddr = Chain.Entry + Index;
    B.EndAddr = B.StartAddr + 1;
    B.Ops.resize(1);
    B.Ops.front().Opcode = NdOp::NOP;
    if (Index)
      B.Preds.push_back(B.Id - 1);
    B.Succs.push_back(B.Id + 1);
    Chain.Blocks.push_back(std::move(B));
  }
  Chain.Blocks.back().Succs.clear();
  I->Functions.push_back(std::move(Chain));
  expectAbandoned(*I, FixedAllowance);
  I->Functions.pop_back();

  LowFunc Loop = ordinaryPadding(100000);
  LowBlock &B = Loop.Blocks.front();
  B.Preds = {0};
  B.Succs = {0};
  LowOp Advance;
  Advance.Opcode = NdOp::INT_ADD;
  Advance.Output = NdVar::reg(getTargetRegInfo(I->Image.Arch).StackPointer, 8);
  Advance.addInput(Advance.Output);
  Advance.addInput(NdVar::cst(1, 8));
  B.Ops.push_back(Advance);
  I->Functions.push_back(std::move(Loop));
  expectAbandoned(*I, FixedAllowance);
}

TEST_F(ModuleJumpTableBudget, FinalRecordingSpendsTheSameTransferAccount) {
  auto I = fixture();
  ASSERT_TRUE(I);
  // Entry SP needs two distinct input transfers. The unchanged third input
  // reuses its output, but recording still needs a complete third transfer.
  // Only that final pass exhausts the larger input.
  for (size_t Ops : {size_t{330000}, size_t{350000}}) {
    SCOPED_TRACE(Ops);
    LowFunc Loop = ordinaryPadding(Ops);
    Loop.Blocks.front().Preds = {0};
    Loop.Blocks.front().Succs = {0};
    I->Functions.push_back(std::move(Loop));
    if (Ops == 330000) {
      const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
          I->Image, I->Functions, FixedAllowance);
      EXPECT_TRUE(Result.AnalysisComplete);
    } else {
      expectAbandoned(*I, FixedAllowance);
    }
    I->Functions.pop_back();
  }
}

TEST_F(ModuleJumpTableBudget, UnchangedIncomingFactsReuseACompletedTransfer) {
  auto I = fixture();
  ASSERT_TRUE(I);
  const auto Baseline = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Baseline.AnalysisComplete);
  LowFunc Loop = ordinaryPadding(300000);
  Loop.Blocks.front().Preds = {0};
  Loop.Blocks.front().Succs = {0};
  I->Functions.push_back(std::move(Loop));
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions, FixedAllowance);
    ASSERT_TRUE(Result.AnalysisComplete);
    EXPECT_EQ(Result.ProtectedRelocationSlots,
              Baseline.ProtectedRelocationSlots);
    EXPECT_EQ(Result.UnsafeBranches, Baseline.UnsafeBranches);
    if (Reverse)
      std::reverse(I->Functions.begin(), I->Functions.end());
  }
}

TEST_F(ModuleJumpTableBudget, StableOutputStillRecordsTheLatestIncomingUse) {
  for (bool KillBeforeStore : {false, true}) {
    SCOPED_TRACE(KillBeforeStore);
    auto I = fixture();
    ASSERT_TRUE(I);
    ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
    const auto &Table = I->Functions.front().JumpTables.front();
    const va_t Branch = Table.InsnAddr;
    const auto *Section = I->Image.getSectionFor(Table.BaseAddr);
    ASSERT_NE(Section, nullptr);
    const NdVar Pointer =
        NdVar::reg(getTargetRegInfo(I->Image.Arch).IntReturnReg, 8);
    LowOp Kill;
    Kill.Opcode = NdOp::COPY;
    Kill.Output = Pointer;
    Kill.addInput(NdVar::cst(0, 8));
    LowOp Source = Kill;
    Source.Inputs[0] = NdVar::dataAddress(Table.BaseAddr, 8, Section->VA);
    LowOp Store;
    Store.Opcode = NdOp::STORE;
    Store.addInput(Pointer);
    Store.addInput(NdVar::cst(0, 8));

    // This small LowIR cycle isolates the solver: the first visit has no
    // table pointer. The backedge supplies the real fixture's table address;
    // consuming and then killing it leaves exactly the same outgoing state.
    LowFunc Loop = ordinaryPadding(0);
    Loop.Blocks.resize(3);
    Loop.ModuleAnalysisRoots = {Loop.Entry};
    for (size_t Index = 0; Index < Loop.Blocks.size(); ++Index) {
      auto &Block = Loop.Blocks[Index];
      Block.Id = Index;
      Block.StartAddr = Loop.Entry + Index * 16;
      Block.EndAddr = Block.StartAddr + 16;
    }
    Loop.Blocks[0].Succs = {2};
    Loop.Blocks[1].Preds = {2};
    Loop.Blocks[1].Succs = {2};
    Loop.Blocks[1].Ops = {Source};
    Loop.Blocks[2].Preds = {0, 1};
    Loop.Blocks[2].Succs = {1};
    Loop.Blocks[2].Ops = KillBeforeStore ? std::vector<LowOp>{Kill, Store}
                                         : std::vector<LowOp>{Store, Kill};
    for (auto &Block : Loop.Blocks)
      for (size_t Index = 0; Index < Block.Ops.size(); ++Index) {
        Block.Ops[Index].Addr = Block.StartAddr + Index;
        Block.Ops[Index].Seq = 0;
      }
    I->Functions.push_back(std::move(Loop));
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions);
    ASSERT_TRUE(Result.AnalysisComplete);
    EXPECT_EQ(Result.UnsafeBranches.count(Branch), KillBeforeStore ? 0u : 1u);
  }
}

TEST_F(ModuleJumpTableBudget, FunctionsShareTheTransferAccount) {
  auto I = fixture();
  ASSERT_TRUE(I);
  std::vector<LowFunc> Loops;
  for (va_t Entry : {va_t{0x70000000}, va_t{0x71000000}}) {
    LowFunc Loop = ordinaryPadding(180000);
    Loop.Entry = Entry;
    Loop.Blocks.front().StartAddr = Entry;
    Loop.Blocks.front().EndAddr = Entry + 180000;
    Loop.Blocks.front().Preds = {0};
    Loop.Blocks.front().Succs = {0};
    I->Functions.push_back(Loop);
    const auto Alone = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions, FixedAllowance);
    EXPECT_TRUE(Alone.AnalysisComplete);
    I->Functions.pop_back();
    Loops.push_back(std::move(Loop));
  }
  for (LowFunc &Loop : Loops)
    I->Functions.push_back(std::move(Loop));
  expectAbandoned(*I, FixedAllowance);
  std::reverse(I->Functions.begin(), I->Functions.end());
  expectAbandoned(*I, FixedAllowance);
}

TEST_F(ModuleJumpTableBudget, RepeatedCFGWorkGrowsWithTheCodeRead) {
  // A chain of one-operation blocks needs two visits a block, more than the
  // fixed allowance in all, and far fewer than its allowance per operation.
  auto I = fixture();
  ASSERT_TRUE(I);
  const auto Baseline = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Baseline.AnalysisComplete);
  LowFunc Chain = ordinaryPadding(1);
  Chain.Blocks.clear();
  for (size_t Index = 0; Index < FixedAllowance / 2 + 1; ++Index) {
    LowBlock B;
    B.Id = static_cast<int>(Index);
    B.StartAddr = Chain.Entry + Index;
    B.EndAddr = B.StartAddr + 1;
    B.Ops.resize(1);
    B.Ops.front().Opcode = NdOp::NOP;
    if (Index)
      B.Preds.push_back(B.Id - 1);
    B.Succs.push_back(B.Id + 1);
    Chain.Blocks.push_back(std::move(B));
  }
  Chain.Blocks.back().Succs.clear();
  I->Functions.push_back(std::move(Chain));
  expectAbandoned(*I, FixedAllowance);
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  EXPECT_EQ(Result.ProtectedRelocationSlots, Baseline.ProtectedRelocationSlots);
  EXPECT_EQ(Result.UnsafeBranches, Baseline.UnsafeBranches);
}

TEST_F(ModuleJumpTableBudget, WorkBeyondTheAllowancePerOperationIsAbandoned) {
  // Each frame walk is one operation that takes over a hundred visits to
  // widen.  A few fit the fixed allowance; a thousand need more than both it
  // and their sixteen visits per operation.
  for (size_t Walks : {size_t{100}, size_t{1000}}) {
    SCOPED_TRACE(Walks);
    auto I = fixture();
    ASSERT_TRUE(I);
    for (size_t Index = 0; Index < Walks; ++Index)
      I->Functions.push_back(frameWalk(0x70000000 + Index * 0x100, I->Image));
    ASSERT_LT(opCount(I->Functions) *
                  ::neverd::limits::pipeline::kPipelineLowIRAddressVisitsPerOp,
              FixedAllowance);
    if (Walks == 100)
      EXPECT_TRUE(pipeline_detail::arbitrateModuleJumpTablesForTesting(
                      I->Image, I->Functions)
                      .AnalysisComplete);
    else
      expectAbandoned(*I);
  }
}

TEST_F(ModuleJumpTableBudget, ParallelSolvesChargeTheirWorkInFunctionOrder) {
  // Arbitration solves its functions in parallel, each with every account
  // whole, and charges them in function order afterwards.  On any number of
  // threads it completes at the same least budget, fails one below it, and
  // keeps the same answer.
  auto I = fixture();
  ASSERT_TRUE(I);
  for (size_t Index = 0; Index < 2 * limits::kMinParallelIRWorkItems; ++Index)
    I->Functions.push_back(frameWalk(0x70000000 + Index * 0x100, I->Image));
  struct ThreadCount {
    explicit ThreadCount(unsigned N) { setWorkerThreadCount(N); }
    ~ThreadCount() { setWorkerThreadCount(0); }
  };
  auto completes = [&](size_t Budget) {
    return pipeline_detail::arbitrateModuleJumpTablesForTesting(
               I->Image, I->Functions, Budget)
        .AnalysisComplete;
  };
  size_t Least = 0;
  {
    ThreadCount One(1);
    Least = leastCompletingBudget(*I);
  }
  ASSERT_GT(Least, size_t{4096});
  std::optional<pipeline_detail::ModuleJumpTableArbitrationTestResult> First;
  for (unsigned Threads : {1u, 4u, 16u}) {
    SCOPED_TRACE(Threads);
    ThreadCount Count(Threads);
    EXPECT_FALSE(completes(Least - 1));
    const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
        I->Image, I->Functions, Least);
    ASSERT_TRUE(Result.AnalysisComplete);
    if (!First) {
      First = Result;
      continue;
    }
    EXPECT_EQ(Result.ProtectedRelocationSlots, First->ProtectedRelocationSlots);
    EXPECT_EQ(Result.UnsafeBranches, First->UnsafeBranches);
  }
}

// A relative table inside its function's code: no relocation slot to suppress,
// and the bytes are read-only unless the section says otherwise.
std::string relativeDispatch(const char *SectionDirective) {
  return std::string("        ") + SectionDirective + "\n" + R"(
        .globl  module_relative_dispatch
        .type   module_relative_dispatch,@function
module_relative_dispatch:
        cmpl    $3, %edi
        ja      .Lmodule_relative_default
        movl    %edi, %edi
        leaq    .Lmodule_relative_table(%rip), %rdx
        movslq  (%rdx,%rdi,4), %rax
        addq    %rdx, %rax
        jmpq    *%rax
.Lmodule_relative_case0:
        movl    $7100, %eax
        retq
.Lmodule_relative_case1:
        movl    $7101, %eax
        retq
.Lmodule_relative_case2:
        movl    $7102, %eax
        retq
.Lmodule_relative_case3:
        movl    $7103, %eax
        retq
.Lmodule_relative_default:
        xorl    %eax, %eax
        retq
        .p2align 2
.Lmodule_relative_table:
        .long   .Lmodule_relative_case0-.Lmodule_relative_table
        .long   .Lmodule_relative_case1-.Lmodule_relative_table
        .long   .Lmodule_relative_case2-.Lmodule_relative_table
        .long   .Lmodule_relative_case3-.Lmodule_relative_table
        .size   module_relative_dispatch, .-module_relative_dispatch
)";
}

TEST_F(ModuleJumpTableBudget, NothingToDecideNeedsNoModuleInventory) {
  auto I = assembled(relativeDispatch(".text"), {"module_relative_dispatch"});
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  ASSERT_TRUE(requested(I->Functions).empty());
  // A budget of zero cannot pay for the owner list, so any inventory abandons.
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions, 0);
  EXPECT_TRUE(Result.AnalysisComplete);
  EXPECT_TRUE(Result.ProtectedRelocationSlots.empty());
  EXPECT_TRUE(Result.UnsafeBranches.empty());
}

TEST_F(ModuleJumpTableBudget, AWritableOwnerStillTakesTheInventory) {
  auto I = assembled(
      relativeDispatch(
          ".section .text.module_relative_writable,\"awx\",@progbits"),
      {"module_relative_dispatch"});
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  ASSERT_TRUE(requested(I->Functions).empty());
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions, 0);
  EXPECT_FALSE(Result.AnalysisComplete);
  EXPECT_EQ(Result.UnsafeBranches,
            std::set<va_t>{I->Functions.front().JumpTables.front().InsnAddr});
}

TEST_F(ModuleJumpTableBudget, AFrameStoreAfterAJoinLetsTheSolveSettle) {
  // The shape of rustc_demangle's hex-nibble iterator.  The join is first read
  // before its other predecessors, which marks the frame base imprecise, so
  // the frame address stored after it escapes; once every predecessor is read
  // the base is exact and nothing escapes.  The loop that follows used to
  // carry both states around until the fixed-point work ran out.
  auto I = fixture(0, false, R"(
        .text
        .globl  module_owner_frame_store_loop
        .type   module_owner_frame_store_loop,@function
module_owner_frame_store_loop:
        movq    %rsp, %rbp
        jb      .Lmodule_owner_frame_store
        jb      .Lmodule_owner_frame_store
        cmpb    $0xf8, %r9b
.Lmodule_owner_frame_store:
        leaq    -0x14(%rbp), %r9
        movq    %r9, -0x28(%rbp)
.Lmodule_owner_frame_store_loop:
        ja      .Lmodule_owner_frame_store_done
        jae     .Lmodule_owner_frame_store_done
        jne     .Lmodule_owner_frame_store_loop
.Lmodule_owner_frame_store_done:
        retq
        .size   module_owner_frame_store_loop, .-module_owner_frame_store_loop
)");
  ASSERT_TRUE(I);
  const auto Without = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Without.AnalysisComplete);
  ASSERT_TRUE(appendFunction(*I, "module_owner_frame_store_loop"));
  const auto With = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  EXPECT_TRUE(With.AnalysisComplete);
  EXPECT_EQ(With.ProtectedRelocationSlots, Without.ProtectedRelocationSlots);
  EXPECT_EQ(With.UnsafeBranches, Without.UnsafeBranches);
}

TEST_F(ModuleJumpTableBudget, MixedWidthStackArgumentsKeepTheirEscape) {
  // Two paths store a scalar and the safe table's address at the same
  // outgoing stack slot, at different widths.  The callee may read either,
  // so the address escapes and the writable table becomes unsafe.
  auto I = fixture(0, false, R"(
        .text
        .globl  module_owner_mixed_width_argument
        .type   module_owner_mixed_width_argument,@function
module_owner_mixed_width_argument:
        subq    $24, %rsp
        testl   %edi, %edi
        je      .Lmodule_owner_mixed_table
        movl    $1, (%rsp)
        jmp     .Lmodule_owner_mixed_call
.Lmodule_owner_mixed_table:
        leaq    .Lmodule_owner_safe_table(%rip), %rax
        movq    %rax, (%rsp)
.Lmodule_owner_mixed_call:
        callq   module_owner_unrelated_pointer_induction
        addq    $24, %rsp
        retq
        .size   module_owner_mixed_width_argument, .-module_owner_mixed_width_argument
)");
  ASSERT_TRUE(I);
  ASSERT_EQ(I->Functions.front().JumpTables.size(), 1u);
  const va_t SafeBranch = I->Functions.front().JumpTables.front().InsnAddr;
  const std::vector<va_t> SafeSlots =
      I->Functions.front().JumpTables.front().SuppressibleRelocationSlots;
  ASSERT_FALSE(SafeSlots.empty());
  const auto Without = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Without.AnalysisComplete);
  ASSERT_EQ(Without.UnsafeBranches.count(SafeBranch), 0u);
  auto Protected = Without.ProtectedRelocationSlots;
  Protected.insert(SafeSlots.begin(), SafeSlots.end());
  auto Unsafe = Without.UnsafeBranches;
  Unsafe.insert(SafeBranch);
  ASSERT_TRUE(appendFunction(*I, "module_owner_mixed_width_argument"));
  const auto With = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  EXPECT_TRUE(With.AnalysisComplete);
  EXPECT_EQ(With.ProtectedRelocationSlots, Protected);
  EXPECT_EQ(With.UnsafeBranches, Unsafe);
}

TEST_F(ModuleJumpTableBudget, AnImpreciseFrameWalkWidensAtOnce) {
  // Nothing reads the frame offsets of an imprecise value that may lie
  // outside the frame, so a loop walking one widens it at the first join
  // rather than gaining an offset a round: each walk adds a few visits to the
  // least budget the module completes with, not one per offset.
  auto I = fixture();
  ASSERT_TRUE(I);
  const auto Baseline = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Baseline.AnalysisComplete);
  const size_t Alone = leastCompletingBudget(*I);
  ASSERT_NE(Alone, SIZE_MAX);
  constexpr size_t Walks = 64;
  for (size_t Index = 0; Index < Walks; ++Index)
    I->Functions.push_back(
        impreciseFrameWalk(0x70000000 + Index * 0x100, I->Image));
  EXPECT_LE(leastCompletingBudget(*I), Alone + Walks * 16);
  const auto Result = pipeline_detail::arbitrateModuleJumpTablesForTesting(
      I->Image, I->Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  EXPECT_EQ(Result.ProtectedRelocationSlots, Baseline.ProtectedRelocationSlots);
  EXPECT_EQ(Result.UnsafeBranches, Baseline.UnsafeBranches);
}

} // namespace
