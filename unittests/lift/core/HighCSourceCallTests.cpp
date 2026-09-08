#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/high/HighIR.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
ExprPtr parameter(unsigned Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, Type);
}

HighFunc returning(llvm::StringRef Name, ExprPtr Value,
                   std::vector<TypeRef> Parameters = {}) {
  HighFunc Function;
  Function.Name = Name.str();
  Function.ReturnType = Value->Type;
  for (size_t I = 0; I < Parameters.size(); ++I)
    Function.Params.push_back({"arg" + std::to_string(I), Parameters[I]});
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = std::move(Value);
  Function.Body.push_back(std::move(Return));
  return Function;
}

ExprPtr call(SourceCallTypeHint Hint, TypeRef Carrier,
             std::vector<ExprPtr> Arguments = {}) {
  auto Result = HighExpr::makeCall(Hint.TargetName, Hint.TargetAddress,
                                   std::move(Arguments));
  Result->Type = std::move(Carrier);
  Result->SourceCallHint =
      std::make_shared<const SourceCallTypeHint>(std::move(Hint));
  return Result;
}

SourceCallTypeHint native(llvm::StringRef Name, TypeRef Return,
                          std::vector<TypeRef> Arguments) {
  SourceCallTypeHint Hint;
  Hint.TargetName = Name.str();
  Hint.Signature.ReturnType = std::move(Return);
  for (const auto &Type : Arguments)
    Hint.Signature.Parameters.push_back({"", Type});
  return Hint;
}

