#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedTypePass.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;
namespace {
MedVar regValue(int Id, uint64_t Register, int Version, Arch Architecture) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = Id;
  V.RegOff = Register;
  V.SSAVer = Version;
  V.Size = 8;
  V.TheArch = Architecture;
  return V;
}

HighFunc callArithmetic(Arch Architecture, bool Indirect, bool Subtract) {
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeInt(8, false);
  Hint.Parameters = {{"arg0", Hint.ReturnType}};
  std::string Reason;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Reason));
  MedFunc F;
  F.Name = std::string("call_") + (Indirect ? "indirect_" : "direct_") +
           (Subtract ? "subtract" : "multiply");
  F.Entry = 0x1000;
  F.SourceTypeHint = Hint;
  MedBlock B;
  B.Id = 0;
  B.StartAddr = 0x1000;
  B.EndAddr = 0x1010;
  const auto Input = regValue(1, TRI.IntParamRegs[0], 0, Architecture);
  // LowToMed represents an entry live-in by its leading self-copy. Without
  // that seed the signature's position exists, but this SSA value is unbound.
  MedOp EntryInput;
  EntryInput.Opcode = NdOp::COPY;
  EntryInput.Addr = F.Entry;
  EntryInput.Output = Input;
  EntryInput.addInput(Input);
  B.Ops.push_back(EntryInput);
  const auto Returned = regValue(2, TRI.IntReturnReg, 1, Architecture);
  auto Final = Returned;
  Final.SSAVer = 2;
  MedOp Call;
  Call.Opcode = Indirect ? NdOp::INDIR_CALL : NdOp::CALL;
  Call.Addr = 0x1000;
  Call.Output = Returned;
  Call.addInput(MedVar::makeConst(0x2000, 8));
  Call.addInput(Input);
  auto Binding = std::make_shared<SourceCallTypeHint>();
  Binding->CallKind = SourceCallTypeHint::Kind::Native;
  Binding->Signature = Hint;
  Binding->TargetAddress = 0x2000;
  Binding->TargetName = "helper";
  Call.SourceCallHint = Binding;
  B.Ops.push_back(Call);
  MedOp Arithmetic;
  Arithmetic.Opcode = Subtract ? NdOp::INT_SUB : NdOp::INT_MULT;
  Arithmetic.Addr = 0x1004;
  Arithmetic.Output = Final;
  Arithmetic.addInput(Returned);
  Arithmetic.addInput(MedVar::makeConst(Subtract ? 5 : 3, 8));
  B.Ops.push_back(Arithmetic);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1008;
  // Native arm64 RET reads LR, not the ABI's source return carrier. Force
  // lowerReturn's register-def recovery, where the arithmetic was lost.
  if (Architecture == Arch::AArch64)
    Return.addInput(regValue(4, 240, 0, Architecture));
  else
    Return.addInput(Final);
  B.Ops.push_back(Return);
  F.Blocks.push_back(B);
  inferMedTypes(F, Architecture);
  EXPECT_TRUE(F.SourceParametersBound);
  MedToHighConverter Converter;
  std::map<va_t, std::string> Names{{0x2000, "helper"}};
  Converter.setFuncNames(&Names);
  return Converter.convert(F, Architecture);
}

