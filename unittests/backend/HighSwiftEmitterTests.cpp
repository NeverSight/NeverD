#include "gtest/gtest.h"

#include "neverd/backend/swift/HighSwiftEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/SourceCallTypeHint.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedTypePass.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Program.h"

#include <cstdlib>
#include <fstream>

using namespace neverd;
namespace {
SwiftSourceType i64() {
  return {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr};
}
SwiftSourceSignature signature() {
  SwiftSourceSignature S;
  S.Entry = 0x1000;
  S.MangledSymbol = "$s4Demo3addys5Int64VAD_ADtF";
  S.Module = "Demo";
  S.Name = "add";
  S.Labels = {"_", "_"};
  S.Parameters = {{"arg0", i64()}, {"arg1", i64()}};
  S.ReturnType = i64();
  return S;
}
ExprPtr param(int Index) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Index;
  V.SSAVer = 0;
  V.RenameTag = -1;
  return HighExpr::makeVar(V, NdType::makeInt(8, true));
}
HighFunc function() {
  HighFunc F;
  F.Entry = 0x1000;
  F.Name = "native_add";
  F.ReturnType = NdType::makeInt(8, true);
  F.Params = {{"arg0", F.ReturnType}, {"arg1", F.ReturnType}};
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeBinop(NdOp::INT_ADD, param(0), param(1));
  F.Body.push_back(Return);
  return F;
}

std::pair<HighFunc, SwiftSourceSignature> wideFunction(bool Memory) {
  auto F = function();
  auto S = signature();
  S.Name = Memory ? "wideReadCopy" : "concatSlice";
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 77;
  V.Size = 16;
  V.SSAVer = 1;
  auto Local = HighExpr::makeVar(V, NdType::makeInt(16, false));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Local;
  if (Memory) {
    S.Parameters[0].Type = {SwiftSourceType::Kind::Pointer,
                            "UnsafeMutableRawPointer", 0, false, nullptr};
    F.Params[0].Type = NdType::makePtr(NdType::makeVoid());
    auto Pointer = param(0);
    Pointer->Type = F.Params[0].Type;
    Assign.Val = HighExpr::makeLoad(Pointer, Local->Type);
    HighStmt Store;
    Store.Kind = StmtKind::Store;
    Store.StoreVal = Local;
    Store.StoreAddr =
        HighExpr::makeBinop(NdOp::INT_ADD, Pointer, HighExpr::makeConst(16, 8));
    F.Body.insert(F.Body.begin(), Store);
  } else {
    Assign.Val = HighExpr::makeBinop(NdOp::CONCAT, param(0), param(1));
    Assign.Val->Type = Local->Type;
  }
  F.Body.back().RetVal = HighExpr::makeBinop(
      NdOp::SUBBYTES, Local, HighExpr::makeConst(Memory ? 7 : 4, 8));
  F.Body.back().RetVal->Type = F.ReturnType;
  F.Body.insert(F.Body.begin(), Assign);
  return {F, S};
}
} // namespace

TEST(HighSwiftEmitter, ScalarBodyPreservesWrappingMachineArithmetic) {
  auto Result = HighSwiftEmitter().emit(function(), signature());
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find(
                "public func `add`(_ arg0: Swift.Int64, _ arg1: Swift.Int64)"),
            std::string::npos);
  EXPECT_NE(Result.Source.find(
                "return Swift.Int64(truncatingIfNeeded: (nd_arg0 &+ nd_arg1))"),
            std::string::npos);
  EXPECT_EQ(Result.Source.find("@_silgen_name"), std::string::npos);
}

TEST(HighSwiftEmitter, UnusedDeclaredParametersKeepTheirPositions) {
  auto F = function();
  F.Body[0].RetVal = param(1);
  auto Result = HighSwiftEmitter().emit(F, signature());
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("nd_arg1"), std::string::npos);
  EXPECT_NE(Result.Source.find("_ arg0: Swift.Int64, _ arg1: Swift.Int64"),
            std::string::npos);
}

TEST(HighSwiftEmitter, UnboundIncomingRegisterIsNotInventedAsZero) {
  auto F = function();
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = 19;
  V.SSAVer = 0;
  F.Body[0].RetVal = HighExpr::makeVar(V, NdType::makeInt(8));
  auto Result = HighSwiftEmitter().emit(F, signature());
  EXPECT_FALSE(Result.Recovered);
  EXPECT_TRUE(Result.Source.empty());
  EXPECT_NE(Result.Reason.find("unbound register"), std::string::npos);
}

TEST(HighSwiftEmitter, UndecodedExpressionAndDynamicCallAreExplicitFailures) {
  for (auto E :
       {HighExpr::makeUndef(8), HighExpr::makeCall("unbound", 0x2000, {})}) {
    auto F = function();
    F.Body[0].RetVal = E;
    auto Result = HighSwiftEmitter().emit(F, signature());
    EXPECT_FALSE(Result.Recovered);
    EXPECT_TRUE(Result.Source.empty());
  }
}

TEST(HighSwiftEmitter, SignedComparisonUsesSignedBitInterpretation) {
  auto F = function();
  auto Compare = HighExpr::makeBinop(NdOp::INT_SLESS, param(0), param(1));
  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Type = F.ReturnType;
  Select->Operands = {Compare, param(0), param(1)};
  F.Body[0].RetVal = Select;
  auto Result = HighSwiftEmitter().emit(F, signature());
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find(
                "Swift.Int64(bitPattern: Swift.UInt64(truncatingIfNeeded: "
                "nd_arg0)) < Swift.Int64(bitPattern: "
                "Swift.UInt64(truncatingIfNeeded: nd_arg1))"),
            std::string::npos);
}

