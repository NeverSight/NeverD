//===- SourceCallABITests.cpp - Source call carrier contracts -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks platform placement and refuses incomplete call-state evidence.

#include "gtest/gtest.h"

#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallABI.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/med/MedIR.h"

#include <vector>
using namespace neverd;
namespace {
#define SOURCE_CALL_ABI_VALUE(Type, Name, Value) constexpr Type Name = Value;
#include "SourceCallABIFixtures.def"

SourceCallTypeHint contract(std::vector<TypeRef> Types, int Fixed = -1) {
  SourceCallTypeHint H;
  H.CallKind = SourceCallTypeHint::Kind::Analyst;
  H.AnalystSite = SourceCallOccurrenceKey{0x1000, 3, NdOp::CALL, 0x2000};
  H.AnalystFixedCount = Fixed;
  H.Signature.ReturnType = NdType::makeInt(8);
  for (const auto &T : Types)
    H.Signature.Parameters.push_back({"", T});
  return H;
}
TEST(SourceCallABI, PlacesFixedScalarArgumentsByPlatform) {
  const auto Int = NdType::makeInt(4), Double = NdType::makeFloat(8),
             Pointer = NdType::makePtr(NdType::makeVoid());
  std::string Error;
  for (auto A : {Arch::X64, Arch::AArch64})
    for (auto F :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      auto H = contract({Int, Double, Pointer});
      ASSERT_TRUE(assignAnalystCallABI(H, A, F, Error)) << Error;
      EXPECT_EQ(H.Signature.Parameters[1].Location.Kind,
                SourceABICarrierKind::FloatingRegister);
      EXPECT_EQ(H.Signature.Parameters[0].Location.ValueBytes, 4);
    }
  auto Windows = contract({Int, Double, Pointer, Int, Pointer});
  ASSERT_TRUE(
      assignAnalystCallABI(Windows, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  EXPECT_EQ(Windows.Signature.Parameters[4].Location.EntryStackOffset, 40);
  EXPECT_EQ(
      Windows.Signature.Parameters[1].Location.RegisterOffset,
      getTargetRegInfo(Arch::X64).floatingParamRegs(BinaryFormat::COFF)[1]);
}
TEST(SourceCallABI, DarwinWidePrototypeKeepsBothWordsAndOddRegisters) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (bool Signed : {false, true})
    for (unsigned Prefix = 0; Prefix <= kDarwinRegisterArguments; ++Prefix) {
      std::vector<TypeRef> Types(Prefix, NdType::makeInt(kDarwinRegisterBytes));
      Types.push_back(NdType::makeInt(kDarwinWideBytes, Signed));
      auto Hint = contract(Types);
      Hint.Signature.ReturnType = Types.back();
      std::string Error;
      ASSERT_TRUE(
          assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::MachO, Error))
          << Error;
      const auto &Parts = Hint.Signature.Parameters.back().Components;
      ASSERT_EQ(Parts.size(), kDarwinPairWords);
      ASSERT_EQ(Hint.Signature.ReturnComponents.size(), kDarwinPairWords);
      for (unsigned I = 0; I < kDarwinPairWords; ++I) {
        EXPECT_EQ(Parts[I].ValueBytes, kDarwinRegisterBytes);
        if (Prefix + kDarwinPairWords <= kDarwinRegisterArguments) {
          EXPECT_EQ(Parts[I].Kind, SourceABICarrierKind::IntegerRegister);
          EXPECT_EQ(Parts[I].RegisterOffset, TRI.IntParamRegs[Prefix + I]);
        } else {
          EXPECT_EQ(Parts[I].Kind, SourceABICarrierKind::Stack);
          EXPECT_EQ(Parts[I].EntryStackOffset, I * kDarwinRegisterBytes);
        }
      }
      EXPECT_EQ(sourceABIParameters(Hint.Signature).size(),
                Prefix + kDarwinPairWords);
      auto Missing = Hint.Signature;
      Missing.Parameters.back().Components.pop_back();
      EXPECT_FALSE(validateSourceABI(Missing, Error));
    }
}

TEST(SourceCallABI, DarwinRecordPrototypeKeepsHFAAndWholeStackSpills) {
  const auto Word = NdType::makeInt(kDarwinRegisterBytes);
  const auto Pair = NdType::makeStruct({Word, Word});
  const auto Float = NdType::makeFloat(kDarwinFloatBytes);
  const auto HFA =
      NdType::makeStruct(std::vector<TypeRef>(kDarwinHFAMembers, Float));
  for (const auto Record : {Pair, HFA})
    for (unsigned Prefix = 0; Prefix <= kDarwinRegisterArguments; ++Prefix) {
      const bool Floating = Record == HFA;
      const unsigned Members = Floating ? kDarwinHFAMembers : kDarwinPairWords;
      std::vector<TypeRef> Types(Prefix, Floating ? Float : Word);
      Types.push_back(Record);
      Types.push_back(Floating ? Float : Word);
      auto Hint = contract(Types);
      Hint.Signature.ReturnType = Record;
      std::string Error;
      ASSERT_TRUE(
          assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::MachO, Error))
          << Error;
      const auto &Parts = Hint.Signature.Parameters[Prefix].Components;
      ASSERT_EQ(Parts.size(), Members);
      ASSERT_EQ(Hint.Signature.ReturnComponents.size(), Members);
      if (Prefix + Members > kDarwinRegisterArguments) {
        for (unsigned I = 0; I < Members; ++I) {
          EXPECT_EQ(Parts[I].Kind, SourceABICarrierKind::Stack);
          EXPECT_EQ(Parts[I].EntryStackOffset,
                    I * (Floating ? kDarwinFloatBytes : kDarwinRegisterBytes));
        }
        EXPECT_EQ(Hint.Signature.Parameters.back().Location.Kind,
                  SourceABICarrierKind::Stack);
        EXPECT_EQ(Hint.Signature.Parameters.back().Location.EntryStackOffset,
                  Record->Size);
      }
    }
}
TEST(SourceCallABI, WindowsARM64FixedScalarsKeepSeparateBanksAndStackSlots) {
  std::vector<TypeRef> Types;
  for (unsigned I = 0; I < 9; ++I) {
    Types.push_back(NdType::makeInt(4));
    Types.push_back(NdType::makeFloat(8));
  }
  auto Hint = contract(Types);
  Hint.Signature.ReturnType = NdType::makeFloat(8);
  std::string Error;
  ASSERT_TRUE(
      assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::COFF, Error))
      << Error;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned I = 0; I < 8; ++I) {
    const auto &Integer = Hint.Signature.Parameters[I * 2].Location;
    const auto &Floating = Hint.Signature.Parameters[I * 2 + 1].Location;
    EXPECT_EQ(Integer.Kind, SourceABICarrierKind::IntegerRegister);
    EXPECT_EQ(Integer.RegisterOffset, TRI.IntParamRegs[I]);
    EXPECT_EQ(Integer.ValueBytes, 4);
    EXPECT_EQ(Floating.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Floating.RegisterOffset, TRI.FPParamRegs[I]);
    EXPECT_EQ(Floating.ValueBytes, 8);
  }
  for (unsigned I = 0; I < 2; ++I) {
    const auto &Stack = Hint.Signature.Parameters[16 + I].Location;
    EXPECT_EQ(Stack.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(Stack.EntryStackOffset, I * 8);
    EXPECT_EQ(Stack.ValueBytes, I ? 8 : 4);
  }
  EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint.Signature.ReturnLocation.RegisterOffset, TRI.FPReturnReg);
  EXPECT_EQ(Hint.Signature.ReturnLocation.ValueBytes, 8);
}
TEST(SourceCallABI, DarwinARM64FixedSpillsKeepNaturalSizeAndAlignment) {
  struct Spill {
    bool Floating;
    unsigned Bytes;
    int64_t Offset;
  };
  const Spill Spills[] = {
#define SOURCE_CALL_ABI_SPILL(Floating, Bytes, Offset)                         \
  {Floating, Bytes, Offset},
#include "SourceCallABIFixtures.def"
  };
  std::vector<TypeRef> Types;
  for (unsigned I = 0; I < kDarwinRegisterArguments; ++I) {
    Types.push_back(NdType::makeInt(kDarwinRegisterBytes));
    Types.push_back(NdType::makeFloat(kDarwinRegisterBytes));
  }
  const unsigned RegisterCount = Types.size();
  for (const auto &Spill : Spills)
    Types.push_back(Spill.Floating ? NdType::makeFloat(Spill.Bytes)
                                   : NdType::makeInt(Spill.Bytes));
  for (const auto Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    auto Hint = contract(Types);
    std::string Diagnostic;
    ASSERT_TRUE(assignAnalystCallABI(Hint, Arch::AArch64, Format, Diagnostic))
        << Diagnostic;
    for (unsigned I = 0; I < std::size(Spills); ++I) {
      const auto &Location =
          Hint.Signature.Parameters[RegisterCount + I].Location;
      EXPECT_EQ(Location.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(Location.EntryStackOffset, Format == BinaryFormat::MachO
                                               ? Spills[I].Offset
                                               : I * kDarwinRegisterBytes);
      EXPECT_EQ(Location.ValueBytes, Spills[I].Bytes);
    }
  }
}

TEST(SourceCallABI, DarwinFixedAndNativeScalarContractsAgree) {
  const unsigned Widths[] = {
#define SOURCE_CALL_ABI_NARROW(Bytes) Bytes,
#include "SourceCallABIFixtures.def"
  };
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (const unsigned Bytes : Widths)
      for (const bool Signed : {false, true}) {
        const auto Narrow = NdType::makeInt(Bytes, Signed);
        auto Hint = contract(std::vector<TypeRef>(
            kDarwinRegisterArguments + kDarwinExtraArguments, Narrow));
        Hint.Signature.ReturnType = Narrow;
        auto Native = Hint.Signature;
        std::string Error;
        ASSERT_TRUE(assignAnalystCallABI(Hint, Architecture,
                                         BinaryFormat::MachO, Error))
            << Error;
        ASSERT_TRUE(assignDarwinScalarSourceABI(Native, Architecture, Error))
            << Error;
        // The origins differ; compare the complete assigned ABI itself.
        Native.Origin = Hint.Signature.Origin;
        EXPECT_TRUE(equalSourceABIs(Hint.Signature, Native));
        for (const auto &Parameter : Hint.Signature.Parameters)
          EXPECT_EQ(Parameter.Location.ExtendTo32Bits,
                    Parameter.Location.Kind ==
                        SourceABICarrierKind::IntegerRegister);
        EXPECT_EQ(Hint.Signature.ReturnLocation.ExtendTo32Bits,
                  Architecture == Arch::AArch64);
      }
}

TEST(SourceCallABI, WindowsARM64VariadicsUseOneIntegerBankForAllScalars) {
  const std::vector<TypeRef> Types{NdType::makeInt(4),   NdType::makeFloat(4),
                                   NdType::makeFloat(8), NdType::makeInt(8),
                                   NdType::makeFloat(8), NdType::makeInt(8),
                                   NdType::makeFloat(8), NdType::makeInt(8),
                                   NdType::makeFloat(8), NdType::makeInt(8)};
  auto Hint = contract(Types, 2);
  Hint.Signature.ReturnType = NdType::makeFloat(8);
  std::string Error;
  ASSERT_TRUE(
      assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::COFF, Error))
      << Error;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned I = 0; I < Types.size(); ++I) {
    const auto &Location = Hint.Signature.Parameters[I].Location;
    EXPECT_EQ(Location.ValueBytes, Types[I]->Size);
    if (I < TRI.IntParamRegs.size()) {
      EXPECT_EQ(Location.Kind, SourceABICarrierKind::IntegerRegister);
      EXPECT_EQ(Location.RegisterOffset, TRI.IntParamRegs[I]);
    } else {
      EXPECT_EQ(Location.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(Location.EntryStackOffset, (I - TRI.IntParamRegs.size()) * 8);
    }
  }
  EXPECT_EQ(Hint.Signature.ReturnLocation.Kind,
            SourceABICarrierKind::FloatingRegister);
  EXPECT_EQ(Hint.Signature.ReturnLocation.RegisterOffset, TRI.FPReturnReg);

  // An explicit floating bit carrier does not broaden inferred signatures or
  // authorize a floating result in an integer register.
  auto Invalid = Hint.Signature;
  Invalid.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  EXPECT_FALSE(validateSourceABI(Invalid, Error));
  Invalid = Hint.Signature;
  Invalid.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                            TRI.IntReturnReg, 0, 8};
  EXPECT_FALSE(validateSourceABI(Invalid, Error));
  Hint.Signature.Parameters[2].Type = NdType::makeFloat(4);
  EXPECT_FALSE(
      assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::COFF, Error));
  EXPECT_NE(Error.find("default promotions"), std::string::npos);

  for (auto Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    EXPECT_FALSE(placeSourceParameters(Arch::AArch64, Format,
                                       NdType::makeInt(8), Types, true));
  const auto Wide =
      placeSourceParameters(Arch::AArch64, BinaryFormat::COFF,
                            NdType::makeInt(8), {NdType::makeInt(16)}, true);
  ASSERT_TRUE(Wide);
  EXPECT_TRUE(Wide->Parameters.empty());
}
TEST(SourceCallABI,
     RequiresPromotedVariadicsAndExplicitFloatingDuplicateSupport) {
  std::string Error;
  auto H = contract({NdType::makePtr(NdType::makeInt(1)), NdType::makeInt(4),
                     NdType::makeInt(8)},
                    1);
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  H.Signature.Parameters[1].Type = NdType::makeInt(1);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  H.Signature.Parameters[1].Type = NdType::makeFloat(8);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  EXPECT_NE(Error.find("duplicate"), std::string::npos);
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::ELF, Error))
      << Error;
  EXPECT_TRUE(
      assignAnalystCallABI(H, Arch::AArch64, BinaryFormat::MachO, Error))
      << Error;
  EXPECT_EQ(H.Signature.Parameters[1].Location.Kind,
            SourceABICarrierKind::Stack);
}
TEST(SourceCallABI, NamedRecordPointersDoNotClaimRecordValueTransport) {
  auto Record = NdType::makeNamedRecord("DEVICE_OBJECT", 0);
  auto Pointer = NdType::makePtr(Record);
  auto H = contract({Pointer});
  H.Signature.ReturnType = Pointer;
  std::string Error;
  EXPECT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error))
      << Error;
  H.Signature.Parameters[0].Type = Record;
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
}
LowOp op(NdOp Code, NdVar Output, std::initializer_list<NdVar> Inputs) {
  LowOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (auto V : Inputs)
    O.addInput(V);
  return O;
}
TEST(SourceCallABI, CompositeStackArgumentsRequireEveryDefinedMemberByte) {
  const auto Word = NdType::makeInt(kDarwinRegisterBytes);
  const auto Float = NdType::makeFloat(kDarwinFloatBytes);
  const auto Wide = NdType::makeInt(kDarwinWideBytes);
  const auto HFA =
      NdType::makeStruct(std::vector<TypeRef>(kDarwinHFAMembers, Float));
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (const auto Type : {Wide, HFA}) {
    std::vector<TypeRef> Types(kDarwinRegisterArguments,
                               Type == Wide ? Word : Float);
    Types.push_back(Type);
    auto Hint = contract(Types);
    std::string Error;
    ASSERT_TRUE(
        assignAnalystCallABI(Hint, Arch::AArch64, BinaryFormat::MachO, Error))
        << Error;
    const auto SP = NdVar::reg(TRI.StackPointer, kDarwinRegisterBytes);
    const auto Address =
        NdVar::tmp(kDarwinRegisterArguments, kDarwinRegisterBytes);
    LowFunc Function;
    LowBlock Block;
    Block.Ops = {
        op(NdOp::STORE, {}, {SP, NdVar::cst(0, kDarwinRegisterBytes)}),
        op(NdOp::INT_ADD, Address,
           {SP, NdVar::cst(kDarwinRegisterBytes, kDarwinRegisterBytes)}),
        op(NdOp::STORE, {}, {Address, NdVar::cst(0, kDarwinRegisterBytes)}),
        op(NdOp::CALL, {},
           {NdVar::cst(*Hint.AnalystSite->StaticTarget,
                       kDarwinRegisterBytes)})};
    Block.Ops.back().Addr = Hint.AnalystSite->Instruction;
    Block.Ops.back().Seq = Hint.AnalystSite->Sequence;
    Function.Blocks.push_back(std::move(Block));
    EXPECT_TRUE(sourceCallStackArgumentsDefined(Function, Hint, Arch::AArch64,
                                                0, Error))
        << Error;
    Function.Blocks.front().Ops.erase(Function.Blocks.front().Ops.begin() +
                                      kDarwinPairWords);
    EXPECT_FALSE(sourceCallStackArgumentsDefined(Function, Hint, Arch::AArch64,
                                                 0, Error));
    Hint.Signature.Parameters.back().Components.pop_back();
    EXPECT_FALSE(sourceCallStackArgumentsDefined(Function, Hint, Arch::AArch64,
                                                 0, Error));
  }
}

