//===- ELFObjectBranchRelocationTests.cpp - Relocated code in objects -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "NeverDLiftFixture.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/support/BinaryLoading.h"
#include "neverd/support/BranchEncoding.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Error.h"

#include <cstring>
#include <fstream>
#include <optional>
#include <string>

namespace {

using namespace neverd;

class ELFObjectBranchRelocationTest : public NeverDLiftTest {
protected:
  llvm::Expected<BinaryImage> loadAssembly(const std::string &Source,
                                           const char *Triple) {
    const auto Assembly = tmpFile("relocated.s");
    std::ofstream(Assembly) << Source;
    const auto Object = tmpFile("relocated.o");
    const auto Compiled =
        exec(NEVERD_TEST_CLANG, {"-target", Triple, "-c", Assembly.string(),
                                 "-o", Object.string()});
    if (!Compiled.ok())
      return llvm::make_error<llvm::StringError>(
          Compiled.err, llvm::inconvertibleErrorCode());
    return loadBinary(Object);
  }

  static uint32_t wordAt(const BinaryImage &Image, va_t Address) {
    uint32_t Word = 0;
    if (const uint8_t *Bytes = Image.readVA(Address, sizeof(Word)))
      std::memcpy(&Word, Bytes, sizeof(Word));
    return Word;
  }

  static LowFunc build(const BinaryImage &Image, const Symbol &Function) {
    Decoder Decode;
    EXPECT_TRUE(Decode.init(Image.Arch, Image.Mode));
    CFGBuilder Builder;
    return Builder.build(Image, Decode, Function.Addr, Function.Name);
  }
};

// A conditional branch to a global label is left to the linker: the
// assembler writes a zero displacement and a relocation.  Unapplied, each
// branch below targets itself.
TEST_F(ELFObjectBranchRelocationTest, AArch64ConditionalBranchesAreApplied) {
  auto ImageOrErr = loadAssembly(R"(
        .text
        .p2align 2
        .globl  relbr
        .type   relbr, %function
relbr:
        tbz     w0, #4, relbr_tbz
        add     w0, w0, #1
        .globl  relbr_tbz
relbr_tbz:
        cbz     w0, relbr_cbz
        add     w0, w0, #2
        .globl  relbr_cbz
relbr_cbz:
        cmp     w0, #100
        b.hi    relbr_bcc
        add     w0, w0, #4
        .globl  relbr_bcc
relbr_bcc:
        add     w0, w0, #8
        ret
        .size   relbr, .-relbr
)",
                                 "aarch64-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Image = *ImageOrErr;
  const Symbol *Function = Image.findSymbol("relbr");
  const Symbol *Tbz = Image.findSymbol("relbr_tbz");
  const Symbol *Cbz = Image.findSymbol("relbr_cbz");
  const Symbol *Bcc = Image.findSymbol("relbr_bcc");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Tbz, nullptr);
  ASSERT_NE(Cbz, nullptr);
  ASSERT_NE(Bcc, nullptr);

  auto Reach = [&](va_t Branch) {
    return branch::a64ConditionalBranchDisplacement(wordAt(Image, Branch));
  };
  EXPECT_EQ(Reach(Function->Addr),
            std::optional<int64_t>(Tbz->Addr - Function->Addr));
  EXPECT_EQ(Reach(Tbz->Addr), std::optional<int64_t>(Cbz->Addr - Tbz->Addr));
  EXPECT_EQ(Reach(Cbz->Addr + 4),
            std::optional<int64_t>(Bcc->Addr - (Cbz->Addr + 4)));
  EXPECT_TRUE(Image.UnappliedCodeRelocations.empty());
  EXPECT_TRUE(build(Image, *Function).UnsupportedInstructionAddresses.empty());
}