TEST(HighSwiftEmitter, ClassBodyUsesOnlyProvenStoredProperties) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "class";
  S.ContextName = "Calculator";
  F.Params.push_back({"swift_self", NdType::makePtr(NdType::makeVoid())});
  auto Self = param(2);
  Self->Type = F.Params.back().Type;
  auto Address =
      HighExpr::makeBinop(NdOp::INT_ADD, Self, HighExpr::makeConst(16, 8));
  F.Body[0].RetVal = HighExpr::makeLoad(Address, F.ReturnType);
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"bias", i64(), 16, false}};
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("extension `Calculator`"), std::string::npos);
  EXPECT_EQ(Result.Source.find("Unmanaged"), std::string::npos);
  EXPECT_NE(Result.Source.find("self.`bias`"), std::string::npos);
  EXPECT_EQ(Result.Source.find("loadUnaligned"), std::string::npos);
  S.ContextLayoutKnown = false;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  S.ContextLayoutKnown = true;
  S.ContextFields[0].Offset = 24;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, ClassInitializerUsesActualStoresAndReturnsMemberSource) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "class";
  S.ContextName = "Calculator";
  S.DeclarationKind = "initializer";
  S.Name = "init";
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"bias", i64(), 16, false}};
  S.ReturnType = {SwiftSourceType::Kind::Pointer, "UnsafeMutableRawPointer", 0,
                  false, nullptr};
  F.ReturnType = NdType::makePtr(NdType::makeVoid());
  F.Params.push_back({"swift_self", F.ReturnType});
  auto Self = param(2);
  Self->Type = F.ReturnType;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr =
      HighExpr::makeBinop(NdOp::INT_ADD, Self, HighExpr::makeConst(16, 8));
  Store.StoreVal = HighExpr::makeBinop(NdOp::INT_ADD, param(0), param(1));
  F.Body[0].RetVal = Self;
  F.Body.insert(F.Body.begin(), Store);
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.MemberSource.find(
                "public init(_ arg0: Swift.Int64, _ arg1: Swift.Int64)"),
            std::string::npos);
  EXPECT_NE(
      Result.MemberSource.find("self.`bias` = Swift.Int64(truncatingIfNeeded: "
                               "(nd_arg0 &+ nd_arg1))"),
      std::string::npos);
  EXPECT_EQ(Result.MemberSource.find("Unmanaged"), std::string::npos);
  EXPECT_EQ(Result.Source, Result.MemberSource);
  F.Body.insert(F.Body.begin(), Store);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  F.Body.erase(F.Body.begin(), F.Body.begin() + 2);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, StructInitializerUsesCompleteNativeReturnValue) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "struct";
  S.ContextName = "Counter";
  S.DeclarationKind = "initializer";
  S.Name = "init";
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"value", i64(), 0, true}};
  S.ReturnsContextValue = true;
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find(
                "public init(_ arg0: Swift.Int64, _ arg1: Swift.Int64)"),
            std::string::npos);
  EXPECT_NE(Result.Source.find("self.`value` = Swift.Int64(truncatingIfNeeded: "
                               "(nd_arg0 &+ nd_arg1))"),
            std::string::npos);
  EXPECT_EQ(Result.Source.find("Unmanaged"), std::string::npos);
  EXPECT_EQ(Result.Source, Result.MemberSource);
  S.ReturnsContextValue = false;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  S.ReturnsContextValue = true;
  S.ContextFields[0].Offset = 8;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  S.ContextFields[0].Offset = 0;
  F.Body[0].RetVal.reset();
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, NarrowSignedFieldLoadRemainsItsExactMachineWidth) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "class";
  S.ContextName = "Small";
  S.ContextLayoutKnown = true;
  S.ContextFields = {
      {"value",
       {SwiftSourceType::Kind::Integer, "Int32", 32, true, nullptr},
       16,
       true}};
  F.Params.push_back({"swift_self", NdType::makePtr(NdType::makeVoid())});
  auto Self = param(2);
  Self->Type = F.Params.back().Type;
  auto Load = HighExpr::makeLoad(
      HighExpr::makeBinop(NdOp::INT_ADD, Self, HighExpr::makeConst(16, 8)),
      NdType::makeInt(4, false));
  F.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_EQUAL, Load,
                                         HighExpr::makeConst(0xffffffff, 4));
  S.ReturnType = {SwiftSourceType::Kind::Boolean, "Bool", 1, false, nullptr};
  F.ReturnType = NdType::makeInt(1, false);
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(
      Result.Source.find("Swift.UInt64(Swift.UInt32(truncatingIfNeeded: "
                         "Swift.UInt64(truncatingIfNeeded: self.`value`)))"),
      std::string::npos);
}