HighFunc predecessorReturnCarrier() {
  constexpr Arch Architecture = Arch::AArch64;
  const auto &TRI = getTargetRegInfo(Architecture);
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  Hint.ReturnType = NdType::makeInt(8, false);
  std::string Reason;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Reason));

  MedFunc F;
  F.Name = "predecessor_return_carrier";
  F.Entry = 0x1000;
  F.SourceTypeHint = Hint;

  const auto Loaded = regValue(1, 0, 0, Architecture);
  auto ReturnCarrier = regValue(2, TRI.IntReturnReg, 1, Architecture);
  MedVar Condition;
  Condition.Kind = MedVar::Temp;
  Condition.Id = 3;
  Condition.Size = 1;
  Condition.TheArch = Architecture;

  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = 0x1000;
  Entry.EndAddr = 0x100c;
  Entry.Succs = {1, 2};
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = 0x1000;
  Load.Output = Loaded;
  Load.addInput(MedVar::makeConst(0x4000, 8));
  Entry.Ops.push_back(Load);
  MedOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Addr = 0x1004;
  Copy.Output = ReturnCarrier;
  Copy.addInput(Loaded);
  Entry.Ops.push_back(Copy);
  MedOp Compare;
  Compare.Opcode = NdOp::INT_EQUAL;
  Compare.Addr = 0x1008;
  Compare.Output = Condition;
  Compare.addInput(Loaded);
  Compare.addInput(MedVar::makeConst(0, 8));
  Entry.Ops.push_back(Compare);
  MedOp Branch;
  Branch.Opcode = NdOp::COND_BR;
  Branch.Addr = 0x1008;
  Branch.addInput(MedVar::makeConst(0x1010, 8));
  Branch.addInput(Condition);
  Entry.Ops.push_back(Branch);

  MedBlock FastReturn;
  FastReturn.Id = 1;
  FastReturn.StartAddr = 0x100c;
  FastReturn.EndAddr = 0x1010;
  FastReturn.Preds = {0};
  MedOp BareReturn;
  BareReturn.Opcode = NdOp::RETURN;
  BareReturn.Addr = 0x100c;
  BareReturn.addInput(regValue(4, 240, 0, Architecture));
  FastReturn.Ops.push_back(BareReturn);

  MedBlock OtherReturn;
  OtherReturn.Id = 2;
  OtherReturn.StartAddr = 0x1010;
  OtherReturn.EndAddr = 0x1014;
  OtherReturn.Preds = {0};
  MedOp ExplicitReturn;
  ExplicitReturn.Opcode = NdOp::RETURN;
  ExplicitReturn.Addr = 0x1010;
  ExplicitReturn.addInput(ReturnCarrier);
  OtherReturn.Ops.push_back(ExplicitReturn);

  F.Blocks = {std::move(Entry), std::move(FastReturn), std::move(OtherReturn)};
  inferMedTypes(F, Architecture);
  EXPECT_TRUE(F.SourceParametersBound);
  return MedToHighConverter().convert(F, Architecture);
}

TEST(SourceCallReturns, IndirectCallOfReturnedPointerIsNotEntryParameter) {
  constexpr Arch Architecture = Arch::AArch64;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc F;
  F.Name = "invoke_returned_pointer";
  F.Entry = 0x1000;
  MedBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  B.EndAddr = 0x100c;
  const auto Input = regValue(1, TRI.IntParamRegs[0], 0, Architecture);
  const auto Returned = regValue(2, TRI.IntReturnReg, 1, Architecture);
  MedOp EntryInput;
  EntryInput.Opcode = NdOp::COPY;
  EntryInput.Output = Input;
  EntryInput.addInput(Input);
  B.Ops.push_back(EntryInput);
  MedOp Lookup;
  Lookup.Opcode = NdOp::CALL;
  Lookup.Addr = 0x1000;
  Lookup.Output = Returned;
  Lookup.addInput(MedVar::makeConst(0x2000, 8));
  B.Ops.push_back(Lookup);
  MedOp Invoke;
  Invoke.Opcode = NdOp::INDIR_CALL;
  Invoke.Addr = 0x1004;
  Invoke.Output = regValue(3, TRI.IntReturnReg, 2, Architecture);
  Invoke.addInput(Returned);
  B.Ops.push_back(Invoke);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = 0x1008;
  Return.addInput(regValue(4, 240, 0, Architecture));
  B.Ops.push_back(Return);
  F.Blocks.push_back(B);
  inferMedTypes(F, Architecture);
  const auto High = MedToHighConverter().convert(F, Architecture);
  const HighExpr *Indirect = nullptr;
  walkStmts(High.Body, [&](const HighStmt &Statement) {
    forEachExpr(Statement, [&](const ExprPtr &Expr) {
      if (Expr && Expr->IsIndirectCall)
        Indirect = Expr.get();
    });
  });
  ASSERT_NE(Indirect, nullptr);
  EXPECT_EQ(Indirect->IndirectParamIdx, -1);
  EXPECT_EQ(Indirect->CallTarget, "indirect");
  ASSERT_NE(Indirect->IndirectTarget, nullptr);
}

TEST(SourceCallReturns, DirectAndIndirectResultsKeepFollowingArithmeticOnce) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program));
  const std::string Compiler = *Program;
