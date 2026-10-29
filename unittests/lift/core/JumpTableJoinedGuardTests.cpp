//===- JumpTableJoinedGuardTests.cpp - Joined range guards ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks entry and backedge guards through native decoding and both C routes.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/support/BinaryLoading.h"

#include <set>

namespace {

class JumpTableJoinedGuard : public NeverDLiftTest {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  fs::path fixture(const std::string &Format, unsigned Variant) {
    const bool COFF = Format == "coff", MachO = Format == "macho";
    const fs::path Source = tmpFile(Format + std::to_string(Variant) + ".s");
    const fs::path Object = tmpFile(Format + std::to_string(Variant) + ".o");
    std::ofstream Out(Source);
    Out << (MachO ? ".section __TEXT,__text,regular,pure_instructions\n"
                  : ".text\n")
        << ".globl jt_joined_guard\n";
    if (COFF)
      Out << ".def jt_joined_guard; .scl 2; .type 32; .endef\n";
    else if (!MachO)
      Out << ".type jt_joined_guard,@function\n";
    Out << "jt_joined_guard:\n  lea .Ltable(%rip),%r8\n  mov "
        << (COFF ? (Variant == 5 ? "%rcx" : "%ecx")
                 : (Variant == 5 ? "%rdi" : "%edi"))
        << (Variant == 5 ? ",%r10\n" : ",%r10d\n");
    auto Guard = [&] {
      Out << (Variant == 7 ? "  cmp $3,%r10d\n  jae .Lexit\n"
                           : "  cmp $2,%r10d\n  ja .Lexit\n");
    };
    if (Variant != 1)
      Guard();
    Out << ".Ldispatch:\n";
    if (Variant == 1)
      Guard();
    if (Variant == 3)
      Out << "  mov $255,%r10b\n";
    Out << "  movslq (%r8,%r10,4),%rax\n"
           "  add %r8,%rax\n  jmp *%rax\n.Lzero:\n  mov "
        << (COFF ? (Variant == 5 ? "%rdx" : "%edx")
                 : (Variant == 5 ? "%rsi" : "%esi"))
        << (Variant == 5 ? ",%r10\n" : ",%r10d\n");
    if (Variant == 1 || Variant == 2)
      Out << "  jmp .Ldispatch\n";
    else {
      Out << "  cmp $"
          << (Variant == 6   ? 5
              : Variant == 7 ? 3
                             : 2)
          << ",%r10d\n";
      // INC preserves CF but changes ZF. Those flags no longer describe one
      // index comparison; the equality arm can admit an out-of-range value.
      if (Variant == 4)
        Out << "  inc %r9d\n";
      Out << (Variant == 7 ? "  jb .Ldispatch\n" : "  jbe .Ldispatch\n");
    }
    Out << ".Lexit:\n  mov $99,%eax\n  ret\n"
           ".Lone:\n  mov $1,%eax\n  ret\n"
           ".Ltwo:\n  mov $2,%eax\n  ret\n";
    if (!COFF && !MachO)
      Out << ".size jt_joined_guard,.-jt_joined_guard\n";
    Out << (MachO  ? ".section __TEXT,__const\n"
            : COFF ? ".section .rdata,\"dr\"\n"
                   : ".section .rodata\n")
        << ".p2align 2\n.Ltable:\n"
           "  .long .Lzero-.Ltable\n  .long .Lone-.Ltable\n"
           "  .long .Ltwo-.Ltable\n  .long .Lexit-.Ltable\n";
    if (!COFF && !MachO)
      Out << ".section .note.GNU-stack,\"\",@progbits\n";
    Out.close();
    const std::string Triple = COFF    ? "x86_64-pc-windows-msvc"
                               : MachO ? "x86_64-apple-darwin"
                                       : "x86_64-unknown-linux-gnu";
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", Triple, "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static neverd::LowFunc build(const neverd::BinaryImage &Image,
                               neverd::CFGBuilder &Builder,
                               const char *Name = "jt_joined_guard") {
    const auto *Function = Image.findSymbol(Name);
    EXPECT_NE(Function, nullptr);
    if (!Function)
      return {};
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    return Builder.build(Image, Decoder, Function->Addr, Function->Name);
  }

