//===- IndirectTailFrameTests.cpp - Native tail-transfer frame guards -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "IndirectTailFrame.h"
#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"

#include <fstream>
#include <iterator>

namespace {

class IndirectTailFrame : public NeverDLiftTest {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path compileTailFrameFixture() {
    const auto Source =
        fs::path(TEST_SOURCE_DIR) / "aarch64/test_indirect_tail_frame_a64.S";
    const auto Object = tmpFile("tail-frame.o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               const std::string &Name) {
    const auto *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    neverd::CFGBuilder Builder;
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  static bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == Opcode)
          return true;
    return false;
  }
};

TEST_F(IndirectTailFrame, ActiveOrUnknownTailFrameRetainsItsIndirectBranch) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tail_frame_live",           "tail_frame_unknown",
        "tail_frame_join",           "tail_link_changed",
        "tail_link_unrestored",      "tail_link_slot_changed",
        "tail_link_slot_partial",    "tail_frame_borrowed",
        "tail_stack_target",         "tail_link_target",
        "tail_frame_loop",           "tail_link_loop",
        "tail_independent_root",     "tail_exclusive_live",
        "tail_exclusive_link_slot",  "tail_atomic_link_slot",
        "tail_exclusive_escape",     "tail_atomic_escape",
        "tail_exclusive_link_load",  "tail_unmodelled_intrinsic",
        "tail_frame_alias",          "tail_exposed_frame_alias",
        "tail_reloaded_frame_alias", "tail_narrow_frame_alias",
        "tail_outside_frame_alias",  "tail_link_from_sysreg"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

TEST_F(IndirectTailFrame,
       RestoredNativeFrameRetainsItsFunctionPointerTailCall) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tail_leaf", "tail_frame_balanced", "tail_frame_diamond",
        "tail_link_restored", "tail_loop_balanced", "tail_exclusive_release",
        "tail_exclusive_pair", "tail_lse_atomics", "tail_value_intrinsic",
        "tail_unknown_alias", "tail_saved_link_foreign_store",
        "tail_after_trap_path", "tail_udf_padding", "tail_thread_pointer"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));
  }
}

