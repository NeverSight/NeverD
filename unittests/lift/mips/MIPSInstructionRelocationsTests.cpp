//===- MIPSInstructionRelocationsTests.cpp - Split address provenance ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent linked ELF bytes exercise source operands and scalar controls.
//===----------------------------------------------------------------------===//

#include "MIPSFixtures.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/ELF/ELFLoader.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <string>

using namespace neverd;

namespace {

struct SplitAddressELF : mips_test::ELFBytes {
  bool Rela;
  va_t Base;
  va_t Target;
  size_t RelocationCount;
  size_t RelocationSize;
  static constexpr size_t Relocations = 0x8700;
  static constexpr size_t SectionHeaders = 0x8800;

  SplitAddressELF(Arch A, bool Explicit, int64_t Addend = 0, bool Micro = false,
                  bool High64 = false)
      : ELFBytes(isMIPSN32(A)
                     ? (archLittleEndian(A) ? Arch::MIPS32EL : Arch::MIPS32)
                     : A),
        Rela(Explicit), Base(High64 ? 0x120010000ull : 0x10000),
        Target(Base + 0x8100), RelocationCount(High64 ? 4 : 2),
        RelocationSize(Is64 ? (Rela ? 24 : 16) : (Rela ? 12 : 8)) {
    Bytes.resize(0x9000);
    const unsigned Width = Is64 ? 8 : 4;
    put(24, Base + 0x100, Width);
    put(Is64 ? 48 : 36,
        (isMIPSN32(A) ? 0x80000020u
         : Is64       ? 0x80000000u
                      : 0x70001000u) |
            (Micro ? 0x02000000u : 0),
        4);
    const size_t P = Is64 ? 64 : 52;
    put(P + (Is64 ? 16 : 8), Base, Width);
    put(P + (Is64 ? 24 : 12), Base, Width);
    put(P + (Is64 ? 32 : 16), Bytes.size(), Width);
    put(P + (Is64 ? 40 : 20), Bytes.size(), Width);
    put(Is64 ? 40 : 32, SectionHeaders, Width);
    put(Is64 ? 60 : 48, 6, 2);
    put(Is64 ? 62 : 50, 5, 2);
    const std::string Names("\0.text\0.symtab\0.strtab\0.rel.text\0.shstrtab\0",
                            43);
    std::copy(Names.begin(), Names.end(), Bytes.begin() + 0x8a00);
    const std::string Strings("\0pointer_bits\0target\0", 21);
    std::copy(Strings.begin(), Strings.end(), Bytes.begin() + 0x8600);
    const auto Section = [&](unsigned I, uint32_t Name, uint32_t Type,
                             uint64_t Flags, uint64_t Offset, uint64_t Size,
                             unsigned Link, unsigned Info, unsigned Entry) {
      const size_t S = SectionHeaders + I * (Is64 ? 64 : 40);
      put(S, Name, 4);
      put(S + 4, Type, 4);
      put(S + 8, Flags, Width);
      put(S + (Is64 ? 16 : 12), Flags ? Base + Offset : 0, Width);
      put(S + (Is64 ? 24 : 16), Offset, Width);
      put(S + (Is64 ? 32 : 20), Size, Width);
      put(S + (Is64 ? 40 : 24), Link, 4);
      put(S + (Is64 ? 44 : 28), Info, 4);
      put(S + (Is64 ? 48 : 32), 4, Width);
      put(S + (Is64 ? 56 : 36), Entry, Width);
    };
    Section(1, 1, 1, 6, 0x100, 0x8100, 0, 0, 0);
    Section(2, 7, 2, 0, 0x8500, 3 * (Is64 ? 24 : 16), 3, 1, Is64 ? 24 : 16);
    Section(3, 15, 3, 0, 0x8600, Strings.size(), 0, 0, 0);
    Section(4, 23, Rela ? 4 : 9, 0, Relocations,
            RelocationCount * RelocationSize, 2, 1, RelocationSize);
    Section(5, 33, 3, 0, 0x8a00, Names.size(), 0, 0, 0);
    const auto Symbol = [&](unsigned I, unsigned Name, va_t Address,
                            unsigned Size, uint8_t Other) {
      const size_t S = 0x8500 + I * (Is64 ? 24 : 16);
      put(S, Name, 4);
      put(S + (Is64 ? 8 : 4), Address, Width);
      put(S + (Is64 ? 16 : 8), Size, Width);
      put(S + (Is64 ? 4 : 12), 0x12, 1);
      put(S + (Is64 ? 5 : 13), Other, 1);
      put(S + (Is64 ? 6 : 14), 1, 2);
    };
    Symbol(1, 1, Base + 0x100, High64 ? 32 : 20, 0);
    Symbol(2, 14, Target, 8, Micro ? 0x80 : 0);
    const uint64_t Value = (Target | uint64_t(Micro)) + uint64_t(Addend);
    const uint16_t Lo = Value, Hi = (Value + 0x8000) >> 16;
    if (High64) {
      put(0x100, 0x3c010000u | uint16_t((Value + 0x800080008000ull) >> 48), 4);
      put(0x104, 0x64210000u | uint16_t((Value + 0x80008000ull) >> 32), 4);
      put(0x108, 0x00010c38, 4); // dsll at,at,16
      put(0x10c, 0x64210000u | Hi, 4);
      put(0x110, 0x00010c38, 4);
      put(0x114, 0x64220000u | Lo, 4);
      put(0x118, 0x03e00008, 4);
      put(0x11c, 0, 4);
      relocation(0, 0x100, 29, Addend); // HIGHEST
      relocation(1, 0x104, 28, Addend); // HIGHER
      relocation(2, 0x10c, 5, Addend);  // HI16
      relocation(3, 0x114, 6, Addend);  // LO16
    } else {
      // Clang O0 can schedule LO before HI in two different registers.
      put(0x100, 0x24020000u | Lo, 4);
      put(0x104, 0x3c010000u | Hi, 4);
      put(0x108, 0x00221021, 4); // addu v0,at,v0
      put(0x10c, 0x03e00008, 4);
      put(0x110, 0, 4);
      relocation(Rela ? 0 : 1, 0x100, 6, Addend);
      relocation(Rela ? 1 : 0, 0x104, 5, Addend);
    }
    put(0x8100, Micro ? 0x45bf : 0x03e00008, Micro ? 2 : 4);
  }

