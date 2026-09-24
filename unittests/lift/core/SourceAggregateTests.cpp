#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/loader/ObjC/ObjCEncoding.h"

#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
TypeRef pair(unsigned Bytes = 8) {
  return NdType::makeStruct(
      {NdType::makeFloat(Bytes), NdType::makeFloat(Bytes)});
}
TypeRef quad(unsigned Bytes = 8) {
  return NdType::makeStruct({pair(Bytes), pair(Bytes)});
}
SourceFunctionTypeHint declaration(TypeRef Record) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = Record;
  Hint.Parameters = {{"self", NdType::makePtr(NdType::makeVoid())},
                     {"cmd", NdType::makePtr(NdType::makeVoid())},
                     {"value", Record}};
  return Hint;
}

TEST(SourceAggregate,
     RecordLayoutPreservesNestedMembersAndRejectsMalformedGraphs) {
  auto Q = quad();
  ASSERT_TRUE(Q);
  EXPECT_EQ(Q->Size, 32);
  EXPECT_EQ(Q->Alignment, 8);
  EXPECT_EQ(Q->FieldOffsets, (std::vector<uint16_t>{0, 16}));
  EXPECT_TRUE(equalSourceTypes(Q, quad()));
  EXPECT_FALSE(equalSourceTypes(
      Q, NdType::makeStruct({NdType::makeFloat(8), NdType::makeFloat(8),
                             NdType::makeFloat(8), NdType::makeFloat(8)})));
  auto Padded = NdType::makeStruct({NdType::makeInt(1), NdType::makeFloat(8)});
  ASSERT_TRUE(Padded);
  EXPECT_EQ(Padded->Size, 16);
  EXPECT_EQ(Padded->FieldOffsets, (std::vector<uint16_t>{0, 8}));
  EXPECT_TRUE(sourceAggregateMembers(Padded).empty());
  Q->FieldOffsets[1] = 8;
  EXPECT_FALSE(equalSourceTypes(Q, Q));
  EXPECT_FALSE(NdType::makeStruct({Q}));
  EXPECT_FALSE(NdType::makeStruct({}));
  EXPECT_FALSE(NdType::makeStruct({NdType::makeVoid()}));
  auto Cycle = quad();
  Cycle->Fields[0] = Cycle;
  EXPECT_FALSE(NdType::makeStruct({Cycle}));
  EXPECT_FALSE(equalSourceTypes(Cycle, Cycle));
  Cycle->Fields.clear();
  auto Deep = pair();
  for (unsigned I = 0; I < 17 && Deep; ++I)
    Deep = NdType::makeStruct({Deep});
  EXPECT_FALSE(Deep);
}

TEST(SourceAggregate, RecordEncodingSeparatesShapeFromPhysicalABI) {
  auto A = parseObjCMethodEncoding(
      "value:", "{Outer={One=dd}{Two=dd}}48@0:8{Other={A=dd}{B=dd}}16");
  ASSERT_TRUE(A);
  EXPECT_TRUE(equalSourceTypes(A->ReturnType, A->Parameters.back().Type));
  EXPECT_FALSE(A->HasExplicitABI);
  std::string Error;
  EXPECT_TRUE(assignDarwinObjCSourceABI(*A, Arch::AArch64, Error)) << Error;
  auto B = *A;
  EXPECT_FALSE(assignDarwinObjCSourceABI(B, Arch::X64, Error));
  auto Scalar = *A;
  EXPECT_FALSE(assignDarwinScalarSourceABI(Scalar, Arch::AArch64, Error));
  for (const auto *Encoding : {"{Opaque}", "{S=}", "{S=d", "{S=vd}", "{S=[2d]}",
                               "(U=dd)", "{S=b4d}"}) {
    size_t Offset = 0;
    EXPECT_FALSE(parseObjCSourceType(Encoding, Offset)) << Encoding;
  }
  auto Mixed = parseObjCMethodEncoding("value:", "v32@0:8{S=Qd}16");
  ASSERT_TRUE(Mixed);
  EXPECT_FALSE(assignDarwinObjCSourceABI(*Mixed, Arch::AArch64, Error));
}

