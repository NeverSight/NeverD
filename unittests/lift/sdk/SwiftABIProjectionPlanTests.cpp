#include "../../../lib/sdk/capi/SwiftABIProjectionPlan.h"
#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"

using namespace neverd;
using namespace neverd::sdk::swift_source;

namespace {
SourceFunctionTypeHint signature(TypeRef Type = NdType::makeInt(8),
                                 Arch Architecture = Arch::X64) {
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = Type;
  Hint.Parameters.push_back({"arg0", Type});
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error)) << Error;
  return Hint;
}
ABIProjectionRow row(size_t Index, va_t Entry,
                     const SourceFunctionTypeHint &Hint) {
  return {Index, Entry, "$s_identity_" + std::to_string(Index), Hint};
}
std::map<size_t, std::pair<size_t, va_t>>
ownership(const ABIProjectionPlan &Plan,
          const std::vector<ABIProjectionRow> &Rows) {
  std::map<size_t, va_t> Entries;
  for (const auto &Row : Rows)
    Entries.emplace(Row.Index, Row.Entry);
  std::map<size_t, std::pair<size_t, va_t>> Result;
  for (size_t I = 0; I < Plan.Batches.size(); ++I)
    for (size_t Index : Plan.Batches[I].Rows) {
      EXPECT_TRUE(Entries.count(Index));
      EXPECT_TRUE(Plan.Batches[I].BodyHints.count(Entries.at(Index)));
      EXPECT_TRUE(
          Result.emplace(Index, std::make_pair(I, Entries.at(Index))).second);
    }
  return Result;
}
} // namespace

TEST(SwiftABIProjectionPlan, EqualAliasesShareOneBodyWithoutLosingRowIdentity) {
  auto Hint = signature();
  std::vector<ABIProjectionRow> Rows = {
      row(9, 0x1000, Hint), row(3, 0x1000, Hint), row(17, 0x2000, Hint)};
  auto Plan = planABIProjections(Rows, {});
  ASSERT_EQ(Plan.Batches.size(), 1U);
  EXPECT_EQ(Plan.Batches[0].BodyHints.size(), 2U);
  auto Owners = ownership(Plan, Rows);
  ASSERT_EQ(Owners.size(), 3U);
  EXPECT_EQ(Owners.at(9), Owners.at(3));
  EXPECT_NE(Owners.at(9).second, Owners.at(17).second);
  EXPECT_TRUE(Plan.BaseCalleeHints.empty());
}

TEST(SwiftABIProjectionPlan, BatchCountIsMaximumEntryVariantsNotTheirSum) {
  std::vector<ABIProjectionRow> Rows;
  for (unsigned Entry = 1; Entry <= 3; ++Entry)
    for (unsigned Variant = 0; Variant < Entry; ++Variant) {
      auto Hint = signature(NdType::makeInt(1U << Variant));
      Rows.push_back(row(Rows.size(), Entry * 0x1000, Hint));
      Rows.push_back(row(Rows.size(), Entry * 0x1000, Hint));
    }
  auto Plan = planABIProjections(Rows, {});
  ASSERT_EQ(Plan.Batches.size(), 3U);
  EXPECT_EQ(ownership(Plan, Rows).size(), 12U);
  EXPECT_EQ(Plan.Batches[0].BodyHints.size(), 3U);
  EXPECT_EQ(Plan.Batches[1].BodyHints.size(), 2U);
  EXPECT_EQ(Plan.Batches[2].BodyHints.size(), 1U);
}

TEST(SwiftABIProjectionPlan, CoalescedFloatAndDoubleKeepDistinctCarrierWidths) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Float = signature(NdType::makeFloat(4), Architecture);
    auto Double = signature(NdType::makeFloat(8), Architecture);
    std::vector<ABIProjectionRow> Rows = {
        row(0, 0x1000, Float), row(1, 0x1000, Double), row(2, 0x1000, Float)};
    auto Plan = planABIProjections(Rows, {});
    ASSERT_EQ(Plan.Batches.size(), 2U);
    auto Owners = ownership(Plan, Rows);
    EXPECT_EQ(Owners.at(0), Owners.at(2));
    EXPECT_NE(Owners.at(0).first, Owners.at(1).first);
    const auto &F = Plan.Batches[Owners.at(0).first].BodyHints.at(0x1000);
    const auto &D = Plan.Batches[Owners.at(1).first].BodyHints.at(0x1000);
    EXPECT_EQ(F.Parameters[0].Location.ValueBytes, 4U);
    EXPECT_EQ(D.Parameters[0].Location.ValueBytes, 8U);
    EXPECT_EQ(F.ReturnLocation.ValueBytes, 4U);
    EXPECT_EQ(D.ReturnLocation.ValueBytes, 8U);
    EXPECT_EQ(F.Parameters[0].Location.RegisterOffset,
              D.Parameters[0].Location.RegisterOffset);
  }
}

