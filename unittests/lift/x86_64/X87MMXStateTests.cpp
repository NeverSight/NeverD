//===- X87MMXStateTests.cpp - Owned MMX reset semantics ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Separate defined tag transitions from undefined FEMMS payloads, and keep
/// the original instruction in every native source projection.
//===----------------------------------------------------------------------===//

#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX87ContextAsm.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/low/X86/X87NativeContext.h"

#include "llvm/Object/ObjectFile.h"

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif

namespace {
constexpr uint64_t ResultRegister = 90001;

LowOp resetStep(uint32_t Selector) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X87ContextStep), 2));
  Op.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
  Op.addInput(NdVar::cst(Selector, 4));
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Entry, 8));
  return Op;
}

LowOp mmxStep(Intrinsic Id, unsigned Register = 0, uint64_t Value = 0) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(unsigned(Id), 2));
  Op.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
  if (Id != Intrinsic::X87ContextMMXEnter)
    Op.addInput(NdVar::cst(Register, 4));
  if (Id == Intrinsic::X87ContextMMXWrite)
    Op.addInput(NdVar::cst(Value, 8));
  if (Id == Intrinsic::X87ContextMMXRead)
    Op.Output = NdVar::tmp(ResultRegister, 8);
  return Op;
}

std::array<uint8_t, 512> incoming(unsigned Top, uint8_t Tags) {
  std::array<uint8_t, 512> State{};
  State[0] = 0x7f;
  State[1] = 3;
  // All condition codes and masked sticky exceptions are retained.
  State[2] = 0x25;
  State[3] = 0x47 | (Top << 3);
  State[4] = Tags;
  for (unsigned I = 6; I < 24; ++I)
    State[I] = I * 7;
  for (unsigned I = 32; I < 160; ++I)
    State[I] = I * 13 + 5;
  return State;
}

TEST(X87MMXState, ResetClearsTagsAndOnlyFemmsForgetsEveryPayload) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (bool Strict : {false, true})
    for (unsigned Top = 0; Top < 8; ++Top)
      for (uint8_t Tags : {0, 0x55, 0xff})
        for (uint32_t Selector : {0x770fU, 0x0e0fU}) {
          SCOPED_TRACE(Selector);
          SCOPED_TRACE(Top);
          NdOpEmulator E(Img);
          E.setStrictMode(Strict);
          const auto Before = incoming(Top, Tags);
          ASSERT_TRUE(E.setX87Context(Before));
          ASSERT_TRUE(E.step(resetStep(Selector)));
          EXPECT_EQ(E.getRegister(x86reg::FPU_CW), 0x037f);
          EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x4725 | (Top << 11));
          if (Selector == 0x770f) {
            auto Expected = Before;
            Expected[4] = 0;
            EXPECT_EQ(E.getX87Context(), Expected);
            continue;
          }
          EXPECT_FALSE(E.getX87Context());
          for (unsigned I = 0; I < 8; ++I) {
            EXPECT_FALSE(E.getRegisterBytes(x86reg::stReg(I)));
            EXPECT_FALSE(E.step(mmxStep(Intrinsic::X87ContextMMXRead, I)));
          }
          ASSERT_TRUE(E.step(mmxStep(Intrinsic::X87ContextMMXEnter)));
          // Unknown, now nonempty MMX aliases must still permit a reset.
          ASSERT_TRUE(E.step(resetStep(Selector)));
          ASSERT_TRUE(E.step(mmxStep(Intrinsic::X87ContextMMXEnter)));
          for (unsigned I = 0; I < 8; ++I) {
            ASSERT_TRUE(E.step(mmxStep(Intrinsic::X87ContextMMXWrite, I,
                                       UINT64_C(0x1020304050607080) + I)));
            ASSERT_TRUE(E.step(mmxStep(Intrinsic::X87ContextMMXRead, I)));
            EXPECT_EQ(E.getRegister(ResultRegister),
                      UINT64_C(0x1020304050607080) + I);
            EXPECT_EQ(bool(E.getX87Context()), I == 7);
          }
          auto After = E.getX87Context();
          ASSERT_TRUE(After);
          EXPECT_EQ((*After)[4], 0xff);
          EXPECT_TRUE(std::equal(Before.begin() + 6, Before.begin() + 24,
                                 After->begin() + 6));
        }
}

