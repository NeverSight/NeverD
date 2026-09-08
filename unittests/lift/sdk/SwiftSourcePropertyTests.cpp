#include "../../../lib/sdk/capi/SwiftSourceNamespace.h"
#include "../../../lib/sdk/capi/SwiftSourceProperties.h"
#include "gtest/gtest.h"

#include "neverd/backend/swift/HighSwiftEmitter.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
using namespace neverd::sdk;

namespace {
SwiftSourceType integer() {
  return {SwiftSourceType::Kind::Integer, "Int64", 64, true, nullptr};
}
ExprPtr parameter(unsigned Index, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Index;
  V.Size = Type->Size;
  V.RenameTag = -1;
  return HighExpr::makeVar(V, Type);
}
struct Fixture {
  std::vector<std::optional<SwiftSourceSignature>> Signatures;
  std::vector<HighFunc> Functions;
  explicit Fixture(bool Struct = false) {
    auto I64 = NdType::makeInt(8);
    auto Pointer = NdType::makePtr(NdType::makeVoid());
    for (unsigned Index = 0; Index < 4; ++Index) {
      SwiftSourceSignature S;
      S.Entry = 0x1000 + Index * 16;
      S.MangledSymbol = "$s_property_" + std::to_string(Index);
      S.Module = "Demo";
      S.ContextKind = Struct ? "struct" : "class";
      S.ContextName = Struct ? "ValueBox" : "ReferenceBox";
      S.ContextLayoutKnown = true;
      S.ContextFields = {{"value", integer(), Struct ? 0U : 16U, true}};
      S.Name = Index == 2 ? "init" : Index == 3 ? "nativeValue" : "value";
      S.DeclarationKind = Index == 0   ? "getter"
                          : Index == 1 ? "setter"
                          : Index == 2 ? "initializer"
                                       : "function";
      S.ReturnType = Index == 1 ? SwiftSourceType() : integer();
      S.IsMutatingKnown = true;
      S.IsMutating = Struct && Index == 1;
      S.SelfConvention =
          Struct ? (Index == 1 ? "indirect-mutating" : "direct-fields") : "";
      if (Index == 1 || Index == 2) {
        S.Parameters = {{"arg0", integer()}};
        S.Labels = {"_"};
      }
      if (Index == 2) {
        S.ReturnsContextValue = Struct;
        if (!Struct)
          S.ReturnType = {SwiftSourceType::Kind::Pointer,
                          "UnsafeMutableRawPointer", 0, false, nullptr};
      }
      HighFunc F;
      F.Entry = S.Entry;
      F.Name = "native_" + std::to_string(Index);
      F.ReturnType = Index == 1              ? NdType::makeVoid()
                     : Index == 2 && !Struct ? Pointer
                                             : I64;
      if (Index == 1 || Index == 2)
        F.Params.push_back({"arg0", I64});
      const bool Direct = Struct && (Index == 0 || Index == 3);
      if (Direct)
        F.Params.push_back({"swift_self_0", I64});
      else if (!(Struct && Index == 2))
        F.Params.push_back({"swift_self", Pointer});
      ExprPtr Address;
      if (!Direct && !(Struct && Index == 2)) {
        Address = parameter(F.Params.size() - 1, Pointer);
        if (!Struct)
          Address = HighExpr::makeBinop(NdOp::INT_ADD, Address,
                                        HighExpr::makeConst(16, 8));
      }
      if (Index == 1 || (!Struct && Index == 2)) {
        HighStmt Store;
        Store.Kind = StmtKind::Store;
        Store.StoreAddr = Address;
        Store.StoreVal = parameter(0, I64);
        if (Index == 1)
          Store.StoreVal = HighExpr::makeBinop(NdOp::INT_MULT, Store.StoreVal,
                                               HighExpr::makeConst(3, 8));
        F.Body.push_back(Store);
      }
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      if (Index != 1) {
        Return.RetVal =
            Index == 2 ? (Struct ? parameter(0, I64) : parameter(1, Pointer))
            : Direct   ? parameter(0, I64)
                       : HighExpr::makeLoad(Address, I64);
        if (Index == 0)
          Return.RetVal = HighExpr::makeBinop(NdOp::INT_ADD, Return.RetVal,
                                              HighExpr::makeConst(7, 8));
      }
      F.Body.push_back(Return);
      Signatures.push_back(S);
      Functions.push_back(F);
    }
  }
};

void executeSwift(const std::string &Source) {
  auto Compiler = llvm::sys::findProgramByName("swiftc");
  if (!Compiler)
    GTEST_SKIP()
        << "Swift property execution requires a local swiftc toolchain";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("neverd-swift-properties",
                                                    Directory));
  const std::string Path = Directory.str().str() + "/main.swift";
  const std::string Executable = Directory.str().str() + "/verify";
  const std::string Log = Directory.str().str() + "/compiler.log";
  const std::string Cache = Directory.str().str() + "/cache";
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(Path, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  std::string Error;
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, Log};
  auto Compiled = llvm::sys::ExecuteAndWait(
      *Compiler,
      {*Compiler, "-module-cache-path", Cache, Path, "-o", Executable},
      std::nullopt, Redirects, 60, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(Log);
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  EXPECT_EQ(llvm::sys::ExecuteAndWait(Executable, {Executable}, std::nullopt,
                                      Redirects, 30, 0, &Error),
            0)
      << Error;
  llvm::sys::fs::remove_directories(Directory);
}
} // namespace

