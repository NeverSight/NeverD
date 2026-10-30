//===- MedMutableSourceTests.cpp - Mutable source semantics ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/ir/Limits.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedMutableSource.h"
#include "neverd/ir/med/MedSourceParameterUses.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"

#include <algorithm>
#include <stdexcept>

namespace {
using namespace neverd;

MedVar reg(int ID, uint64_t Offset, unsigned Size = 8) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.Id = ID;
  V.Size = Size;
  V.RegOff = Offset;
  V.TheArch = Arch::X64;
  return V;
}

MedVar temp(int ID, unsigned Size = 8) {
  auto V = reg(ID, 0, Size);
  V.Kind = MedVar::Temp;
  return V;
}

MedOp op(NdOp Code, MedVar Output, std::initializer_list<MedVar> Inputs = {}) {
  MedOp O;
  O.Opcode = Code;
  O.Output = Output;
  for (const auto &I : Inputs)
    O.addInput(I);
  return O;
}

MedOp branch(uint64_t Target) {
  return op(NdOp::BRANCH, {}, {MedVar::makeConst(Target, 8)});
}

void parameter(MedFunc &F, uint64_t Offset) {
  auto P = reg(100 + F.Params.size(), Offset);
  P.Kind = MedVar::Param;
  F.Params.push_back(P);
  F.TypedParams.push_back(
      {"p" + std::to_string(F.Params.size()), NdType::makeInt(8, false)});
}

MedFunc base(unsigned Blocks = 1) {
  MedFunc F;
  F.Entry = 0x1000;
  F.Name = "mutable_probe";
  F.CC = CallingConv::SysV_AMD64;
  F.ReturnType = NdType::makeInt(8, false);
  F.SkippedSSA = true;
  F.Blocks.resize(Blocks);
  for (unsigned I = 0; I < Blocks; ++I) {
    auto &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = F.Entry + 16 * I;
    B.EndAddr = B.StartAddr + 16;
  }
  return F;
}

void connect(MedFunc &F, int From, std::initializer_list<int> To) {
  F.Blocks[From].Succs = To;
  for (int ID : To)
    F.Blocks[ID].Preds.push_back(From);
}

MedFunc loop(bool ReverseBlocks = false) {
  auto F = base(4);
  parameter(F, x86reg::RDI);
  const auto N = reg(10, x86reg::RDI);
  const auto Sum = reg(11, x86reg::RAX);
  const auto Cond = temp(12, 1);
  F.Blocks[0].Ops = {op(NdOp::COPY, Sum, {MedVar::makeConst(0, 8)}),
                     branch(F.Blocks[1].StartAddr)};
  F.Blocks[1].Ops = {op(NdOp::INT_NOTEQUAL, Cond, {N, MedVar::makeConst(0, 8)}),
                     op(NdOp::COND_BR, {},
                        {MedVar::makeConst(F.Blocks[2].StartAddr, 8), Cond})};
  F.Blocks[2].Ops = {op(NdOp::INT_ADD, Sum, {Sum, N}),
                     op(NdOp::INT_SUB, N, {N, MedVar::makeConst(1, 8)}),
                     branch(F.Blocks[1].StartAddr)};
  F.Blocks[3].Ops = {op(NdOp::RETURN, {})};
  connect(F, 0, {1});
  connect(F, 1, {3, 2});
  connect(F, 2, {1});
  if (ReverseBlocks) {
    std::swap(F.Blocks[1], F.Blocks[2]);
    const auto Map = [](int ID) { return ID == 1 ? 2 : ID == 2 ? 1 : ID; };
    for (auto &B : F.Blocks) {
      B.Id = Map(B.Id);
      for (int &ID : B.Succs)
        ID = Map(ID);
      for (int &ID : B.Preds)
        ID = Map(ID);
    }
  }
  return F;
}

void compileSourceAndRun(const std::string &Source, size_t MaxSourceBytes,
                         bool ExpectTrap = false) {
  if (MaxSourceBytes)
    ASSERT_LE(Source.size(), MaxSourceBytes);
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for mutable source checks";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("mutable-source", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("mutable-source", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("mutable-source", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC);
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Opt : {"-O0", "-O2"}) {
    SCOPED_TRACE(Opt);
    std::string Error;
    const llvm::SmallVector<llvm::StringRef, 12> Args{
        Compiler, "-std=c11", Opt, "-fsanitize=undefined",
        // Decompiled C is built as its prelude says: for a target with
        // unaligned access, without strict aliasing.
        "-fno-sanitize=alignment", "-fno-strict-aliasing",
        "-fno-sanitize-recover=all", SourcePath, "-o", BinaryPath};
    int Status = llvm::sys::ExecuteAndWait(Compiler, Args, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "");
    Status = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 10, 0, &Error);
    if (ExpectTrap) {
      ASSERT_EQ(Status, -2) << Error << '\n' << Source;
      ASSERT_EQ(Error.find("timed out"), std::string::npos) << Error;
    } else {
      ASSERT_EQ(Status, 0) << Error << '\n' << Source;
    }
  }
}

void compileAndRun(const MedFunc &F, const char *Main,
                   size_t MaxSourceBytes = 0, bool OptimizeLLVM = false,
                   bool ExpectTrap = false) {
  llvm::LLVMContext Context;
  auto Module = MedLLVMEmitter().emit({F}, Context, F.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  if (OptimizeLLVM) {
    Pipeline::OptimizationOptions Options;
    const auto Result = Pipeline::optimizeModule(*Module, Options);
    ASSERT_NE(Result.Stop, OptimizationStopReason::InputInvalid);
    ASSERT_NE(Result.Stop, OptimizationStopReason::VerificationFailed);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
  }
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, Stream, CEmitterOptions{}));
  Stream.flush();
  {
    SCOPED_TRACE("LLVMC");
    compileSourceAndRun(Source + Main, MaxSourceBytes, ExpectTrap);
  }
  if (!F.SkippedSSA || OptimizeLLVM)
    return;
  SCOPED_TRACE("HighC");
  Source.clear();
  auto High = MedToHighConverter().convert(F, Arch::X64);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  compileSourceAndRun(Source + Main, MaxSourceBytes, ExpectTrap);
}