TEST(SourceAggregate, FixedCRecordsAllocateOnlyExplicitParameters) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(A);
    for (bool Floating : {false, true}) {
      if (Floating && A != Arch::AArch64)
        continue;
      const auto Scalar = Floating ? NdType::makeFloat(8) : NdType::makeInt(8);
      const auto Record = NdType::makeStruct({Scalar, Scalar});
      for (unsigned Count : {0U, 1U, 2U}) {
        SourceFunctionTypeHint H;
        H.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
        H.ReturnType = Record;
        H.Parameters.assign(Count, {"value", Record});
        if (Count)
          H.Parameters.push_back({"tail", Scalar});
        std::string Error;
        ASSERT_TRUE(assignDarwinFixedSourceABI(H, A, Error)) << Error;
        const auto Params = Floating ? TRI.FPParamRegs : TRI.IntParamRegs;
        const auto Returns = Floating ? TRI.FPParamRegs : TRI.IntReturnRegs;
        ASSERT_EQ(H.ReturnComponents.size(), 2U);
        for (unsigned I = 0; I < 2; ++I)
          EXPECT_EQ(H.ReturnComponents[I].RegisterOffset, Returns[I]);
        for (unsigned I = 0; I < Count; ++I) {
          ASSERT_EQ(H.Parameters[I].Components.size(), 2U);
          for (unsigned J = 0; J < 2; ++J)
            EXPECT_EQ(H.Parameters[I].Components[J].RegisterOffset,
                      Params[I * 2 + J]);
        }
        if (Count)
          EXPECT_EQ(H.Parameters.back().Location.RegisterOffset,
                    Params[Count * 2]);
        EXPECT_EQ(sourceABIParameters(H).size(), Count ? Count * 2 + 1 : 0);
        auto Restricted = H;
        EXPECT_FALSE(assignDarwinScalarSourceABI(Restricted, A, Error));
        Restricted = H;
        EXPECT_FALSE(assignDarwinObjCSourceABI(Restricted, A, Error));
        if (Count) {
          auto Method = declaration(Record);
          ASSERT_TRUE(assignDarwinObjCSourceABI(Method, A, Error));
          const auto Physical = sourceABIParameters(Method);
          Method.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
          ASSERT_TRUE(assignDarwinFixedSourceABI(Method, A, Error));
          const auto Explicit = sourceABIParameters(Method);
          ASSERT_EQ(Physical.size(), Explicit.size());
          for (size_t I = 0; I < Physical.size(); ++I) {
            EXPECT_EQ(Physical[I].Location.RegisterOffset,
                      Explicit[I].Location.RegisterOffset);
            EXPECT_EQ(Physical[I].Location.Kind, Explicit[I].Location.Kind);
          }
        }
      }
    }
    for (const auto &Record :
         {quad(),
          NdType::makeStruct({NdType::makeInt(8), NdType::makeFloat(8)})}) {
      auto H = declaration(Record);
      H.Origin = SourceFunctionTypeHint::OriginKind::DarwinSDK;
      std::string Error;
      EXPECT_EQ(assignDarwinFixedSourceABI(H, A, Error),
                A == Arch::AArch64 && Record->Size == 32);
    }
    auto Malformed = pair();
    Malformed->FieldOffsets[1] = 0;
    auto H = declaration(Malformed);
    std::string Error;
    EXPECT_FALSE(assignDarwinFixedSourceABI(H, A, Error));
  }
}

TEST(SourceAggregate, FloatingMembersUseIndependentContiguousRegisters) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  for (unsigned Bytes : {4U, 8U})
    for (unsigned Count : {1U, 2U, 3U, 4U}) {
      auto Record = NdType::makeStruct(
          std::vector<TypeRef>(Count, NdType::makeFloat(Bytes)));
      auto H = declaration(Record);
      H.Parameters.insert(H.Parameters.begin() + 2,
                          {"leading", NdType::makeFloat(8)});
      H.Parameters.push_back({"integer", NdType::makeInt(8)});
      H.Parameters.push_back({"trailing", NdType::makeFloat(8)});
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(H, Arch::AArch64, Error)) << Error;
      ASSERT_EQ(H.ReturnComponents.size(), Count);
      ASSERT_EQ(H.Parameters[3].Components.size(), Count);
      for (unsigned I = 0; I < Count; ++I) {
        EXPECT_EQ(H.ReturnComponents[I].RegisterOffset, TRI.FPParamRegs[I]);
        EXPECT_EQ(H.Parameters[3].Components[I].RegisterOffset,
                  TRI.FPParamRegs[I + 1]);
        EXPECT_EQ(H.Parameters[3].Components[I].ValueBytes, Bytes);
      }
      EXPECT_EQ(H.Parameters[4].Location.RegisterOffset, TRI.IntParamRegs[2]);
      EXPECT_EQ(H.Parameters[5].Location.RegisterOffset,
                TRI.FPParamRegs[Count + 1]);
      const auto Physical = sourceABIParameters(H);
      ASSERT_EQ(Physical.size(), Count + 5);
      for (unsigned I = 0; I < Count; ++I) {
        EXPECT_EQ(Physical[I + 3].ParameterIndex, 3U);
        EXPECT_EQ(Physical[I + 3].ByteOffset, I * Bytes);
      }
    }
}