TEST(HighSwiftEmitter, InitializerRequiresEveryPathAndRejectsReadBeforeStore) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "class";
  S.ContextName = "Calculator";
  S.DeclarationKind = "initializer";
  S.Name = "init";
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"bias", i64(), 16, false}};
  S.ReturnType = {SwiftSourceType::Kind::Pointer, "UnsafeMutableRawPointer", 0,
                  false, nullptr};
  F.ReturnType = NdType::makePtr(NdType::makeVoid());
  F.Params.push_back({"swift_self", F.ReturnType});
  auto Self = param(2);
  Self->Type = F.ReturnType;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr =
      HighExpr::makeBinop(NdOp::INT_ADD, Self, HighExpr::makeConst(16, 8));
  Store.StoreVal = param(0);
  HighStmt Branch;
  Branch.Kind = StmtKind::IfElse;
  Branch.Cond = param(1);
  Branch.Body = {Store};
  F.Body[0].RetVal = Self;
  F.Body.insert(F.Body.begin(), Branch);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  F.Body[0].ElseBody = {Store};
  EXPECT_TRUE(HighSwiftEmitter().emit(F, S).Recovered);
  F.Body[0].Body[0].StoreVal =
      HighExpr::makeLoad(Store.StoreAddr, NdType::makeInt(8, true));
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, StructInstanceLayoutIsNotGuessed) {
  auto S = signature();
  S.ContextKind = "struct";
  S.ContextName = "Counter";
  auto Result = HighSwiftEmitter().emit(function(), S);
  EXPECT_FALSE(Result.Recovered);
  EXPECT_TRUE(Result.Source.empty());
  EXPECT_NE(Result.Reason.find("layout"), std::string::npos);
}

TEST(HighSwiftEmitter, ProvenStructValueSelfUsesDeclaredFieldValue) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "struct";
  S.ContextName = "Counter";
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"value", i64(), 0, true}};
  S.SelfConvention = "direct-fields";
  S.IsMutatingKnown = true;
  F.Params.push_back({"swift_self_0", NdType::makeInt(8, true)});
  F.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_ADD, param(0), param(2));
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(
      Result.Source.find("nd_arg2: Swift.UInt64 = "
                         "Swift.UInt64(truncatingIfNeeded: self.`value`)"),
      std::string::npos);
  EXPECT_EQ(Result.Source.find("Unmanaged"), std::string::npos);
  S.IsMutating = true;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, ProvenMutatingStructSelfWritesDeclaredField) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "struct";
  S.ContextName = "Counter";
  S.ContextLayoutKnown = true;
  S.ContextFields = {{"value", i64(), 0, true}};
  S.SelfConvention = "indirect-mutating";
  S.IsMutatingKnown = S.IsMutating = true;
  S.ReturnType = {};
  F.ReturnType = NdType::makeVoid();
  F.Params.push_back({"swift_self", NdType::makePtr(NdType::makeVoid())});
  auto Self = param(2);
  Self->Type = F.Params.back().Type;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Self;
  Store.StoreVal = HighExpr::makeBinop(
      NdOp::INT_ADD, HighExpr::makeLoad(Self, NdType::makeInt(8, true)),
      param(0));
  F.Body[0].RetVal.reset();
  F.Body.insert(F.Body.begin(), Store);
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("public mutating func `add`"),
            std::string::npos);
  EXPECT_NE(
      Result.Source.find("self.`value` = Swift.Int64(truncatingIfNeeded:"),
      std::string::npos);
  EXPECT_EQ(Result.Source.find("storeBytes"), std::string::npos);
}

TEST(HighSwiftEmitter, InvalidSourceSignatureAndEmptyBodyCannotBeRecovered) {
  auto F = function();
  auto S = signature();
  S.Parameters[0].Name = "arg1";
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  S = signature();
  S.Entry++;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  S = signature();
  F.Body.clear();
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, ConditionalDefinitionDoesNotLeakPastMissingBranch) {
  auto F = function();
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 50;
  V.SSAVer = 1;
  auto Local = HighExpr::makeVar(V, NdType::makeInt(8));
  HighStmt Assign;
  Assign.Kind = StmtKind::Assign;
  Assign.Dst = Local;
  Assign.Val = param(0);
  HighStmt If;
  If.Kind = StmtKind::If;
  If.Cond = param(1);
  If.Body = {Assign};
  F.Body[0].RetVal = Local;
  F.Body.insert(F.Body.begin(), If);
  auto Result = HighSwiftEmitter().emit(F, signature());
  EXPECT_FALSE(Result.Recovered);
  EXPECT_NE(Result.Reason.find("undefined local"), std::string::npos);
}

TEST(HighSwiftEmitter, DirectCallBindsTypesLabelsAndRecordsBodyDependency) {
  auto F = function();
  auto Target = signature();
  Target.Entry = 0x2000;
  Target.Name = "target";
  Target.Labels = {"x", "y"};
  F.Body[0].RetVal =
      HighExpr::makeCall("machine_name", 0x2000, {param(0), param(1)});
  F.Body[0].RetVal->Type = NdType::makeInt(8, true);
  auto Result = HighSwiftEmitter().emit(F, signature(), {Target});
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_EQ(Result.Dependencies, std::vector<va_t>({0x2000}));
  EXPECT_NE(Result.Source.find(
                "`target`(`x`: Swift.Int64(truncatingIfNeeded: nd_arg0), `y`: "
                "Swift.Int64(truncatingIfNeeded: nd_arg1))"),
            std::string::npos);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, signature()).Recovered);
}

TEST(HighSwiftEmitter, SameClassDirectCallRequiresItsExactBoundReceiver) {
  auto F = function();
  auto S = signature();
  S.ContextKind = "class";
  S.ContextName = "Calculator";
  F.Params.push_back({"swift_self", NdType::makePtr(NdType::makeVoid())});
  auto Self = param(2);
  Self->Type = F.Params.back().Type;
  auto Target = S;
  Target.Entry = 0x2000;
  Target.Name = "leaf";
  F.Body[0].RetVal = HighExpr::makeCall("native_leaf", Target.Entry,
                                        {param(0), param(1), Self});
  F.Body[0].RetVal->Type = F.ReturnType;
  auto Result = HighSwiftEmitter().emit(F, S, {Target});
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("self.`leaf`("), std::string::npos);
  EXPECT_EQ(Result.Dependencies, std::vector<va_t>({Target.Entry}));
  F.Body[0].RetVal->Operands.back() = HighExpr::makeConst(0x1234, 8);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S, {Target}).Recovered);
}