TEST(SourceCallABI, VariadicFloatingNeedsTheActualVectorCount) {
  auto H = contract({NdType::makeInt(4), NdType::makeFloat(8)}, 1);
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::ELF, Error));
  LowFunc F;
  LowBlock B;
  B.Id = 0;
  B.Ops = {op(NdOp::COPY, NdVar::tmp(1, 4), {NdVar::cst(1, 4)}),
           op(NdOp::INT_ZEXT, NdVar::reg(0, 8), {NdVar::tmp(1, 4)}),
           op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  B.Ops.back().Addr = 0x1000;
  B.Ops.back().Seq = 3;
  F.Blocks = {B};
  EXPECT_TRUE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error))
      << Error;
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(0, 4);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(9, 4);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  F.Blocks[0].Ops[0].Inputs[0] = NdVar::cst(1, 4);
  auto Earlier = op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)});
  Earlier.Addr = 0xff0;
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Earlier);
  EXPECT_FALSE(sourceCallVariadicStateDefined(F, H, BinaryFormat::ELF, Error));
  H.Signature.Parameters[0].Type = NdType::makeFloat(8);
  EXPECT_FALSE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
}
TEST(SourceCallABI, RequiresCompleteSameBlockOutgoingStackBytes) {
  auto H = contract(std::vector<TypeRef>(5, NdType::makeInt(8)));
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(H, Arch::X64, BinaryFormat::COFF, Error));
  const auto SP = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
  LowFunc F;
  LowBlock B;
  B.Id = 0;
  B.Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::cst(64, 8)}),
           op(NdOp::INT_ADD, NdVar::tmp(1, 8), {SP, NdVar::cst(32, 8)}),
           op(NdOp::STORE, {}, {NdVar::tmp(1, 8), NdVar::cst(17, 8)}),
           op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  B.Ops.back().Addr = 0x1000;
  B.Ops.back().Seq = 3;
  F.Blocks = {B};
  ASSERT_TRUE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error))
      << Error;
  F.Blocks[0].Ops[2].Inputs[1].Size = 4;
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error));
  F.Blocks[0].Ops[2].Inputs[1].Size = 8;
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 0, Error));
  auto Earlier = op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)});
  Earlier.Addr = 0x0ff0;
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.end() - 1, Earlier);
  EXPECT_FALSE(sourceCallStackArgumentsDefined(F, H, Arch::X64, 8, Error));
}
TEST(SourceCallABI, VariadicCountCannotSurviveExpiredTempsOrOpaqueEffects) {
  auto Hint = contract({NdType::makeInt(4), NdType::makeFloat(8)}, 1);
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(Hint, Arch::X64, BinaryFormat::ELF, Error));
  const auto RAX = NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
  LowFunc Function;
  LowBlock Block;
  Block.Ops = {op(NdOp::COPY, NdVar::tmp(7, 4), {NdVar::cst(1, 4)}),
               op(NdOp::INT_ZEXT, RAX, {NdVar::tmp(7, 4)}),
               op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  Block.Ops[0].Addr = Block.Ops[1].Addr = 0xf00;
  Block.Ops.back().Addr = 0x1000;
  Block.Ops.back().Seq = 3;
  Function.Blocks = {Block};
  ASSERT_TRUE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
  Function.Blocks[0].Ops[1].Addr = 0xf04;
  EXPECT_FALSE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
  Function.Blocks[0] = Block;
  auto Opaque = op(NdOp::INTRINSIC, {}, {});
  Opaque.Addr = 0xf08;
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.end() - 1, Opaque);
  EXPECT_FALSE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
  auto Redefined = op(NdOp::COPY, RAX, {NdVar::cst(1, 8)});
  Redefined.Addr = 0xf0c;
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.end() - 1, Redefined);
  EXPECT_TRUE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
}