TEST(X87MMXState, PendingExceptionsAndUnknownInputDoNotCommitAReset) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  for (bool Strict : {false, true})
    for (uint32_t Selector : {0x770fU, 0x0e0fU}) {
      NdOpEmulator E(Img);
      E.setStrictMode(Strict);
      EXPECT_FALSE(E.step(resetStep(Selector)));
      auto State = incoming(3, 0xff);
      State[0] &= ~1U; // Unmasked invalid exception is pending.
      State[2] |= 0x81;
      ASSERT_TRUE(E.setX87Context(State));
      EXPECT_FALSE(E.step(resetStep(Selector)));
      EXPECT_EQ(E.getX87Context(), State);
      E.setX87PortableMode(true);
      EXPECT_FALSE(E.step(resetStep(Selector)));
      EXPECT_EQ(E.getX87Context(), State);
    }
}

TEST(X87MMXState, FreshX87LoadsRebuildOnlyTheWrittenPayloadsAfterFemms) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Segment Data;
  Data.VA = 0x3000;
  Data.Flags = SegmentFlags::Readable;
  // Independent binary80 encoding of one.
  Data.Data = {0, 0, 0, 0, 0, 0, 0, 0x80, 0xff, 0x3f};
  Data.Size = Data.FileSz = Data.Data.size();
  Img.Segments.push_back(Data);
  for (bool Strict : {false, true})
    for (unsigned Top = 0; Top < 8; ++Top) {
      NdOpEmulator E(Img);
      E.setStrictMode(Strict);
      ASSERT_TRUE(E.setX86LinearAddressBits(48));
      ASSERT_TRUE(E.setX87Context(incoming(Top, 0xff)));
      ASSERT_TRUE(E.step(resetStep(0x0e0f)));
      EXPECT_FALSE(E.step(resetStep(0xe5d9))); // FXAM needs retained ST0 bits.
      // The existing explicit portable load needs no physical FEMMS host.
      E.setX87PortableMode(true);
      auto Load = resetStep(0xdb | (0x28U << 8) | X87ContextMemory |
                            (0x28U << X87ContextOriginalModRMShift));
      Load.Inputs[3] = NdVar::cst(Data.VA, 8);
      for (unsigned I = 0; I < 8; ++I) {
        ASSERT_TRUE(E.step(Load)); // All eight original tags must be empty.
        const unsigned Physical = (Top + 7 - I) & 7;
        EXPECT_EQ(E.getRegisterBytes(x86reg::stReg(Physical)), Data.Data);
        EXPECT_EQ(bool(E.getX87Context()), I == 7);
      }
      ASSERT_TRUE(E.getX87Context());
      EXPECT_EQ((*E.getX87Context())[4], 0xff);
      EXPECT_EQ((*E.getRegister(x86reg::FPU_SW) >> 11) & 7, Top);
    }
}

TEST(X87MMXState, NativeEvaluatorChecksFeatureBeforeExecutingFemms) {
  auto State = incoming(0, 0xff);
  const auto Before = State;
  uint64_t Result = 0x1234;
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  unsigned A, B, C, D;
  const bool Supported =
      __get_cpuid(0x80000001, &A, &B, &C, &D) && (D & (1U << 31));
#else
  const bool Supported = false;
#endif
  EXPECT_EQ(executeNativeX87Context(0x0e0f, State, nullptr, 0, Result),
            Supported);
  if (!Supported) {
    EXPECT_EQ(State, Before);
    EXPECT_EQ(Result, 0x1234);
  } else {
    EXPECT_EQ(State[4], 0);
    // The hardware-selected payloads are deliberately not compared.
    EXPECT_TRUE(std::equal(State.begin(), State.begin() + 4, Before.begin()));
  }
}