TEST(SwiftABIProjectionPlan, RegisterAndStackLocationsArePartOfTheFullHint) {
  auto A = signature();
  auto B = A;
  B.Parameters[0].Location.RegisterOffset =
      getTargetRegInfo(Arch::X64).IntParamRegs[1];
  auto C = A;
  C.Parameters[0].Location = {SourceABICarrierKind::Stack, 0, 8, 8};
  auto D = C;
  D.Parameters[0].Location.EntryStackOffset = 16;
  auto Plan = planABIProjections({row(0, 0x1000, A), row(1, 0x1000, B),
                                  row(2, 0x1000, C), row(3, 0x1000, D)},
                                 {});
  EXPECT_EQ(Plan.Batches.size(), 4U);
}

TEST(SwiftABIProjectionPlan, PointeeVectorsAndArrayElementsKeepIdentity) {
  const auto Word = NdType::makeInt(sizeof(uint64_t));
  auto Vector = std::make_shared<NdType>(*Word);
  Vector->SourceName = kSourceAArch64Vector64CType;
  const auto ArrayRecord = [](unsigned ElementBytes) {
    return NdType::makeStruct(
        {NdType::makeArray(NdType::makeInt(ElementBytes),
                           sizeof(uint64_t) / ElementBytes, sizeof(uint64_t)),
         NdType::makeFloat(sizeof(double))});
  };
  for (const auto &[Left, Right] :
       {std::pair{Word, Vector}, std::pair{ArrayRecord(sizeof(uint32_t)),
                                           ArrayRecord(sizeof(uint16_t))}}) {
    ASSERT_TRUE(Left);
    ASSERT_TRUE(Right);
    auto A = signature(NdType::makePtr(Left), Arch::AArch64);
    auto B = signature(NdType::makePtr(Right), Arch::AArch64);
    const auto Entry = getTargetRegInfo(Arch::AArch64).PointerSize;
    auto Plan = planABIProjections({row(0, Entry, A), row(1, Entry, B)}, {});
    EXPECT_EQ(Plan.Batches.size(), 2U);
  }
}

TEST(SwiftABIProjectionPlan, PairResultsAndCallbackTypesKeepCompleteABIKeys) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Pair = signature(NdType::makeInt(8), Architecture);
    Pair.ReturnType = NdType::makeInt(16, false);
    std::string Diagnostic;
    ASSERT_TRUE(assignDarwinScalarSourceABI(Pair, Architecture, Diagnostic));
    auto Scalar = signature(NdType::makeInt(8), Architecture);
    auto Callback = signature(
        NdType::makePtr(NdType::makeFunc(NdType::makeInt(8))), Architecture);
    auto OtherCallback = signature(
        NdType::makePtr(NdType::makeFunc(NdType::makeVoid())), Architecture);
    auto Plan = planABIProjections(
        {row(0, 0x1000, Pair), row(1, 0x1000, Scalar), row(2, 0x1000, Callback),
         row(3, 0x1000, OtherCallback)},
        {});
    EXPECT_EQ(Plan.Batches.size(), 4U);
    Pair.ReturnComponents.pop_back();
    EXPECT_THROW(planABIProjections({row(0, 0x1000, Pair)}, {}),
                 std::invalid_argument);
  }
  auto Extended = signature(NdType::makeInt(1), Arch::AArch64);
  auto Unextended = Extended;
  Unextended.ReturnLocation.ExtendTo32Bits = false;
  EXPECT_EQ(planABIProjections(
                {row(0, 0x1000, Extended), row(1, 0x1000, Unextended)}, {})
                .Batches.size(),
            2U);
}