  fs::path branchFixture(const std::string &Format, unsigned Variant,
                         bool I386 = false, bool Spilled = false) {
    const bool COFF = Format == "coff", MachO = Format == "macho";
    const std::string Stem = Format + std::to_string(Variant) +
                             (I386 ? "-i386" : "-x64") +
                             (Spilled ? "-frame" : "");
    const fs::path Source = tmpFile("branch-" + Stem + ".s");
    const fs::path Object = tmpFile("branch-" + Stem + ".o");
    std::ofstream Out(Source);
    Out << (MachO ? ".section __TEXT,__text,regular,pure_instructions\n"
                  : ".text\n")
        << ".globl jt_branch_join\n";
    if (COFF)
      Out << ".def jt_branch_join; .scl 2; .type 32; .endef\n";
    else if (!MachO)
      Out << ".type jt_branch_join,@function\n";
    Out << "jt_branch_join:\n";
    const char *IndexReg = I386 ? "%eax" : "%r10d";
    const char *Index = Spilled ? (I386 ? "-16(%ebp)" : "-16(%rbp)") : IndexReg;
    const char *Side = I386 ? "%edx" : "%r11d";
    if (Spilled)
      Out << (I386 ? "  push %ebp\n  mov %esp,%ebp\n  sub $32,%esp\n"
                   : "  push %rbp\n  mov %rsp,%rbp\n  sub $32,%rsp\n");
    if (I386)
      Out << (Spilled ? "  mov 8(%ebp),%eax\n  mov 12(%ebp),%edx\n"
                      : "  mov 4(%esp),%eax\n  mov 8(%esp),%edx\n");
    else
      Out << "  lea .Ltable(%rip),%r8\n  mov "
          << (COFF ? (Variant == 8 ? "%rcx" : "%ecx")
                   : (Variant == 8 ? "%rdi" : "%edi"))
          << (Variant == 8 ? ",%r10\n  mov " : ",%r10d\n  mov ")
          << (COFF ? "%edx" : "%esi") << ",%r11d\n";
    if (Spilled)
      Out << "  mov " << IndexReg << ',' << Index << '\n';
    if (Variant == 1)
      Out << "  test " << Side << ',' << Side << "\n  je .Ldispatch\n";
    if (Variant != 7) {
      Out << "  cmpl $" << (Variant == 2 ? 3 : 1) << ',' << Index
          << "\n  jne .Lbound\n  test " << Side << ',' << Side
          << "\n  je .Ldispatch\n  cmp $2," << Side << "\n  je .Ldispatch\n";
      if (Variant == 9)
        Out << "  movabs $0x100000000,%r10\n";
      else if (Spilled)
        Out << "  movl $0," << Index << '\n';
      else
        Out << "  xor " << Index << ',' << Index << '\n';
    }
    Out << ".Lbound:\n";
    if (Variant != 6)
      Out << "  cmpl $2," << Index << '\n'
          << (Variant == 3 ? "  jg .Ldefault\n" : "  ja .Ldefault\n");
    if (Variant == 4)
      Out << "  movb $255,"
          << (Spilled ? Index
              : I386  ? "%al"
                      : "%r10b")
          << '\n';
    if (Variant == 5)
      Out << "  incl " << Index << '\n';
    Out << ".Ldispatch:\n";
    if (Spilled) {
      // This unrelated push must not erase the guarded slot's identity.
      Out << "  push $0\n";
      if (Variant == 10)
        Out << "  mov " << Side << ',' << Index << '\n';
      if (Variant == 11)
        Out << (I386 ? "  mov %dl,-15(%ebp)\n" : "  mov %r11b,-15(%rbp)\n");
      if (Variant == 12)
        Out << "  call guard_memory_helper\n";
      if (Variant == 13)
        Out << (I386 ? "  movl $7,(%ecx)\n" : "  movl $7,(%r9)\n");
      Out << "  mov " << Index << ',' << IndexReg << '\n';
    }
    if (I386)
      Out << "  jmp *.Ltable(,%eax,4)\n";
    else
      Out << "  movslq (%r8,%r10,4),%rax\n  add %r8,%rax\n  jmp *%rax\n";
    for (unsigned I = 0; I < 3; ++I)
      Out << ".Lcase" << I << ":\n  mov $" << I + 10 << ",%eax\n"
          << (Spilled ? "  leave\n" : "") << "  ret\n";
    Out << ".Ldefault:\n  mov $99,%eax\n"
        << (Spilled ? "  leave\n" : "") << "  ret\n";
    if (!COFF && !MachO)
      Out << ".size jt_branch_join,.-jt_branch_join\n";
    Out << (MachO  ? ".section __TEXT,__const\n"
            : COFF ? ".section .rdata,\"dr\"\n"
                   : ".section .rodata\n")
        << ".p2align 2\n.Ltable:\n";
    for (unsigned I = 0; I < 3; ++I)
      Out << "  .long .Lcase" << I << (I386 ? "\n" : "-.Ltable\n");
    if (!COFF && !MachO)
      Out << ".section .note.GNU-stack,\"\",@progbits\n";
    Out.close();
    const std::string Triple = (I386 ? "i686" : "x86_64") +
                               std::string(COFF    ? "-pc-windows-msvc"
                                           : MachO ? "-apple-darwin"
                                                   : "-unknown-linux-gnu");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", Triple, "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  static std::set<neverd::va_t>
  entryInventory(const neverd::BinaryImage &Image) {
    neverd::Decoder Decoder;
    EXPECT_TRUE(Decoder.init(Image.Arch, Image.Mode));
    const auto Functions = neverd::FuncDetector().detect(Image, Decoder);
    std::set<neverd::va_t> Entries;
    for (const auto &[Address, Name] : Functions)
      Entries.insert(Address);
    return Entries;
  }

  fs::path filteredPrefixFixture(const std::string &Format, unsigned Variant,
                                 bool I386) {
    const bool COFF = Format == "coff", MachO = Format == "macho";
    const std::string Stem =
        Format + std::to_string(Variant) + (I386 ? "-i386" : "-x64");
    const auto Source = tmpFile("prefix-" + Stem + ".s");
    const auto Object = tmpFile("prefix-" + Stem + ".o");
    std::ofstream Out(Source);
    Out << (MachO ? ".section __TEXT,__text,regular,pure_instructions\n"
                  : ".text\n")
        << ".globl jt_guard_prefix\n";
    if (COFF)
      Out << ".def jt_guard_prefix; .scl 2; .type 32; .endef\n";
    else if (!MachO)
      Out << ".type jt_guard_prefix,@function\n";
    Out << "jt_guard_prefix:\n";
    if (I386)
      Out << "  mov 4(%esp),%edx\n";
    const char *Address = I386 ? "(%edx)" : COFF ? "(%rcx)" : "(%rdi)";
    auto RangeGuard = [&] {
      Out << "  cmp $" << (Variant == 3 ? 11 : 10) << ','
          << (Variant == 4 ? "%ecx" : "%eax") << '\n';
      if (Variant == 9)
        Out << "  inc %esi\n";
      Out << (Variant == 5   ? "  jg .Ldefault\n"
              : Variant == 3 ? "  jae .Ldefault\n"
                             : "  ja .Ldefault\n");
    };
    if (Variant == 2) {
      Out << "  mov " << Address << ",%eax\n";
      RangeGuard();
    }
    const bool Exclusion = Variant == 1 || Variant == 2 || Variant == 5;
    Out << "  cmpl $"
        << (Exclusion       ? 3
            : Variant == 10 ? 13
                            : 6)
        << ',' << Address
        << (Exclusion ? "\n  je .Ldefault\n" : "\n  jne .Ldefault\n");
    Out << "  mov " << Address << ",%eax\n";
    if (Variant == 8)
      Out << "  test %esi,%esi\n  jne .Ldispatch\n";
    if (Variant != 2 && Variant != 6)
      RangeGuard();
    if (Variant == 7)
      Out << "  mov %esi,%eax\n";
    Out << ".Ldispatch:\n";
    // Relative COFF tables avoid promoting bare absolute case pointers to
    // independent unsized entries. ELF and Mach-O exercise absolute tables.
    if (COFF && I386)
      Out << "  lea .Ltable,%ecx\n  mov (%ecx,%eax,4),%eax\n"
             "  add %ecx,%eax\n  jmp *%eax\n";
    else if (COFF)
      Out << "  lea .Ltable(%rip),%r8\n  movslq (%r8,%rax,4),%rax\n"
             "  add %r8,%rax\n  jmp *%rax\n";
    else if (I386)
      Out << "  jmp *.Ltable(,%eax,4)\n";
    else
      Out << "  lea .Ltable(%rip),%r8\n  jmp *(%r8,%rax,8)\n";
    for (unsigned I = 0; I < 11; ++I) {
      Out << ".Lcase" << I << ":\n";
      if (I == 6)
        Out << "  call prefix_helper\n";
      Out << "  mov $" << I + 10 << ",%eax\n  ret\n";
    }
    Out << ".Ldefault:\n  mov $99,%eax\n  ret\n";
    if (!COFF && !MachO)
      Out << ".size jt_guard_prefix,.-jt_guard_prefix\n";
    Out << (MachO  ? ".section __TEXT,__const\n"
            : COFF ? ".section .rdata,\"dr\"\n"
                   : ".section .rodata\n")
        << ".p2align 3\n.Ltable:\n";
    for (unsigned I = 0; I < 11; ++I)
      Out << (COFF || I386 ? "  .long " : "  .quad ") << ".Lcase" << I
          << (COFF ? "-.Ltable\n" : "\n");
    Out << (MachO ? ".section __TEXT,__text,regular,pure_instructions\n"
                  : ".text\n")
        << ".globl prefix_helper\n";
    if (COFF)
      Out << ".def prefix_helper; .scl 2; .type 32; .endef\n";
    else if (!MachO)
      Out << ".type prefix_helper,@function\n";
    Out << "prefix_helper:\n  ret\n";
    if (!COFF && !MachO)
      Out << ".size prefix_helper,.-prefix_helper\n"
             ".section .note.GNU-stack,\"\",@progbits\n";
    Out.close();
    const std::string Triple = (I386 ? "i686" : "x86_64") +
                               std::string(COFF    ? "-pc-windows-msvc"
                                           : MachO ? "-apple-darwin"
                                                   : "-unknown-linux-gnu");
    const auto Built =
        exec(NEVERD_TEST_CLANG,
             {"-target", Triple, "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }
};

TEST_F(JumpTableJoinedGuard, AStricterFilterKeepsAnIndependentDenseGuard) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (bool I386 : {false, true}) {
      if (I386 && Format == "macho")
        continue;
      for (unsigned Variant : {0u, 1u, 2u, 3u}) {
        SCOPED_TRACE(Format + ':' + std::to_string(I386) + ':' +
                     std::to_string(Variant));
        auto Image =
            neverd::loadBinary(filteredPrefixFixture(Format, Variant, I386));
        ASSERT_TRUE(static_cast<bool>(Image))
            << llvm::toString(Image.takeError());
        const auto Entries = entryInventory(*Image);
        neverd::CFGBuilder Builder;
        Builder.setKnownFuncEntries(&Entries);
        const auto Low = build(*Image, Builder, "jt_guard_prefix");
        EXPECT_EQ(Low.JumpTables.size(), 1u);
        if (Low.JumpTables.empty())
          continue;
        EXPECT_EQ(Low.JumpTables.front().Targets.size(), 11u);
        EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                  (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10}));
        EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
        neverd::CFGBuilder Exhausted;
        Exhausted.setKnownFuncEntries(&Entries);
        Exhausted.setMaskFixedPointEvidenceBudgetForTesting(1);
        const auto Short = build(*Image, Exhausted, "jt_guard_prefix");
        EXPECT_TRUE(Short.JumpTables.empty());
        EXPECT_TRUE(Short.EverPublishedJumpTableBranchAddresses.empty());
      }
    }
}

TEST_F(JumpTableJoinedGuard, FilteredGuardStillNeedsItsOwnCompleteProof) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (unsigned Variant : {4u, 5u, 6u, 7u, 8u, 9u, 10u}) {
      SCOPED_TRACE(Format + ':' + std::to_string(Variant));
      auto Image =
          neverd::loadBinary(filteredPrefixFixture(Format, Variant, false));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      const auto Entries = entryInventory(*Image);
      neverd::CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      const auto Low = build(*Image, Builder, "jt_guard_prefix");
      EXPECT_TRUE(Low.JumpTables.empty());
    }
}

TEST_F(JumpTableJoinedGuard,
       EqualityAndRangePathsJoinWithoutOneDominatingGuard) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (bool I386 : {false, true}) {
      if (I386 && Format == "macho")
        continue;
      SCOPED_TRACE(Format + (I386 ? ":i386" : ":x64"));
      auto Image = neverd::loadBinary(branchFixture(Format, 0, I386));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder, "jt_branch_join");
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().Targets.size(), 3u);
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{0, 1, 2}));
      EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
    }
}

