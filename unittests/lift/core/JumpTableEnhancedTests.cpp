//===- JumpTableEnhancedTests.cpp - Enhanced jump-table tests -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Cross-architecture tests validating jump-table resolution enhancements:
///   - alignment-aware sanity checks (ARM/AArch64 vs x86)
///   - multi-stage recovery
///   - nested switch recovery
///   - offset-base normalization
///   - dense/large table handling
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"
#include "PipelineLowIRDetail.h"
#include "../../../lib/ir/low/jumptable/JumpTableResolverDetail.h"

#include "neverd/Limits.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstring>
#include <functional>
#include <optional>
#include <set>

static std::string llvmFunctionBody(const std::string &IR,
                                    const std::string &Name) {
  std::string::size_type Begin = IR.find("@" + Name + "(");
  if (Begin == std::string::npos)
    return {};
  Begin = IR.rfind("define ", Begin);
  if (Begin == std::string::npos)
    return {};
  std::string::size_type End = IR.find("\n}", Begin);
  return End == std::string::npos ? IR.substr(Begin)
                                  : IR.substr(Begin, End + 2 - Begin);
}

struct DirectPipelineRun {
  std::unique_ptr<llvm::LLVMContext> Context;
  neverd::PipelineResult Result;
  std::string LLVMIR;
};

static DirectPipelineRun
runPipelineWithEvidenceBudget(const neverd::BinaryImage &Image, size_t Budget) {
  DirectPipelineRun Run;
  Run.Context = std::make_unique<llvm::LLVMContext>();
  neverd::PipelineOptions Options;
  Options.LiftMode = true;
  Options.NoOpt = true;
  Options.EmitDumpOutput = false;
  Options.JumpTableEvidenceBudgetForTesting = Budget;
  neverd::Pipeline Pipeline;
  Run.Result = Pipeline.run(Image, *Run.Context, Options);
  if (Run.Result.LlvmModule) {
    llvm::raw_string_ostream OS(Run.LLVMIR);
    Run.Result.LlvmModule->print(OS, nullptr);
  }
  return Run;
}

static const neverd::LowFunc *
findLowFunction(const neverd::PipelineResult &Result, const std::string &Name) {
  auto It = std::find_if(
      Result.LowFuncs.begin(), Result.LowFuncs.end(),
      [&](const neverd::LowFunc &Function) { return Function.Name == Name; });
  return It == Result.LowFuncs.end() ? nullptr : &*It;
}

static bool lowFunctionHasOpcode(const neverd::LowFunc &Function,
                                 neverd::NdOp Opcode) {
  for (const neverd::LowBlock &Block : Function.Blocks)
    if (std::any_of(
            Block.Ops.begin(), Block.Ops.end(),
            [&](const neverd::LowOp &Op) { return Op.Opcode == Opcode; }))
      return true;
  return false;
}

static const neverd::LowOp *
findExactLowOp(const neverd::LowFunc &Function, neverd::va_t Address,
               int Sequence, neverd::NdOp Opcode) {
  const neverd::LowOp *Match = nullptr;
  for (const neverd::LowBlock &Block : Function.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Addr == Address && Op.Seq == Sequence &&
          Op.Opcode == Opcode) {
        if (Match)
          return nullptr;
        Match = &Op;
      }
  return Match;
}

static std::optional<std::string>
llvmSwitchConditionToken(const std::string &Body) {
  const std::string::size_type Switch = Body.find("switch i");
  if (Switch == std::string::npos)
    return std::nullopt;
  const std::string::size_type Percent = Body.find('%', Switch);
  if (Percent == std::string::npos)
    return std::nullopt;
  std::string::size_type End = Percent + 1;
  while (End < Body.size() &&
         (std::isalnum(static_cast<unsigned char>(Body[End])) ||
          Body[End] == '_' || Body[End] == '.'))
    ++End;
  return Body.substr(Percent, End - Percent);
}

static bool llvmHasSwitchCase(const std::string &Body, uint64_t Label) {
  const std::string Suffix = " " + std::to_string(Label) + ", label";
  for (const char *Width : {"i8", "i16", "i32", "i64"})
    if (Body.find(std::string(Width) + Suffix) != std::string::npos)
      return true;
  return false;
}

static bool llvmSSAValueDependsOn(const std::string &Body,
                                  const std::string &Value,
                                  const std::string &Needle) {
  std::set<std::string> Active;
  std::function<bool(const std::string &)> Visit =
      [&](const std::string &Current) -> bool {
    if (Current == Needle)
      return true;
    if (Current.empty() || Current[0] != '%' || !Active.insert(Current).second)
      return false;

    const std::string DefinitionNeedle = "\n  " + Current + " = ";
    const std::string::size_type Definition = Body.find(DefinitionNeedle);
    if (Definition == std::string::npos) {
      Active.erase(Current);
      return false;
    }
    const std::string::size_type LineEnd = Body.find('\n', Definition + 1);
    const std::string Line = Body.substr(
        Definition, LineEnd == std::string::npos ? std::string::npos
                                                 : LineEnd - Definition);
    for (std::string::size_type Pos = Line.find('%'); Pos != std::string::npos;
         Pos = Line.find('%', Pos + 1)) {
      std::string::size_type End = Pos + 1;
      while (End < Line.size() &&
             (std::isalnum(static_cast<unsigned char>(Line[End])) ||
              Line[End] == '_' || Line[End] == '.'))
        ++End;
      if (Visit(Line.substr(Pos, End - Pos))) {
        Active.erase(Current);
        return true;
      }
      Pos = End - 1;
    }
    Active.erase(Current);
    return false;
  };
  return Visit(Value);
}

static bool llvmSSAValueDependencyContains(const std::string &Body,
                                           const std::string &Value,
                                           const std::string &First,
                                           const std::string &Second) {
  std::set<std::string> Seen;
  std::function<bool(const std::string &)> Visit =
      [&](const std::string &Current) -> bool {
    if (Current.empty() || Current[0] != '%' || !Seen.insert(Current).second)
      return false;
    const std::string DefinitionNeedle = "\n  " + Current + " = ";
    const std::string::size_type Definition = Body.find(DefinitionNeedle);
    if (Definition == std::string::npos)
      return false;
    const std::string::size_type LineEnd = Body.find('\n', Definition + 1);
    const std::string Line = Body.substr(
        Definition, LineEnd == std::string::npos ? std::string::npos
                                                 : LineEnd - Definition);
    if (Line.find(First) != std::string::npos &&
        Line.find(Second) != std::string::npos)
      return true;
    for (std::string::size_type Pos = Line.find('%'); Pos != std::string::npos;
         Pos = Line.find('%', Pos + 1)) {
      std::string::size_type End = Pos + 1;
      while (End < Line.size() &&
             (std::isalnum(static_cast<unsigned char>(Line[End])) ||
              Line[End] == '_' || Line[End] == '.'))
        ++End;
      if (Visit(Line.substr(Pos, End - Pos)))
        return true;
      Pos = End - 1;
    }
    return false;
  };
  return Visit(Value);
}

TEST(JumpTableHighSelector, MissingExactSelectorRemainsAnUnresolvedGoto) {
  neverd::MedFunc Med;
  Med.Entry = 0x100;
  Med.Name = "missing_exact_jump_table_selector";

  neverd::MedVar Target;
  Target.Kind = neverd::MedVar::Reg;
  Target.TheArch = neverd::Arch::X64;
  Target.Id = 1;
  Target.SSAVer = 1;
  Target.RegOff = 0;
  Target.Size = 8;

  neverd::MedBlock Dispatch;
  Dispatch.Id = 0;
  Dispatch.StartAddr = 0x100;
  Dispatch.EndAddr = 0x101;
  Dispatch.Succs = {1, 2};
  neverd::MedOp Branch;
  Branch.Opcode = neverd::NdOp::INDIR_BR;
  Branch.Addr = 0x100;
  Branch.addInput(Target);
  Dispatch.Ops.push_back(Branch);

  auto makeReturnBlock = [](int Id, neverd::va_t Addr, uint64_t Value) {
    neverd::MedBlock Block;
    Block.Id = Id;
    Block.StartAddr = Addr;
    Block.EndAddr = Addr + 1;
    Block.Preds = {0};
    neverd::MedOp Ret;
    Ret.Opcode = neverd::NdOp::RETURN;
    Ret.Addr = Addr;
    Ret.addInput(neverd::MedVar::makeConst(Value, 4));
    Block.Ops.push_back(Ret);
    return Block;
  };
  Med.Blocks = {std::move(Dispatch), makeReturnBlock(1, 0x200, 1),
                makeReturnBlock(2, 0x300, 2)};

  neverd::JumpTable JT;
  JT.InsnAddr = 0x100;
  JT.Targets = {0x200, 0x300};
  neverd::MedToHighConverter Converter;
  Converter.setJumpTables({JT});
  const neverd::HighFunc High = Converter.convert(Med, neverd::Arch::X64);

  EXPECT_TRUE(std::none_of(High.Body.begin(), High.Body.end(),
                           [](const neverd::HighStmt &Stmt) {
                             return Stmt.Kind == neverd::StmtKind::Switch;
                           }));
  EXPECT_TRUE(std::any_of(High.Body.begin(), High.Body.end(),
                          [](const neverd::HighStmt &Stmt) {
                            return Stmt.Kind == neverd::StmtKind::Goto &&
                                   Stmt.GotoTarget == neverd::InvalidVA;
                          }));
}

//===----------------------------------------------------------------------===//
// i386
//===----------------------------------------------------------------------===//

class JTE_X86_32 : public NeverDLiftTest {};

static fs::path frameLaneX86Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_frame_lane32.o";
}

static fs::path i386GOTPCModelObj() {
  return fs::path(TEST_OBJ_DIR) / "test_i386_gotpc_model.o";
}

static fs::path i386RelocationWriterFootprintObj() {
  return fs::path(TEST_OBJ_DIR) / "test_i386_relocation_writer_footprint.o";
}

TEST_F(JTE_X86_32, GOTPCModelRequiresLifterAuthenticatedCallPopSeed) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotpc_call_pop_seed");
  ASSERT_NE(Function, nullptr);
  size_t FunctionGOTPCFields = 0;
  for (const auto &[FieldVA, Field] : Image.I386GOTPCFields) {
    (void)Field;
    if (FieldVA >= Function->Addr && FieldVA < Function->Addr + Function->Size)
      ++FunctionGOTPCFields;
  }
  ASSERT_EQ(FunctionGOTPCFields, 1u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "jt_i386_gotpc_call_pop_seed");

  ASSERT_EQ(Low.I386GetPcOccurrences.size(), 1u);
  ASSERT_EQ(Low.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  const auto &GetPc = Low.I386GetPcOccurrences.front();
  const auto &Model = Low.RelocatedInstructionScalarModelOccurrences.front();
  EXPECT_EQ(GetPc.PCValue, 5u);
  EXPECT_EQ(GetPc.CallInstructionAddr, 0u);
  EXPECT_EQ(GetPc.InstructionAddr, 5u);
  EXPECT_EQ(GetPc.OutputOpcode, neverd::NdOp::COPY);
  const neverd::LowOp *SeedCopy =
      findExactLowOp(Low, GetPc.InstructionAddr, GetPc.OpSeq,
                     GetPc.OutputOpcode);
  ASSERT_NE(SeedCopy, nullptr);
  ASSERT_EQ(SeedCopy->NumInputs, 1u);
  EXPECT_EQ(GetPc.InputWitness, SeedCopy->Inputs[0]);
  EXPECT_FALSE(GetPc.RawPCAuthenticated)
      << "ELF must publish only the paired GOTPC scalar model";
  EXPECT_TRUE(SeedCopy->Inputs[0].isTemp())
      << "the occurrence must name the real pop LOAD/COPY, not a synthetic "
         "constant overwrite";
  const neverd::LowOp *SeedLoad = nullptr;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Addr == GetPc.InstructionAddr && Op.Opcode == neverd::NdOp::LOAD &&
          Op.Output == SeedCopy->Inputs[0]) {
        ASSERT_EQ(SeedLoad, nullptr);
        SeedLoad = &Op;
      }
  ASSERT_NE(SeedLoad, nullptr);
  ASSERT_EQ(SeedLoad->NumInputs, 1u);
  EXPECT_TRUE(SeedLoad->Inputs[0].isReg());
  EXPECT_EQ(Model.Model, neverd::RelocatedInstructionScalarModelOccurrence::
                             ModelKind::I386ELFGOTBaseZero);
  EXPECT_EQ(Model.SeedInstructionAddr, GetPc.InstructionAddr);
  EXPECT_EQ(Model.SeedOpSeq, GetPc.OpSeq);
  EXPECT_EQ(Model.SeedOpcode, GetPc.OutputOpcode);
  EXPECT_EQ(Model.SeedOutputWitness, GetPc.OutputWitness);
}

TEST_F(JTE_X86_32, GOTPCModelRejectsRelocationFreeAbsoluteAddressSeed) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotpc_absolute_seed");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "jt_i386_gotpc_absolute_seed");

  EXPECT_TRUE(Low.I386GetPcOccurrences.empty());
  EXPECT_TRUE(Low.RelocatedInstructionScalarModelOccurrences.empty())
      << "a role-neutral Address constant with the expected numeric value "
         "must not authenticate the relocatable GOTPC model";
}

TEST_F(JTE_X86_32, GOTPCModelRejectsAddressTakenPopEntry) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotpc_rooted_pop");
  const neverd::Symbol *Pointer =
      Image.findSymbol("jt_i386_gotpc_rooted_pop_pointer");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Pointer, nullptr);
  ASSERT_TRUE(Image.CodePtrRelocSlots.count(Pointer->Addr));

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(
      Image, Decoder, Function->Addr, "jt_i386_gotpc_rooted_pop");

  ASSERT_EQ(Low.I386GetPcOccurrences.size(), 1u);
  const auto &GetPc = Low.I386GetPcOccurrences.front();
  const neverd::LowOp *SeedCopy =
      findExactLowOp(Low, GetPc.InstructionAddr, GetPc.OpSeq,
                     GetPc.OutputOpcode);
  ASSERT_NE(SeedCopy, nullptr);
  ASSERT_EQ(SeedCopy->NumInputs, 1u);
  EXPECT_EQ(GetPc.InputWitness, SeedCopy->Inputs[0]);
  EXPECT_FALSE(GetPc.RawPCAuthenticated);
  EXPECT_TRUE(SeedCopy->Inputs[0].isTemp())
      << "an independently reachable pop must retain its actual stack value";
  EXPECT_TRUE(Low.RelocatedInstructionScalarModelOccurrences.empty())
      << "an address-taken pop has an independent live-in and cannot inherit "
         "the adjacent call's pushed PC";
}