std::string emit(const std::vector<HighFunc> &Functions, bool Includes = true) {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  Options.EmitIncludes = Includes;
  Options.EmitComments = false;
  EXPECT_TRUE(HighCEmitter().emit(Functions, OS, Options));
  return Result;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "c",
                                                  SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-call", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  llvm::SmallVector<llvm::StringRef, 12> Arguments{
      Compiler,
      "-std=c11",
      "-O1",
      "-fno-inline",
      "-fblocks",
      "-Werror=implicit-function-declaration",
      "-Werror=return-type",
      SourcePath,
      "-o",
      BinaryPath};
  std::string Error;
  const int Compiled = llvm::sys::ExecuteAndWait(
      Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
  auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
  ASSERT_EQ(Compiled, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << "\n"
                         << Source;
  const int Ran = llvm::sys::ExecuteAndWait(
      BinaryPath, {BinaryPath}, std::nullopt, Redirects, 30, 0, &Error);
  ASSERT_EQ(Ran, 0) << Error << "\n" << Source;
}

TEST(HighCSourceCalls,
     NativeForwardPrototypesPreserveFloatingArgumentAndResultBits) {
  std::vector<HighFunc> Functions;
  for (unsigned Width : {4U, 8U}) {
    auto Bits = NdType::makeInt(Width, false);
    auto Float = NdType::makeFloat(Width);
    const std::string Name = "round_trip_" + std::to_string(Width);
    const std::string Helper = "_helper_" + std::to_string(Width);
    Functions.push_back(returning(
        Name, call(native(Helper, Float, {Float}), Bits, {parameter(0, Bits)}),
        {Bits}));
    Functions.push_back(returning(Helper, parameter(0, Float), {Float}));
  }
  const auto Source = emit(Functions);
  EXPECT_EQ(Source.find("extern int helper"), std::string::npos);
  EXPECT_NE(Source.find("float helper_4(float);"), std::string::npos);
  EXPECT_NE(Source.find("double helper_8(double);"), std::string::npos);
  EXPECT_EQ(Source.find("bad source call"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  const uint32_t a[] = {0,0x80000000U,1,0x3fa00000U,0x7fc00042U,0x7f800000U};
  const uint64_t b[] = {0,0x8000000000000000ULL,1,0x3ff4000000000000ULL,
                        0x7ff8000000000042ULL,0x7ff0000000000000ULL};
  for (unsigned i=0; i<sizeof(a)/sizeof(a[0]); ++i)
    if (round_trip_4(a[i]) != a[i]) return 1;
  for (unsigned i=0; i<sizeof(b)/sizeof(b[0]); ++i)
    if (round_trip_8(b[i]) != b[i]) return 2;
  return 0;
})");
}

TEST(HighCSourceCalls,
     MessageCallCompilesAndExecutesMixedPointerFloatIntegerSignature) {
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Double = NdType::makeFloat(8);
  auto Hint = native("objc_msgSend", Double, {Pointer, Pointer, Double, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "compute:count:";
  auto Expression = call(Hint, U64,
                         {parameter(0, U64), parameter(1, U64),
                          parameter(2, U64), parameter(3, I32)});
  const auto Source =
      emit({returning("send", Expression, {U64, U64, U64, I32})}, false);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
  EXPECT_NE(Source.find("(*)(id, SEL, double, int32_t)"), std::string::npos);
  // A portable dispatch double tests the generated host ABI and exact operands.
  // The separate macOS executable corpus exercises the actual Objective-C
  // runtime.
  compileAndRun(R"(
#include <stdint.h>
typedef void *id;
typedef void *SEL;
static double implementation(id receiver, SEL selector, double value, int32_t count) {
  return (receiver == (id)(uintptr_t)0x1234 && selector == (SEL)(uintptr_t)0x5678)
          ? value + count : -999;
}
static double (*objc_msgSend)(id, SEL, double, int32_t) = implementation;
)" + Source + R"(
int main(void) {
  for (int32_t count=-3; count<4; ++count) {
    double input=1.25, expected=input+count;
    uint64_t bits=__builtin_bit_cast(uint64_t,input);
    if (send(0x1234,0x5678,bits,count) != __builtin_bit_cast(uint64_t,expected)) return 1;
  }
  return 0;
})");
}

TEST(HighCSourceCalls, RuntimeReferencesUseEscapedNamesAndNoOriginalAddresses) {
  auto U64 = NdType::makeInt(8, false);
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  std::vector<HighFunc> Functions;
  const SourceCallTypeHint::Kind Kinds[] = {
      SourceCallTypeHint::Kind::RuntimeSelector,
      SourceCallTypeHint::Kind::RuntimeClass,
      SourceCallTypeHint::Kind::RuntimeMetaclass,
      SourceCallTypeHint::Kind::RuntimeIvarOffset};
  for (size_t I = 0; I < 4; ++I) {
    auto Hint =
        native("quote\"slash\\line\n\?\?/9", I == 3 ? U64 : Pointer, {});
    Hint.CallKind = Kinds[I];
    Hint.OwnerClass = "Owner";
    Hint.TargetAddress = 0xdeadbeef;
    Functions.push_back(
        returning("reference_" + std::to_string(I), call(Hint, U64)));
  }
  const auto Source = emit(Functions, false);
  EXPECT_EQ(Source.find("DEADBEEF"), std::string::npos);
  EXPECT_EQ(Source.find("extern int"), std::string::npos);
  EXPECT_NE(Source.find("\\012\\?\\?/9"), std::string::npos);
  compileAndRun(R"(
#include <stdint.h>
#include <stddef.h>
#include <string.h>
typedef void *Class;
static const char wanted[]="quote\"slash\\line\n\?\?/9";
static void *sel_registerName(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 11 : 0); }
static void *objc_getClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,"Owner")==0 ? 31 : strcmp(s,wanted)==0 ? 12 : 0); }
static void *objc_getMetaClass(const char *s) { return (void *)(uintptr_t)(strcmp(s,wanted)==0 ? 13 : 0); }
static void *class_getInstanceVariable(Class c,const char *s) { return (void *)(uintptr_t)(c==(void *)(uintptr_t)31 && strcmp(s,wanted)==0 ? 32 : 0); }
static ptrdiff_t ivar_getOffset(void *v) { return v==(void *)(uintptr_t)32 ? 24 : -1; }
)" + Source + R"(
int main(void) { return reference_0()!=11 || reference_1()!=12 || reference_2()!=13 || reference_3()!=24; }
)");
}