TEST(HighSwiftEmitter,
     BoundCallsInterpretMachineBitsUsingTheirExactDeclaration) {
  auto F = function();
  auto Target = signature();
  Target.Entry = 0x2000;
  Target.Name = "target";
  auto Call =
      HighExpr::makeCall("machine_target", Target.Entry, {param(0), param(1)});
  Call->Type = NdType::makeInt(8, false);
  F.Body[0].RetVal = Call;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, signature(), {Target}).Recovered);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->TargetAddress = Target.Entry;
  Binding->Signature.ReturnType = NdType::makeInt(8, true);
  Binding->Signature.Parameters = {{"arg0", NdType::makeInt(8, true), {}},
                                   {"arg1", NdType::makeInt(8, true), {}}};
  std::string Diagnostic;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Binding->Signature, Arch::AArch64,
                                          Diagnostic));
  Call->SourceCallHint = Binding;
  auto Result = HighSwiftEmitter().emit(F, signature(), {Target});
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("Swift.UInt64(truncatingIfNeeded: `target`("),
            std::string::npos);
  Call->Type = NdType::makeInt(4, false);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, signature(), {Target}).Recovered);
  Call->Type = NdType::makeInt(8, false);
  Binding->Signature.ReturnType = NdType::makeInt(8, false);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, signature(), {Target}).Recovered);
  Binding->Signature.ReturnType = NdType::makeInt(8, true);
  Binding->TargetAddress++;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, signature(), {Target}).Recovered);
}

TEST(HighSwiftEmitter, FloatBitPatternsAreReinterpretedBeforeArithmetic) {
  auto F = function();
  auto S = signature();
  SwiftSourceType Float{SwiftSourceType::Kind::Floating, "Float", 32, false,
                        nullptr};
  S.ReturnType = Float;
  for (auto &P : S.Parameters)
    P.Type = Float;
  F.ReturnType = NdType::makeFloat(4);
  for (auto &P : F.Params)
    P.Type = F.ReturnType;
  auto A = param(0), B = param(1);
  A->Type = F.ReturnType;
  B->Type = F.ReturnType;
  auto Bits = std::make_shared<HighExpr>();
  Bits->Kind = ExprKind::BitCast;
  Bits->Type = NdType::makeInt(4, false);
  Bits->Operands = {A};
  auto Reinterpreted = std::make_shared<HighExpr>();
  Reinterpreted->Kind = ExprKind::BitCast;
  Reinterpreted->Type = F.ReturnType;
  Reinterpreted->Operands = {Bits};
  F.Body[0].RetVal = HighExpr::makeBinop(NdOp::FLOAT_ADD, Reinterpreted, B);
  F.Body[0].RetVal->Type = F.ReturnType;
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("Swift.UInt64((arg0).bitPattern)"),
            std::string::npos);
  EXPECT_NE(Result.Source.find(
                "Swift.Float(bitPattern: Swift.UInt32(truncatingIfNeeded:"),
            std::string::npos);
  EXPECT_EQ(Result.Source.find("Swift.Float(nd_arg0)"), std::string::npos);
  Bits->Type = NdType::makeInt(8, false);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
}

TEST(HighSwiftEmitter, StandardLibraryModuleCannotBeShadowed) {
  auto S = signature();
  S.Name = "Swift";
  EXPECT_FALSE(HighSwiftEmitter().emit(function(), S).Recovered);
  S = signature();
  S.ContextKind = "struct";
  S.ContextName = "Swift";
  S.IsStatic = true;
  EXPECT_FALSE(HighSwiftEmitter().emit(function(), S).Recovered);
  S = signature();
  S.Labels[0] = "Swift";
  EXPECT_FALSE(HighSwiftEmitter().emit(function(), S).Recovered);
}

TEST(HighSwiftEmitter, WideContainersRequireExplicitExactLaneOperations) {
  auto [F, S] = wideFunction(false);
  auto Result = HighSwiftEmitter().emit(F, S);
  ASSERT_TRUE(Result.Recovered) << Result.Reason;
  EXPECT_NE(Result.Source.find("struct nd_word128_0"), std::string::npos);
  EXPECT_NE(Result.Source.find(").slice(4)"), std::string::npos);
  EXPECT_EQ(Result.Source.find("Swift.UInt128"), std::string::npos);
  F.Body[0].Val->Op = NdOp::INT_ADD;
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  F.Body[0].Val->Op = NdOp::CONCAT;
  F.Body.back().RetVal->Operands[1] = HighExpr::makeConst(9, 8);
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  F.Body.back().RetVal->Operands[1] = HighExpr::makeConst(4, 8);
  F.Body.erase(F.Body.begin());
  EXPECT_FALSE(HighSwiftEmitter().emit(F, S).Recovered);
  auto [LoadF, LoadS] = wideFunction(true);
  EXPECT_TRUE(HighSwiftEmitter().emit(LoadF, LoadS).Recovered);
  LoadF.Body[0].Val->MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  EXPECT_FALSE(HighSwiftEmitter().emit(LoadF, LoadS).Recovered);
  LoadF.Body[0].Val->Operands[0].reset();
  EXPECT_FALSE(HighSwiftEmitter().emit(LoadF, LoadS).Recovered);
}