// A frame description that only unwinds -- the FDE every function of an
// ordinary shared library has -- leaves a restored frame's branch a tail call,
// while a live frame still keeps its indirect branch, and so does a computed
// jump into the function's own code (a switch whose table stays unresolved),
// though not a branch to the function's own entry, which recurses.
TEST_F(IndirectTailFrame, UnwindOnlyFrameDescriptionKeepsTheTailCall) {
  const auto Source = tmpFile("tail-cfi.s");
  std::ofstream(Source) << R"(.text
.p2align 2
.globl tail_cfi_leaf
.type tail_cfi_leaf,@function
tail_cfi_leaf:
  .cfi_startproc
  br x0
  .cfi_endproc
.size tail_cfi_leaf,.-tail_cfi_leaf
.p2align 2
.globl tail_cfi_live
.type tail_cfi_live,@function
tail_cfi_live:
  .cfi_startproc
  sub sp, sp, #64
  .cfi_def_cfa_offset 64
  br x0
  .cfi_endproc
.size tail_cfi_live,.-tail_cfi_live
.p2align 2
.globl tail_cfi_switch
.type tail_cfi_switch,@function
tail_cfi_switch:
  .cfi_startproc
  adr x1, 1f
  add x0, x1, w0, sxtw #2
  br x0
1:
  mov w0, #1
  ret
  .cfi_endproc
.size tail_cfi_switch,.-tail_cfi_switch
.p2align 2
.globl tail_cfi_self
.type tail_cfi_self,@function
tail_cfi_self:
.Ltail_cfi_self:
  .cfi_startproc
  sub sp, sp, #16
  .cfi_def_cfa_offset 16
  adr x1, .Ltail_cfi_self
  str x1, [sp, #8]
  ldr x1, [sp, #8]
  add sp, sp, #16
  .cfi_def_cfa_offset 0
  cbz x0, 1f
  sub x0, x0, #1
  br x1
1:
  ret
  .cfi_endproc
.size tail_cfi_self,.-tail_cfi_self
)";
  const auto Object = tmpFile("tail-cfi.o");
  const auto Library = tmpFile("tail-cfi.so");
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  const auto Linked =
      exec("ld.lld", {"-shared", Object.string(), "-o", Library.string()});
  ASSERT_TRUE(Linked.ok()) << Linked.err;
  auto Image = neverd::loadBinary(Library.string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  const auto Leaf = build(*Image, "tail_cfi_leaf");
  ASSERT_TRUE(Leaf.ExceptionMetadata.has_value());
  EXPECT_TRUE(Leaf.ExceptionMetadata->describesUnwindOnly());
  EXPECT_FALSE(hasOpcode(Leaf, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Leaf, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(hasOpcode(Leaf, neverd::NdOp::RETURN));

  const auto Live = build(*Image, "tail_cfi_live");
  EXPECT_TRUE(hasOpcode(Live, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Live, neverd::NdOp::INDIR_CALL));

  const auto Switch = build(*Image, "tail_cfi_switch");
  ASSERT_TRUE(Switch.ExceptionMetadata.has_value());
  EXPECT_TRUE(hasOpcode(Switch, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Switch, neverd::NdOp::INDIR_CALL));
  // A branch to the function's own entry recurses: it stays a tail call.
  const auto Self = build(*Image, "tail_cfi_self");
  EXPECT_FALSE(hasOpcode(Self, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Self, neverd::NdOp::INDIR_CALL));
}

// A C++ table lets a branch out of the frame be a tail call where the call
// site covering it names no landing pad: an exception there goes on to the
// caller either way.  An address the table leaves out terminates under the
// C++ personality, as a noexcept destructor's empty table makes it.
TEST_F(IndirectTailFrame, ItaniumTableForwardsOnlyFromCoveredBranches) {
  const auto Source = tmpFile("tail-lsda.s");
  std::ofstream(Source) << R"(.text
.p2align 2
.globl tail_lsda_forward
.type tail_lsda_forward,@function
tail_lsda_forward:
.Lforward_begin:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.__gxx_personality_v0
  .cfi_lsda 0x1b, .Lforward_table
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset w30, -8
  .cfi_offset w29, -16
  bl helper
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  br x0
.Lforward_end:
  .cfi_endproc
.size tail_lsda_forward,.-tail_lsda_forward
.p2align 2
.globl tail_lsda_noexcept
.type tail_lsda_noexcept,@function
tail_lsda_noexcept:
  .cfi_startproc
  .cfi_personality 0x9b, DW.ref.__gxx_personality_v0
  .cfi_lsda 0x1b, .Lnoexcept_table
  br x0
  .cfi_endproc
.size tail_lsda_noexcept,.-tail_lsda_noexcept
.p2align 2
.globl helper
.type helper,@function
helper:
  ret
.size helper,.-helper
.section .gcc_except_table,"a",@progbits
.p2align 2
.Lforward_table:
  .byte 255
  .byte 255
  .byte 1
  .uleb128 .Lforward_sites_end-.Lforward_sites
.Lforward_sites:
  .uleb128 .Lforward_begin-.Lforward_begin
  .uleb128 .Lforward_end-.Lforward_begin
  .byte 0
  .byte 0
.Lforward_sites_end:
.p2align 2
.Lnoexcept_table:
  .byte 255
  .byte 255
  .byte 1
  .uleb128 0
.data
.hidden DW.ref.__gxx_personality_v0
.weak DW.ref.__gxx_personality_v0
.p2align 3
.type DW.ref.__gxx_personality_v0,@object
.size DW.ref.__gxx_personality_v0,8
DW.ref.__gxx_personality_v0:
  .xword __gxx_personality_v0
)";
  const auto Object = tmpFile("tail-lsda.o");
  const auto Library = tmpFile("tail-lsda.so");
  const auto Built =
      exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu", "-c",
                               Source.string(), "-o", Object.string()});
  ASSERT_TRUE(Built.ok()) << Built.err;
  const auto Linked =
      exec("ld.lld", {"-shared", Object.string(), "-o", Library.string()});
  ASSERT_TRUE(Linked.ok()) << Linked.err;
  auto Image = neverd::loadBinary(Library.string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());

  const auto Forward = build(*Image, "tail_lsda_forward");
  ASSERT_TRUE(Forward.ExceptionMetadata.has_value());
  EXPECT_EQ(Forward.ExceptionMetadata->Personality,
            neverd::ExceptionPersonality::GxxPersonalityV0);
  ASSERT_TRUE(Forward.ExceptionMetadata->Itanium.has_value());
  EXPECT_FALSE(Forward.ExceptionMetadata->describesUnwindOnly());
  EXPECT_FALSE(hasOpcode(Forward, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Forward, neverd::NdOp::INDIR_CALL));

  const auto Noexcept = build(*Image, "tail_lsda_noexcept");
  ASSERT_TRUE(Noexcept.ExceptionMetadata.has_value());
  ASSERT_TRUE(Noexcept.ExceptionMetadata->Itanium.has_value());
  EXPECT_TRUE(Noexcept.ExceptionMetadata->Itanium->CallSites.empty());
  EXPECT_TRUE(hasOpcode(Noexcept, neverd::NdOp::INDIR_BR));
  EXPECT_FALSE(hasOpcode(Noexcept, neverd::NdOp::INDIR_CALL));
}

// Only a record that names no handler of the frame lets a branch out of it be
// a tail call.
TEST(ExceptionFunctionUnwindOnly, RecordsThatNameAHandlerDoNot) {
  neverd::ExceptionFunction Plain;
  Plain.Dwarf.emplace();
  EXPECT_TRUE(Plain.describesUnwindOnly());
  neverd::ExceptionFunction WithLSDA = Plain;
  WithLSDA.Dwarf->LSDAVA = 0x4000;
  EXPECT_FALSE(WithLSDA.describesUnwindOnly());
  neverd::ExceptionFunction WithPersonality = Plain;
  WithPersonality.PersonalityVA = 0x5000;
  EXPECT_FALSE(WithPersonality.describesUnwindOnly());
  neverd::ExceptionFunction WithHandlerData = Plain;
  WithHandlerData.HandlerDataVA = 0x6000;
  EXPECT_FALSE(WithHandlerData.describesUnwindOnly());
  neverd::ExceptionFunction Partial = Plain;
  Partial.ParseStatus = neverd::ExceptionParseStatus::Partial;
  EXPECT_FALSE(Partial.describesUnwindOnly());
}

TEST_F(IndirectTailFrame, UnresolvedLiveFrameCannotPublishACallReturn) {
  const auto Object = compileTailFrameFixture();
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto File = tmpFile("tail-frame-" + std::to_string(Route) + ".c");
    std::vector<std::string> Args = {"decompile", Object.string(),
                                     "--func",    "tail_frame_live",
                                     "-o",        File.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    std::ifstream Input(File);
    const std::string Source((std::istreambuf_iterator<char>(Input)), {});
    EXPECT_NE(Source.find("__builtin_trap"), std::string::npos) << Source;
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-fsyntax-only", "-Werror", File.string()});
    EXPECT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
  }
}

// The refcount release before libc++'s virtual tail call reaches every C
// route as a call through the vtable slot it loads, not as a trap.
TEST_F(IndirectTailFrame, ExclusiveReleaseCallsItsVirtualSlotInEveryRoute) {
  const auto Object = compileTailFrameFixture();
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto File = tmpFile("tail-release-" + std::to_string(Route) + ".c");
    std::vector<std::string> Args = {"decompile", Object.string(),
                                     "--func",    "tail_exclusive_release",
                                     "-o",        File.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    std::ifstream Input(File);
    const std::string Source((std::istreambuf_iterator<char>(Input)), {});
    EXPECT_EQ(Source.find("unresolved indirect branch"), std::string::npos)
        << Source;
    // Both routes write the slot offset in its natural base.
    const std::string Slot = "0x20";
    EXPECT_NE(Source.find("(*(void **)((uintptr_t)(*(void **)(arg0)) + " +
                          Slot + ")))("),
              std::string::npos)
        << Source;
    if (Route)
      continue;
    // The early return hands back the object the caller passed.
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", "aarch64-linux-gnu",
                                 "-fsyntax-only", "-w", File.string()});
    EXPECT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
  }
}