TEST_F(JumpTableJoinedGuard,
       JoinedPathsCannotBorrowLiteralsOrPhysicalCapacity) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (unsigned Variant : {1u, 2u, 3u, 4u, 5u, 6u, 8u, 9u}) {
      SCOPED_TRACE(Format + ':' + std::to_string(Variant));
      auto Image = neverd::loadBinary(branchFixture(Format, Variant));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder, "jt_branch_join");
      EXPECT_TRUE(Low.JumpTables.empty());
    }
}

TEST_F(JumpTableJoinedGuard, StoredSelectorsJoinEqualityAndRange) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (bool I386 : {false, true}) {
      if (I386 && Format == "macho")
        continue;
      SCOPED_TRACE(Format + (I386 ? ":i386" : ":x64"));
      auto Image = neverd::loadBinary(branchFixture(Format, 0, I386, true));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder, "jt_branch_join");
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{0, 1, 2}));
      EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
    }
}

TEST_F(JumpTableJoinedGuard, StoredSelectorsRejectStaleOrUnboundedPaths) {
  for (const std::string Format : {"elf", "coff", "macho"})
    for (bool I386 : {false, true})
      for (unsigned Variant : {1u, 2u, 3u, 4u, 5u, 6u, 10u, 11u, 12u, 13u}) {
        if (I386 && Format == "macho")
          continue;
        SCOPED_TRACE(Format + (I386 ? ":i386:" : ":x64:") +
                     std::to_string(Variant));
        auto Image =
            neverd::loadBinary(branchFixture(Format, Variant, I386, true));
        ASSERT_TRUE(static_cast<bool>(Image))
            << llvm::toString(Image.takeError());
        neverd::CFGBuilder Builder;
        const auto Low = build(*Image, Builder, "jt_branch_join");
        EXPECT_TRUE(Low.JumpTables.empty());
      }
}

