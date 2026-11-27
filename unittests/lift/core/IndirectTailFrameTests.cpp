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

neverd::LowFunc build(const neverd::BinaryImage &Image,
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

bool hasOpcode(const neverd::LowFunc &Function, neverd::NdOp Opcode) {
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      if (Op.Opcode == Opcode)
        return true;
  return false;
}

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
};

// The x86-64 fixture is linked and stripped at build time.
class X64IndirectTailFrame : public NeverDLiftTest {
protected:
  static fs::path linkedFixture() {
    return fs::path(TEST_OBJ_DIR) / "test_indirect_tail_frame_x64.so";
  }
};

// An x86-64 indirect jump is a tail call only when every path to it leaves
// RSP where the caller's CALL left the return address: after pops, LEAVE
// (also undoing a realignment), an added-back frame, `lea -8(%rbp), %rsp`,
// a balanced diamond or loop, or with nothing pushed.  One whose RSP derives
// from nothing of its incoming stack pointer runs on another stack: a fiber
// switch to an argument's stack, or a resumption that loads RSP from memory.
TEST_F(X64IndirectTailFrame, RestoredStackPointerKeepsTheTailCall) {
  auto Image = neverd::loadBinary(linkedFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name : {"tf_leaf", "tf_pop_frame", "tf_leave", "tf_aligned",
                           "tf_add_back", "tf_diamond", "tf_lea_restore",
                           "tf_loop_balanced", "tf_switch", "tf_resume"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));
  }
}

// PLT0 pushes the lazy-binding resolver's argument and jumps through the
// DT_PLTGOT slot the dynamic linker fills with that resolver, so its jump
// stays a tail call although RSP sits a word below its incoming value.
TEST_F(X64IndirectTailFrame, LazyBindingResolverJumpKeepsTheTailCall) {
  auto Image = neverd::loadBinary(linkedFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  const neverd::Section *Plt = nullptr;
  for (const neverd::Section &Section : Image->Sections)
    if (Section.Name == ".plt")
      Plt = &Section;
  ASSERT_NE(Plt, nullptr);
  neverd::Decoder Decoder;
  ASSERT_TRUE(Decoder.init(Image->Arch, Image->Mode));
  neverd::CFGBuilder Builder;
  const auto Low = Builder.build(*Image, Decoder, Plt->VA, "plt0");
  EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));

  // tf_ext's PLT entry jumps through its GOT slot, which names the entry's
  // own push until the dynamic linker binds it.  Writable memory is no table
  // of the entry's labels, so that jump stays a tail call too.
  ASSERT_GE(Plt->Size, 32u);
  neverd::CFGBuilder EntryBuilder;
  const auto Entry = EntryBuilder.build(*Image, Decoder, Plt->VA + 16, "plt1");
  EXPECT_FALSE(hasOpcode(Entry, neverd::NdOp::INDIR_BR));
  EXPECT_TRUE(hasOpcode(Entry, neverd::NdOp::INDIR_CALL));
  EXPECT_TRUE(hasOpcode(Entry, neverd::NdOp::RETURN));
}