TEST(SourceCallABI, StackWriteProofCannotSurviveExpiredTempsOrOpaqueEffects) {
  auto Hint = contract(std::vector<TypeRef>(5, NdType::makeInt(8)));
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(Hint, Arch::X64, BinaryFormat::COFF, Error));
  const auto SP = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
  LowFunc Function;
  LowBlock Block;
  Block.Ops = {op(NdOp::INT_ADD, NdVar::tmp(7, 8), {SP, NdVar::cst(32, 8)}),
               op(NdOp::STORE, {}, {NdVar::tmp(7, 8), NdVar::cst(17, 8)}),
               op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  Block.Ops[0].Addr = Block.Ops[1].Addr = 0xf00;
  Block.Ops.back().Addr = 0x1000;
  Block.Ops.back().Seq = 3;
  Function.Blocks = {Block};
  ASSERT_TRUE(
      sourceCallStackArgumentsDefined(Function, Hint, Arch::X64, 8, Error));
  Function.Blocks[0].Ops[1].Addr = 0xf04;
  EXPECT_FALSE(
      sourceCallStackArgumentsDefined(Function, Hint, Arch::X64, 8, Error));
  Function.Blocks[0] = Block;
  auto Opaque = op(NdOp::INTRINSIC, {}, {});
  Opaque.Addr = 0xf08;
  Function.Blocks[0].Ops.insert(Function.Blocks[0].Ops.end() - 1, Opaque);
  EXPECT_FALSE(
      sourceCallStackArgumentsDefined(Function, Hint, Arch::X64, 8, Error));
}