TEST(HighSwiftEmitter, BoundVoidReturnPreservesNativeStoresAndCalls) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    for (bool WithCall : {false, true}) {
      const auto &Registers = getTargetRegInfo(Architecture);
      MedFunc Med;
      Med.Name = "write";
      Med.Entry = 0x1000;
      SourceFunctionTypeHint Hint;
      Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
      Hint.ReturnType = NdType::makeVoid();
      Hint.Parameters = {
          {"arg0", NdType::makePtr(NdType::makeInt(4, true)), {}}};
      std::string Diagnostic;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Diagnostic))
          << Diagnostic;
      Med.SourceTypeHint = Hint;
      MedVar Address;
      Address.Kind = MedVar::Reg;
      Address.TheArch = Architecture;
      Address.Id = 1;
      Address.SSAVer = 0;
      Address.RegOff = Registers.IntParamRegs[0];
      Address.Size = 8;
      MedBlock Block;
      Block.Id = 0;
      Block.StartAddr = Med.Entry;
      MedOp LiveIn;
      LiveIn.Opcode = NdOp::COPY;
      LiveIn.Output = Address;
      LiveIn.addInput(Address);
      Block.Ops.push_back(LiveIn);
      MedOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(Address);
      Store.addInput(MedVar::makeConst(37, 4));
      Block.Ops.push_back(Store);
      MedVar Result = Address;
      if (WithCall) {
        auto Binding = std::make_shared<SourceCallTypeHint>();
        Binding->TargetAddress = 0x2000;
        Binding->Signature.ReturnType = NdType::makeInt(8, true);
        ASSERT_TRUE(assignDarwinScalarSourceABI(Binding->Signature,
                                                Architecture, Diagnostic));
        MedOp Call;
        Call.Opcode = NdOp::CALL;
        Call.addInput(MedVar::makeConst(0x2000, 8));
        Call.SourceCallHint = Binding;
        Result.Id = 2;
        Result.SSAVer = 1;
        Result.RegOff = Registers.IntReturnReg;
        Call.Output = Result;
        Block.Ops.push_back(Call);
      }
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Result);
      Block.Ops.push_back(Return);
      Med.Blocks.push_back(Block);
      inferMedTypes(Med, Architecture);
      ASSERT_TRUE(Med.SourceParametersBound);
      ASSERT_TRUE(Med.SourceTypeHint);
      const auto High = MedToHighConverter().convert(Med, Architecture);
      ASSERT_EQ(High.ReturnType->Kind, NdTypeKind::Void);
      unsigned Returns = 0, Stores = 0, Calls = 0;
      auto Body = High.Body;
      walkStmts(Body, [&](HighStmt &Statement) {
        if (Statement.Kind == StmtKind::Return) {
          ++Returns;
          EXPECT_FALSE(Statement.RetVal);
        }
        if (Statement.Kind == StmtKind::Store) {
          ++Stores;
          ASSERT_TRUE(Statement.StoreVal);
          EXPECT_EQ(Statement.StoreVal->ConstVal, 37u);
        }
        forEachExpr(Statement, [&](ExprPtr &Root) {
          std::vector<ExprPtr> Work{Root};
          while (!Work.empty()) {
            auto E = Work.back();
            Work.pop_back();
            if (!E)
              continue;
            if (E->Kind == ExprKind::Call) {
              ++Calls;
              EXPECT_EQ(E->CallAddr, 0x2000u);
            }
            Work.insert(Work.end(), E->Operands.begin(), E->Operands.end());
          }
        });
      });
      EXPECT_EQ(Returns, 1u);
      EXPECT_EQ(Stores, 1u);
      EXPECT_EQ(Calls, unsigned(WithCall));
    }
  }
}