// A test branch reaches 32 KiB.  One the linker would refuse keeps its
// placeholder, and the instruction is reported instead of lifted as a branch
// to itself.
TEST_F(ELFObjectBranchRelocationTest, AnUnreachableBranchIsUnsupported) {
  auto ImageOrErr = loadAssembly(R"(
        .text
        .p2align 2
        .globl  far_branch
        .type   far_branch, %function
far_branch:
        tbz     w0, #1, far_label
        ret
        .size   far_branch, .-far_branch
        .space  0x10000
        .globl  far_label
        .type   far_label, %function
far_label:
        ret
        .size   far_label, .-far_label
)",
                                 "aarch64-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Image = *ImageOrErr;
  const Symbol *Function = Image.findSymbol("far_branch");
  ASSERT_NE(Function, nullptr);
  ASSERT_EQ(Image.UnappliedCodeRelocations.size(), 1u);
  EXPECT_EQ(Image.UnappliedCodeRelocations.begin()->first, Function->Addr);
  EXPECT_EQ(Image.UnappliedCodeRelocations.begin()->second,
            uint32_t{llvm::ELF::R_AARCH64_TSTBR14});
  EXPECT_TRUE(llvm::is_contained(
      build(Image, *Function).UnsupportedInstructionAddresses, Function->Addr));
}

// A relocation type the loader does not model leaves its field as the
// assembler wrote it: here the thread-pointer offset of every thread-local
// variable reads as zero.  The instruction is reported, not lifted.
TEST_F(ELFObjectBranchRelocationTest, AnUnmodelledRelocationIsUnsupported) {
  auto ImageOrErr = loadAssembly(R"(
        .text
        .globl  tls_read
        .type   tls_read, @function
tls_read:
        movq    %fs:0, %rax
        .globl  tls_read_field
tls_read_field:
        movl    tls_second@tpoff(%rax), %eax
        ret
        .size   tls_read, .-tls_read
        .globl  plain_read
        .type   plain_read, @function
plain_read:
        movl    plain_value(%rip), %eax
        ret
        .size   plain_read, .-plain_read
        .section .tbss,"awT",@nobits
        .p2align 2
tls_first:
        .long   0
tls_second:
        .long   0
        .data
        .p2align 2
plain_value:
        .long   7
)",
                                 "x86_64-linux-gnu");
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Image = *ImageOrErr;
  const Symbol *Function = Image.findSymbol("tls_read");
  const Symbol *Field = Image.findSymbol("tls_read_field");
  const Symbol *Plain = Image.findSymbol("plain_read");
  ASSERT_NE(Function, nullptr);
  ASSERT_NE(Field, nullptr);
  ASSERT_NE(Plain, nullptr);
  ASSERT_EQ(Image.UnappliedCodeRelocations.size(), 1u);
  EXPECT_EQ(Image.UnappliedCodeRelocations.begin()->second,
            uint32_t{llvm::ELF::R_X86_64_TPOFF32});
  EXPECT_TRUE(Image.hasUnappliedCodeRelocation(Field->Addr, 6));
  EXPECT_FALSE(Image.hasUnappliedCodeRelocation(Function->Addr, 9));
  EXPECT_TRUE(llvm::is_contained(
      build(Image, *Function).UnsupportedInstructionAddresses, Field->Addr));
  EXPECT_TRUE(build(Image, *Plain).UnsupportedInstructionAddresses.empty());
}

// The ARMv4 `bx` marker asks for nothing unless the linker is told to
// rewrite those returns.
TEST_F(ELFObjectBranchRelocationTest, AMarkerRelocationChangesNothing) {
  auto ImageOrErr = loadAssembly(R"(
        .text
        .arm
        .globl  v4_return
        .type   v4_return, %function
v4_return:
        add     r0, r0, #1
        .reloc  ., R_ARM_V4BX
        bx      lr
        .size   v4_return, .-v4_return
)",
                                 "armv4t-linux-gnueabi");
  ASSERT_TRUE(static_cast<bool>(ImageOrErr))
      << llvm::toString(ImageOrErr.takeError());
  const BinaryImage &Image = *ImageOrErr;
  const Symbol *Function = Image.findSymbol("v4_return");
  ASSERT_NE(Function, nullptr);
  EXPECT_TRUE(llvm::any_of(Image.Relocations, [](const RelocationEntry &R) {
    return R.Type == llvm::ELF::R_ARM_V4BX;
  }));
  EXPECT_TRUE(Image.UnappliedCodeRelocations.empty());
  EXPECT_TRUE(build(Image, *Function).UnsupportedInstructionAddresses.empty());
}

} // namespace
