//===- X87PackedBCDTests.cpp - Typed packed decimal projection ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify the guarded typed BCD value slice separately from owned state.
//===----------------------------------------------------------------------===//

#include "../../LLVMHostFixture.h"
#include "X87ProjectionFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/X86/LLVMX87GuardedValueAsm.h"
#include "neverd/ir/X86/X87GuardedValue.h"
#include "neverd/ir/high/X86/X87GuardedValueShape.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <csignal>
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
  Symbol.Name = "bcd_probe";
  Image.Symbols.push_back(Symbol);
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Bytes.size());
  return Image;
}

LowOp conversion(Intrinsic Id) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Output = NdVar::tmp(90000, x87GuardedValueBytes(Id));
  Op.addInput(NdVar::cst(unsigned(Id), 2));
  Op.addInput(NdVar::tmp(90001, 10));
  Op.addInput(NdVar::cst(0x37f, 2));
  return Op;
}

TEST(X87GuardedValue, ShapeAndConcreteRefusalAreConsistentAcrossIRs) {
  for (auto Id : {Intrinsic::X87LoadPackedBCD, Intrinsic::X87StorePackedBCD,
                  Intrinsic::X87FptanPair, Intrinsic::X87FsincosPair,
                  Intrinsic::X87FxtractPair})
    for (Arch A : {Arch::X86, Arch::X64})
      for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
        SCOPED_TRACE(unsigned(Id));
        SCOPED_TRACE(Mutation);
        auto Op = conversion(Id);
        switch (Mutation) {
        case 1:
          Op.Inputs[0].Size = 4;
          break;
        case 2:
          Op.Output.Size = 8;
          break;
        case 3:
          Op.Inputs[1].Size = 8;
          break;
        case 4:
          Op.Inputs[2].Size = 8;
          break;
        case 5:
          Op.NumInputs = 2;
          break;
        case 6:
          Op.Inputs[1].Space = VnodeSpace::RAM;
          break;
        case 7:
          Op.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        case 8:
          Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 9:
          Op.Inputs[0].Offset |= UINT64_C(1) << 32;
          break;
        }
        EXPECT_EQ(x87GuardedValueShapeIsValid(x87GuardedValueLowShape(Op, A)),
                  Mutation == 0);
        EXPECT_FALSE(x87GuardedValueShapeIsValid(
            x87GuardedValueLowShape(Op, Arch::AArch64)));
        MedOp Med;
        Med.Opcode = Op.Opcode;
        Med.Output.Kind = MedVar::Temp;
        Med.Output.Size = Op.Output.Size;
        Med.MemoryOrdering = Op.MemoryOrdering;
        Med.MemoryAddressSpace = Op.MemoryAddressSpace;
        for (unsigned I = 0; I != Op.NumInputs; ++I) {
          MedVar V;
          V.Kind = Op.Inputs[I].isRam() ? MedVar::Stack : MedVar::Temp;
          V.Size = Op.Inputs[I].Size;
          if (Op.Inputs[I].isConst())
            V = MedVar::makeConst(Op.Inputs[I].Offset, Op.Inputs[I].Size);
          Med.addInput(V);
        }
        EXPECT_EQ(x87GuardedValueShapeIsValid(x87GuardedValueMedShape(Med, A)),
                  Mutation == 0);
        if (Mutation != 1 && Mutation != 6 && Mutation != 9) {
          HighExpr High;
          High.Kind = ExprKind::Call;
          High.IntrinsicId = Id;
          High.Type = NdType::makeInt(Op.Output.Size, false);
          High.MemoryOrdering = Op.MemoryOrdering;
          High.MemoryAddressSpace = Op.MemoryAddressSpace;
          for (unsigned I = 1; I != Op.NumInputs; ++I)
            High.Operands.push_back(HighExpr::makeVar(
                Med.Inputs[I], NdType::makeInt(Op.Inputs[I].Size, false)));
          EXPECT_EQ(
              x87GuardedValueShapeIsValid(x87GuardedValueHighShape(High, A)),
              Mutation == 0);
        }
        for (bool Strict : {false, true}) {
          auto Image = image(A, BinaryFormat::ELF, {0xc3});
          NdOpEmulator E(Image);
          E.setStrictMode(Strict);
          E.setRegister(90000, 0x1234);
          E.setRegister(x86reg::FPU_SW, 0x4700);
          EXPECT_FALSE(E.step(Op));
          EXPECT_EQ(E.getRegister(90000), 0x1234u);
          EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x4700u);
          EXPECT_FALSE(E.skips().any());
        }
      }
}

TEST(X87PackedBCD, InvalidDecodedOperandsCannotMoveTopOrReadMemory) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Load : {false, true})
      for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
        Decoder Dec;
        ASSERT_TRUE(Dec.init(A));
        Dec.setStrict(true);
        const std::array<uint8_t, 2> Bytes{0xdf, uint8_t(Load ? 0x20 : 0x30)};
        DecodedInsn Insn{};
        ASSERT_EQ(
            Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn), 2);
        auto &X86 = Insn.Raw->detail->x86;
        if (Mutation == 0)
          X86.op_count = 0;
        if (Mutation == 1)
          X86.operands[0].type = X86_OP_REG;
        if (Mutation == 2)
          X86.operands[0].size = 8;
        std::vector<LowOp> Ops;
        EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        EXPECT_EQ(Dec.getX86FpuTop(), 0);
      }
}