TEST(SwiftABIProjectionPlan,
     NamesSignednessOriginAndPointeeTypesNeverCoalesce) {
  auto A = signature(NdType::makePtr(NdType::makeInt(4)));
  auto B = A;
  B.Parameters[0].Name = "swift_self";
  auto C = signature(NdType::makePtr(NdType::makeInt(4, false)));
  auto D = A;
  D.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto E = signature(NdType::makePtr(NdType::makeInt(8)));
  auto Plan = planABIProjections({row(0, 0x1000, A), row(1, 0x1000, B),
                                  row(2, 0x1000, C), row(3, 0x1000, D),
                                  row(4, 0x1000, E)},
                                 {});
  EXPECT_EQ(Plan.Batches.size(), 5U);
}

TEST(SwiftABIProjectionPlan, BooleanPointeesAndCallbackTypesKeepDistinctKeys) {
  const auto Boolean = NdType::makeInt(1, false);
  Boolean->SourceName = kSourceBooleanCType;
  const auto Byte = NdType::makeInt(1, false);
  for (const auto Architecture : {Arch::AArch64, Arch::X64})
    for (bool Callback : {false, true}) {
      const auto Type = [&](const TypeRef &Value) {
        return NdType::makePtr(Callback ? NdType::makeFunc(Value) : Value);
      };
      const auto A = signature(Type(Boolean), Architecture);
      const auto B = signature(Type(Byte), Architecture);
      EXPECT_FALSE(equalSourceABIs(A, B));
      const auto Plan = planABIProjections(
          {row(0, 0x1000, A), row(1, 0x1000, B), row(2, 0x1000, A)}, {});
      ASSERT_EQ(Plan.Batches.size(), 2U);
      const auto Owners = ownership(
          Plan, {row(0, 0x1000, A), row(1, 0x1000, B), row(2, 0x1000, A)});
      EXPECT_EQ(Owners.at(0), Owners.at(2));
      EXPECT_NE(Owners.at(0), Owners.at(1));
      EXPECT_THROW(planABIProjections({row(0, 0x1000, A)}, {{0x1000, B}}),
                   std::invalid_argument);
    }
}

TEST(SwiftABIProjectionPlan, BatchHintsNeverExpandUnambiguousCalleeEvidence) {
  auto Hint = signature();
  const std::map<va_t, SourceFunctionTypeHint> Base{{0x5000, Hint}};
  auto Plan = planABIProjections(
      {row(0, 0x5000, Hint), row(1, 0x1000, Hint), row(2, 0x1000, Hint)}, Base);
  EXPECT_EQ(Plan.BaseRows, (std::vector<size_t>{0}));
  ASSERT_EQ(Plan.Batches.size(), 1U);
  EXPECT_TRUE(Plan.Batches[0].BodyHints.count(0x1000));
  EXPECT_FALSE(Plan.BaseCalleeHints.count(0x1000));
  EXPECT_EQ(Plan.BaseCalleeHints.size(), 1U);
  EXPECT_TRUE(Plan.BaseCalleeHints.count(0x5000));
  auto Different = signature(NdType::makeFloat(8));
  EXPECT_THROW(planABIProjections({row(0, 0x5000, Different)}, Base),
               std::invalid_argument);
}

TEST(SwiftABIProjectionPlan, ConventionAndContextRoleKeepDistinctIdentity) {
  auto C = signature(NdType::makePtr(NdType::makeVoid()), Arch::AArch64);
  std::string Error;
  auto Swift = C;
  ASSERT_TRUE(assignDarwinSwiftSourceABI(Swift, Arch::AArch64, Error));
  // This shape has equal physical locations under both calling conventions.
  EXPECT_EQ(C.Parameters[0].Location.RegisterOffset,
            Swift.Parameters[0].Location.RegisterOffset);
  auto Context = Swift;
  Context.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
  ASSERT_TRUE(assignDarwinSwiftSourceABI(Context, Arch::AArch64, Error));
  auto Ordinary = C;
  Ordinary.Parameters[0].Location = Context.Parameters[0].Location;
  ASSERT_TRUE(validateSourceABI(Ordinary, Error));
  auto Plan =
      planABIProjections({row(0, 0x1000, C), row(1, 0x1000, Swift),
                          row(2, 0x1000, Context), row(3, 0x1000, Ordinary)},
                         {});
  EXPECT_EQ(Plan.Batches.size(), 4U);
}