TEST_F(JTE_X86_32, GOTOFFSwitchRequiresExactCallPopAndDataFieldOccurrences) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Positive =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop");
  const neverd::Symbol *Negative =
      Image.findSymbol("jt_i386_gotoff_switch_absolute_seed");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_i386_gotoff_switch_table");
  ASSERT_NE(Positive, nullptr);
  ASSERT_NE(Negative, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 4u);

  auto Recover = [&](const neverd::Symbol &Function) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function.Addr, Function.Name);
  };

  const neverd::LowFunc Authenticated = Recover(*Positive);
  ASSERT_EQ(Authenticated.JumpTables.size(), 1u);
  EXPECT_EQ(Authenticated.JumpTables.front().Targets.size(), 5u);

  const neverd::LowFunc Unauthenticated = Recover(*Negative);
  EXPECT_TRUE(Unauthenticated.JumpTables.empty())
      << "a numerically equal absolute seed is not an exact GOTPC producer";
  EXPECT_TRUE(lowFunctionHasOpcode(Unauthenticated, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(lowFunctionHasOpcode(Unauthenticated, neverd::NdOp::INDIR_BR))
      << "missing model provenance alone must not be mistaken for a scoped "
         "multiply-owned-field tombstone";
  EXPECT_TRUE(Unauthenticated.UnsafeIndirectBranchAddresses.empty());

  std::vector<neverd::va_t> MatchingFields;
  for (const auto &[FieldVA, Field] : Image.DataAddressRelocOperands)
    if (FieldVA >= Positive->Addr &&
        FieldVA < Positive->Addr + Positive->Size &&
        Field.Width == Image.getPointerSize() &&
        Field.TargetVA == Table->Addr)
      MatchingFields.push_back(FieldVA);
  ASSERT_EQ(MatchingFields.size(), 1u);
  const neverd::va_t FieldVA = MatchingFields.front();
  const neverd::RelocatedAddressField SavedField =
      Image.DataAddressRelocOperands.at(FieldVA);

  Image.DataAddressRelocOperands.erase(FieldVA);
  EXPECT_TRUE(Recover(*Positive).JumpTables.empty())
      << "the numeric GOTOFF displacement cannot replace its exact field";

  Image.DataAddressRelocOperands.emplace(FieldVA, SavedField);
  Image.DataAddressRelocOperands.at(FieldVA).TargetOwnerVA = neverd::InvalidVA;
  EXPECT_TRUE(Recover(*Positive).JumpTables.empty())
      << "a GOTOFF field without an authenticated target owner must fail closed";

  Image.DataAddressRelocOperands.at(FieldVA) = SavedField;
  ASSERT_EQ(SavedField.Kind,
            neverd::RelocatedAddressFieldKind::I386ELFGOTOFF);
  Image.DataAddressRelocOperands.at(FieldVA).Kind =
      neverd::RelocatedAddressFieldKind::Generic;
  EXPECT_TRUE(Recover(*Positive).JumpTables.empty())
      << "an absolute relocation with the same folded value and owner must "
         "not borrow GOTOFF semantics";
}

TEST_F(JTE_X86_32, TLSDescWriterFootprintInvalidatesOnlyOverlappingGOTPCField) {
  auto ImageOrErr = neverd::loadBinary(i386RelocationWriterFootprintObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Overlap =
      Image.findSymbol("jt_i386_gotoff_tls_desc_overlap");
  const neverd::Symbol *OverlapRecord =
      Image.findSymbol("jt_i386_gotoff_tls_desc_overlap_record");
  const neverd::Symbol *OverlapField =
      Image.findSymbol("jt_i386_gotoff_tls_desc_overlap_field");
  const neverd::Symbol *OverlapBranch =
      Image.findSymbol("jt_i386_gotoff_tls_desc_overlap_branch");
  const neverd::Symbol *NonOverlap =
      Image.findSymbol("jt_i386_gotoff_tls_desc_nonoverlap");
  const neverd::Symbol *NonOverlapRecord =
      Image.findSymbol("jt_i386_gotoff_tls_desc_nonoverlap_record");
  const neverd::Symbol *NonOverlapField =
      Image.findSymbol("jt_i386_gotoff_tls_desc_nonoverlap_field");
  const neverd::Symbol *NonOverlapBranch =
      Image.findSymbol("jt_i386_gotoff_tls_desc_nonoverlap_branch");
  ASSERT_NE(Overlap, nullptr);
  ASSERT_NE(OverlapRecord, nullptr);
  ASSERT_NE(OverlapField, nullptr);
  ASSERT_NE(OverlapBranch, nullptr);
  ASSERT_NE(NonOverlap, nullptr);
  ASSERT_NE(NonOverlapRecord, nullptr);
  ASSERT_NE(NonOverlapField, nullptr);
  ASSERT_NE(NonOverlapBranch, nullptr);
  ASSERT_EQ(OverlapRecord->Addr + 4, OverlapField->Addr)
      << "R_386_TLS_DESC writes the four-byte word at r_offset+4";
  ASSERT_LE(NonOverlapRecord->Addr + 8, NonOverlapField->Addr)
      << "the positive control's biased writer footprint must stay disjoint";

  EXPECT_EQ(Image.AmbiguousI386GOTPCFields.count(OverlapField->Addr), 1u);
  EXPECT_EQ(Image.I386GOTPCFields.count(OverlapField->Addr), 0u)
      << "a biased overlapping writer must suppress exact GOTPC provenance";
  EXPECT_EQ(Image.AmbiguousI386GOTPCFields.count(NonOverlapField->Addr), 0u);
  EXPECT_EQ(Image.I386GOTPCFields.count(NonOverlapField->Addr), 1u)
      << "a disjoint TLS descriptor relocation cannot taint the GOTPC field";

  auto Recover = [&](const neverd::Symbol &Function) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function.Addr, Function.Name);
  };
  auto HasOpcodeAt = [](const neverd::LowFunc &Low, neverd::va_t Addr,
                        neverd::NdOp Opcode) {
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Addr && Op.Opcode == Opcode)
          return true;
    return false;
  };

  const neverd::LowFunc Ambiguous = Recover(*Overlap);
  EXPECT_TRUE(Ambiguous.JumpTables.empty());
  EXPECT_TRUE(
      HasOpcodeAt(Ambiguous, OverlapBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(
      HasOpcodeAt(Ambiguous, OverlapBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Ambiguous.UnsafeIndirectBranchAddresses.count(OverlapBranch->Addr),
            1u);
  EXPECT_EQ(Ambiguous.EverPublishedJumpTableBranchAddresses.count(
                OverlapBranch->Addr),
            0u);

  const neverd::LowFunc Exact = Recover(*NonOverlap);
  ASSERT_EQ(Exact.JumpTables.size(), 1u);
  EXPECT_EQ(Exact.JumpTables.front().Targets.size(), 2u);
  EXPECT_TRUE(
      HasOpcodeAt(Exact, NonOverlapBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(
      HasOpcodeAt(Exact, NonOverlapBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Exact.UnsafeIndirectBranchAddresses.count(NonOverlapBranch->Addr),
            0u);
}

TEST_F(JTE_X86_32,
       SameI386FieldWithMultipleValueRelocationsStaysOpaque) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_same_field_conflict");
  const neverd::Symbol *PC =
      Image.findSymbol("jt_i386_gotoff_same_field_conflict_pc");
  const neverd::Symbol *Field =
      Image.findSymbol("jt_i386_gotoff_same_field_conflict_field");
  const neverd::Symbol *Bias =
      Image.findSymbol("jt_i386_gotpc_conflict_bias");
  const neverd::Symbol *TextBase =
      Image.findSymbol("jt_i386_gotpc_text_base");
  const neverd::Symbol *AmbiguousBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_ambiguous_branch");
  const neverd::Symbol *CallbackBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_callback_branch");
  const neverd::Symbol *ValidBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_valid_branch");
  const neverd::Symbol *ZeroBase = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_zero_base");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(PC, nullptr);
  ASSERT_NE(Field, nullptr);
  ASSERT_NE(Bias, nullptr);
  ASSERT_NE(TextBase, nullptr);
  ASSERT_NE(AmbiguousBranch, nullptr);
  ASSERT_NE(CallbackBranch, nullptr);
  ASSERT_NE(ValidBranch, nullptr);
  ASSERT_NE(ZeroBase, nullptr);
  ASSERT_NE(Bias->Addr, TextBase->Addr)
      << "the nonzero symbol offset distinguishes original-addend semantics "
         "from sequential relocation write-back";
  EXPECT_EQ(Image.AmbiguousI386GOTPCFields.count(Field->Addr), 1u);
  EXPECT_EQ(Image.I386GOTPCFields.count(Field->Addr), 0u)
      << "a multiply-owned field is a negative certificate, never an "
         "authenticated GOTPC model";
  EXPECT_EQ(Image.CodeAddressRelocOperands.count(Field->Addr), 0u);
  EXPECT_EQ(Image.DataAddressRelocOperands.count(Field->Addr), 0u);
  EXPECT_EQ(Image.CodePtrRelocSlots.count(Field->Addr), 0u);
  EXPECT_EQ(Image.DataPtrRelocSlots.count(Field->Addr), 0u);
  EXPECT_EQ(Image.CodeRefTargets.count(Bias->Addr), 0u)
      << "the first R_386_32 writer cannot leave exact provenance behind";

  const uint8_t *FieldBytes = Image.readVA(Field->Addr, 4);
  ASSERT_NE(FieldBytes, nullptr);
  uint32_t AppliedField = 0;
  std::memcpy(&AppliedField, FieldBytes, sizeof(AppliedField));
  // ld.lld evaluates the last R_386_GOTPC as GOT + A - P, where A is
  // the original section addend, regardless of the preceding R_386_32.
  // The loader deliberately models GOT as zero, so this is the exact linked
  // formula normalized by subtracting the linked GOT base.
  const uint32_t LLDModelZeroEncoded = static_cast<uint32_t>(
      TextBase->Addr - PC->Addr - Bias->Addr);
  EXPECT_EQ(AppliedField, LLDModelZeroEncoded)
      << "each REL must consume the original section addend; the last "
         "R_386_GOTPC write must match ld.lld under the loader's model-zero "
         "GOT convention, rather than consume the preceding R_386_32 result";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(
      Image, Decoder, Function->Addr, Function->Name);
  EXPECT_FALSE(Builder.hasPendingI386GOTPCAmbiguityForTesting())
      << "the stable graph replays the exact MayDepend query and commits the "
         "semantic ambiguity certificate without an incomplete carry";

  const neverd::LowOp *DecodedAdd = nullptr;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Addr != PC->Addr + 1 || Op.Opcode != neverd::NdOp::INT_ADD)
        continue;
      bool HasLLDImmediate = false;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        const neverd::NdVar &Input = Op.Inputs[I];
        HasLLDImmediate |= Input.isConst() && Input.Size == 4 &&
                           static_cast<uint32_t>(Input.Offset) ==
                               LLDModelZeroEncoded;
      }
      if (HasLLDImmediate)
        DecodedAdd = &Op;
    }
  EXPECT_NE(DecodedAdd, nullptr)
      << "the lifted add immediate must match the model-zero ld.lld formula";
  const bool ZeroBaseDecoded = std::any_of(
      Low.Blocks.begin(), Low.Blocks.end(), [&](const neverd::LowBlock &Block) {
        return std::any_of(Block.Ops.begin(), Block.Ops.end(),
                           [&](const neverd::LowOp &Op) {
                             return Op.Addr == ZeroBase->Addr;
                           });
      });
  EXPECT_TRUE(ZeroBaseDecoded)
      << "the fourth-argument diamond must retain the zero-base arm that "
         "distinguishes MayDepend from MustEqual";

  EXPECT_TRUE(std::none_of(
      Low.RelocatedInstructionScalarModelOccurrences.begin(),
      Low.RelocatedInstructionScalarModelOccurrences.end(),
      [&](const auto &Model) { return Model.FieldVA == Field->Addr; }))
      << "a multiply-owned REL field cannot authenticate GOT-base zero";

  auto HasOpcodeAt = [&](neverd::va_t Addr, neverd::NdOp Opcode) {
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Addr && Op.Opcode == Opcode)
          return true;
    return false;
  };
  EXPECT_TRUE(HasOpcodeAt(AmbiguousBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(AmbiguousBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(AmbiguousBranch->Addr),
            1u);
  EXPECT_EQ(Low.EverPublishedJumpTableBranchAddresses.count(
                AmbiguousBranch->Addr),
            0u)
      << "the ambiguous branch must never publish a provisional table";

  EXPECT_TRUE(HasOpcodeAt(CallbackBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(HasOpcodeAt(CallbackBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(CallbackBranch->Addr), 0u)
      << "complete ambiguity is local semantic evidence, not stage-wide "
         "incompleteness";

  const auto ValidTable = std::find_if(
      Low.JumpTables.begin(), Low.JumpTables.end(), [&](const auto &Table) {
        return Table.InsnAddr == ValidBranch->Addr;
      });
  ASSERT_NE(ValidTable, Low.JumpTables.end());
  EXPECT_EQ(ValidTable->Targets.size(), 2u);
  EXPECT_TRUE(HasOpcodeAt(ValidBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(ValidBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(ValidBranch->Addr), 0u);
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.size(), 1u);
}

TEST_F(JTE_X86_32, I386GOTPCReplayRetiresOnlyTheExactQueryKey) {
  using Key = neverd::detail::I386GOTOFFAmbiguityReplayKey;
  const neverd::va_t Branch = 0x1000;
  const Key First = std::make_tuple(Branch, 0x1010, 2, 0, 0x2000);
  const Key Second = std::make_tuple(Branch, 0x1020, 4, 1, 0x3000);
  std::set<Key> Pending{First, Second};
  std::set<Key> Replayed{Second};

  neverd::detail::retireReplayedI386GOTPCAmbiguities(Pending, Replayed);
  EXPECT_EQ(Pending.size(), 1u);
  EXPECT_EQ(Pending.count(First), 1u)
      << "a completed sibling query on the same branch cannot retire a "
         "different pending use";
  EXPECT_EQ(Pending.count(Second), 0u);

  Replayed = {First};
  neverd::detail::retireReplayedI386GOTPCAmbiguities(Pending, Replayed);
  EXPECT_TRUE(Pending.empty());

  const neverd::va_t OtherBranch = 0x4000;
  const Key SameBranchFirst =
      std::make_tuple(Branch, 0x1030, 6, 0, 0x5000);
  const Key SameBranchSecond =
      std::make_tuple(Branch, 0x1040, 8, 1, 0x6000);
  const Key Other =
      std::make_tuple(OtherBranch, 0x4010, 2, 0, 0x7000);
  Pending = {SameBranchFirst, SameBranchSecond, Other};
  Replayed.clear();
  const std::set<neverd::va_t> SafelyPublished{Branch};
  neverd::detail::retireReplayedI386GOTPCAmbiguities(
      Pending, Replayed, &SafelyPublished);
  EXPECT_EQ(Pending.size(), 1u);
  EXPECT_EQ(Pending.count(Other), 1u)
      << "a stable validated table retires every old key only for its own "
         "branch";
}

TEST_F(JTE_X86_32,
       I386GOTOFFProposalRootCacheSeparatesSiblingCandidates) {
  using Key = neverd::detail::I386GOTOFFProposalRootCacheKey;
  const neverd::va_t FirstBranch = 0x1000;
  const neverd::va_t SecondBranch = 0x1100;
  const neverd::va_t SharedTable = 0x2000;
  constexpr uint32_t SharedProofRank = 3;
  constexpr bool ConsumerAudit = true;
  const Key First = neverd::detail::makeI386GOTOFFProposalRootCacheKey(
      FirstBranch, SharedTable, SharedProofRank, ConsumerAudit);
  const Key Second = neverd::detail::makeI386GOTOFFProposalRootCacheKey(
      SecondBranch, SharedTable, SharedProofRank, ConsumerAudit);

  EXPECT_NE(First, Second)
      << "consumer-audit roots exclude the current candidate by address";

  const std::optional<std::set<neverd::va_t>> FirstRoots =
      std::set<neverd::va_t>{0x3100};
  const std::optional<std::set<neverd::va_t>> SecondRoots =
      std::set<neverd::va_t>{0x3000};
  for (bool ReverseOrder : {false, true}) {
    std::map<Key, std::optional<std::set<neverd::va_t>>> Cache;
    if (ReverseOrder) {
      Cache.emplace(Second, SecondRoots);
      Cache.emplace(First, FirstRoots);
    } else {
      Cache.emplace(First, FirstRoots);
      Cache.emplace(Second, SecondRoots);
    }
    EXPECT_EQ(Cache.size(), 2u);
    EXPECT_EQ(Cache.at(First), FirstRoots);
    EXPECT_EQ(Cache.at(Second), SecondRoots);
  }
}

TEST_F(JTE_X86_32,
       AmbiguousI386GOTPCCarrySurvivesUnreplayedStableGraph) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_ambiguous_late_shape_loss");
  const neverd::Symbol *AmbiguousBranch = Image.findSymbol(
      "jt_i386_gotoff_ambiguous_late_shape_loss_branch");
  const neverd::Symbol *ValidBranch = Image.findSymbol(
      "jt_i386_gotoff_ambiguous_late_shape_loss_valid_branch");
  const neverd::Symbol *LateEdge =
      Image.findSymbol("jt_i386_gotoff_ambiguous_late_shape_loss_edge");
  const neverd::Symbol *ValidGOTPC = Image.findSymbol(
      "jt_i386_gotoff_ambiguous_late_shape_loss_valid_gotpc_field");
  const neverd::Symbol *ValidTable =
      Image.findSymbol("jt_i386_gotoff_late_valid_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(AmbiguousBranch, nullptr);
  ASSERT_NE(ValidBranch, nullptr);
  ASSERT_NE(LateEdge, nullptr);
  ASSERT_NE(ValidGOTPC, nullptr);
  ASSERT_NE(ValidTable, nullptr);
  ASSERT_EQ(ValidTable->Size, 8u);
  EXPECT_EQ(Image.I386GOTPCFields.count(ValidGOTPC->Addr), 1u)
      << "the sibling must use a supported single-writer call/pop GOTPC "
         "model";
  for (unsigned I = 0; I < 2; ++I) {
    const neverd::va_t Slot = ValidTable->Addr + I * 4;
    EXPECT_EQ(Image.CodePtrRelocSlots.count(Slot), 0u);
    EXPECT_EQ(Image.DataPtrRelocSlots.count(Slot), 0u);
    EXPECT_EQ(Image.RelCodeRelocSlots.count(Slot), 0u);
  }
  EXPECT_FALSE(LateEdge->IsFunc);
  EXPECT_EQ(Image.CodeRefTargets.count(LateEdge->Addr), 0u)
      << "raw table entries must not preseed the late predecessor as a CFG "
         "root";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_FALSE(Builder.proposalStageCommitTailEvidenceExhaustedForTesting())
      << "pending carry must come from an unreplayed stable query, not a "
         "resource rollback";

  auto HasOpcodeAt = [&](neverd::va_t Addr, neverd::NdOp Opcode) {
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Addr && Op.Opcode == Opcode)
          return true;
    return false;
  };
  const bool LateEdgeDecoded = std::any_of(
      Low.Blocks.begin(), Low.Blocks.end(), [&](const neverd::LowBlock &Block) {
        return std::any_of(Block.Ops.begin(), Block.Ops.end(),
                           [&](const neverd::LowOp &Op) {
                             return Op.Addr == LateEdge->Addr;
                           });
      });
  EXPECT_TRUE(LateEdgeDecoded)
      << "the independently published table must decode the late predecessor";
  EXPECT_TRUE(Builder.hasPendingI386GOTPCAmbiguityForTesting())
      << "the stable graph lost the structural slice before reaching the exact "
         "negative-replay seam";
  EXPECT_TRUE(HasOpcodeAt(AmbiguousBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(AmbiguousBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(AmbiguousBranch->Addr),
            1u);
  EXPECT_EQ(Low.EverPublishedJumpTableBranchAddresses.count(
                AmbiguousBranch->Addr),
            0u);

  const auto Valid = std::find_if(
      Low.JumpTables.begin(), Low.JumpTables.end(), [&](const auto &Table) {
        return Table.InsnAddr == ValidBranch->Addr;
      });
  ASSERT_NE(Valid, Low.JumpTables.end());
  EXPECT_EQ(Valid->Targets.size(), 2u);
  EXPECT_TRUE(HasOpcodeAt(ValidBranch->Addr, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(ValidBranch->Addr, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(ValidBranch->Addr), 0u);
}

TEST_F(JTE_X86_32,
       AmbiguousI386GOTPCReplaySurvivesCommitTailExhaustion) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_same_field_conflict");
  const neverd::Symbol *AmbiguousBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_ambiguous_branch");
  const neverd::Symbol *CallbackBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_callback_branch");
  const neverd::Symbol *ValidBranch = Image.findSymbol(
      "jt_i386_gotoff_same_field_conflict_valid_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(AmbiguousBranch, nullptr);
  ASSERT_NE(CallbackBranch, nullptr);
  ASSERT_NE(ValidBranch, nullptr);

  struct BudgetedBuild {
    neverd::LowFunc Low;
    bool CommitTailExhausted = false;
    bool RollbackRetainedPending = false;
    bool PendingCarry = false;
  };
  auto BuildWithBudget = [&](size_t Budget,
                             bool ExhaustStableCommitTail = false) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    Builder.setExhaustStableI386AmbiguityCommitTailForTesting(
        ExhaustStableCommitTail);
    BudgetedBuild Result;
    Result.Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    Result.CommitTailExhausted =
        Builder.proposalStageCommitTailEvidenceExhaustedForTesting();
    Result.RollbackRetainedPending =
        Builder.commitTailRollbackRetainedPendingI386AmbiguityForTesting();
    Result.PendingCarry =
        Builder.hasPendingI386GOTPCAmbiguityForTesting();
    return Result;
  };
  auto HasOpcodeAt = [](const neverd::LowFunc &Low, neverd::va_t Addr,
                        neverd::NdOp Opcode) {
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Addr && Op.Opcode == Opcode)
          return true;
    return false;
  };
  auto IsComplete = [&](const BudgetedBuild &Result) {
    const auto Valid = std::find_if(
        Result.Low.JumpTables.begin(), Result.Low.JumpTables.end(),
        [&](const auto &Table) { return Table.InsnAddr == ValidBranch->Addr; });
    return Valid != Result.Low.JumpTables.end() &&
           Valid->Targets.size() == 2 &&
           HasOpcodeAt(Result.Low, AmbiguousBranch->Addr,
                       neverd::NdOp::INDIR_BR) &&
           HasOpcodeAt(Result.Low, CallbackBranch->Addr,
                       neverd::NdOp::INDIR_CALL);
  };

  const size_t CompleteBudget =
      neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  ASSERT_TRUE(IsComplete(BuildWithBudget(CompleteBudget)));

  // Trigger the exact stable-stage transaction boundary directly.  Searching
  // for "first complete minus one" is brittle because stricter candidate
  // accounting can legitimately move an earlier proof boundary without
  // changing the atomic commit tail itself.
  const BudgetedBuild Boundary =
      BuildWithBudget(CompleteBudget, /*ExhaustStableCommitTail=*/true);
  EXPECT_TRUE(Boundary.CommitTailExhausted)
      << "the one-shot hook must fail at the atomically prepaid stable commit "
         "tail";
  EXPECT_TRUE(Boundary.RollbackRetainedPending)
      << "the injected commit-tail rollback must retain the exact pending key "
         "before the next immutable graph retries";
  EXPECT_FALSE(Boundary.PendingCarry)
      << "the next stable graph must replay and retire the retained key";
  EXPECT_TRUE(IsComplete(Boundary));
  EXPECT_TRUE(HasOpcodeAt(Boundary.Low, AmbiguousBranch->Addr,
                          neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(Boundary.Low, AmbiguousBranch->Addr,
                           neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Boundary.Low.UnsafeIndirectBranchAddresses.count(
                AmbiguousBranch->Addr),
            1u);
  EXPECT_EQ(Boundary.Low.EverPublishedJumpTableBranchAddresses.count(
                AmbiguousBranch->Addr),
            0u);
  EXPECT_TRUE(HasOpcodeAt(Boundary.Low, CallbackBranch->Addr,
                          neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(HasOpcodeAt(Boundary.Low, CallbackBranch->Addr,
                           neverd::NdOp::INDIR_BR));
  EXPECT_EQ(Boundary.Low.UnsafeIndirectBranchAddresses.count(
                CallbackBranch->Addr),
            0u);
  EXPECT_TRUE(HasOpcodeAt(Boundary.Low, ValidBranch->Addr,
                          neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(Boundary.Low, ValidBranch->Addr,
                           neverd::NdOp::INDIR_CALL));
  const auto BoundaryValid = std::find_if(
      Boundary.Low.JumpTables.begin(), Boundary.Low.JumpTables.end(),
      [&](const auto &Table) { return Table.InsnAddr == ValidBranch->Addr; });
  ASSERT_NE(BoundaryValid, Boundary.Low.JumpTables.end());
  EXPECT_EQ(BoundaryValid->Targets.size(), 2u);
  EXPECT_EQ(Boundary.Low.UnsafeIndirectBranchAddresses.count(
                ValidBranch->Addr),
            0u);

  const BudgetedBuild Complete = BuildWithBudget(CompleteBudget);
  EXPECT_TRUE(IsComplete(Complete));
  EXPECT_FALSE(Complete.CommitTailExhausted);
  EXPECT_FALSE(Complete.PendingCarry)
      << "a stable exact replay must retire the matching pending query key";
  EXPECT_EQ(Complete.Low.UnsafeIndirectBranchAddresses.count(
                AmbiguousBranch->Addr),
            1u);
  EXPECT_EQ(Complete.Low.UnsafeIndirectBranchAddresses.count(
                CallbackBranch->Addr),
            0u);
  EXPECT_EQ(Complete.Low.UnsafeIndirectBranchAddresses.count(ValidBranch->Addr),
            0u);
}

TEST_F(JTE_X86_32, GOTPCOperandBindingRejectsDisplacementImmediateCollision) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotpc_operand_collision");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(
      Image, Decoder, Function->Addr, "jt_i386_gotpc_operand_collision");

  ASSERT_EQ(Low.I386GetPcOccurrences.size(), 1u);
  EXPECT_TRUE(Low.RelocatedInstructionScalarModelOccurrences.empty())
      << "the GOTPC immediate must not authenticate a numerically equal "
         "effective-address displacement in the same instruction";
}

TEST_F(JTE_X86_32, AbsoluteDataRelocationCannotMasqueradeAsGOTOFF) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_abs32_displacement_switch");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_i386_abs32_switch_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);

  std::vector<neverd::RelocatedAddressField> Fields;
  for (const auto &[FieldVA, Field] : Image.DataAddressRelocOperands)
    if (FieldVA >= Function->Addr &&
        FieldVA < Function->Addr + Function->Size &&
        Field.TargetVA == Table->Addr)
      Fields.push_back(Field);
  ASSERT_EQ(Fields.size(), 1u);
  EXPECT_EQ(Fields.front().Kind,
            neverd::RelocatedAddressFieldKind::Generic);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(
      Image, Decoder, Function->Addr, "jt_i386_abs32_displacement_switch");
  ASSERT_EQ(Low.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  EXPECT_TRUE(Low.JumpTables.empty())
      << "R_386_32 is an absolute address field, not a GOTOFF displacement";
}

TEST_F(JTE_X86_32, GOTOFFSwitchRequiresTheExactScalarModelOrigin) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  auto Recover = [&](llvm::StringRef Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Computed =
      Recover("jt_i386_gotoff_literal_zero_base");
  ASSERT_EQ(Computed.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  EXPECT_TRUE(Computed.JumpTables.empty())
      << "an independently computed zero cannot borrow another register's "
         "GOT model occurrence";

  const neverd::LowFunc Mixed = Recover("jt_i386_gotoff_mixed_zero_base");
  ASSERT_EQ(Mixed.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  EXPECT_TRUE(Mixed.JumpTables.empty())
      << "a merge containing an unauthenticated zero arm is not a must-origin "
         "GOT base";
}

TEST_F(JTE_X86_32, PreScaledGOTOFFRequiresExactFieldAndModelZeroOrigin) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  auto Recover = [&](llvm::StringRef Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Authenticated =
      Recover("jt_i386_gotoff_prescaled_call_pop");
  ASSERT_EQ(Authenticated.JumpTables.size(), 1u);
  EXPECT_EQ(Authenticated.JumpTables.front().Targets.size(), 4u);

  const neverd::LowFunc Absolute = Recover("jt_i386_abs32_prescaled_switch");
  ASSERT_EQ(Absolute.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  EXPECT_TRUE(Absolute.JumpTables.empty())
      << "a pre-scaled R_386_32 operand must not borrow GOTOFF semantics";

  const neverd::LowFunc Literal =
      Recover("jt_i386_gotoff_prescaled_literal_zero");
  ASSERT_EQ(Literal.RelocatedInstructionScalarModelOccurrences.size(), 1u);
  EXPECT_TRUE(Literal.JumpTables.empty())
      << "an exact pre-scaled GOTOFF field still requires the exact base input "
         "to reach the model-zero occurrence";
}

TEST_F(JTE_X86_32, GOTPCModelBudgetExhaustionPublishesNoPartialProof) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop");
  const neverd::Symbol *Branch =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Branch, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  Builder.setI386GOTModelEvidenceBudgetForTesting(0);
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "jt_i386_gotoff_switch_call_pop");

  EXPECT_TRUE(Low.RelocatedInstructionScalarModelOccurrences.empty());
  EXPECT_TRUE(Low.JumpTables.empty())
      << "budget exhaustion must not publish a partial GOT model batch";
  auto HasOpcodeAt = [&](neverd::NdOp Opcode) {
    for (const neverd::LowBlock &Block : Low.Blocks)
      for (const neverd::LowOp &Op : Block.Ops)
        if (Op.Addr == Branch->Addr && Op.Opcode == Opcode)
          return true;
    return false;
  };
  EXPECT_TRUE(HasOpcodeAt(neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(HasOpcodeAt(neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(Branch->Addr), 1u);
  EXPECT_EQ(Low.EverPublishedJumpTableBranchAddresses.count(Branch->Addr),
            0u);
}

TEST_F(JTE_X86_32, GOTPCModelGraphWorkExhaustionFailsClosed) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X86));
  neverd::CFGBuilder Builder;
  // Model collection consumes fewer than 64 units, leaving a non-zero balance
  // for the whole-CFG query.  The query itself must consume that same balance
  // and report incomplete rather than caching a false model-origin fact.
  Builder.setI386GOTModelEvidenceBudgetForTesting(64);
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "jt_i386_gotoff_switch_call_pop");

  EXPECT_TRUE(Low.RelocatedInstructionScalarModelOccurrences.empty());
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_32, GOTOFFProposalBudgetExhaustionFailsClosed) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop");
  const neverd::Symbol *Branch =
      Image.findSymbol("jt_i386_gotoff_switch_call_pop_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Branch, nullptr);

  struct BudgetedBuild {
    neverd::LowFunc Low;
    bool GraphQueryIssued = false;
    bool GraphBudgetExhausted = false;
  };
  auto BuildWithBudget = [&](size_t Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X86));
    neverd::CFGBuilder Builder;
    Builder.setI386GOTOFFProposalEvidenceBudgetForTesting(Budget);
    BudgetedBuild Result;
    Result.Low = Builder.build(
        Image, Decoder, Function->Addr, "jt_i386_gotoff_switch_call_pop");
    Result.GraphQueryIssued =
        Builder.i386GOTOFFGraphQueryIssuedForTesting();
    Result.GraphBudgetExhausted =
        Builder.i386GOTOFFGraphQueryBudgetExhaustedForTesting();
    return Result;
  };
  auto ExpectOpaque = [&](const BudgetedBuild &Result, size_t Budget) {
    SCOPED_TRACE(Budget);
    const neverd::LowFunc &Low = Result.Low;

    ASSERT_EQ(Low.RelocatedInstructionScalarModelOccurrences.size(), 1u)
        << "model completion has an independent transactional budget";
    EXPECT_TRUE(Low.JumpTables.empty())
        << "proposal-budget exhaustion must not suppress relocation roots or "
           "publish a partial GOTOFF table proof";
    auto HasOpcodeAt = [&](neverd::NdOp Opcode) {
      for (const neverd::LowBlock &Block : Low.Blocks)
        for (const neverd::LowOp &Op : Block.Ops)
          if (Op.Addr == Branch->Addr && Op.Opcode == Opcode)
            return true;
      return false;
    };
    EXPECT_TRUE(HasOpcodeAt(neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(HasOpcodeAt(neverd::NdOp::INDIR_CALL));
    EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(Branch->Addr), 1u)
        << "an unfinished exact GOTOFF proof is opaque, not callback evidence";
    EXPECT_EQ(
        Low.EverPublishedJumpTableBranchAddresses.count(Branch->Addr), 0u);
  };
  // Budget zero stops before the combined reaching-value query.
  const BudgetedBuild Zero = BuildWithBudget(0);
  EXPECT_FALSE(Zero.GraphQueryIssued);
  ExpectOpaque(Zero, 0);

  // This allowance reaches the combined query with margin after all
  // root/model bookkeeping, then exhausts only inside metered graph
  // propagation.  Passing a null GraphWorkBudget makes that query unbounded
  // and incorrectly publishes the table (with EverPublished set and no unsafe
  // identity), so the semantic assertions below are a true wiring mutation.
  constexpr size_t QueryBudget = 2816;
  const BudgetedBuild GraphBoundary = BuildWithBudget(QueryBudget);
  EXPECT_TRUE(GraphBoundary.GraphQueryIssued);
  EXPECT_TRUE(GraphBoundary.GraphBudgetExhausted)
      << "the selected bounded combined query must consume its graph "
         "allowance; budget="
      << QueryBudget;
  ExpectOpaque(GraphBoundary, QueryBudget);
}

TEST_F(JTE_X86_32, FullWidthESPPrivateFrameSpillPreservesSwitch) {
  auto R = liftToLLVMIRUnopt(frameLaneX86Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      llvmFunctionBody(R.out, "jt_i386_private_frame_spill");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("switch i"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 1, label"), std::string::npos) << Body;
  // A recovered switch deliberately traps on selectors outside its proven
  // domain.  The regression is the absence of the two-case switch (the whole
  // indirect branch becoming an unsafe trap), not that sound default block.
  EXPECT_NE(Body.find("jt.default.trap"), std::string::npos) << Body;
}

//===----------------------------------------------------------------------===//
// x86-64
//===----------------------------------------------------------------------===//

class JTE_X86_64 : public NeverDLiftTest {
protected:
  void expectMutableTableFailsClosed(const fs::path &Object,
                                     const std::string &Function);
};

static fs::path jteX64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_enhanced.o";
}

TEST_F(JTE_X86_64, AllStagesSucceed) { verifyAllStages(jteX64Obj()); }

TEST_F(JTE_X86_64, LowIRHasBranchInd) {
  auto R = liftToLowIR(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INDIR_BR")) << "Expected INDIR_BR for jump tables";
}

TEST_F(JTE_X86_64, MultipleSwitchFunctions) {
  auto R = liftToLowIR(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  int FuncCount = 0;
  std::string::size_type Pos = 0;
  while ((Pos = R.out.find("func ", Pos)) != std::string::npos) {
    ++FuncCount;
    ++Pos;
  }
  EXPECT_GE(FuncCount, 4) << "Expected at least 4 functions";
}

TEST_F(JTE_X86_64, HighIRHasControlFlow) {
  auto R = liftToHighIR(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasControl = R.out.find("switch") != std::string::npos ||
                    R.out.find("case ") != std::string::npos ||
                    R.out.find("if") != std::string::npos ||
                    R.out.find("return") != std::string::npos;
  EXPECT_TRUE(HasControl) << "Expected control flow in HighIR";
}

TEST_F(JTE_X86_64, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(jteX64Obj());
}

TEST_F(JTE_X86_64, NoConstantTrueBranches) {
  verifyNoConstantTrueBranch(jteX64Obj());
}

TEST_F(JTE_X86_64, DecompileSucceeds) {
  auto R = decompileToHighC(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
  EXPECT_GT(fs::file_size(CFile), 50u) << "Decompiled C too small";
}

TEST_F(JTE_X86_64, Medium32Entries) {
  auto R = liftToLowIR(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("jt_medium_32") || R.contains("func "))
      << "Expected the 32-entry function to be lifted";
}

// A switch whose lowest case label is negative is lowered by normalizing the
// variable to a zero-based table index; recovery must present the source
// variable with the true negative case labels rather than the 0-based index.
TEST_F(JTE_X86_64, NegativeCaseLabelsRecovered) {
  auto R = decompileToHighC(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  std::ifstream IFS(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(IFS) << "C output file not created";
  std::ostringstream SS;
  SS << IFS.rdbuf();
  std::string C = SS.str();
  // The negative-base dispatch must yield the source labels (-6..3), not the
  // normalized 0-based index.
  EXPECT_NE(C.find("case -6:"), std::string::npos)
      << "Expected negative case label -6 from jt_neg_dispatch";
  EXPECT_NE(C.find("case -1:"), std::string::npos)
      << "Expected negative case label -1 from jt_neg_dispatch";
}

static fs::path inclusivePostLoadClobberObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_inclusive_postload_clobber.o";
}

static fs::path inclusivePreLoadAliasObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_inclusive_preload_alias.o";
}

static fs::path inclusivePostLoadUnrelatedObj() {
  return fs::path(TEST_OBJ_DIR) /
         "test_jumptable_inclusive_postload_unrelated.o";
}

static fs::path inclusivePreLoadReuseObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_inclusive_preload_reuse.o";
}

static fs::path identityCfgLaneObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_identity_cfg_lane.o";
}

static fs::path moduloDomainObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_modulo_domain.o";
}

static fs::path relativeIdentityObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_relative_identity.o";
}

static fs::path moduloDeadProducerObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_modulo_dead_producer.o";
}

static fs::path rawRelativeDomainObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_raw_relative_domain.o";
}

static fs::path lostPublishedX64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_lost_published.o";
}

static fs::path targetOwnershipX64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_target_ownership.o";
}

static fs::path maskEqualBoundObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_mask_equal_bound.o";
}

static fs::path moduleAddressOwnerInductionObj() {
  return fs::path(TEST_OBJ_DIR) / "test_module_address_owner_induction.o";
}

static fs::path selectorOccurrenceX64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_selector_occurrence.o";
}

static std::string lowFunctionBody(const std::string &Low,
                                   const std::string &Name);

TEST_F(JTE_X86_64, PreScaledSelectorUsesExactPreLoadOccurrence) {
  auto Low = liftToLowIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_selector_prescaled_postload_reuse");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("0x17D4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("0x17D7"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(selectorOccurrenceX64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_selector_prescaled_postload_reuse");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  const auto Condition = llvmSwitchConditionToken(LLVMBody);
  ASSERT_TRUE(Condition) << LLVMBody;
  EXPECT_TRUE(llvmSSAValueDependsOn(LLVMBody, *Condition, "%arg0"))
      << "switch condition " << *Condition
      << " must depend on the pre-LOAD table index\n"
      << LLVMBody;
  EXPECT_FALSE(llvmSSAValueDependsOn(LLVMBody, *Condition, "%arg1"))
      << "switch condition " << *Condition
      << " must not use the post-LOAD R10 lifetime\n"
      << LLVMBody;

  auto High = liftToHighIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_selector_prescaled_postload_reuse");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  const std::string::size_type Switch = HighBody.find("switch ");
  ASSERT_NE(Switch, std::string::npos) << HighBody;
  const std::string::size_type SwitchEnd = HighBody.find('\n', Switch);
  const std::string SwitchLine = HighBody.substr(Switch, SwitchEnd - Switch);
  EXPECT_NE(SwitchLine.find("arg0"), std::string::npos) << HighBody;
  EXPECT_EQ(SwitchLine.find("arg1"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, MaskedSelectorKeepsTheExactPostAndValue) {
  auto Low = liftToLowIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_selector_masked_same_reg");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("0x1838"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("0x183F"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(selectorOccurrenceX64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_selector_masked_same_reg");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  const auto Condition = llvmSwitchConditionToken(LLVMBody);
  ASSERT_TRUE(Condition) << LLVMBody;
  EXPECT_TRUE(
      llvmSSAValueDependencyContains(LLVMBody, *Condition, " = and i", ", 7"))
      << "x=8 must be masked to selector 0 before the switch\n"
      << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 0)) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 7)) << LLVMBody;

  auto High = liftToHighIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_selector_masked_same_reg");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  const std::string::size_type Switch = HighBody.find("switch ");
  ASSERT_NE(Switch, std::string::npos) << HighBody;
  const std::string::size_type SwitchEnd = HighBody.find('\n', Switch);
  const std::string SwitchLine = HighBody.substr(Switch, SwitchEnd - Switch);
  EXPECT_NE(SwitchLine.find("arg0 & 7"), std::string::npos) << HighBody;
  EXPECT_NE(HighBody.find("case 0:\n      return (i64)0x1838"),
            std::string::npos)
      << "x=8 must route through mask value 0 to case0\n"
      << HighBody;
}

TEST_F(JTE_X86_64, TwoTableSelectorUsesExactCompositeOccurrences) {
  auto Low = liftToLowIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_selector_twotable_relay");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(selectorOccurrenceX64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_selector_twotable_relay");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  const auto Condition = llvmSwitchConditionToken(LLVMBody);
  ASSERT_TRUE(Condition) << LLVMBody;
  EXPECT_TRUE(llvmSSAValueDependsOn(LLVMBody, *Condition, "%arg0"))
      << "the merged selector must retain the pre-LOAD byte index\n"
      << LLVMBody;
  EXPECT_TRUE(llvmSSAValueDependsOn(LLVMBody, *Condition, "%arg1"))
      << "the merged selector must retain the exact table-select condition\n"
      << LLVMBody;
  EXPECT_FALSE(llvmSSAValueDependsOn(LLVMBody, *Condition, "%arg2"))
      << "the post-LOAD R10 lifetime is not the selector\n"
      << LLVMBody;
  EXPECT_NE(LLVMBody.find("i64 0, label"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("i64 24, label"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("i64 32, label"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("i64 56, label"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_selector_twotable_relay");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  const std::string::size_type Switch = HighBody.find("switch ");
  ASSERT_NE(Switch, std::string::npos) << HighBody;
  const std::string::size_type SwitchEnd = HighBody.find('\n', Switch);
  const std::string SwitchLine = HighBody.substr(Switch, SwitchEnd - Switch);
  EXPECT_NE(SwitchLine.find("arg0"), std::string::npos) << HighBody;
  EXPECT_NE(SwitchLine.find("arg1"), std::string::npos) << HighBody;
  EXPECT_EQ(SwitchLine.find("arg2"), std::string::npos) << HighBody;
  EXPECT_NE(HighBody.find("case 0:"), std::string::npos) << HighBody;
  EXPECT_NE(HighBody.find("case 32:"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, TwoTableDiamondNeedsAnEdgeMergedSelectorPlan) {
  auto Low = liftToLowIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_selector_twotable_diamond");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(selectorOccurrenceX64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_selector_twotable_diamond");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(selectorOccurrenceX64Obj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_selector_twotable_diamond");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch "), std::string::npos) << HighBody;
}

static std::string lowFunctionBody(const std::string &Low,
                                   const std::string &Name) {
  const std::string Prefix = "func " + Name + " ";
  std::string::size_type Begin = Low.find(Prefix);
  if (Begin == std::string::npos)
    return {};
  std::string::size_type End = Low.find("\nfunc ", Begin + Prefix.size());
  return End == std::string::npos ? Low.substr(Begin)
                                  : Low.substr(Begin, End - Begin);
}

static void
expectIndirectDispatchHasNoStaticSuccessors(const std::string &Body) {
  std::string::size_type Indirect = Body.find("INDIR_CALL");
  if (Indirect == std::string::npos)
    Indirect = Body.find("INDIR_BR");
  ASSERT_NE(Indirect, std::string::npos) << Body;
  const std::string::size_type Block = Body.rfind("\n  block ", Indirect);
  ASSERT_NE(Block, std::string::npos) << Body;
  const std::string::size_type HeaderEnd = Body.find('\n', Block + 1);
  ASSERT_NE(HeaderEnd, std::string::npos) << Body;
  const std::string Header = Body.substr(Block, HeaderEnd - Block);
  EXPECT_NE(Header.find("succs=[]"), std::string::npos) << Header << '\n'
                                                        << Body;
}

static void expectSwitchNotOnLoadedEntry(const std::string &Body,
                                         const std::string &EntryType) {
  const std::string LoadNeedle = "= load " + EntryType;
  for (std::string::size_type Load = Body.find(LoadNeedle);
       Load != std::string::npos; Load = Body.find(LoadNeedle, Load + 1)) {
    std::string::size_type LoadLine = Body.rfind('\n', Load);
    LoadLine = LoadLine == std::string::npos ? 0 : LoadLine + 1;
    LoadLine = Body.find('%', LoadLine);
    if (LoadLine == std::string::npos)
      continue;
    std::string::size_type LoadNameEnd = Body.find(' ', LoadLine);
    if (LoadNameEnd == std::string::npos)
      continue;
    const std::string LoadName = Body.substr(LoadLine, LoadNameEnd - LoadLine);

    const std::string ExtendNeedle = "= zext " + EntryType + " " + LoadName;
    std::string::size_type Extend = Body.find(ExtendNeedle, Load);
    if (Extend == std::string::npos)
      continue;
    std::string::size_type ExtendLine = Body.rfind('\n', Extend);
    ExtendLine = ExtendLine == std::string::npos ? 0 : ExtendLine + 1;
    ExtendLine = Body.find('%', ExtendLine);
    if (ExtendLine == std::string::npos)
      continue;
    std::string::size_type EntryNameEnd = Body.find(' ', ExtendLine);
    if (EntryNameEnd == std::string::npos)
      continue;
    const std::string EntryName =
        Body.substr(ExtendLine, EntryNameEnd - ExtendLine);

    std::string::size_type Switch = Body.find("switch i", Extend);
    if (Switch == std::string::npos)
      continue;
    std::string::size_type SwitchEnd = Body.find('\n', Switch);
    const std::string SwitchLine = Body.substr(Switch, SwitchEnd - Switch);
    EXPECT_EQ(SwitchLine.find(EntryName), std::string::npos)
        << "Switch must use the table index, not the loaded compact entry: "
        << SwitchLine << "\n"
        << Body;
    return;
  }
  FAIL() << "Expected a compact table-entry load/extension chain:\n" << Body;
}

static void expectInclusiveLastCase(const RunResult &R) {
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(llvmHasSwitchCase(R.out, 2))
      << "Expected the inclusive guard to retain table slot 2:\n"
      << R.out;
}

static void expectCasesZeroAndOne(const RunResult &R) {
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(llvmHasSwitchCase(R.out, 0)) << R.out;
  EXPECT_TRUE(llvmHasSwitchCase(R.out, 1)) << R.out;
}

// The physical index register can be reused after the table load but before
// the indirect branch.  Guard recovery must stay anchored at the load use
// point; tracing from the branch would mistake the later value for the index
// and drop the inclusive guard's final case.
TEST_F(JTE_X86_64, InclusiveBoundSurvivesPostLoadIndexClobber) {
  expectInclusiveLastCase(liftToLLVMIRUnopt(inclusivePostLoadClobberObj()));
}

// Conversely, a compiler may copy the guarded source register into a fresh
// register before using it as the table index.  Point-sensitive recovery must
// retain that pre-load alias and still associate the guard with the table.
TEST_F(JTE_X86_64, InclusiveBoundFollowsPreLoadIndexAlias) {
  expectInclusiveLastCase(liftToLLVMIRUnopt(inclusivePreLoadAliasObj()));
}

// A post-load copy from an independently guarded register must not be treated
// as index provenance.  The third physical relocation is deliberately
// adjacent to the two-entry masked table; consuming the unrelated inclusive
// guard would expose that unreachable poison slot as case 2.
TEST_F(JTE_X86_64, InclusiveBoundIgnoresPostLoadUnrelatedCopy) {
  auto R = liftToLLVMIRUnopt(inclusivePostLoadUnrelatedObj());
  expectCasesZeroAndOne(R);
  EXPECT_FALSE(llvmHasSwitchCase(R.out, 2)) << R.out;
  EXPECT_EQ(R.out.find("trunc i64 399"), std::string::npos) << R.out;
}

// A physical register reused before the table load denotes a new value
// lifetime.  The old r10 guard constrains arg1, while the new r10 table index
// comes from independently range-checked arg0; register-number equality must
// not expose slot 2.
TEST_F(JTE_X86_64, InclusiveBoundRejectsPreLoadRegisterReuse) {
  auto R = liftToLLVMIRUnopt(inclusivePreLoadReuseObj());
  expectCasesZeroAndOne(R);
  EXPECT_FALSE(llvmHasSwitchCase(R.out, 2)) << R.out;
}

TEST_F(JTE_X86_64, IndexIdentityMergesEqualDiamondDefinitions) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_diamond_agree");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x1F4:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x1F5:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x1F6:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityRejectsAmbiguousDiamondDefinitions) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_diamond_ambiguous");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x258:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x259:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x2BB:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityPreservesNonOverlappingHighByteWrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_ah_nonoverlap");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x2BC:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x2BD:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x2BE:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityKillsOverlappingHighByteWrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_ah_overlap");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x320:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x321:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x383:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityAcceptsExactSignExtension) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_sext_agree");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x384:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x385:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x386:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityDistinguishesZeroAndSignExtension) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_zext_sext_mismatch");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x3E8:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x3E9:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x44B:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityFollowsExactFrameSpillReload) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_spill_reload");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x44C:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x44D:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x44E:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityRevalidatesAfterCaseBackedge) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_case_backedge");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x4B0:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x4B1:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x513:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, CoverageInventoryExcludesPrunedProvisionalCases) {
  auto ImageOrErr = neverd::loadBinary(lostPublishedX64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_lost_published_table");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "jt_identity_lost_published_table");

  size_t PublishedInstructions = 0;
  for (const neverd::LowBlock &Block : Low.Blocks)
    PublishedInstructions += Block.InstructionBoundaries.size();
  ASSERT_GT(PublishedInstructions, 0u);
  EXPECT_EQ(Low.DecodedInstructionCount, PublishedInstructions);
  EXPECT_EQ(Low.LiftedInstructionCount, PublishedInstructions);
  EXPECT_TRUE(Low.DecodeFailureAddresses.empty());
  EXPECT_TRUE(Low.UnsupportedInstructionAddresses.empty());
  EXPECT_TRUE(Low.TruncatedPathAddresses.empty());
  EXPECT_TRUE(Low.hasCompleteLiftCoverage());
}

TEST_F(JTE_X86_64,
       LostPublishedTableRemainsOpaqueBranchAfterNormalRevalidation) {
  auto Low = liftToLowIR(lostPublishedX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string Body =
      lowFunctionBody(Low.out, "jt_identity_lost_published_table");
  ASSERT_FALSE(Body.empty()) << Low.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;

  auto LLVM = liftToLLVMIRUnopt(lostPublishedX64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_lost_published_table");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_NE(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_X86_64,
       FreshBuilderRetainsPreviouslyPublishedOpaqueBranchIdentity) {
  auto ImageOrErr = neverd::loadBinary(lostPublishedX64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_fresh_published_table");
  const neverd::Symbol *Observer =
      Image.findSymbol("jt_identity_fresh_published_observer");
  const neverd::Symbol *IndirectCall =
      Image.findSymbol("jt_identity_fresh_published_indirect_call");
  const neverd::Symbol *Storage =
      Image.findSymbol("jt_identity_fresh_published_table_storage");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Observer, nullptr);
  ASSERT_NE(IndirectCall, nullptr);
  ASSERT_NE(Storage, nullptr);

  const std::set<neverd::va_t> Entries{Function->Addr, Observer->Addr,
                                       IndirectCall->Addr};
  neverd::Decoder FirstDecoder;
  ASSERT_TRUE(FirstDecoder.init(neverd::Arch::X64));
  neverd::CFGBuilder FirstBuilder;
  FirstBuilder.setKnownFuncEntries(&Entries);
  const neverd::LowFunc First = FirstBuilder.build(
      Image, FirstDecoder, Function->Addr, Function->Name);
  ASSERT_EQ(First.JumpTables.size(), 1u);
  const neverd::va_t Branch = First.JumpTables.front().InsnAddr;
  ASSERT_EQ(First.JumpTables.front().Targets.size(), 2u);
  ASSERT_NE(std::find(First.JumpTables.front().SuppressibleRelocationSlots.begin(),
                      First.JumpTables.front().SuppressibleRelocationSlots.end(),
                      Storage->Addr),
            First.JumpTables.front().SuppressibleRelocationSlots.end());
  ASSERT_EQ(First.EverPublishedJumpTableBranchAddresses.count(Branch), 1u);

  // Protecting slot zero makes case 0 an independent proof root.  Even without
  // history, full-object storage and authoritative local target ownership
  // preserve the unresolved dispatch as an opaque branch.  Published history
  // remains a separate, one-shot fact checked below.
  const std::set<neverd::va_t> ProtectedSlots{Storage->Addr};
  neverd::Decoder NoSeedDecoder;
  ASSERT_TRUE(NoSeedDecoder.init(neverd::Arch::X64));
  neverd::CFGBuilder NoSeedBuilder;
  NoSeedBuilder.setKnownFuncEntries(&Entries);
  NoSeedBuilder.setProtectedJumpTableRelocationSlots(&ProtectedSlots);
  const neverd::LowFunc NoSeed = NoSeedBuilder.build(
      Image, NoSeedDecoder, Function->Addr, Function->Name);
  ASSERT_TRUE(NoSeed.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(NoSeed, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(NoSeed, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(NoSeed.UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_TRUE(NoSeed.EverPublishedJumpTableBranchAddresses.empty());

  neverd::Decoder SeededDecoder;
  ASSERT_TRUE(SeededDecoder.init(neverd::Arch::X64));
  neverd::CFGBuilder SeededBuilder;
  SeededBuilder.setKnownFuncEntries(&Entries);
  SeededBuilder.setProtectedJumpTableRelocationSlots(&ProtectedSlots);
  std::set<neverd::va_t> SeedHistory =
      First.EverPublishedJumpTableBranchAddresses;
  SeedHistory.insert(Function->Addr); // current, but not an indirect branch
  SeedHistory.insert(Storage->Addr);  // data, not a current instruction
  SeededBuilder.setPreviouslyPublishedJumpTableBranches(&SeedHistory);
  const neverd::LowFunc Seeded = SeededBuilder.build(
      Image, SeededDecoder, Function->Addr, Function->Name);
  ASSERT_TRUE(Seeded.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Seeded, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Seeded, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Seeded.EverPublishedJumpTableBranchAddresses.count(Branch), 1u);
  EXPECT_EQ(Seeded.EverPublishedJumpTableBranchAddresses.size(), 1u);
  EXPECT_EQ(Seeded.UnsafeIndirectBranchAddresses.count(Branch), 1u);

  // A history seed is scoped to exactly one build.  Reusing the public
  // builder without another setter call must neither dereference the caller's
  // old set nor inject the previous function/build's branch identities.
  neverd::Decoder ReusedDecoder;
  ASSERT_TRUE(ReusedDecoder.init(neverd::Arch::X64));
  const neverd::LowFunc Reused = SeededBuilder.build(
      Image, ReusedDecoder, Function->Addr, Function->Name);
  ASSERT_TRUE(Reused.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Reused, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Reused, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Reused.UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_TRUE(Reused.EverPublishedJumpTableBranchAddresses.empty());

  const std::set<neverd::va_t> CallSeed{IndirectCall->Addr};
  neverd::Decoder CallDecoder;
  ASSERT_TRUE(CallDecoder.init(neverd::Arch::X64));
  neverd::CFGBuilder CallBuilder;
  CallBuilder.setKnownFuncEntries(&Entries);
  CallBuilder.setPreviouslyPublishedJumpTableBranches(&CallSeed);
  const neverd::LowFunc CallControl = CallBuilder.build(
      Image, CallDecoder, IndirectCall->Addr, IndirectCall->Name);
  EXPECT_TRUE(CallControl.EverPublishedJumpTableBranchAddresses.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(CallControl, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(CallControl.UnsafeIndirectBranchAddresses.empty());

  neverd::LowToMedConverter Converter;
  Converter.setBinaryImage(&Image);
  neverd::MedFunc Med =
      Converter.convert(Seeded, neverd::Arch::X64, neverd::BinaryFormat::ELF);
  llvm::LLVMContext Context;
  auto Module = neverd::MedLLVMEmitter().emit(
      {Med}, Context, "fresh-builder-ever-published", neverd::Arch::X64, {},
      &Image, neverd::BinaryFormat::ELF);
  ASSERT_NE(Module, nullptr);
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Module->print(OS, nullptr);
  OS.flush();
  const std::string Body = llvmFunctionBody(IR, Function->Name);
  ASSERT_FALSE(Body.empty()) << IR;
  EXPECT_NE(Body.find("llvm.trap"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("switch i"), std::string::npos) << Body;

  DirectPipelineRun PipelineRun = runPipelineWithEvidenceBudget(Image, 256);
  ASSERT_TRUE(PipelineRun.Result.Success) << PipelineRun.Result.Error;
  const neverd::LowFunc *PipelineLow =
      findLowFunction(PipelineRun.Result, Function->Name);
  ASSERT_NE(PipelineLow, nullptr);
  EXPECT_TRUE(PipelineLow->JumpTables.empty());
  EXPECT_EQ(
      PipelineLow->EverPublishedJumpTableBranchAddresses.count(Branch), 1u);
  EXPECT_EQ(PipelineLow->UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_TRUE(lowFunctionHasOpcode(*PipelineLow, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(*PipelineLow, neverd::NdOp::INDIR_CALL));
  const std::string PipelineBody =
      llvmFunctionBody(PipelineRun.LLVMIR, Function->Name);
  ASSERT_FALSE(PipelineBody.empty()) << PipelineRun.LLVMIR;
  EXPECT_NE(PipelineBody.find("llvm.trap"), std::string::npos) << PipelineBody;
  EXPECT_EQ(PipelineBody.find("switch i"), std::string::npos) << PipelineBody;
}

TEST_F(JTE_X86_64, IndexIdentityRejectsAtomicFrameOverwrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_atomic_overwrite");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x514:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x515:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x577:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityRejectsCrossFrameBaseOverwrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body =
      lowFunctionBody(R.out, "jt_identity_cross_frame_overwrite");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x578:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x579:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x5DB:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityRejectsEscapedFrameSlotAcrossCall) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_call_overwrite");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x640:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x641:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x6A3:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, IndexIdentityRejectsMayAliasSelectStore) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_select_store_alias");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x6A4:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x6A5:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x707:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsPathAmbiguousFlagsAtJoin) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_flags_diamond");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x708:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x709:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x76B:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceChecksTableReachingBranchPolarity) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_reversed_polarity");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x76C:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x76D:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x7CF:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceIntersectsSequentialUpperBounds) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body =
      lowFunctionBody(R.out, "jt_identity_redundant_weaker_inclusive");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x7D0:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x7D1:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x833:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsPeriodicAliasOutsideSamplePrefix) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_periodic_alias");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsSignedNegativeHalfDomain) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_signed_negative_alias");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, SemanticGuardRejectionDoesNotCaptureSizedCallbackTailCall) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_guard_signed_callback_tailcall");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_guard_signed_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 2u * 8u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, GuardEvidenceStopsAtOverlappingSubregisterWrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_overlapping_write");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsBoundBeyondDiscoveryWindow) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_bound_beyond_sample");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceFindsPostLaidOutLargeBound) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_postlaid_guard_large");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceAcceptsPostLaidOutDensePrefix) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_postlaid_guard_four");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xF3C:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xF3D:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xF3E:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xF3F:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceMemoizesSharedExpressionDag) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_shared_dag_budget");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xFA0:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xFA1:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xFA2:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xFA3:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceDepthBudgetFailsClosed) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_depth_budget");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceControlBatchBudgetFailsClosed) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_control_budget");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, JumpTableRejectsNextFunctionInteriorTarget) {
  auto Low = liftToLowIR(targetOwnershipX64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string Body = lowFunctionBody(
      Low.out, "jt_target_ownership_rejects_next_function_interior");
  ASSERT_FALSE(Body.empty()) << Low.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x1387:4"), std::string::npos)
      << "an interior address in the next known function is not owned by the "
         "current jump table:\n"
      << Body;
}

TEST_F(JTE_X86_64, JumpTableAcceptsExplicitSameOwnerUnwindFragment) {
  auto ImageOrErr = neverd::loadBinary(targetOwnershipX64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Owner =
      Image.findSymbol("jt_target_ownership_rejects_next_function_interior");
  const neverd::Symbol *Foreign =
      Image.findSymbol("jt_target_ownership_foreign_function");
  ASSERT_NE(Owner, nullptr);
  ASSERT_NE(Foreign, nullptr);
  ASSERT_NE(Owner->Size, 0u);
  ASSERT_GT(Foreign->Size, 1u);

  Image.ExceptionMetadata.Functions.clear();
  neverd::ExceptionFunction Primary;
  Primary.Kind = neverd::RuntimeFunctionKind::Primary;
  Primary.CodeRange = {Owner->Addr, Owner->Addr + Owner->Size};
  Image.ExceptionMetadata.Functions.push_back(Primary);
  neverd::ExceptionFunction Fragment;
  Fragment.Kind = neverd::RuntimeFunctionKind::Fragment;
  Fragment.CodeRange = {Foreign->Addr + 1, Foreign->Addr + Foreign->Size};
  Fragment.PrimaryFunctionIndex = 0;
  Fragment.ChainedPrimaryRange = Primary.CodeRange;
  Image.ExceptionMetadata.Functions.push_back(Fragment);
  Image.ExceptionMetadata.rebuildIndex();

  std::set<neverd::va_t> Entries{Owner->Addr, Foreign->Addr};
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Owner->Addr, Owner->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  ASSERT_EQ(Low.JumpTables.front().Targets.size(), 2u);
  EXPECT_NE(std::find(Low.JumpTables.front().Targets.begin(),
                      Low.JumpTables.front().Targets.end(), Foreign->Addr + 1),
            Low.JumpTables.front().Targets.end())
      << "a fragment is an out-of-range case only when unwind metadata links "
         "it to this exact primary function";
}

TEST_F(JTE_X86_64, ModuloBoundRecoversClangO0U140FrameReload) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_u140_spill_reload_loop");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Function->Size, 0u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const neverd::JumpTable &JT = Low.JumpTables.front();
  ASSERT_EQ(JT.Targets.size(), 140u);
  EXPECT_EQ(std::set<neverd::va_t>(JT.Targets.begin(), JT.Targets.end()).size(),
            140u);
  ASSERT_TRUE(JT.HasDispatchSlotMap);
  ASSERT_EQ(JT.SlotIndices.size(), 140u);
  for (uint32_t Slot = 0; Slot < 140; ++Slot)
    EXPECT_EQ(JT.SlotIndices[Slot], Slot);
  uint64_t PhysicalSlots = 0;
  for (const neverd::JumpTableStorageRange &Range : JT.StorageRanges) {
    PhysicalSlots += Range.PhysicalSlotCount;
    EXPECT_EQ(Range.EntrySize, 4u);
    EXPECT_EQ(Range.EntryStride, 4u);
  }
  EXPECT_EQ(PhysicalSlots, 140u);
}

TEST_F(JTE_X86_64, ModuloBoundRejectsDeadExactProducer) {
  auto ImageOrErr = neverd::loadBinary(moduloDeadProducerObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_u140_dead_producer");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Function->Size, 0u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(Low.EverPublishedJumpTableBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsIndependentQuotientRoot) {
  auto R = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_modulo_independent_quotient");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_TRUE(Body.find("INDIR_CALL") != std::string::npos ||
              Body.find("INDIR_BR") != std::string::npos)
      << Body;
  expectIndirectDispatchHasNoStaticSuccessors(Body);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_modulo_independent_quotient");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_modulo_independent_quotient");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, ModuloRejectedSizedMixedSelfEntryTableStaysOpaque) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_sized_mixed_callback");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JTE_X86_64, SizedCallbackTableCannotOverrideIncompleteResolverEvidence) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_callback_graph_incomplete");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_callback_graph_incomplete_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 2u * 8u);

  auto BuildWithBudget = [&](std::optional<size_t> Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };
  auto IsTailCall = [](const neverd::LowFunc &Low) {
    return Low.JumpTables.empty() &&
           lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL) &&
           !lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR);
  };

  const neverd::LowFunc Full = BuildWithBudget(std::nullopt);
  ASSERT_TRUE(IsTailCall(Full));

  size_t Lo = 1;
  size_t Hi = neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  while (Lo < Hi) {
    const size_t Mid = Lo + (Hi - Lo) / 2;
    if (IsTailCall(BuildWithBudget(Mid)))
      Hi = Mid;
    else
      Lo = Mid + 1;
  }
  ASSERT_GT(Lo, 1u);
  ASSERT_TRUE(IsTailCall(BuildWithBudget(Lo)));

  const neverd::LowFunc Exhausted = BuildWithBudget(Lo - 1);
  EXPECT_TRUE(Exhausted.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Exhausted.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, UnsizedRelativeIdentityClassifiesWholeOwnerRun) {
  auto ImageOrErr = neverd::loadBinary(relativeIdentityObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;

  const neverd::Symbol *ForeignTarget =
      Image.findSymbol("jt_relative_identity_foreign_target");
  ASSERT_NE(ForeignTarget, nullptr);
  ASSERT_TRUE(Image.hasFunctionSymbolAt(ForeignTarget->Addr));
  for (llvm::StringRef TableName :
       {"jt_modulo_unsized_relative_mixed_table",
        "jt_modulo_unsized_relative_foreign_table",
        "jt_modulo_unsized_relative_unknown_table"}) {
    const neverd::Symbol *Table = Image.findSymbol(TableName);
    ASSERT_NE(Table, nullptr) << TableName.str();
    ASSERT_EQ(Table->Size, 0u) << TableName.str();
    EXPECT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                            Image.RelCodeRelocSlots.end(),
                            [&](neverd::va_t Slot) {
                              return Slot >= Table->Addr &&
                                     Slot < Table->Addr + 3u * 4u;
                            }),
              3)
        << TableName.str();
  }
  const neverd::Symbol *UnknownTwoTable =
      Image.findSymbol("jt_modulo_unsized_relative_unknown_two_table");
  ASSERT_NE(UnknownTwoTable, nullptr);
  ASSERT_EQ(UnknownTwoTable->Size, 0u);
  EXPECT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= UnknownTwoTable->Addr &&
                                   Slot < UnknownTwoTable->Addr + 2u * 4u;
                          }),
            2);

  auto Build = [&](llvm::StringRef Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr) << Name.str();
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Mixed =
      Build("jt_modulo_unsized_relative_mixed_callback");
  EXPECT_TRUE(Mixed.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Mixed, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Mixed, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Mixed.UnsafeIndirectBranchAddresses.empty());

  const neverd::LowFunc Foreign =
      Build("jt_modulo_unsized_relative_foreign_callback");
  EXPECT_TRUE(Foreign.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Foreign, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(Foreign, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(Foreign, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Foreign.UnsafeIndirectBranchAddresses.empty());

  const neverd::LowFunc Unknown =
      Build("jt_modulo_unsized_relative_unknown_callback");
  EXPECT_TRUE(Unknown.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Unknown, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Unknown, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Unknown.UnsafeIndirectBranchAddresses.empty());

  const neverd::LowFunc UnknownTwo =
      Build("jt_modulo_unsized_relative_unknown_two_callback");
  EXPECT_TRUE(UnknownTwo.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(UnknownTwo, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(UnknownTwo, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(UnknownTwo.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, RelocationRunLimitRequiresCompleteIdentityScan) {
  auto ImageOrErr = neverd::loadBinary(relativeIdentityObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  constexpr uint64_t Limit = neverd::limits::kMaxJumpTableEntries;

  const neverd::Symbol *ExactTable =
      Image.findSymbol("jt_identity_exact_limit_foreign_table");
  const neverd::Symbol *AbsoluteOverflow =
      Image.findSymbol("jt_identity_over_limit_absolute_table");
  const neverd::Symbol *RelativeOverflow =
      Image.findSymbol("jt_identity_over_limit_relative_table");
  ASSERT_NE(ExactTable, nullptr);
  ASSERT_NE(AbsoluteOverflow, nullptr);
  ASSERT_NE(RelativeOverflow, nullptr);
  ASSERT_EQ(ExactTable->Size, Limit * 8u);
  ASSERT_EQ(AbsoluteOverflow->Size, 0u);
  ASSERT_EQ(RelativeOverflow->Size, 0u);

  auto CountSlots = [](const std::set<neverd::va_t> &Slots,
                       neverd::va_t Base, uint64_t Count,
                       uint64_t Stride) {
    return std::count_if(Slots.begin(), Slots.end(), [&](neverd::va_t Slot) {
      return Slot >= Base && Slot < Base + Count * Stride;
    });
  };
  EXPECT_EQ(CountSlots(Image.CodePtrRelocSlots, ExactTable->Addr, Limit, 8),
            Limit);
  EXPECT_EQ(CountSlots(Image.CodePtrRelocSlots, AbsoluteOverflow->Addr,
                       Limit + 1u, 8),
            Limit + 1u);
  EXPECT_EQ(CountSlots(Image.RelCodeRelocSlots, RelativeOverflow->Addr,
                       Limit + 1u, 4),
            Limit + 1u);
  bool ExactComplete = false;
  EXPECT_EQ(neverd::countCodePtrRelocRun(Image, ExactTable->Addr, 8,
                                        &ExactComplete),
            Limit);
  EXPECT_TRUE(ExactComplete);
  bool AbsoluteComplete = true;
  EXPECT_EQ(neverd::countCodePtrRelocRun(Image, AbsoluteOverflow->Addr, 8,
                                        &AbsoluteComplete),
            Limit);
  EXPECT_FALSE(AbsoluteComplete);
  bool RelativeComplete = true;
  EXPECT_EQ(neverd::countRelCodeRelocRun(Image, RelativeOverflow->Addr, 4,
                                        &RelativeComplete),
            Limit);
  EXPECT_FALSE(RelativeComplete);

  auto Build = [&](const neverd::BinaryImage &CandidateImage,
                   llvm::StringRef Name) {
    const neverd::Symbol *Function = CandidateImage.findSymbol(Name);
    EXPECT_NE(Function, nullptr) << Name.str();
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(CandidateImage.Arch, CandidateImage.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(CandidateImage, Decoder, Function->Addr,
                         Function->Name);
  };
  for (llvm::StringRef Name : {"jt_identity_over_limit_absolute_callback",
                               "jt_identity_over_limit_relative_callback"}) {
    const neverd::LowFunc Overflow = Build(Image, Name);
    EXPECT_TRUE(Overflow.JumpTables.empty()) << Name.str();
    EXPECT_TRUE(lowFunctionHasOpcode(Overflow, neverd::NdOp::INDIR_BR))
        << Name.str();
    EXPECT_FALSE(lowFunctionHasOpcode(Overflow, neverd::NdOp::INDIR_CALL))
        << Name.str();
    EXPECT_FALSE(Overflow.UnsafeIndirectBranchAddresses.empty())
        << Name.str();
  }

  auto SizedImageOrErr = neverd::loadBinary(relativeIdentityObj());
  ASSERT_TRUE(static_cast<bool>(SizedImageOrErr))
      << llvm::toString(SizedImageOrErr.takeError());
  neverd::BinaryImage &SizedImage = *SizedImageOrErr;
  auto AbsoluteSymbol =
      std::find_if(SizedImage.Symbols.begin(), SizedImage.Symbols.end(),
                   [&](const neverd::Symbol &S) {
                     return S.Name ==
                            "jt_identity_over_limit_absolute_table";
                   });
  ASSERT_NE(AbsoluteSymbol, SizedImage.Symbols.end());
  AbsoluteSymbol->Size = (Limit + 1u) * 8u;
  const neverd::LowFunc SizedOverflow =
      Build(SizedImage, "jt_identity_over_limit_absolute_callback");
  EXPECT_TRUE(SizedOverflow.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(SizedOverflow, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(SizedOverflow, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(SizedOverflow.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloRejectedUnsizedMixedTableUsesFullRawRun) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_unsized_mixed_callback");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_unsized_mixed_callback_table");
  const neverd::Symbol *Next =
      Image.findSymbol("jt_modulo_unsized_mixed_next_anchor");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(Next, nullptr);
  EXPECT_EQ(Table->Size, 0u);
  EXPECT_EQ(Next->Addr, Table->Addr + 3u * 8u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JTE_X86_64,
       ModuloRejectedUnsizedInteriorAnchorDoesNotTruncateMixedTable) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_unsized_interior_anchor_mixed_callback");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_modulo_unsized_interior_anchor_mixed_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  EXPECT_EQ(Table->Size, 0u);
  EXPECT_EQ(std::count_if(
                Image.CodePtrRelocSlots.begin(), Image.CodePtrRelocSlots.end(),
                [&](neverd::va_t Slot) {
                  return Slot >= Table->Addr && Slot < Table->Addr + 3u * 8u;
                }),
            3);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64,
       ModuloRejectedUnsizedInteriorAnchorKeepsUnknownTargetOpaque) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_unsized_interior_anchor_unknown_callback");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_modulo_unsized_interior_anchor_unknown_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  EXPECT_EQ(Table->Size, 0u);
  EXPECT_EQ(std::count_if(
                Image.CodePtrRelocSlots.begin(), Image.CodePtrRelocSlots.end(),
                [&](neverd::va_t Slot) {
                  return Slot >= Table->Addr && Slot < Table->Addr + 3u * 8u;
                }),
            3);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloRejectedUnsizedInteriorAnchorKeepsForeignTailCall) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_unsized_interior_anchor_foreign_callback");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_modulo_unsized_interior_anchor_foreign_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  EXPECT_EQ(Table->Size, 0u);
  EXPECT_EQ(std::count_if(
                Image.CodePtrRelocSlots.begin(), Image.CodePtrRelocSlots.end(),
                [&](neverd::va_t Slot) {
                  return Slot >= Table->Addr && Slot < Table->Addr + 3u * 8u;
                }),
            3);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundKeepsIndependentLinearRootsDistinct) {
  auto R = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body = lowFunctionBody(R.out, "jt_modulo_mixed_roots");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_TRUE(Body.find("INDIR_CALL") != std::string::npos ||
              Body.find("INDIR_BR") != std::string::npos)
      << Body;
  expectIndirectDispatchHasNoStaticSuccessors(Body);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_modulo_mixed_roots");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_modulo_mixed_roots");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, ModuloBoundRejectsSignedRemainderAsUnsignedDomain) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_signed_remainder_not_unsigned");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_signed_remainder_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            5);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_SREM));
  EXPECT_TRUE(Low.JumpTables.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsWrongUnsignedDivisionMagic) {
  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody = lowFunctionBody(Low.out, "jt_modulo_wrong_magic");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  expectIndirectDispatchHasNoStaticSuccessors(LowBody);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_modulo_wrong_magic");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_modulo_wrong_magic");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, ModuloBoundRejectsWrongUnsignedDivisionPostShift) {
  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_modulo_wrong_postshift");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  expectIndirectDispatchHasNoStaticSuccessors(LowBody);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_modulo_wrong_postshift");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_modulo_wrong_postshift");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, ModuloBoundAcceptsExactFactorizedSixBackMultiply) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_factorized_six");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 6u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundAcceptsMaskedShiftFactorizedTwelve) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_factorized_twelve_masked_shift");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 12u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundAcceptsExactSixtyFourBitRemainder) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_u64_direct_remainder");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundAcceptsExactSixtyFourBitUnsignedDivision) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_u64_explicit_udiv");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsInexactFactorizedSixRecipes) {
  constexpr const char *Names[] = {
      "jt_modulo_factorized_six_wrong_factor",
      "jt_modulo_factorized_six_wrong_magic",
      "jt_modulo_factorized_six_wrong_postshift",
  };

  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  for (const char *Name : Names) {
    const std::string Body = lowFunctionBody(Low.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << Low.out;
    expectIndirectDispatchHasNoStaticSuccessors(Body);
  }

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  for (const char *Name : Names) {
    const std::string Body = llvmFunctionBody(LLVM.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << LLVM.out;
    EXPECT_EQ(Body.find("switch i"), std::string::npos) << Body;
  }

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  for (const char *Name : Names) {
    const std::string Body = lowFunctionBody(High.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << High.out;
    EXPECT_EQ(Body.find("switch"), std::string::npos) << Body;
  }
}

TEST_F(JTE_X86_64, ModuloBoundAcceptsConstantShiftFactorizedTwelve) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_factorized_twelve_shift");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 12u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsInexactConstantShiftRecipes) {
  constexpr const char *Names[] = {
      "jt_modulo_factorized_twelve_wrong_shift",
      "jt_modulo_factorized_twelve_wrong_multiplier",
      "jt_modulo_factorized_twelve_wrong_width",
  };

  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  for (const char *Name : Names) {
    const std::string Body = lowFunctionBody(Low.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << Low.out;
    expectIndirectDispatchHasNoStaticSuccessors(Body);
  }

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  for (const char *Name : Names) {
    const std::string Body = llvmFunctionBody(LLVM.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << LLVM.out;
    EXPECT_EQ(Body.find("switch i"), std::string::npos) << Body;
  }
}

TEST_F(JTE_X86_64,
       ModuloBoundAuthenticatesExactPreLoadAddAfterScaledDifference) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  auto Recover = [&](const char *Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr) << Name;
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Ordered =
      Recover("jt_modulo_add_after_scaled_difference");
  ASSERT_EQ(Ordered.JumpTables.size(), 1u);
  EXPECT_EQ(Ordered.JumpTables.front().Targets.size(), 7u);

  const neverd::LowFunc Commuted =
      Recover("jt_modulo_add_after_scaled_difference_commuted");
  ASSERT_EQ(Commuted.JumpTables.size(), 1u);
  EXPECT_EQ(Commuted.JumpTables.front().Targets.size(), 7u);

  const neverd::LowFunc Foreign =
      Recover("jt_modulo_add_after_scaled_difference_foreign");
  EXPECT_TRUE(Foreign.JumpTables.empty())
      << "a quotient derived from a different dividend must remain only a "
         "failed modulus proposal";

  const neverd::LowFunc WrongCapacity =
      Recover("jt_modulo_add_after_scaled_difference_wrong_capacity");
  EXPECT_TRUE(WrongCapacity.JumpTables.empty())
      << "a five-slot relocation run cannot authorize a selector whose exact "
         "producer is remainder modulo seven";

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  for (const char *Name : {
           "jt_modulo_add_after_scaled_difference_foreign",
           "jt_modulo_add_after_scaled_difference_wrong_capacity",
       }) {
    const std::string Body = llvmFunctionBody(LLVM.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << "\n" << LLVM.out;
    EXPECT_EQ(Body.find("switch i"), std::string::npos) << Name << "\n" << Body;
  }
}

TEST_F(JTE_X86_64, ModuloBoundFinalReplayRequiresReachableProducer) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_constant_selector_unreachable_rem");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_unreachable_rem_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            5)
      << "the regression requires five physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawUnsignedRemainder = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      SawUnsignedRemainder |= Op.Opcode == neverd::NdOp::INT_REM;
  ASSERT_TRUE(SawUnsignedRemainder)
      << "the dead lexical block must still propose an unsigned remainder";
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a literal selector must not borrow five cases from an unreachable "
         "INT_REM occurrence";
}

TEST_F(JTE_X86_64, ModuloBoundUsesCandidateLocalLeastFixedPoint) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  auto Build = [&](const char *Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr) << Name;
    if (!Function)
      return neverd::LowFunc{};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Positive =
      Build("jt_modulo_lfp_late_reachable_producer");
  size_t AddCount = 0;
  for (const neverd::LowBlock &Block : Positive.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      AddCount += Op.Opcode == neverd::NdOp::INT_ADD;
  ASSERT_GT(AddCount, 512u)
      << "the producer must sit beyond the flat proposal prefix";
  ASSERT_EQ(Positive.JumpTables.size(), 1u);
  EXPECT_EQ(Positive.JumpTables.front().Targets.size(), 5u);
  EXPECT_TRUE(Positive.UnsafeIndirectBranchAddresses.empty());

  const neverd::LowFunc Negative =
      Build("jt_modulo_lfp_rejects_self_bootstrap");
  EXPECT_TRUE(Negative.JumpTables.empty())
      << "a producer in an unauthorized case must not open its own edge";
  EXPECT_TRUE(lowFunctionHasOpcode(Negative, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Negative, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Negative.EverPublishedJumpTableBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsLateInteriorPredecessor) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_lfp_rejects_late_interior_predecessor");
  ASSERT_NE(Function, nullptr);
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc InteriorPredecessor =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(InteriorPredecessor.JumpTables.empty())
      << "a late candidate edge entering the middle of a producer must "
         "invalidate its exact modulo recipe at fixed-point closure";
  EXPECT_TRUE(
      InteriorPredecessor.EverPublishedJumpTableBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundRejectsLateSelectorEscape) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_lfp_rejects_late_selector_escape");
  ASSERT_NE(Function, nullptr);
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a valid modulo producer cannot hide an authorized case that "
         "writes an out-of-domain selector";
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.EverPublishedJumpTableBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundDecodesTargetsBeforeFixedPointPublication) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol(
      "jt_modulo_lfp_rejects_undecoded_late_escape");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_lfp_relative_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 4u);

  const size_t AbsoluteCodePointers =
      std::count_if(Image.CodePtrRelocSlots.begin(),
                    Image.CodePtrRelocSlots.end(), [&](neverd::va_t Slot) {
                      return Slot >= Table->Addr &&
                             Slot < Table->Addr + Table->Size;
                    });
  ASSERT_EQ(AbsoluteCodePointers, 0u)
      << "case destinations must not be decoded as address-taken roots";
  const size_t RelativeCodeSlots =
      std::count_if(Image.RelCodeRelocSlots.begin(),
                    Image.RelCodeRelocSlots.end(), [&](neverd::va_t Slot) {
                      return Slot >= Table->Addr &&
                             Slot < Table->Addr + Table->Size;
                    });
  ASSERT_EQ(RelativeCodeSlots, 5u)
      << "the graph-growth regression requires an exact physical capacity";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a closed numeric domain cannot publish before every authorized "
         "destination has entered the immutable resolver graph";
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.EverPublishedJumpTableBranchAddresses.empty());
}

TEST_F(JTE_X86_64, ModuloBoundGrowsRelativeTargetsBeforePublication) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_lfp_relative_entry_positive");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_lfp_relative_positive_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 4u);
  ASSERT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            0u);
  ASSERT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            5u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64,
       ModuloFixedPointEvidenceBudgetExhaustionIsTransactionalAndOpaque) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_lfp_relative_entry_positive");
  ASSERT_NE(Function, nullptr);

  auto BuildWithBudget = [&](std::optional<size_t> Budget,
                             bool *HasPendingExploration = nullptr) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    if (HasPendingExploration)
      *HasPendingExploration =
          Builder.hasMaskFixedPointExplorationTargetsForTesting();
    return Low;
  };
  auto Recovered = [](const neverd::LowFunc &Low) {
    return Low.JumpTables.size() == 1u &&
           Low.JumpTables.front().Targets.size() == 5u;
  };

  const neverd::LowFunc Full = BuildWithBudget(std::nullopt);
  ASSERT_TRUE(Recovered(Full));
  const neverd::va_t Branch = Full.JumpTables.front().InsnAddr;

  bool ZeroHasPendingExploration = true;
  const neverd::LowFunc ZeroBudget =
      BuildWithBudget(size_t{0}, &ZeroHasPendingExploration);
  EXPECT_TRUE(ZeroBudget.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(ZeroBudget, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(ZeroBudget, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(ZeroBudget.UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_FALSE(ZeroHasPendingExploration)
      << "zero budget must not retain provisional modulo targets";

  size_t Lo = 1;
  size_t Hi = neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork;
  while (Lo < Hi) {
    const size_t Mid = Lo + (Hi - Lo) / 2;
    if (Recovered(BuildWithBudget(Mid)))
      Hi = Mid;
    else
      Lo = Mid + 1;
  }
  const size_t MinimumSuccessfulBudget = Lo;
  ASSERT_GT(MinimumSuccessfulBudget, 1u);
  EXPECT_TRUE(Recovered(BuildWithBudget(MinimumSuccessfulBudget)));

  bool HasPendingExploration = true;
  const neverd::LowFunc Exhausted = BuildWithBudget(
      MinimumSuccessfulBudget - 1, &HasPendingExploration);
  EXPECT_TRUE(Exhausted.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Exhausted.UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_FALSE(HasPendingExploration)
      << "budget exhaustion must roll back provisional modulo targets";
}

TEST_F(JTE_X86_64, ModuloBoundSharedDAGStaysWithinEvidenceBudget) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_shared_dag_budget");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_shared_dag_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  size_t AddCount = 0;
  size_t UnsignedRemainderCount = 0;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      AddCount += Op.Opcode == neverd::NdOp::INT_ADD;
      UnsignedRemainderCount += Op.Opcode == neverd::NdOp::INT_REM;
    }
  ASSERT_GE(AddCount, 12u)
      << "the regression requires the shared doubling proposal DAG";
  ASSERT_GE(UnsignedRemainderCount, 2u)
      << "the regression requires an unrelated exact large modulus before "
         "the real small producer";
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
}

TEST(JumpTableProofPointTest, AnyDuplicateOccurrenceIsPermanentlyAmbiguous) {
  std::map<neverd::detail::JumpTableProofPoint,
           neverd::detail::JumpTableProofLocation>
      Unique;
  std::set<neverd::detail::JumpTableProofPoint> Ambiguous;
  const neverd::detail::JumpTableProofPoint Point{0x1234, 2};

  EXPECT_TRUE(neverd::detail::recordUniqueJumpTableProofPoint(
      Unique, Ambiguous, Point, {/*Block=*/1, /*Op=*/3}));
  ASSERT_EQ(Unique.count(Point), 1u);
  EXPECT_TRUE(Ambiguous.empty());

  EXPECT_FALSE(neverd::detail::recordUniqueJumpTableProofPoint(
      Unique, Ambiguous, Point, {/*Block=*/1, /*Op=*/4}));
  EXPECT_EQ(Unique.count(Point), 0u);
  EXPECT_EQ(Ambiguous.count(Point), 1u);

  EXPECT_FALSE(neverd::detail::recordUniqueJumpTableProofPoint(
      Unique, Ambiguous, Point, {/*Block=*/2, /*Op=*/0}));
  EXPECT_EQ(Unique.count(Point), 0u)
      << "a third insertion must not resurrect a last-wins proof point";
}

TEST_F(JTE_X86_64, ModuloBoundProbesCompactCandidatesIndependently) {
  auto ImageOrErr = neverd::loadBinary(moduloDomainObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_modulo_compact_probe_isolated");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_modulo_compact_probe_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 4u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            0)
      << "the compact-table regression requires no relocation capacity";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  size_t UnsignedRemainderCount = 0;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      UnsignedRemainderCount += Op.Opcode == neverd::NdOp::INT_REM;
  ASSERT_GE(UnsignedRemainderCount, 2u)
      << "the fixture requires exact `% 5` and `% 7` proposals";
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
}

TEST_F(JTE_X86_64, GuardDomainReplaysAfterForeignRootIsRestored) {
  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_guard_domain_replays_after_root_restore");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  expectIndirectDispatchHasNoStaticSuccessors(LowBody);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_guard_domain_replays_after_root_restore");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_guard_domain_replays_after_root_restore");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64,
       AbsoluteRelocationCapacityDoesNotAuthenticateRawIndexDomain) {
  auto Low = liftToLowIR(moduloDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_raw_absolute_capacity");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  expectIndirectDispatchHasNoStaticSuccessors(LowBody);

  auto LLVM = liftToLLVMIRUnopt(moduloDomainObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_raw_absolute_capacity");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(moduloDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_raw_absolute_capacity");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64,
       RelativeRelocationCapacityDoesNotAuthenticateRawIndexDomain) {
  auto Low = liftToLowIR(rawRelativeDomainObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_raw_relative_capacity");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  expectIndirectDispatchHasNoStaticSuccessors(LowBody);

  auto High = liftToHighIR(rawRelativeDomainObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_raw_relative_capacity");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;

  // The LLVM backend may conservatively reject the unresolved relocatable
  // target instead of materializing an opaque indirect tail call.  Either
  // outcome is fail-closed, but neither may synthesize a static switch.
  auto LLVM = liftToLLVMIRUnopt(rawRelativeDomainObj());
  if (LLVM.exitCode == 0) {
    const std::string LLVMBody =
        llvmFunctionBody(LLVM.out, "jt_raw_relative_capacity");
    ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
    EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  } else {
    EXPECT_NE(LLVM.err.find("refusing stale-address fallback"),
              std::string::npos)
        << LLVM.err;
    EXPECT_EQ(LLVM.out.find("switch i"), std::string::npos) << LLVM.out;
  }
}

TEST_F(JTE_X86_64, MaskBoundUsesExactPreLoadValueLifetime) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body =
      lowFunctionBody(R.out, "jt_identity_postload_mask_lifetime");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x834:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x835:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x836:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x837:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundAcceptsDenseBoundedMerge) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_dense_bounded_merge");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 4u);
  EXPECT_EQ(Low.JumpTables.front().CaseLabels,
            (std::vector<int64_t>{0, 1, 2, 3}));
}

TEST_F(JTE_X86_64, MaskBoundReplaysTightenedDenseMerge) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_guard5_mask4_dense_merge");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 4u);
  EXPECT_EQ(Low.JumpTables.front().CaseLabels,
            (std::vector<int64_t>{0, 1, 2, 3}));
}

TEST_F(JTE_X86_64, DeadMaskDependencyDoesNotOverrideModuloDomain) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_dead_mask_dependency_mod5");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_dead_mask_dependency_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 32u * 4u);
  EXPECT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            32)
      << "the regression requires one continuous 32-slot relocation run";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  bool SawMask31 = false;
  bool SawMultiplyByZero = false;
  bool SawRemainder = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      auto HasScalar = [&](uint64_t Value) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value)
            return true;
        return false;
      };
      SawMask31 |= Op.Opcode == neverd::NdOp::INT_AND && HasScalar(31);
      SawMultiplyByZero |=
          Op.Opcode == neverd::NdOp::INT_MULT && HasScalar(0);
      SawRemainder |= Op.Opcode == neverd::NdOp::INT_REM;
    }
  ASSERT_TRUE(SawMask31);
  ASSERT_TRUE(SawMultiplyByZero);
  ASSERT_TRUE(SawRemainder);

  ASSERT_LE(Low.JumpTables.size(), 1u);
  if (!Low.JumpTables.empty()) {
    EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u)
        << "x&31 is erased by multiplication by zero and cannot widen the "
           "authenticated modulo domain";
    EXPECT_EQ(Low.JumpTables.front().CaseLabels,
              (std::vector<int64_t>{0, 1, 2, 3, 4}));
  }
}