TEST(X87GuardedValue, GuardedAssemblyCannotBeReinterpretedAfterMutation) {
  for (auto Id : {Intrinsic::X87LoadPackedBCD, Intrinsic::X87StorePackedBCD,
                  Intrinsic::X87FptanPair, Intrinsic::X87FsincosPair,
                  Intrinsic::X87FxtractPair}) {
    llvm::LLVMContext Context;
    llvm::Module Module("bcd-shape", Context);
    auto *Ptr = llvm::PointerType::get(Context, 0);
    auto *Type = llvm::FunctionType::get(llvm::Type::getVoidTy(Context),
                                         {Ptr, Ptr, Ptr, Ptr}, false);
    auto *Function = llvm::Function::Create(
        Type, llvm::GlobalValue::ExternalLinkage, "probe", Module);
    llvm::IRBuilder<> Builder(
        llvm::BasicBlock::Create(Context, "entry", Function));
    auto *Call = Builder.CreateCall(
        llvm::InlineAsm::get(Type, x87GuardedValueAsm(Id),
                             x87GuardedValueConstraints(), true),
        {Function->getArg(0), Function->getArg(1), Function->getArg(2),
         Function->getArg(3)});
    Call->setMetadata(X87GuardedValueAsmMetadata,
                      llvm::MDNode::get(Context, {}));
    EXPECT_EQ(classifyX87GuardedValueAsm(*Call), Id);
    Call->setCalledOperand(
        llvm::InlineAsm::get(Type, "nop", x87GuardedValueConstraints(), true));
    EXPECT_DEATH_IF_SUPPORTED((void)classifyX87GuardedValueAsm(*Call),
                              "invalid guarded x87 value assembly");
    Call->setCalledOperand(
        llvm::InlineAsm::get(Type, x87GuardedValueAsm(Id), "r,r,r,r", true));
    EXPECT_DEATH_IF_SUPPORTED((void)classifyX87GuardedValueAsm(*Call),
                              "invalid guarded x87 value assembly");
  }
}

TEST(X87PackedBCD, StackFaultsAndUnknownPayloadsCannotAcquireATypedProjection) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Load : {false, true}) {
        std::vector<uint8_t> Bytes{0xdb, 0xe3};
        if (Load)
          for (unsigned I = 0; I != 8; ++I)
            Bytes.insert(Bytes.end(), {0xd9, 0xe8});
        const va_t Address = 0x1000 + Bytes.size();
        Bytes.insert(Bytes.end(), {0xdf, uint8_t(Load ? 0x20 : 0x30), 0xc3});
        auto Image = image(A, Format, Bytes);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Image));
        CFGBuilder Builder;
        const auto Low = Builder.build(Image, Dec, Image.Entry);
        EXPECT_FALSE(Low.hasCompleteInstructionLift());
        EXPECT_TRUE(
            llvm::is_contained(Low.UnsupportedInstructionAddresses, Address));
      }
}

class X87PackedBCDNative : public X87ProjectionFixture {};

