#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"

#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

using namespace neverd;

namespace {
MedVar temporary(int Id, Arch Architecture) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.Id = Id;
  Value.Size = 8;
  Value.TheArch = Architecture;
  return Value;
}

MedOp operation(NdOp Opcode, MedVar Output,
                std::initializer_list<MedVar> Inputs) {
  MedOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

MedFunc frameFunction(Arch Architecture, bool Used) {
  const auto &TRI = getTargetRegInfo(Architecture);
  MedFunc Function;
  Function.Entry = 0x1000;
  Function.Name = "frame_capture";
  Function.FrameSize = 64;
  Function.ReturnType = NdType::makeVoid();
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Hint.ReturnType = Function.ReturnType;
  Hint.Parameters = {{"out", NdType::makePtr(NdType::makeInt(8, false))},
                     {"delta", NdType::makeInt(8, false)}};
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Hint, Architecture, Error)) << Error;
  Function.SourceTypeHint = Hint;
  Function.SourceParametersBound = true;
  for (size_t I = 0; I < Hint.Parameters.size(); ++I) {
    auto Value = temporary(I, Architecture);
    Value.Kind = MedVar::Param;
    Value.RegOff = Hint.Parameters[I].Location.RegisterOffset;
    Function.Params.push_back(Value);
    Function.TypedParams.push_back(
        {Hint.Parameters[I].Name, Hint.Parameters[I].Type});
  }
  MedBlock Block;
  Block.Id = 0;
  auto SP = temporary(10, Architecture);
  SP.Kind = MedVar::Reg;
  SP.RegOff = TRI.StackPointer;
  auto Frame = SP;
  Frame.SSAVer = 1;
  Block.Ops.push_back(operation(NdOp::COPY, SP, {SP}));
  Block.Ops.push_back(
      operation(NdOp::INT_SUB, Frame, {SP, MedVar::makeConst(64, 8)}));
  auto First = temporary(20, Architecture);
  auto FirstNext = temporary(21, Architecture);
  auto Second = temporary(22, Architecture);
  auto SecondNext = temporary(23, Architecture);
  const MedVar Addresses[] = {First, FirstNext, Second, SecondNext};
  Block.Ops.push_back(
      operation(NdOp::INT_ADD, First, {Frame, MedVar::makeConst(8, 8)}));
  Block.Ops.push_back(
      operation(NdOp::INT_ADD, FirstNext, {First, MedVar::makeConst(8, 8)}));
  Block.Ops.push_back(
      operation(NdOp::INT_ADD, Second, {Frame, MedVar::makeConst(24, 8)}));
  Block.Ops.push_back(
      operation(NdOp::INT_ADD, SecondNext, {Second, MedVar::makeConst(8, 8)}));
  if (Used) {
    for (unsigned I = 0; I < 4; ++I) {
      auto Value = temporary(30 + I, Architecture);
      Block.Ops.push_back(
          operation(NdOp::INT_ADD, Value,
                    {Function.Params[1], MedVar::makeConst(11 * (I + 1), 8)}));
      Block.Ops.push_back(operation(NdOp::STORE, {}, {Addresses[I], Value}));
    }
    for (unsigned I = 0; I < 4; ++I) {
      auto Value = temporary(40 + I, Architecture);
      auto Destination = temporary(50 + I, Architecture);
      Block.Ops.push_back(operation(NdOp::LOAD, Value, {Addresses[I]}));
      Block.Ops.push_back(
          operation(NdOp::INT_ADD, Destination,
                    {Function.Params[0], MedVar::makeConst(I * 8, 8)}));
      Block.Ops.push_back(operation(NdOp::STORE, {}, {Destination, Value}));
    }
  } else {
    // Make both address definitions initially multi-use. All their consumers
    // are dead, so later DCE must remove the whole unused computation chain.
    Block.Ops.push_back(
        operation(NdOp::INT_ADD, temporary(60, Architecture), {First, Second}));
    Block.Ops.push_back(operation(NdOp::INT_ADD, temporary(61, Architecture),
                                  {FirstNext, SecondNext}));
  }
  Block.Ops.push_back(operation(NdOp::RETURN, {}, {}));
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program))
      << "clang is required for recovered source execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-frame-temp", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-temp", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-frame-temp", "err",
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
  llvm::SmallVector<llvm::StringRef, 12> Arguments{Compiler,
                                                   "-std=c11",
                                                   "-O1",
                                                   "-Werror=uninitialized",
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

TEST(HighFrameTemporaries, UsedStackAddressesSurviveCleanupAndExecute) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Function = MedToHighConverter().convert(
        frameFunction(Architecture, true), Architecture);
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    Options.EmitComments = false;
    ASSERT_TRUE(HighCEmitter().emit({Function}, OS, Options));
    compileAndRun(Source + R"(
int main(void) {
    const uint64_t values[] = {0, 7, 101, 65535};
    for (unsigned i = 0; i < 4; ++i) {
        uint64_t output[4] = {0};
        frame_capture(output, values[i]);
        for (unsigned j = 0; j < 4; ++j)
            if (output[j] != values[i] + 11 * (j + 1)) return 1;
    }
    return 0;
}
)");
  }
}

TEST(HighFrameTemporaries, DeadFrameAddressChainsStillDisappear) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Function = MedToHighConverter().convert(
        frameFunction(Architecture, false), Architecture);
    ASSERT_FALSE(Function.Body.empty());
    for (const auto &Statement : Function.Body)
      EXPECT_EQ(Statement.Kind, StmtKind::Return) << Function.Name;
  }
}
} // namespace
