//===- HighCIntegerConversionTests.cpp - Integer conversions in HighC ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

TypeRef integer(uint16_t Bytes, bool Signed) {
  return NdType::makeInt(Bytes, Signed);
}

ExprPtr input() {
  MedVar V;
  V.Kind = MedVar::Param;
  V.Id = 0;
  V.Size = 8;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, integer(8, false));
}

ExprPtr local(int Id, TypeRef Type) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.Id = Id;
  V.Size = Type->Size;
  V.TheArch = Arch::X64;
  return HighExpr::makeVar(V, std::move(Type));
}

ExprPtr constant(uint64_t N, uint16_t Bytes = 8) {
  return HighExpr::makeConst(N, Bytes);
}

ExprPtr typed(ExprPtr E, TypeRef Type) {
  E->Type = std::move(Type);
  return E;
}

ExprPtr op(NdOp Op, ExprPtr A, ExprPtr B, TypeRef Type) {
  return typed(HighExpr::makeBinop(Op, std::move(A), std::move(B)),
               std::move(Type));
}

/// The low bytes of \p E, as MedToHigh leaves a truncation.
ExprPtr lowPart(ExprPtr E, TypeRef Type) {
  return op(NdOp::SUBBYTES, std::move(E), constant(0, 4), std::move(Type));
}

ExprPtr cast(ExprPtr E, TypeRef Type) {
  auto C = std::make_shared<HighExpr>();
  C->Kind = ExprKind::Cast;
  C->Type = C->CastTo = std::move(Type);
  C->Operands = {std::move(E)};
  return C;
}

ExprPtr extend(NdOp Op, ExprPtr E, TypeRef Type) {
  return typed(HighExpr::makeUnary(Op, std::move(E)), std::move(Type));
}

HighStmt assign(ExprPtr Dst, ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Assign;
  S.Dst = std::move(Dst);
  S.Val = std::move(Value);
  return S;
}

HighStmt result(ExprPtr Value) {
  HighStmt S;
  S.Kind = StmtKind::Return;
  S.RetVal = std::move(Value);
  return S;
}

HighFunc function(const std::string &Name, TypeRef Return,
                  std::vector<HighStmt> Body) {
  HighFunc F;
  F.Name = Name;
  F.Entry = 0x1000;
  F.ReturnType = std::move(Return);
  F.Params = {{"arg0", integer(8, false)}};
  F.Body = std::move(Body);
  return F;
}

std::string emit(const HighFunc &F) {
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  EXPECT_TRUE(HighCEmitter().emit({F}, Out, Options));
  return Source;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-conversion", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-conversion", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-conversion", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined",
        "-fsanitize-trap=undefined",
        "-Werror=uninitialized",
        "-Werror=int-conversion",
        "-Werror=constant-conversion",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    ASSERT_EQ(Result, 0) << Error << '\n' << Source;
  }
}

constexpr const char *Inputs = R"(
static const uint64_t xs[] = {0, 1, 0x7F, 0x80, 0xFF, 0x7FFFFFFF, 0x80000000,
                              0xFFFFFFFF, 0x100000001ull, 0x7FFFFFFFFFFFFFFFull,
                              0x8000000000000000ull, 0xFFFFFFFFFFFF8001ull,
                              0xFFFFFFFFFFFFFFFFull};
)";