TEST_F(JTE_X86_64, MaskBoundDistinguishesStackPointerEpochs) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_frame_epoch_mask");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x898:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x899:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8F4:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8F9:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundAuthenticatesOffsetTransform) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_offset");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0x961:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0x968:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x9C3:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsUnprovedNegativeOffsetDomain) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_negative_offset");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x9C4:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xA27:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundClosesMultipleCheckedOffsets) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_double_offset");
  ASSERT_FALSE(Body.empty()) << R.out;
  // `(x & 3) + 1 + 1` reaches slots 2..5 only.  Do not require the two
  // unreachable prefix slots merely because they are readable table bytes.
  EXPECT_EQ(Body.find("cst:0xA28:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xA29:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xA2A:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xA2D:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xA8B:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsUnknownOffsetDependency) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_unknown_offset");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xA8C:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xAEF:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsRuntimeMaskDependency) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_runtime_mask");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xAF0:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xB53:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsNarrowArithmeticWrap) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_byte_wrap");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xB54:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xBB7:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundPreservesPartialRegisterDependencies) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_partial_wide");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xBB8:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xBB9:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xC1B:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundIntersectsNestedMaskDomains) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body =
      lowFunctionBody(R.out, "jt_identity_nested_mask_intersection");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("cst:0xC1C:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xC1D:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xC78:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xC7D:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundIntersectsTranslatedNestedMergeDomains) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_nested_mask_offset_merge");
  ASSERT_NE(Function, nullptr);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  ASSERT_EQ(Low.JumpTables.size(), 1u);
  ASSERT_EQ(Low.JumpTables.front().Targets.size(), 2u);
  EXPECT_EQ(Low.JumpTables.front().CaseLabels,
            (std::vector<int64_t>{1, 2}));
}