MedFunc alignmentCheck(bool Guarded, uint64_t Alignment = 16) {
  auto F = base();
  parameter(F, x86reg::RDI);
  const auto Address = reg(10, x86reg::RDI);
  const auto Result = reg(11, x86reg::RAX);
  MedOp Check = op(NdOp::INTRINSIC, {},
                   {MedVar::makeConst(uint64_t(Intrinsic::RequireAligned), 2),
                    Address, MedVar::makeConst(Alignment, 8)});
  if (Guarded) {
    parameter(F, x86reg::RSI);
    const auto Active = temp(12, 1);
    F.Blocks[0].Ops.push_back(op(NdOp::COPY, Active, {reg(13, x86reg::RSI)}));
    Check.addInput(Active);
  }
  F.Blocks[0].Ops.push_back(Check);
  F.Blocks[0].Ops.push_back(
      op(NdOp::INT_ADD, Result, {Address, MedVar::makeConst(1, 8)}));
  F.Blocks[0].Ops.push_back(op(NdOp::RETURN, {}));
  return F;
}

TEST(MedMutableSource, AlignmentChecksRetainTheirPredicateAndInputs) {
  for (bool Guarded : {false, true}) {
    auto F = alignmentCheck(Guarded);
    std::string Error;
    auto Plan = analyzeMedMutableSource(F, Arch::X64, &Error);
    ASSERT_TRUE(Plan) << Error;
    EXPECT_EQ(Plan->EntryBytes.at(x86reg::RDI), 255u);
    compileAndRun(F, Guarded ? R"(
int main(void) {
  const uint64_t values[]={0,16,UINT64_C(0x8000000000000000)};
  for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
    if(mutable_probe(values[i],1)!=values[i]+1 ||
       mutable_probe(values[i],255)!=values[i]+1) return 1;
  return mutable_probe(3,0)!=4 || mutable_probe(UINT64_MAX,0)!=0;
}
)"
                             : R"(
int main(void) {
  const uint64_t values[]={0,16,UINT64_C(0x8000000000000000)};
  for(unsigned i=0;i<sizeof(values)/sizeof(*values);++i)
    if(mutable_probe(values[i])!=values[i]+1) return 1;
  return 0;
}
)");
    compileAndRun(F,
                  Guarded ? "int main(void) { mutable_probe(3,1); return 0; }"
                          : "int main(void) { mutable_probe(3); return 0; }",
                  0, false, true);
  }
}

TEST(MedMutableSource, AlignmentChecksRejectMalformedOrImplicitShapes) {
  auto Good = alignmentCheck(true);
  for (unsigned Kind = 0; Kind != 9; ++Kind) {
    auto F = Good;
    auto &Check = F.Blocks[0].Ops[1];
    switch (Kind) {
    case 0:
      Check.Inputs[2] = MedVar::makeConst(0, 8);
      break;
    case 1:
      Check.Inputs[2] = MedVar::makeConst(3, 8);
      break;
    case 2:
      Check.Inputs[2] = reg(10, x86reg::RDI);
      break;
    case 3:
      Check.Inputs[1].Size = 4;
      break;
    case 4:
      Check.Inputs[3].Size = 8;
      break;
    case 5:
      Check.Inputs[0].Size = 8;
      break;
    case 6:
      Check.Output = temp(20);
      break;
    case 7:
      Check.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 8:
      Check.NumInputs = 2;
      break;
    }
    std::string Error;
    EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64, &Error));
    EXPECT_FALSE(Error.empty());
    EXPECT_THROW(MedToHighConverter().convert(F, Arch::X64),
                 std::runtime_error);
    llvm::LLVMContext Context;
    EXPECT_THROW(MedLLVMEmitter().emit({F}, Context), std::runtime_error);
  }
}

TEST(MedMutableSource, LoopRequiresIncomingParameterRegardlessOfBlockOrder) {
  for (bool Reverse : {false, true}) {
    auto F = loop(Reverse);
    // Original PCs may repeat across specialized blocks. Only the block
    // identities, not these operation origins, are valid branch labels.
    for (auto &B : F.Blocks)
      for (auto &O : B.Ops)
        O.Addr = F.Entry;
    std::string Error;
    auto Plan = analyzeMedMutableSource(F, Arch::X64, &Error);
    ASSERT_TRUE(Plan) << Error;
    EXPECT_EQ(Plan->EntryBytes,
              (std::map<uint64_t, uint64_t>{{x86reg::RDI, 255}}));
    compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 100; ++n)
    if (mutable_probe(n) != n * (n + 1) / 2) return 1;
  return 0;
}

)");
  }
}

TEST(MedMutableSource, StraightLineBoundariesFollowExplicitEdgesInAnyOrder) {
  for (bool Reverse : {false, true}) {
    auto F = loop(Reverse);
    F.Blocks[0].Ops.pop_back();
    F.Blocks[Reverse ? 1 : 2].Ops.pop_back();
    std::string Error;
    ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64, &Error)) << Error;
    compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 100; ++n)
    if (mutable_probe(n) != n * (n + 1) / 2) return 1;
  return 0;
}
)");

    // Keep symmetric, in-range CFG metadata while removing or making the
    // continuation ambiguous. Neither case may invent a fallthrough target.
    for (bool Ambiguous : {false, true}) {
      auto Bad = F;
      if (Ambiguous) {
        Bad.Blocks[0].Succs.push_back(3);
        Bad.Blocks[3].Preds.push_back(0);
      } else {
        const int Next = Bad.Blocks[0].Succs.front();
        auto &Preds = Bad.Blocks[Next].Preds;
        Preds.erase(std::remove(Preds.begin(), Preds.end(), 0), Preds.end());
        Bad.Blocks[0].Succs.clear();
      }
      EXPECT_FALSE(analyzeMedMutableSource(Bad, Arch::X64));
      llvm::LLVMContext Context;
      EXPECT_THROW(MedLLVMEmitter().emit({Bad}, Context, Bad.Name, Arch::X64),
                   std::runtime_error);
      EXPECT_THROW(MedToHighConverter().convert(Bad, Arch::X64),
                   std::runtime_error);
    }
  }
}

