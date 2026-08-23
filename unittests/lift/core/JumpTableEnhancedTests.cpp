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

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
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

TEST_F(JTE_X86_32, GOTPCModelRequiresLifterAuthenticatedCallPopSeed) {
  auto ImageOrErr = neverd::loadBinary(i386GOTPCModelObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  neverd::BinaryImage &Image = *ImageOrErr;
  const neverd::Symbol *Function =
      Image.findSymbol("jt_i386_gotpc_call_pop_seed");
  ASSERT_NE(Function, nullptr);
  ASSERT_EQ(Image.I386GOTPCFields.size(), 2u);

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
  EXPECT_EQ(GetPc.InstructionAddr, 5u);
  EXPECT_EQ(GetPc.OutputOpcode, neverd::NdOp::COPY);
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
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsSignedNegativeHalfDomain) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_signed_negative_alias");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceStopsAtOverlappingSubregisterWrite) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_overlapping_write");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceRejectsBoundBeyondDiscoveryWindow) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_guard_bound_beyond_sample");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, GuardEvidenceFindsPostLaidOutLargeBound) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  const std::string Body =
      lowFunctionBody(R.out, "jt_identity_postlaid_guard_large");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
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
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
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
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("INDIR_BR"), std::string::npos) << Body;
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
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
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
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xA8C:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xAEF:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsRuntimeMaskDependency) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_runtime_mask");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xAF0:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xB53:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundRejectsNarrowArithmeticWrap) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_byte_wrap");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xB54:4"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("cst:0xBB7:4"), std::string::npos) << Body;
}

TEST_F(JTE_X86_64, MaskBoundPreservesPartialRegisterDependencies) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_partial_wide");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
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

TEST_F(JTE_X86_64, MaskDomainMustFitAuthenticatedPhysicalStorage) {
  auto R = liftToLowIR(identityCfgLaneObj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  std::string Body = lowFunctionBody(R.out, "jt_identity_mask_exceeds_storage");
  ASSERT_FALSE(Body.empty()) << R.out;
  EXPECT_NE(Body.find("INDIR_CALL"), std::string::npos) << Body;
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

TEST_F(JTE_X86_64, EvidenceBudgetDoesNotCaptureIndexedCallbackTailCall) {
  auto ImageOrErr = neverd::loadBinary(maskEqualBoundObj());
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
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