TEST(SwiftABIProjectionPlan, ByValueCopyNeverCoalescesWithSourcePointer) {
  for (unsigned Count : {6U, 16U}) {
    const auto Record =
        NdType::makeStruct(std::vector<TypeRef>(Count, NdType::makeFloat(8)));
    SourceFunctionTypeHint Value;
    Value.ReturnType = NdType::makeVoid();
    Value.Parameters.push_back({"value", Record});
    std::string Error;
    ASSERT_TRUE(assignDarwinFixedSourceABI(Value, Arch::AArch64, Error));
    auto Pointer = Value;
    Pointer.Parameters[0].Type = NdType::makePtr(Record);
    ASSERT_TRUE(assignDarwinFixedSourceABI(Pointer, Arch::AArch64, Error));
    EXPECT_FALSE(equalSourceABIs(Value, Pointer));
    const auto Plan = planABIProjections(
        {row(0, 0x1000, Value), row(1, 0x1000, Pointer)}, {});
    EXPECT_EQ(Plan.Batches.size(), 2U);
    auto Missing = Value;
    Missing.Parameters[0].IndirectByValue = false;
    EXPECT_THROW(planABIProjections({row(0, 0x1000, Missing)}, {}),
                 std::invalid_argument);
    auto Spurious = Pointer;
    Spurious.Parameters[0].IndirectByValue = true;
    EXPECT_THROW(planABIProjections({row(0, 0x1000, Spurious)}, {}),
                 std::invalid_argument);
  }
}

TEST(SwiftABIProjectionPlan,
     InvalidOrDuplicateIdentitiesCannotStackInOneBatch) {
  auto Hint = signature();
  auto A = row(0, 0x1000, Hint);
  auto DuplicateIndex = row(0, 0x2000, Hint);
  auto DuplicateIdentity = A;
  DuplicateIdentity.Index = 1;
  EXPECT_THROW(planABIProjections({A, DuplicateIndex}, {}),
               std::invalid_argument);
  EXPECT_THROW(planABIProjections({A, DuplicateIdentity}, {}),
               std::invalid_argument);
  auto Invalid = A;
  Invalid.Hint.HasExplicitABI = false;
  EXPECT_THROW(planABIProjections({Invalid}, {}), std::invalid_argument);
  Invalid = A;
  Invalid.Hint.Parameters[0].Location.ValueBytes = 4;
  EXPECT_THROW(planABIProjections({Invalid}, {}), std::invalid_argument);
  auto Aggregate = std::make_shared<NdType>();
  Aggregate->Kind = NdTypeKind::Array;
  Aggregate->Size = 8;
  Aggregate->ArrayCount = 2;
  Aggregate->ElemType = NdType::makeInt(4);
  Invalid = A;
  Invalid.Hint.Parameters[0].Type = NdType::makePtr(Aggregate);
  EXPECT_THROW(planABIProjections({Invalid}, {}), std::invalid_argument);
}

TEST(SwiftABIProjectionPlan,
     MachOUnderscoreSpellingsDoNotCreateAnotherIdentity) {
  auto Hint = signature();
  auto A = row(0, 0x1000, Hint), Duplicate = A;
  Duplicate.Index = 1;
  Duplicate.MangledSymbol = "_" + A.MangledSymbol;
  EXPECT_THROW(planABIProjections({A, Duplicate}, {}), std::invalid_argument);
  auto Distinct = row(1, 0x1000, Hint);
  auto Plan = planABIProjections({A, Distinct}, {});
  ASSERT_EQ(Plan.Batches.size(), 1U);
  EXPECT_EQ(Plan.Batches[0].Rows, (std::vector<size_t>{0, 1}));
  EXPECT_EQ(A.MangledSymbol, "$s_identity_0");
  EXPECT_EQ(Duplicate.MangledSymbol, "_$s_identity_0");
  EXPECT_NE(sourceIdentity(0x1000, "__$s_identity_0"),
            sourceIdentity(0x1000, "$s_identity_0"));
}