TEST(SourceAggregate, ExhaustedFloatingBankKeepsWholeRecordOnStack) {
  for (unsigned Bytes : {4U, 8U}) {
    auto H = declaration(pair(Bytes));
    H.Parameters.insert(H.Parameters.begin() + 2, 7,
                        {"leading", NdType::makeFloat(8)});
    H.Parameters.push_back({"tail", NdType::makeFloat(Bytes)});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(H, Arch::AArch64, Error)) << Error;
    const auto &Record = H.Parameters[9];
    ASSERT_EQ(Record.Components.size(), 2U);
    for (unsigned I = 0; I < 2; ++I) {
      EXPECT_EQ(Record.Components[I].Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(Record.Components[I].EntryStackOffset, I * Bytes);
    }
    EXPECT_EQ(H.Parameters[10].Location.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(H.Parameters[10].Location.EntryStackOffset, 2 * Bytes);
  }
}

TEST(SourceAggregate, WordRecordsPreserveTypedMembersAndIntegerCarriers) {
  const auto Signed = NdType::makeInt(8, true);
  const auto Unsigned = NdType::makeInt(8, false);
  const auto Pointer = NdType::makePtr(Unsigned);
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (auto First : {Signed, Unsigned, Pointer})
      for (auto Second : {Signed, Unsigned, Pointer})
        for (unsigned Count : {1U, 2U}) {
          const auto Record = NdType::makeStruct(
              Count == 1
                  ? std::vector<TypeRef>{First}
                  : std::vector<TypeRef>{NdType::makeStruct({First}), Second});
          auto H = declaration(Record);
          H.Parameters.insert(H.Parameters.begin() + 2,
                              {"float", NdType::makeFloat(8)});
          H.Parameters.push_back({"tail", Unsigned});
          std::string Error;
          ASSERT_TRUE(assignDarwinObjCSourceABI(H, A, Error)) << Error;
          const auto &TRI = getTargetRegInfo(A);
          ASSERT_EQ(H.ReturnComponents.size(), Count);
          ASSERT_EQ(H.Parameters[3].Components.size(), Count);
          const auto Physical = sourceABIParameters(H);
          ASSERT_EQ(Physical.size(), Count + 4);
          for (unsigned I = 0; I < Count; ++I) {
            EXPECT_EQ(H.ReturnComponents[I].Kind,
                      SourceABICarrierKind::IntegerRegister);
            EXPECT_EQ(H.ReturnComponents[I].RegisterOffset,
                      TRI.IntReturnRegs[I]);
            EXPECT_EQ(H.Parameters[3].Components[I].RegisterOffset,
                      TRI.IntParamRegs[I + 2]);
            EXPECT_EQ(Physical[I + 3].ParameterIndex, 3U);
            EXPECT_EQ(Physical[I + 3].ByteOffset, I * 8);
            EXPECT_TRUE(equalSourceTypes(Physical[I + 3].Type,
                                         I == 0 ? First : Second));
          }
          EXPECT_EQ(H.Parameters[2].Location.RegisterOffset,
                    TRI.FPParamRegs[0]);
          EXPECT_EQ(H.Parameters[4].Location.RegisterOffset,
                    TRI.IntParamRegs[Count + 2]);
        }
  for (auto Bad : {NdType::makeStruct({Unsigned, NdType::makeFloat(8)}),
                   NdType::makeStruct({NdType::makeInt(4), NdType::makeInt(4)}),
                   NdType::makeStruct({NdType::makeInt(1), Pointer})}) {
    EXPECT_TRUE(sourceAggregateMembers(Bad).empty());
    for (Arch A : {Arch::AArch64, Arch::X64}) {
      auto H = declaration(Bad);
      std::string Error;
      EXPECT_FALSE(assignDarwinObjCSourceABI(H, A, Error));
    }
  }
}

TEST(SourceAggregate, ThreeWordResultsUseOnlyTheDarwinArm64IndirectPointer) {
  auto Hint =
      parseObjCMethodEncoding("operatingSystemVersion", "{?=qqq}16@0:8");
  ASSERT_TRUE(Hint);
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(*Hint, Arch::AArch64, Error)) << Error;
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  EXPECT_EQ(Hint->ReturnLocation.Kind,
            SourceABICarrierKind::IndirectResultPointer);
  EXPECT_EQ(Hint->ReturnLocation.RegisterOffset, TRI.indirectResultReg());
  EXPECT_EQ(Hint->ReturnLocation.ValueBytes, 8U);
  EXPECT_TRUE(Hint->ReturnComponents.empty());
  ASSERT_EQ(sourceABIParameters(*Hint).size(), 2U);
  EXPECT_EQ(Hint->Parameters[0].Location.RegisterOffset, TRI.IntParamRegs[0]);
  EXPECT_EQ(Hint->Parameters[1].Location.RegisterOffset, TRI.IntParamRegs[1]);
  EXPECT_EQ(sourceAggregateMembers(Hint->ReturnType).size(), 3U);
  const auto Signed = NdType::makeInt(8, true);
  auto Nested = *Hint;
  Nested.ReturnType =
      NdType::makeStruct({NdType::makeStruct({Signed, Signed}), Signed});
  ASSERT_TRUE(assignDarwinFixedSourceABI(Nested, Arch::AArch64, Error));
  const auto Members = sourceAggregateMembers(Nested.ReturnType);
  ASSERT_EQ(Members.size(), 3U);
  EXPECT_EQ(Members[2].ByteOffset, 16U);
  EXPECT_TRUE(equalSourceTypes(Members[2].Type, Signed));
  EXPECT_TRUE(Nested.ReturnComponents.empty());
  for (const auto &Other :
       {NdType::makeInt(8, false), NdType::makePtr(NdType::makeVoid())}) {
    for (const auto &Fields : {std::vector<TypeRef>{Other, Other, Other},
                               std::vector<TypeRef>{Signed, Other, Signed}}) {
      auto Bad = *Hint;
      Bad.ReturnType = NdType::makeStruct(Fields);
      EXPECT_TRUE(sourceAggregateMembers(Bad.ReturnType).empty());
      EXPECT_FALSE(validateSourceABI(Bad, Error));
      EXPECT_FALSE(assignDarwinFixedSourceABI(Bad, Arch::AArch64, Error));
    }
  }
  auto Unsupported = *Hint;
  EXPECT_FALSE(assignDarwinObjCSourceABI(Unsupported, Arch::AArch64, Error));
  Unsupported = *Hint;
  EXPECT_FALSE(assignDarwinObjCSourceABI(Unsupported, Arch::X64, Error));
  Unsupported = *Hint;
  EXPECT_FALSE(assignDarwinScalarSourceABI(Unsupported, Arch::AArch64, Error));
  Unsupported = *Hint;
  EXPECT_FALSE(assignDarwinSwiftSourceABI(Unsupported, Arch::AArch64, Error));
  Unsupported = *Hint;
  Unsupported.Parameters.push_back({"record", Hint->ReturnType});
  EXPECT_FALSE(assignDarwinFixedSourceABI(Unsupported, Arch::AArch64, Error));
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Bad = *Hint;
    switch (Mutation) {
    case 0:
      Bad.ReturnLocation.RegisterOffset = TRI.IntReturnReg;
      break;
    case 1:
      Bad.ReturnLocation.ValueBytes = 24;
      break;
    case 2:
      Bad.ReturnLocation.EntryStackOffset = 8;
      break;
    case 3:
      Bad.ReturnLocation.ExtendTo32Bits = true;
      break;
    case 4:
      Bad.ReturnComponents.push_back(Bad.Parameters[0].Location);
      break;
    case 5:
      Bad.ReturnType = NdType::makeStruct(
          {NdType::makeFloat(8), NdType::makeFloat(8), NdType::makeFloat(8)});
      break;
    case 6:
      Bad.ReturnType = NdType::makeInt(8);
      break;
    case 7:
      Bad.Parameters[0].Location.RegisterOffset = TRI.indirectResultReg();
      break;
    case 8:
      Bad.Architecture = Arch::X64;
      break;
    }
    EXPECT_FALSE(validateSourceABI(Bad, Error));
  }
}