MedFunc longMutableChain(unsigned Blocks) {
  auto F = base(Blocks);
  parameter(F, x86reg::RDI);
  const auto R = reg(11, x86reg::RAX);
  F.Blocks.front().Ops = {op(NdOp::COPY, R, {reg(10, x86reg::RDI)})};
  for (unsigned I = 1; I < Blocks; ++I)
    F.Blocks[I].Ops.push_back(
        op(NdOp::INT_ADD, R, {R, MedVar::makeConst(1, 8)}));
  for (unsigned I = 0; I + 1 < Blocks; ++I) {
    F.Blocks[I].Ops.push_back(branch(F.Blocks[I + 1].StartAddr));
    connect(F, I, {int(I + 1)});
  }
  F.Blocks.back().Ops.push_back(op(NdOp::RETURN, {}));
  return F;
}

TEST(MedMutableSource, LargeNarrowGraphsKeepExactRuntimeValues) {
  const unsigned Blocks = limits::ir::kMedMutableSourceMaxBlocks / 2;
  auto F = longMutableChain(Blocks);
  std::string Error;
  auto Plan = analyzeMedMutableSource(F, Arch::X64, &Error);
  ASSERT_TRUE(Plan) << Error;
  EXPECT_EQ(Plan->EntryBytes,
            (std::map<uint64_t, uint64_t>{{x86reg::RDI, 255}}));
  const auto Main =
      "\nint main(void) {\n"
      "const uint64_t inputs[]={0,7,UINT64_MAX};\n"
      "for(unsigned i=0;i<3;++i) if(mutable_probe(inputs[i]) != inputs[i]+" +
      std::to_string(Blocks - 1) + "ULL) return 1; return 0; }\n";
  compileAndRun(F, Main.c_str());
}

TEST(MedMutableSource, LargerBlockCeilingRetainsIndependentStorageBudgets) {
  auto Exact = longMutableChain(limits::ir::kMedMutableSourceMaxBlocks);
  std::string Error;
  ASSERT_TRUE(analyzeMedMutableSource(Exact, Arch::X64, &Error)) << Error;
  auto TooMany = base(limits::ir::kMedMutableSourceMaxBlocks + 1);
  EXPECT_FALSE(analyzeMedMutableSource(TooMany, Arch::X64, &Error));
  EXPECT_NE(Error.find("block budget"), std::string::npos);

  auto Matrix = longMutableChain(limits::ir::kMedMutableSourceMaxBlocks / 2);
  const size_t Values =
      limits::ir::kMedMutableSourceMaxBlockValues / Matrix.Blocks.size();
  // RDI and RAX already occupy two identities. Extra definitions need no
  // liveness propagation, but still count toward matrix allocation.
  ASSERT_GT(Values, 2U);
  auto &Ops = Matrix.Blocks.front().Ops;
  for (size_t I = 0; I != Values - 2; ++I)
    Ops.insert(std::prev(Ops.end()),
               op(NdOp::COPY, temp(1000 + I), {MedVar::makeConst(I, 8)}));
  ASSERT_TRUE(analyzeMedMutableSource(Matrix, Arch::X64, &Error)) << Error;
  Ops.insert(std::prev(Ops.end()),
             op(NdOp::COPY, temp(1000 + Values), {MedVar::makeConst(0, 8)}));
  EXPECT_FALSE(analyzeMedMutableSource(Matrix, Arch::X64, &Error));
  EXPECT_NE(Error.find("storage matrix budget"), std::string::npos);
}

TEST(MedMutableSource, EntryBackedgeCannotReinitializeMutableArgument) {
  auto F = base(2);
  parameter(F, x86reg::RDI);
  auto N = reg(10, x86reg::RDI);
  auto R = reg(11, x86reg::RAX);
  F.Blocks[0].Ops = {
      op(NdOp::INT_ADD, N, {N, MedVar::makeConst(1, 8)}),
      op(NdOp::INT_LESS, temp(12, 1), {N, MedVar::makeConst(10, 8)}),
      op(NdOp::COND_BR, {}, {MedVar::makeConst(F.Entry, 8), temp(12, 1)})};
  F.Blocks[1].Ops = {op(NdOp::COPY, R, {N}), op(NdOp::RETURN, {})};
  connect(F, 0, {1, 0});
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 20; ++n)
    if (mutable_probe(n) != (n < 10 ? 10 : n + 1)) return 1;
  return 0;
}
)");
}

TEST(MedMutableSource, MutableStackRegisterUsesRuntimeMemoryAndArithmetic) {
  auto F = base();
  parameter(F, x86reg::RDI);
  parameter(F, x86reg::RSI);
  auto Ptr = reg(10, x86reg::RDI);
  auto Delta = reg(11, x86reg::RSI);
  auto SP = reg(12, x86reg::RSP);
  auto R = reg(13, x86reg::RAX);
  F.Blocks[0].Ops = {op(NdOp::COPY, SP, {Ptr}),
                     op(NdOp::STORE, {}, {SP, MedVar::makeConst(42, 8)}),
                     op(NdOp::INT_SUB, SP, {SP, Delta}),
                     op(NdOp::STORE, {}, {SP, MedVar::makeConst(17, 8)}),
                     op(NdOp::LOAD, R, {SP}),
                     op(NdOp::INT_ADD, SP, {SP, Delta}),
                     op(NdOp::LOAD, temp(14), {SP}),
                     op(NdOp::INT_ADD, R, {R, temp(14)}),
                     op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  uint64_t words[2] = {0, 0};
  if (mutable_probe((uintptr_t)&words[1], 8) != 59) return 1;
  return words[0] != 17 || words[1] != 42;
}
)");
}