#endif
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    for (bool Indirect : {false, true})
      for (bool Subtract : {false, true})
        Functions.push_back(callArithmetic(Architecture, Indirect, Subtract));
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    Source += R"(
static unsigned calls;
uint64_t helper(uint64_t x) { ++calls; return x + 17; }
int main(void) {
  const uint64_t values[] = {0, 1, 5, UINT64_MAX, UINT64_C(0x8000000000000000)};
  for (unsigned i=0;i<5;++i) {
    unsigned before = calls;
    if (call_direct_multiply(values[i]) != (values[i]+17)*3) return 1;
    if (call_indirect_multiply(values[i]) != (values[i]+17)*3) return 2;
    if (call_direct_subtract(values[i]) != values[i]+17-5) return 3;
    if (call_indirect_subtract(values[i]) != values[i]+17-5) return 4;
    if (calls != before+4) return 5;
  }
  return 0;
}
)";
    llvm::SmallString<128> Input, Output, Errors;
    ASSERT_FALSE(
        llvm::sys::fs::createTemporaryFile("neverd-return-call", "c", Input));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-return-call", "exe",
                                                    Output));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-return-call", "err",
                                                    Errors));
    llvm::FileRemover R1(Input), R2(Output), R3(Errors);
    std::error_code EC;
    {
      llvm::raw_fd_ostream File(Input, EC);
      ASSERT_FALSE(EC);
      File << Source;
    }
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    const llvm::StringRef Arguments[] = {
        Compiler, "-std=c11", "-O1", "-Werror=uninitialized",
        Input,    "-o",       Output};
    std::string Error;
    int Compiled = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                             Redirects, 30, 0, &Error);
    auto Diagnostic = llvm::MemoryBuffer::getFile(Errors);
    ASSERT_EQ(Compiled, 0) << Error
                           << (Diagnostic ? (*Diagnostic)->getBuffer().str()
                                          : "")
                           << Source;
    ASSERT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt,
                                        Redirects, 30, 0, &Error),
              0)
        << Error << Source;
  }
}

TEST(SourceCallReturns, RecordPointerFieldsCompileAndPreserveAllWords) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program));
  const std::string Compiler = *Program;
#endif
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    const auto Word = NdType::makeInt(8, false);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    HighFunc Function;
    Function.Entry = 0x1000;
    Function.Name = "forwarded_pointer_words";
    Function.ReturnType = NdType::makeStruct({Word, Word, Pointer, Pointer});
    std::vector<ExprPtr> Values;
    for (unsigned I = 0; I < 4; ++I) {
      const auto Type = I < 2 ? Word : Pointer;
      Function.Params.push_back({"arg" + std::to_string(I), Type});
      MedVar Parameter;
      Parameter.Kind = MedVar::Param;
      Parameter.Id = I;
      Parameter.Size = 8;
      Parameter.RegOff = getTargetRegInfo(Architecture).IntParamRegs[I];
      Parameter.TheArch = Architecture;
      Values.push_back(HighExpr::makeVar(Parameter, Type));
    }
    HighStmt Return;
    Return.Kind = StmtKind::Return;
    Return.RetVal = HighExpr::makeRecord(Function.ReturnType, Values);
    Function.Body = {Return};
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    OS.flush();
    Source += R"(
int main(void) {
  unsigned char storage[256];
  uint64_t bits = UINT64_C(0x8000000000000001);
  for (unsigned i = 0; i < 512; ++i) {
    bits ^= bits << 13; bits ^= bits >> 7; bits ^= bits << 17;
    void *first = (i & 1) ? storage + (i & 255) : (void *)0;
    void *second = storage + ((i + 37) & 255);
    __auto_type result = forwarded_pointer_words(bits, ~bits, first, second);
    if (result.field_0 != bits || result.field_1 != ~bits ||
        result.field_2 != first || result.field_3 != second) return 1;
  }
  return 0;
}
)";
    llvm::SmallString<128> Input, Output, Errors;
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-record-pointer",
                                                    "c", Input));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-record-pointer",
                                                    "exe", Output));
    ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-record-pointer",
                                                    "err", Errors));
    llvm::FileRemover R1(Input), R2(Output), R3(Errors);
    std::error_code EC;
    {
      llvm::raw_fd_ostream File(Input, EC);
      ASSERT_FALSE(EC);
      File << Source;
    }
    const std::optional<llvm::StringRef> Redirects[] = {
        std::nullopt, std::nullopt, Errors.str()};
    for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization.str());
      const llvm::StringRef Arguments[] = {Compiler,
                                           "-std=c11",
                                           Optimization,
                                           "-Werror=int-conversion",
                                           "-Werror=uninitialized",
                                           Input,
                                           "-o",
                                           Output};
      std::string Error;
      const int Compiled = llvm::sys::ExecuteAndWait(
          Compiler, Arguments, std::nullopt, Redirects, 30, 0, &Error);
      const auto Diagnostic = llvm::MemoryBuffer::getFile(Errors);
      ASSERT_EQ(Compiled, 0)
          << Error << (Diagnostic ? (*Diagnostic)->getBuffer().str() : "")
          << Source;
      ASSERT_EQ(llvm::sys::ExecuteAndWait(Output, {Output}, std::nullopt,
                                          Redirects, 30, 0, &Error),
                0)
          << Error << Source;
    }
  }
}