TEST(SourceAggregate, IndirectResultEntryRequiresASeparateStorageProof) {
  auto Hint =
      parseObjCMethodEncoding("operatingSystemVersion", "{?=qqq}16@0:8");
  ASSERT_TRUE(Hint);
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(*Hint, Arch::AArch64, Error));
  MedFunc Function;
  Function.SourceTypeHint = *Hint;
  inferMedTypes(Function, Arch::AArch64);
  EXPECT_FALSE(Function.SourceTypeHint);
  EXPECT_FALSE(Function.SourceParametersBound);
}

TEST(SourceAggregate,
     NarrowLeadingWordRecordUsesDistinctIntegerReturnCarriers) {
  const auto Record = NdType::makeStruct(
      {NdType::makeInt(4, false), NdType::makeInt(8, false)});
  ASSERT_TRUE(Record);
  const auto Members = sourceAggregateMembers(Record);
  ASSERT_EQ(Members.size(), 2U);
  EXPECT_EQ(Members[0].ByteOffset, 0U);
  EXPECT_EQ(Members[0].Type->Size, 4U);
  EXPECT_EQ(Members[1].ByteOffset, 8U);
  EXPECT_EQ(Members[1].Type->Size, 8U);

  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = parseObjCMethodEncoding("preferredPixelFormat:",
                                        "{SDImagePixelFormat=IQ}20@0:8B16");
    ASSERT_TRUE(Hint);
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(*Hint, Architecture, Error)) << Error;
    ASSERT_EQ(Hint->ReturnComponents.size(), 2U);
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint->ReturnComponents[0].RegisterOffset, TRI.IntReturnRegs[0]);
    EXPECT_EQ(Hint->ReturnComponents[0].ValueBytes, 4U);
    EXPECT_EQ(Hint->ReturnComponents[1].RegisterOffset, TRI.IntReturnRegs[1]);
    EXPECT_EQ(Hint->ReturnComponents[1].ValueBytes, 8U);
    EXPECT_TRUE(validateSourceABI(*Hint, Error)) << Error;

    auto Bad = *Hint;
    Bad.ReturnComponents[0].ValueBytes = 8;
    EXPECT_FALSE(validateSourceABI(Bad, Error));

    SourceFunctionTypeHint ParameterHint;
    ParameterHint.ReturnType = NdType::makeVoid();
    ParameterHint.Parameters = {{"value", Record}};
    ASSERT_TRUE(assignDarwinFixedSourceABI(ParameterHint, Architecture, Error))
        << Error;
    ASSERT_EQ(ParameterHint.Parameters[0].Components.size(), 2U);
    const auto Physical = sourceABIParameters(ParameterHint);
    ASSERT_EQ(Physical.size(), 2U);
    EXPECT_EQ(Physical[0].ByteOffset, 0U);
    EXPECT_EQ(Physical[0].Location.ValueBytes, 4U);
    EXPECT_EQ(Physical[1].ByteOffset, 8U);
    EXPECT_EQ(Physical[1].Location.ValueBytes, 8U);
  }
}