TEST(MedMutableSource, ExplicitSourcePointerIsCapturedBeforeEntryBackedges) {
  auto F = base(2);
  parameter(F, x86reg::RDI);
  F.TypedParams[0].Type = NdType::makePtr(NdType::makeInt(8, false));
  SourceFunctionTypeHint Hint;
  Hint.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Hint.Architecture = Arch::X64;
  Hint.HasExplicitABI = true;
  Hint.ReturnType = F.ReturnType;
  Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister, x86reg::RAX, 0,
                         8};
  Hint.Parameters.push_back(
      {"words",
       F.TypedParams[0].Type,
       {SourceABICarrierKind::IntegerRegister, x86reg::RDI, 0, 8}});
  F.SourceTypeHint = Hint;
  const auto P = reg(10, x86reg::RDI);
  const auto Word = temp(12), Condition = temp(13, 1);
  F.Blocks[0].Ops = {
      op(NdOp::LOAD, Word, {P}),
      op(NdOp::INT_ADD, P, {P, MedVar::makeConst(8, 8)}),
      op(NdOp::INT_NOTEQUAL, Condition, {Word, MedVar::makeConst(0, 8)}),
      op(NdOp::COND_BR, {}, {MedVar::makeConst(F.Entry, 8), Condition})};
  F.Blocks[1].Ops = {op(NdOp::COPY, reg(11, x86reg::RAX), {P}),
                     op(NdOp::RETURN, {})};
  connect(F, 0, {1, 0});
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  uint64_t words[] = {1, UINT64_MAX, 9, 0};
  for (unsigned start = 0; start != 4; ++start)
    if (mutable_probe(&words[start]) != (uintptr_t)&words[4]) return 1;
  return words[0] != 1 || words[1] != UINT64_MAX || words[2] != 9 || words[3];
}
)");
  F.SourceTypeHint->Parameters[0].Location.RegisterOffset = x86reg::RSI;
  EXPECT_THROW(MedToHighConverter().convert(F, Arch::X64), std::runtime_error);
  F.SourceTypeHint = Hint;
  F.SourceTypeHint->ReturnLocation.RegisterOffset = x86reg::RDX;
  EXPECT_THROW(MedToHighConverter().convert(F, Arch::X64), std::runtime_error);
}

TEST(MedMutableSource, OneSidedDefinitionCannotHideUndefinedEntryTemporary) {
  auto F = loop();
  F.Blocks[2].Ops.insert(F.Blocks[2].Ops.begin(),
                         op(NdOp::COPY, temp(20), {MedVar::makeConst(7, 8)}));
  F.Blocks[3].Ops.insert(F.Blocks[3].Ops.begin(),
                         op(NdOp::COPY, reg(11, x86reg::RAX), {temp(20)}));
  EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64));
  F.Blocks[0].Ops.insert(F.Blocks[0].Ops.begin(),
                         op(NdOp::COPY, temp(20), {MedVar::makeConst(3, 8)}));
  EXPECT_TRUE(analyzeMedMutableSource(F, Arch::X64));
}

TEST(MedMutableSource, DeadAndPlaceholderOutputsCannotDefineTheReturn) {
  for (bool Dead : {false, true}) {
    auto F = base();
    auto O = op(Dead ? NdOp::COPY : NdOp::NOP, reg(10, x86reg::RAX),
                {MedVar::makeConst(0, 8)});
    O.Dead = Dead;
    F.Blocks[0].Ops = {O, op(NdOp::RETURN, {})};
    EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64));
  }
}

TEST(MedMutableSource, RejectsAmbiguousStorageAndUnnormalizedControl) {
  for (unsigned Case = 0; Case < 24; ++Case) {
    SCOPED_TRACE(Case);
    auto F = loop();
    auto &B = F.Blocks[2];
    switch (Case) {
    case 0:
      B.Ops[0].Output.Size = 4;
      break;
    case 1:
      B.Ops[0].Output.Kind = MedVar::Temp;
      break;
    case 2:
      B.Ops[0].Output.SSAVer = 1;
      break;
    case 3:
      B.Ops[0].Output.RegOff = x86reg::RDX;
      break;
    case 4:
      B.Ops.insert(B.Ops.begin(), op(NdOp::COPY, reg(20, x86reg::RDI + 1, 1),
                                     {MedVar::makeConst(0, 1)}));
      break;
    case 5:
      B.Preds.clear();
      break;
    case 6:
      B.Id = 30;
      break;
    case 7:
      B.Ops.insert(B.Ops.begin(), op(NdOp::RETURN, {}));
      break;
    case 8:
      B.Ops[0].Opcode = NdOp::CALL;
      break;
    case 9:
      B.Phis.push_back({});
      break;
    case 10:
      F.ScalarAddressModels.push_back({});
      break;
    case 11:
      F.JumpTables.push_back({});
      break;
    case 12:
      B.Ops[0].IntrinsicOutputs.push_back(temp(30));
      break;
    case 13:
      F.DoesNotReturn = true;
      break;
    case 14:
      F.TypedParams[0].Type = NdType::makeInt(4, false);
      break;
    case 15:
      F.Params[0].Size = 4;
      F.TypedParams[0].Type = NdType::makeInt(4, false);
      break;
    case 16:
      B.Ops[0] = op(NdOp::SUBBYTES, temp(30, 1),
                    {reg(10, x86reg::RDI), reg(10, x86reg::RDI)});
      break;
    case 17:
      B.Ops[0] = op(NdOp::SUBBYTES, temp(30, 1),
                    {reg(10, x86reg::RDI), MedVar::makeConst(8, 8)});
      break;
    case 18:
      B.Ops[0] = op(NdOp::INT_ASHR, temp(30, 1),
                    {MedVar::makeConst(128, 1), MedVar::makeConst(1, 8)});
      break;
    case 19:
      B.Ops[0] = op(NdOp::INT_EQUAL, temp(30, 1),
                    {MedVar::makeConst(255, 1), MedVar::makeConst(~0ULL, 8)});
      break;
    case 20:
      F.FrameSize = 128;
      break;
    case 21:
      B.Ops[0].Inputs[0] =
          MedVar::makeConst(42, 8, ConstantAddressProvenance::AddressFragment);
      break;
    case 22:
      B.Ops[0].ReturnAddressRead = true;
      break;
    case 23:
      B.Ops[0].RegistrationRoot =
          MedOp::RegistrationRootKind::EstablishedFramePointer;
      break;
    }
    EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64));
    EXPECT_THROW(MedToHighConverter().convert(F, Arch::X64),
                 std::runtime_error);
  }
}

