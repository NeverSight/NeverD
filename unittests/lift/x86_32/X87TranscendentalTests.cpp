//===- X87TranscendentalTests.cpp - x87 status tests ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Observe finite native trigonometric results and defined status consumers
/// separately from complete x87 state and processor-independent exactness.
//===----------------------------------------------------------------------===//

#include "../../LLVMHostFixture.h"
#include "X87ProjectionFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/high/X86/X87ValueShape.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/IntrinsicShapes.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <vector>

using namespace neverd;

namespace {
BinaryImage image(Arch A, const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = A;
  Image.Bits = A == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
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
  Symbol.Name = "trig_probe";
  Image.Symbols.push_back(Symbol);
  Image.KnownCodeRanges.emplace_back(Image.Entry, Image.Entry + Bytes.size());
  return Image;
}

TEST(X87PushPair, StackFaultsAndUnknownPayloadsCannotAcquireATypedProjection) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (uint8_t Opcode : {0xf2, 0xfb, 0xf4})
        for (unsigned Setup = 0; Setup != 3; ++Setup) {
          std::vector<uint8_t> Bytes{0xdb, 0xe3}; // FNINIT
          if (Setup == 1)
            for (unsigned I = 0; I != 8; ++I)
              Bytes.insert(Bytes.end(), {0xd9, 0xe8}); // FLD1: full stack
          if (Setup == 2)
            Bytes.insert(Bytes.end(), {0xd9, 0xe8, 0xdd, 0xc0}); // Free ST0
          const va_t Address = 0x1000 + Bytes.size();
          Bytes.insert(Bytes.end(), {0xd9, Opcode, 0xc3});
          auto Image = image(A, Bytes);
          Image.Format = Format;
          Decoder Dec;
          ASSERT_TRUE(Dec.init(Image));
          CFGBuilder Builder;
          const auto Low = Builder.build(Image, Dec, Image.Entry);
          EXPECT_FALSE(Low.hasCompleteInstructionLift());
          EXPECT_TRUE(
              llvm::is_contained(Low.UnsupportedInstructionAddresses, Address));
        }
}

TEST(X87ValueExecution, UnmodelledNativeValuesCannotBeSkipped) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (auto Id :
           {Intrinsic::X87Fsin, Intrinsic::X87Fcos, Intrinsic::X87F2xm1,
            Intrinsic::X87Fscale, Intrinsic::X87Fpatan, Intrinsic::X87Fyl2x,
            Intrinsic::X87Fyl2xp1, Intrinsic::X87Fptan,
            Intrinsic::X87Fxtractsig, Intrinsic::X87Fxtractexp}) {
        auto Image = image(A, {0xc3});
        NdOpEmulator E(Image);
        E.setStrictMode(Strict);
        E.setRegister(90000, 0x1234);
        E.setRegister(x86reg::FPU_SW, 0x4700);
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.Output = NdVar::tmp(90000, 10);
        Op.addInput(NdVar::cst(unsigned(Id), 2));
        Op.addInput(NdVar::reg(x86reg::ST0, 10));
        if (Id == Intrinsic::X87Fscale || Id == Intrinsic::X87Fpatan ||
            Id == Intrinsic::X87Fyl2x || Id == Intrinsic::X87Fyl2xp1)
          Op.addInput(NdVar::reg(x86reg::ST1, 10));
        EXPECT_FALSE(E.step(Op));
        EXPECT_EQ(E.getRegister(90000), 0x1234u);
        EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x4700u);
        EXPECT_FALSE(E.skips().any());
      }
}