  void relocation(size_t Index, uint64_t Offset, uint8_t Type, int64_t Addend) {
    const size_t R = Relocations + Index * RelocationSize;
    put(R, Base + Offset, Is64 ? 8 : 4);
    if (Is64) {
      // n64 r_info has an endian symbol word followed by four byte fields.
      put(R + 8, 2, 4);
      put(R + 12, 0, 3);
      put(R + 15, Type, 1);
    } else
      put(R + 4, 0x200 | Type, 4);
    if (Rela)
      put(R + (Is64 ? 16 : 8), uint64_t(Addend), Is64 ? 8 : 4);
  }

  void relocationCount(unsigned Count) {
    put(SectionHeaders + 4 * (Is64 ? 64 : 40) + (Is64 ? 32 : 20),
        Count * RelocationSize, Is64 ? 8 : 4);
  }
};

class MIPSInstructionRelocations : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSInstructionRelocations, LinkedFieldsRetainModeAddendAndTableOrder) {
  for (bool Explicit : {false, true}) {
    if (!Explicit && isMIPS64(GetParam()) && !isMIPSN32(GetParam()))
      continue;
    for (int64_t Addend : {-16, 0, 32}) {
      for (bool Micro : {false, true}) {
        SplitAddressELF File(GetParam(), Explicit, Addend, Micro);
        auto Image = ELFLoader().loadBuffer(File.buffer());
        ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
        ASSERT_EQ(Image->MIPS->InstructionRelocations.size(), 2u);
        for (const auto &Field : Image->MIPS->InstructionRelocations) {
          ASSERT_TRUE(Field.Value);
          EXPECT_EQ(Field.Value->SymbolValue, File.Target | uint64_t(Micro));
          EXPECT_EQ(Field.Value->Addend, Addend);
        }
      }
    }
  }
}