TEST(HighCIntegerConversion, FloatingCallKeepsAConstantBitCast) {
  for (const auto Architecture :
       {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    for (uint16_t Bytes : {4, 8}) {
      SCOPED_TRACE(Bytes);
      const TypeRef Float = NdType::makeFloat(Bytes);
      auto Parameter = typed(input(), Float);
      Parameter->Var.Size = Bytes;
      Parameter->Var.TheArch = Architecture;
      HighFunc Callee = function("identity", Float, {result(Parameter)});
      Callee.Entry = 0x2000;
      Callee.Params = {{"arg0", Float}};
      auto Value = cast(
          constant(Bytes == 4 ? 0x3F800000ull : 0x3FF0000000000000ull, Bytes),
          Float);
      Value->Kind = ExprKind::BitCast;
      auto Call = typed(HighExpr::makeCall("identity", 0x2000, {Value}), Float);
      HighFunc Caller = function("call_constant", Float, {result(Call)});
      Caller.Params.clear();
      CEmitterOptions Options;
      Options.TheArch = Architecture;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      ASSERT_TRUE(HighCEmitter().emit({Caller, Callee}, Out, Options));
      compileAndRun(Source +
                    "\nint main(void) { return call_constant() != 1.0; }\n");
    }
  }
}

TEST(HighCIntegerConversion, NarrowCopyKeepsLowBytesThroughSourceProjection) {
  for (const auto Architecture : {Arch::X86, Arch::X64, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    std::vector<HighFunc> Functions;
    std::string Checks;
    for (uint16_t Bytes : {1, 2, 4}) {
      const auto &Registers = getTargetRegInfo(Architecture);
      MedFunc F;
      F.Name = "copy_low_" + std::to_string(Bytes);
      F.Entry = 0x1000;
      F.ReturnType = integer(Registers.PointerSize, false);
      MedVar Input;
      Input.Kind = MedVar::Param;
      Input.Id = 0;
      Input.Size = Registers.PointerSize;
      Input.TheArch = Architecture;
      Input.RegOff =
          Registers.IntParamRegs.empty() ? 0 : Registers.IntParamRegs[0];
      F.Params = {Input};
      MedVar Narrow;
      Narrow.Id = 1;
      Narrow.Size = Bytes;
      Narrow.TheArch = Architecture;
      MedVar Wide = Narrow;
      Wide.Kind = MedVar::Reg;
      Wide.Id = 2;
      Wide.SSAVer = 1;
      Wide.RegOff = Registers.IntReturnReg;
      Wide.Size = Registers.PointerSize;
      MedOp Copy;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = Narrow;
      Copy.addInput(Input);
      MedOp Extend;
      Extend.Opcode = NdOp::INT_ZEXT;
      Extend.Output = Wide;
      Extend.addInput(Narrow);
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Wide);
      MedBlock B;
      B.Id = 0;
      B.StartAddr = F.Entry;
      B.EndAddr = F.Entry + 1;
      B.Ops = {Copy, Extend, Return};
      F.Blocks = {B};
      Functions.push_back(MedToHighConverter().convert(F, Architecture));
      Checks += "if (" + F.Name + "(xs[i]) != (uint" +
                std::to_string(Bytes * 8) + "_t)xs[i]) return 1;\n";
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    compileAndRun(Source + Inputs +
                  "\nint main(void) {\n"
                  "for (unsigned i=0; i<sizeof xs/sizeof xs[0]; ++i) {\n" +
                  Checks + "} return 0; }\n");
  }
}

TEST(HighCIntegerConversion, ASignedCompareOfShiftsComparesSigned) {
  // AArch64 `cmp w14, w13, lsl #24; cset lt` compares two bytes as signed
  // chars by their shifted words.  Each shift happens in an unsigned
  // carrier, whatever its signed type says, so the signed compare converts
  // both: the low and second bytes of x, as signed chars.
  const TypeRef U32 = integer(4, false), I32 = integer(4, true);
  auto Low = lowPart(input(), U32);
  auto Second = op(NdOp::INT_RIGHT, lowPart(input(), U32), constant(8, 4), U32);
  auto Less =
      op(NdOp::INT_SLESS, op(NdOp::INT_LEFT, Low, constant(24, 4), I32),
         op(NdOp::INT_LEFT, Second, constant(24, 4), I32), integer(1, false));
  HighFunc F =
      function("signed_byte_less", integer(8, false),
               {result(extend(NdOp::INT_ZEXT, Less, integer(8, false)))});
  const std::string Source = emit(F);
  compileAndRun(Source + R"(
int main(void) {
  const uint64_t xs[] = {0x0000, 0x8001, 0x0180, 0x7F80, 0x807F, 0xFF00, 0x00FF};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i)
    if (signed_byte_less(xs[i]) != ((int8_t)xs[i] < (int8_t)(xs[i] >> 8)))
      return 1;
  return 0;
}
)");
}

TEST(HighCIntegerConversion, AConversionKeepingTheBytesNeedsNoCastOfItsOwn) {
  // t1 = x * 3 (read twice); return (uint32_t)(int32_t)t1 + (t1 >> 32):
  // the signed truncation inside the 32-bit sum keeps only bytes the
  // unsigned carrier keeps anyway.
  const TypeRef U32 = integer(4, false), I32 = integer(4, true);
  const TypeRef U64 = integer(8, false);
  HighFunc F = function(
      "mix", U32,
      {assign(local(1, U64), op(NdOp::INT_MULT, input(), constant(3), U64)),
       result(op(
           NdOp::INT_ADD, lowPart(local(1, U64), I32),
           lowPart(op(NdOp::INT_RIGHT, local(1, U64), constant(32), U64), U32),
           U32))});
  const std::string Source = emit(F);
  EXPECT_EQ(Source.find("(int32_t)"), std::string::npos) << Source;
  EXPECT_NE(Source.find("(uint32_t)t1 + "), std::string::npos) << Source;
  compileAndRun(Source + Inputs + R"(
int main(void) {
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint64_t t1 = xs[i] * 3;
    if (mix(xs[i]) != (uint32_t)((uint32_t)t1 + (uint32_t)(t1 >> 32)))
      return 1;
  }
  return 0;
}
)");
}