TEST_F(IndirectTailFrame, IncompleteOrExhaustedGuardPublishesNoCandidate) {
  using namespace neverd;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  LowFunc Function;
  Function.Entry = 0x1000;
  Function.DecodedInstructionCount = Function.LiftedInstructionCount = 1;
  Function.ModuleAnalysisRoots.insert(Function.Entry);
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Function.Entry;
  Block.EndAddr = Function.Entry + 4;
  LowOp Branch;
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Addr = Function.Entry;
  Branch.addInput(NdVar::reg(TRI.IntParamRegs.front(), 8));
  Block.Ops.push_back(Branch);
  LowInstructionBoundary Boundary;
  Boundary.Address = Function.Entry;
  Boundary.Size = 4;
  Boundary.OpCount = 1;
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags =
      LowInstructionControlFlag::Branch | LowInstructionControlFlag::Indirect;
  Block.InstructionBoundaries.push_back(Boundary);
  Function.Blocks.push_back(std::move(Block));
  EXPECT_EQ(restoredAArch64IndirectTailFrames(Function, BinaryFormat::ELF),
            (std::set<va_t>{Function.Entry}));
  for (size_t Budget : {size_t{0}, size_t{1}})
    EXPECT_TRUE(
        restoredAArch64IndirectTailFrames(Function, BinaryFormat::ELF, Budget)
            .empty());

  auto Incomplete = Function;
  Incomplete.Blocks[0].InstructionBoundaries[0].OpCount = 0;
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
  Incomplete = Function;
  Incomplete.ModuleAnalysisRoots.insert(Function.Entry + 1);
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
  Incomplete = Function;
  Incomplete.Blocks[0].Succs.push_back(1);
  EXPECT_TRUE(
      restoredAArch64IndirectTailFrames(Incomplete, BinaryFormat::ELF).empty());
}