void checkSource(SplitAddressELF &File, bool Symbolic, bool Success = true) {
  auto Image = ELFLoader().loadBuffer(File.buffer());
  ASSERT_TRUE(bool(Image)) << llvm::toString(Image.takeError());
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.OnlyFunctionEntries = {File.Base + 0x100};
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    auto Result = Pipeline().run(*Image, Context, Options);
    ASSERT_EQ(Result.Success, Success) << Result.Error;
    if (!Success) {
      EXPECT_NE(Result.Error.find("MIPS symbolic"), std::string::npos);
      continue;
    }
    EXPECT_EQ(Result.MedIRVerifierFailures, 0u);
    // Source relocation expressions must not replace physical machine effects.
    for (const auto &Function : Result.LowFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            EXPECT_NE(Op.Inputs[I].Provenance,
                      ConstantAddressProvenance::CodeAddress);
    bool SawAddress = false;
    for (const auto &Function : Result.MedFuncs)
      for (const auto &Block : Function.Blocks)
        for (const auto &Op : Block.Ops)
          for (unsigned I = 0; I < Op.NumInputs; ++I)
            if (Op.Inputs[I].isConst() &&
                Op.Inputs[I].Provenance ==
                    ConstantAddressProvenance::CodeAddress) {
              SawAddress = true;
              // Addends and HI carries observe bits of this same symbol;
              // neither may become a new unrelated interior code identity.
              EXPECT_EQ(Op.Inputs[I].ConstVal, File.Target);
            }
    EXPECT_EQ(SawAddress, Symbolic);
    CEmitterOptions Emit;
    Emit.TheArch = Image->Arch;
    Emit.Format = BinaryFormat::ELF;
    Emit.Image = &*Image;
    std::string Text;
    llvm::raw_string_ostream Out(Text);
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Emit));
    } else
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Emit));
    EXPECT_EQ(Text.find("target") != std::string::npos, Symbolic) << Text;
  }
}

TEST_P(MIPSInstructionRelocations, BothSourcesRetainSplitSymbolOperands) {
  for (int64_t Addend : {-16, 0, 4, 32}) {
    SCOPED_TRACE(Addend);
    SplitAddressELF File(GetParam(), true, Addend);
    checkSource(File, true);
  }
}

TEST_P(MIPSInstructionRelocations, EqualUnrelocatedLiteralsStayScalar) {
  SplitAddressELF File(GetParam(), true);
  File.relocationCount(0);
  checkSource(File, false);
}

TEST_P(MIPSInstructionRelocations, ConflictingAndMismatchedClaimsRefuseSource) {
  SplitAddressELF Conflict(GetParam(), true);
  Conflict.relocation(2, 0x104, 5, 0);
  Conflict.relocationCount(3);
  checkSource(Conflict, false, false);
  SplitAddressELF Mismatch(GetParam(), true);
  Mismatch.put(0x104, 0x3c010003, 4);
  checkSource(Mismatch, false, false);
  SplitAddressELF Undefined(GetParam(), true);
  Undefined.put(0x8500 + 2 * (Undefined.Is64 ? 24 : 16) +
                    (Undefined.Is64 ? 6 : 14),
                0, 2);
  checkSource(Undefined, false, false);
}

TEST(MIPSSplitAddressFields, High64FieldsUseIndependentCarries) {
  for (Arch A : {Arch::MIPS64, Arch::MIPS64EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    SplitAddressELF File(A, true, 0, false, true);
    checkSource(File, true);
  }
}

TEST(MIPSSplitAddressFields, UnpairedImplicitHighRefusesSource) {
  for (Arch A :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64N32, Arch::MIPS64N32EL}) {
    SCOPED_TRACE(static_cast<unsigned>(A));
    SplitAddressELF File(A, false);
    File.relocationCount(1);
    checkSource(File, false, false);
  }
}

INSTANTIATE_TEST_SUITE_P(ByArchitecture, MIPSInstructionRelocations,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL,
                                           Arch::MIPS64N32, Arch::MIPS64N32EL));

} // namespace