TEST_F(JTE_X86_64, MaskRawDenseNestedPreciseReplayFailsClosed) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_nested_mask_offset_merge");
  ASSERT_NE(Function, nullptr);

  auto BuildWithBudget = [&](std::optional<size_t> Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };

  const neverd::LowFunc Full = BuildWithBudget(std::nullopt);
  ASSERT_EQ(Full.JumpTables.size(), 1u);
  ASSERT_EQ(Full.JumpTables.front().Targets.size(), 2u);
  const neverd::va_t Branch = Full.JumpTables.front().InsnAddr;

  // The capacity-shaped outer mask schedules an eight-slot raw batch, while
  // the nested translated mask proves only cases {1,2}.  Sweep bounded
  // allowances across the raw-to-precise handoff: an unfinished precise replay
  // may remain opaque or eventually recover the exact two cases, but must never
  // fall through and publish the outer eight-slot capacity.  Restoring the old
  // Bounds.cpp fallthrough after a failed/incomplete precise replay makes an
  // intermediate allowance publish eight targets and turns this test RED.
  constexpr size_t Budgets[] = {
      8192,  16384, 32768, 65536,
      131072, 262144,
      neverd::limits::kMaxJumpTableMaskFixedPointEvidenceWork};
  bool SawOpaque = false;
  bool SawExact = false;
  for (size_t Budget : Budgets) {
    const neverd::LowFunc Low = BuildWithBudget(Budget);
    if (Low.JumpTables.empty()) {
      SawOpaque = true;
      EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
      EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
      EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(Branch), 1u);
      continue;
    }

    ASSERT_EQ(Low.JumpTables.size(), 1u) << "budget=" << Budget;
    EXPECT_EQ(Low.JumpTables.front().Targets.size(), 2u)
        << "raw capacity widened a failed precise replay; budget=" << Budget;
    EXPECT_EQ(Low.JumpTables.front().CaseLabels,
              (std::vector<int64_t>{1, 2}))
        << "budget=" << Budget;
    SawExact = true;
  }
  EXPECT_TRUE(SawOpaque)
      << "the sweep must cross an incomplete precise-replay boundary";
  EXPECT_TRUE(SawExact)
      << "the largest allowance must recover the exact domain";
}

