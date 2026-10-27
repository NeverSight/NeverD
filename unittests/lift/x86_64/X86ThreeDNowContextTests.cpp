//===- X86ThreeDNowContextTests.cpp - Owned packed MMX state -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Byte/detail admission, conservative concrete state and original instruction
/// retention through all three native source projections.
//===----------------------------------------------------------------------===//

#include "../x86/X86LegacyAdmissionFixtures.h"
#include "X86FPStateAccuracyFixture.h"

#include "neverd/backend/llvm/X86/LLVMX87ContextAsm.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/low/X86/X87NativeContext.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/Object/ObjectFile.h"

#include <functional>

#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
#include <cpuid.h>
#endif

namespace {
LowOp packedStep(uint8_t Opcode, uint8_t ModRM, bool Memory = false) {
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.addInput(NdVar::cst(unsigned(Intrinsic::X87ContextStep), 2));
  Op.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
  Op.addInput(
      NdVar::cst(X87ContextThreeDNow | 0x0f | (unsigned(ModRM) << 8) |
                     (unsigned(Opcode) << X87ContextThreeDNowOpcodeShift) |
                     (Memory ? X87ContextMemory : 0),
                 4));
  Op.addInput(NdVar::cst(Memory ? 0x3000 : 0, 8));
  Op.addInput(NdVar::cst(0, 8));
  Op.addInput(NdVar::cst(Entry, 8));
  return Op;
}

TEST(X86ThreeDNowContext, AllRegistersIgnoredPrefixesAndDecodeRoutes) {
  const std::vector<std::vector<uint8_t>> Prefixes = {
      {},           {0x66},       {0xf2},       {0xf3},
      {0x67},       {0x64},       {0x65},       {0x4f},
      {0x4f, 0x66}, {0x66, 0x4f}, {0x4f, 0x40}, {0xf2, 0x66, 0xf3}};
  unsigned Cases = 0;
  for (const auto &E : test::X86ThreeDNowEncodings)
    for (unsigned Destination = 0; Destination < 8; ++Destination)
      for (unsigned Source = 0; Source < 8; ++Source)
        for (const auto &Prefix : Prefixes)
          for (bool Strict : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              Decoder D;
              ASSERT_TRUE(D.init(Arch::X64));
              D.setStrict(Strict);
              D.setX87ContextOwnership(true);
              const uint8_t ModRM = 0xc0 | (Destination << 3) | Source;
              auto Bytes = Prefix;
              Bytes.insert(Bytes.end(), {0x0f, 0x0f, ModRM, E.Opcode});
              SCOPED_TRACE(testing::PrintToString(Bytes));
              DecodedInsn I{};
              const int Size =
                  Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, I)
                  : Route == 1
                      ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I)
                      : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry,
                                           I);
              ASSERT_EQ(Size, Bytes.size());
              ASSERT_EQ(I.Id, E.Id);
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(D.liftToLow(I, Ops));
              ASSERT_EQ(Ops.size(), 1u);
              EXPECT_TRUE(x87ContextLowShapeIsValid(Ops[0], Arch::X64));
              EXPECT_EQ(Ops[0].Inputs[2],
                        packedStep(E.Opcode, ModRM).Inputs[2]);
              EXPECT_FALSE(
                  x87ContextUpdatesInstructionPointer(Ops[0].Inputs[2].Offset));
              ++Cases;
            }
  EXPECT_EQ(Cases, 110592u);
  RecordProperty("register_admission_cases", Cases);
}

