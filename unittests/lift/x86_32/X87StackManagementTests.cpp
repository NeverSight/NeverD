//===- X87StackManagementTests.cpp - Typed x87 stack operations ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare typed i386/x64 stack rotation and tag invalidation with original
/// instructions. Numerical/classification observations are not full-state
/// proof.
//===----------------------------------------------------------------------===//

#include "../../LLVMHostFixture.h"
#include "X87ProjectionFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <fstream>
#include <vector>

using namespace neverd;

namespace {
BinaryImage image(Arch A, BinaryFormat Format,
                  const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = Format;
  Image.X87ContextOverride = false;
  Image.Entry = 0x1000;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Bytes;
  Text.Size = Text.FileSz = Bytes.size();
  Image.Segments.push_back(Text);
  auto Symbol = Symbol::makeFunc(Image.Entry, Bytes.size());
  Symbol.Name = "stack_probe";
  Image.Symbols.push_back(Symbol);
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Bytes.size());
  return Image;
}

TEST(X87StackManagement, DecodingKeepsPhysicalSlotsAcrossEveryTopAndSelector) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Top = 0; Top != 8; ++Top)
      for (unsigned Index = 0; Index != 8; ++Index)
        for (bool FreePop : {false, true}) {
          SCOPED_TRACE(static_cast<int>(A));
          SCOPED_TRACE(Top);
          SCOPED_TRACE(Index);
          SCOPED_TRACE(FreePop);
          Decoder Dec;
          ASSERT_TRUE(Dec.init(A));
          Dec.setStrict(true);
          for (unsigned I = 0; I != Top; ++I) {
            const std::array<uint8_t, 2> Rotate{0xd9, 0xf7};
            DecodedInsn Insn{};
            ASSERT_EQ(Dec.decodeOneForLift(Rotate.data(), Rotate.size(), 0x1000,
                                           Insn),
                      2);
            std::vector<LowOp> Ops;
            Dec.liftToLow(Insn, Ops);
          }
          const std::array<uint8_t, 2> Bytes =
              FreePop ? std::array<uint8_t, 2>{0xdf, uint8_t(0xc0 + Index)}
                      : std::array<uint8_t, 2>{0xd9, 0xf6};
          DecodedInsn Insn{};
          ASSERT_EQ(
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              2);
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
          ASSERT_EQ(Ops.size(), 1u);
          EXPECT_EQ(Ops[0].Opcode, NdOp::INTRINSIC);
          EXPECT_EQ(Ops[0].Output.Size, 0);
          EXPECT_EQ(Dec.getX86FpuTop(), int((Top + (FreePop ? 1 : 7)) & 7));
          if (FreePop) {
            ASSERT_EQ(Ops[0].NumInputs, 3u);
            EXPECT_EQ(Ops[0].Inputs[1],
                      NdVar::reg(x86reg::stReg((Top + Index) & 7), 10));
            EXPECT_EQ(Ops[0].Inputs[2], NdVar::cst(Index, 1));
          }
          // The typed concrete evaluator has no complete TOP/tag model.
          // Both strict and lenient execution must refuse, not skip the effect.
          for (bool Strict : {false, true}) {
            auto Image = image(A, BinaryFormat::ELF, {0xc3});
            NdOpEmulator E(Image);
            E.setStrictMode(Strict);
            E.setRegister(x86reg::FPU_SW, 0x4700);
            E.setRegister(x86reg::RAX, 0x1234);
            EXPECT_FALSE(E.step(Ops[0]));
            EXPECT_FALSE(E.skips().any());
            EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x4700u);
            EXPECT_EQ(E.getRegister(x86reg::RAX), 0x1234u);
          }
        }
}