TEST_F(JTE_X86_64,
       MaskFixedPointEvidenceBudgetExhaustionIsTransactionalAndOpaque) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_nested_mask_offset_merge");
  ASSERT_NE(Function, nullptr);

  auto BuildWithBudget = [&](std::optional<size_t> Budget,
                             bool *HasPendingExploration = nullptr) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    Builder.setMaskFixedPointEvidenceBudgetForTesting(Budget);
    neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    if (HasPendingExploration)
      *HasPendingExploration =
          Builder.hasMaskFixedPointExplorationTargetsForTesting();
    return Low;
  };

  const neverd::LowFunc Full = BuildWithBudget(std::nullopt);
  ASSERT_EQ(Full.JumpTables.size(), 1u);
  const neverd::va_t Branch = Full.JumpTables.front().InsnAddr;

  // This allowance pays the physical-prefix/seed/core work and recovers the
  // table if resolver-graph charging is removed.  With complete accounting it
  // is exhausted inside graph/value evidence, so this locks the transactional
  // fail-closed boundary instead of failing before the code under test runs.
  bool HasPendingExploration = true;
  const neverd::LowFunc Exhausted =
      BuildWithBudget(size_t{8192}, &HasPendingExploration);
  EXPECT_TRUE(Exhausted.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Exhausted.UnsafeIndirectBranchAddresses.count(Branch), 1u);
  EXPECT_FALSE(HasPendingExploration)
      << "budget exhaustion must not retain provisional case targets";
}

TEST_F(JTE_X86_64, MaskFixedPointRejectsSelfBootstrappedCaseMasks) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_fp_self_bootstrap");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_fp_self_bootstrap_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the regression requires eight physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawMask7 = false;
  const neverd::LowBlock *Dispatch = nullptr;
  neverd::va_t DispatchAddr = neverd::InvalidVA;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Opcode == neverd::NdOp::INT_AND)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          SawMask7 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 7;
      if (Op.Opcode == neverd::NdOp::INDIR_BR) {
        ASSERT_EQ(Dispatch, nullptr)
            << "the fixture must contain one indirect dispatch";
        Dispatch = &Block;
        DispatchAddr = Op.Addr;
      }
    }
  ASSERT_TRUE(SawMask7)
      << "the case-local x&7 producer must be present in LowIR";
  ASSERT_NE(Dispatch, nullptr);
  EXPECT_TRUE(Dispatch->Succs.empty());
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(DispatchAddr), 1u);
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty());
}

TEST_F(JTE_X86_64, MaskFixedPointRejectsLateOutOfDomainSelector) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_fp_late_escape");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_fp_late_escape_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the regression requires eight physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawMask7 = false;
  bool SawLiteral15 = false;
  const neverd::LowBlock *Dispatch = nullptr;
  neverd::va_t DispatchAddr = neverd::InvalidVA;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Opcode == neverd::NdOp::INT_AND)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          SawMask7 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 7;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawLiteral15 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 15;
      if (Op.Opcode == neverd::NdOp::INDIR_BR) {
        ASSERT_EQ(Dispatch, nullptr)
            << "the fixture must contain one indirect dispatch";
        Dispatch = &Block;
        DispatchAddr = Op.Addr;
      }
    }
  ASSERT_TRUE(SawMask7)
      << "case zero must expose the eight-value mask domain";
  ASSERT_TRUE(SawLiteral15)
      << "case seven must expose the late out-of-domain selector";
  ASSERT_NE(Dispatch, nullptr);
  EXPECT_TRUE(Dispatch->Succs.empty());
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(DispatchAddr), 1u);
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty());
}

TEST_F(JTE_X86_64, MaskFixedPointReplaysCompleteEntryDomain) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_fp_entry_bound_late_escape");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_identity_mask_fp_entry_bound_late_escape_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the regression requires eight physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawMask7 = false;
  bool SawLiteral15 = false;
  const neverd::LowBlock *Dispatch = nullptr;
  neverd::va_t DispatchAddr = neverd::InvalidVA;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Opcode == neverd::NdOp::INT_AND)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          SawMask7 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 7;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawLiteral15 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 15;
      if (Op.Opcode == neverd::NdOp::INDIR_BR) {
        ASSERT_EQ(Dispatch, nullptr)
            << "the fixture must contain one indirect dispatch";
        Dispatch = &Block;
        DispatchAddr = Op.Addr;
      }
    }
  ASSERT_TRUE(SawMask7) << "entry must expose the initial eight-value domain";
  ASSERT_TRUE(SawLiteral15)
      << "case seven must expose the post-edge out-of-domain selector";
  ASSERT_NE(Dispatch, nullptr);
  EXPECT_TRUE(Dispatch->Succs.empty());
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(DispatchAddr), 1u);
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty());
}

TEST_F(JTE_X86_64, MaskFixedPointRejectsPrescaledBackedgeWidening) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_fp_prescaled_late_escape");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_fp_prescaled_late_escape_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the regression requires eight physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawMask56 = false;
  bool SawLiteral120 = false;
  const neverd::LowBlock *Dispatch = nullptr;
  neverd::va_t DispatchAddr = neverd::InvalidVA;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      if (Op.Opcode == neverd::NdOp::INT_AND)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          SawMask56 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 56;
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawLiteral120 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 120;
      if (Op.Opcode == neverd::NdOp::INDIR_BR) {
        ASSERT_EQ(Dispatch, nullptr)
            << "the fixture must contain one indirect dispatch";
        Dispatch = &Block;
        DispatchAddr = Op.Addr;
      }
    }
  ASSERT_TRUE(SawMask56)
      << "entry must expose the initial pre-scaled eight-value domain";
  ASSERT_TRUE(SawLiteral120)
      << "case seven must expose the widened pre-scaled selector";
  EXPECT_TRUE(Low.JumpTables.empty());
  if (Dispatch) {
    EXPECT_TRUE(Dispatch->Succs.empty());
    EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(DispatchAddr), 1u);
  } else {
    EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
    EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::RETURN));
  }
}

TEST_F(JTE_X86_64,
       SiblingCandidatesPublishFromOneStageSnapshotWithOwnOccurrences) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  struct ExpectedSibling {
    const char *BeginName;
    const char *EndName;
  };

  auto CheckLayout = [&](const char *FunctionName, const char *TableName,
                         const std::vector<const char *> &TargetNames,
                         const std::vector<ExpectedSibling> &AddressOrder) {
    const neverd::Symbol *Function = Image.findSymbol(FunctionName);
    const neverd::Symbol *Table = Image.findSymbol(TableName);
    ASSERT_NE(Function, nullptr) << FunctionName;
    ASSERT_NE(Table, nullptr) << TableName;
    ASSERT_EQ(TargetNames.size(), 4u);
    ASSERT_EQ(AddressOrder.size(), 5u);

    std::vector<const neverd::Symbol *> Targets;
    for (const char *TargetName : TargetNames) {
      const neverd::Symbol *Target = Image.findSymbol(TargetName);
      ASSERT_NE(Target, nullptr) << TargetName;
      Targets.push_back(Target);
    }

    std::vector<const neverd::Symbol *> Begins;
    std::vector<const neverd::Symbol *> Ends;
    for (const ExpectedSibling &Expected : AddressOrder) {
      const neverd::Symbol *Begin = Image.findSymbol(Expected.BeginName);
      const neverd::Symbol *End = Image.findSymbol(Expected.EndName);
      ASSERT_NE(Begin, nullptr) << Expected.BeginName;
      ASSERT_NE(End, nullptr) << Expected.EndName;
      ASSERT_LT(Begin->Addr, End->Addr);
      if (!Ends.empty())
        ASSERT_LE(Ends.back()->Addr, Begin->Addr)
            << "the mirror must reverse only the sibling address order";
      Begins.push_back(Begin);
      Ends.push_back(End);
    }

    ASSERT_EQ(Table->Size, 4u * 8u);
    EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                            Image.CodePtrRelocSlots.end(),
                            [&](neverd::va_t Slot) {
                              return Slot >= Table->Addr &&
                                     Slot < Table->Addr + Table->Size;
                            }),
              4)
        << "the fixture requires four conditional code-root relocations";

    neverd::Decoder Decoder;
    ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    const neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    ASSERT_EQ(Low.JumpTables.size(), AddressOrder.size())
        << FunctionName
        << " must publish the entry and all four cyclic siblings as one "
           "stage batch";
    EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL))
        << "a lost sibling must not be reclassified as an indirect tail call";
    EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty())
        << "every cyclic sibling has a complete four-value domain";

    auto FindIn = [&](const neverd::Symbol &Begin,
                      const neverd::Symbol &End)
        -> const neverd::JumpTable * {
      const neverd::JumpTable *Found = nullptr;
      for (const neverd::JumpTable &JT : Low.JumpTables) {
        if (JT.InsnAddr < Begin.Addr || JT.InsnAddr >= End.Addr)
          continue;
        if (Found)
          return nullptr;
        Found = &JT;
      }
      return Found;
    };

    std::set<std::pair<neverd::va_t, int>> SelectorOccurrences;
    std::set<std::pair<neverd::va_t, int>> LoadOccurrences;
    std::set<const neverd::JumpTable *> SeenTables;
    for (size_t I = 0; I < AddressOrder.size(); ++I) {
      const ExpectedSibling &Expected = AddressOrder[I];
      const neverd::Symbol &Begin = *Begins[I];
      const neverd::Symbol &End = *Ends[I];
      const neverd::JumpTable *JT = FindIn(Begin, End);
      ASSERT_NE(JT, nullptr) << Expected.BeginName;
      ASSERT_TRUE(SeenTables.insert(JT).second)
          << "one branch cannot stand in for a sibling candidate";

      EXPECT_EQ(JT->BaseAddr, Table->Addr);
      EXPECT_TRUE(JT->HasBaseAddr);
      ASSERT_EQ(JT->Targets.size(), Targets.size())
          << "every sibling must publish its own complete four-value domain";
      for (const neverd::Symbol *Target : Targets)
        EXPECT_EQ(std::count(JT->Targets.begin(), JT->Targets.end(),
                             Target->Addr),
                  1)
            << "the candidate graph must retain every sibling consumer";

      ASSERT_EQ(JT->SelectorUseRefs.size(), 1u)
          << "a sibling JumpTableInfo must not import the other branch's "
             "selector alternatives";
      const neverd::JumpTableSelectorUseRef &Selector =
          JT->SelectorUseRefs.front();
      EXPECT_GE(Selector.Addr, Begin.Addr);
      EXPECT_LT(Selector.Addr, End.Addr)
          << "selector occurrence escaped its owning sibling";
      EXPECT_NE(findExactLowOp(Low, Selector.Addr, Selector.Seq,
                               Selector.ExpectedOpcode),
                nullptr);
      EXPECT_TRUE(
          SelectorOccurrences.insert({Selector.Addr, Selector.Seq}).second)
          << "numeric register equality is not exact occurrence identity";

      ASSERT_EQ(JT->AuthenticatedTableLoads.size(), 1u)
          << "a sibling JumpTableInfo must retain only its own table LOAD";
      const neverd::JumpTableOpOccurrence &Load =
          JT->AuthenticatedTableLoads.front();
      EXPECT_GE(Load.Addr, Begin.Addr);
      EXPECT_LT(Load.Addr, End.Addr)
          << "authenticated LOAD occurrence escaped its owning sibling";
      EXPECT_NE(
          findExactLowOp(Low, Load.Addr, Load.Seq, neverd::NdOp::LOAD),
          nullptr);
      EXPECT_TRUE(LoadOccurrences.insert({Load.Addr, Load.Seq}).second)
          << "shared storage does not make sibling LOAD occurrences aliases";
    }
  };

  CheckLayout(
      "jt_identity_sibling_snapshot_narrow_first",
      "jt_identity_sibling_nf_table",
      {"jt_identity_sibling_nf_t0_begin",
       "jt_identity_sibling_nf_t1_begin",
       "jt_identity_sibling_nf_t2_begin",
       "jt_identity_sibling_nf_t3_begin"},
      {{"jt_identity_sibling_nf_entry_begin",
        "jt_identity_sibling_nf_entry_end"},
       {"jt_identity_sibling_nf_t0_begin", "jt_identity_sibling_nf_t0_end"},
       {"jt_identity_sibling_nf_t1_begin", "jt_identity_sibling_nf_t1_end"},
       {"jt_identity_sibling_nf_t2_begin", "jt_identity_sibling_nf_t2_end"},
       {"jt_identity_sibling_nf_t3_begin", "jt_identity_sibling_nf_t3_end"}});
  CheckLayout(
      "jt_identity_sibling_snapshot_wide_first",
      "jt_identity_sibling_wf_table",
      {"jt_identity_sibling_wf_t0_begin",
       "jt_identity_sibling_wf_t1_begin",
       "jt_identity_sibling_wf_t2_begin",
       "jt_identity_sibling_wf_t3_begin"},
      {{"jt_identity_sibling_wf_entry_begin",
        "jt_identity_sibling_wf_entry_end"},
       {"jt_identity_sibling_wf_t3_begin", "jt_identity_sibling_wf_t3_end"},
       {"jt_identity_sibling_wf_t2_begin", "jt_identity_sibling_wf_t2_end"},
       {"jt_identity_sibling_wf_t1_begin", "jt_identity_sibling_wf_t1_end"},
       {"jt_identity_sibling_wf_t0_begin", "jt_identity_sibling_wf_t0_end"}});
}

TEST_F(JTE_X86_64, MaskDomainMustFitAuthenticatedPhysicalStorage) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_exceeds_storage");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskDomainCannotTrustCallablePhysicalPrefix) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_exceeds_callback_storage");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_exceeds_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.size(), 1u);
}