TEST(MedMutableSource, BooleanInputsAreTruthValuesAndCountsMayWiden) {
  for (NdOp Code : {NdOp::BOOL_AND, NdOp::BOOL_OR, NdOp::BOOL_XOR,
                    NdOp::POPCOUNT, NdOp::LZCOUNT}) {
    auto F = base();
    auto R = reg(11, x86reg::RAX);
    const bool Count = Code == NdOp::POPCOUNT || Code == NdOp::LZCOUNT;
    auto O = op(Code, R, {MedVar::makeConst(2, 1)});
    if (!Count)
      O.addInput(MedVar::makeConst(4, 1));
    F.Blocks[0].Ops = {O, op(NdOp::RETURN, {})};
    ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
    const unsigned Expected = Code == NdOp::BOOL_XOR  ? 0
                              : Code == NdOp::LZCOUNT ? 6
                                                      : 1;
    const auto Main = "\nint main(void) { return mutable_probe() != " +
                      std::to_string(Expected) + "; }\n";
    for (bool Mutable : {false, true}) {
      F.SkippedSSA = Mutable;
      compileAndRun(F, Main.c_str());
    }
  }
}

TEST(MedMutableSource, WideBitMasksKeepTheirExactRawValue) {
  auto F = base();
  auto Wide = temp(10, 16);
  F.Blocks[0].Ops = {
      op(NdOp::CONCAT, Wide,
         {MedVar::makeConst(1, 8), MedVar::makeConst(0, 8)}),
      op(NdOp::INT_AND, Wide, {Wide, MedVar::makeConst(~0ULL, 16)}),
      op(NdOp::SUBBYTES, reg(11, x86reg::RAX), {Wide, MedVar::makeConst(8, 8)}),
      op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, "\nint main(void) { return mutable_probe() != 0; }\n");
}

TEST(MedMutableSource, WideDivisionRetainsBothDividendAndDivisorHalves) {
  for (NdOp Code :
       {NdOp::INT_DIV, NdOp::INT_REM, NdOp::INT_SDIV, NdOp::INT_SREM}) {
    auto F = base();
    const bool Signed = Code == NdOp::INT_SDIV || Code == NdOp::INT_SREM;
    auto N = temp(10, 16), D = temp(11, 16), Result = temp(12, 16);
    F.Blocks[0].Ops = {
        op(NdOp::CONCAT, N,
           {MedVar::makeConst(Signed ? ~0ULL : 1, 8), MedVar::makeConst(0, 8)}),
        op(NdOp::CONCAT, D, {MedVar::makeConst(1, 8), MedVar::makeConst(1, 8)}),
        op(Code, Result, {N, D}),
        op(NdOp::SUBBYTES, temp(13), {Result, MedVar::makeConst(0, 8)}),
        op(NdOp::SUBBYTES, temp(14), {Result, MedVar::makeConst(8, 8)}),
        op(NdOp::INT_XOR, reg(15, x86reg::RAX), {temp(13), temp(14)}),
        op(NdOp::RETURN, {})};
    ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
    const char *Expected = Code == NdOp::INT_REM    ? "1"
                           : Code == NdOp::INT_SREM ? "UINT64_MAX"
                                                    : "0";
    const auto Main =
        std::string("\nint main(void) { return mutable_probe() != ") +
        Expected + "; }\n";
    compileAndRun(F, Main.c_str());
  }
}

TEST(MedMutableSource, DeclaredParameterWithoutAnSSASeedStillBindsItsCarrier) {
  auto F = base();
  parameter(F, x86reg::RDI);
  F.Params[0].Id = -1;
  F.Blocks[0].Ops = {
      op(NdOp::COPY, reg(11, x86reg::RAX), {reg(10, x86reg::RDI)}),
      op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  return mutable_probe(UINT64_C(0xfedcba9876543210)) !=
         UINT64_C(0xfedcba9876543210);
}
)");
}