TEST(HighCSourceCalls, RejectsWrongArgumentCountWidthAndIncompleteBlocks) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSend", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCMessage;
  Hint.Selector = "step:";
  auto Missing = call(Hint, I32, {HighExpr::makeConst(0, 8)});
  EXPECT_NE(emit({returning("missing", Missing)}, false)
                .find("bad source call: argument count"),
            std::string::npos);
  auto WrongWidth = call(Hint, I32,
                         {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                          HighExpr::makeConst(1, 8)});
  EXPECT_NE(emit({returning("width", WrongWidth)}, false)
                .find("bad source call: argument carrier"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  EXPECT_NE(emit({returning("block", call(Hint, I32,
                                          {HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(0, 8),
                                           HighExpr::makeConst(1, 4)}))},
                 false)
                .find("bad source call: block invocation"),
            std::string::npos);
}

TEST(HighCSourceCalls,
     Super2UsesTheCurrentClassRecordAndAnExplicitRuntimeDeclaration) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("objc_msgSendSuper2", I32, {Pointer, Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::ObjCSuper2;
  Hint.Selector = "step:";
  auto Expr = call(Hint, I32,
                   {HighExpr::makeConst(0, 8), HighExpr::makeConst(0, 8),
                    HighExpr::makeConst(1, 4)});
  auto Source = emit({returning("super_send", Expr)});
  EXPECT_NE(Source.find("#include <objc/message.h>"), std::string::npos);
  EXPECT_NE(Source.find("extern void objc_msgSendSuper2(void);"),
            std::string::npos);
  EXPECT_NE(Source.find("(*)(struct objc_super *, SEL, int32_t)"),
            std::string::npos);
  EXPECT_EQ(Source.find("extern int objc_msgSend"), std::string::npos);
}

TEST(HighCSourceCalls, NativeTargetAddressSurvivesProjectionRenaming) {
  auto Type = NdType::makeInt(4);
  auto Hint = native("original_symbol", Type, {Type});
  Hint.TargetAddress = 0x1400;
  auto Caller =
      returning("invoke", call(Hint, Type, {parameter(0, Type)}), {Type});
  Caller.Entry = 0x1300;
  auto Helper = returning("neverd_objc_imp_1400", parameter(0, Type), {Type});
  Helper.Entry = 0x1400;
  const auto Source = emit({Caller, Helper});
  EXPECT_EQ(Source.find("original_symbol"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_objc_imp_1400(int32_t);"),
            std::string::npos);
  compileAndRun(Source + "int main(void) { return invoke(-37) != -37; }\n");
}

TEST(HighCSourceCalls,
     TypedBlockDispatchExecutesCapturedIntegerAndMixedFloatValues) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto U64 = NdType::makeInt(8, false);
  auto F64 = NdType::makeFloat(8);
  auto Integer = native("", I32, {Pointer, I32});
  Integer.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Integer.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Floating = native("", F64, {Pointer, F64, I32});
  Floating.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Floating.Signature.Origin = SourceFunctionTypeHint::OriginKind::BlockRuntime;
  auto A =
      returning("integer_block",
                call(Integer, I32, {parameter(0, Pointer), parameter(1, I32)}),
                {Pointer, I32});
  auto B = returning(
      "floating_block",
      call(Floating, U64,
           {parameter(0, Pointer), parameter(1, U64), parameter(2, I32)}),
      {Pointer, U64, I32});
  auto Source = emit({A, B});
  EXPECT_EQ(Source.find("bad source call"), std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32], *_NSConcreteGlobalBlock[32];
int main(void) {
  int captured = 17;
  int (^integer)(int) = ^(int value) { return (int)((unsigned)value * 3u + (unsigned)captured); };
  double (^floating)(double,int) = ^(double a,int b) { return a * 2.0 + b; };
  unsigned cases[] = {0,1,0x80,0xff,0x80000001u,0xffffffffu};
  for (unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
    int x=(int)cases[i];
    if (integer_block((void *)integer,x)!=integer(x)) return 1;
    double a=(double)(int)i - 2.25;
    uint64_t bits=__builtin_bit_cast(uint64_t,a);
    uint64_t actual=floating_block((void *)floating,bits,x);
    if (__builtin_bit_cast(double,actual)!=floating(a,x)) return 2;
  }
  return 0;
}
)");
}

TEST(HighCSourceCalls, BlockReceiverIsEvaluatedExactlyOnce) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto I32 = NdType::makeInt(4);
  auto Hint = native("", I32, {Pointer, I32});
  Hint.CallKind = SourceCallTypeHint::Kind::BlockInvoke;
  Hint.Signature.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  auto Next = call(native("next_block", Pointer, {}), Pointer);
  auto Source = emit({returning(
      "invoke_once", call(Hint, I32, {Next, parameter(0, I32)}), {I32})});
  compileAndRun(Source + R"(
void *_NSConcreteGlobalBlock[32];
static unsigned calls;
void *next_block(void) {
  ++calls;
  return (void *)^(int value) { return value + 7; };
}
int main(void) { return invoke_once(5)!=12 || calls!=1; }
)");
}