TEST_F(JTE_X86_64, NonContiguousMaskPreservesPhysicalSlotCoordinates) {
  auto R = liftToLLVMIR(maskEqualBoundObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body =
      llvmFunctionBody(R.out, "jt_identity_mask_equal_bound_coordinate");
  ASSERT_FALSE(Body.empty()) << R.out;
  const size_t Case2 = Body.find("i32 2, label %");
  ASSERT_NE(Case2, std::string::npos) << Body;
  const size_t BlockBegin = Body.find('%', Case2);
  const size_t BlockEnd = Body.find_first_of(" \r\n", BlockBegin);
  ASSERT_NE(BlockBegin, std::string::npos) << Body;
  ASSERT_NE(BlockEnd, std::string::npos) << Body;
  const std::string Case2Block = Body.substr(BlockBegin, BlockEnd - BlockBegin);
  EXPECT_NE(Body.find("[ 3302, " + Case2Block + " ]"), std::string::npos)
      << "selector 2 must route to physical slot 2, not compressed slot 1:\n"
      << Body;
  EXPECT_EQ(Body.find("[ 3301, " + Case2Block + " ]"), std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("i32 60, label"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, SparseMaskDoesNotOwnIndependentRelocationGap) {
  auto Low = liftToLowIR(maskEqualBoundObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_identity_sparse_mask_gap_root");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("cst:0xD48:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xD4A:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xDAB:4"), std::string::npos)
      << "the relocation in physical gap slot 1 must remain an independent "
         "code root:\n"
      << LowBody;

  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_sparse_mask_gap_root");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  const bool HasExactSwitch =
      (LLVMBody.find("i64 0, label %") != std::string::npos &&
       LLVMBody.find("i64 2, label %") != std::string::npos) ||
      (LLVMBody.find("i32 0, label %") != std::string::npos &&
       LLVMBody.find("i32 2, label %") != std::string::npos);
  const bool HasEquivalentFoldedSelect =
      LLVMBody.find("or disjoint i32 %and, 3400") != std::string::npos;
  EXPECT_TRUE(HasExactSwitch || HasEquivalentFoldedSelect) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("i64 1, label %"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("i32 1, label %"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVM.out.find("__nd_codeptr_"), std::string::npos)
      << "the independent relocation gap must remain in the code-pointer "
         "mirror:\n"
      << LLVM.out;
  EXPECT_NE(LLVM.out.find("blockaddress(@jt_identity_sparse_mask_gap_root"),
            std::string::npos)
      << "the independently referenced gap target must stay relocatable:\n"
      << LLVM.out;
}

TEST_F(JTE_X86_64, SparseMaskPreservesUnclassifiedExecutableRelocation) {
  auto Low = liftToLowIR(maskEqualBoundObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string Body =
      lowFunctionBody(Low.out, "jt_identity_sparse_mask_dead_consumer");
  ASSERT_FALSE(Body.empty()) << Low.out;
  EXPECT_NE(Body.find("cst:0xDAC:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xDAE:4"), std::string::npos) << Body;
  EXPECT_NE(Body.find("cst:0xE0F:4"), std::string::npos)
      << "an unexplored relocation in executable bytes is indistinguishable "
         "from inline data and must be preserved:\n"
      << Body;

  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_sparse_mask_dead_consumer");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 0)) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 2)) << LLVMBody;
  EXPECT_FALSE(llvmHasSwitchCase(LLVMBody, 1)) << LLVMBody;

  const size_t GapValue = LLVMBody.find("3599");
  ASSERT_NE(GapValue, std::string::npos) << LLVMBody;
  const size_t GapBlockLine = LLVMBody.rfind("\nbb_", GapValue);
  ASSERT_NE(GapBlockLine, std::string::npos) << LLVMBody;
  const size_t GapBlockEnd = LLVMBody.find(':', GapBlockLine + 1);
  ASSERT_NE(GapBlockEnd, std::string::npos) << LLVMBody;
  const std::string GapBlock =
      LLVMBody.substr(GapBlockLine + 1, GapBlockEnd - GapBlockLine - 1);
  EXPECT_NE(
      LLVM.out.find("blockaddress(@jt_identity_sparse_mask_dead_consumer, %" +
                    GapBlock + ")"),
      std::string::npos)
      << "the exact 3599 gap target must remain in the relocation mirror:\n"
      << LLVM.out;
}

TEST_F(JTE_X86_64, PCRelativeNonTrailingDisplacementUsesDecodedInstructionEnd) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *FunctionSym =
      Image.findSymbol("jt_identity_pcrel_nontrailing");
  const neverd::Symbol *DataSym =
      Image.findSymbol("jt_identity_pcrel_nontrailing_data");
  ASSERT_NE(FunctionSym, nullptr);
  ASSERT_NE(DataSym, nullptr);

  auto FieldIt = std::find_if(
      Image.DataAddressRelocOperands.begin(),
      Image.DataAddressRelocOperands.end(), [&](const auto &Entry) {
        return Entry.first >= FunctionSym->Addr &&
               Entry.first < FunctionSym->Addr + FunctionSym->Size &&
               Entry.second.PCRelativeFromInstructionEnd;
      });
  ASSERT_NE(FieldIt, Image.DataAddressRelocOperands.end());
  const neverd::va_t FieldVA = FieldIt->first;
  EXPECT_EQ(FieldIt->second.TargetVA, neverd::InvalidVA);
  EXPECT_EQ(Image.RelocDataAddrs.count(DataSym->Addr), 0u);
  EXPECT_EQ(Image.WritableRelocDataAddrs.count(DataSym->Addr), 0u);
  EXPECT_EQ(Image.RelocDataAddrs.count(DataSym->Addr - 1), 0u);
  EXPECT_EQ(Image.WritableRelocDataAddrs.count(DataSym->Addr - 1), 0u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Function = Builder.build(
      Image, Decoder, FunctionSym->Addr, "jt_identity_pcrel_nontrailing");
  const auto Occurrence = std::find_if(
      Function.RelocatedInstructionAddressOccurrences.begin(),
      Function.RelocatedInstructionAddressOccurrences.end(),
      [&](const neverd::RelocatedInstructionAddressOccurrence &Item) {
        return Item.FieldVA == FieldVA;
      });
  ASSERT_NE(Occurrence, Function.RelocatedInstructionAddressOccurrences.end());
  EXPECT_EQ(Occurrence->TargetVA, DataSym->Addr);
  EXPECT_EQ(Occurrence->Provenance,
            neverd::ConstantAddressProvenance::DataAddress);

  bool SawExactDataAddress = false;
  for (const neverd::LowBlock &Block : Function.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        SawExactDataAddress |=
            Op.Inputs[I].isConst() && Op.Inputs[I].Offset == DataSym->Addr &&
            Op.Inputs[I].Provenance ==
                neverd::ConstantAddressProvenance::DataAddress;
  EXPECT_TRUE(SawExactDataAddress);

  // Corrupt the encoded displacement so the decoded target escapes the
  // relocation's authenticated owner.  Matching bits/field location alone
  // must not attach that owner's exact address provenance.
  const neverd::Section *DataOwner = Image.getSectionFor(DataSym->Addr);
  ASSERT_NE(DataOwner, nullptr);
  ASSERT_LE(FunctionSym->Addr + 7, neverd::InvalidVA);
  ASSERT_LE(DataOwner->VA + DataOwner->Size + 0x100, neverd::InvalidVA);
  const neverd::va_t EscapedTarget = DataOwner->VA + DataOwner->Size + 0x100;
  const int64_t EscapedDisp = static_cast<int64_t>(EscapedTarget) -
                              static_cast<int64_t>(FunctionSym->Addr + 7);
  const uint32_t EncodedEscapedDisp = static_cast<uint32_t>(EscapedDisp);
  ASSERT_TRUE(Image.writeVA(
      FieldVA, reinterpret_cast<const uint8_t *>(&EncodedEscapedDisp),
      sizeof(EncodedEscapedDisp)));
  FieldIt->second.EncodedValue = EncodedEscapedDisp;

  neverd::Decoder CorruptDecoder;
  ASSERT_TRUE(CorruptDecoder.init(neverd::Arch::X64));
  neverd::CFGBuilder CorruptBuilder;
  const neverd::LowFunc Corrupt =
      CorruptBuilder.build(Image, CorruptDecoder, FunctionSym->Addr,
                           "jt_identity_pcrel_nontrailing_corrupt");
  EXPECT_TRUE(Corrupt.RelocatedInstructionAddressOccurrences.empty());
  for (const neverd::LowBlock &Block : Corrupt.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      for (uint8_t I = 0; I < Op.NumInputs; ++I)
        EXPECT_FALSE(Op.Inputs[I].isConst() &&
                     Op.Inputs[I].Offset == EscapedTarget &&
                     neverd::isExactAddressProvenance(Op.Inputs[I].Provenance));
}

TEST_F(JTE_X86_64, PublishedPCRelativeCodeOccurrenceRestoresCodeIdentity) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Caller =
      Image.findSymbol("jt_identity_pcrel_code_call");
  const neverd::Symbol *Target =
      Image.findSymbol("jt_identity_pcrel_code_target");
  ASSERT_NE(Caller, nullptr);
  ASSERT_NE(Target, nullptr);

  auto FieldIt = std::find_if(
      Image.CodeAddressRelocOperands.begin(),
      Image.CodeAddressRelocOperands.end(), [&](const auto &Entry) {
        return Entry.first >= Caller->Addr &&
               Entry.first < Caller->Addr + Caller->Size &&
               Entry.second.PCRelativeFromInstructionEnd;
      });
  ASSERT_NE(FieldIt, Image.CodeAddressRelocOperands.end());
  EXPECT_EQ(FieldIt->second.TargetVA, neverd::InvalidVA);
  EXPECT_EQ(Image.CodeRefTargets.count(Target->Addr), 0u)
      << "the loader must not publish a field-end approximation";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Function = Builder.build(Image, Decoder, Caller->Addr,
                                                 "jt_identity_pcrel_code_call");
  const auto Occurrence = std::find_if(
      Function.RelocatedInstructionAddressOccurrences.begin(),
      Function.RelocatedInstructionAddressOccurrences.end(),
      [&](const neverd::RelocatedInstructionAddressOccurrence &Item) {
        return Item.FieldVA == FieldIt->first;
      });
  ASSERT_NE(Occurrence, Function.RelocatedInstructionAddressOccurrences.end());
  EXPECT_EQ(Occurrence->TargetVA, Target->Addr);
  EXPECT_EQ(Occurrence->Provenance,
            neverd::ConstantAddressProvenance::CodeAddress);
  EXPECT_NE(std::find(Function.CodeRefTargets.begin(),
                      Function.CodeRefTargets.end(), Target->Addr),
            Function.CodeRefTargets.end());
}

TEST_F(JTE_X86_64, RuntimeSlotEscapePreservesRelocationMirror) {
  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string Body =
      llvmFunctionBody(LLVM.out, "jt_identity_runtime_slot_escape");
  ASSERT_FALSE(Body.empty()) << LLVM.out;
  const bool HasCase0 = Body.find("i64 0, label %") != std::string::npos ||
                        Body.find("i32 0, label %") != std::string::npos;
  const bool HasCase1 = Body.find("i64 1, label %") != std::string::npos ||
                        Body.find("i32 1, label %") != std::string::npos;
  EXPECT_TRUE(HasCase0) << Body;
  EXPECT_TRUE(HasCase1) << Body;
  const std::string Needle = "blockaddress(@jt_identity_runtime_slot_escape";
  size_t Count = 0;
  for (size_t Pos = LLVM.out.find(Needle); Pos != std::string::npos;
       Pos = LLVM.out.find(Needle, Pos + Needle.size()))
    ++Count;
  EXPECT_EQ(Count, 1u)
      << "a non-dispatch direct LOAD of runtime slot 0 must retain precisely "
         "that code-pointer relocation in the mirror:\n"
      << LLVM.out;
}

TEST_F(JTE_X86_64, TableBaseEscapePreservesWholeRelocationObject) {
  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string Body =
      llvmFunctionBody(LLVM.out, "jt_identity_table_base_escape");
  ASSERT_FALSE(Body.empty()) << LLVM.out;
  const bool HasCase0 = Body.find("i64 0, label %") != std::string::npos ||
                        Body.find("i32 0, label %") != std::string::npos;
  const bool HasCase1 = Body.find("i64 1, label %") != std::string::npos ||
                        Body.find("i32 1, label %") != std::string::npos;
  EXPECT_TRUE(HasCase0) << Body;
  EXPECT_TRUE(HasCase1) << Body;

  const std::string Needle = "blockaddress(@jt_identity_table_base_escape";
  size_t Count = 0;
  for (size_t Pos = LLVM.out.find(Needle); Pos != std::string::npos;
       Pos = LLVM.out.find(Needle, Pos + Needle.size()))
    ++Count;
  EXPECT_GE(Count, 2u) << "escaping the table base exposes every pointer slot, "
                          "not only slot 0:\n"
                       << LLVM.out;
}

TEST_F(JTE_X86_64, InteriorAddressEscapePreservesWholeRelocationObject) {
  auto Low = liftToLowIR(maskEqualBoundObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_identity_interior_address_escape");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("cst:0xED8:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xEDA:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xF3B:4"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string Body =
      llvmFunctionBody(LLVM.out, "jt_identity_interior_address_escape");
  ASSERT_FALSE(Body.empty()) << LLVM.out;
  const std::string Needle =
      "blockaddress(@jt_identity_interior_address_escape";
  size_t Count = 0;
  for (size_t Pos = LLVM.out.find(Needle); Pos != std::string::npos;
       Pos = LLVM.out.find(Needle, Pos + Needle.size()))
    ++Count;
  EXPECT_GE(Count, 3u)
      << "escaping an interior element pointer exposes the complete physical "
         "table object:\n"
      << LLVM.out;
}

TEST_F(JTE_X86_64, CrossFunctionConsumersVetoRelocationSuppression) {
  auto Low = liftToLowIR(maskEqualBoundObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "jt_identity_cross_function_sparse_dispatch");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("cst:0xF3C:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xF3E:4"), std::string::npos) << LowBody;
  EXPECT_NE(LowBody.find("cst:0xF9F:4"), std::string::npos)
      << "a gap relocation independently read by another function must be "
         "restored as a CFG root in the table owner:\n"
      << LowBody;

  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string Needle =
      "blockaddress(@jt_identity_cross_function_sparse_dispatch";
  size_t Count = 0;
  for (size_t Pos = LLVM.out.find(Needle); Pos != std::string::npos;
       Pos = LLVM.out.find(Needle, Pos + Needle.size()))
    ++Count;
  EXPECT_GE(Count, 2u)
      << "cross-function direct LOADs of gap and runtime slots must veto the "
         "module-level suppression union:\n"
      << LLVM.out;
}

TEST_F(JTE_X86_64, CrossFunctionVetoReachesModuleFixedPoint) {
  auto Low = liftToLowIR(maskEqualBoundObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;

  const std::string FirstOwner =
      lowFunctionBody(Low.out, "jt_identity_cross_function_sparse_dispatch");
  ASSERT_FALSE(FirstOwner.empty()) << Low.out;
  EXPECT_NE(FirstOwner.find("cst:0xF9F:4"), std::string::npos)
      << "round one must restore the first table's gap root:\n"
      << FirstOwner;

  const std::string SecondOwner =
      lowFunctionBody(Low.out, "jt_identity_cross_function_chain_dispatch");
  ASSERT_FALSE(SecondOwner.empty()) << Low.out;
  EXPECT_NE(SecondOwner.find("cst:0xFA0:4"), std::string::npos) << SecondOwner;
  EXPECT_NE(SecondOwner.find("cst:0xFA2:4"), std::string::npos) << SecondOwner;
  EXPECT_NE(SecondOwner.find("cst:0x1003:4"), std::string::npos)
      << "the root restored in round one reads table2.slot1, so round two "
         "must restore table2's gap root:\n"
      << SecondOwner;
}

void JTE_X86_64::expectMutableTableFailsClosed(const fs::path &Object,
                                               const std::string &Function) {
  auto Low = liftToLowIR(Object);
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody = lowFunctionBody(Low.out, Function);
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos)
      << "a mutable computed-goto remains an unresolved indirect branch, "
         "not a function-pointer call:\n"
      << LowBody;
  EXPECT_EQ(LowBody.find("INDIR_CALL"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(Object);
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody = llvmFunctionBody(LLVM.out, Function);
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos)
      << "a writeable table mutation must not emit the stale static mapping:\n"
      << LLVMBody;
  EXPECT_NE(LLVMBody.find("llvm.trap"), std::string::npos)
      << "the unresolved mutable dispatch must fail closed loudly:\n"
      << LLVMBody;

  auto High = liftToHighIR(Object);
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody = lowFunctionBody(High.out, Function);
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos)
      << "HighIR must not resurrect a stale switch (or selector const0):\n"
      << HighBody;
  EXPECT_NE(HighBody.find("goto"), std::string::npos)
      << "the unresolved mutable dispatch must stay explicit in HighIR:\n"
      << HighBody;
}

TEST_F(JTE_X86_64, CrossFunctionWritableTableStoreRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_cross_dispatch");
}

TEST_F(JTE_X86_64, SameFunctionWritableTableStoreRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_local_store");
}

TEST_F(JTE_X86_64, WritableTableEscapeToCallRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_call_escape");
}

TEST_F(JTE_X86_64, WritableTableInsideEscapedFrameSlotRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_frame_slot_escape");
}

TEST_F(JTE_X86_64, WritableTableStoredAfterFrameSlotEscapeRejectsStaticSwitch) {
  expectMutableTableFailsClosed(
      maskEqualBoundObj(), "jt_identity_writable_delayed_frame_slot_escape");
}

TEST_F(JTE_X86_64, WritableTableOutgoingStackArgumentRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_stack_arg_escape");
}

TEST_F(JTE_X86_64,
       WritableTableSecondStackArgumentFrameEscapeRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_stack_arg_frame_escape");
}

TEST_F(JTE_X86_64, WritableTablePrivateFrameSpillAcrossCallPreservesSwitch) {
  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_writable_private_spill");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_NE(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 0)) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 1)) << LLVMBody;
}

TEST_F(JTE_X86_64, StaleArgumentRegisterAliasDoesNotEscapeWritableTable) {
  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_writable_arg_alias_clobber");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_NE(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 0)) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 1)) << LLVMBody;
}

TEST_F(JTE_X86_64, LowByteSelfCopyPreservesWritableTableArgumentEscape) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_dil_self_escape");
}

TEST_F(JTE_X86_64, HighByteSelfCopyPreservesWritableTableArgumentEscape) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_dh_self_escape");
}

TEST_F(JTE_X86_64, NarrowStackPointerPivotEscapesWritableTable) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_esp_pivot_escape");
}

TEST_F(JTE_X86_64, NarrowStackPointerSelfTruncationEscapesWritableTable) {
  expectMutableTableFailsClosed(
      maskEqualBoundObj(), "jt_identity_writable_esp_self_truncate_escape");
}

TEST_F(JTE_X86_64, GlobalDataPointerToWritableTableRejectsStaticSwitch) {
  expectMutableTableFailsClosed(maskEqualBoundObj(),
                                "jt_identity_writable_indirect_dispatch");
}

TEST_F(JTE_X86_64, GlobalDataPointerToReadonlyTablePreservesSwitch) {
  auto LLVM = liftToLLVMIRUnopt(maskEqualBoundObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "jt_identity_readonly_dataptr_dispatch");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_NE(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 0)) << LLVMBody;
  EXPECT_TRUE(llvmHasSwitchCase(LLVMBody, 1)) << LLVMBody;

  auto High = liftToHighIR(maskEqualBoundObj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "jt_identity_readonly_dataptr_dispatch");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_NE(HighBody.find("switch"), std::string::npos) << HighBody;
}

TEST_F(JTE_X86_64, UnrelatedAddressInductionDoesNotPoisonModuleOwners) {
  auto ImageOrErr = neverd::loadBinary(moduleAddressOwnerInductionObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Safe = Image.findSymbol("module_owner_safe_dispatch");
  const neverd::Symbol *Mutated =
      Image.findSymbol("module_owner_mutated_dispatch");
  const neverd::Symbol *Writer =
      Image.findSymbol("module_owner_mutated_loop_writer");
  const neverd::Symbol *Unrelated =
      Image.findSymbol("module_owner_unrelated_pointer_induction");
  ASSERT_NE(Safe, nullptr);
  ASSERT_NE(Mutated, nullptr);
  ASSERT_NE(Writer, nullptr);
  ASSERT_NE(Unrelated, nullptr);

  std::set<neverd::va_t> Entries{Safe->Addr, Mutated->Addr, Writer->Addr,
                                 Unrelated->Addr};
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  const neverd::LowFunc SafeCandidate =
      Builder.build(Image, Decoder, Safe->Addr, Safe->Name);
  const neverd::LowFunc MutatedCandidate =
      Builder.build(Image, Decoder, Mutated->Addr, Mutated->Name);
  ASSERT_EQ(SafeCandidate.JumpTables.size(), 1u);
  ASSERT_EQ(SafeCandidate.JumpTables.front().Targets.size(), 2u);
  ASSERT_FALSE(SafeCandidate.JumpTables.front().StorageRanges.empty());
  ASSERT_EQ(MutatedCandidate.JumpTables.size(), 1u);
  ASSERT_EQ(MutatedCandidate.JumpTables.front().Targets.size(), 20u);
  ASSERT_FALSE(MutatedCandidate.JumpTables.front().StorageRanges.empty());

  DirectPipelineRun Run = runPipelineWithEvidenceBudget(Image, 256);
  ASSERT_TRUE(Run.Result.Success) << Run.Result.Error;

  const std::string SafeBody =
      llvmFunctionBody(Run.LLVMIR, "module_owner_safe_dispatch");
  ASSERT_FALSE(SafeBody.empty()) << Run.LLVMIR;
  EXPECT_NE(SafeBody.find("switch i"), std::string::npos) << SafeBody;
  EXPECT_TRUE(llvmHasSwitchCase(SafeBody, 0)) << SafeBody;
  EXPECT_TRUE(llvmHasSwitchCase(SafeBody, 1)) << SafeBody;

  const std::string MutatedBody =
      llvmFunctionBody(Run.LLVMIR, "module_owner_mutated_dispatch");
  ASSERT_FALSE(MutatedBody.empty()) << Run.LLVMIR;
  EXPECT_EQ(MutatedBody.find("switch i"), std::string::npos) << MutatedBody;
  EXPECT_NE(MutatedBody.find("llvm.trap"), std::string::npos) << MutatedBody;
}

TEST_F(JTE_X86_64, WidenedTableRootAliasesOverlappingLogicalSubtable) {
  auto ImageOrErr = neverd::loadBinary(moduleAddressOwnerInductionObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const auto Data =
      std::find_if(Image.Sections.begin(), Image.Sections.end(),
                   [](const neverd::Section &S) {
                     return S.Name == ".data.module_address_owner_induction";
                   });
  ASSERT_NE(Data, Image.Sections.end());

  const neverd::JumpTableStorageRange Safe{Data->VA, 8, 8, 2};
  const neverd::JumpTableStorageRange Wide{Data->VA + 24, 8, 8, 20};
  const neverd::JumpTableStorageRange Nested{Wide.BaseAddr + 18 * 8, 8, 8, 2};
  const neverd::JumpTableStorageRange Strided{Wide.BaseAddr, 4, 8, 20};
  const neverd::JumpTableStorageRange Padding{Wide.BaseAddr + 4, 4, 8, 20};
  EXPECT_TRUE(neverd::pipeline_detail::tableObjectSummaryMayAlias(
      Image, Wide.BaseAddr, Wide, Nested.BaseAddr, Nested));
  EXPECT_FALSE(neverd::pipeline_detail::tableObjectSummaryMayAlias(
      Image, Wide.BaseAddr, Wide, Safe.BaseAddr, Safe));
  EXPECT_FALSE(neverd::pipeline_detail::tableObjectSummaryMayAlias(
      Image, Strided.BaseAddr, Strided, Padding.BaseAddr, Padding));

  neverd::BinaryImage AdjacentImage;
  neverd::Section LeftSection;
  LeftSection.Name = "left";
  LeftSection.VA = 0x1000;
  LeftSection.Size = 0x10;
  LeftSection.Flags = neverd::SegmentFlags::Readable;
  neverd::Section RightSection;
  RightSection.Name = "right";
  RightSection.VA = 0x1010;
  RightSection.Size = 0x10;
  RightSection.Flags = neverd::SegmentFlags::Readable;
  AdjacentImage.Sections = {LeftSection, RightSection};
  neverd::Segment MappedSegment;
  MappedSegment.Name = "mapped";
  MappedSegment.VA = 0x1000;
  MappedSegment.Size = 0x20;
  MappedSegment.Flags = neverd::SegmentFlags::Readable;
  AdjacentImage.Segments.push_back(std::move(MappedSegment));
  const neverd::JumpTableStorageRange CrossesBoundary{0x1008, 8, 8, 2};
  const neverd::JumpTableStorageRange InRightSection{0x1010, 8, 8, 1};
  EXPECT_TRUE(neverd::pipeline_detail::tableObjectSummaryMayAlias(
      AdjacentImage, CrossesBoundary.BaseAddr, CrossesBoundary,
      InRightSection.BaseAddr, InRightSection));

  std::vector<neverd::JumpTableStorageRange> ManyLeft;
  std::vector<neverd::JumpTableStorageRange> ManyRight;
  for (uint64_t I = 0; I < 65; ++I) {
    ManyLeft.push_back({0x2000 + I * 16, 8, 8, 1});
    ManyRight.push_back({0x200000 + I * 16, 8, 8, 1});
  }
  EXPECT_TRUE(neverd::pipeline_detail::tableObjectSummaryMayAlias(
      AdjacentImage, ManyLeft.front().BaseAddr, ManyLeft,
      ManyRight.front().BaseAddr, ManyRight));
}

TEST_F(JTE_X86_64, ModuleArbitrationPropagatesWidenedRootToOverlappingOwner) {
  auto ImageOrErr = neverd::loadBinary(moduleAddressOwnerInductionObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Safe = Image.findSymbol("module_owner_safe_dispatch");
  const neverd::Symbol *Mutated =
      Image.findSymbol("module_owner_mutated_dispatch");
  const neverd::Symbol *Writer =
      Image.findSymbol("module_owner_mutated_loop_writer");
  const neverd::Symbol *Unrelated =
      Image.findSymbol("module_owner_unrelated_pointer_induction");
  ASSERT_NE(Safe, nullptr);
  ASSERT_NE(Mutated, nullptr);
  ASSERT_NE(Writer, nullptr);
  ASSERT_NE(Unrelated, nullptr);

  std::set<neverd::va_t> Entries{Safe->Addr, Mutated->Addr, Writer->Addr,
                                 Unrelated->Addr};
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::X64));
  neverd::CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  neverd::LowFunc SafeCandidate =
      Builder.build(Image, Decoder, Safe->Addr, Safe->Name);
  neverd::LowFunc MutatedCandidate =
      Builder.build(Image, Decoder, Mutated->Addr, Mutated->Name);
  neverd::LowFunc WriterCandidate =
      Builder.build(Image, Decoder, Writer->Addr, Writer->Name);
  ASSERT_EQ(SafeCandidate.JumpTables.size(), 1u);
  ASSERT_EQ(MutatedCandidate.JumpTables.size(), 1u);
  const neverd::JumpTable &WideTable = MutatedCandidate.JumpTables.front();
  const neverd::va_t WideBranch = WideTable.InsnAddr;
  ASSERT_GE(WideTable.StorageRanges.size(), 20u);
  std::vector<neverd::JumpTableStorageRange> NestedRanges(
      WideTable.StorageRanges.end() - 2, WideTable.StorageRanges.end());
  constexpr neverd::va_t NestedBranch = 0x7FFF0000;
  neverd::JumpTable NestedTable;
  NestedTable.InsnAddr = NestedBranch;
  NestedTable.BaseAddr = NestedRanges.front().BaseAddr;
  NestedTable.HasBaseAddr = true;
  NestedTable.EntrySize = NestedRanges.front().EntrySize;
  NestedTable.EntryStride = NestedRanges.front().EntryStride;
  NestedTable.StorageRanges = std::move(NestedRanges);
  neverd::LowFunc NestedOwner;
  NestedOwner.Entry = NestedBranch;
  NestedOwner.Name = "synthetic_overlapping_subtable_owner";
  NestedOwner.JumpTables.push_back(std::move(NestedTable));

  std::vector<neverd::LowFunc> Functions;
  Functions.push_back(std::move(SafeCandidate));
  Functions.push_back(std::move(MutatedCandidate));
  Functions.push_back(std::move(WriterCandidate));
  Functions.push_back(std::move(NestedOwner));
  const neverd::pipeline_detail::ModuleJumpTableArbitrationTestResult Result =
      neverd::pipeline_detail::arbitrateModuleJumpTablesForTesting(Image,
                                                                   Functions);
  ASSERT_TRUE(Result.AnalysisComplete);
  EXPECT_EQ(Result.UnsafeBranches.count(WideBranch), 1u);
  EXPECT_EQ(Result.UnsafeBranches.count(NestedBranch), 1u);
  EXPECT_EQ(Result.UnsafeBranches.count(
                Functions.front().JumpTables.front().InsnAddr),
            0u);
}

TEST_F(JTE_X86_64, EvidenceBudgetDoesNotCaptureIndexedCallbackTailCall) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const neverd::Symbol *Table =
      ImageOrErr->findSymbol("jt_identity_callback_tailcall_table");
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 2u * 8u);
  DirectPipelineRun Run = runPipelineWithEvidenceBudget(*ImageOrErr, 0);
  ASSERT_TRUE(Run.Result.Success) << Run.Result.Error;
  const neverd::LowFunc *Low =
      findLowFunction(Run.Result, "jt_identity_callback_tailcall");
  ASSERT_NE(Low, nullptr);
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_BR));

  const std::string LLVMBody =
      llvmFunctionBody(Run.LLVMIR, "jt_identity_callback_tailcall");
  ASSERT_FALSE(LLVMBody.empty()) << Run.LLVMIR;
  EXPECT_NE(LLVMBody.find("call"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("ret"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_X86_64,
       IncompleteMaskDomainDoesNotCaptureIndexedCallbackTailCall) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_incomplete_mask_callback_table");
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 2u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            2);

  DirectPipelineRun Run = runPipelineWithEvidenceBudget(Image, 0);
  ASSERT_TRUE(Run.Result.Success) << Run.Result.Error;
  const neverd::LowFunc *Low = findLowFunction(
      Run.Result, "jt_identity_incomplete_mask_callback_tailcall");
  ASSERT_NE(Low, nullptr);
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::INT_AND));
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low->JumpTables.empty());
  EXPECT_TRUE(Low->UnsafeIndirectBranchAddresses.empty());

  const std::string LLVMBody = llvmFunctionBody(
      Run.LLVMIR, "jt_identity_incomplete_mask_callback_tailcall");
  ASSERT_FALSE(LLVMBody.empty()) << Run.LLVMIR;
  EXPECT_NE(LLVMBody.find("call"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("ret"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_X86_64,
       IncompleteMaskDomainValidatesWholeSizedCallbackObject) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol(
      "jt_identity_incomplete_mask_prefix_callback_tailcall");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_identity_incomplete_mask_prefix_callback_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 4u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_AND));
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64,
       IncompleteMaskDomainRejectsNestedFunctionSymbolsWithoutEntrySet) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol(
      "jt_identity_incomplete_mask_nested_function_tailcall");
  const neverd::Symbol *Table = Image.findSymbol(
      "jt_identity_incomplete_mask_nested_function_table");
  const neverd::Symbol *CallbackA =
      Image.findSymbol("jt_identity_nested_callback_a");
  const neverd::Symbol *CallbackB =
      Image.findSymbol("jt_identity_nested_callback_b");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_NE(CallbackA, nullptr);
  ASSERT_NE(CallbackB, nullptr);
  ASSERT_EQ(Table->Size, 2u * 8u);
  ASSERT_TRUE(Image.hasFunctionSymbolAt(CallbackA->Addr));
  ASSERT_TRUE(Image.hasFunctionSymbolAt(CallbackB->Addr));

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_AND));
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64, EvidenceBudgetDoesNotClaimNextEntryOnlyCallbackEnvelope) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  DirectPipelineRun Run = runPipelineWithEvidenceBudget(*ImageOrErr, 0);
  ASSERT_TRUE(Run.Result.Success) << Run.Result.Error;
  const neverd::LowFunc *Low = findLowFunction(
      Run.Result, "jt_identity_unsized_local_callback_tailcall");
  ASSERT_NE(Low, nullptr);
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_BR));

  const std::string LLVMBody = llvmFunctionBody(
      Run.LLVMIR, "jt_identity_unsized_local_callback_tailcall");
  ASSERT_FALSE(LLVMBody.empty()) << Run.LLVMIR;
  EXPECT_NE(LLVMBody.find("call"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("ret"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_X86_64, EvidenceBudgetDoesNotClaimUnsizedSelfCallbackTable) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  DirectPipelineRun Run = runPipelineWithEvidenceBudget(*ImageOrErr, 0);
  ASSERT_TRUE(Run.Result.Success) << Run.Result.Error;
  const neverd::LowFunc *Low =
      findLowFunction(Run.Result, "jt_identity_unsized_self_callback_tailcall");
  ASSERT_NE(Low, nullptr);
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(lowFunctionHasOpcode(*Low, neverd::NdOp::RETURN));
  EXPECT_FALSE(lowFunctionHasOpcode(*Low, neverd::NdOp::INDIR_BR));

  const std::string LLVMBody = llvmFunctionBody(
      Run.LLVMIR, "jt_identity_unsized_self_callback_tailcall");
  ASSERT_FALSE(LLVMBody.empty()) << Run.LLVMIR;
  EXPECT_NE(LLVMBody.find("call"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("ret"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_X86_64, PatchObjSkips) {
  auto R = patchBinary(jteX64Obj());
  EXPECT_NE(R.exitCode, 0) << "ELF .o should not be directly patchable";
}

//===----------------------------------------------------------------------===//
// Two-level (index-byte) table — the MSVC-style `jmptab[idxtab[switchvar]]`
// sparse-switch lowering.  Recovery must dispatch on the real switch variable
// (one case per value) rather than on the intermediate address-table index.
//===----------------------------------------------------------------------===//

static fs::path twoLevelObj() {
  return fs::path(TEST_OBJ_DIR) / "test_twolevel_switch.o";
}

TEST_F(JTE_X86_64, TwoLevelAllStagesSucceed) { verifyAllStages(twoLevelObj()); }

TEST_F(JTE_X86_64, TwoLevelDispatchesOnRealVariable) {
  auto R = liftToLLVMIR(twoLevelObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  // The switch must be on the 32-bit source variable, not the 8-bit index-table
  // load value (which would collapse the case set to the address-table size and
  // dispatch on a value the program never compares).
  EXPECT_TRUE(R.contains("switch i32"))
      << "Expected the two-level table to dispatch on the 32-bit switch "
         "variable:\n"
      << R.out;
  EXPECT_FALSE(R.contains("switch i8"))
      << "Two-level table must not dispatch on the intermediate index byte:\n"
      << R.out;
}

TEST_F(JTE_X86_64, TwoLevelRecoversAllCases) {
  auto R = liftToLLVMIR(twoLevelObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  // idxtab has 21 entries (switch values 0..20), so a faithful recovery emits a
  // case per in-range value rather than only the 5 distinct address-table
  // targets.  Count switch case-label lines ("    i32 <n>, label ...").
  size_t Cases = 0;
  for (std::string::size_type P = R.out.find("i32 ", 0); P != std::string::npos;
       P = R.out.find("i32 ", P + 1))
    if (R.out.find(", label", P) != std::string::npos &&
        R.out.find(", label", P) < R.out.find('\n', P))
      ++Cases;
  EXPECT_GE(Cases, 10u)
      << "Expected many recovered case labels (one per switch value), got "
      << Cases << ":\n"
      << R.out;
}

TEST_F(JTE_X86_64, TwoLevelNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(twoLevelObj());
}

//===----------------------------------------------------------------------===//
// Unguarded, mask-bounded ABSOLUTE tables — `switch(x & mask)` lowered non-PIC
// to `jmp *table(,index,8)` with absolute code-pointer entries and no `cmp`
// range guard.  The table must be bounded by its code-pointer relocation run
// (its exact physical entry count); a resolver that only trusts a comparison
// guard drops every target and miscompiles the dispatch into dead control flow.
//===----------------------------------------------------------------------===//

static fs::path maskedAbsObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_masked_abs.o";
}

TEST_F(JTE_X86_64, MaskedAbsAllStagesSucceed) {
  verifyAllStages(maskedAbsObj());
}

TEST_F(JTE_X86_64, MaskedAbsContiguousRecoversSwitch) {
  auto R = liftToLLVMIR(maskedAbsObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  // The mask-bounded absolute switch must recover as a real dispatch, not a
  // dead table load with the indirect branch dropped.  The dispatch is on the
  // masked index (an iN value, N = mask width), so accept any switch width.
  EXPECT_TRUE(R.contains("switch i") || R.contains("indirectbr"))
      << "Expected a recovered switch/indirectbr for the mask-bounded absolute "
         "table:\n"
      << R.out;
}

TEST_F(JTE_X86_64, MaskedAbsRecoversManyCases) {
  auto R = liftToLLVMIR(maskedAbsObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  // Both functions have 16 distinct in-range arms.  Count switch arm edges
  // (", label %") across both recovered switches; recovering both tables yields
  // well over 24, while dropping either collapses far below it.
  size_t Arms = 0;
  for (std::string::size_type P = R.out.find(", label %", 0);
       P != std::string::npos; P = R.out.find(", label %", P + 1))
    ++Arms;
  EXPECT_GE(Arms, 24u)
      << "Expected many recovered case arms across the two mask-bounded "
         "tables, got "
      << Arms << ":\n"
      << R.out;
}

TEST_F(JTE_X86_64, MaskedAbsNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(maskedAbsObj());
}

//===----------------------------------------------------------------------===//
// World-class adversarial shapes — a merged (PHI) switch variable, a genuine
// distinct default block, a signed switch with negative base and interior gap,
// and a nested switch sharing index dataflow.  These stress switch-variable
// identification across merged paths, default-edge preservation, and exact
// (signed) case-label recovery — the axes a path-intersection recovery model
// must get right.
//===----------------------------------------------------------------------===//

static fs::path worldClassObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_worldclass.o";
}

TEST_F(JTE_X86_64, WorldClassAllStagesSucceed) {
  verifyAllStages(worldClassObj());
}

TEST_F(JTE_X86_64, WorldClassNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(worldClassObj());
}

TEST_F(JTE_X86_64, WorldClassNoConstantTrueBranches) {
  verifyNoConstantTrueBranch(worldClassObj());
}

// The merged switch variable (k = phi(a+1, b+2)) must still recover a real
// multi-way dispatch: the recovered switch condition is the value common to
// both incoming paths, so a switch with many arms survives.
TEST_F(JTE_X86_64, WorldClassMergedVariableRecoversSwitch) {
  auto R = liftToLLVMIR(worldClassObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  size_t Arms = 0;
  for (std::string::size_type P = R.out.find(", label %", 0);
       P != std::string::npos; P = R.out.find(", label %", P + 1))
    ++Arms;
  EXPECT_TRUE(R.contains("switch i"))
      << "Expected a recovered switch across the adversarial tables:\n"
      << R.out;
  EXPECT_GE(Arms, 20u)
      << "Expected many recovered case arms across the four switches, got "
      << Arms << ":\n"
      << R.out;
}

// A genuinely guarded switch has a distinct default block (SINK = y*3 + 1); its
// out-of-range computation must be preserved rather than folded onto a case.
// The `y * 3` multiply is the unique default signature in the module (case
// bodies multiply by other constants), so its survival proves the out-of-range
// edge was kept as a distinct destination and the recovered switch dispatches
// on the source variable (not the table index).
TEST_F(JTE_X86_64, WorldClassDefaultBlockPreserved) {
  auto R = liftToLLVMIR(worldClassObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("switch i32 %arg0"))
      << "Expected wc_guard_default to dispatch on the source variable:\n"
      << R.out;
  // Find a `mul i32 <x>, 3` line (clang may keep the multiply or
  // strength-reduce it; the resolver run here keeps the multiply), proving the
  // default's y*3 computation survived as a live, distinct block.
  bool FoundMul3 = false;
  for (std::string::size_type P = R.out.find("mul i32 ", 0);
       P != std::string::npos; P = R.out.find("mul i32 ", P + 1)) {
    std::string::size_type Eol = R.out.find('\n', P);
    std::string Line = R.out.substr(P, Eol - P);
    if (Line.size() >= 3 && Line.compare(Line.size() - 3, 3, ", 3") == 0) {
      FoundMul3 = true;
      break;
    }
  }
  EXPECT_TRUE(FoundMul3)
      << "Expected the distinct default computation (y * 3) to survive:\n"
      << R.out;
}

// The signed switch with a negative base must present the true negative labels
// rather than a 0-based normalized index.
TEST_F(JTE_X86_64, WorldClassSignedNegativeLabels) {
  auto R = decompileToHighC(worldClassObj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  std::ifstream IFS(tmpFile("decompiled_high.c"));
  ASSERT_TRUE(IFS) << "C output file not created";
  std::ostringstream SS;
  SS << IFS.rdbuf();
  std::string C = SS.str();
  EXPECT_NE(C.find("case -4:"), std::string::npos)
      << "Expected negative case label -4 from wc_signed_gap:\n"
      << C;
  EXPECT_NE(C.find("case -3:"), std::string::npos)
      << "Expected negative case label -3 from wc_signed_gap:\n"
      << C;
}

//===----------------------------------------------------------------------===//
// Computed-goto (threaded dispatch) — `goto *tab[idx]` with a function-local
// code-pointer label table, compiled at -O0 so the table load is decoupled
// from the indirect branch by a spill/reload relay.  A single dispatch site and
// a many-goto-site shared dispatch both recover a real multi-way switch rather
// than degrading to an indirect tail call.
//===----------------------------------------------------------------------===//

static fs::path cgotoObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_cgoto.o";
}

TEST_F(JTE_X86_64, ComputedGotoAllStagesSucceed) {
  verifyAllStages(cgotoObj());
}

TEST_F(JTE_X86_64, ComputedGotoNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(cgotoObj());
}

// Neither computed-goto dispatch may survive as an indirect branch or degrade
// to an indirect tail call; both must recover as real multi-way switches.
TEST_F(JTE_X86_64, ComputedGotoRecoversDispatch) {
  auto R = liftToLLVMIR(cgotoObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.contains("indirectbr"))
      << "Computed-goto dispatch left an unresolved indirect branch:\n"
      << R.out;
  size_t Switches = 0;
  for (std::string::size_type P = R.out.find("switch i", 0);
       P != std::string::npos; P = R.out.find("switch i", P + 1))
    ++Switches;
  EXPECT_GE(Switches, 2u)
      << "Expected both computed-goto sites to recover as switches:\n"
      << R.out;
  // The single-site dispatch (cg_single) must not degrade into an indirect
  // call of a loaded pointer (the tail-call fallback for an unresolved table).
  bool CgSingleHasIndirectCall = false;
  std::string::size_type F = R.out.find("@cg_single");
  if (F != std::string::npos) {
    std::string::size_type End = R.out.find("\n}", F);
    std::string Body = R.out.substr(F, End - F);
    CgSingleHasIndirectCall = Body.find("call i64 %") != std::string::npos ||
                              Body.find("call i32 %") != std::string::npos;
  }
  EXPECT_FALSE(CgSingleHasIndirectCall)
      << "cg_single degraded to an indirect tail call:\n"
      << R.out;
}

TEST_F(JTE_X86_64,
       StackTableEvidenceBudgetExhaustionIsTransactionalAndOpaque) {
  auto ImageOrErr = neverd::loadBinary(cgotoObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function = Image.findSymbol("cg_local_budget");
  ASSERT_NE(Function, nullptr);

  auto BuildWithBudget = [&](size_t Budget) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::X64));
    neverd::CFGBuilder Builder;
    Builder.setStackTableEvidenceBudgetForTesting(Budget);
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  };
  auto Recovered = [](const neverd::LowFunc &Low) {
    return !Low.JumpTables.empty();
  };

  const neverd::LowFunc Full =
      BuildWithBudget(neverd::limits::kMaxJumpTableEvidenceWork);
  ASSERT_TRUE(Recovered(Full));
  ASSERT_EQ(Full.JumpTables.size(), 1u);
  const neverd::va_t Branch = Full.JumpTables.front().InsnAddr;

  size_t Lo = 1;
  size_t Hi = neverd::limits::kMaxJumpTableEvidenceWork;
  while (Lo < Hi) {
    const size_t Mid = Lo + (Hi - Lo) / 2;
    if (Recovered(BuildWithBudget(Mid)))
      Hi = Mid;
    else
      Lo = Mid + 1;
  }
  const size_t MinimumSuccessfulBudget = Lo;
  ASSERT_GT(MinimumSuccessfulBudget, 0u);
  EXPECT_TRUE(Recovered(BuildWithBudget(MinimumSuccessfulBudget)));

  const neverd::LowFunc Exhausted =
      BuildWithBudget(MinimumSuccessfulBudget - 1);
  EXPECT_TRUE(Exhausted.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Exhausted, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Exhausted.UnsafeIndirectBranchAddresses.count(Branch));
}

TEST_F(JTE_X86_64, PatchELFExecutable) {
  auto ElfExe = fs::path(TEST_OBJ_DIR) / "test_jumptable_enhanced_elf";
  if (!fs::exists(ElfExe))
    GTEST_SKIP() << "ELF executable not built (ld.lld not available)";
  auto R = patchBinary(ElfExe);
  ASSERT_EQ(R.exitCode, 0) << "ELF patch failed: " << R.err;
  auto PatchedFile = tmpFile("patched");
  EXPECT_TRUE(fs::exists(PatchedFile)) << "Patched binary not created";
  if (fs::exists(PatchedFile))
    EXPECT_GT(fs::file_size(PatchedFile), 0u) << "Patched binary is empty";
}

//===----------------------------------------------------------------------===//
// AArch64
//===----------------------------------------------------------------------===//

class JTE_AArch64 : public NeverDLiftTest {};

static fs::path jteA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_enhanced_a64.o";
}

static fs::path indexIdentityA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_index_identity_a64.o";
}

static fs::path pageoffReachingA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_pageoff_reaching_a64.o";
}

static fs::path pageoffAlternativesA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_pageoff_alternatives_a64.o";
}

static fs::path pageoffNominalMissA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_pageoff_nominal_miss_a64.o";
}