TEST(X86ThreeDNowContext, MemoryAddressWidthSegmentAndStackReference) {
  unsigned Cases = 0;
  for (const auto &E : test::X86ThreeDNowEncodings)
    for (unsigned Destination = 0; Destination < 8; ++Destination)
      for (bool Address32 : {false, true})
        for (uint8_t Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
          for (uint8_t Rex : {0, 0x43, 0x4f})
            for (unsigned Form = 0; Form < 4; ++Form) {
              std::vector<uint8_t> Bytes;
              if (Segment)
                Bytes.push_back(Segment);
              if (Address32)
                Bytes.push_back(0x67);
              if (Rex)
                Bytes.push_back(Rex);
              const uint8_t ModRM = (Destination << 3) | (Form == 0   ? 1
                                                          : Form == 1 ? 0x45
                                                                      : 0x44);
              Bytes.insert(Bytes.end(), {0x0f, 0x0f, ModRM});
              if (Form >= 2)
                Bytes.push_back(Form == 2 ? 0x84 : 0x45);
              if (Form)
                Bytes.push_back(8);
              Bytes.push_back(E.Opcode);
              SCOPED_TRACE(testing::PrintToString(Bytes));
              Decoder D;
              ASSERT_TRUE(D.init(Arch::X64));
              D.setStrict(true);
              D.setX87ContextOwnership(true);
              DecodedInsn I{};
              ASSERT_EQ(
                  D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I),
                  Bytes.size());
              std::vector<LowOp> Ops;
              ASSERT_NO_THROW(D.liftToLow(I, Ops));
              ASSERT_FALSE(Ops.empty());
              const auto &Step = Ops.back();
              ASSERT_TRUE(x87ContextLowShapeIsValid(Step, Arch::X64));
              const auto Selector = Step.Inputs[2].Offset;
              EXPECT_TRUE(Selector & X87ContextMemory);
              EXPECT_EQ(bool(Selector & X87ContextStackReference),
                        !Address32 && !(Rex & 1) && Form != 0 &&
                            Segment != 0x64 && Segment != 0x65);
              EXPECT_EQ(Step.MemoryAddressSpace,
                        Segment == 0x64   ? NdMemoryAddressSpace::X86FS
                        : Segment == 0x65 ? NdMemoryAddressSpace::X86GS
                                          : NdMemoryAddressSpace::Default);
              for (const auto &Op : Ops)
                EXPECT_NE(Op.Opcode, NdOp::LOAD);
              ++Cases;
            }
  EXPECT_EQ(Cases, 32256u);
  RecordProperty("memory_admission_cases", Cases);
}