TEST(X87ValueContract, MissingSecondOperandIsRejectedAcrossLowAndMed) {
  for (Arch A : {Arch::X86, Arch::X64}) {
    auto Image = image(
        A, {0xdb, 0xe3, 0xd9, 0xe8, 0xd9, 0xe8, 0xd9, 0xfd, 0xdf, 0xe0, 0xc3});
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    CFGBuilder Builder;
    auto Low = Builder.build(Image, Dec, Image.Entry);
    ASSERT_TRUE(Low.hasCompleteInstructionLift());
    auto Med = LowToMedConverter().convert(Low, A, BinaryFormat::ELF);
    ASSERT_TRUE(verifyMedFunc(Med, "valid-x87-value"));
    llvm::LLVMContext Context;
    EXPECT_DEATH_IF_SUPPORTED((void)MedLLVMEmitter().emit(
                                  {Med}, Context, "wrong-host", Arch::AArch64),
                              "invalid x87 scalar value/status contract");
    unsigned LowChanges = 0, MedChanges = 0;
    for (auto &Block : Low.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].Offset == unsigned(Intrinsic::X87Fscale)) {
          ASSERT_EQ(Op.NumInputs, 3);
          Op.NumInputs = 2;
          ++LowChanges;
        }
    for (auto &Block : Med.Blocks)
      for (auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs &&
            Op.Inputs[0].isConst() &&
            Op.Inputs[0].ConstVal == unsigned(Intrinsic::X87Fscale)) {
          ASSERT_EQ(Op.NumInputs, 3);
          Op.NumInputs = 2;
          ++MedChanges;
        }
    ASSERT_EQ(LowChanges, 1u);
    ASSERT_EQ(MedChanges, 1u);
    auto Error = validateLowInstructionBoundaries(Low);
    EXPECT_TRUE(bool(Error));
    llvm::consumeError(std::move(Error));
    EXPECT_FALSE(verifyMedFunc(Med, "missing-x87-value-operand"));
    EXPECT_DEATH_IF_SUPPORTED(
        (void)MedLLVMEmitter().emit({Med}, Context, "invalid-value", A),
        "invalid x87 scalar value/status contract");
  }
}

TEST(X87ValueContract, HighCRejectsMissingOperandsAndLegacySinglePush) {
  for (auto Id : {Intrinsic::X87Fscale, Intrinsic::X87Fptan,
                  Intrinsic::X87Fxtractsig, Intrinsic::X87Fxtractexp}) {
    HighFunc Func;
    Func.Entry = 0x1000;
    Func.Name = "invalid_value";
    Func.ReturnType = NdType::makeInt(10, false);
    auto Call =
        HighExpr::makeCall(intrinsicName(Id), 0, {HighExpr::makeConst(0, 10)});
    Call->IntrinsicId = Id;
    Call->Type = Func.ReturnType;
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = std::move(Call);
    Func.Body.push_back(std::move(Return));
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    EXPECT_DEATH_IF_SUPPORTED((void)HighCEmitter().emit({Func}, OS, Options),
                              "invalid HighC x87 scalar value/status contract");
  }
}

TEST(X87ValueContract, ArityWidthsAndLegacyRefusalAreSharedAcrossIRs) {
  struct Case {
    Intrinsic Id;
    unsigned Operands;
    bool Admitted;
  };
  const Case Cases[] = {
      {Intrinsic::X87Fsin, 1, true},       {Intrinsic::X87Fcos, 1, true},
      {Intrinsic::X87F2xm1, 1, true},      {Intrinsic::X87Fscale, 2, true},
      {Intrinsic::X87Fpatan, 2, true},     {Intrinsic::X87Fyl2x, 2, true},
      {Intrinsic::X87Fyl2xp1, 2, true},    {Intrinsic::X87Fprem, 2, true},
      {Intrinsic::X87Fprem1, 2, true},     {Intrinsic::X87ReadStatus, 0, true},
      {Intrinsic::X87Fptan, 1, false},     {Intrinsic::X87Fxtractsig, 1, false},
      {Intrinsic::X87Fxtractexp, 1, false}};
  for (const auto C : Cases)
    for (Arch A : {Arch::X86, Arch::X64})
      for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
        SCOPED_TRACE(unsigned(C.Id));
        SCOPED_TRACE(Mutation);
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.Output = NdVar::tmp(90000, C.Operands ? 10 : 2);
        Op.addInput(NdVar::cst(unsigned(C.Id), 2));
        for (unsigned I = 0; I != C.Operands; ++I)
          Op.addInput(NdVar::tmp(90001 + I, 10));
        switch (Mutation) {
        case 1:
          Op.Output.Size = 8;
          break;
        case 2:
          --Op.NumInputs;
          break;
        case 3:
          Op.addInput(NdVar::tmp(90003, 10));
          break;
        case 4:
          Op.Inputs[0].Size = 4;
          break;
        case 5:
          if (C.Operands)
            Op.Inputs[1].Size = 8;
          else
            Op.Output.Size = 4;
          break;
        case 6:
          Op.MemoryOrdering = NdMemoryOrdering::Acquire;
          break;
        case 7:
          Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
          break;
        case 8:
          Op.Inputs[0].Offset |= UINT64_C(1) << 32;
          break;
        case 9:
          Op.Output.Space = VnodeSpace::CONST;
          break;
        case 10:
          Op.Inputs[C.Operands ? 1 : 0].Space = VnodeSpace::RAM;
          break;
        }
        const bool Valid = C.Admitted && Mutation == 0;
        EXPECT_EQ(x87ValueShapeIsValid(x87ValueLowShape(Op, A)), Valid);
        EXPECT_FALSE(x87ValueShapeIsValid(x87ValueLowShape(Op, Arch::AArch64)));
        MedOp Med;
        Med.Opcode = Op.Opcode;
        Med.Output.Kind = Op.Output.isConst() ? MedVar::Const : MedVar::Temp;
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
        EXPECT_EQ(x87ValueShapeIsValid(x87ValueMedShape(Med, A)), Valid);
        // ID carriers, RAM varnodes and writable outputs have no equivalent
        // spelling as a HighIR call expression; check the shared value shapes.
        if (Mutation == 4 || Mutation == 8 || Mutation == 9 || Mutation == 10 ||
            (Mutation == 2 && C.Operands == 0))
          continue;
        HighExpr High;
        High.Kind = ExprKind::Call;
        High.IntrinsicId = C.Id;
        High.Type = NdType::makeInt(Op.Output.Size, false);
        High.MemoryOrdering = Op.MemoryOrdering;
        High.MemoryAddressSpace = Op.MemoryAddressSpace;
        for (unsigned I = 1; I < Op.NumInputs; ++I)
          High.Operands.push_back(HighExpr::makeVar(
              Med.Inputs[I], NdType::makeInt(Op.Inputs[I].Size, false)));
        EXPECT_EQ(x87ValueShapeIsValid(x87ValueHighShape(High, A)), Valid);
      }
}