static neverd::BinaryImage a64StrictSourceSpanImage(bool RightWritable = false) {
  neverd::BinaryImage Image;
  Image.Arch = neverd::Arch::AArch64;
  Image.Format = neverd::BinaryFormat::ELF;
  Image.Bits = neverd::Bitness::Bits64;
  Image.IsRelocatable = false;

  neverd::Segment Mapped;
  Mapped.Name = "rodata";
  Mapped.VA = 0x1000;
  Mapped.Size = 0x20;
  Mapped.FileSz = 0x20;
  Mapped.Flags = neverd::SegmentFlags::Readable;
  Mapped.Data.resize(0x20);
  Image.Segments.push_back(std::move(Mapped));

  neverd::Section Left;
  Left.Name = ".rodata.left";
  Left.VA = 0x1000;
  Left.Size = 0x10;
  Left.FileSz = 0x10;
  Left.Flags = neverd::SegmentFlags::Readable;
  Image.Sections.push_back(std::move(Left));

  neverd::Section Right;
  Right.Name = ".rodata.right";
  Right.VA = 0x1010;
  Right.Size = 0x10;
  Right.FileSz = 0x10;
  Right.Flags = neverd::SegmentFlags::Readable;
  if (RightWritable)
    Right.Flags = Right.Flags | neverd::SegmentFlags::Writable;
  Image.Sections.push_back(std::move(Right));
  return Image;
}

TEST_F(JTE_AArch64, RelocationFreeLoadCannotCrossSourceOwner) {
  const neverd::BinaryImage Image = a64StrictSourceSpanImage();
  ASSERT_EQ(neverd::exactImmutableDataSpanOwner(Image, 0x1008, 8, 0x1000),
            std::optional<neverd::va_t>{0x1000});
  EXPECT_FALSE(
      neverd::exactImmutableDataSpanOwner(Image, 0x100c, 8, 0x1000))
      << "an authenticated ADRP/ADD address does not authorize LOAD bytes "
         "owned by the adjacent object";
}

TEST_F(JTE_AArch64, RelocationFreeLoadCannotDereferenceOwnerOnePast) {
  const neverd::BinaryImage Image = a64StrictSourceSpanImage();
  ASSERT_EQ(neverd::exactImmutableDataSpanOwner(Image, 0x1010, 8, 0x1010),
            std::optional<neverd::va_t>{0x1010});
  EXPECT_FALSE(
      neverd::exactImmutableDataSpanOwner(Image, 0x1010, 8, 0x1000))
      << "the left object's legal one-past pointer is not authority to read "
         "the adjacent right object";
}

TEST_F(JTE_AArch64, RelocationFreeMemcpyRejectsRuntimeWritableSourceSpan) {
  const neverd::BinaryImage Image = a64StrictSourceSpanImage(true);
  ASSERT_EQ(neverd::exactImmutableDataSpanOwner(Image, 0x1000, 0x10, 0x1000),
            std::optional<neverd::va_t>{0x1000});
  EXPECT_FALSE(
      neverd::exactImmutableDataSpanOwner(Image, 0x1010, 0x10, 0x1010))
      << "the exact memcpy length must remain inside immutable source bytes";
}

static fs::path widthMismatchA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_width_mismatch_a64.o";
}

TEST_F(JTE_AArch64, AllStagesSucceed) { verifyAllStages(jteA64Obj()); }

TEST_F(JTE_AArch64, LowIRHasBranchOrIndirect) {
  auto R = liftToLowIR(jteA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INDIR_BR") || R.contains("COND_BR"))
      << "Expected INDIR_BR or COND_BR in AArch64 LowIR";
}

TEST_F(JTE_AArch64, HighIRHasControlFlow) {
  auto R = liftToHighIR(jteA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasControl = R.out.find("switch") != std::string::npos ||
                    R.out.find("case ") != std::string::npos ||
                    R.out.find("if") != std::string::npos ||
                    R.out.find("return") != std::string::npos;
  EXPECT_TRUE(HasControl) << "Expected control flow in HighIR";
}

TEST_F(JTE_AArch64, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(jteA64Obj());
}

TEST_F(JTE_AArch64, NoConstantTrueBranches) {
  verifyNoConstantTrueBranch(jteA64Obj());
}

TEST_F(JTE_AArch64, DecompileSucceeds) {
  auto R = decompileToHighC(jteA64Obj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
}

TEST_F(JTE_AArch64, NestedSwitchLifts) {
  auto R = liftToHighIR(jteA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_FALSE(R.out.empty());
}

TEST_F(JTE_AArch64, MaskBoundRejectsConstantSelectorWithUnreachableProducer) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_constant_selector_unreachable_mask");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_constant_selector_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 4u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            4)
      << "the regression requires four absolute code-pointer relocations";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawDeadMask3 = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops)
      if (Op.Opcode == neverd::NdOp::INT_AND)
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          SawDeadMask3 |= Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 3;
  ASSERT_TRUE(SawDeadMask3)
      << "the regression requires the unreachable lexical x&3 producer to "
         "be present in LowIR";
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a literal-zero selector must not borrow four cases from a "
         "disconnected lexical x&3 producer";
}

TEST_F(JTE_AArch64, ModuloBoundRejectsDestructiveDivisionNumerator) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_destructive_udiv_is_not_remainder");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_destructive_udiv_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            5)
      << "the regression requires five physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  bool SawUnsignedDivision = false;
  bool SawMultiply = false;
  bool SawSubtract = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      SawUnsignedDivision |= Op.Opcode == neverd::NdOp::INT_DIV;
      SawMultiply |= Op.Opcode == neverd::NdOp::INT_MULT;
      SawSubtract |= Op.Opcode == neverd::NdOp::INT_SUB;
    }
  ASSERT_TRUE(SawUnsignedDivision);
  ASSERT_TRUE(SawMultiply);
  ASSERT_TRUE(SawSubtract);
  EXPECT_TRUE(Low.JumpTables.empty())
      << "q-q*N after a destructive UDIV is not an unsigned remainder";
}

TEST_F(JTE_AArch64,
       ModuloBoundReplaysExplicitUnsignedDivisionAcrossFixedPoint) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_explicit_udiv_modulo_lfp");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_explicit_udiv_modulo_lfp_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            5)
      << "the regression requires five physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  size_t AddCount = 0;
  bool SawUnsignedDivision = false;
  bool SawMultiply = false;
  bool SawSubtract = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      AddCount += Op.Opcode == neverd::NdOp::INT_ADD;
      SawUnsignedDivision |= Op.Opcode == neverd::NdOp::INT_DIV;
      SawMultiply |= Op.Opcode == neverd::NdOp::INT_MULT;
      SawSubtract |= Op.Opcode == neverd::NdOp::INT_SUB;
    }
  ASSERT_GT(AddCount, 512u)
      << "the exact producer must sit beyond the expanded graph's flat "
         "proposal prefix";
  ASSERT_TRUE(SawUnsignedDivision);
  ASSERT_TRUE(SawMultiply);
  ASSERT_TRUE(SawSubtract);
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets.size(), 5u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_AArch64,
       ModuloBoundReplaysIneligibleConditionalSignedOldTargets) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_ineligible_sdiv_old_targets");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_ineligible_sdiv_old_targets_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 65u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            65);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_SDIV));
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_MULT));
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_SUB));
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  ASSERT_EQ(Low.JumpTables.front().Targets.size(), 5u);
  EXPECT_EQ(std::set<neverd::va_t>(Low.JumpTables.front().Targets.begin(),
                                  Low.JumpTables.front().Targets.end())
                .size(),
            5u);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_AArch64,
       ModuloBoundRejectsIneligibleConditionalSignedOldTargetEscape) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_ineligible_sdiv_old_target_escape");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_ineligible_sdiv_old_target_escape_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 65u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            65);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_SDIV));
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_MULT));
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_SUB));
  EXPECT_TRUE(Low.JumpTables.empty());
  EXPECT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
}

TEST_F(JTE_AArch64, ModuloBoundRejectsInexactExplicitDivisionRecipes) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  struct Negative {
    const char *Function;
    const char *Table;
    size_t Slots;
  };
  constexpr Negative Cases[] = {
      {"a64_explicit_sdiv_not_unsigned_modulo",
       "a64_explicit_sdiv_not_unsigned_modulo_table", 5},
      {"a64_explicit_udiv_foreign_dividend",
       "a64_explicit_udiv_foreign_dividend_table", 5},
      {"a64_explicit_udiv_wrong_divisor",
       "a64_explicit_udiv_wrong_divisor_table", 5},
      {"a64_explicit_udiv_wrong_multiplier",
       "a64_explicit_udiv_wrong_multiplier_table", 7},
  };

  for (const Negative &Case : Cases) {
    const neverd::Symbol *Function = Image.findSymbol(Case.Function);
    const neverd::Symbol *Table = Image.findSymbol(Case.Table);
    ASSERT_NE(Function, nullptr) << Case.Function;
    ASSERT_NE(Table, nullptr) << Case.Table;
    ASSERT_EQ(Table->Size, Case.Slots * 8u) << Case.Table;
    EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                            Image.CodePtrRelocSlots.end(),
                            [&](neverd::va_t Slot) {
                              return Slot >= Table->Addr &&
                                     Slot < Table->Addr + Table->Size;
                            }),
              Case.Slots)
        << Case.Table;

    neverd::Decoder Decoder;
    ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    const neverd::LowFunc Low =
        Builder.build(Image, Decoder, Function->Addr, Function->Name);
    EXPECT_TRUE(Low.JumpTables.empty()) << Case.Function;
  }
}

TEST_F(JTE_AArch64, ModuloBoundRejectsExplicitDivisionLateInteriorEdge) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_explicit_udiv_late_interior");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_explicit_udiv_late_interior_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 6u * 8u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_DIV));
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a newly authorized predecessor into the middle of the exact recipe "
         "must invalidate full-graph replay";
}

TEST_F(JTE_AArch64, ModuloBoundRejectsExplicitDivisionLateSelectorEscape) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_explicit_udiv_late_selector_escape");
  const neverd::Symbol *Table =
      Image.findSymbol("a64_explicit_udiv_late_selector_escape_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 5u * 8u);

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);
  ASSERT_TRUE(lowFunctionHasOpcode(Low, neverd::NdOp::INT_DIV));
  EXPECT_TRUE(Low.JumpTables.empty())
      << "full selector-domain replay must reject an authorized case that "
         "escapes [0,N)";
}

TEST_F(JTE_AArch64, CompactIndexAcceptsExplicitWToXZeroExtension) {
  auto R = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "a64_compact_explicit_zext");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_TRUE(llvmHasSwitchCase(Body, 0)) << Body;
  EXPECT_TRUE(llvmHasSwitchCase(Body, 1)) << Body;
  EXPECT_NE(Body.find("i64 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i64 399"), std::string::npos) << Body;
  expectSwitchNotOnLoadedEntry(Body, "i8");
}

TEST_F(JTE_AArch64, CompactIndexFollowsExactFrameSpillReload) {
  auto R = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "a64_compact_spill_reload");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("i64 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i64 1, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i64 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i64 499"), std::string::npos) << Body;
  expectSwitchNotOnLoadedEntry(Body, "i8");
}

TEST_F(JTE_AArch64, CompactIndexRejectsAtomicFrameOverwrite) {
  auto R = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "a64_compact_atomic_overwrite");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("i64 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i64 1, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("i64 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i64 1599"), std::string::npos) << Body;
  expectSwitchNotOnLoadedEntry(Body, "i8");
}

TEST_F(JTE_AArch64, StaleWRegisterAliasDoesNotEscapeWritableTable) {
  auto R = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      llvmFunctionBody(R.out, "a64_writable_arg_alias_clobber");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("switch i"), std::string::npos) << Body;
  EXPECT_TRUE(llvmHasSwitchCase(Body, 0)) << Body;
  EXPECT_TRUE(llvmHasSwitchCase(Body, 1)) << Body;
}

TEST_F(JTE_AArch64, WSelfCopyPreservesWritableTableArgumentEscape) {
  auto Low = liftToLowIR(indexIdentityA64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "a64_writable_w_self_escape");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;
  EXPECT_EQ(LowBody.find("INDIR_CALL"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "a64_writable_w_self_escape");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;

  auto High = liftToHighIR(indexIdentityA64Obj());
  ASSERT_EQ(High.exitCode, 0) << High.err;
  const std::string HighBody =
      lowFunctionBody(High.out, "a64_writable_w_self_escape");
  ASSERT_FALSE(HighBody.empty()) << High.out;
  EXPECT_EQ(HighBody.find("switch"), std::string::npos) << HighBody;
  EXPECT_NE(HighBody.find("goto"), std::string::npos) << HighBody;
}

TEST_F(JTE_AArch64, PageOffsetOutputRequiresExactReachingPageBase) {
  auto ImageOrErr = neverd::loadBinary(indexIdentityA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::TargetRegInfo &TRI =
      neverd::getTargetRegInfo(neverd::Arch::AArch64);
  ASSERT_FALSE(TRI.IntParamRegs.empty());
  const uint64_t X0 = TRI.IntParamRegs.front();

  auto buildFunction = [&](const char *Name) {
    const neverd::Symbol *Sym = Image.findSymbol(Name);
    EXPECT_NE(Sym, nullptr) << Name;
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::AArch64));
    neverd::CFGBuilder Builder;
    return Sym ? Builder.build(Image, Decoder, Sym->Addr, Name)
               : neverd::LowFunc{};
  };
  auto outputCertificates = [&](const neverd::LowFunc &Function,
                                const char *TableName) {
    const neverd::Symbol *Table = Image.findSymbol(TableName);
    EXPECT_NE(Table, nullptr) << TableName;
    std::vector<const neverd::RelocatedInstructionAddressOccurrence *> Found;
    if (!Table)
      return Found;
    for (const auto &Occurrence :
         Function.RelocatedInstructionAddressOccurrences)
      if (Occurrence.DefinesOutput && Occurrence.TargetVA == Table->Addr &&
          Occurrence.OutputWitness.isReg() &&
          Occurrence.OutputWitness.Offset == X0)
        Found.push_back(&Occurrence);
    return Found;
  };

  const neverd::LowFunc Exact = buildFunction("a64_writable_w_self_escape");
  const auto ExactCertificates =
      outputCertificates(Exact, "a64_writable_w_self_table");
  ASSERT_EQ(ExactCertificates.size(), 1u);
  EXPECT_FALSE(ExactCertificates.front()->OutputMayDepend);

  const neverd::LowFunc Wrong =
      buildFunction("a64_pageoff_wrong_base_no_escape");
  EXPECT_TRUE(outputCertificates(Wrong, "a64_pageoff_wrong_base_table").empty())
      << "a PAGEOFF relocation alone must not authenticate its output";

  const neverd::LowFunc Clobbered =
      buildFunction("a64_pageoff_clobbered_base_no_escape");
  EXPECT_TRUE(
      outputCertificates(Clobbered, "a64_pageoff_clobbered_base_table").empty())
      << "a matching ADRP killed before the ADD is not a reaching source";

  auto BypassImageOrErr = neverd::loadBinary(pageoffReachingA64Obj());
  ASSERT_TRUE(static_cast<bool>(BypassImageOrErr))
      << llvm::toString(BypassImageOrErr.takeError());
  neverd::BinaryImage &BypassImage = *BypassImageOrErr;
  const neverd::Symbol *BypassFunction =
      BypassImage.findSymbol("a64_pageoff_bypass_may_escape");
  const neverd::Symbol *BypassTable =
      BypassImage.findSymbol("a64_pageoff_bypass_table");
  ASSERT_NE(BypassFunction, nullptr);
  ASSERT_NE(BypassTable, nullptr);
  neverd::Decoder BypassDecoder;
  ASSERT_TRUE(BypassDecoder.init(neverd::Arch::AArch64));
  neverd::CFGBuilder BypassBuilder;
  const neverd::LowFunc Bypass =
      BypassBuilder.build(BypassImage, BypassDecoder, BypassFunction->Addr,
                          "a64_pageoff_bypass_may_escape");
  std::vector<const neverd::RelocatedInstructionAddressOccurrence *>
      BypassCertificates;
  for (const auto &Occurrence : Bypass.RelocatedInstructionAddressOccurrences)
    if (Occurrence.DefinesOutput && Occurrence.TargetVA == BypassTable->Addr &&
        Occurrence.OutputWitness.isReg() &&
        Occurrence.OutputWitness.Offset == X0)
      BypassCertificates.push_back(&Occurrence);
  ASSERT_EQ(BypassCertificates.size(), 1u);
  EXPECT_TRUE(BypassCertificates.front()->OutputMayDepend)
      << "the direct predecessor prevents an exact output certificate, but "
         "the ADRP path remains a conservative escape source";
}

TEST_F(JTE_AArch64, UnpairedAndClobberedPageOffsetsDoNotEscapeTable) {
  auto R = liftToLLVMIRUnopt(indexIdentityA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  for (const char *Name : {"a64_pageoff_wrong_base_no_escape",
                           "a64_pageoff_clobbered_base_no_escape"}) {
    const std::string Body = llvmFunctionBody(R.out, Name);
    ASSERT_FALSE(Body.empty()) << Name << '\n' << R.out;
    EXPECT_NE(Body.find("switch i"), std::string::npos) << Body;
    EXPECT_TRUE(llvmHasSwitchCase(Body, 0)) << Body;
    EXPECT_TRUE(llvmHasSwitchCase(Body, 1)) << Body;
  }
}

TEST_F(JTE_AArch64, BypassedPageBaseMayEscapeWritableTable) {
  auto Low = liftToLowIR(pageoffReachingA64Obj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string LowBody =
      lowFunctionBody(Low.out, "a64_pageoff_bypass_may_escape");
  ASSERT_FALSE(LowBody.empty()) << Low.out;
  EXPECT_NE(LowBody.find("INDIR_BR"), std::string::npos) << LowBody;
  EXPECT_EQ(LowBody.find("INDIR_CALL"), std::string::npos) << LowBody;

  auto LLVM = liftToLLVMIRUnopt(pageoffReachingA64Obj());
  EXPECT_NE(LLVM.exitCode, 0)
      << "an all-path-inexact relocated address must not reach a static "
         "switch:\n"
      << LLVM.out;
  EXPECT_NE(LLVM.err.find("incomplete relocatable address value"),
            std::string::npos)
      << LLVM.err;
}

TEST_F(JTE_AArch64, PageOffsetPairsAcrossOwnersOnTheSamePage) {
  auto ImageOrErr = neverd::loadBinary(pageoffAlternativesA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("a64_pageoff_same_page_cross_owner");
  const neverd::Symbol *TargetA = Image.findSymbol("a64_pageoff_same_page_a");
  const neverd::Symbol *PageSourceB =
      Image.findSymbol("a64_pageoff_same_page_b");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(TargetA, nullptr);
  ASSERT_NE(PageSourceB, nullptr);
  const neverd::Section *OwnerA = Image.getSectionFor(TargetA->Addr);
  const neverd::Section *OwnerB = Image.getSectionFor(PageSourceB->Addr);
  ASSERT_NE(OwnerA, nullptr);
  ASSERT_NE(OwnerB, nullptr);
  EXPECT_NE(OwnerA->VA, OwnerB->VA);
  EXPECT_EQ(TargetA->Addr & ~neverd::va_t{0xfff},
            PageSourceB->Addr & ~neverd::va_t{0xfff});

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::AArch64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(
      Image, Decoder, Function->Addr, "a64_pageoff_same_page_cross_owner");
  const neverd::TargetRegInfo &TRI =
      neverd::getTargetRegInfo(neverd::Arch::AArch64);
  ASSERT_FALSE(TRI.IntParamRegs.empty());
  std::vector<const neverd::RelocatedInstructionAddressOccurrence *> Found;
  for (const auto &Occurrence : Low.RelocatedInstructionAddressOccurrences)
    if (Occurrence.DefinesOutput && Occurrence.TargetVA == TargetA->Addr &&
        Occurrence.OutputWitness.isReg() &&
        Occurrence.OutputWitness.Offset == TRI.IntParamRegs.front())
      Found.push_back(&Occurrence);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_FALSE(Found.front()->OutputMayDepend)
      << "same-page arithmetic is exact even when the authenticated ADRP and "
         "PAGEOFF symbols have different container owners";
  EXPECT_EQ(Found.front()->TargetOwnerVA, OwnerA->VA);
}

TEST_F(JTE_AArch64,
       DifferentPageAlternativesDoNotLoseEitherWritableTableEscape) {
  auto ImageOrErr = neverd::loadBinary(pageoffAlternativesA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Escape =
      Image.findSymbol("a64_pageoff_two_page_escape");
  const neverd::Symbol *TableA =
      Image.findSymbol("a64_pageoff_two_page_table_a");
  const neverd::Symbol *TableB =
      Image.findSymbol("a64_pageoff_two_page_table_b");
  ASSERT_NE(Escape, nullptr);
  ASSERT_NE(TableA, nullptr);
  ASSERT_NE(TableB, nullptr);
  EXPECT_EQ(TableA->Addr & neverd::va_t{0xfff},
            TableB->Addr & neverd::va_t{0xfff});
  EXPECT_NE(TableA->Addr & ~neverd::va_t{0xfff},
            TableB->Addr & ~neverd::va_t{0xfff});

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::AArch64));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc EscapeLow = Builder.build(
      Image, Decoder, Escape->Addr, "a64_pageoff_two_page_escape");
  std::vector<const neverd::RelocatedInstructionAddressOccurrence *> Found;
  for (const auto &Occurrence :
       EscapeLow.RelocatedInstructionAddressOccurrences)
    if (Occurrence.DefinesOutput && Occurrence.TargetVA == TableA->Addr)
      Found.push_back(&Occurrence);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_TRUE(Found.front()->OutputMayDepend)
      << "the PAGEOFF relocation names A, but the B-page predecessor remains "
         "a feasible output alternative";

  llvm::LLVMContext Context;
  neverd::PipelineOptions Options;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  neverd::Pipeline Pipeline;
  neverd::PipelineResult Result = Pipeline.run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto findLow = [&](const char *Name) -> const neverd::LowFunc * {
    auto It = std::find_if(
        Result.LowFuncs.begin(), Result.LowFuncs.end(),
        [&](const neverd::LowFunc &Function) { return Function.Name == Name; });
    return It == Result.LowFuncs.end() ? nullptr : &*It;
  };
  for (const char *Name :
       {"a64_pageoff_two_page_dispatch_a", "a64_pageoff_two_page_dispatch_b"}) {
    const neverd::LowFunc *Function = findLow(Name);
    ASSERT_NE(Function, nullptr) << Name;
    ASSERT_FALSE(Function->JumpTables.empty()) << Name;
    EXPECT_TRUE(std::all_of(
        Function->JumpTables.begin(), Function->JumpTables.end(),
        [](const neverd::JumpTable &JT) { return JT.MutatedUnsafe; }))
        << Name
        << " must fail closed when a partial output witness makes "
           "module escape analysis incomplete";
    for (const neverd::JumpTable &JT : Function->JumpTables)
      EXPECT_TRUE(Function->UnsafeIndirectBranchAddresses.count(JT.InsnAddr))
          << Name;
  }
}

TEST_F(JTE_AArch64, NonNominalReachingPageStillEscapesTheWritableTable) {
  auto ImageOrErr = neverd::loadBinary(pageoffNominalMissA64Obj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *TableA =
      Image.findSymbol("a64_pageoff_nominal_miss_table_a");
  const neverd::Symbol *TableB =
      Image.findSymbol("a64_pageoff_nominal_miss_table_b");
  ASSERT_NE(TableA, nullptr);
  ASSERT_NE(TableB, nullptr);
  EXPECT_EQ(TableA->Addr & neverd::va_t{0xfff},
            TableB->Addr & neverd::va_t{0xfff});
  EXPECT_NE(TableA->Addr & ~neverd::va_t{0xfff},
            TableB->Addr & ~neverd::va_t{0xfff});

  llvm::LLVMContext Context;
  neverd::PipelineOptions Options;
  Options.DumpLow = true;
  Options.EmitDumpOutput = false;
  neverd::Pipeline Pipeline;
  neverd::PipelineResult Result = Pipeline.run(Image, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto It = std::find_if(Result.LowFuncs.begin(), Result.LowFuncs.end(),
                         [](const neverd::LowFunc &Function) {
                           return Function.Name ==
                                  "a64_pageoff_nominal_miss_dispatch_b";
                         });
  ASSERT_NE(It, Result.LowFuncs.end());
  ASSERT_FALSE(It->JumpTables.empty());
  EXPECT_TRUE(
      std::all_of(It->JumpTables.begin(), It->JumpTables.end(),
                  [](const neverd::JumpTable &JT) { return JT.MutatedUnsafe; }))
      << "the only reaching ADRP page is B, so passing the PAGEOFF result to "
         "an opaque callee must invalidate B's writable static table";
  for (const neverd::JumpTable &JT : It->JumpTables)
    EXPECT_TRUE(It->UnsafeIndirectBranchAddresses.count(JT.InsnAddr));
}

TEST_F(JTE_AArch64, CompactIndexRejectsWGuardForUnmodifiedXValue) {
  auto R = liftToLLVMIRUnopt(widthMismatchA64Obj());
  EXPECT_NE(R.exitCode, 0)
      << "A W0 guard must not bound the unmodified X0 table index:\n"
      << R.out;
  EXPECT_NE(R.err.find("refusing stale-address fallback"), std::string::npos)
      << R.err;
}

static fs::path worldClassA64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_worldclass_a64.o";
}

TEST_F(JTE_AArch64, WorldClassAllStagesSucceed) {
  verifyAllStages(worldClassA64Obj());
}

TEST_F(JTE_AArch64, WorldClassNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(worldClassA64Obj());
}

TEST_F(JTE_AArch64, WorldClassRecoversSwitches) {
  auto R = liftToLLVMIR(worldClassA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  size_t Switches = 0;
  for (std::string::size_type P = R.out.find("switch i", 0);
       P != std::string::npos; P = R.out.find("switch i", P + 1))
    ++Switches;
  EXPECT_GE(Switches, 3u)
      << "Expected the adversarial tables to recover as switches:\n"
      << R.out;
}

// AArch64 -O1 lowers the gapped signed switch to a comparison tree rather than
// one indexed table, so the negative case value surfaces in the recovered
// switch/branch conditions rather than as a single `case -4:` arm.  Requiring
// the signed value -4 to appear in the LLVM IR proves the negative label was
// recovered with correct sign regardless of the structurer's rendering.
TEST_F(JTE_AArch64, WorldClassSignedNegativeLabels) {
  auto R = liftToLLVMIR(worldClassA64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("i32 -4") || R.contains(", -4") || R.contains("-4,"))
      << "Expected the signed value -4 from wc_signed_gap to be recovered:\n"
      << R.out;
}

//===----------------------------------------------------------------------===//
// ARM32
//===----------------------------------------------------------------------===//

class JTE_ARM32 : public NeverDLiftTest {};

static fs::path jteARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_enhanced_arm.o";
}

static fs::path indexIdentityARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_index_identity_arm.obj";
}

static fs::path incompleteDomainARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_incomplete_domain_arm.obj";
}

static fs::path predicatedModuleEscapeARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_module_predicated_escape_arm.o";
}