TEST(SwiftSourceProperties,
     BackingNamesAreFrozenAcrossInitializerMethodsAndFailedAccessors) {
  Fixture F;
  F.Signatures[0]->UnsupportedReason = "unrecovered native getter";
  F.Signatures[3]->Name = "neverd_storage_16";
  swift_source::planPropertyStorage(F.Signatures);
  for (const auto &S : F.Signatures) {
    ASSERT_TRUE(S);
    EXPECT_EQ(S->ContextFields[0].Name, "value");
    EXPECT_EQ(S->ContextFields[0].BackingName, "neverd_storage_16_1");
  }
  swift_source::planPropertyStorage(F.Signatures);
  EXPECT_EQ(F.Signatures[1]->ContextFields[0].BackingName,
            "neverd_storage_16_1");
}

TEST(SwiftSourceProperties,
     GetterSetterPairSharesOnePropertyWithoutAStorageCollision) {
  Fixture F;
  EXPECT_EQ(swift_source::namespaceConflicts(F.Signatures),
            (std::set<size_t>{0, 1}));
  swift_source::planPropertyStorage(F.Signatures);
  EXPECT_TRUE(swift_source::namespaceConflicts(F.Signatures).empty());
  auto Bad = F.Signatures;
  Bad.push_back(Bad[0]);
  EXPECT_EQ(swift_source::namespaceConflicts(Bad), (std::set<size_t>{0, 1, 4}));
  Bad = F.Signatures;
  Bad[3]->Name = "value";
  EXPECT_EQ(swift_source::namespaceConflicts(Bad), (std::set<size_t>{0, 1, 3}));
  Bad = F.Signatures;
  Bad[1]->Parameters[0].Type = {SwiftSourceType::Kind::Floating, "Double", 64,
                                false, nullptr};
  EXPECT_EQ(swift_source::namespaceConflicts(Bad), (std::set<size_t>{0, 1}));
}

TEST(SwiftSourceProperties,
     SetterNeedsItsRecoveredGetterButGetterCanStandAlone) {
  Fixture F;
  EXPECT_TRUE(swift_source::incompleteProperties(F.Signatures, {0}).empty());
  EXPECT_TRUE(swift_source::incompleteProperties(F.Signatures, {0, 1}).empty());
  auto Missing = swift_source::incompleteProperties(F.Signatures, {1, 2, 3});
  ASSERT_EQ(Missing.size(), 1U);
  EXPECT_TRUE(Missing.count(1));
  swift_source::planPropertyStorage(F.Signatures);
  EXPECT_THROW(
      swift_source::assemblePropertyContext(
          *F.Signatures[1], {{&*F.Signatures[1], "set(arg0) { return }\n"}}),
      std::invalid_argument);
}