TEST(X87StackManagement, MalformedOperandsDoNotChangeTop) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      Decoder Dec;
      ASSERT_TRUE(Dec.init(A));
      Dec.setStrict(true);
      const std::array<uint8_t, 2> Bytes =
          Mutation == 0 ? std::array<uint8_t, 2>{0xd9, 0xf6}
                        : std::array<uint8_t, 2>{0xdf, 0xc1};
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                2);
      auto &X86 = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X86.op_count = 1;
        break;
      case 1:
        X86.op_count = 0;
        break;
      case 2:
        X86.operands[0].type = X86_OP_MEM;
        break;
      case 3:
        X86.operands[0].reg = X86_REG_EAX;
        break;
      case 4:
        X86.operands[0].size = 8;
        break;
      }
      std::vector<LowOp> Ops;
      EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
      EXPECT_EQ(Dec.getX86FpuTop(), 0);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(X87StackManagement,
     RotationCannotInitializeAPayloadAndFreePopEmptiesBothTags) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Kind = 0; Kind != 4; ++Kind) {
        std::vector<uint8_t> Bytes;
        if (Kind != 0)
          Bytes = {0xd9, 0xe8, 0xd9, 0xe8}; // two known values
        if (Kind >= 2)
          Bytes.insert(Bytes.end(), {0xdf, 0xc1}); // free ST1 and pop ST0
        if (Kind != 2)
          Bytes.insert(Bytes.end(), {0xd9, 0xf6}); // rotate to an empty slot
        const va_t Consumer = 0x1000 + Bytes.size();
        Bytes.insert(Bytes.end(), {0xd9, 0xe1, 0xc3}); // fabs requires full tag
        auto Image = image(A, Format, Bytes);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        const auto Low = Builder.build(Image, Dec, Image.Entry);
        EXPECT_FALSE(Low.hasCompleteInstructionLift());
        EXPECT_TRUE(
            llvm::is_contained(Low.UnsupportedInstructionAddresses, Consumer));
      }
}

class X87StackManagementNative : public X87ProjectionFixture {};