TEST(SourceCallABI, VariadicCountUsesSharedWidthCorrectArithmeticFacts) {
  auto Hint = contract({NdType::makeInt(4), NdType::makeFloat(8)}, 1);
  std::string Error;
  ASSERT_TRUE(assignAnalystCallABI(Hint, Arch::X64, BinaryFormat::ELF, Error));
  const auto RAX = NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
  LowFunc Function;
  Function.Blocks.emplace_back();
  auto &Ops = Function.Blocks.front().Ops;
  Ops = {op(NdOp::COPY, RAX, {NdVar::cst(3, 8)}),
         op(NdOp::INT_SUB, RAX, {RAX, NdVar::cst(2, 8)}),
         op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)})};
  Ops[0].Addr = 0xf00;
  Ops[1].Addr = 0xf04;
  Ops.back().Addr = 0x1000;
  Ops.back().Seq = 3;
  EXPECT_TRUE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
  Ops[1].Inputs[1] = NdVar::cst(4, 8);
  EXPECT_FALSE(
      sourceCallVariadicStateDefined(Function, Hint, BinaryFormat::ELF, Error));
}

MedVar value(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = 8;
  return V;
}
TEST(SourceCallABI, RefusesClobberedAndUnseededPhiCarriers) {
  auto H = contract({NdType::makeInt(8)});
  MedFunc F;
  MedBlock B;
  B.Id = 0;
  MedOp D;
  D.Opcode = NdOp::COPY;
  D.Output = value(1);
  D.addInput(MedVar::makeConst(7, 8));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.addInput(value(1));
  B.Ops = {D, Call};
  F.Blocks = {B};
  std::string Error;
  EXPECT_TRUE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F.CallClobbers.push_back({value(1), 1});
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F.CallClobbers.clear();
  F.Blocks[0].Ops.erase(F.Blocks[0].Ops.begin());
  PhiNode Phi;
  Phi.Output = value(1);
  Phi.Args = {{0, value(1)}};
  F.Blocks[0].Phis = {Phi};
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
}
TEST(SourceCallABI, AcceptsOnlyTheExactPromotedEntryRegisterMarker) {
  auto H = contract({NdType::makeInt(8)});
  MedFunc F;
  MedBlock B;
  B.Id = 0;
  MedVar P = value(1);
  P.Kind = MedVar::Param;
  P.RegOff = 56;
  F.Params = {P};
  auto Input = P;
  Input.Kind = MedVar::Reg;
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.addInput(Input);
  B.Ops = {Call};
  F.Blocks = {B};
  std::string Error;
  EXPECT_TRUE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error)) << Error;
  F.Blocks[0].Ops[0].Inputs[1].SSAVer = 1;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
}