TEST_F(JumpTableJoinedGuard, UnrelatedRelocationsLeaveBoundaryProofBudget) {
  auto Image = neverd::loadBinary(branchFixture("coff", 7, true));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  neverd::CFGBuilder BaselineBuilder;
  const auto Baseline = build(*Image, BaselineBuilder, "jt_branch_join");
  ASSERT_EQ(Baseline.JumpTables.size(), 1u);

  // These real immediate fields belong to unrelated code and name data far
  // from the table. None can bound this candidate's relocation run.
  neverd::Segment Text;
  Text.VA = 0x10000000;
  Text.Flags =
      neverd::SegmentFlags::Readable | neverd::SegmentFlags::Executable;
  constexpr neverd::va_t DataVA = 0x20000000;
  for (unsigned I = 0; I < 4096; ++I) {
    const auto Field = Text.VA + Text.Data.size() + 1;
    Text.Data.insert(Text.Data.end(), {0xb8, 0, 0, 0, 0x20});
    Image->DataAddressRelocOperands[Field] = {DataVA, DataVA, 4, DataVA};
  }
  Text.Data.push_back(0xc3);
  Text.Size = Text.FileSz = Text.Data.size();
  Image->Segments.push_back(std::move(Text));
  neverd::Segment Data;
  Data.VA = DataVA;
  Data.Flags = neverd::SegmentFlags::Readable;
  Data.Size = Data.FileSz = 4;
  Data.Data.resize(Data.Size);
  Image->Segments.push_back(std::move(Data));
  for (unsigned I = 0; I < 8192; ++I) {
    neverd::Import Import;
    Import.Module = "unrelated";
    Import.Name = "external_" + std::to_string(I);
    Import.IATAddr = 0x30000000 + I * 4;
    Image->Imports.push_back(std::move(Import));
  }
  neverd::CFGBuilder Builder;
  const auto Low = build(*Image, Builder, "jt_branch_join");
  ASSERT_EQ(Low.JumpTables.size(), 1u);
  EXPECT_EQ(Low.JumpTables.front().Targets,
            Baseline.JumpTables.front().Targets);
  EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
}