class X87MMXEmissionTest : public X86FPStateFixture {
protected:
  X87MMXEmissionTest() { X87ContextOverride = true; }

  void checkObject(llvm::StringRef Bytes) {
    auto Object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Bytes, "femms.o"));
    ASSERT_TRUE(bool(Object)) << llvm::toString(Object.takeError());
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    unsigned Count = 0;
    for (const auto &Section : (*Object)->sections()) {
      if (!Section.isText())
        continue;
      auto Text = Section.getContents();
      ASSERT_TRUE(bool(Text)) << llvm::toString(Text.takeError());
      for (size_t Offset = 0; Offset < Text->size();) {
        DecodedInsn Instruction{};
        const auto Size = D.decodeOneForLift(
            reinterpret_cast<const uint8_t *>(Text->data() + Offset),
            Text->size() - Offset, Entry + Offset, Instruction);
        ASSERT_NE(Size, 0U);
        Count += Instruction.Id == X86_INS_FEMMS;
        Offset += Size;
      }
    }
    EXPECT_EQ(Count, 1U);
  }
};

TEST_F(X87MMXEmissionTest, FemmsSurvivesLLVMHighCAndLLVMCOptimization) {
  const uint8_t Base = memoryModRM();
  const std::vector<uint8_t> Bytes = {0x0f, 0x6f,
                                      Base,       // MOVQ MM0, [argument]
                                      0x0f, 0x0e, // FEMMS
                                      0xd9, 0xe8, // FLD1 uses the empty stack.
                                      0xdd, uint8_t(0x58 | Base),
                                      8, // FSTP [argument + 8]
                                      0x31, 0xc0,
                                      0xc3}; // Defined GPR return.
  for (bool NoOpt : {false, true})
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Route);
      SCOPED_TRACE(NoOpt);
      auto Img = image(Bytes);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = Route != 1;
      Options.SourceProjection = Route == 2;
      Options.NoOpt = NoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry};
      auto Lifted = Pipeline().run(Img, Context, Options);
      ASSERT_TRUE(Lifted.Success) << Lifted.Error;
      if (Route == 0) {
        ASSERT_NE(Lifted.LlvmModule, nullptr);
        ASSERT_FALSE(llvm::verifyModule(*Lifted.LlvmModule, &llvm::errs()));
        auto Object =
            Codegen().compile(*Lifted.LlvmModule, Arch::X64, hostFormat());
        ASSERT_TRUE(Object.Success);
        const llvm::StringRef Data(
            reinterpret_cast<const char *>(Object.ObjectData.data()),
            Object.ObjectData.size());
        checkObject(Data);
        retain(NoOpt ? "llvm-noopt.o" : "llvm-default.o", Data);
        continue;
      }
      CEmitterOptions Emit;
      Emit.TheArch = Arch::X64;
      Emit.Format = hostFormat();
      Emit.Image = &Img;
      Emit.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      ASSERT_TRUE(Route == 1 ? HighCEmitter().emit(Lifted.HighFuncs, OS, Emit)
                             : LLVMCEmitter().emit(*Lifted.LlvmModule, OS, Emit,
                                                   nullptr, &Img));
      const auto Stem = std::string(Route == 1 ? "high" : "llvm-c") +
                        (NoOpt ? "-noopt" : "-default");
      const auto Unit = file(Stem + ".c");
      write(Unit, Source);
      retain(Stem + ".c", Source);
      for (const char *Opt : {"-O0", "-O2"}) {
        const auto Output = file(Stem + Opt + ".o");
        const auto Built = command(Compiler, {Opt, "-c", Unit, "-o", Output});
        ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
        checkObject(contents(Output));
        retainFile(Stem + Opt + ".o", Output);
      }
    }
}
} // namespace