TEST(SourceAggregate,
     NarrowLeadingWordRecordCallExtractsFieldsAtNaturalOffsets) {
  for (const auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.ReturnType = NdType::makeStruct(
        {NdType::makeInt(4, false), NdType::makeInt(8, false)});
    std::string Error;
    ASSERT_TRUE(assignDarwinFixedSourceABI(Hint, Architecture, Error)) << Error;

    LowFunc Low;
    Low.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = 0x1000;
    Block.EndAddr = 0x1008;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Addr = 0x1000;
    Call.addInput(NdVar::cst(0x2000, 8));
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x1004;
    Block.Ops = {Call, Return};
    Low.Blocks = {Block};
    std::map<va_t, SourceFunctionTypeHint> Hints{{0x2000, Hint}};
    LowToMedConverter Converter;
    Converter.setSourceCallHintsEnabled(true);
    Converter.setSourceCalleeTypeHints(&Hints);
    const auto Med = Converter.convert(Low, Architecture, BinaryFormat::MachO);
    ASSERT_GE(Med.Blocks[0].Ops.size(), 4U);
    const auto &BoundCall = Med.Blocks[0].Ops[0];
    ASSERT_TRUE(BoundCall.SourceCallHint);
    ASSERT_EQ(BoundCall.Output.Size, 16U);
    std::map<uint64_t, uint16_t> Extracts;
    for (const auto &Operation : Med.Blocks[0].Ops)
      if (Operation.Opcode == NdOp::SUBBYTES && Operation.NumInputs == 2 &&
          Operation.Inputs[0] == BoundCall.Output &&
          Operation.Inputs[1].isConst())
        Extracts.emplace(Operation.Inputs[1].ConstVal, Operation.Output.Size);
    EXPECT_EQ(Extracts, (std::map<uint64_t, uint16_t>{{0, 4}, {8, 8}}));
  }
}