TEST_F(JumpTableJoinedGuard, SeparateGuardsRecoverExactlyTheDenseCases) {
  for (const std::string Format : {"elf", "coff", "macho"}) {
    for (unsigned Variant : {0u, 1u, 7u}) {
      SCOPED_TRACE(Format + ":" + std::to_string(Variant));
      auto Image = neverd::loadBinary(fixture(Format, Variant));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder);
      ASSERT_EQ(Low.JumpTables.size(), 1u);
      EXPECT_EQ(Low.JumpTables.front().SlotIndices,
                (std::vector<uint32_t>{0, 1, 2}));
      EXPECT_EQ(Low.JumpTables.front().Targets.size(), 3u);
      EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
    }
  }
}

TEST_F(JumpTableJoinedGuard, UncheckedAndChangedInputsRemainUnresolved) {
  for (const std::string Format : {"elf", "coff", "macho"}) {
    for (unsigned Variant : {2u, 3u, 4u, 5u, 6u}) {
      SCOPED_TRACE(Format + ":" + std::to_string(Variant));
      auto Image = neverd::loadBinary(fixture(Format, Variant));
      ASSERT_TRUE(static_cast<bool>(Image))
          << llvm::toString(Image.takeError());
      neverd::CFGBuilder Builder;
      const auto Low = build(*Image, Builder);
      EXPECT_TRUE(Low.JumpTables.empty());
    }
  }
}