TEST(HighSwiftEmitter,
     QualifiedStandardLibraryCompilesAndRunsWithShadowingDeclarations) {
  auto Compiler = llvm::sys::findProgramByName("swiftc");
  if (!Compiler)
    GTEST_SKIP() << "Swift source execution requires a local swiftc toolchain";
  std::string Source =
      "public struct UInt64 {}\npublic struct Int64 {}\n"
      "public struct Float {}\npublic struct Double {}\npublic struct UInt32 "
      "{}\n"
      "public struct UInt {}\npublic struct Bool {}\npublic struct Void {}\n"
      "public struct UnsafeRawPointer {}\npublic struct "
      "UnsafeMutableRawPointer {}\n"
      "public struct UnsafePointer {}\npublic struct UnsafeMutablePointer {}\n"
      "public struct unsafeBitCast {}\npublic final class Host {}\n";
  auto Append = [&](const HighFunc &F, const SwiftSourceSignature &S) {
    auto Result = HighSwiftEmitter().emit(F, S);
    EXPECT_TRUE(Result.Recovered) << Result.Reason;
    Source += Result.Source;
  };
  auto F = function();
  auto S = signature();
  S.ContextKind = "struct";
  S.ContextName = "UInt64";
  S.IsStatic = true;
  Append(F, S);
  S.ContextKind = "class";
  S.ContextName = "Host";
  F.Params.push_back({"swift_self", NdType::makePtr(NdType::makeVoid())});
  Append(F, S);
  for (unsigned Bytes : {4u, 8u}) {
    F = function();
    S = signature();
    S.Name = Bytes == 4 ? "floatAdd" : "doubleAdd";
    SwiftSourceType T{SwiftSourceType::Kind::Floating,
                      Bytes == 4 ? "Float" : "Double", Bytes * 8, false,
                      nullptr};
    S.ReturnType = T;
    for (auto &P : S.Parameters)
      P.Type = T;
    F.ReturnType = NdType::makeFloat(Bytes);
    for (auto &P : F.Params)
      P.Type = F.ReturnType;
    auto A = param(0), B = param(1);
    A->Type = B->Type = F.ReturnType;
    F.Body[0].RetVal = HighExpr::makeBinop(NdOp::FLOAT_ADD, A, B);
    F.Body[0].RetVal->Type = F.ReturnType;
    Append(F, S);
  }
  F = function();
  S = signature();
  S.Name = "writeRead";
  S.Parameters[0].Type = {SwiftSourceType::Kind::Pointer,
                          "UnsafeMutableRawPointer", 0, false, nullptr};
  F.Params[0].Type = NdType::makePtr(NdType::makeVoid());
  auto Pointer = param(0);
  Pointer->Type = F.Params[0].Type;
  HighStmt Store;
  Store.Kind = StmtKind::Store;
  Store.StoreAddr = Pointer;
  Store.StoreVal = param(1);
  F.Body[0].RetVal = HighExpr::makeLoad(Pointer, F.ReturnType);
  F.Body.insert(F.Body.begin(), Store);
  Append(F, S);
  F = function();
  S = signature();
  S.Name = "typedIdentity";
  S.Parameters.resize(1);
  S.Labels.resize(1);
  F.Params.resize(1);
  S.ReturnType = S.Parameters[0].Type = {
      SwiftSourceType::Kind::Pointer, "UnsafeMutablePointer", 0, false,
      std::make_shared<SwiftSourceType>(i64())};
  F.ReturnType = F.Params[0].Type = NdType::makePtr(NdType::makeInt(8, true));
  F.Body[0].RetVal = param(0);
  F.Body[0].RetVal->Type = F.ReturnType;
  Append(F, S);
  F = function();
  S = signature();
  S.Name = "less";
  S.ReturnType = {SwiftSourceType::Kind::Boolean, "Bool", 1, false, nullptr};
  F.ReturnType = NdType::makeInt(1, false);
  F.Body[0].RetVal = HighExpr::makeBinop(NdOp::INT_SLESS, param(0), param(1));
  Append(F, S);
  for (bool Memory : {false, true}) {
    auto [Wide, Signature] = wideFunction(Memory);
    Append(Wide, Signature);
  }
  Source += R"swift(
Swift.precondition(UInt64.add(Swift.Int64.max, 1) == Swift.Int64.min)
Swift.precondition(Host.add(3, 4) == 7)
Swift.precondition(floatAdd(1.5, -2.25) == -0.75)
Swift.precondition(doubleAdd(1.5, -2.25) == -0.75)
Swift.precondition(less(-1, 0) && !less(0, -1))
var word: Swift.Int64 = 0
Swift.withUnsafeMutablePointer(to: &word) { pointer in
    Swift.precondition(writeRead(Swift.UnsafeMutableRawPointer(pointer), -37) == -37)
    Swift.precondition(typedIdentity(pointer) == pointer)
}
Swift.precondition(word == -37)
Swift.precondition(concatSlice(Swift.Int64(bitPattern: 0x8877665544332211), Swift.Int64(bitPattern: 0xffeeddccbbaa0099)) == 0x44332211ffeeddcc)
var bytes = (0..<32).map { Swift.UInt8($0) }
bytes.withUnsafeMutableBytes { memory in
    Swift.precondition(wideReadCopy(memory.baseAddress!, 0) == 0x0e0d0c0b0a090807)
}
Swift.precondition(Swift.Array(bytes[16..<32]) == Swift.Array(bytes[0..<16]))
)swift";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-swift-shadowing",
                                                    Directory));
  struct Cleanup {
    std::string Path;
    ~Cleanup() { (void)llvm::sys::fs::remove_directories(Path); }
  } Cleanup{Directory.str().str()};
  const std::string Path = Directory.str().str() + "/main.swift";
  const std::string Executable = Directory.str().str() + "/verify.exe";
  const std::string CompilerLog = Directory.str().str() + "/compiler.log";
  const char *ConfiguredCache = std::getenv("NEVERD_SWIFT_MODULE_CACHE");
  const std::string Cache = ConfiguredCache
                                ? ConfiguredCache
                                : Directory.str().str() + "/module-cache";
  {
    std::ofstream Output(Path);
    ASSERT_TRUE(Output);
    Output << Source;
  }
  std::string Error;
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, CompilerLog};
  const int CompileStatus = llvm::sys::ExecuteAndWait(
      *Compiler,
      {*Compiler, "-module-cache-path", Cache, Path, "-o", Executable},
      std::nullopt, Redirects, 300, 0, &Error);
  std::ifstream Log(CompilerLog);
  const std::string Diagnostics((std::istreambuf_iterator<char>(Log)),
                                std::istreambuf_iterator<char>());
  ASSERT_EQ(CompileStatus, 0) << Error << "\n" << Diagnostics << "\n" << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      {}, 30, 0, &Error),
            0)
      << Error;
}