TEST(HighCIntegerConversion, AssignmentsAndReturnsConvertImplicitly) {
  // t1 = x; t2 = (int32_t)t1; t3 = (int64_t)t2; t4 = (uint64_t)(uint32_t)t1;
  // t5 = (t3 + t4) * (t3 - t4) + t2; return (int32_t)t5: each conversion is
  // the one C's assignment or return performs.  Every local is read twice.
  const TypeRef U64 = integer(8, false), I64 = integer(8, true);
  const TypeRef U32 = integer(4, false), I32 = integer(4, true);
  HighFunc F = function(
      "convert", I32,
      {assign(local(1, U64), input()),
       assign(local(2, I32), cast(lowPart(local(1, U64), U32), I32)),
       assign(local(3, I64), extend(NdOp::INT_SEXT, local(2, I32), I64)),
       assign(local(4, U64),
              extend(NdOp::INT_ZEXT, lowPart(local(1, U64), U32), U64)),
       assign(local(5, U64),
              op(NdOp::INT_ADD,
                 op(NdOp::INT_MULT,
                    op(NdOp::INT_ADD, local(3, I64), local(4, U64), U64),
                    op(NdOp::INT_SUB, local(3, I64), local(4, U64), U64), U64),
                 extend(NdOp::INT_SEXT, local(2, I32), I64), U64)),
       result(op(NdOp::INT_ADD, lowPart(local(5, U64), I32),
                 lowPart(local(5, U64), I32), I32))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find("t2 = t1;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t3 = t2;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t4 = (uint32_t)t1;"), std::string::npos) << Source;
  compileAndRun(Source + Inputs + R"(
static int32_t reference(uint64_t x) {
  const int32_t t2 = (int32_t)(uint32_t)x;
  const int64_t t3 = t2;
  const uint64_t t4 = (uint32_t)x;
  const uint64_t t5 =
      ((uint64_t)t3 + t4) * ((uint64_t)t3 - t4) + (uint64_t)(int64_t)t2;
  return (int32_t)((uint32_t)t5 + (uint32_t)t5);
}
int main(void) {
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i)
    if (convert(xs[i]) != reference(xs[i]))
      return 1;
  return 0;
}
)");
}