TEST_F(JumpTableJoinedGuard, ExhaustedEvidenceCannotKeepProvisionalTargets) {
  for (bool BranchJoin : {false, true}) {
    SCOPED_TRACE(BranchJoin);
    auto Image = neverd::loadBinary(BranchJoin ? branchFixture("elf", 0)
                                               : fixture("elf", 0));
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    for (bool SymbolBudget : {false, true}) {
      SCOPED_TRACE(SymbolBudget);
      neverd::CFGBuilder Builder;
      if (SymbolBudget)
        Builder.setFiniteSetSymbolEvidenceBudgetForTesting(0);
      else
        Builder.setMaskFixedPointEvidenceBudgetForTesting(0);
      const auto Low = build(*Image, Builder,
                             BranchJoin ? "jt_branch_join" : "jt_joined_guard");
      EXPECT_TRUE(Low.JumpTables.empty());
      EXPECT_FALSE(Low.UnsafeIndirectBranchAddresses.empty());
    }
  }
}

TEST_F(JumpTableJoinedGuard, GeneratedSourceExecutesJoinedEqualityAndRange) {
  for (bool Spilled : {false, true}) {
    SCOPED_TRACE(Spilled);
    const fs::path Object = branchFixture("elf", 0, false, Spilled);
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(Route);
      const fs::path Source =
          tmpFile(std::string(Spilled ? "frame-joined-" : "branch-joined-") +
                  std::to_string(Route) + ".c");
      std::vector<std::string> Args = {"decompile", Object.string(),
                                       "--func",    "jt_branch_join",
                                       "-o",        Source.string()};
      if (Route)
        Args.push_back("--llvm");
      if (Route == 2)
        Args.push_back("--no-opt");
      const auto Decompiled = exec(ndBin(), Args);
      ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
      std::ofstream Out(Source, std::ios::app);
      Out << R"(
static int check(uint64_t first, uint64_t side) {
  uint32_t index = (uint32_t)first, control = (uint32_t)side;
  if (index == 1 && control != 0 && control != 2) index = 0;
  uint64_t expected = index <= 2 ? index + 10 : 99;
  return jt_branch_join(first, side) != expected;
}
int main(void) {
  for (uint64_t a = 0; a < 256; ++a)
    for (uint64_t b = 0; b < 8; ++b)
      if (check(a, b)) return 1;
  const uint64_t edges[] = {0, 1, 2, 3, UINT64_C(0x80000000), UINT32_MAX,
                            UINT64_C(0x100000000), UINT64_MAX};
  for (unsigned a = 0; a < sizeof(edges)/sizeof(edges[0]); ++a)
    for (unsigned b = 0; b < sizeof(edges)/sizeof(edges[0]); ++b)
      if (check(edges[a], edges[b])) return 2;
  return 0;
}
)";
      Out.close();
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Exe = tmpFile("branch-joined-run");
        const auto Built = exec(
            NEVERD_TEST_CLANG,
            {"-std=c11", Optimization, "-fsanitize=undefined",
             "-fsanitize-trap=undefined", Source.string(), "-o", Exe.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        const auto Run = exec(Exe.string(), {});
        EXPECT_TRUE(Run.ok()) << Run.err;
      }
    }
  }
}