TEST(MedMutableSource, NarrowProductsAndConstantShiftsAvoidSignedPromotion) {
  auto F = base();
  parameter(F, x86reg::RDI);
  auto Word = temp(20, 2);
  F.Blocks[0].Ops = {
      op(NdOp::SUBBYTES, Word, {reg(10, x86reg::RDI), MedVar::makeConst(0, 8)}),
      op(NdOp::INT_MULT, Word, {Word, Word}),
      op(NdOp::INT_ZEXT, reg(11, x86reg::RAX), {Word}), op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint32_t n = 0; n <= 65535; ++n)
    if (mutable_probe(n) != ((n * n) & 65535)) return 1;
  return 0;
}
)");
  F.Blocks[0].Ops = {op(NdOp::INT_LEFT, reg(11, x86reg::RAX),
                        {MedVar::makeConst(~0ULL, 8), reg(10, x86reg::RDI)}),
                     op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (unsigned n = 0; n < 64; ++n)
    if (mutable_probe(n) != (UINT64_MAX << n)) return 1;
  return 0;
}
)");
}

TEST(MedMutableSource, EarlierReadsSurviveLaterParameterAndConstantStores) {
  auto F = base();
  parameter(F, x86reg::RDI);
  auto P = reg(10, x86reg::RDI);
  auto R = reg(11, x86reg::RAX);
  F.Blocks[0].Ops = {op(NdOp::COPY, temp(20), {P}),
                     op(NdOp::COPY, P, {MedVar::makeConst(9, 8)}),
                     op(NdOp::COPY, R, {MedVar::makeConst(1, 8)}),
                     op(NdOp::COPY, temp(21), {R}),
                     op(NdOp::COPY, R, {MedVar::makeConst(2, 8)}),
                     op(NdOp::INT_ADD, R, {R, temp(21)}),
                     op(NdOp::INT_ADD, R, {R, temp(20)}),
                     op(NdOp::INT_ADD, R, {R, P}),
                     op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 100; ++n)
    if (mutable_probe(n) != n + 12) return 1;
  return 0;
}
)");
}

TEST(MedMutableSource, RepeatedUpdatesKeepCExpressionExpansionBounded) {
  auto F = base();
  parameter(F, x86reg::RDI);
  auto R = reg(11, x86reg::RAX);
  F.Blocks[0].Ops.push_back(op(NdOp::COPY, R, {reg(10, x86reg::RDI)}));
  for (unsigned I = 0; I < 512; ++I)
    F.Blocks[0].Ops.push_back(
        op(NdOp::INT_ADD, R, {R, MedVar::makeConst(1, 8)}));
  F.Blocks[0].Ops.push_back(op(NdOp::RETURN, {}));
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 100; ++n)
    if (mutable_probe(n) != n + 512) return 1;
  return mutable_probe(UINT64_MAX) != 511;
}
)",
                100000);
  F.Blocks[0].Ops.resize(1);
  for (unsigned I = 0; I < 64; ++I)
    F.Blocks[0].Ops.push_back(op(NdOp::INT_ADD, R, {R, R}));
  F.Blocks[0].Ops.push_back(op(NdOp::RETURN, {}));
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n < 100; ++n)
    if (mutable_probe(n) != 0) return 1;
  return mutable_probe(UINT64_MAX) != 0;
}
)",
                32768);
}

TEST(MedMutableSource, LeadingZeroCountIncludesZeroAtEveryScalarWidth) {
  for (unsigned Width : {1u, 2u, 4u, 8u}) {
    auto F = base();
    parameter(F, x86reg::RDI);
    F.Blocks[0].Ops = {
        op(NdOp::SUBBYTES, temp(20, Width),
           {reg(10, x86reg::RDI), MedVar::makeConst(0, 8)}),
        op(NdOp::LZCOUNT, reg(11, x86reg::RAX), {temp(20, Width)}),
        op(NdOp::RETURN, {})};
    ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
    std::string Main = "\nint main(void) {\n  const unsigned bits = " +
                       std::to_string(Width * 8) + R"(;
  for (unsigned bit = 0; bit < bits; ++bit)
    if (mutable_probe(UINT64_C(1) << bit) != bits - bit - 1) return 1;
  return mutable_probe(0) != bits;
}
)";
    compileAndRun(F, Main.c_str());
  }
}

TEST(MedMutableSource, RelocationAndCompositeStateBudgetsFailClosed) {
  llvm::LLVMContext Context;
  auto F = loop();
  BinaryImage Image;
  Image.Arch = Arch::X64;
  EXPECT_THROW(
      MedLLVMEmitter().emit({F}, Context, F.Name, Arch::X64, {}, &Image),
      std::runtime_error);
  MedToHighConverter High;
  High.setBinaryImage(&Image);
  EXPECT_THROW(High.convert(F, Arch::X64), std::runtime_error);
  F = base(2049);
  for (auto &B : F.Blocks)
    B.Ops.push_back(op(NdOp::RETURN, {}));
  for (int I = 0; I < 2048; ++I)
    F.Blocks[0].Ops.insert(
        F.Blocks[0].Ops.begin(),
        op(NdOp::COPY, temp(I + 10), {MedVar::makeConst(I, 8)}));
  EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64));
}