namespace {
ExprPtr flowLocal() {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = 811;
  V.Size = 8;
  V.SSAVer = 1;
  return HighExpr::makeVar(V, NdType::makeInt(8));
}
HighStmt flowAssign() {
  HighStmt A;
  A.Kind = StmtKind::Assign;
  A.Dst = flowLocal();
  A.Val = HighExpr::makeBinop(NdOp::INT_ADD, param(0), param(1));
  return A;
}
HighStmt flowExit(StmtKind Kind) {
  HighStmt S;
  S.Kind = Kind;
  return S;
}
HighFunc flowFunction(HighStmt Loop) {
  auto F = function();
  F.Body[0].RetVal = flowLocal();
  F.Body.insert(F.Body.begin(), std::move(Loop));
  return F;
}
HighStmt flowLoop(StmtKind Kind = StmtKind::While) {
  HighStmt L;
  L.Kind = Kind;
  L.Cond = HighExpr::makeConst(1, 1);
  L.Body = {flowAssign(), flowExit(StmtKind::Break)};
  return L;
}
} // namespace

TEST(HighSwiftEmitter, AlwaysEnteredLoopExportsDefinitionsFromEveryNormalExit) {
  for (auto Kind : {StmtKind::While, StmtKind::DoWhile}) {
    auto L = flowLoop(Kind);
    auto R = HighSwiftEmitter().emit(flowFunction(L), signature());
    ASSERT_TRUE(R.Recovered) << R.Reason;
  }
  auto L = flowLoop(StmtKind::DoWhile);
  L.Cond = HighExpr::makeConst(0, 1);
  L.Body = {flowAssign()};
  EXPECT_TRUE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
}
TEST(HighSwiftEmitter, ZeroTripAndMissingBreakAssignmentsRemainUnrecovered) {
  auto L = flowLoop();
  L.Cond = param(0);
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  L.Cond = HighExpr::makeConst(256, 1); // Truncated machine condition is zero.
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  L = flowLoop();
  HighStmt B;
  B.Kind = StmtKind::If;
  B.Cond = param(0);
  B.Body = {flowExit(StmtKind::Break)};
  L.Body.insert(L.Body.begin(), B);
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  L = flowLoop();
  L.Body = {flowExit(StmtKind::Break), flowAssign()};
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
}
TEST(HighSwiftEmitter, NestedBreaksBelongToTheirOwnLoopOrSwitch) {
  auto L = flowLoop();
  auto Inner = flowLoop();
  Inner.Body = {flowExit(StmtKind::Break)};
  L.Body.insert(L.Body.begin(), Inner);
  EXPECT_TRUE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = param(1);
  SwitchCase Case;
  Case.Value = 1;
  Case.Body = {flowExit(StmtKind::Break)};
  Switch.Cases = {Case};
  Switch.DefaultBody = {flowExit(StmtKind::Break)};
  L = flowLoop();
  L.Body.insert(L.Body.begin(), Switch);
  EXPECT_TRUE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  // An inner loop whose own early break lacks a definition cannot seed the
  // outer loop merely because another inner exit writes the value.
  HighStmt Early;
  Early.Kind = StmtKind::If;
  Early.Cond = param(0);
  Early.Body = {flowExit(StmtKind::Break)};
  Inner = flowLoop();
  Inner.Body.insert(Inner.Body.begin(), Early);
  L = flowLoop();
  L.Body = {Inner, flowExit(StmtKind::Break)};
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
}
TEST(HighSwiftEmitter, ReturnAndContinuePathsAreDistinctFromNormalLoopExits) {
  for (auto Exit : {StmtKind::Return, StmtKind::Continue}) {
    auto L = flowLoop();
    HighStmt Branch;
    Branch.Kind = StmtKind::If;
    Branch.Cond = param(0);
    auto Terminal = flowExit(Exit);
    if (Exit == StmtKind::Return)
      Terminal.RetVal = param(1);
    Branch.Body = {Terminal};
    L.Body.insert(L.Body.begin(), Branch);
    EXPECT_TRUE(
        HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
  }
  // repeat/while evaluates its exit condition after a continue, so the
  // continue path may exit without executing the later assignment.
  auto L = flowLoop(StmtKind::DoWhile);
  L.Cond = param(1);
  L.Body = {flowAssign()};
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = param(0);
  Branch.Body = {flowExit(StmtKind::Continue)};
  L.Body.insert(L.Body.begin(), Branch);
  EXPECT_FALSE(HighSwiftEmitter().emit(flowFunction(L), signature()).Recovered);
}

TEST(HighSwiftEmitter, NonVoidReturnBranchesMustCoverTheNormalFallthroughExit) {
  auto F = function();
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond = param(0);
  Branch.Body = F.Body;
  F.Body = {Branch};
  auto Missing = HighSwiftEmitter().emit(F, signature());
  EXPECT_FALSE(Missing.Recovered);
  EXPECT_NE(Missing.Reason.find("fallthrough"), std::string::npos);
  F.Body[0].ElseBody = Branch.Body;
  EXPECT_TRUE(HighSwiftEmitter().emit(F, signature()).Recovered);
}

TEST(HighSwiftEmitter,
     LoopExitDefinitionsCompileAndExecuteWithoutInventedInitialValues) {
  auto Compiler = llvm::sys::findProgramByName("swiftc");
  if (!Compiler)
    GTEST_SKIP() << "Swift loop execution requires a local swiftc toolchain";
  std::string Source;
  auto Append = [&](const std::string &Name, HighStmt Loop) {
    auto S = signature();
    S.Name = Name;
    auto R = HighSwiftEmitter().emit(flowFunction(std::move(Loop)), S);
    ASSERT_TRUE(R.Recovered) << R.Reason;
    EXPECT_EQ(R.Source.find("nd_v0: Swift.UInt64 ="), std::string::npos);
    Source += R.Source;
  };
  Append("loopBreak", flowLoop());
  auto Repeat = flowLoop(StmtKind::DoWhile);
  Repeat.Cond = HighExpr::makeConst(0, 1);
  Repeat.Body = {flowAssign()};
  Append("repeatOnce", Repeat);
  auto Nested = flowLoop();
  auto Inner = flowLoop();
  Inner.Body = {flowExit(StmtKind::Break)};
  Nested.Body.insert(Nested.Body.begin(), Inner);
  Append("nestedBreak", Nested);
  auto WithReturn = flowLoop();
  HighStmt Branch;
  Branch.Kind = StmtKind::If;
  Branch.Cond =
      HighExpr::makeBinop(NdOp::INT_SLESS, param(0), HighExpr::makeConst(0, 8));
  auto Return = flowExit(StmtKind::Return);
  Return.RetVal = param(1);
  Branch.Body = {Return};
  WithReturn.Body.insert(WithReturn.Body.begin(), Branch);
  Append("returnOrBreak", WithReturn);
  auto WithContinue = flowLoop();
  Branch.Cond = param(0);
  Branch.Body = {flowExit(StmtKind::Continue)};
  WithContinue.Body.insert(WithContinue.Body.begin(), Branch);
  Append("continueOrBreak", WithContinue);
  HighStmt Switch;
  Switch.Kind = StmtKind::Switch;
  Switch.SwitchExpr = param(0);
  SwitchCase Case;
  Case.Value = 1;
  Case.Body = {flowExit(StmtKind::Break)};
  Switch.Cases = {Case};
  Switch.DefaultBody = {flowExit(StmtKind::Break)};
  auto WithSwitch = flowLoop();
  WithSwitch.Body.insert(WithSwitch.Body.begin(), Switch);
  Append("switchBreak", WithSwitch);
  auto Counter = flowLocal(), Accumulator = flowLocal();
  Counter->Var.Id = 812;
  Accumulator->Var.Id = 813;
  auto AssignValue = [&](ExprPtr Dst, ExprPtr Val) {
    HighStmt A;
    A.Kind = StmtKind::Assign;
    A.Dst = Dst;
    A.Val = Val;
    return A;
  };
  auto ContinueLoop = flowLoop();
  Branch.Cond =
      HighExpr::makeBinop(NdOp::INT_SLESS, HighExpr::makeConst(0, 8), Counter);
  Branch.Body = {flowExit(StmtKind::Continue)};
  ContinueLoop.Body = {
      AssignValue(Accumulator,
                  HighExpr::makeBinop(NdOp::INT_ADD, Accumulator, Counter)),
      AssignValue(Counter, HighExpr::makeBinop(NdOp::INT_SUB, Counter,
                                               HighExpr::makeConst(1, 8))),
      Branch,
      AssignValue(flowLocal(),
                  HighExpr::makeBinop(NdOp::INT_ADD, Accumulator, param(1))),
      flowExit(StmtKind::Break)};
  auto ContinueFunction = flowFunction(ContinueLoop);
  ContinueFunction.Body.insert(
      ContinueFunction.Body.begin(),
      AssignValue(Accumulator, HighExpr::makeConst(0, 8)));
  ContinueFunction.Body.insert(ContinueFunction.Body.begin(),
                               AssignValue(Counter, param(0)));
  auto ContinueSignature = signature();
  ContinueSignature.Name = "continueSum";
  auto Continued = HighSwiftEmitter().emit(ContinueFunction, ContinueSignature);
  ASSERT_TRUE(Continued.Recovered) << Continued.Reason;
  Source += Continued.Source;
  Source += R"swift(
for x: Swift.Int64 in [Swift.Int64.min, -7, 0, 1, Swift.Int64.max] {
  for y: Swift.Int64 in [-31, 0, 21] {
    Swift.precondition(loopBreak(x,y) == (x &+ y))
    Swift.precondition(repeatOnce(x,y) == (x &+ y))
    Swift.precondition(nestedBreak(x,y) == (x &+ y))
    Swift.precondition(switchBreak(x,y) == (x &+ y))
    Swift.precondition(returnOrBreak(x,y) == (x < 0 ? y : (x &+ y)))
    Swift.precondition(continueOrBreak(0,y) == y)
    for n: Swift.Int64 in [0, 1, 2, 7] { Swift.precondition(continueSum(n,y) == n * (n + 1) / 2 + y) }
  }
}
)swift";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-swift-loop-flow",
                                                    Directory));
  struct Cleanup {
    std::string Path;
    ~Cleanup() { (void)llvm::sys::fs::remove_directories(Path); }
  } Cleanup{Directory.str().str()};
  const auto Path = Directory.str().str() + "/main.swift",
             Executable = Directory.str().str() + "/verify.exe",
             LogPath = Directory.str().str() + "/compile.log";
  {
    std::ofstream Out(Path);
    ASSERT_TRUE(Out);
    Out << Source;
  }
  const char *Configured = std::getenv("NEVERD_SWIFT_MODULE_CACHE");
  const std::string Cache =
      Configured ? Configured : Directory.str().str() + "/module-cache";
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, LogPath};
  std::string Error;
  const int Built = llvm::sys::ExecuteAndWait(
      *Compiler,
      {*Compiler, "-module-cache-path", Cache, Path, "-o", Executable},
      std::nullopt, Redirects, 300, 0, &Error);
  std::ifstream Log(LogPath);
  const std::string Diagnostics((std::istreambuf_iterator<char>(Log)),
                                std::istreambuf_iterator<char>());
  ASSERT_EQ(Built, 0) << Error << "\n" << Diagnostics << "\n" << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      {}, 30, 0, &Error),
            0)
      << Error;
}