TEST(SourceCallABI, RecordMemberProjectionDoesNotRequirePaddingOrDefineIt) {
  auto H = contract({NdType::makeInt(kDarwinFloatBytes)});
  MedFunc F;
  MedVar Parameter = value(1);
  Parameter.Kind = MedVar::Param;
  Parameter.RegOff = 0;
  Parameter.Size = kDarwinFloatBytes;
  F.Params = {Parameter};
  auto Input = Parameter;
  Input.Kind = MedVar::Reg;
  Input.Size = kDarwinRegisterBytes;
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Output = value(2);
  Copy.Output.Size = kDarwinRegisterBytes;
  Copy.addInput(Input);
  MedOp Extract;
  Extract.Opcode = NdOp::SUBBYTES;
  Extract.Output = value(3);
  Extract.Output.Size = kDarwinFloatBytes;
  Extract.addInput(Copy.Output);
  Extract.addInput(MedVar::makeConst(0, kDarwinFloatBytes));
  MedOp Call;
  Call.Opcode = NdOp::CALL;
  Call.SourceCallHint = std::make_shared<SourceCallTypeHint>(H);
  Call.addInput(MedVar::makeConst(0x2000, kDarwinRegisterBytes));
  Call.addInput(Extract.Output);
  F.Blocks.emplace_back();
  F.Blocks.front().Ops = {Copy, Extract, Call};
  std::string Error;
  EXPECT_TRUE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error)) << Error;
  const auto Good = F;
  F.Blocks.front().Ops.back().Inputs[1] = Copy.Output;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F = Good;
  F.Blocks.front().Ops[1].Inputs[1].ConstVal = kDarwinFloatBytes;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F = Good;
  F.Blocks.front().Ops.front().Inputs[0].SSAVer = 1;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F = Good;
  F.Blocks.front().Ops.front().Inputs[0].RegOff = kDarwinRegisterBytes;
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
  F = Good;
  F.Blocks.front().Ops.front().Opcode = NdOp::INT_ADD;
  F.Blocks.front().Ops.front().addInput(
      MedVar::makeConst(0, kDarwinRegisterBytes));
  EXPECT_FALSE(sourceCallArgumentsDefined(F, *H.AnalystSite, Error));
}
} // namespace