// An exclusive or atomic intrinsic names its address and the value it writes
// only in the exact shape the AArch64 lifter emits; anything else keeps the
// guard's refusal.
TEST(LowIntrinsicMemoryOperands, DecodeOnlyTheLiftedShapes) {
  using namespace neverd;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const NdVar Address = NdVar::reg(TRI.IntParamRegs[1], 8);
  const NdVar Value = NdVar::reg(TRI.IntParamRegs[2], 8);
  const NdVar Width = NdVar::cst(8, 2);
  const auto make = [](Intrinsic Id, NdVar Output,
                       std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = Output;
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
    for (const NdVar &Input : Inputs)
      Op.addInput(Input);
    return Op;
  };

  const LowOp Load =
      make(Intrinsic::A64_Ldaxr, NdVar::tmp(0, 8), {Address, Width});
  const auto Loaded = lowIntrinsicMemoryOperands(Load);
  ASSERT_TRUE(Loaded.Complete);
  EXPECT_EQ(Loaded.Address, &Load.Inputs[1]);
  EXPECT_EQ(Loaded.StoredValue, nullptr);
  EXPECT_EQ(Loaded.AccessSize, 8);

  const LowOp Store =
      make(Intrinsic::A64_Stlxr, NdVar::reg(TRI.IntParamRegs[3], 4),
           {Value, Address, Width});
  const auto Stored = lowIntrinsicMemoryOperands(Store);
  ASSERT_TRUE(Stored.Complete);
  EXPECT_EQ(Stored.Address, &Store.Inputs[2]);
  EXPECT_EQ(Stored.StoredValue, &Store.Inputs[1]);

  const LowOp Combine = make(Intrinsic::A64_AtomicOr, NdVar::tmp(0, 8),
                             {Value, Address, Width, NdVar::cst(5, 1)});
  const auto Combined = lowIntrinsicMemoryOperands(Combine);
  ASSERT_TRUE(Combined.Complete);
  EXPECT_EQ(Combined.Address, &Combine.Inputs[2]);
  EXPECT_EQ(Combined.StoredValue, &Combine.Inputs[1]);

  const LowOp Malformed[] = {
      // The loaded value disagrees with the access width.
      make(Intrinsic::A64_Ldaxr, NdVar::tmp(0, 4), {Address, Width}),
      // The width is not a constant, or is missing.
      make(Intrinsic::A64_Ldaxr, NdVar::tmp(0, 8), {Address, Value}),
      make(Intrinsic::A64_Ldaxr, NdVar::tmp(0, 8), {Address}),
      // The stored value is narrower than the access.
      make(Intrinsic::A64_Stlxr, NdVar::tmp(0, 4),
           {NdVar::reg(TRI.IntParamRegs[2], 4), Address, Width}),
      // The ordering is not a constant.
      make(Intrinsic::A64_AtomicOr, NdVar::tmp(0, 8),
           {Value, Address, Width, Value}),
      // A supervisor call has no memory contract.
      make(Intrinsic::Svc, NdVar::reg(TRI.IntParamRegs[0], 8),
           {NdVar::cst(0, 2)}),
  };
  for (const LowOp &Op : Malformed)
    EXPECT_FALSE(lowIntrinsicMemoryOperands(Op).Complete);
  LowOp Segmented = Load;
  Segmented.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  EXPECT_FALSE(lowIntrinsicMemoryOperands(Segmented).Complete);
  LowOp Plain = Load;
  Plain.Opcode = NdOp::LOAD;
  EXPECT_FALSE(lowIntrinsicMemoryOperands(Plain).Complete);
}