TEST(SwiftSourceProperties, ConflictingContextLayoutsDoNotAcquireBackingNames) {
  Fixture F;
  F.Signatures[2]->ContextFields[0].Offset = 24;
  swift_source::planPropertyStorage(F.Signatures);
  for (const auto &S : F.Signatures)
    EXPECT_FALSE(S->UnsupportedReason.empty());
}

TEST(SwiftSourceProperties,
     RealAccessorBodiesCompileAndExecuteWithSharedBackingStorage) {
  std::string Source;
  for (bool Struct : {false, true}) {
    Fixture F(Struct);
    swift_source::planPropertyStorage(F.Signatures);
    ASSERT_TRUE(swift_source::namespaceConflicts(F.Signatures).empty());
    std::vector<swift_source::PropertySourceMember> Members;
    for (size_t I = 0; I < F.Signatures.size(); ++I) {
      auto E = HighSwiftEmitter().emit(F.Functions[I], *F.Signatures[I]);
      ASSERT_TRUE(E.Recovered) << I << ":" << E.Reason;
      EXPECT_EQ(E.MemberSource.find("self.`value`"), std::string::npos);
      EXPECT_NE(E.MemberSource.find("self.`neverd_storage_"),
                std::string::npos);
      Members.push_back({&*F.Signatures[I], E.MemberSource});
    }
    swift_source::PropertyRuntimeSource Runtime;
    if (Struct)
      Runtime.Modifiers["value"] = 0;
    else
      Runtime.TrivialDestructor = true;
    auto Unit = swift_source::assemblePropertyContext(*F.Signatures[0], Members,
                                                      Runtime);
    EXPECT_NE(Unit.find("public var `value`: Swift.Int64"), std::string::npos);
    EXPECT_NE(Unit.find("private var `neverd_storage_"), std::string::npos);
    EXPECT_EQ(Unit.find("func `value`"), std::string::npos);
    Source += Unit;
  }
  executeSwift(Source + R"(
let cases: [Int64] = [.min, .min+1, -1, 0, 1, .max-1, .max]
func addFive(_ storage: inout Int64) { storage &+= 5 }
for input in cases {
  let reference = ReferenceBox(input)
  var value = ValueBox(input)
  precondition(reference.value == input &+ 7 && value.value == input &+ 7)
  reference.value = input
  value.value = input
  precondition(reference.nativeValue() == input &* 3)
  precondition(value.nativeValue() == input &* 3)
  precondition(reference.value == (input &* 3) &+ 7)
  precondition(value.value == (input &* 3) &+ 7)
  addFive(&value.value)
  precondition(value.nativeValue() == (input &* 3) &+ 5)
  precondition(value.value == ((input &* 3) &+ 5) &+ 7)
}
)");
}

TEST(SwiftSourceProperties,
     MetadataCannotReplaceAnUnknownGetterOrMissingSelfProof) {
  Fixture F;
  auto Unplanned = HighSwiftEmitter().emit(F.Functions[0], *F.Signatures[0]);
  EXPECT_FALSE(Unplanned.Recovered);
  swift_source::planPropertyStorage(F.Signatures);
  F.Functions[0].Body[0].RetVal = HighExpr::makeUndef(8);
  EXPECT_FALSE(
      HighSwiftEmitter().emit(F.Functions[0], *F.Signatures[0]).Recovered);
  Fixture Value(true);
  swift_source::planPropertyStorage(Value.Signatures);
  Value.Signatures[0]->IsMutatingKnown = false;
  EXPECT_FALSE(HighSwiftEmitter()
                   .emit(Value.Functions[0], *Value.Signatures[0])
                   .Recovered);
}
