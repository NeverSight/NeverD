//===- MIPSRecordEntryTests.cpp - C entry carrier reconstruction ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/backend/c/MIPS/CRecordEntry.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"

using namespace neverd;

namespace {
struct RecordFixture {
  BinaryImage Image;
  NullDebugContext Debug;
  CEmitterOptions Options;
  FunctionSym Function;
  std::vector<mips::CEntryCarrier> Inputs;
  mips::CEntryCarrier Result;

  RecordFixture(Arch Target, uint16_t Count)
      : Result{{kNoParamReg, -1, 8}, NdTypeKind::Int, "uint64_t"} {
    Image.Arch = Target;
    Image.Format = BinaryFormat::ELF;
    Image.MIPS.emplace();
    Options.Image = &Image;
    Options.TheArch = Target;
    Options.Format = BinaryFormat::ELF;
    Options.RecordDefinitions = [](const auto &) {
      CRecordDefinitions D;
      D.Tags.emplace("Payload", false);
      return D;
    };
    auto Record = NdType::makeStruct(
        {NdType::makeArray(NdType::makeInt(1, false), Count, Count)});
    Record->Passing = NdRecordPassing::ByValue;
    Record->SourceName = "Payload";
    Function.Addr = 0x1000;
    Function.PrototypeDeclaration = "struct Payload entry(struct Payload)";
    Function.Params.emplace_back("value", Record);
    Function.ReturnType = Record;
    const auto Layout =
        getTargetRegInfo(Target).integerArgumentLayout(Options.Format);
    const unsigned Hidden = isMIPSO32(Target) || Count > 16;
    for (unsigned I = 0;
         I < Hidden + (Count + Layout.SlotBytes - 1) / Layout.SlotBytes; ++I) {
      const auto Reg =
          I < Layout.Registers.size() ? Layout.Registers[I] : kNoParamReg;
      Inputs.push_back({{Reg, static_cast<int>(I), Layout.SlotBytes},
                        NdTypeKind::Int,
                        Layout.SlotBytes == 4 ? "uint32_t" : "uint64_t"});
    }
  }

  auto project() const {
    return mips::cRecordEntry(Options, &Debug, Function, Function.Addr, "entry",
                              Inputs, Result);
  }
};

TEST(MIPSRecordSourceEntry, PartialWordUsesTheSourceObjectRepresentation) {
  for (const auto A :
       {Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 3);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry);
    EXPECT_EQ(Entry->Declaration,
              "struct Payload entry(struct Payload __neverd_arg0)");
    // A short record occupies the lowest addressed bytes on both orders.
    // The significant bits of its carrier differ between them.
    EXPECT_NE(Entry->Definition.find(
                  "__builtin_memcpy((unsigned char *)&__neverd_word0, (const "
                  "unsigned char *)&__neverd_arg0, 3)"),
              std::string::npos);
    EXPECT_NE(Entry->Definition.find(
                  "__builtin_memcpy((unsigned char *)&__neverd_result, (const "
                  "unsigned char *)&__neverd_return_word, 3)"),
              std::string::npos);
    F.Inputs.front().Site.RegOff = getTargetRegInfo(A).IntReturnReg;
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry, HiddenStorageIsTakenFromTheEntryArgument) {
  for (const auto A :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64O32, Arch::MIPS64O32EL,
        Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL}) {
    RecordFixture F(A, 17);
    const auto Entry = F.project();
    ASSERT_TRUE(Entry) << static_cast<unsigned>(A);
    EXPECT_NE(Entry->Definition.find("(uintptr_t)&__neverd_result"),
              std::string::npos);
    EXPECT_EQ(Entry->Definition.find("__neverd_return_word"),
              std::string::npos);
    F.Inputs.erase(F.Inputs.begin());
    EXPECT_FALSE(F.project());
  }
}

TEST(MIPSRecordSourceEntry,
     IncompleteOrUnboundDeclarationsRetainMachineCarriers) {
  RecordFixture F(Arch::MIPS64N32EL, 3);
  F.Function.PrototypeDeclaration.clear();
  EXPECT_FALSE(F.project());
  F.Function.PrototypeDeclaration = "struct Payload entry(struct Payload)";
  F.Function.ReturnType->Passing = NdRecordPassing::Unknown;
  EXPECT_FALSE(F.project());
}
} // namespace