TEST(SourceCallReturns, BareReturnUsesOnlyPredecessorsCarrierDefinition) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  ASSERT_TRUE(HighCEmitter().emit({predecessorReturnCarrier()}, OS, Options));
  EXPECT_NE(Source.find("return v1_0;"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("return;"), std::string::npos) << Source;
}

TEST(SourceCallReturns, BareReturnOfAZeroExtendingWriteIsTheWholeRegister) {
  // `add w0, w8, #14` zero-extends into x0, and the w0 view a loop reads
  // again follows it. A `long` result returned by a bare `ret` is x0, whose
  // upper half is zero, not the 32-bit view widened by its sign.
  constexpr Arch Architecture = Arch::AArch64;
  const auto &TRI = getTargetRegInfo(Architecture);
  auto Word = [&](int Id, uint64_t Register, int Version) {
    MedVar V = regValue(Id, Register, Version, Architecture);
    V.Size = 4;
    return V;
  };
  const MedVar Loaded = Word(2, 8 * 8, 1);
  const MedVar Sum = Word(4, TRI.IntReturnReg, 2);
  const MedVar Whole = regValue(1, TRI.IntReturnReg, 3, Architecture);
  const MedVar View = Word(4, TRI.IntReturnReg, 3);

  MedFunc F;
  F.Name = "zero_extended_result";
  F.Entry = 0x1000;
  MedBlock Body;
  Body.Id = 0;
  Body.StartAddr = 0x1000;
  Body.EndAddr = 0x100c;
  Body.Succs = {1};
  MedOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Addr = 0x1000;
  Load.Output = Loaded;
  Load.addInput(MedVar::makeConst(0x4000, 8));
  Body.Ops.push_back(Load);
  MedOp Add;
  Add.Opcode = NdOp::INT_ADD;
  Add.Addr = 0x1004;
  Add.Output = Sum;
  Add.addInput(Loaded);
  Add.addInput(MedVar::makeConst(14, 4));
  Body.Ops.push_back(Add);
  MedOp Extend;
  Extend.Opcode = NdOp::INT_ZEXT;
  Extend.Addr = 0x1004;
  Extend.Output = Whole;
  Extend.addInput(Sum);
  Body.Ops.push_back(Extend);
  MedOp Slice;
  Slice.Opcode = NdOp::SUBBYTES;
  Slice.Addr = 0x1008;
  Slice.Output = View;
  Slice.addInput(Whole);
  Slice.addInput(MedVar::makeConst(0, 4));
  Body.Ops.push_back(Slice);

  MedBlock Exit;
  Exit.Id = 1;
  Exit.StartAddr = 0x100c;
  Exit.EndAddr = 0x1010;
  Exit.Preds = {0};
  MedOp BareReturn;
  BareReturn.Opcode = NdOp::RETURN;
  BareReturn.Addr = 0x100c;
  BareReturn.addInput(regValue(5, 240, 0, Architecture));
  Exit.Ops.push_back(BareReturn);

  F.Blocks = {std::move(Body), std::move(Exit)};
  inferMedTypes(F, Architecture);
  ASSERT_TRUE(F.ReturnType);
  ASSERT_EQ(F.ReturnType->Size, 8u);
  HighFunc H = MedToHighConverter().convert(F, Architecture);

  const HighExpr *Returned = nullptr;
  walkStmts(H.Body, [&](HighStmt &S) {
    if (S.Kind == StmtKind::Return && S.RetVal)
      Returned = S.RetVal.get();
  });
  ASSERT_NE(Returned, nullptr);
  ASSERT_TRUE(Returned->Type);
  EXPECT_EQ(Returned->Type->Size, 8u);
  EXPECT_EQ(Returned->Kind, ExprKind::UnaryOp);
  EXPECT_EQ(Returned->Op, NdOp::INT_ZEXT);
}
} // namespace
