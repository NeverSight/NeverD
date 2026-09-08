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
} // namespace