static fs::path rel32ReachingARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_arm_rel32_reaching.o";
}

static fs::path rel32MultipleSourcesARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_arm_rel32_multiple_sources.o";
}

TEST_F(JTE_ARM32, AllStagesSucceed) { verifyAllStages(jteARMObj()); }

TEST_F(JTE_ARM32, LowIRHasBranchOrIndirect) {
  auto R = liftToLowIR(jteARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INDIR_BR") || R.contains("COND_BR"))
      << "Expected INDIR_BR or COND_BR in ARM32 LowIR";
}

TEST_F(JTE_ARM32, HighIRHasControlFlow) {
  auto R = liftToHighIR(jteARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  bool HasControl = R.out.find("switch") != std::string::npos ||
                    R.out.find("case ") != std::string::npos ||
                    R.out.find("if") != std::string::npos ||
                    R.out.find("return") != std::string::npos;
  EXPECT_TRUE(HasControl) << "Expected control flow in HighIR";
}

TEST_F(JTE_ARM32, LLVMIRNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(jteARMObj());
}

TEST_F(JTE_ARM32, DecompileSucceeds) {
  auto R = decompileToHighC(jteARMObj());
  ASSERT_EQ(R.exitCode, 0) << "Decompile failed: " << R.err;
  auto CFile = tmpFile("decompiled_high.c");
  ASSERT_TRUE(fs::exists(CFile));
}

TEST_F(JTE_ARM32, TBBInclusiveGuardKeepsLastCaseWithoutPoison) {
  auto R = liftToLLVMIRUnopt(indexIdentityARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "arm_tbb_inclusive");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("i32 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 1, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i32 399"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, TBHExclusiveGuardRejectsAdjacentPoison) {
  auto R = liftToLLVMIRUnopt(indexIdentityARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "arm_tbh_exclusive");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("i32 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 1, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("i32 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i32 499"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, TBBInclusiveGuardFollowsExactFrameSpillReload) {
  auto R = liftToLLVMIRUnopt(indexIdentityARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = llvmFunctionBody(R.out, "arm_tbb_spill_inclusive");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("i32 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 1, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 2, label"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("ret i32 599"), std::string::npos) << Body;
  expectSwitchNotOnLoadedEntry(Body, "i8");
}

TEST_F(JTE_ARM32, PredicatedMaskDoesNotNarrowTheUntakenPath) {
  auto R = liftToLowIR(incompleteDomainARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "arm_tbb_predicated_mask");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8FC:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8FD:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8FE:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x8FF:4"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, PredicatedMaskOffsetDoesNotCreateACompleteDomain) {
  auto R = liftToLowIR(incompleteDomainARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "arm_tbb_predicated_offset");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x906:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x907:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x908:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x95F:4"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, PredicatedMaskTaintSurvivesLowByteExtraction) {
  auto R = liftToLowIR(incompleteDomainARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "arm_tbb_predicated_mask_low8");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x910:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x911:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x912:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0x95E:4"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, PredicatedLoadPreservesSkippedTableArgumentEscape) {
  auto Low = liftToLowIR(predicatedModuleEscapeARMObj());
  ASSERT_EQ(Low.exitCode, 0) << Low.err;
  const std::string Body =
      lowFunctionBody(Low.out, "arm_predicated_load_may_escape_table");
  ASSERT_FALSE(Body.empty()) << Low.out;
  EXPECT_NE(Body.find("INDIR_BR"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_CALL"), std::string::npos) << Body;

  auto LLVM = liftToLLVMIRUnopt(predicatedModuleEscapeARMObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string LLVMBody =
      llvmFunctionBody(LLVM.out, "arm_predicated_load_may_escape_table");
  ASSERT_FALSE(LLVMBody.empty()) << LLVM.out;
  EXPECT_EQ(LLVMBody.find("switch i"), std::string::npos) << LLVMBody;
  EXPECT_NE(LLVMBody.find("llvm.trap"), std::string::npos) << LLVMBody;
}

TEST_F(JTE_ARM32, PredicatedUnrelatedLoadDoesNotRejectStaticTable) {
  auto LLVM = liftToLLVMIRUnopt(predicatedModuleEscapeARMObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;
  const std::string Body =
      llvmFunctionBody(LLVM.out, "arm_predicated_unrelated_load_keeps_table");
  ASSERT_FALSE(Body.empty()) << LLVM.out;
  EXPECT_NE(Body.find("switch i"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 0, label"), std::string::npos) << Body;
  EXPECT_NE(Body.find("i32 1, label"), std::string::npos) << Body;
}

TEST_F(JTE_ARM32, RelativeLiteralOutputRequiresExactReachingLoad) {
  auto ExactImageOrErr = neverd::loadBinary(predicatedModuleEscapeARMObj());
  ASSERT_TRUE(static_cast<bool>(ExactImageOrErr))
      << llvm::toString(ExactImageOrErr.takeError());
  neverd::BinaryImage &ExactImage = *ExactImageOrErr;
  const neverd::Symbol *ExactFunction =
      ExactImage.findSymbol("arm_predicated_load_may_escape_table");
  const neverd::Symbol *ExactTarget =
      ExactImage.findSymbol("arm_predicated_escape_table");
  ASSERT_NE(ExactFunction, nullptr);
  ASSERT_NE(ExactTarget, nullptr);
  neverd::Decoder ExactDecoder;
  ASSERT_TRUE(ExactDecoder.init(neverd::Arch::ARM));
  neverd::CFGBuilder ExactBuilder;
  const neverd::LowFunc Exact =
      ExactBuilder.build(ExactImage, ExactDecoder, ExactFunction->Addr,
                         "arm_predicated_load_may_escape_table");
  const neverd::TargetRegInfo &TRI =
      neverd::getTargetRegInfo(neverd::Arch::ARM);
  ASSERT_FALSE(TRI.IntParamRegs.empty());
  std::vector<const neverd::RelocatedInstructionAddressOccurrence *>
      ExactCertificates;
  for (const auto &Occurrence : Exact.RelocatedInstructionAddressOccurrences)
    if (Occurrence.DefinesOutput && Occurrence.TargetVA == ExactTarget->Addr &&
        Occurrence.OutputWitness.isReg() &&
        Occurrence.OutputWitness.Offset == TRI.IntParamRegs.front())
      ExactCertificates.push_back(&Occurrence);
  ASSERT_EQ(ExactCertificates.size(), 1u);
  EXPECT_FALSE(ExactCertificates.front()->OutputMayDepend)
      << "the exact literal LOAD must authenticate the argument-producing "
         "PC ADD";

  auto ImageOrErr = neverd::loadBinary(rel32ReachingARMObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("arm_rel32_bypass_may_depend");
  const neverd::Symbol *Target = Image.findSymbol("arm_rel32_bypass_target");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Target, nullptr);
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(neverd::Arch::ARM));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low = Builder.build(Image, Decoder, Function->Addr,
                                            "arm_rel32_bypass_may_depend");
  std::vector<const neverd::RelocatedInstructionAddressOccurrence *> Found;
  for (const auto &Occurrence : Low.RelocatedInstructionAddressOccurrences)
    if (Occurrence.DefinesOutput && Occurrence.TargetVA == Target->Addr)
      Found.push_back(&Occurrence);
  ASSERT_EQ(Found.size(), 1u);
  EXPECT_TRUE(Found.front()->OutputMayDepend)
      << "the direct predecessor must prevent an exact output certificate";
}

TEST_F(JTE_ARM32,
       RelativeLiteralUsesAppliedSlotsAndPublishesEveryReachingField) {
  auto ImageOrErr = neverd::loadBinary(rel32ReachingARMObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  const neverd::RelocationEntry *RawA = nullptr;
  const neverd::RelocationEntry *RawB = nullptr;
  for (const neverd::RelocationEntry &Reloc : Image.Relocations) {
    if (Reloc.SymbolName == "arm_rel32_offset_target_a")
      RawA = &Reloc;
    else if (Reloc.SymbolName == "arm_rel32_offset_target_b")
      RawB = &Reloc;
  }
  ASSERT_NE(RawA, nullptr);
  ASSERT_NE(RawB, nullptr);
  EXPECT_EQ(RawA->Address, RawB->Address)
      << "the fixture must keep equal raw section-relative r_offsets";
  EXPECT_NE(RawA->SectionName, RawB->SectionName);

  auto appliedSlotsFor = [&](const char *TargetName) {
    const neverd::Symbol *Target = Image.findSymbol(TargetName);
    EXPECT_NE(Target, nullptr) << TargetName;
    std::vector<neverd::va_t> Slots;
    if (!Target)
      return Slots;
    for (const auto &[Slot, Field] : Image.ARMRelativeLiteralFields)
      if (Field.TargetVA == Target->Addr)
        Slots.push_back(Slot);
    return Slots;
  };
  const std::vector<neverd::va_t> AppliedA =
      appliedSlotsFor("arm_rel32_offset_target_a");
  const std::vector<neverd::va_t> AppliedB =
      appliedSlotsFor("arm_rel32_offset_target_b");
  ASSERT_EQ(AppliedA.size(), 1u);
  ASSERT_EQ(AppliedB.size(), 1u);
  EXPECT_NE(AppliedA.front(), AppliedB.front());
  EXPECT_TRUE(AppliedA.front() != RawA->Address ||
              AppliedB.front() != RawB->Address)
      << "at least one mapped SlotVA must differ from its raw r_offset";

  auto buildFunction = [&](const char *Name) {
    const neverd::Symbol *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr) << Name;
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(neverd::Arch::ARM));
    neverd::CFGBuilder Builder;
    return Function ? Builder.build(Image, Decoder, Function->Addr, Name)
                    : neverd::LowFunc{};
  };
  auto outputCertificates = [&](const neverd::LowFunc &Function,
                                const char *TargetName) {
    const neverd::Symbol *Target = Image.findSymbol(TargetName);
    EXPECT_NE(Target, nullptr) << TargetName;
    std::vector<const neverd::RelocatedInstructionAddressOccurrence *> Found;
    if (!Target)
      return Found;
    for (const auto &Occurrence :
         Function.RelocatedInstructionAddressOccurrences)
      if (Occurrence.DefinesOutput && Occurrence.TargetVA == Target->Addr)
        Found.push_back(&Occurrence);
    return Found;
  };

  const neverd::LowFunc FunctionA = buildFunction("arm_rel32_same_offset_a");
  const auto CertificatesA =
      outputCertificates(FunctionA, "arm_rel32_offset_target_a");
  ASSERT_EQ(CertificatesA.size(), 1u);
  EXPECT_EQ(CertificatesA.front()->FieldVA, AppliedA.front());
  EXPECT_FALSE(CertificatesA.front()->OutputMayDepend);

  const neverd::LowFunc FunctionB = buildFunction("arm_rel32_same_offset_b");
  const auto CertificatesB =
      outputCertificates(FunctionB, "arm_rel32_offset_target_b");
  ASSERT_EQ(CertificatesB.size(), 1u);
  EXPECT_EQ(CertificatesB.front()->FieldVA, AppliedB.front());
  EXPECT_FALSE(CertificatesB.front()->OutputMayDepend);

  const neverd::LowFunc TwoSources =
      buildFunction("arm_rel32_same_target_two_literals");
  const auto Certificates =
      outputCertificates(TwoSources, "arm_rel32_two_literal_target");
  ASSERT_EQ(Certificates.size(), 2u)
      << "both reaching literal fields must remain public provenance";
  std::set<neverd::va_t> FieldVAs;
  for (const neverd::RelocatedInstructionAddressOccurrence *Occurrence :
       Certificates) {
    EXPECT_FALSE(Occurrence->OutputMayDepend);
    FieldVAs.insert(Occurrence->FieldVA);
  }
  EXPECT_EQ(FieldVAs.size(), 2u);
}

TEST_F(JTE_ARM32,
       EquivalentRelativeLiteralFieldsDoNotAbandonModuleArbitration) {
  auto LLVM = liftToLLVMIRUnopt(rel32MultipleSourcesARMObj());
  ASSERT_EQ(LLVM.exitCode, 0) << LLVM.err;

  const std::string Escaped =
      llvmFunctionBody(LLVM.out, "arm_rel32_two_source_dispatch_a");
  ASSERT_FALSE(Escaped.empty()) << LLVM.out;
  EXPECT_EQ(Escaped.find("switch i"), std::string::npos) << Escaped;
  EXPECT_NE(Escaped.find("llvm.trap"), std::string::npos) << Escaped;

  const std::string Independent =
      llvmFunctionBody(LLVM.out, "arm_rel32_two_source_dispatch_b");
  ASSERT_FALSE(Independent.empty()) << LLVM.out;
  EXPECT_NE(Independent.find("switch i"), std::string::npos) << Independent;
  EXPECT_NE(Independent.find("i32 0, label"), std::string::npos) << Independent;
  EXPECT_NE(Independent.find("i32 1, label"), std::string::npos) << Independent;
}

static fs::path worldClassARMObj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_worldclass_arm.o";
}

TEST_F(JTE_ARM32, WorldClassAllStagesSucceed) {
  verifyAllStages(worldClassARMObj());
}

TEST_F(JTE_ARM32, WorldClassNoVerifierErrors) {
  verifyLLVMIRNoVerifierErrors(worldClassARMObj());
}

TEST_F(JTE_ARM32, WorldClassRecoversControlFlow) {
  auto R = liftToLLVMIR(worldClassARMObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  // ARM32 may lower some tables to compare chains; require at least one real
  // switch dispatch to survive across the four adversarial functions.
  EXPECT_TRUE(R.contains("switch i"))
      << "Expected at least one recovered switch on ARM32:\n"
      << R.out;
}

TEST_F(JTE_X86_64, MaskBoundAcceptsBitSetBeforeNegativeOffset) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;

  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_or_negative_offset");
  const neverd::Symbol *TableSymbol =
      Image.findSymbol("jt_identity_mask_or_negative_offset_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(TableSymbol, nullptr);
  ASSERT_EQ(TableSymbol->Size, 8u * 4u);
  EXPECT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= TableSymbol->Addr &&
                                   Slot < TableSymbol->Addr + TableSymbol->Size;
                          }),
            8)
      << "the odd poison slots must be real adjacent code relocations";

  auto SymbolAddress = [&](const char *Name) {
    const neverd::Symbol *Symbol = Image.findSymbol(Name);
    EXPECT_NE(Symbol, nullptr) << Name;
    return Symbol ? Symbol->Addr : neverd::va_t{0};
  };
  const std::vector<neverd::va_t> ExpectedTargets = {
      SymbolAddress("jt_identity_mask_or_negative_case0"),
      SymbolAddress("jt_identity_mask_or_negative_case2"),
      SymbolAddress("jt_identity_mask_or_negative_case4"),
      SymbolAddress("jt_identity_mask_or_negative_case6")};
  const std::vector<neverd::va_t> PoisonTargets = {
      SymbolAddress("jt_identity_mask_or_negative_poison1"),
      SymbolAddress("jt_identity_mask_or_negative_poison3"),
      SymbolAddress("jt_identity_mask_or_negative_poison5"),
      SymbolAddress("jt_identity_mask_or_negative_poison7")};

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const neverd::JumpTable &Table = Low.JumpTables.front();
  EXPECT_EQ(Table.CaseLabels, (std::vector<int64_t>{0, 2, 4, 6}));
  EXPECT_TRUE(Table.HasDispatchSlotMap);
  EXPECT_EQ(Table.SlotIndices, (std::vector<uint32_t>{0, 2, 4, 6}));
  EXPECT_EQ(Table.Targets, ExpectedTargets)
      << "the low-bit OR proves only the four even physical coordinates";
  for (neverd::va_t Poison : PoisonTargets)
    EXPECT_EQ(std::find(Table.Targets.begin(), Table.Targets.end(), Poison),
              Table.Targets.end())
        << "an adjacent odd poison slot was published";
}

TEST_F(JTE_X86_64,
       MaskKnownOneWitnessDoesNotLeakFromUnrelatedSiblingPath) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_or_unrelated_sibling");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_or_unrelated_sibling_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the regression requires eight physical code-pointer slots";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  size_t Mask7Count = 0;
  bool SawOrOne = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      auto HasScalar = [&](uint64_t Value) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value)
            return true;
        return false;
      };
      Mask7Count += Op.Opcode == neverd::NdOp::INT_AND && HasScalar(7);
      SawOrOne |= Op.Opcode == neverd::NdOp::INT_OR && HasScalar(1);
    }
  ASSERT_GE(Mask7Count, 2u)
      << "both the dispatch mask and reachable sibling mask must be decoded";
  ASSERT_TRUE(SawOrOne)
      << "the unrelated sibling must expose a real known-one producer";

  ASSERT_EQ(Low.JumpTables.size(), 1u);
  const neverd::JumpTable &JumpTable = Low.JumpTables.front();
  EXPECT_EQ(JumpTable.Targets.size(), 8u)
      << "the sibling OR must not narrow the independent x&7 selector";
  EXPECT_EQ(JumpTable.CaseLabels,
            (std::vector<int64_t>{0, 1, 2, 3, 4, 5, 6, 7}));
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JTE_X86_64,
       MaskKnownOneFixedPointRejectsSelfBootstrappedProducer) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_or_fp_self_bootstrap");
  const neverd::Symbol *Table =
      Image.findSymbol("jt_identity_mask_or_fp_self_bootstrap_table");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Table, nullptr);
  ASSERT_EQ(Table->Size, 8u * 8u);
  EXPECT_EQ(std::count_if(Image.CodePtrRelocSlots.begin(),
                          Image.CodePtrRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= Table->Addr &&
                                   Slot < Table->Addr + Table->Size;
                          }),
            8)
      << "the provisional proposal must expose all eight physical targets";

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  bool SawOrOne = false;
  bool SawMask7 = false;
  bool SawSubtractOne = false;
  const neverd::LowBlock *Dispatch = nullptr;
  neverd::va_t DispatchAddr = neverd::InvalidVA;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      auto HasScalar = [&](uint64_t Value) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value)
            return true;
        return false;
      };
      SawOrOne |= Op.Opcode == neverd::NdOp::INT_OR && HasScalar(1);
      SawMask7 |= Op.Opcode == neverd::NdOp::INT_AND && HasScalar(7);
      SawSubtractOne |= Op.Opcode == neverd::NdOp::INT_SUB && HasScalar(1);
      if (Op.Opcode == neverd::NdOp::INDIR_BR) {
        ASSERT_EQ(Dispatch, nullptr)
            << "the fixture must contain one indirect dispatch";
        Dispatch = &Block;
        DispatchAddr = Op.Addr;
      }
    }
  ASSERT_TRUE(SawOrOne);
  ASSERT_TRUE(SawMask7);
  ASSERT_TRUE(SawSubtractOne)
      << "case two must contain the complete (x|1)&7; -1 producer";

  ASSERT_NE(Dispatch, nullptr);
  EXPECT_TRUE(Dispatch->Succs.empty());
  EXPECT_EQ(Low.UnsafeIndirectBranchAddresses.count(DispatchAddr), 1u);
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.JumpTables.empty())
      << "a provisional case edge cannot authenticate the known-one producer "
         "that would keep that same case reachable";
}

TEST_F(JTE_X86_64, MaskKnownOneLineageIgnoresPrunedProvisionalSibling) {
  auto ImageOrErr = neverd::loadBinary(identityCfgLaneObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_identity_mask_or_pruned_sibling_clean");
  const neverd::Symbol *TableSymbol =
      Image.findSymbol("jt_identity_mask_or_pruned_sibling_clean_table");
  const neverd::Symbol *FirstCase =
      Image.findSymbol("jt_identity_mask_or_pruned_sibling_case0");
  const neverd::Symbol *SiblingBegin =
      Image.findSymbol("jt_identity_mask_or_pruned_sibling_begin");
  const neverd::Symbol *SiblingEnd =
      Image.findSymbol("jt_identity_mask_or_pruned_sibling_end");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(TableSymbol, nullptr);
  ASSERT_NE(FirstCase, nullptr);
  ASSERT_NE(SiblingBegin, nullptr);
  ASSERT_NE(SiblingEnd, nullptr);
  ASSERT_LT(Function->Addr, FirstCase->Addr);
  ASSERT_LT(SiblingBegin->Addr, SiblingEnd->Addr);
  ASSERT_EQ(TableSymbol->Size, 8u * 4u);
  EXPECT_EQ(std::count_if(Image.RelCodeRelocSlots.begin(),
                          Image.RelCodeRelocSlots.end(),
                          [&](neverd::va_t Slot) {
                            return Slot >= TableSymbol->Addr &&
                                   Slot < TableSymbol->Addr + TableSymbol->Size;
                          }),
            8)
      << "the four excluded odd slots must be genuine adjacent code "
         "relocations";

  auto SymbolAddress = [&](const char *Name) {
    const neverd::Symbol *Symbol = Image.findSymbol(Name);
    EXPECT_NE(Symbol, nullptr) << Name;
    return Symbol ? Symbol->Addr : neverd::va_t{0};
  };
  const std::vector<neverd::va_t> ExpectedTargets = {
      SymbolAddress("jt_identity_mask_or_pruned_sibling_case0"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_case2"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_case4"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_case6")};
  const std::vector<neverd::va_t> PoisonTargets = {
      SymbolAddress("jt_identity_mask_or_pruned_sibling_poison1"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_poison3"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_poison5"),
      SymbolAddress("jt_identity_mask_or_pruned_sibling_poison7")};

  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image.Arch, Image.Mode));
  neverd::CFGBuilder Builder;
  const neverd::LowFunc Low =
      Builder.build(Image, Decoder, Function->Addr, Function->Name);

  bool SawCleanOrOne = false;
  bool SawCleanMaskSeven = false;
  bool SawCleanSubtractOne = false;
  bool RetainedPrunedSibling = false;
  for (const neverd::LowBlock &Block : Low.Blocks)
    for (const neverd::LowOp &Op : Block.Ops) {
      auto HasScalar = [&](uint64_t Value) {
        for (uint8_t I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value)
            return true;
        return false;
      };
      if (Op.Addr >= Function->Addr && Op.Addr < FirstCase->Addr) {
        SawCleanOrOne |= Op.Opcode == neverd::NdOp::INT_OR && HasScalar(1);
        SawCleanMaskSeven |= Op.Opcode == neverd::NdOp::INT_AND && HasScalar(7);
        SawCleanSubtractOne |=
            Op.Opcode == neverd::NdOp::INT_SUB && HasScalar(1);
      }
      RetainedPrunedSibling |=
          Op.Addr >= SiblingBegin->Addr && Op.Addr < SiblingEnd->Addr;
    }
  ASSERT_TRUE(SawCleanOrOne);
  ASSERT_TRUE(SawCleanMaskSeven);
  ASSERT_TRUE(SawCleanSubtractOne)
      << "the entry graph must contain its complete independent sparse proof";
  EXPECT_FALSE(RetainedPrunedSibling)
      << "the unrelated OR recipe must disappear with its provisional odd "
         "case root";

  ASSERT_EQ(Low.JumpTables.size(), 1u)
      << "removing the unrelated sibling root must not remove the clean "
         "selector's witness";
  const neverd::JumpTable &Table = Low.JumpTables.front();
  ASSERT_EQ(Table.Targets.size(), 4u);
  EXPECT_EQ(Table.Targets, ExpectedTargets);
  EXPECT_EQ(Table.CaseLabels, (std::vector<int64_t>{0, 2, 4, 6}));
  EXPECT_TRUE(Table.HasDispatchSlotMap);
  ASSERT_EQ(Table.SlotIndices.size(), 4u);
  EXPECT_EQ(Table.SlotIndices, (std::vector<uint32_t>{0, 2, 4, 6}));
  for (neverd::va_t Poison : PoisonTargets)
    EXPECT_EQ(std::find(Table.Targets.begin(), Table.Targets.end(), Poison),
              Table.Targets.end())
        << "a provisional odd sibling escaped the exact sparse domain";
  EXPECT_FALSE(lowFunctionHasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}