TEST(X86ThreeDNowContext, ContradictoryDetailCannotPublishEffects) {
  using Mutation = std::function<void(cs_insn &)>;
  const std::vector<Mutation> Mutations = {
      [](auto &I) { I.bytes[I.size - 1] ^= 1; },
      [](auto &I) { I.bytes[0] = 0xf0; },
      [](auto &I) { I.id = X86_INS_PFMIN; },
      [](auto &I) { I.detail->x86.opcode[1] ^= 1; },
      [](auto &I) { I.detail->x86.opcode[2] = 1; },
      [](auto &I) { I.detail->x86.modrm ^= 8; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset++; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.addr_size = 4; },
      [](auto &I) { I.detail->x86.rex = 0x48; },
      [](auto &I) { I.detail->x86.eflags = 1; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_MM7; },
      [](auto &I) { I.detail->x86.operands[0].size = 4; },
      [](auto &I) { I.detail->x86.operands[1].size = 4; },
      [](auto &I) { I.detail->x86.op_count = 1; }};
  csh Handle;
  ASSERT_EQ(cs_open(CS_ARCH_X86, CS_MODE_64, &Handle), CS_ERR_OK);
  ASSERT_EQ(cs_option(Handle, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
  for (uint8_t ModRM : {0xc1, 0x01})
    for (const auto &Mutate : Mutations)
      for (bool Strict : {false, true}) {
        const uint8_t Bytes[] = {0x0f, 0x0f, ModRM, 0x9e};
        cs_insn *I = nullptr;
        ASSERT_EQ(cs_disasm(Handle, Bytes, sizeof(Bytes), Entry, 1, &I), 1u);
        Mutate(*I);
        X86Lifter L(Arch::X64);
        L.setStrict(Strict);
        L.setX87ContextOwnership(true);
        std::vector<LowOp> Ops;
        EXPECT_THROW(L.lift(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        cs_free(I, 1);
      }
  cs_close(&Handle);
}

TEST(X86ThreeDNowContext,
     StrictPortableUnknownAndPendingStateRefuseWithoutMutation) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::array<uint8_t, 512> State{};
  State[0] = 0x7f;
  State[1] = 3;
  State[4] = 0xff;
  for (const auto &E : test::X86ThreeDNowEncodings)
    for (bool Memory : {false, true})
      for (unsigned Mode = 0; Mode < 4; ++Mode) {
        const auto Step = packedStep(E.Opcode, Memory ? 0 : 0xc1, Memory);
        NdOpEmulator Emu(Img);
        Emu.setStrictMode(Mode == 0 || Mode == 2);
        Emu.setX87NativeReferenceMode(Mode >= 2);
        Emu.setX87PortableMode(Mode == 3);
        EXPECT_FALSE(Emu.step(Step));
        ASSERT_TRUE(Emu.setX87Context(State));
        EXPECT_FALSE(Emu.step(Step));
        EXPECT_EQ(Emu.getX87Context(), State);
        auto Pending = State;
        Pending[0] &= ~1U;
        Pending[2] = 0x81;
        ASSERT_TRUE(Emu.setX87Context(Pending));
        Emu.setStrictMode(false);
        Emu.setX87NativeReferenceMode(true);
        EXPECT_FALSE(Emu.step(Step));
        EXPECT_EQ(Emu.getX87Context(), Pending);
      }
}

bool nativeFeature(uint8_t Opcode) {
#if (defined(__x86_64__) || defined(_M_X64)) &&                                \
    (defined(__clang__) || defined(__GNUC__))
  unsigned A, B, C, D;
  if (!__get_cpuid(0x80000001, &A, &B, &C, &D) || !(D & (1U << 31)))
    return false;
  const bool Enhanced = Opcode == 0x1c || Opcode == 0x8a || Opcode == 0x8e ||
                        Opcode == 0x0c || Opcode == 0xbb;
  return !Enhanced || (D & (1U << 30));
#else
  return false;
#endif
}

TEST(X86ThreeDNowContext, NativeFeatureChecksPrecedeStateOrOutputMutation) {
  std::array<uint8_t, 512> Before{};
  Before[0] = 0x7f;
  Before[1] = 3;
  Before[4] = 0xff;
  const uint64_t Memory = UINT64_C(0x400000003f800000);
  for (unsigned I = 0; I < 8; ++I) {
    std::memcpy(Before.data() + 32 + I * 16, &Memory, 8);
    Before[40 + I * 16] = Before[41 + I * 16] = 0xff;
  }
  for (const auto &E : test::X86ThreeDNowEncodings)
    for (bool Mem : {false, true}) {
      auto State = Before;
      uint64_t Result = 0x1234;
      const auto Op = packedStep(E.Opcode, Mem ? 0 : 0xc1, Mem);
      const bool Available = nativeFeature(E.Opcode);
      EXPECT_EQ(executeNativeX87Context(
                    Op.Inputs[2].Offset, State,
                    Mem ? const_cast<uint64_t *>(&Memory) : nullptr, 0, Result),
                Available);
      if (!Available) {
        EXPECT_EQ(State, Before);
        EXPECT_EQ(Result, 0x1234);
      } else {
        EXPECT_EQ(State[0], Before[0]);
        EXPECT_EQ(State[1], Before[1]);
        EXPECT_EQ(State[4], 0xff);
      }
    }
}

TEST(X86ThreeDNowContext, FemmsUnknownPayloadsRequireOnlyActualSourceSlots) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  std::array<uint8_t, 512> State{};
  State[0] = 0x7f;
  State[1] = 3;
  for (const auto &E : test::X86ThreeDNowEncodings) {
    SCOPED_TRACE(E.Opcode);
    NdOpEmulator Emu(Img);
    Emu.setStrictMode(false);
    Emu.setX87NativeReferenceMode(true);
    ASSERT_TRUE(Emu.setX87Context(State));
    auto Reset = packedStep(E.Opcode, 0xc1);
    Reset.Inputs[2] = NdVar::cst(0x0e0f, 4);
    ASSERT_TRUE(Emu.step(Reset));
    LowOp Enter;
    Enter.Opcode = NdOp::INTRINSIC;
    Enter.addInput(NdVar::cst(unsigned(Intrinsic::X87ContextMMXEnter), 2));
    Enter.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
    ASSERT_TRUE(Emu.step(Enter));
    LowOp Write = Enter;
    Write.Inputs[0] = NdVar::cst(unsigned(Intrinsic::X87ContextMMXWrite), 2);
    Write.addInput(NdVar::cst(1, 4));
    Write.addInput(NdVar::cst(UINT64_C(0x400000003f800000), 8));
    ASSERT_TRUE(Emu.step(Write));
    const bool Unary = E.Opcode == 0x1d || E.Opcode == 0x1c ||
                       E.Opcode == 0x96 || E.Opcode == 0x97 ||
                       E.Opcode == 0x0d || E.Opcode == 0x0c || E.Opcode == 0xbb;
    const bool Executes = Unary && nativeFeature(E.Opcode);
    EXPECT_EQ(Emu.step(packedStep(E.Opcode, 0xc1)), Executes);
    EXPECT_EQ(bool(Emu.getRegisterBytes(x86reg::ST0)), Executes);
    EXPECT_TRUE(Emu.getRegisterBytes(x86reg::ST1));
    for (unsigned I = 2; I < 8; ++I)
      EXPECT_FALSE(Emu.getRegisterBytes(x86reg::stReg(I)));
    EXPECT_FALSE(Emu.getX87Context());
  }
}

TEST(X86ThreeDNowContext,
     SelectorsRejectReservedBitsAndContradictoryAddresses) {
  for (const auto &E : test::X86ThreeDNowEncodings) {
    auto Op = packedStep(E.Opcode, 0xc1);
    ASSERT_TRUE(x87ContextLowShapeIsValid(Op, Arch::X64));
    for (unsigned Bit : {17u, 18u, 28u, 29u, 30u, 31u}) {
      auto Bad = Op;
      Bad.Inputs[2].Offset |= UINT32_C(1) << Bit;
      EXPECT_FALSE(x87ContextLowShapeIsValid(Bad, Arch::X64));
    }
    Op = packedStep(E.Opcode, 0, true);
    Op.Inputs[2].Offset |= X87ContextStackReference;
    EXPECT_TRUE(x87ContextLowShapeIsValid(Op, Arch::X64));
    Op.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    EXPECT_FALSE(x87ContextLowShapeIsValid(Op, Arch::X64));
    Op.MemoryAddressSpace = NdMemoryAddressSpace::Default;
    Op.Inputs[2].Offset |= 0x100;
    EXPECT_FALSE(x87ContextLowShapeIsValid(Op, Arch::X64));
  }
}

class X86ThreeDNowEmissionTest : public X86FPStateFixture {
protected:
  X86ThreeDNowEmissionTest() { X87ContextOverride = true; }
  void checkObject(llvm::StringRef Bytes, unsigned Id) {
    auto Object = llvm::object::ObjectFile::createObjectFile(
        llvm::MemoryBufferRef(Bytes, "3dnow.o"));
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
        DecodedInsn I{};
        const auto Size = D.decodeOneForLift(
            reinterpret_cast<const uint8_t *>(Text->data() + Offset),
            Text->size() - Offset, Entry + Offset, I);
        ASSERT_NE(Size, 0u);
        Count += I.Id == Id;
        Offset += Size;
      }
    }
    EXPECT_EQ(Count, 1u);
  }
};

