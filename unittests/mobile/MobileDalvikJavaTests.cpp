#include "MobileDalvik.h"
#include "gtest/gtest.h"

#include <functional>
#include <stdexcept>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction op(uint32_t PC, std::string Name,
               std::vector<unsigned> Registers = {}, Literal Value = {},
               Reference Ref = {}, std::optional<uint32_t> Target = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Name);
  I.registers = std::move(Registers);
  I.literal = std::move(Value);
  I.reference = std::move(Ref);
  I.target = Target;
  return I;
}
Method method(std::string Name, std::vector<std::string> Parameters,
              std::string Returns, unsigned Registers,
              std::vector<Instruction> Code,
              std::vector<TryRegion> Tries = {}) {
  Method M;
  M.reference = {"Lfixture/Core;", std::move(Name), std::move(Parameters),
                 std::move(Returns)};
  M.access = {"public", "static"};
  M.registers = Registers;
  M.instructions = std::move(Code);
  M.tries = std::move(Tries);
  M.code_end = M.instructions.empty() ? 0 : M.instructions.back().pc + 1;
  return M;
}
Class klass(std::string Name, std::vector<Method> Methods = {}) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.methods = std::move(Methods);
  for (auto &M : C.methods)
    M.reference.owner = C.name;
  return C;
}
llvm::json::Object recover(std::vector<Class> Input, Limits L = {}) {
  ClassMap Classes;
  for (auto &C : Input) {
    std::string Name = C.name;
    Classes.emplace(std::move(Name), std::move(C));
  }
  Budget B(L);
  return recoverJava(Classes, B);
}
llvm::json::Object recoverMethods(std::vector<Method> Methods, Limits L = {}) {
  return recover({klass("Lfixture/Core;", std::move(Methods))}, L);
}
std::string source(const llvm::json::Object &Report, size_t Index = 0) {
  const auto *Units = Report.getArray("source_units");
  if (!Units || Index >= Units->size())
    return {};
  const auto *Unit = (*Units)[Index].getAsObject();
  if (!Unit)
    return {};
  auto Source = Unit->getString("source");
  return Source ? Source->str() : "";
}
void rejected(const std::function<void()> &Run, std::string_view Fragment) {
  try {
    Run();
    FAIL() << "accepted an unsupported source body";
  } catch (const std::runtime_error &E) {
    EXPECT_NE(std::string(E.what()).find(Fragment), std::string::npos)
        << E.what();
  }
}

