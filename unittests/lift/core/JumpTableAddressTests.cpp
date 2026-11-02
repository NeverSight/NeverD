//===- JumpTableAddressTests.cpp - CFG-derived table addresses -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Assemble native-format switch fixtures whose table bases precede unrelated
/// lexical blocks, and check discovery against independent case-label offsets.
///
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <optional>

namespace {
using namespace neverd;

struct AddressTarget {
  std::string Triple;
  std::string Name;
  bool MachO;
  Arch Architecture;
};

class JumpTableAddress : public NeverDLiftTest,
                         public ::testing::WithParamInterface<AddressTarget> {
protected:
  void SetUp() override {
    NeverDLiftTest::SetUp();
    if (!hasCrossTargetClang())
      GTEST_SKIP() << "cross-target Clang is unavailable";
  }

  std::string assembly(unsigned Blocks, const std::string &Variant) const {
    const auto &T = GetParam();
    const bool X64 = T.Architecture == Arch::X64;
    const bool X86 = T.Architecture == Arch::X86;
    const bool A64 = T.Architecture == Arch::AArch64;
    const bool X = X64 || X86;
    const bool Absolute = Variant == "absolute" || Variant == "composite";
    const std::string Name = T.MachO ? "_probe" : "probe";
    const std::string Ret = X ? "ret" : A64 ? "ret" : "bx lr";
    const std::string Base = X64 ? "%r10" : "%edx";
    const std::string Jump = X ? "jmp " : "b ";
    std::ostringstream S;
    auto SetBase = [&](bool Decoy) {
      const std::string Label = Decoy ? "Lcase0" : "Ltable";
      if (X64)
        S << "leaq " << Label << "(%rip)," << Base << "\n";
      else if (X86)
        S << "leal " << Label << "," << Base << "\n";
      else if (A64)
        S << "adr x10," << Label << "\nadr x11,Lcase0\n";
      else
        S << "adr.w r3," << Label << "\n";
      if (Variant == "composite")
        S << (X64 ? "leaq Louter(%rip),%r9\n" : "adr x12,Louter\n");
    };
    S << ".text\n.p2align 2\n.globl " << Name << "\n";
    if (!T.MachO)
      S << ".type " << Name << ",%function\n";
    if (!X && !A64)
      S << ".syntax unified\n.thumb\n.thumb_func\n";
    S << Name << ":\n";
    if (X)
      S << "testl %esi,%esi\njne Lfiller\n";
    else
      S << (A64 ? "cmp w1,#0\n" : "cmp r1,#0\n") << "bne Lfiller\n";
    SetBase(false);
    S << Jump << "Ldispatch\nLfiller:\n";
    S << (X ? "xorl %eax,%eax\n" : A64 ? "mov w2,#0\n" : "movs r2,#0\n");
    for (unsigned I = 0; I < Blocks; ++I)
      S << (X     ? "addl $1,%eax\n"
            : A64 ? "add w2,w2,#1\n"
                  : "adds r2,r2,#1\n")
        << Jump << "Lfiller" << I << "\nLfiller" << I << ":\n";
    if (Variant == "decoy" || Variant == "divergent" || Variant == "join")
      SetBase(Variant != "join");
    if (Variant == "divergent" || Variant == "join")
      S << Jump << "Ldispatch\n";
    else
      S << (X     ? "movl $99,%eax\n"
            : A64 ? "mov w0,#99\n"
                  : "movs r0,#99\n")
        << Ret << "\n";
    S << "Ldispatch:\n";
    if (Variant == "clobber")
      S << (X64   ? "movb $1,%r10b\n"
            : X86 ? "movb $1,%dl\n"
            : A64 ? "mov w10,#1\n"
                  : "movs r3,#1\n");
    if (Variant == "call")
      S << (X ? "call " : "bl ")
        << (T.MachO ? "_address_helper" : "address_helper") << "\n";
    if (Absolute && X64) {
      S << "movl %edi,%edi\ncmpl $5,%edi\nja Ldefault\n";
      if (Variant == "composite")
        S << "movzbl (%r9,%rdi),%eax\nmovq (%r10,%rax,8),%rax\n";
      else
        S << "movq (%r10,%rdi,8),%rax\n";
      S << "jmp *%rax\n";
    } else if (Absolute && A64) {
      S << "cmp w0,#5\nb.hi Ldefault\n";
      if (Variant == "composite")
        S << "ldrb w9,[x12,w0,uxtw]\nldr x9,[x10,w9,uxtw #3]\n";
      else
        S << "ldr x9,[x10,w0,uxtw #3]\n";
      S << "br x9\n";
    } else if (X)
      S << (X64 ? "movl %edi,%edi\ncmpl $5,%edi\nja Ldefault\n"
                  "movslq (%r10,%rdi,4),%rax\naddq %r10,%rax\njmp *%rax\n"
                : "cmpl $5,%ecx\nja Ldefault\nmovl (%edx,%ecx,4),%eax\n"
                  "addl %edx,%eax\njmp *%eax\n");
    else if (A64)
      S << "cmp w0,#5\nb.hi Ldefault\nldrb w9,[x10,w0,uxtw]\n"
           "add x9,x11,x9,lsl #2\nbr x9\n";
    else
      S << "cmp r0,#5\nbhi Ldefault\ntbb [r3,r0]\nLanchor:\n";
    for (unsigned I = 0; I < 6; ++I)
      S << "Lcase" << I << ":\n"
        << (X     ? "movl $"
            : A64 ? "mov w0,#"
                  : "movs r0,#")
        << 10 + I << (X ? ",%eax\n" : "\n") << Ret << "\n";
    S << "Ldefault:\n"
      << (X     ? "movl $99,%eax\n"
          : A64 ? "mov w0,#99\n"
                : "movs r0,#99\n")
      << Ret << "\n";
    if (!T.MachO)
      S << ".size " << Name << ",.-" << Name << "\n";
    // Local Mach-O labels must not become independent function starts.
    // Keep ADR/Thumb and Mach-O tables in the same section so these offsets
    // are assembled without cross-section subtractor relocations.
    if (X && !T.MachO)
      S << ".section .rodata,\"a\",@progbits\n";
    S << ".p2align 2\nLtable:\n";
    for (unsigned I = 0; I < 6; ++I) {
      if (Absolute)
        S << ".quad Lcase" << I << "\n";
      else
        S << (X ? ".long Lcase" : ".byte (Lcase") << I
          << (X     ? "-Ltable\n"
              : A64 ? "-Lcase0)/4\n"
                    : "-Lanchor)/2\n");
    }
    if (Variant == "composite")
      S << "Louter:\n.byte 0,1,2,3,4,5\n";
    S << (T.MachO ? ".section __DATA,__data\n" : ".section .rodata\n")
      << ".p2align 2\nexpected_cases:\n";
    // Independent case-label offsets, never a code-pointer relocation run.
    for (unsigned I = 0; I < 6; ++I)
      S << ".long Lcase" << I << "-" << Name << "\n";
    S << ".text\n.p2align 2\n.globl "
      << (T.MachO ? "_address_helper" : "address_helper") << "\n";
    if (!T.MachO)
      S << ".type address_helper,%function\n";
    if (!X && !A64)
      S << ".thumb_func\n";
    S << (T.MachO ? "_address_helper" : "address_helper") << ":\n";
    // The negative case must really clobber the base. A closed leaf that
    // only returns can be proved to preserve volatile registers as well.
    if (Variant == "call")
      S << (X64   ? "movb $1,%r10b\n"
            : X86 ? "movb $1,%dl\n"
            : A64 ? "mov w10,#1\n"
                  : "movs r3,#1\n");
    S << Ret << "\n";
    return S.str();
  }

  fs::path object(unsigned Blocks, const std::string &Variant) {
    const auto Source = tmpFile(Variant + std::to_string(Blocks) + ".s");
    std::ofstream(Source) << assembly(Blocks, Variant);
    auto Object = Source;
    Object.replace_extension(".o");
    const auto Built =
        exec(NEVERD_TEST_CLANG, {"-target", GetParam().Triple, "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Built.ok()) << Built.err;
    return Object;
  }

  void check(const fs::path &Input, bool Expected,
             std::optional<size_t> Budget = std::nullopt) {
    auto Image = loadBinary(Input.string());
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    const auto *Function =
        Image->findSymbol(GetParam().MachO ? "_probe" : "probe");
    const auto *Cases = Image->findSymbol("expected_cases");
    ASSERT_NE(Function, nullptr);
    ASSERT_NE(Cases, nullptr);
    std::vector<va_t> Targets;
    for (unsigned I = 0; I < 6; ++I) {
      const auto *P = Image->readVA(Cases->Addr + I * 4, 4);
      ASSERT_NE(P, nullptr);
      Targets.push_back(Function->Addr + uint32_t(P[0]) +
                        (uint32_t(P[1]) << 8) + (uint32_t(P[2]) << 16) +
                        (uint32_t(P[3]) << 24));
    }
    Decoder D;
    ASSERT_TRUE(D.init(Image->Arch, Image->Mode));
    CFGBuilder B;
    if (Budget)
      B.setMaskFixedPointEvidenceBudgetForTesting(*Budget);
    const auto Low = B.build(*Image, D, Function->Addr, Function->Name);
    ASSERT_TRUE(Low.UnsupportedInstructionAddresses.empty());
    if (!Expected) {
      EXPECT_TRUE(Low.JumpTables.empty());
      return;
    }
    ASSERT_EQ(Low.JumpTables.size(), 1u);
    EXPECT_EQ(Low.JumpTables.front().Targets, Targets);
    EXPECT_TRUE(Low.DecodeFailureAddresses.empty());
    EXPECT_TRUE(Low.UnsafeIndirectBranchAddresses.empty());
  }
};

TEST_P(JumpTableAddress, ObjectsIgnoreUnrelatedBlockDistanceAndDecoys) {
  for (unsigned Count : {0u, 32u, 160u})
    for (const std::string Variant : {"valid", "decoy"}) {
      SCOPED_TRACE(Variant + ":" + std::to_string(Count));
      check(object(Count, Variant), true);
    }
  // These are actual CFG predecessors, unlike the unrelated lexical filler.
  // Keep the agreeing-join control within the existing resolver depth limit.
  for (unsigned Count : {0u, 32u}) {
    SCOPED_TRACE("join:" + std::to_string(Count));
    check(object(Count, "join"), true);
  }
  if (GetParam().Architecture == Arch::X64 ||
      GetParam().Architecture == Arch::AArch64) {
    SCOPED_TRACE("absolute");
    check(object(160, "absolute"), true);
  }
  // This unscaled outer-load shape is currently recognized for x64 ELF;
  // AArch64 and Mach-O variants reject it even with adjacent definitions.
  if (GetParam().Architecture == Arch::X64 && !GetParam().MachO) {
    SCOPED_TRACE("composite");
    check(object(160, "composite"), true);
  }
}

TEST_P(JumpTableAddress, LinkedImagesIgnoreUnrelatedBlockDistance) {
  const auto LLD = llvm::sys::findProgramByName("ld.lld");
  if (!LLD)
    GTEST_SKIP() << "ld.lld is unavailable";
  for (unsigned Count : {0u, 32u, 160u}) {
    SCOPED_TRACE(Count);
    const auto Object = object(Count, "decoy");
    auto Binary = Object;
    Binary.replace_extension(".exe");
    std::vector<std::string> Args;
    if (GetParam().MachO)
      Args = {"-flavor",
              "darwin",
              "-arch",
              GetParam().Architecture == Arch::X64 ? "x86_64" : "arm64",
              "-platform_version",
              "macos",
              "13.0",
              "13.0",
              "-e",
              "_probe"};
    else
      Args = {"-e", "probe"};
    Args.insert(Args.end(), {"-o", Binary.string(), Object.string()});
    const auto Linked = exec(*LLD, Args);
    ASSERT_TRUE(Linked.ok()) << Linked.err;
    check(Binary, true);
  }
}

TEST_P(JumpTableAddress, ConflictsClobbersCallsAndExhaustionGrantNoTable) {
  for (const std::string Variant : {"divergent", "clobber", "call"}) {
    SCOPED_TRACE(Variant);
    check(object(32, Variant), false);
  }
  const auto Object = object(160, "valid");
  for (size_t Budget : {size_t{0}, size_t{1}, size_t{2048}})
    check(Object, false, Budget);
  check(Object, true);
}

INSTANTIATE_TEST_SUITE_P(
    Native, JumpTableAddress,
    ::testing::Values(
        AddressTarget{"x86_64-linux-gnu", "X64ELF", false, Arch::X64},
        AddressTarget{"aarch64-linux-gnu", "AArch64ELF", false, Arch::AArch64},
        AddressTarget{"i386-linux-gnu", "X86ELF", false, Arch::X86},
        AddressTarget{"thumbv7-linux-gnueabi", "ThumbELF", false, Arch::ARM},
        AddressTarget{"x86_64-apple-macos13", "X64MachO", true, Arch::X64},
        AddressTarget{"arm64-apple-macos13", "AArch64MachO", true,
                      Arch::AArch64}),
    [](const auto &Info) { return Info.param.Name; });
} // namespace