TEST_F(X86ThreeDNowEmissionTest,
       OriginalOperationsSurviveAllNativeProjections) {
  for (const auto &E : test::X86ThreeDNowEncodings)
    for (unsigned Form = 0; Form < 4; ++Form) {
      SCOPED_TRACE(E.Id);
      SCOPED_TRACE(Form);
      std::vector<uint8_t> Bytes;
      if (Form == 3)
        Bytes = {0x55, 0x48, 0x89,
                 uint8_t(hostFormat() == BinaryFormat::COFF ? 0xcd : 0xfd)};
      const uint8_t ModRM = Form == 0   ? 0xc1
                            : Form == 1 ? 0xdb
                            : Form == 2 ? uint8_t(0x28 | memoryModRM())
                                        : 0x7d;
      Bytes.insert(Bytes.end(), {0x0f, 0x0f, ModRM});
      if (Form == 3)
        Bytes.push_back(0);
      Bytes.push_back(E.Opcode);
      if (Form == 3)
        Bytes.push_back(0x5d);
      Bytes.insert(Bytes.end(), {0x31, 0xc0, 0xc3});
      llvm::json::Array Raw;
      for (auto Byte : Bytes)
        Raw.push_back(Byte);
      retain("probe.json",
             llvm::formatv(
                 "{0:2}",
                 llvm::json::Value(llvm::json::Object{
                     {"opcode", E.Opcode},
                     {"form", Form},
                     {"bytes", std::move(Raw)},
                     {"format",
                      hostFormat() == BinaryFormat::ELF ? "ELF" : "other"}}))
                 .str());
      for (bool NoOpt : {false, true})
        for (unsigned Route = 0; Route < 3; ++Route) {
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
            checkObject(Data, E.Id);
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
          ASSERT_TRUE(Route == 1
                          ? HighCEmitter().emit(Lifted.HighFuncs, OS, Emit)
                          : LLVMCEmitter().emit(*Lifted.LlvmModule, OS, Emit,
                                                nullptr, &Img));
          const auto Stem = std::string(Route == 1 ? "high" : "llvm-c") +
                            (NoOpt ? "-noopt" : "-default");
          if (Route == 1) {
            const auto Name =
                llvm::StringRef(x86ThreeDNowInstruction(E.Opcode)->Name)
                    .lower();
            EXPECT_NE(Source.find(Name + "_mm"), std::string::npos);
          }
          const auto Unit = file(Stem + ".c");
          write(Unit, Source);
          retain(Stem + ".c", Source);
          for (const char *Opt : {"-O0", "-O2"}) {
            const auto Output = file(Stem + Opt + ".o");
            const auto Built =
                command(Compiler, {Opt, "-c", Unit, "-o", Output});
            ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
            checkObject(contents(Output), E.Id);
            retainFile(Stem + Opt + ".o", Output);
          }
        }
      ++ProbeNumber;
    }
}
} // namespace