// A live, disagreeing or unknown frame keeps the jump an unresolved branch:
// its target would return to whatever the frame left on the stack.  No
// relocation names tf_live_table's table in the stripped library, so only its
// frame keeps the case labels from being called as functions.
TEST_F(X64IndirectTailFrame, LiveOrUnknownStackPointerKeepsTheBranch) {
  auto Image = neverd::loadBinary(linkedFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tf_live", "tf_live_table", "tf_join", "tf_aligned_lost", "tf_dynamic",
        "tf_frame_lost", "tf_loop_push"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

// A jump through a read-only table of the function's own labels stays within
// the function whatever its frame: calling a case label as a function would
// drop the cases from the decompiled body.  Neither table is bounded, and the
// stripped library names neither, yet each first entry decodes to a label of
// its function, also through a base RBX keeps across a call.
TEST_F(X64IndirectTailFrame, OwnLabelTableKeepsTheBranch) {
  auto Image = neverd::loadBinary(linkedFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name : {"tf_leaf_table", "tf_table_kept"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    ASSERT_FALSE(Low.Blocks.empty());
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

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

TEST_F(IndirectTailFrame, IndexedMemoryKeepsRestoredFrameEvidence) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name : {"tail_indexed_leaf", "tail_indexed_signed",
                           "tail_indexed_virtual", "tail_indexed_foreign_store",
                           "tail_indexed_vector", "tail_indexed_after_call"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::RETURN));
  }
}

TEST_F(IndirectTailFrame, IndexedMemoryRetainsFrameAliasesAndUnknownLinks) {
  auto Image = neverd::loadBinary(compileTailFrameFixture().string());
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  for (const char *Name :
       {"tail_indexed_link_slot", "tail_indexed_frame_index",
        "tail_indexed_frame_escape", "tail_indexed_link_load",
        "tail_indexed_stack_target", "tail_indexed_local_target"}) {
    SCOPED_TRACE(Name);
    const auto Low = build(*Image, Name);
    EXPECT_TRUE(hasOpcode(Low, neverd::NdOp::INDIR_BR));
    EXPECT_FALSE(hasOpcode(Low, neverd::NdOp::INDIR_CALL));
  }
}

TEST_F(IndirectTailFrame, IndexedVirtualTailMatchesNativeReceiverAndIndex) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "native AArch64 execution is unavailable";
#else
  const auto Object = compileTailFrameFixture();
  const auto Original = tmpFile("indexed-original.S");
  std::ofstream(Original) << R"(
#ifdef __APPLE__
#define original_indexed_tail _original_indexed_tail
#endif
.text
.p2align 2
.globl original_indexed_tail
original_indexed_tail:
  ldr x9,[x0,#8]
  ldr x0,[x9,x1,lsl #3]
  ldr x10,[x0]
  ldr x16,[x10,#40]
  br x16
)";
  for (unsigned Route : {0u, 1u, 2u}) {
    SCOPED_TRACE(Route);
    const auto File = tmpFile("indexed-tail-" + std::to_string(Route) + ".c");
    std::vector<std::string> Args = {"decompile", Object.string(),
                                     "--func",    "tail_indexed_virtual",
                                     "-o",        File.string()};
    if (Route)
      Args.push_back("--llvm");
    if (Route == 2)
      Args.push_back("--no-opt");
    const auto Recovered = exec(ndBin(), Args);
    ASSERT_TRUE(Recovered.ok()) << Recovered.err;
    std::ifstream Input(File);
    const std::string Source((std::istreambuf_iterator<char>(Input)), {});
    ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    std::ofstream(File, std::ios::app) << R"(
struct object { uintptr_t *vtable; uint64_t payload; };
struct container { uint64_t unused; struct object **entries; };
extern uint64_t original_indexed_tail(void *, uint64_t);
static uint64_t calls;
static uintptr_t receiver;
static uint64_t callback_a(uintptr_t p, uint64_t index) {
  struct object *o = (struct object *)p;
  ++calls;
  receiver = p;
  const uint64_t value = o->payload + index * 17;
  o->payload += 1;
  return value ^ UINT64_C(0xA5A55A5A12345678);
}
static uint64_t callback_b(uintptr_t p, uint64_t index) {
  struct object *o = (struct object *)p;
  ++calls;
  receiver = p;
  const uint64_t value = o->payload + index * 17;
  o->payload += 2;
  return value ^ UINT64_C(0x5A5AA5A887654321);
}
int main(void) {
  uintptr_t tables[2][6] = {{0}, {0}};
  tables[0][5] = (uintptr_t)&callback_a;
  tables[1][5] = (uintptr_t)&callback_b;
  struct object objects[16];
  struct object *entries[16];
  for (unsigned i = 0; i < 16; ++i) {
    objects[i].vtable = tables[i & 1];
    entries[i] = &objects[i];
  }
  struct container c = {0, entries};
  for (uint64_t round = 0; round < 7; ++round) {
    for (uint64_t i = 0; i < 16; ++i) {
      const uint64_t initial = UINT64_C(0xFEDCBA9876543210) * round + i;
      const uint64_t salt = (i & 1) ? UINT64_C(0x5A5AA5A887654321)
                                   : UINT64_C(0xA5A55A5A12345678);
      const uint64_t expected = (initial + i * 17) ^ salt;
      for (unsigned original = 0; original < 2; ++original) {
        objects[i].payload = initial;
        calls = receiver = 0;
        const uint64_t result = original ? original_indexed_tail(&c, i)
                                         : tail_indexed_virtual(&c, i);
        if (result != expected || calls != 1 ||
            receiver != (uintptr_t)&objects[i] ||
            objects[i].payload != initial + 1 + (i & 1))
          return 1;
      }
    }
  }
  return 0;
}
)";
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      const auto Executable = tmpFile("indexed-tail.exe");
      const auto Compiled =
          exec(NEVERD_TEST_CLANG,
               {"-std=c17", "-fno-strict-aliasing",
                "-Wno-deprecated-non-prototype", Optimization, File.string(),
                Original.string(), "-o", Executable.string()});
      ASSERT_TRUE(Compiled.ok()) << Compiled.err << "\n" << Source;
      const auto Run = exec(Executable, {});
      EXPECT_TRUE(Run.ok()) << Run.err;
    }
  }
#endif
}

TEST_F(IndirectTailFrame, FirstVirtualSlotKeepsTheReceiverAcrossABranch) {
#if !defined(__aarch64__)
  GTEST_SKIP() << "native AArch64 execution is unavailable";
#else
  const auto Object = compileTailFrameFixture();
  for (bool Indexed : {false, true}) {
    SCOPED_TRACE(Indexed);
    const auto Original = tmpFile("first-slot-original.S");
    std::ofstream(Original)
        << R"(
#ifdef __APPLE__
#define original_first_slot _original_first_slot
#endif
.text
.p2align 2
.globl original_first_slot
original_first_slot:
)"
        << (Indexed ? "  ldr x9,[x0,#8]\n  ldr x0,[x9,x1,lsl #3]\n"
                    : "  ldr x0,[x0,#8]\n")
        << R"(
  cbz x0,1f
  ldr x10,[x0]
  ldr x16,[x10]
  br x16
1:
  ret
)";
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(Route);
      const auto File = tmpFile("first-slot-" + std::to_string(Route) + ".c");
      std::vector<std::string> Args = {
          "decompile",
          Object.string(),
          "--func",
          Indexed ? "tail_indexed_virtual_zero" : "tail_direct_virtual_zero",
          "-o",
          File.string()};
      if (Route)
        Args.push_back("--llvm");
      if (Route == 2)
        Args.push_back("--no-opt");
      const auto Recovered = exec(ndBin(), Args);
      ASSERT_TRUE(Recovered.ok()) << Recovered.err;
      std::ofstream(File, std::ios::app)
          << "\n#define DIRECT_RECEIVER " << !Indexed << '\n'
          << (Indexed ? "#define recovered_first_slot(c,i) "
                        "tail_indexed_virtual_zero(c,i)\n"
                      : "#define recovered_first_slot(c,i) "
                        "tail_direct_virtual_zero(c)\n");
      std::ofstream(File, std::ios::app) << R"(
struct slot_object { uintptr_t *vtable; uint64_t payload; };
struct slot_container { uint64_t unused; void *entries; };
extern uint64_t original_first_slot(void *, uint64_t);
static uintptr_t receiver;
static unsigned calls;
static uint64_t callback(uintptr_t p) {
  receiver = p;
  ++calls;
  return UINT64_C(0xA12345678B987654);
}
int main(void) {
  uintptr_t table[] = {(uintptr_t)&callback};
  struct slot_object objects[16];
  struct slot_object *entries[16];
  for (unsigned i = 0; i < 16; ++i) {
    objects[i].vtable = table;
    entries[i] = (i % 3) ? &objects[i] : 0;
  }
  struct slot_container c = {0, entries};
  for (unsigned i = 0; i < 16; ++i)
    for (unsigned original = 0; original < 2; ++original) {
      receiver = calls = 0;
      c.entries = DIRECT_RECEIVER ? (void *)entries[i] : (void *)entries;
      const uint64_t value = original ? original_first_slot(&c, i)
                                     : recovered_first_slot(&c, i);
      if (value != (entries[i] ? UINT64_C(0xA12345678B987654) : 0) ||
          receiver != (uintptr_t)entries[i] || calls != (entries[i] != 0))
        return 1;
    }
  return 0;
}
)";
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Executable = tmpFile("first-slot.exe");
        const auto Compiled =
            exec(NEVERD_TEST_CLANG,
                 {"-std=c17", "-fno-strict-aliasing",
                  "-Wno-deprecated-non-prototype", Optimization, File.string(),
                  Original.string(), "-o", Executable.string()});
        ASSERT_TRUE(Compiled.ok()) << Compiled.err;
        const auto Run = exec(Executable, {});
        EXPECT_TRUE(Run.ok()) << Run.err;
      }
    }
  }
#endif
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
    EXPECT_NE(Source.find("indirect targets not proved at 0x"),
              std::string::npos)
        << Source;
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
    // The native raw-call wrapper may enclose the pointer cast. Check the
    // loaded slot independently of that call wrapper's parentheses.
    EXPECT_NE(Source.find("(*(void **)((uintptr_t)(*(void **)(arg0)) + " +
                          Slot + "))"),
              std::string::npos)
        << Source;
    if (Route)
      continue;
    // The early return hands back the object the caller passed.
    EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
    const auto Compiled =
        checkHighCClangSyntax(File, {"-target", "aarch64-linux-gnu",
                                     "-ffreestanding", "-nostdlibinc"});
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
    EXPECT_TRUE(restoredAArch64IndirectTailFrames(Function, BinaryFormat::ELF,
                                                  {}, Budget)
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
  // The register an MRS names, `cntvct_el0`.
  EXPECT_TRUE(lowIntrinsicWritesOnlyItsOutput(
      make(Intrinsic::Mrs, Register, {NdVar::cst(0xDF02, 2)})));
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