TEST_F(X87StackManagementNative,
       PayloadClassificationAndRetainedSignMatchOriginalInstructions) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires Linux x86-64 with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang x86 targets";
  // Restore the prior full slot by rotation; or examine each of the two slots
  // FFREEP ST1 empties. Empty tags retain the original payload's sign.
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Kind = 0; Kind != 5; ++Kind) {
      selectCase(A, Kind);
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(Kind);
      const std::string Target = A == Arch::X86 ? "--target=i386-linux-gnu"
                                                : "--target=x86_64-linux-gnu";
      std::vector<uint8_t> Bytes =
          A == Arch::X86 ? std::vector<uint8_t>{0x8b, 0x4c, 0x24, 4, 0xdb, 0x29}
                         : std::vector<uint8_t>{0xdb, 0x2f}; // fld input
      if (Kind == 0)
        Bytes.insert(Bytes.end(), {0xd9, 0xf7, 0xeb, 0, 0xd9, 0xf6});
      else if (Kind < 3) {
        Bytes.insert(Bytes.end(), {0xdb, uint8_t(A == Arch::X86 ? 0x29 : 0x2f),
                                   0xdf, 0xc1, 0xeb, 0});
        if (Kind == 2)
          Bytes.insert(Bytes.end(), {0xd9, 0xf6});
      } else
        Bytes.insert(Bytes.end(), {0xdf, uint8_t(Kind == 3 ? 0xc0 : 0xc7), 0xeb,
                                   0, 0xd9, 0xf6});
      Bytes.insert(Bytes.end(), {0xd9, 0xe5, 0xdf, 0xe0}); // fxam; fnstsw ax
      if (Kind == 0)
        Bytes.insert(Bytes.end(), {0xdb, uint8_t(A == Arch::X86 ? 0x79 : 0x7f),
                                   0x10}); // store all ten bytes at input+16
      else {
        Bytes.insert(Bytes.end(), {0xd9, 0xf7}); // rotate empty slot
        if (Kind == 2)
          Bytes.insert(Bytes.end(), {0xd9, 0xf7});
      }
      Bytes.insert(Bytes.end(), {0x25, 0, 0x47, 0, 0, 0xc3});
      auto Image = image(A, BinaryFormat::ELF, Bytes);
      const auto NativePath = tmpFile("original.s");
      std::ofstream Native(NativePath);
      Native << ".text\n.globl native_probe\nnative_probe:\n.byte ";
      for (size_t I = 0; I != Bytes.size(); ++I)
        Native << (I ? "," : "") << unsigned(Bytes[I]);
      Native << "\n.globl _start\n_start:\nfninit\ncall test_main\n";
      if (A == Arch::X86)
        Native << "mov %eax,%ebx\nmov $1,%eax\nint $0x80\n";
      else
        Native << "mov %eax,%edi\nmov $60,%eax\nsyscall\n";
      Native << ".section .note.GNU-stack,\"\",@progbits\n";
      Native.close();
      const auto NativeObject = tmpFile("original.o");
      const auto NativeBuild =
          exec(NEVERD_TEST_CLANG, {Target, "-c", NativePath.string(), "-o",
                                   NativeObject.string()});
      ASSERT_TRUE(NativeBuild.ok()) << NativeBuild.err;
      const auto HarnessPath = tmpFile("harness.c");
      std::ofstream(HarnessPath) << R"(
#include <stdint.h>
#include <stddef.h>
void *memcpy(void *out, const void *in, size_t n) {
  unsigned char *d = out; const unsigned char *s = in;
  for (size_t i = 0; i < n; ++i) d[i] = s[i]; return out;
}
void *memset(void *out, int v, size_t n) {
  unsigned char *d = out;
  for (size_t i = 0; i < n; ++i) d[i] = v; return out;
}
struct __attribute__((packed)) Bits { uint64_t significand; uint16_t exponent; };
struct Packet { struct Bits input; unsigned char guard[22]; };
extern unsigned native_probe(struct Packet *);
extern unsigned stack_probe(struct Packet *);
int test_main(void) {
  const uint64_t significands[] = {0, 1, UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
    UINT64_C(0xc000000000000000), UINT64_MAX};
  const unsigned exponents[] = {0, 1, 0x3fff, 0x7ffe, 0x7fff};
  for (unsigned i = 0; i != 7; ++i)
    for (unsigned j = 0; j != 5; ++j)
      for (unsigned sign = 0; sign != 2; ++sign) {
        struct Packet expected, actual;
        memset(&expected, 0xa5, sizeof(expected));
        expected.input.significand = significands[i];
        expected.input.exponent = exponents[j] | (sign << 15);
        memcpy(&actual, &expected, sizeof(expected));
        const unsigned classification = native_probe(&expected);
        if (stack_probe(&actual) != classification) return 1 + i * 10 + j * 2 + sign;
        const unsigned char *a = (const unsigned char *)&actual;
        const unsigned char *e = (const unsigned char *)&expected;
        for (unsigned k = 0; k < sizeof(actual); ++k)
          if (a[k] != e[k]) return 100 + k;
      }
  return 0;
}
)";
      for (bool NoOpt : {false, true}) {
        const std::string Prefix = NoOpt ? "noopt-" : "default-";
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = true;
        Options.NoOpt = NoOpt;
        auto Lift = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Lift.Success) << Lift.Error;
        ASSERT_NE(Lift.LlvmModule, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Lift.LlvmModule, &llvm::errs()));
        const auto IRPath = tmpFile(Prefix + "generated.ll");
        std::string IR;
        llvm::raw_string_ostream OS(IR);
        neverd::test::printHostCompilerFixture(*Lift.LlvmModule, OS);
        std::ofstream(IRPath) << IR;
        CEmitterOptions Emit;
        Emit.TheArch = A;
        Emit.Format = BinaryFormat::ELF;
        Emit.Image = &Image;
        const auto LLVMCPath = tmpFile(Prefix + "llvm.c");
        std::string LLVMC;
        llvm::raw_string_ostream LOS(LLVMC);
        ASSERT_TRUE(
            LLVMCEmitter().emit(*Lift.LlvmModule, LOS, Emit, nullptr, &Image));
        std::ofstream(LLVMCPath) << LLVMC;
        Options.LiftMode = false;
        auto High = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(High.Success) << High.Error;
        const auto CPath = tmpFile(Prefix + "high.c");
        std::string C;
        llvm::raw_string_ostream COS(C);
        ASSERT_TRUE(HighCEmitter().emit(High.HighFuncs, COS, Emit));
        std::ofstream(CPath) << C;
        for (const auto &Source : {IRPath, CPath, LLVMCPath})
          for (const std::string Opt : {"-O0", "-O2"}) {
            SCOPED_TRACE(Source.string());
            SCOPED_TRACE(Opt);
            const auto Binary = tmpFile(Source.stem().string() + Opt + ".exe");
            const auto Build =
                exec(NEVERD_TEST_CLANG,
                     {Target, Opt, "-ffreestanding", "-fno-strict-aliasing",
                      "-fuse-ld=lld", "-nostdlib", "-static", "-no-pie",
                      Source.string(), HarnessPath.string(),
                      NativeObject.string(), "-o", Binary.string()});
            ASSERT_TRUE(Build.ok()) << Build.err;
            const auto Run = exec(Binary.string(), {});
            ASSERT_EQ(Run.exitCode, 0) << Run.err << IR << C << LLVMC;
          }
      }
    }
#endif
}
} // namespace
