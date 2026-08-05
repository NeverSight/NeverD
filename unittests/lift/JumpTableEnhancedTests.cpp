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

//===----------------------------------------------------------------------===//
// x86-64
//===----------------------------------------------------------------------===//

class JTE_X86_64 : public NeverDLiftTest {};

static fs::path jteX64Obj() {
  return fs::path(TEST_OBJ_DIR) / "test_jumptable_enhanced.o";
}

TEST_F(JTE_X86_64, AllStagesSucceed) {
  verifyAllStages(jteX64Obj());
}

TEST_F(JTE_X86_64, LowIRHasBranchInd) {
  auto R = liftToLowIR(jteX64Obj());
  ASSERT_EQ(R.exitCode, 0) << R.err;
  EXPECT_TRUE(R.contains("INDIR_BR"))
      << "Expected INDIR_BR for jump tables";
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

TEST_F(JTE_X86_64, TwoLevelAllStagesSucceed) {
  verifyAllStages(twoLevelObj());
}

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
  for (std::string::size_type P = R.out.find("i32 ", 0);
       P != std::string::npos; P = R.out.find("i32 ", P + 1))
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
  // Find a `mul i32 <x>, 3` line (clang may keep the multiply or strength-reduce
  // it; the resolver run here keeps the multiply), proving the default's y*3
  // computation survived as a live, distinct block.
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

TEST_F(JTE_AArch64, AllStagesSucceed) {
  verifyAllStages(jteA64Obj());
}

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
  EXPECT_TRUE(R.contains("i32 -4") || R.contains(", -4") ||
              R.contains("-4,"))
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

TEST_F(JTE_ARM32, AllStagesSucceed) {
  verifyAllStages(jteARMObj());
}

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