TEST(MobileDalvikJava, RejectsUndefinedRegisterBeforePublication) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "I", 1, {op(0, "return", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, DefinitionOnOneConditionalPathDoesNotInitializeTheJoin) {
  rejected(
      [] {
        recoverMethods({method("bad", {"I"}, "I", 2,
                               {op(0, "if-eqz", {1}, {}, {}, 2),
                                op(1, "const/4", {0}, int64_t(7)),
                                op(2, "return", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, OverwrittenHighWordCannotSupplyAWideReturn) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "J", 2,
                               {op(0, "const-wide/16", {0}, int64_t(9)),
                                op(1, "const/4", {1}, int64_t(0)),
                                op(2, "return-wide", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, RetiresBothWordsOfAnOverlappingOldPair) {
  auto R = recoverMethods(
      {method("overlap", {}, "J", 4,
              {op(0, "const-wide/16", {2}, int64_t(9)),
               op(1, "const-wide/16", {1}, int64_t(7)),
               op(2, "const/4", {3}, int64_t(0)), op(3, "return-wide", {1})})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 1);
  EXPECT_NE(source(R).find("w1 = 0x0000000000000007L;"), std::string::npos);
  EXPECT_NE(source(R).find("return w1;"), std::string::npos);
}
TEST(MobileDalvikJava, BranchCannotBypassInvocationResultProducer) {
  rejected(
      [] {
        MethodRef Ref{"Ljava/lang/Math;", "abs", {"I"}, "I"};
        recoverMethods(
            {method("bad", {"I"}, "I", 2,
                    {op(0, "if-eqz", {1}, {}, {}, 2),
                     op(1, "invoke-static", {1}, {}, Ref),
                     op(2, "move-result", {0}), op(3, "return", {0})})});
      },
      "bypasses");
}
TEST(MobileDalvikJava, ExceptionalEdgeUsesStateBeforeTheThrowingWrite) {
  rejected(
      [] {
        recoverMethods({method(
            "bad", {"I", "I"}, "I", 4,
            {op(0, "div-int", {0, 2, 3}), op(1, "return", {0}),
             op(2, "move-exception", {1}), op(3, "return", {0})},
            {{0, 1, {{std::string("Ljava/lang/ArithmeticException;"), 2}}}})});
      },
      "undefined");
}
TEST(MobileDalvikJava, NormalFlowCannotEnterAnExceptionOnlyValue) {
  rejected(
      [] {
        recoverMethods({method(
            "bad", {}, "V", 1,
            {op(0, "nop"), op(1, "move-exception", {0}), op(2, "return-void")},
            {{0, 1, {{std::nullopt, 1}}}})});
      },
      "normal flow");
}
TEST(MobileDalvikJava, UnknownAndUnreachableOperationsAreNotEmptyBodies) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "V", 0,
                               {op(0, "unrecognized"), op(1, "return-void")})});
      },
      "unsupported instruction");
  rejected(
      [] {
        recoverMethods({method("bad", {}, "V", 0,
                               {op(0, "return-void"), op(1, "unrecognized")})});
      },
      "unreachable");
}
TEST(MobileDalvikJava, LocalCalleeIdentityIncludesReturnTypeAndStaticKind) {
  Method Target = method("target", {"I"}, "I", 1, {op(0, "return", {0})});
  rejected(
      [&] {
        MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "J"};
        recoverMethods({Target, method("bad", {"I"}, "J", 3,
                                       {op(0, "invoke-static", {2}, {}, Ref),
                                        op(1, "move-result-wide", {0}),
                                        op(2, "return-wide", {0})})});
      },
      "member type differs");
  rejected(
      [&] {
        MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "I"};
        recoverMethods(
            {Target,
             method("bad", {"Lfixture/Core;", "I"}, "I", 3,
                    {op(0, "invoke-virtual", {1, 2}, {}, Ref),
                     op(1, "move-result", {0}), op(2, "return", {0})})});
      },
      "static/instance");
}
TEST(MobileDalvikJava, MatchingLocalCalleeRetainsArgumentsAndResult) {
  MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "I"};
  auto R = recoverMethods(
      {method("target", {"I"}, "I", 1, {op(0, "return", {0})}),
       method("caller", {"I"}, "I", 2,
              {op(0, "invoke-static", {1}, {}, Ref), op(1, "move-result", {0}),
               op(2, "return", {0})})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(R).find("result32 = ((fixture.Core) null).target(v1);"),
            std::string::npos);
  EXPECT_NE(source(R).find("v0 = result32;"), std::string::npos);
}
TEST(MobileDalvikJava,
     ExactFieldBindingRejectsWrongTypeStaticKindAndMissingMember) {
  Class C = klass("Lfixture/Core;");
  C.fields.push_back(
      {{"Lfixture/Core;", "value", "I"}, {"public", "static"}, {}});
  for (const auto &Ref : {FieldRef{"Lfixture/Core;", "value", "J"},
                          FieldRef{"Lfixture/Core;", "missing", "I"}}) {
    Class Input = C;
    Input.methods = {
        method("bad", {}, Ref.type, 2,
               {op(0, Ref.type == "J" ? "sget-wide" : "sget", {0}, {}, Ref),
                op(1, Ref.type == "J" ? "return-wide" : "return", {0})})};
    rejected([&] { recover({Input}); }, "member");
  }
  C.methods = {method(
      "bad", {"Lfixture/Core;"}, "I", 2,
      {op(0, "iget", {0, 1}, {}, FieldRef{"Lfixture/Core;", "value", "I"}),
       op(1, "return", {0})})};
  rejected([&] { recover({C}); }, "static/instance");
}
TEST(MobileDalvikJava, SuperCallRequiresActualThisProvenance) {
  MethodRef Ref{"Ljava/lang/Object;", "hashCode", {}, "I"};
  Method M = method("run", {"Lfixture/Core;"}, "I", 3,
                    {op(0, "invoke-super", {2}, {}, Ref),
                     op(1, "move-result", {0}), op(2, "return", {0})});
  M.access.erase("static");
  rejected([&] { recoverMethods({M}); }, "actual receiver");
  M.instructions[0].registers = {1};
  auto R = recoverMethods({M});
  EXPECT_NE(source(R).find("result32 = super.hashCode();"), std::string::npos);
}
TEST(MobileDalvikJava,
     ExternalSuperConstructorRequiresInheritedTypeDeclarations) {
  MethodRef Ref{
      "Ljava/io/FileInputStream;", "<init>", {"Ljava/lang/String;"}, "V"};
  Method M =
      method("<init>", {"Ljava/lang/String;"}, "V", 2,
             {op(0, "invoke-direct", {0, 1}, {}, Ref), op(1, "return-void")});
  M.access = {"public", "constructor"};
  Class C = klass("Lfixture/Core;", {M});
  C.superclass = Ref.owner;
  // The external superclass may contribute member types that obscure
  // java.lang.String or the runtime helpers. Missing declarations do not
  // establish that those qualified names retain their intended bindings.
  rejected([&] { recover({C}); }, "unresolved inherited Java type");
}
TEST(MobileDalvikJava,
     KnownSuperConstructorPreservesCheckedThrowableAndOriginalArguments) {
  Method BaseInit =
      method("<init>", {"Ljava/lang/Throwable;"}, "V", 2,
             {op(0, "invoke-direct", {0}, {},
                 MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
              op(1, "if-eqz", {1}, {}, {}, 3), op(2, "throw", {1}),
              op(3, "return-void")});
  BaseInit.access = {"public", "constructor"};
  Class Base = klass("Lfixture/CheckedBase;", {BaseInit});
  Method M = method(
      "<init>", {"Ljava/lang/Throwable;"}, "V", 2,
      {op(0, "invoke-direct", {0, 1}, {},
          MethodRef{Base.name, "<init>", {"Ljava/lang/Throwable;"}, "V"}),
       op(1, "return-void")});
  M.access = {"public", "constructor"};
  Class C = klass("Lfixture/Core;", {M});
  C.superclass = Base.name;
  auto Report = recover({Base, C});
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(Report).find("throw __neverdThrow((java.lang.Throwable)"),
            std::string::npos);
  EXPECT_NE(source(Report, 1).find("super(arg0);"), std::string::npos);
  C.methods[0].instructions[0].registers = {0, 0};
  rejected([&] { recover({Base, C}); }, "unavailable argument");
}
TEST(MobileDalvikJava,
     AllocationRequiresAdjacentMatchingInitializerAndNoBypass) {
  MethodRef Ref{"Ljava/lang/Object;", "<init>", {}, "V"};
  auto Good = method(
      "allocate", {}, "Ljava/lang/Object;", 1,
      {op(0, "new-instance", {0}, {}, std::string("Ljava/lang/Object;")),
       op(1, "invoke-direct", {0}, {}, Ref), op(2, "return-object", {0})});
  auto R = recoverMethods({Good});
  EXPECT_NE(source(R).find("o0 = new java.lang.Object();"), std::string::npos);
  Good.instructions[1] = op(1, "nop");
  rejected([&] { recoverMethods({Good}); }, "adjacent initializer");
}
TEST(MobileDalvikJava, FinalConstantsAvoidCompileTimeInliningAndKeepFloatBits) {
  Class C = klass("Lfixture/Core;");
  C.fields = {
      {{C.name, "magic", "I"}, {"public", "static", "final"}, int64_t(7)},
      {{C.name, "negativeZero", "F"},
       {"public", "static", "final"},
       FloatBits{UINT64_C(0x80000000), false}},
      {{C.name, "nan", "D"},
       {"public", "static", "final"},
       FloatBits{UINT64_C(0x7ff8123456789abc), true}}};
  auto R = recover({C});
  auto S = source(R);
  EXPECT_NE(S.find("magic = __neverdConstant(0x00000007);"), std::string::npos);
  EXPECT_NE(S.find("intBitsToFloat(0x80000000)"), std::string::npos);
  EXPECT_NE(S.find("longBitsToDouble(0x7ff8123456789abcL)"), std::string::npos);
  C.fields[0].value = std::monostate{};
  rejected([&] { recover({C}); }, "verified initialization");
}
TEST(MobileDalvikJava, NativeAndAbstractDeclarationsHaveSeparateCoverage) {
  Class C = klass("Lfixture/Core;", {method("body", {}, "I", 1,
                                            {op(0, "const/4", {0}, int64_t(42)),
                                             op(1, "return", {0})})});
  C.access.insert("abstract");
  Method Native = method("nativeEntry", {"J"}, "I", 0, {});
  Native.access.insert("native");
  Method Abstract = method("abstractEntry", {}, "V", 0, {});
  Abstract.access = {"public", "abstract"};
  C.methods.push_back(Native);
  C.methods.push_back(Abstract);
  auto R = recover({C});
  EXPECT_EQ(R.getInteger("method_count"), 3);
  EXPECT_EQ(R.getInteger("recovered_method_count"), 1);
  EXPECT_EQ(R.getInteger("declaration_only_method_count"), 2);
  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
  ASSERT_EQ(R.getArray("methods")->size(), 3u);
  EXPECT_NE(source(R).find("abstract void abstractEntry();"),
            std::string::npos);
}
TEST(MobileDalvikJava, NestedClassesShareAUnitWithoutDroppingMethods) {
  Class Outer = klass(
      "Lfixture/Outer;",
      {method("outer", {}, "I", 1,
              {op(0, "const/4", {0}, int64_t(1)), op(1, "return", {0})})});
  Class Inner = klass(
      "Lfixture/Outer$Nested;",
      {method("inner", {}, "I", 1,
              {op(0, "const/4", {0}, int64_t(2)), op(1, "return", {0})})});
  Inner.enclosing = Outer.name;
  Inner.inner_name = "Nested";
  Inner.inner_access = {"public", "static"};
  auto R = recover({Outer, Inner});
  EXPECT_EQ(R.getInteger("class_count"), 2);
  EXPECT_EQ(R.getInteger("method_count"), 2);
  ASSERT_EQ(R.getArray("source_units")->size(), 1u);
  EXPECT_NE(source(R).find("public static class Nested"), std::string::npos);
  EXPECT_NE(source(R).find("int inner()"), std::string::npos);
  Inner.inner_access.erase("static");
  rejected([&] { recover({Outer, Inner}); }, "outer-instance initialization");
}
TEST(MobileDalvikJava,
     CyclicOrExcessiveNestedOwnershipCannotReachTypeRecursion) {
  Class A = klass("LA;"), B = klass("LB;");
  A.enclosing = B.name;
  A.inner_name = "A";
  A.inner_access = {"static"};
  B.enclosing = A.name;
  B.inner_name = "B";
  B.inner_access = {"static"};
  rejected([&] { recover({A, B}); }, "recursive nested");
  std::vector<Class> Classes;
  for (unsigned I = 0; I < 129; ++I) {
    Class C = klass("LC" + std::to_string(I) + ";");
    if (I) {
      C.enclosing = "LC" + std::to_string(I - 1) + ";";
      C.inner_name = "N" + std::to_string(I);
      C.inner_access = {"static"};
    }
    Classes.push_back(std::move(C));
  }
  rejected([&] { recover(Classes); }, "source depth limit");
}
TEST(
    MobileDalvikJava,
    StringEscapingPreservesLoneSurrogatesSupplementaryCharactersAndSlashUText) {
  std::string Value = "\\u000a\n";
  Value += std::string("\xed\xa0\x80", 3);     // lone UTF-16 high surrogate
  Value += std::string("\xed\xb0\x80", 3);     // lone UTF-16 low surrogate
  Value += std::string("\xf0\x9f\x98\x80", 4); // supplementary code point
  Value.push_back('\0');
  auto R = recoverMethods({method(
      "text", {}, "Ljava/lang/String;", 1,
      {op(0, "const-string", {0}, Value), op(1, "return-object", {0})})});
  EXPECT_NE(
      source(R).find("\"\\\\u000a\\n\\ud800\\udc00\\ud83d\\ude00\\u0000\""),
      std::string::npos);
  rejected(
      [] {
        recoverMethods(
            {method("bad", {}, "Ljava/lang/String;", 1,
                    {op(0, "const-string", {0}, std::string("\xc0\x80", 2)),
                     op(1, "return-object", {0})})});
      },
      "invalid UTF-8");
}
TEST(MobileDalvikJava, ArrayPayloadChecksCompleteRangeBeforeAnyStore) {
  Instruction Fill = op(0, "fill-array-data", {0});
  Fill.element_width = 4;
  Fill.data = {UINT64_C(0xffffffff), 7};
  auto R = recoverMethods(
      {method("fill", {"[I"}, "V", 1, {Fill, op(1, "return-void")})});
  auto S = source(R);
  auto Check = S.find(
      ".length < 2) throw new java.lang.ArrayIndexOutOfBoundsException();");
  auto Store = S.find("[0] = (int) 0xffffffff;");
  ASSERT_NE(Check, std::string::npos);
  ASSERT_NE(Store, std::string::npos);
  EXPECT_LT(Check, Store);
  Fill.element_width = 8;
  rejected(
      [&] {
        recoverMethods(
            {method("bad", {"[I"}, "V", 1, {Fill, op(1, "return-void")})});
      },
      "element width");
}
TEST(MobileDalvikJava, SwitchAndExceptionDispatchPreserveExplicitDestinations) {
  Instruction Switch = op(0, "sparse-switch", {1}, {}, {}, 10);
  Switch.keys = {-1, 7};
  Switch.targets = {3, 5};
  auto R = recoverMethods(
      {method("choose", {"I"}, "I", 2,
              {Switch, op(1, "const/4", {0}, int64_t(3)), op(2, "return", {0}),
               op(3, "const/4", {0}, int64_t(4)), op(4, "return", {0}),
               op(5, "const/4", {0}, int64_t(5)), op(6, "return", {0})}),
       method(
           "divide", {"I", "I"}, "I", 3,
           {op(0, "div-int", {0, 1, 2}), op(1, "return", {0}),
            op(2, "move-exception", {0}), op(3, "const/4", {0}, int64_t(-1)),
            op(4, "return", {0})},
           {{0, 1, {{std::string("Ljava/lang/ArithmeticException;"), 2}}}})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(R).find("case 0xffffffff: pc = 3; break;"),
            std::string::npos);
  EXPECT_NE(source(R).find("failure instanceof java.lang.ArithmeticException"),
            std::string::npos);
  EXPECT_NE(source(R).find("caught = failure; pc = 2; continue dispatch;"),
            std::string::npos);
}
TEST(MobileDalvikJava, FloatingArgumentsAndReturnCarriersRetainRawBits) {
  auto R = recoverMethods(
      {method("floatBits", {"F"}, "F", 1, {op(0, "return", {0})}),
       method("doubleBits", {"D"}, "D", 2, {op(0, "return-wide", {0})})});
  auto S = source(R);
  EXPECT_NE(S.find("floatToRawIntBits(arg0)"), std::string::npos);
  EXPECT_NE(S.find("return ((java.lang.Float) null).intBitsToFloat(v0);"),
            std::string::npos);
  EXPECT_NE(S.find("doubleToRawLongBits(arg0)"), std::string::npos);
  EXPECT_NE(S.find("return ((java.lang.Double) null).longBitsToDouble(w0);"),
            std::string::npos);
}
TEST(MobileDalvikJava, ArithmeticAndLoopUseVerifiedMutableCarriers) {
  auto R = recoverMethods({method(
      "sum", {"[I"}, "I", 5,
      {op(0, "const/4", {0}, int64_t(0)), op(1, "const/4", {1}, int64_t(0)),
       op(2, "array-length", {2, 4}), op(3, "if-ge", {1, 2}, {}, {}, 10),
       op(4, "aget", {3, 4, 1}), op(5, "if-gez", {3}, {}, {}, 7),
       op(6, "neg-int", {3, 3}), op(7, "add-int/2addr", {0, 3}),
       op(8, "add-int/lit8", {1, 1}, int64_t(1)), op(9, "goto", {}, {}, {}, 3),
       op(10, "return", {0})})});
  auto S = source(R);
  EXPECT_NE(S.find("pc = (v1 >= v2) ? 10 : 4;"), std::string::npos);
  EXPECT_NE(S.find("v0 = ((v0) + (v3));"), std::string::npos);
  EXPECT_NE(S.find("pc = 3;"), std::string::npos);
  EXPECT_NE(S.find("return v0;"), std::string::npos);
}
TEST(MobileDalvikJava, ResourceLimitsRejectBeforeReturningSourceUnits) {
  Limits L;
  L.max_bytes = 64;
  rejected(
      [&] {
        recoverMethods({method("body", {}, "I", 1,
                               {op(0, "const/4", {0}, int64_t(42)),
                                op(1, "return", {0})})},
                       L);
      },
      "byte budget");
  Budget B;
  B.remaining = 0;
  Class C = klass("Lfixture/Core;",
                  {method("body", {}, "V", 0, {op(0, "return-void")})});
  ClassMap Classes;
  Classes.emplace(C.name, C);
  rejected([&] { recoverJava(Classes, B); }, "budget");
}
TEST(MobileDalvikJava, StaticExternalTypeQualifierCannotBindToAShadowingField) {
  Class C = klass("Lfixture/Core;");
  C.fields.push_back({{C.name, "java", "I"}, {"public", "static"}, {}});
  C.methods = {method("bad", {"I"}, "I", 1,
                      {op(0, "invoke-static", {0}, {},
                          MethodRef{"Ljava/lang/Math;", "abs", {"I"}, "I"}),
                       op(1, "move-result", {0}), op(2, "return", {0})})};
  rejected([&] { recover({C}); }, "shadowed");
}
TEST(MobileDalvikJava, SamePackageClassNameCannotObscureItsPackageQualifier) {
  Class Core = klass(
      "Lfixture/Core;",
      {method("value", {}, "I", 1,
              {op(0, "const/16", {0}, int64_t(17)), op(1, "return", {0})})});
  Class Shadow =
      klass("Lfixture/fixture;",
            {method("identity", {Core.name}, Core.name, 1,
                    {op(0, "return-object", {0})}),
             method("value", {}, "I", 1,
                    {op(0, "invoke-static", {}, {},
                        MethodRef{Core.name, "value", {}, "I"}),
                     op(1, "move-result", {0}), op(2, "return", {0})})});
  // Fields do not shadow names in a type context. The resolution must not
  // confuse this legal field with a member type named Core.
  Shadow.fields.push_back(
      {{Shadow.name, "Core", "I"}, {"public", "static"}, {}});
  const auto report = recover({Core, Shadow});
  EXPECT_EQ(report.getInteger("recovered_method_count"), 3);
  const auto text = source(report, 1);
  EXPECT_NE(text.find("Core identity(Core arg0)"), std::string::npos);
  EXPECT_NE(text.find("return ((Core) o0);"), std::string::npos);
  EXPECT_NE(text.find("((Core) null).value()"), std::string::npos);
  EXPECT_EQ(text.find("fixture.Core"), std::string::npos);
}
TEST(MobileDalvikJava, ShortenedSamePackageNameCannotSelectANestedNamesake) {
  Class Core = klass("Lfixture/Core;");
  Class Shadow =
      klass("Lfixture/fixture;", {method("identity", {Core.name}, Core.name, 1,
                                         {op(0, "return-object", {0})})});
  Class Nested = klass("Lfixture/fixture$Core;");
  Nested.enclosing = Shadow.name;
  Nested.inner_name = "Core";
  Nested.inner_access = {"public", "static"};
  rejected([&] { recover({Core, Shadow, Nested}); }, "ambiguous Java type");
}
TEST(MobileDalvikJava, UnknownInheritedMemberCannotValidateAPackageQualifier) {
  // Map.Entry is inherited into the body of C. The original Java can name
  // Peer through an import, but Entry.Peer binds Entry to the inherited type.
  // Without Map's declarations, neither absence nor identity is proven.
  Class Peer = klass("LEntry/Peer;");
  Class C = klass("Limpl/C;", {method("identity", {Peer.name}, Peer.name, 1,
                                      {op(0, "return-object", {0})})});
  C.access.insert("abstract");
  C.interfaces = {"Ljava/util/Map;"};
  rejected([&] { recover({Peer, C}); }, "unresolved inherited Java type");

  Class Base = klass("Limpl/Base;");
  Base.access.insert("abstract");
  Base.interfaces = C.interfaces;
  C.interfaces.clear();
  C.superclass = Base.name;
  rejected([&] { recover({Peer, Base, C}); }, "unresolved inherited Java type");
}
TEST(MobileDalvikJava, OwnInheritedMembersAreOutsideTheSupertypeHeaderScope) {
  Class C = klass("Limpl/Bridge;");
  C.access = {"public", "interface", "abstract"};
  C.interfaces = {"Ljava/util/Map;"};
  auto Size = method("observedSize", {}, "I", 0, {});
  Size.reference.owner = C.name;
  Size.access = {"public", "abstract"};
  C.methods = {Size};
  auto Report = recover({C});
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 1);
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 0);
  EXPECT_NE(source(Report).find("interface Bridge extends java.util.Map"),
            std::string::npos);
}
TEST(MobileDalvikJava, ObjectInheritanceHasNoUnknownMemberTypeDeclarations) {
  Class Peer = klass("LEntry/Peer;");
  Class C = klass("Limpl/C;", {method("identity", {Peer.name}, Peer.name, 1,
                                      {op(0, "return-object", {0})})});
  auto Report = recover({Peer, C});
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 1);
  EXPECT_NE(source(Report, 1).find("Entry.Peer identity(Entry.Peer arg0)"),
            std::string::npos);
}
TEST(MobileDalvikJava, RuntimeHelperTypesAlsoRequireProvenPackageBindings) {
  Class C = klass("Limpl/C;", {method("value", {}, "I", 1,
                                      {op(0, "const/4", {0}, int64_t(17)),
                                       op(1, "return", {0})})});
  C.interfaces = {"Lexternal/Contract;"};
  rejected([&] { recover({C}); }, "unresolved inherited Java type");
}
} // namespace