TEST(SourceAggregate, SwiftFourWordCallPreservesEveryReturnCarrier) {
  const auto Word = NdType::makeInt(8, false);
  const auto Record = NdType::makeStruct({Word, Word, Word, Word});
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = Record;
  std::string Error;
  ASSERT_TRUE(assignDarwinSwiftSourceABI(Hint, Arch::AArch64, Error)) << Error;

  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "swift_four_word_call";
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = 0x1000;
  Block.EndAddr = 0x1008;
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Call.Addr = 0x1000;
  Call.addInput(NdVar::cst(0x2000, 8));
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1004;
  Block.Ops = {Call, Return};
  Low.Blocks = {Block};
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1000, Hint}, {0x2000, Hint}};
  LowToMedConverter Converter;
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Hints);
  auto Med = Converter.convert(Low, Arch::AArch64, BinaryFormat::MachO);
  ASSERT_FALSE(Med.Blocks.empty());
  ASSERT_FALSE(Med.Blocks.front().Ops.empty());
  const auto &BoundCall = Med.Blocks.front().Ops.front();
  ASSERT_TRUE(BoundCall.SourceCallHint);
  EXPECT_EQ(BoundCall.Output.Size, 32U);
  std::map<uint64_t, uint16_t> Extracts;
  for (const auto &Operation : Med.Blocks.front().Ops)
    if (Operation.Opcode == NdOp::SUBBYTES && Operation.NumInputs == 2 &&
        Operation.Inputs[0] == BoundCall.Output &&
        Operation.Inputs[1].isConst())
      Extracts.emplace(Operation.Inputs[1].ConstVal, Operation.Output.Size);
  EXPECT_EQ(Extracts,
            (std::map<uint64_t, uint16_t>{{0, 8}, {8, 8}, {16, 8}, {24, 8}}));

  Med.SourceTypeHint = Hint;
  recoverCallAbi(Med, Arch::AArch64, {});
  inferMedTypes(Med, Arch::AArch64);
  const auto High = MedToHighConverter().convert(Med, Arch::AArch64);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({High}, OS, Options));
  OS.flush();
  EXPECT_NE(C.find("swiftcall"), std::string::npos) << C;
  EXPECT_NE(C.find("field_3"), std::string::npos) << C;
}

TEST(SourceAggregate, WordRecordSpillsKeepArchitectureSpecificBankState) {
  const auto Word = NdType::makeInt(8, false);
  for (Arch A : {Arch::AArch64, Arch::X64})
    for (unsigned Available : {0U, 1U, 2U}) {
      const auto &TRI = getTargetRegInfo(A);
      const size_t Prefix = TRI.IntParamRegs.size() - Available;
      auto H = declaration(NdType::makeStruct({Word, Word}));
      H.Parameters.insert(H.Parameters.begin() + 2, Prefix - 2,
                          {"leading", Word});
      H.Parameters.push_back({"single", NdType::makeStruct({Word})});
      H.Parameters.push_back({"tail", Word});
      H.Parameters.push_back({"floating", NdType::makeFloat(8)});
      std::string Error;
      ASSERT_TRUE(assignDarwinObjCSourceABI(H, A, Error)) << Error;
      const auto &Record = H.Parameters[Prefix];
      const bool Stack = Available < 2;
      const int64_t StackBase = A == Arch::X64 ? 8 : 0;
      ASSERT_EQ(Record.Components.size(), 2U);
      for (unsigned I = 0; I < 2; ++I) {
        EXPECT_EQ(Record.Components[I].Kind,
                  Stack ? SourceABICarrierKind::Stack
                        : SourceABICarrierKind::IntegerRegister);
        if (Stack)
          EXPECT_EQ(Record.Components[I].EntryStackOffset, StackBase + I * 8);
        else
          EXPECT_EQ(Record.Components[I].RegisterOffset,
                    TRI.IntParamRegs[Prefix + I]);
      }
      const auto &Single = H.Parameters[Prefix + 1].Components.front();
      if (A == Arch::X64 && Available == 1) {
        EXPECT_EQ(Single.Kind, SourceABICarrierKind::IntegerRegister);
        EXPECT_EQ(Single.RegisterOffset, TRI.IntParamRegs.back());
      } else {
        EXPECT_EQ(Single.Kind, SourceABICarrierKind::Stack);
        EXPECT_EQ(Single.EntryStackOffset, StackBase + (Stack ? 16 : 0));
      }
      EXPECT_EQ(H.Parameters[Prefix + 2].Location.Kind,
                SourceABICarrierKind::Stack);
      EXPECT_EQ(H.Parameters[Prefix + 2].Location.EntryStackOffset,
                Single.Kind == SourceABICarrierKind::Stack
                    ? Single.EntryStackOffset + 8
                    : StackBase + 16);
      EXPECT_EQ(H.Parameters.back().Location.RegisterOffset,
                TRI.FPParamRegs.front());
    }
}