// A system register read writes only its destination, and a trap ends its
// path whatever output its lift names; neither holds for another shape or
// another intrinsic.
TEST(LowIntrinsicEffects, OutputOnlyReadsAndTraps) {
  using namespace neverd;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  const auto make = [](Intrinsic Id, NdVar Output,
                       std::initializer_list<NdVar> Inputs) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = Output;
    Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
    for (const NdVar &Input : Inputs)
      Op.addInput(Input);
    return Op;
  };
  const NdVar Register = NdVar::reg(TRI.IntParamRegs[1], 8);
  EXPECT_TRUE(
      lowIntrinsicWritesOnlyItsOutput(make(Intrinsic::Mrs, Register, {})));
  EXPECT_FALSE(lowIntrinsicWritesOnlyItsOutput(
      make(Intrinsic::Mrs, Register, {Register})));
  EXPECT_FALSE(lowIntrinsicWritesOnlyItsOutput(make(Intrinsic::Mrs, {}, {})));
  EXPECT_FALSE(lowIntrinsicWritesOnlyItsOutput(
      make(Intrinsic::Msr, Register, {Register})));
  EXPECT_TRUE(lowIntrinsicIsUnconditionalTrap(make(Intrinsic::Brk, {}, {})));
  EXPECT_TRUE(
      lowIntrinsicIsUnconditionalTrap(make(Intrinsic::Brk, Register, {})));
  EXPECT_FALSE(lowIntrinsicIsUnconditionalTrap(
      make(Intrinsic::Svc, Register, {NdVar::cst(0, 2)})));
}

} // namespace