TEST_F(X87PackedBCDNative, OriginalBitsStatusAndGuardsAcrossBothModes) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires Linux x86-64 with native i386 execution";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang x86 targets";
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Load : {false, true}) {
      SCOPED_TRACE(unsigned(A));
      SCOPED_TRACE(Load);
      selectCase(A, unsigned(Load));
      const std::string Target = A == Arch::X86 ? "--target=i386-linux-gnu"
                                                : "--target=x86_64-linux-gnu";
      const uint8_t Base = A == Arch::X86 ? 1 : 7;
      std::vector<uint8_t> Bytes =
          A == Arch::X86 ? std::vector<uint8_t>{0x8b, 0x4c, 0x24, 4}
                         : std::vector<uint8_t>{};
      Bytes.insert(Bytes.end(), {0xdb,
                                 0xe3,
                                 0xd9,
                                 uint8_t(0x68 | Base),
                                 32,
                                 uint8_t(Load ? 0xdf : 0xdb),
                                 uint8_t((Load ? 0x20 : 0x28) | Base),
                                 0xeb,
                                 0,
                                 uint8_t(Load ? 0xdb : 0xdf),
                                 uint8_t((Load ? 0x78 : 0x70) | Base),
                                 16,
                                 0xdf,
                                 0xe0,
                                 0x25,
                                 0x3f,
                                 2,
                                 0,
                                 0,
                                 0xc3});
      auto Image = image(A, BinaryFormat::ELF, Bytes);
      const auto NativePath = tmpFile("original.s");
      std::ofstream Native(NativePath);
      Native << ".text\n.globl native_probe\nnative_probe:\n.byte ";
      for (size_t I = 0; I != Bytes.size(); ++I)
        Native << (I ? "," : "") << unsigned(Bytes[I]);
      Native << "\n.section .note.GNU-stack,\"\",@progbits\n";
      Native.close();
      const auto NativeObject = tmpFile("original.o");
      const auto NativeBuild =
          exec(NEVERD_TEST_CLANG, {Target, "-c", NativePath.string(), "-o",
                                   NativeObject.string()});
      ASSERT_TRUE(NativeBuild.ok()) << NativeBuild.err;
      const auto HarnessPath = tmpFile("harness.c");
      std::ofstream(HarnessPath) << "#define BCD_LOAD " << unsigned(Load) << R"(
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <unistd.h>
void *memcpy(void *out, const void *in, size_t n) {
  unsigned char *d = out; const unsigned char *s = in;
  for (size_t i = 0; i < n; ++i) d[i] = s[i]; return out;
}
void *memset(void *out, int v, size_t n) {
  unsigned char *d = out;
  for (size_t i = 0; i < n; ++i) d[i] = v; return out;
}
struct Packet { unsigned char bytes[48]; };
extern unsigned native_probe(struct Packet *);
extern unsigned bcd_probe(struct Packet *);
static void rejected_input(int number) { _exit(128 + number); }
int main(int argc, char **argv) {
  const char *argument = argc > 1 ? argv[1] : 0;
  if (argument) {
    if (signal(SIGILL, rejected_input) == SIG_ERR) return 99;
    unsigned mode = 0;
    while (*argument) mode = mode * 10 + (*argument++ - '0');
    struct Packet p; memset(&p, 0, sizeof(p));
    uint16_t control = 0x37f;
    if (mode <= 108) {
      unsigned digit = (mode - 1) / 6, value = 10 + (mode - 1) % 6;
      p.bytes[digit / 2] = value << ((digit & 1) * 4);
    } else if (mode <= 172) control = 0x340 | (mode - 109);
    else control = 0x17f;
    memcpy(p.bytes + 32, &control, 2);
    (void)bcd_probe(&p); return 0;
  }
  const uint64_t significands[] = {0, 1, UINT64_C(0x7fffffffffffffff),
    UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000001),
    UINT64_C(0xc000000000000000), UINT64_MAX};
  const unsigned exponents[] = {0, 1, 0x3fff, 0x7ffe, 0x7fff};
  const uint64_t magnitudes[] = {0, 1, 9, 10,
    UINT64_C(123456789012345678), UINT64_C(999999999999999999)};
  for (unsigned pc = 0; pc != 4; ++pc) {
    if (pc == 1) continue;
    for (unsigned rc = 0; rc != 4; ++rc)
      for (unsigned i = 0; i != (BCD_LOAD ? 6 : 7); ++i)
        for (unsigned j = 0; j != (BCD_LOAD ? 1 : 5); ++j)
          for (unsigned sign = 0; sign != 2; ++sign) {
            struct Packet expected, actual;
            memset(&expected, 0xa5, sizeof(expected));
            if (BCD_LOAD) {
              uint64_t digits = magnitudes[i];
              for (unsigned k = 0; k != 9; ++k) {
                expected.bytes[k] = digits % 10; digits /= 10;
                expected.bytes[k] |= (digits % 10) << 4; digits /= 10;
              }
              expected.bytes[9] = 0x7f | (sign << 7);
            } else {
              uint16_t exponent = exponents[j] | (sign << 15);
              memcpy(expected.bytes, &significands[i], 8);
              memcpy(expected.bytes + 8, &exponent, 2);
            }
            uint16_t control = 0x7f | (pc << 8) | (rc << 10);
            memcpy(expected.bytes + 32, &control, 2);
            memcpy(&actual, &expected, sizeof(expected));
            unsigned status = native_probe(&expected);
            if (bcd_probe(&actual) != status) return 1 + i * 10 + j * 2 + sign;
            for (unsigned k = 0; k != sizeof(actual); ++k)
              if (actual.bytes[k] != expected.bytes[k]) return 100 + k;
          }
  }
  return 0;
}
)";
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(NoOpt);
        const std::string Prefix = NoOpt ? "noopt-" : "default-";
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
                      "-fuse-ld=lld", "-no-pie", Source.string(),
                      HarnessPath.string(), NativeObject.string(), "-o",
                      Binary.string()});
            ASSERT_TRUE(Build.ok()) << Build.err;
            const auto Run = exec(Binary.string(), {});
            ASSERT_EQ(Run.exitCode, 0) << Run.err << IR << C << LLVMC;
            if (!NoOpt && Opt == "-O2") {
              // Every invalid digit and every unmasked CW must trap in each
              // generated backend. Native undefined results are not an oracle.
              for (unsigned Mode = Load ? 1 : 109; Mode <= 173; ++Mode) {
                if (Mode == 172)
                  continue; // all masks is the admitted case
                SCOPED_TRACE(Mode);
                const auto Rejected =
                    exec(Binary.string(), {std::to_string(Mode)});
                ASSERT_EQ(Rejected.exitCode, 128 + SIGILL) << Rejected.err;
              }
            }
          }
      }
    }
#endif
}
} // namespace