TEST(HighCIntegerConversion, ALiteralTakesTheValueItConvertsTo) {
  // t1 = (int8_t)0x1FF assigns -1; t2 = (uint16_t)-2 assigns 0xFFFE.  The
  // sum reads each local twice.
  const TypeRef I8 = integer(1, true), U16 = integer(2, false);
  const TypeRef U64 = integer(8, false);
  auto Sum = [&] {
    return op(NdOp::INT_ADD, extend(NdOp::INT_SEXT, local(1, I8), U64),
              extend(NdOp::INT_ZEXT, local(2, U16), U64), U64);
  };
  HighFunc F =
      function("literal", U64,
               {assign(local(1, I8), lowPart(constant(0x1FF, 4), I8)),
                assign(local(2, U16), lowPart(constant(~uint64_t{1}, 8), U16)),
                result(op(NdOp::INT_ADD, Sum(), Sum(), U64))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find("t1 = -1;"), std::string::npos) << Source;
  EXPECT_NE(Source.find("t2 = 0xFFFE;"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  return literal(0) == 2 * (0xFFFE - 1) ? 0 : 1;
}
)");
}

TEST(HighCIntegerConversion, APointerKeepsItsExplicitConversion) {
  // t1 is declared a pointer: converting it to an integer stays explicit,
  // since C has no implicit conversion from a pointer.
  const TypeRef U64 = integer(8, false), U32 = integer(4, false);
  HighFunc F =
      function("address", U32,
               {assign(local(1, NdType::makePtr()), input()),
                assign(local(2, U32), lowPart(local(1, U64), U32)),
                result(op(NdOp::INT_ADD, local(2, U32), local(2, U32), U32))});
  const std::string Source = emit(F);
  compileAndRun(Source + R"(
int main(void) {
  return address(0x100000004ull) == 8 ? 0 : 1;
}
)");
}

TEST(HighCIntegerConversion, UnprototypedImmediatesKeepTheirCompleteWord) {
  const TypeRef U64 = integer(8, false), I64 = integer(8, true);
  const TypeRef U32 = integer(4, false), I32 = integer(4, true);
  std::vector<std::pair<ExprPtr, uint64_t>> Cases;
  for (uint64_t Value :
       {UINT64_C(0), UINT64_C(1), UINT64_C(0x7fffffff), UINT64_C(0xffffffff),
        UINT64_C(0x100000000), UINT64_C(0xffffffffffffffff)})
    Cases.emplace_back(typed(constant(Value), U64), Value);
  Cases.emplace_back(typed(constant(UINT64_MAX), I64), UINT64_MAX);
  Cases.emplace_back(
      extend(NdOp::INT_ZEXT, typed(constant(UINT32_MAX, 4), U32), U64),
      UINT32_MAX);
  Cases.emplace_back(
      extend(NdOp::INT_SEXT, typed(constant(UINT32_MAX, 4), I32), I64),
      UINT64_MAX);
  Cases.emplace_back(cast(typed(constant(UINT32_MAX, 4), U32), U64),
                     UINT32_MAX);
  // An extension can change the high half of a literal printed positive or
  // negative; a negative 64-bit word other than -1 needs its carrier too.
  Cases.emplace_back(extend(NdOp::INT_SEXT,
                            typed(constant(UINT32_C(0x80000000), 4), I32), I64),
                     UINT64_C(0xffffffff80000000));
  Cases.emplace_back(extend(NdOp::INT_ZEXT,
                            typed(constant(UINT32_C(0x80000000), 4), I32), U64),
                     UINT64_C(0x80000000));
  Cases.emplace_back(typed(constant(UINT64_MAX - 1), I64), UINT64_MAX - 1);
  std::string Source, Checks;
  for (unsigned I = 0; I < Cases.size(); ++I) {
    const std::string Name = "untyped_word_" + std::to_string(I);
    auto Call = HighExpr::makeCall("indirect", 0x2000, {Cases[I].first});
    Call->Type = U64;
    Call->IsIndirectCall = true;
    Call->IndirectTarget = input();
    Source += emit(function(Name, U64, {result(Call)}));
    Checks += "if (" + Name + "(address) != UINT64_C(" +
              std::to_string(Cases[I].second) + ")) return 1;\n";
  }
  Source += R"(
static uint64_t actual(uint64_t value) { return value; }
int main(void) {
  uint64_t address=(uint64_t)(uintptr_t)&actual;
)" + Checks +
            "return 0;\n}\n";
  compileAndRun(Source);
}

TEST(HighCIntegerConversion, AWideningArgumentKeepsItsExtension) {
  // widen(uint64_t) receives the zero extension of an int32_t local: the
  // argument must not become the local itself, which C would sign-extend.
  class CalleeDbg : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
      if (Addr != 0x2000)
        return std::nullopt;
      FunctionSym FS;
      FS.Name = "widen";
      FS.Addr = Addr;
      FS.CallConv = DebugCallConv::Cdecl;
      FS.ReturnType = NdType::makeInt(8, false);
      FS.Params.emplace_back("value", NdType::makeInt(8, false));
      return FS;
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  const TypeRef U64 = integer(8, false), U32 = integer(4, false);
  const TypeRef I32 = integer(4, true);
  auto Call = HighExpr::makeCall("widen", 0x2000,
                                 {extend(NdOp::INT_ZEXT, local(1, I32), U64)});
  Call->Type = U64;
  HighFunc F =
      function("probe", U64,
               {assign(local(1, I32), cast(lowPart(input(), U32), I32)),
                assign(local(2, U64), Call),
                result(op(NdOp::INT_ADD, local(2, U64),
                          extend(NdOp::INT_SEXT, local(1, I32), U64), U64))});
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options, &Dbg));
  Out.flush();
  compileAndRun("#define __fastcall\n" + Source + R"(
uint64_t widen(uint64_t value) { return value; }
int main(void) {
  const uint32_t xs[] = {0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i)
    if (probe(xs[i]) != (uint64_t)xs[i] + (uint64_t)(int64_t)(int32_t)xs[i])
      return 1;
  return 0;
}
)");
}