TEST(MedMutableSource, EmitterRejectsMalformedInputsBeforeModulePrepasses) {
  llvm::LLVMContext Context;
  MedLLVMEmitter Emitter;
  for (unsigned Case = 0; Case < 8; ++Case) {
    SCOPED_TRACE(Case);
    auto F = loop();
    switch (Case) {
    case 0:
      F.Blocks[0].Ops[0].Inputs.clear();
      break;
    case 1:
      F.Blocks[0].Ops.back().Inputs[0].Size = 1;
      break;
    case 2:
      F.Blocks[1].Ops.back().Inputs[0].Size = 1;
      break;
    case 3:
      F.Blocks[1].Preds.resize(4097, 0);
      break;
    case 4:
      F = base();
      parameter(F, x86reg::XMM0);
      F.Params[0].Size = 32;
      F.TypedParams[0].Type = NdType::makeInt(32, false);
      F.Blocks[0].Ops = {
          op(NdOp::SUBBYTES, reg(11, x86reg::RAX),
             {reg(10, x86reg::XMM0, 32), MedVar::makeConst(16, 8)}),
          op(NdOp::RETURN, {})};
      break;
    case 5:
    case 6:
      F = base();
      F.Blocks[0].Ops = {op(Case == 5 ? NdOp::POPCOUNT : NdOp::LZCOUNT,
                            reg(11, x86reg::RAX), {MedVar::makeConst(1, 16)}),
                         op(NdOp::RETURN, {})};
      break;
    case 7:
      F = base();
      F.Blocks[0].Ops = {
          op(NdOp::COPY, reg(11, x86reg::RAX), {MedVar::makeConst(1, 3)}),
          op(NdOp::RETURN, {})};
      break;
    }
    EXPECT_FALSE(analyzeMedMutableSource(F, Arch::X64));
    EXPECT_THROW(Emitter.emit({F}, Context, F.Name, Arch::X64),
                 std::runtime_error);
    EXPECT_THROW(MedToHighConverter().convert(F, Arch::X64),
                 std::runtime_error);
    const std::vector<char> Mask{0};
    EXPECT_THROW(Emitter.emit({F}, Context, F.Name, Arch::X64, {}, nullptr,
                              BinaryFormat::ELF, false, &Mask),
                 std::runtime_error);
  }
  auto F = loop();
  auto Module = Emitter.emit({F}, Context, F.Name, Arch::X64);
  ASSERT_NE(Module, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
}

TEST(MedMutableSource, LargeRepeatedWritesNeedNoPerOperationSSAProofGraph) {
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "mutable_probe";
  Low.Blocks.resize(1);
  auto &B = Low.Blocks.front();
  B.Id = 0;
  B.StartAddr = Low.Entry;
  B.EndAddr = Low.Entry + 16;
  for (size_t I = 0; I <= limits::kMaxSSAFunctionOps; ++I) {
    LowOp O;
    O.Opcode = NdOp::COPY;
    O.Output = NdVar::reg(x86reg::RAX, 8);
    O.addInput(NdVar::cst(37, 8));
    B.Ops.push_back(O);
  }
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  B.Ops.push_back(Ret);
  auto F = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_TRUE(F.SkippedSSA);
  F.ReturnType = NdType::makeInt(8, false);
  std::string Error;
  auto Plan = analyzeMedMutableSource(F, Arch::X64, &Error);
  ASSERT_TRUE(Plan) << Error;
  EXPECT_TRUE(Plan->EntryBytes.empty());
  EXPECT_LT(Plan->Variables.size(), 4u);
  compileAndRun(F, "\nint main(void) { return mutable_probe() != 37; }\n");
  auto &LastWrite = F.Blocks.front().Ops[F.Blocks.front().Ops.size() - 2];
  LastWrite.Inputs[0] = reg(900, x86reg::RBX);
  Plan = analyzeMedMutableSource(F, Arch::X64);
  ASSERT_TRUE(Plan);
  EXPECT_EQ(Plan->EntryBytes.at(x86reg::RBX), 255u);
  llvm::LLVMContext Context;
  EXPECT_THROW(MedLLVMEmitter().emit({F}, Context, F.Name, Arch::X64),
               std::runtime_error);
}

TEST(MedMutableSource, RepeatedBlockWritesKeepPrivateTrafficBounded) {
  for (bool Reverse : {false, true}) {
    auto F = loop(Reverse);
    parameter(F, x86reg::RSI);
    const auto Sum = reg(11, x86reg::RAX);
    const auto Address = reg(20, x86reg::RSI);
    const auto Scratch = temp(21);
    const auto Before = temp(22);
    const auto After = temp(23);
    auto &Body = F.Blocks[Reverse ? 1 : 2];
    std::vector<MedOp> Updates;
    Updates.push_back(op(NdOp::LOAD, Before, {Address}));
    Updates.push_back(op(NdOp::INT_ADD, Sum, {Sum, Before}));
    for (unsigned I = 0; I < 256; ++I) {
      Updates.push_back(op(NdOp::COPY, Scratch, {Sum}));
      Updates.push_back(
          op(NdOp::INT_ADD, Scratch, {Scratch, MedVar::makeConst(1, 8)}));
      Updates.push_back(op(NdOp::COPY, Sum, {Scratch}));
    }
    Updates.push_back(op(NdOp::STORE, {}, {Address, Sum}));
    Updates.push_back(op(NdOp::LOAD, After, {Address}));
    Updates.push_back(op(NdOp::INT_ADD, Sum, {Sum, After}));
    Body.Ops.insert(Body.Ops.begin() + 1, Updates.begin(), Updates.end());
    auto Plan = analyzeMedMutableSource(F, Arch::X64);
    ASSERT_TRUE(Plan);
    llvm::LLVMContext Context;
    auto Module = MedLLVMEmitter().emit({F}, Context, F.Name, Arch::X64);
    ASSERT_NE(Module, nullptr);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    unsigned PrivateLoads = 0, PrivateStores = 0;
    unsigned GuestLoads = 0, GuestStores = 0;
    for (const auto &Block : *Module->getFunction(F.Name))
      for (const auto &Instruction : Block) {
        if (const auto *Load = llvm::dyn_cast<llvm::LoadInst>(&Instruction))
          llvm::isa<llvm::AllocaInst>(Load->getPointerOperand())
              ? ++PrivateLoads
              : ++GuestLoads;
        if (const auto *Store = llvm::dyn_cast<llvm::StoreInst>(&Instruction))
          llvm::isa<llvm::AllocaInst>(Store->getPointerOperand())
              ? ++PrivateStores
              : ++GuestStores;
      }
    const auto BoundaryBudget = F.Blocks.size() * Plan->Variables.size();
    EXPECT_LE(PrivateLoads, BoundaryBudget);
    EXPECT_LE(PrivateStores, BoundaryBudget + F.Params.size());
    EXPECT_EQ(GuestLoads, 2u);
    EXPECT_EQ(GuestStores, 1u);
    const char *Main = R"(
int main(void) {
  for (uint64_t n = 0; n != 25; ++n) {
    for (uint64_t seed = 0; seed != 8; ++seed) {
      uint64_t cell = UINT64_C(0x9e3779b97f4a7c15) * seed;
      uint64_t expected_cell = cell, expected_sum = 0;
      for (uint64_t i = n; i; --i) {
        expected_cell = expected_sum + i + expected_cell + 256;
        expected_sum = expected_cell + expected_cell;
      }
      if (mutable_probe(n, (uint64_t)(uintptr_t)&cell) != expected_sum ||
          cell != expected_cell)
        return 1;
    }
  }
  return 0;
}
)";
    for (bool OptimizeLLVM : {false, true})
      compileAndRun(F, Main, 100000, OptimizeLLVM);
  }
}