TEST(X87ValueExecution, CallsCannotTurnUnknownStatusIntoZero) {
  for (Arch A : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (auto Id : {Intrinsic::X87ReadStatus, Intrinsic::X87Fnclex,
                      Intrinsic::X87Fprem, Intrinsic::X87Fprem1}) {
        auto Image = image(A, {0xc3});
        NdOpEmulator E(Image);
        E.setStrictMode(Strict);
        E.setRegister(90000, 0x1234);
        E.setCallPreservedRegisters({90000});
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(NdVar::cst(0x2000, A == Arch::X86 ? 4 : 8));
        ASSERT_TRUE(E.step(Call));
        ASSERT_FALSE(E.getRegister(x86reg::FPU_SW));
        LowOp Op;
        Op.Opcode = NdOp::INTRINSIC;
        Op.addInput(NdVar::cst(unsigned(Id), 2));
        if (Id == Intrinsic::X87ReadStatus)
          Op.Output = NdVar::tmp(90000, 2);
        if (Id == Intrinsic::X87Fprem || Id == Intrinsic::X87Fprem1) {
          const uint8_t One[] = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
          E.setRegisterBytes(x86reg::ST0, One);
          E.setRegisterBytes(x86reg::ST1, One);
          Op.Output = NdVar::tmp(90000, 10);
          Op.addInput(NdVar::reg(x86reg::ST0, 10));
          Op.addInput(NdVar::reg(x86reg::ST1, 10));
        }
        EXPECT_FALSE(E.step(Op));
        EXPECT_EQ(E.getRegister(90000), 0x1234u);
        EXPECT_FALSE(E.getRegister(x86reg::FPU_SW));
        EXPECT_FALSE(E.skips().any());
      }
}

class X87TranscendentalNative : public X87ProjectionFixture {
protected:
  enum class Family { UnaryTrigonometric, PushPair, OtherValue };
  void check(Family Group, bool StatusOnly = false);
};

void X87TranscendentalNative::check(Family Group, bool StatusOnly) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "requires native Linux x64 and i386 C runtimes";
#else
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang x86 targets";
  const bool Pairs = Group == Family::PushPair;
  const bool Other = Group == Family::OtherValue;
  for (Arch A : {Arch::X86, Arch::X64})
    for (unsigned Kind = Pairs   ? 2
                         : Other ? 5
                                 : 0;
         Kind != (Pairs   ? 5u
                  : Other ? 10u
                          : 2u);
         ++Kind) {
      SCOPED_TRACE(unsigned(A));
      SCOPED_TRACE(Kind);
      selectCase(A, Kind + (StatusOnly ? 10 : 0));
      const bool Binary = Kind >= 6;
      const uint8_t Opcodes[] = {0xfe, 0xff, 0xf2, 0xfb, 0xf4,
                                 0xf0, 0xfd, 0xf3, 0xf1, 0xf9};
      const std::string Target = A == Arch::X86 ? "--target=i386-linux-gnu"
                                                : "--target=x86_64-linux-gnu";
      const uint8_t Base = A == Arch::X86 ? 1 : 7;
      std::vector<uint8_t> Bytes =
          A == Arch::X86 ? std::vector<uint8_t>{0x8b, 0x4c, 0x24, 4}
                         : std::vector<uint8_t>{};
      Bytes.insert(Bytes.end(), {0xdb, 0xe3,                       // FNINIT
                                 0xd9, uint8_t(0x68 | Base), 32}); // FLDCW
      if (Binary)
        Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x68 | Base), 64});
      Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x28 | Base),      // FLD m80
                                 0xd9, Opcodes[Kind], 0xdf, 0xe0, // FNSTSW AX
                                 0x25, 0x3f, uint8_t(Kind >= 4 ? 2 : 6), 0,
                                 0, // defined flags
                                 0xdb, uint8_t(0x78 | Base), 16}); // FSTP m80
      if (StatusOnly) {
        Bytes.resize(Bytes.size() - 3);
        Bytes.insert(Bytes.end(),
                     {0xdb, 0xe3}); // Discard every numeric result.
      }
      if (Pairs && !StatusOnly)
        Bytes.insert(Bytes.end(), {0xdb, uint8_t(0x78 | Base), 48});
      if (Kind == 6 && !StatusOnly)
        Bytes.insert(Bytes.end(), {0xdd, 0xd8}); // Discard FSCALE's ST1.
      Bytes.insert(Bytes.end(), {0xa9, 0, 4, 0,
                                 0,       // TEST C2
                                 0x74, 6, // JZ in range
                                 0xc6, uint8_t(0x40 | Base), 40, 1, 0xeb, 4,
                                 0xc6, uint8_t(0x40 | Base), 40, 0, 0xc3});
      auto Image = image(A, Bytes);
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
      std::ofstream(HarnessPath)
          << "#define PAIR_TEST " << unsigned(Pairs) << "\n#define KIND "
          << Kind << "\n#define OTHER_VALUE " << unsigned(Other)
          << "\n#define BINARY_VALUE " << unsigned(Binary) << R"(
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
struct Packet { unsigned char bytes[96]; };
extern unsigned native_probe(struct Packet *);
extern unsigned trig_probe(struct Packet *);
static void rejected_input(int number) { _exit(128 + number); }
int main(void) {
  const uint64_t significands[] = {UINT64_C(0x8000000000000000),
    UINT64_C(0x8000000000000001), UINT64_C(0xbfffffffffffffff), UINT64_MAX};
#if OTHER_VALUE
  const uint16_t exponents[] = {0, 1, 0x3ffb, 0x3ffc, 0x3ffd, 0x3ffe,
                               0x3fff, 0x4000, 0x403d, 0x403e, 0x7ffe};
#else
  const uint16_t exponents[] = {0, 1, 0x3ffd, 0x3ffe, 0x3fff,
                                0x403d, 0x403e, 0x403f, 0x7ffe};
#endif
  const uint64_t other_sig[] = {0, 0, UINT64_C(0x8000000000000000),
    UINT64_C(0x8000000000000000), UINT64_C(0xc000000000000000),
    UINT64_C(0xc000000000000000)};
  const uint16_t other_exp[] = {0, 0x8000, 0x3ffe, 0xbffe, 0x4000, 0xc000};
  for (unsigned pc = 0; pc != 4; ++pc) {
    if (pc == 1) continue;
    for (unsigned rc = 0; rc != 4; ++rc)
      for (unsigned i = 0; i != 4; ++i)
        for (unsigned j = 0; j != sizeof(exponents) / sizeof(exponents[0]); ++j)
          for (unsigned sign = 0; sign != 2; ++sign)
           for (unsigned other = 0; other != (BINARY_VALUE ? 6 : 1); ++other) {
            if (PAIR_TEST && KIND != 4 && exponents[j] >= 0x403e) continue;
            // Restrict approximation probes to the documented input domain.
            if (KIND == 5 && (exponents[j] > 0x3fff ||
                (exponents[j] == 0x3fff && i != 0))) continue;
            if (KIND == 8 && (sign || (!exponents[j] && i == 0))) continue;
            if (KIND == 9 && exponents[j] > 0x3ffc) continue;
            struct Packet expected, actual;
            memset(&expected, 0xa5, sizeof(expected));
            uint64_t sig = significands[i];
            if (!exponents[j]) sig &= UINT64_C(0x7fffffffffffffff);
            const uint16_t exponent = exponents[j] | (sign << 15);
            const uint16_t control = 0x7f | (pc << 8) | (rc << 10);
            memcpy(expected.bytes, &sig, 8);
            memcpy(expected.bytes + 8, &exponent, 2);
            memcpy(expected.bytes + 32, &control, 2);
            if (BINARY_VALUE) {
              memcpy(expected.bytes + 64, &other_sig[other], 8);
              memcpy(expected.bytes + 72, &other_exp[other], 2);
            }
            memcpy(&actual, &expected, sizeof(actual));
            unsigned native = native_probe(&expected), generated = trig_probe(&actual);
            if (native != generated || memcmp(&expected, &actual, sizeof(actual))) {
              printf("cw=%04x value=%04x:%016llx native=%04x generated=%04x "
                     "native-branch=%u generated-branch=%u\n", control, exponent,
                     (unsigned long long)sig, native, generated,
                     expected.bytes[40], actual.bytes[40]);
              return 1;
            }
          }
  }
  if (PAIR_TEST) {
    struct Packet *shared = mmap(0, sizeof(*shared), PROT_READ | PROT_WRITE,
                                 MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED) return 2;
    for (unsigned mode = 0; mode != 72; ++mode) {
      if (KIND == 4 && mode >= 70) continue;
      for (unsigned sign = 0; sign != 2; ++sign) {
        struct Packet before; memset(&before, 0xa5, sizeof(before));
        uint16_t control = 0x37f, exponent = 0x3fff;
        uint64_t sig = UINT64_C(0x8000000000000000);
        if (mode < 63) control = 0x340 | mode;
        else if (mode == 63) control = 0x17f;
        else if (mode == 64) control |= 0x8000;
        else if (mode < 68) {
          exponent = 0x7fff;
          if (mode == 66) sig |= UINT64_C(0x4000000000000000);
          if (mode == 67) sig |= 1;
        } else if (mode == 68) sig = 1;
        else if (mode == 69) exponent = 0;
        else exponent = mode == 70 ? 0x403e : 0x403f;
        exponent |= sign << 15;
        memcpy(before.bytes, &sig, 8);
        memcpy(before.bytes + 8, &exponent, 2);
        memcpy(before.bytes + 32, &control, 2);
        memcpy(shared, &before, sizeof(before));
        pid_t child = fork();
        if (child < 0) return 3;
        if (!child) {
          if (signal(SIGILL, rejected_input) == SIG_ERR) _exit(99);
          (void)trig_probe(shared); _exit(0);
        }
        int status;
        if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 128 + SIGILL ||
            memcmp(shared, &before, sizeof(before))) {
          printf("guard mode=%u sign=%u status=%x\n", mode, sign, status);
          return 4;
        }
      }
    }
    munmap(shared, sizeof(*shared));
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
                     {Target, Opt, "-fno-strict-aliasing", "-fuse-ld=lld",
                      "-no-pie", Source.string(), HarnessPath.string(),
                      NativeObject.string(), "-o", Binary.string()});
            ASSERT_TRUE(Build.ok()) << Build.err;
            const auto Run = exec(Binary.string(), {});
            ASSERT_EQ(Run.exitCode, 0)
                << Run.out << Run.err << IR << C << LLVMC;
          }
      }
    }
#endif
}

TEST_F(X87TranscendentalNative, SineCosineStatusAndRangeBranchMatchOriginal) {
  check(Family::UnaryTrigonometric);
}

TEST_F(X87TranscendentalNative,
       GuardedPairsMatchOriginalAndRejectOutsideScope) {
  check(Family::PushPair);
}

TEST_F(X87TranscendentalNative, DiscardingValuesKeepsStatusAndRangeBranch) {
  check(Family::UnaryTrigonometric, true);
  check(Family::PushPair, true);
}

TEST_F(X87TranscendentalNative, OtherValueFamiliesMatchDefinedDomainAndStatus) {
  check(Family::OtherValue);
}
} // namespace