TEST(SourceAggregate, WordRecordValidationRejectsIncompleteAndWrongBanks) {
  for (Arch A : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(A);
    auto H = declaration(NdType::makeStruct(
        {NdType::makeInt(8), NdType::makePtr(NdType::makeVoid())}));
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(H, A, Error));
    for (unsigned I = 0; I < 8; ++I) {
      auto Bad = H;
      switch (I) {
      case 0:
        Bad.ReturnComponents.pop_back();
        break;
      case 1:
        Bad.ReturnComponents[1] = Bad.ReturnComponents[0];
        break;
      case 2:
        Bad.ReturnComponents[0].RegisterOffset = TRI.IntParamRegs.back();
        break;
      case 3:
        Bad.ReturnComponents[1].Kind = SourceABICarrierKind::FloatingRegister;
        Bad.ReturnComponents[1].RegisterOffset = TRI.FPParamRegs[1];
        break;
      case 4:
        Bad.Parameters[2].Components[1].ValueBytes = 4;
        break;
      case 5:
        Bad.Parameters[2].Components[1] = {SourceABICarrierKind::Stack, 0, 8,
                                           8};
        break;
      case 6:
        Bad.Parameters[2].Components[0].ExtendTo32Bits = true;
        break;
      case 7:
        Bad.Parameters[2].Components[1] = Bad.Parameters[2].Components[0];
        break;
      }
      EXPECT_FALSE(validateSourceABI(Bad, Error)) << I;
      EXPECT_TRUE(sourceABIParameters(Bad).empty()) << I;
    }
  }
}

TEST(SourceAggregate, ValidationRejectsIncompleteAliasedAndStaleCarriers) {
  auto H = declaration(quad());
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(H, Arch::AArch64, Error));
  for (unsigned I = 0; I < 10; ++I) {
    auto Bad = H;
    switch (I) {
    case 0:
      Bad.Parameters[2].Components.pop_back();
      break;
    case 1:
      Bad.Parameters[2].Components[1] = Bad.Parameters[2].Components[0];
      break;
    case 2:
      Bad.Parameters[2].Components[0].ValueBytes = 4;
      break;
    case 3:
      Bad.Parameters[2].Location = Bad.Parameters[2].Components[0];
      break;
    case 4:
      Bad.ReturnComponents[3].RegisterOffset =
          Bad.ReturnComponents[2].RegisterOffset;
      break;
    case 5:
      Bad.ReturnComponents[1].ExtendTo32Bits = true;
      break;
    case 6:
      Bad.Architecture = Arch::X64;
      break;
    case 7:
      Bad.ReturnType = NdType::makeInt(32);
      break;
    case 8:
      Bad.Parameters[2].Type = pair();
      break;
    case 9:
      Bad.Parameters[2].Components[1].EntryStackOffset = 8;
      break;
    }
    EXPECT_FALSE(validateSourceABI(Bad, Error)) << I;
    EXPECT_TRUE(sourceABIParameters(Bad).empty()) << I;
  }
  H.Parameters.insert(H.Parameters.end(), 16, H.Parameters.back());
  EXPECT_FALSE(assignDarwinObjCSourceABI(H, Arch::AArch64, Error));
}