TEST(HighCIntegerConversion, AnUntypedDebugCalleeReturnsItsWholeRegister) {
  // A public symbol names a function without its type.  Its result is the
  // whole return register: a 64-bit pointer keeps its upper half.
  class CalleeDbg : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
      if (Addr != 0x2000)
        return std::nullopt;
      FunctionSym FS;
      FS.Name = "lookup";
      FS.Addr = Addr;
      return FS;
    }
    bool hasInfo() const override { return true; }
  } Dbg;
  const TypeRef U64 = integer(8, false);
  HighFunc F =
      function("probe", U64,
               {assign(local(1, U64), HighExpr::makeCall("lookup", 0x2000, {})),
                result(op(NdOp::INT_ADD, local(1, U64), constant(1), U64))});
  std::string Source;
  llvm::raw_string_ostream Out(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({F}, Out, Options, &Dbg));
  Out.flush();
  EXPECT_NE(Source.find("extern uint64_t lookup();"), std::string::npos)
      << Source;
  compileAndRun(Source + R"(
uint64_t lookup(void) { return 0xFFFF800012345678ull; }
int main(void) { return probe(0) == 0xFFFF800012345679ull ? 0 : 1; }
)");
}

TEST(HighCIntegerConversion, AnUndeclaredCalleeReturnsWhatItsCallersRead) {
  // Nothing declares wide() or narrow().  The caller reads all eight bytes
  // of wide()'s result, which an int would cut in half; an int holds what it
  // reads of narrow()'s.
  const TypeRef U64 = integer(8, false), U32 = integer(4, false);
  HighFunc F =
      function("probe", U64,
               {assign(local(1, U64), HighExpr::makeCall("wide", 0x2000, {})),
                assign(local(2, U32), HighExpr::makeCall("narrow", 0x2010, {})),
                result(op(NdOp::INT_ADD, local(1, U64),
                          extend(NdOp::INT_ZEXT, local(2, U32), U64), U64))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find("extern int64_t wide();"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern int narrow();"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int64_t wide(void) { return INT64_C(0x7FFF800012345678); }
int narrow(void) { return 5; }
int main(void) { return probe(0) == 0x7FFF80001234567Dull ? 0 : 1; }
)");
}

TEST(HighCIntegerConversion, UndeclaredPairCallsKeepTheirCompleteReturnType) {
  const TypeRef U64 = integer(8, false), U128 = integer(16, false);
  auto Pair = typed(HighExpr::makeCall("pair", 0x2000, {input()}), U128);
  auto Scalar = typed(HighExpr::makeCall("scalar", 0x2010, {input()}), U64);
  const HighFunc F =
      function("probe", U64,
               {assign(local(1, U128), Pair), assign(local(2, U128), Scalar),
                result(op(NdOp::INT_XOR,
                          op(NdOp::SUBBYTES, local(1, U128), constant(8), U64),
                          lowPart(local(2, U128), U64), U64))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find("extern unsigned __int128 pair();"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("extern int64_t scalar();"), std::string::npos)
      << Source;
  compileAndRun(Source + R"C(
unsigned __int128 pair(uint64_t x) {
  return (unsigned __int128)(x + 17) << 64 | (x ^ 0x1234);
}
int64_t scalar(uint64_t x) { return x ^ 7; }
int main(void) {
  uint64_t xs[] = {0, 1, 0x123456789ABCDEF0, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(xs) / sizeof(xs[0]); ++i)
    if (probe(xs[i]) != ((xs[i] + 17) ^ (xs[i] ^ 7))) return 1;
  return 0;
}
)C");
}

TEST(HighCIntegerConversion, BitCountsCountInTheOperandsWidth) {
  // The machine counts a 32-bit register's leading zeros in 32 bits, and a
  // zero's as 32; C's builtins count in unsigned int or unsigned long long
  // and leave a zero's count undefined.  A negative 32-bit value has 32 bits
  // to count ones in, not 64.
  const TypeRef U64 = integer(8, false), U32 = integer(4, false);
  const TypeRef I32 = integer(4, true), U16 = integer(2, false);
  auto Count = [&](NdOp Op, ExprPtr Operand) {
    return extend(NdOp::INT_ZEXT,
                  typed(HighExpr::makeUnary(Op, std::move(Operand)), U32), U64);
  };
  auto Scaled = [&](ExprPtr Value, uint64_t Factor) {
    return op(NdOp::INT_MULT, std::move(Value), constant(Factor), U64);
  };
  HighFunc F = function(
      "counts", U64,
      {result(
          op(NdOp::INT_ADD,
             op(NdOp::INT_ADD, Count(NdOp::LZCOUNT, lowPart(input(), I32)),
                Scaled(Count(NdOp::LZCOUNT, lowPart(input(), U16)), 100), U64),
             op(NdOp::INT_ADD,
                Scaled(Count(NdOp::POPCOUNT, lowPart(input(), I32)), 10000),
                Scaled(typed(HighExpr::makeUnary(NdOp::LZCOUNT, input()), U64),
                       1000000),
                U64),
             U64))});
  const std::string Source = emit(F);
  compileAndRun(Source + R"(
static uint64_t leading(uint64_t v, int bits) {
  uint64_t n = 0;
  for (int i = bits - 1; i >= 0 && !((v >> i) & 1); --i)
    ++n;
  return n;
}
static uint64_t ones(uint64_t v) {
  uint64_t n = 0;
  for (; v; v >>= 1)
    n += v & 1;
  return n;
}
int main(void) {
  const uint64_t xs[] = {0, 1, 0x80000000ull, 0xFFFFFFFFull, 0x12345678ull,
                         0x8000000000000000ull, 0xFFFF0000FFFFull, 0x10000ull};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint64_t x = xs[i];
    const uint64_t want = leading(x & 0xFFFFFFFF, 32) +
                          100 * leading(x & 0xFFFF, 16) +
                          10000 * ones(x & 0xFFFFFFFF) +
                          1000000 * leading(x, 64);
    if (counts(x) != want)
      return 1;
  }
  return 0;
}
)");
}

TEST(HighCIntegerConversion, ASelectHasItsOwnSignedness) {
  // x / (d == 0 ? 1 : d), signed, with d an unsigned local: C would convert
  // both arms to uint64_t and divide unsigned.  Each arm takes the select's
  // type.
  const TypeRef U64 = integer(8, false), I64 = integer(8, true);
  auto Select = std::make_shared<HighExpr>();
  Select->Kind = ExprKind::BinOp;
  Select->Op = NdOp::SELECT;
  Select->Operands = {
      op(NdOp::INT_EQUAL, local(1, U64), constant(0), integer(1, false)),
      constant(1), local(1, U64)};
  Select->Type = I64;
  HighFunc F = function(
      "quotient", I64,
      {assign(local(1, U64), op(NdOp::INT_AND, input(), constant(15), U64)),
       result(op(NdOp::INT_SDIV, typed(input(), I64), Select, I64))});
  const std::string Source = emit(F);
  compileAndRun(Source + R"(
int main(void) {
  const int64_t xs[] = {-100, -1, 0, 7, 100, -16, 15};
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const int64_t d = (int64_t)((uint64_t)xs[i] & 15);
    if (quotient((uint64_t)xs[i]) != xs[i] / (d == 0 ? 1 : d))
      return 1;
  }
  return 0;
}
)");
}

TEST(HighCIntegerConversion, ALandingPadDeclaresWhatTheUnwinderSets) {
  // A landing pad reads the exception object the unwinder hands it; no C
  // statement assigns it, and the name is declared all the same.
  const TypeRef U64 = integer(8, false);
  MedVar Exception;
  Exception.Kind = MedVar::EHException;
  Exception.Size = 8;
  Exception.TheArch = Arch::X64;
  HighFunc F =
      function("pad", U64,
               {result(op(NdOp::INT_ADD, HighExpr::makeVar(Exception, U64),
                          input(), U64))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find(" eh_exception;"), std::string::npos) << Source;
}

TEST(HighCIntegerConversion, ATruthValueWidensWithoutAByteView) {
  // return (uint64_t)(-(uint32_t)(x == 5)) + (x < 3): a comparison is 0 or
  // 1 already, so neither extension needs the 1-byte view in between.
  const TypeRef U64 = integer(8, false), U32 = integer(4, false);
  const TypeRef U8 = integer(1, false);
  auto Mask = op(NdOp::INT_SUB, constant(0, 4),
                 extend(NdOp::INT_ZEXT,
                        op(NdOp::INT_EQUAL, input(), constant(5), U8), U32),
                 U32);
  HighFunc F = function(
      "truth", U64,
      {result(op(NdOp::INT_ADD, extend(NdOp::INT_ZEXT, Mask, U64),
                 extend(NdOp::INT_ZEXT,
                        op(NdOp::INT_LESS, input(), constant(3), U8), U64),
                 U64))});
  const std::string Source = emit(F);
  EXPECT_EQ(Source.find("(uint8_t)("), std::string::npos) << Source;
  compileAndRun(Source + Inputs + R"(
int main(void) {
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint64_t x = xs[i];
    const uint64_t want = (uint64_t)(uint32_t)(0u - (uint32_t)(x == 5)) + (x < 3);
    if (truth(x) != want)
      return 1;
  }
  if (truth(5) != 0xFFFFFFFFull || truth(1) != 1)
    return 2;
  return 0;
}
)");
}

TEST(HighCIntegerConversion, ACarryPrintsEachOperandOnce) {
  // return carry(x * 3, x + 5): the carry out of the 64-bit sum.  Each
  // operand prints once, so a chain of carries does not double per link.
  const TypeRef U64 = integer(8, false), U8 = integer(1, false);
  HighFunc F = function(
      "carry", U64,
      {result(extend(NdOp::INT_ZEXT,
                     op(NdOp::INT_CARRY,
                        op(NdOp::INT_MULT, input(), constant(3), U64),
                        op(NdOp::INT_ADD, input(), constant(5), U64), U8),
                     U64))});
  const std::string Source = emit(F);
  EXPECT_NE(Source.find("__builtin_add_overflow("), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("arg0 * 3"), Source.rfind("arg0 * 3")) << Source;
  compileAndRun(Source + Inputs + R"(
int main(void) {
  for (unsigned i = 0; i < sizeof xs / sizeof xs[0]; ++i) {
    const uint64_t a = xs[i] * 3, b = xs[i] + 5;
    if (carry(xs[i]) != (uint64_t)(a + b < a))
      return 1;
  }
  return 0;
}
)");
}

} // namespace