TEST(MedMutableSource, LongMixedUpdatesKeepExpressionsBounded) {
  auto F = base();
  parameter(F, x86reg::RDI);
  parameter(F, x86reg::RSI);
  const auto R = reg(11, x86reg::RAX);
  const auto Key = reg(12, x86reg::RSI);
  F.Blocks[0].Ops.push_back(op(NdOp::COPY, R, {reg(10, x86reg::RDI)}));
  for (unsigned I = 0; I < 4096; ++I) {
    F.Blocks[0].Ops.push_back(
        op(NdOp::INT_MULT, R, {R, MedVar::makeConst(33, 8)}));
    F.Blocks[0].Ops.push_back(op(NdOp::INT_XOR, R, {R, Key}));
    F.Blocks[0].Ops.push_back(
        op(NdOp::INT_ADD, R, {R, MedVar::makeConst(I, 8)}));
  }
  F.Blocks[0].Ops.push_back(op(NdOp::RETURN, {}));
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  compileAndRun(F, R"(
int main(void) {
  for (uint64_t n = 0; n != 12; ++n) {
    uint64_t key = n * UINT64_C(0x9e3779b97f4a7c15), expected = n;
    for (uint64_t i = 0; i != 4096; ++i)
      expected = ((expected * 33) ^ key) + i;
    if (mutable_probe(n, key) != expected) return 1;
  }
  return 0;
}
)",
                1500000);
}

TEST(MedMutableSource, ForwardedGuestSnapshotsSurviveOverlappingWrites) {
  auto F = base();
  parameter(F, x86reg::RDI);
  parameter(F, x86reg::RSI);
  const auto Address = reg(10, x86reg::RDI);
  const auto Input = reg(12, x86reg::RSI);
  const auto Before = temp(20), After = temp(21), Interior = temp(22);
  const auto Narrow = temp(23, 2);
  F.Blocks[0].Ops = {
      op(NdOp::LOAD, Before, {Address}),
      op(NdOp::INT_ADD, Interior, {Address, MedVar::makeConst(2, 8)}),
      op(NdOp::SUBBYTES, Narrow, {Input, MedVar::makeConst(0, 8)}),
      op(NdOp::STORE, {}, {Interior, Narrow}),
      op(NdOp::LOAD, After, {Address}),
      op(NdOp::INT_XOR, reg(11, x86reg::RAX), {Before, After}),
      op(NdOp::RETURN, {})};
  ASSERT_TRUE(analyzeMedMutableSource(F, Arch::X64));
  for (bool OptimizeLLVM : {false, true})
    compileAndRun(F, R"(
int main(void) {
  for (uint64_t i = 0; i != 128; ++i) {
    uint64_t before = i * UINT64_C(0x9e3779b97f4a7c15), cell = before;
    uint64_t input = i * 257 + 65521;
    uint64_t after = (before & ~UINT64_C(0xffff0000)) |
                     ((input & 65535) << 16);
    if (mutable_probe((uint64_t)(uintptr_t)&cell, input) != (before ^ after) ||
        cell != after) return 1;
  }
  return 0;
}
)",
                  32768, OptimizeLLVM);
}

TEST(MedMutableSource, DeclaredZeroReturnSurvivesFoldingAndPromotion) {
  auto F = base();
  F.Blocks[0].Ops = {
      op(NdOp::COPY, reg(11, x86reg::RAX), {MedVar::makeConst(0, 8)}),
      op(NdOp::RETURN, {})};
  for (bool OptimizeLLVM : {false, true})
    compileAndRun(F, "\nint main(void) { return mutable_probe() != 0; }\n",
                  4096, OptimizeLLVM);
}

TEST(MedMutableSource, BlockValuesCannotEscapeFunctionOrModuleGeneration) {
  MedLLVMEmitter Emitter;
  for (unsigned Generation = 0; Generation != 3; ++Generation) {
    llvm::LLVMContext Context;
    auto F = loop(Generation % 2 != 0);
    auto G = F;
    G.Name = "another_mutable_probe";
    G.Entry += 0x1000;
    for (auto &B : G.Blocks) {
      B.StartAddr += 0x1000;
      B.EndAddr += 0x1000;
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::BRANCH || O.Opcode == NdOp::COND_BR)
          O.Inputs[0].ConstVal += 0x1000;
    }
    auto Module = Emitter.emit({F, G}, Context, "generations", Arch::X64);
    ASSERT_NE(Module, nullptr);
    EXPECT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    Module.reset();
    const std::vector<char> WrongMask{1, 1};
    EXPECT_EQ(Emitter.emit({F}, Context, F.Name, Arch::X64, {}, nullptr,
                           BinaryFormat::ELF, false, &WrongMask),
              nullptr);
    F.Blocks[0].Ops[0].Inputs.clear();
    EXPECT_THROW(Emitter.emit({F}, Context, F.Name, Arch::X64),
                 std::runtime_error);
  }
}
} // namespace