TEST(HighCSourceCalls,
     BlockSourceAddressesUseActualDefinitionsAndBoundHelpers) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto U64 = NdType::makeInt(8, false);
  auto I32 = NdType::makeInt(4);
  std::vector<HighFunc> Functions;
  auto Address = native("original_untrusted_name", Pointer, {});
  Address.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Address.TargetAddress = 0xabc;
  Functions.push_back(returning("get_invoke", call(Address, U64)));
  auto Definition =
      returning("neverd_block_invoke_abc", parameter(0, I32), {I32});
  Definition.Entry = 0xabc;
  Functions.push_back(Definition);
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  Address.TargetName = "__NSConcreteStackBlock";
  Functions.push_back(returning("get_isa", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  Address.TargetAddress = 0xdead;
  Functions.push_back(returning("get_descriptor", call(Address, U64)));
  Address.CallKind = SourceCallTypeHint::Kind::RuntimeBlockLiteral;
  Address.TargetAddress = 0xbeef;
  Functions.push_back(returning("get_literal", call(Address, U64)));
  auto Source = emit(Functions);
  EXPECT_EQ(Source.find("original_untrusted_name"), std::string::npos);
  EXPECT_NE(Source.find("int32_t neverd_block_invoke_abc(int32_t);"),
            std::string::npos);
  compileAndRun(Source + R"(
void *_NSConcreteStackBlock[32];
static int descriptor, literal;
uintptr_t neverd_block_descriptor_dead_address(void) { return (uintptr_t)&descriptor; }
uintptr_t neverd_block_literal_beef_address(void) { return (uintptr_t)&literal; }
int main(void) {
  if (((int32_t (*)(int32_t))get_invoke())(-71)!=-71) return 1;
  if (get_isa()!=(uintptr_t)_NSConcreteStackBlock) return 2;
  return get_descriptor()!=(uintptr_t)&descriptor || get_literal()!=(uintptr_t)&literal;
}
)");
}

TEST(HighCSourceCalls,
     UnknownOrOperandBearingSourceAddressesRemainExplicitFailures) {
  auto Pointer = NdType::makePtr(NdType::makeVoid());
  auto Hint = native("_NSConcreteStackBlock_extra", Pointer, {});
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockIsa;
  EXPECT_NE(emit({returning("bad_isa", call(Hint, Pointer))})
                .find("bad source call: unknown concrete"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::NativeAddress;
  Hint.TargetAddress = 0x1000;
  EXPECT_NE(emit({returning("missing", call(Hint, Pointer))})
                .find("bad source call: native address"),
            std::string::npos);
  Hint.CallKind = SourceCallTypeHint::Kind::RuntimeBlockDescriptor;
  EXPECT_NE(emit({returning("args",
                            call(Hint, Pointer, {HighExpr::makeConst(0, 8)}))})
                .find("bad source call: invalid source address"),
            std::string::npos);
}
} // namespace