MedFunc convertRecord(bool Call) {
  auto H = declaration(quad());
  std::string Error;
  EXPECT_TRUE(assignDarwinObjCSourceABI(H, Arch::AArch64, Error));
  LowFunc L;
  L.Entry = 0x1000;
  L.Name = Call ? "record_call" : "record_entry";
  LowBlock B;
  B.Id = 0;
  B.StartAddr = 0x1000;
  B.EndAddr = 0x1008;
  if (Call) {
    LowOp Op;
    Op.Opcode = NdOp::CALL;
    Op.Addr = 0x1000;
    Op.addInput(NdVar::cst(0x2000, 8));
    B.Ops.push_back(Op);
  }
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x1004;
  B.Ops.push_back(Ret);
  L.Blocks.push_back(B);
  std::map<va_t, SourceFunctionTypeHint> Hints{{0x1000, H}, {0x2000, H}};
  LowToMedConverter Converter;
  Converter.setSourceCallHintsEnabled(true);
  Converter.setSourceCalleeTypeHints(&Hints);
  auto Med = Converter.convert(L, Arch::AArch64, BinaryFormat::MachO);
  Med.SourceTypeHint = H;
  recoverCallAbi(Med, Arch::AArch64, {});
  inferMedTypes(Med, Arch::AArch64);
  return Med;
}
TEST(SourceAggregate, EntryReturnsKeepEveryDeclaredFloatingDependency) {
  auto M = convertRecord(false);
  ASSERT_TRUE(M.SourceParametersBound);
  ASSERT_EQ(M.Params.size(), 6U);
  auto H = MedToHighConverter().convert(M, Arch::AArch64);
  ASSERT_EQ(H.Params.size(), 3U);
  ASSERT_TRUE(equalSourceTypes(H.Params[2].Type, quad()));
  ASSERT_FALSE(H.Body.empty());
  ASSERT_TRUE(H.Body.back().RetVal);
  EXPECT_EQ(H.Body.back().RetVal->Kind, ExprKind::Record);
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({H}, OS, Options));
  OS.flush();
  EXPECT_EQ(C.find("uint256_t"), std::string::npos) << C;
  EXPECT_NE(C.find(".field_1"), std::string::npos) << C;
  EXPECT_NE(C.find("#ifndef NEVERD_SOURCE_"), std::string::npos) << C;

  C.clear();
  Options.EmitRecordGuards = false;
  ASSERT_TRUE(HighCEmitter().emit({H}, OS, Options));
  OS.flush();
  EXPECT_EQ(C.find("#ifndef NEVERD_SOURCE_"), std::string::npos) << C;
  EXPECT_NE(C.find("struct nd_record_"), std::string::npos) << C;
  EXPECT_NE(C.find("_Static_assert(sizeof("), std::string::npos) << C;
}
TEST(SourceAggregate, CallsReconstructLogicalRecordsFromPhysicalOperands) {
  auto M = convertRecord(true);
  ASSERT_TRUE(M.SourceParametersBound);
  auto H = MedToHighConverter().convert(M, Arch::AArch64);
  const HighExpr *Call = nullptr;
  walkStmts(H.Body, [&](const HighStmt &S) {
    if (S.Val && S.Val->Kind == ExprKind::Call)
      Call = S.Val.get();
  });
  ASSERT_NE(Call, nullptr);
  ASSERT_EQ(Call->Operands.size(), 3U);
  EXPECT_EQ(Call->Operands[2]->Kind, ExprKind::Record);
  EXPECT_TRUE(equalSourceTypes(Call->Type, quad()));
}
TEST(SourceAggregate, FieldIdentityIncludesPositionAndCompleteRecordShape) {
  MedVar P;
  P.Kind = MedVar::Param;
  P.Id = 0;
  P.Size = 16;
  auto V = HighExpr::makeVar(P, pair());
  auto A = HighExpr::makeRecordField(V, 0, 8);
  auto B = HighExpr::makeRecordField(V, 8, 8);
  EXPECT_FALSE(A->structuralEq(*B));
  EXPECT_EQ(HighExpr::makeRecordField(V, 4, 8)->Kind, ExprKind::Undef);
  EXPECT_EQ(HighExpr::makeRecordField(V, 0, 4)->Kind, ExprKind::Undef);
  EXPECT_EQ(HighExpr::makeRecordField(V, 16, 8)->Kind, ExprKind::Undef);
  std::vector<ExprPtr> Leaves{A, B};
  auto R = HighExpr::makeRecord(pair(), Leaves);
  EXPECT_TRUE(HighExpr::makeRecordField(R, 0, 8)->structuralEq(*A));
  EXPECT_TRUE(HighExpr::makeRecordField(R, 8, 8)->structuralEq(*B));
  R->Operands[0].reset();
  EXPECT_EQ(HighExpr::makeRecordField(R, 0, 8)->Kind, ExprKind::Undef);
  R->Operands[0] = R;
  EXPECT_EQ(HighExpr::makeRecordField(R, 0, 8)->Kind, ExprKind::Undef);
  R->Operands[0].reset();
}
} // namespace