TEST_F(JumpTableJoinedGuard, GeneratedSourceExecutesEntryAndLoopSelections) {
  for (unsigned Variant : {0u, 7u}) {
    const fs::path Object = fixture("elf", Variant);
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(std::to_string(Variant) + ":" + std::to_string(Route));
      const fs::path Source = tmpFile("joined-" + std::to_string(Variant) +
                                      std::to_string(Route) + ".c");
      std::vector<std::string> Args = {"decompile", Object.string(),
                                       "--func",    "jt_joined_guard",
                                       "-o",        Source.string()};
      if (Route)
        Args.push_back("--llvm");
      if (Route == 2)
        Args.push_back("--no-opt");
      const auto Decompiled = exec(ndBin(), Args);
      ASSERT_TRUE(Decompiled.ok()) << Decompiled.err;
      std::ofstream Out(Source, std::ios::app);
      Out << R"(
static int check(uint64_t first, uint64_t next) {
  uint32_t index = (uint32_t)first;
  if (index == 0) index = (uint32_t)next;
  if (index == 0) return 0; /* The original loop does not terminate. */
  uint64_t expected = index <= 2 ? index : 99;
  return jt_joined_guard(first, next) != expected;
}
int main(void) {
  for (uint64_t a = 0; a < 256; ++a)
    for (uint64_t b = 0; b < 256; ++b)
      if (check(a, b)) return 1;
  const uint64_t edges[] = {0, 1, 2, 3, UINT32_MAX,
                            UINT64_C(0x100000000), UINT64_MAX};
  for (unsigned a = 0; a < sizeof(edges)/sizeof(edges[0]); ++a)
    for (unsigned b = 0; b < sizeof(edges)/sizeof(edges[0]); ++b)
      if (check(edges[a], edges[b])) return 2;
  return 0;
}
)";
      Out.close();
      for (const char *Optimization : {"-O0", "-O2"}) {
        SCOPED_TRACE(Optimization);
        const auto Exe = tmpFile("joined-run");
        const auto Built = exec(
            NEVERD_TEST_CLANG,
            {"-std=c11", Optimization, "-fsanitize=undefined",
             "-fsanitize-trap=undefined", Source.string(), "-o", Exe.string()});
        ASSERT_TRUE(Built.ok()) << Built.err;
        const auto Run = exec(Exe.string(), {});
        EXPECT_TRUE(Run.ok()) << Run.err;
      }
    }
  }
}

} // namespace
