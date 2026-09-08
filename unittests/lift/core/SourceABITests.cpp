#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/med/MedTypePass.h"
#include "neverd/lift/AArch64Regs.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>

namespace {
using namespace neverd;

SourceFunctionTypeHint declaration(TypeRef Result) {
  SourceFunctionTypeHint Hint;
  Hint.ReturnType = std::move(Result);
  Hint.Parameters = {{"objc_self", NdType::makePtr()},
                     {"objc_cmd", NdType::makePtr()}};
  return Hint;
}

TEST(SourceABI, DarwinMixedArgumentsUseIndependentBanks) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"arg0", NdType::makeInt(4)},
                            {"arg1", NdType::makeFloat(8)},
                            {"arg2", NdType::makeInt(8)},
                            {"arg3", NdType::makeFloat(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    const auto &TRI = getTargetRegInfo(Architecture);
    EXPECT_EQ(Hint.Parameters[2].Location.RegisterOffset, TRI.IntParamRegs[2]);
    EXPECT_EQ(Hint.Parameters[3].Location.RegisterOffset, TRI.FPParamRegs[0]);
    EXPECT_EQ(Hint.Parameters[4].Location.RegisterOffset, TRI.IntParamRegs[3]);
    EXPECT_EQ(Hint.Parameters[5].Location.RegisterOffset, TRI.FPParamRegs[1]);
    EXPECT_EQ(Hint.Parameters[5].Location.ValueBytes, 4);
    EXPECT_EQ(Hint.ReturnLocation.Kind, SourceABICarrierKind::FloatingRegister);
    EXPECT_EQ(Hint.ReturnLocation.RegisterOffset, TRI.FPReturnReg);
  }
}

TEST(SourceABI, DarwinStackLayoutPreservesNarrowArgumentOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeInt(4));
    const auto Registers = getTargetRegInfo(Architecture).IntParamRegs.size();
    for (size_t I = 2; I < Registers; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I - 2), NdType::makeInt(4)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto &L = Hint.Parameters[Registers + I].Location;
      EXPECT_EQ(L.Kind, SourceABICarrierKind::Stack);
      EXPECT_EQ(L.EntryStackOffset, Architecture == Arch::AArch64
                                        ? int64_t(I * 2)
                                        : int64_t(8 + I * 8));
    }
  }
}

TEST(SourceABI, FloatingBankOverflowDoesNotConsumeIntegerRegisters) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    auto Hint = declaration(NdType::makeFloat(8));
    for (unsigned I = 0; I != 9; ++I)
      Hint.Parameters.push_back(
          {"arg" + std::to_string(I), NdType::makeFloat(8)});
    Hint.Parameters.push_back({"integer", NdType::makeInt(8)});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    EXPECT_EQ(Hint.Parameters[10].Location.Kind, SourceABICarrierKind::Stack);
    EXPECT_EQ(Hint.Parameters[10].Location.EntryStackOffset,
              Architecture == Arch::X64 ? 8 : 0);
    EXPECT_EQ(Hint.Parameters[11].Location.RegisterOffset,
              getTargetRegInfo(Architecture).IntParamRegs[2]);
  }
}

TEST(SourceABI, RejectsConflictingCarriersAndUnmodelledTypes) {
  auto Hint = declaration(NdType::makeInt(8));
  std::string Error;
  ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Arch::AArch64, Error));
  auto Bad = Hint;
  Bad.Parameters[1].Location = Bad.Parameters[0].Location;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("share"), std::string::npos);
  Bad = Hint;
  Bad.ReturnLocation.Kind = SourceABICarrierKind::FloatingRegister;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Location.ValueBytes = 4;
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  Bad = Hint;
  Bad.Parameters[0].Type = NdType::makeFloat(16);
  EXPECT_FALSE(assignDarwinObjCSourceABI(Bad, Arch::AArch64, Error));
  Bad = Hint;
  Bad.Parameters[0].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  Bad.Parameters[1].Location = {SourceABICarrierKind::Stack, 0, 0, 8};
  EXPECT_FALSE(validateSourceABI(Bad, Error));
  EXPECT_NE(Error.find("Overlapping"), std::string::npos);
}

TEST(SourceABI, ExplicitSwiftReceiverCanUseDedicatedCalleeSavedRegister) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SourceFunctionTypeHint Hint;
    Hint.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
    Hint.Architecture = Architecture;
    Hint.HasExplicitABI = true;
    Hint.ReturnType = NdType::makeInt(8);
    const auto &TRI = getTargetRegInfo(Architecture);
    Hint.ReturnLocation = {SourceABICarrierKind::IntegerRegister,
                           TRI.IntReturnReg, 0, 8};
    // Swift class receivers use these dedicated registers; the validator must
    // not impose Objective-C's two hidden leading arguments.
    const auto Dedicated =
        Architecture == Arch::AArch64 ? a64reg::X20 : x86reg::R13;
    Hint.Parameters = {
        {"self",
         NdType::makePtr(),
         {SourceABICarrierKind::IntegerRegister, Dedicated, 0, 8}}};
    std::string Error;
    EXPECT_TRUE(validateSourceABI(Hint, Error)) << Error;
  }
}

TEST(SourceABI, PackedGenericSlotReadsBindToDistinctSourceParameters) {
  auto Hint = declaration(NdType::makeInt(4));
  for (unsigned I = 0; I != 6; ++I)
    Hint.Parameters.push_back({"arg" + std::to_string(I), NdType::makeInt(4)});
  Hint.Parameters.insert(Hint.Parameters.end(), {{"arg6", NdType::makeInt(1)},
                                                 {"arg7", NdType::makeInt(2)},
                                                 {"arg8", NdType::makeInt(4)}});
  MedFunc Func;
  Func.Name = "packed";
  Func.SourceTypeHint = Hint;
  MedBlock Block;
  Block.Id = 0;
  for (unsigned I = 0; I != 3; ++I) {
    MedOp Read;
    Read.Opcode = I ? NdOp::SUBBYTES : NdOp::COPY;
    Read.Output.Kind = MedVar::Temp;
    Read.Output.Id = 20 + I;
    Read.Output.Size = uint16_t(1U << I);
    MedVar Slot;
    Slot.Kind = MedVar::Param;
    Slot.Id = 8;
    Slot.RegOff = kNoParamReg;
    Slot.Size = I ? 8 : 1;
    Read.addInput(Slot);
    if (I)
      Read.addInput(MedVar::makeConst(I * 2, 4));
    Block.Ops.push_back(Read);
  }
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  ASSERT_TRUE(Func.SourceParametersBound);
  ASSERT_EQ(Func.Params.size(), 11U);
  for (unsigned I = 0; I != 3; ++I) {
    const auto &Read = Func.Blocks[0].Ops[I];
    EXPECT_EQ(Read.Inputs[0].Id, 8 + int(I));
    EXPECT_EQ(Read.Inputs[0].Size, 1U << I);
    if (I)
      EXPECT_EQ(Read.Inputs[1].ConstVal, 0U);
  }
  inferMedTypes(Func, Arch::AArch64);
  ASSERT_TRUE(Func.SourceTypeHint);
  EXPECT_EQ(Func.Blocks[0].Ops[2].Inputs[0].Id, 10);
  // An eight-byte read of this packed slot includes independent arguments and
  // padding; reject the projection instead of assigning it to the first one.
  Func.SourceParametersBound = false;
  Func.Blocks[0].Ops.resize(1);
  Func.Blocks[0].Ops[0].Inputs[0].Size = 8;
  Func.Blocks[0].Ops[0].Output.Size = 8;
  inferMedTypes(Func, Arch::AArch64);
  EXPECT_FALSE(Func.SourceTypeHint);
}

TEST(SourceABI, SourceStackExpressionsPreserveExactEntryOffsets) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    const auto &TRI = getTargetRegInfo(Architecture);
    auto Hint = declaration(NdType::makeInt(4));
    for (size_t I = 2; I < TRI.IntParamRegs.size(); ++I)
      Hint.Parameters.push_back(
          {"unused" + std::to_string(I), NdType::makeInt(8)});
    Hint.Parameters.insert(Hint.Parameters.end(),
                           {{"byte", NdType::makeInt(1)},
                            {"short_value", NdType::makeInt(2)},
                            {"integer", NdType::makeInt(4)}});
    std::string Error;
    ASSERT_TRUE(assignDarwinObjCSourceABI(Hint, Architecture, Error)) << Error;
    for (size_t I = 0; I != 3; ++I) {
      const auto Index = TRI.IntParamRegs.size() + I;
      const auto &Location = Hint.Parameters[Index].Location;
      const int64_t BaseOffset = Architecture == Arch::X64 ? 8 : 0;
      const auto SlotOffset = Location.EntryStackOffset - BaseOffset;
      MedFunc Func;
      Func.Name = "stack_parameter";
      Func.SourceTypeHint = Hint;
      Func.SourceTypeHint->ReturnType = Hint.Parameters[Index].Type;
      Func.SourceTypeHint->ReturnLocation.ValueBytes = Location.ValueBytes;
      MedVar Slot;
      Slot.Kind = MedVar::Param;
      Slot.TheArch = Architecture;
      Slot.Id = static_cast<int>(TRI.IntParamRegs.size() + SlotOffset / 8);
      Slot.RegOff = kNoParamReg;
      Slot.Size = SlotOffset % 8 ? 8 : Location.ValueBytes;
      MedOp Read;
      Read.Opcode = SlotOffset % 8 ? NdOp::SUBBYTES : NdOp::COPY;
      Read.Output.Kind = MedVar::Reg;
      Read.Output.TheArch = Architecture;
      Read.Output.Id = 20;
      Read.Output.RegOff = TRI.IntReturnReg;
      Read.Output.Size = Location.ValueBytes;
      Read.addInput(Slot);
      if (SlotOffset % 8)
        Read.addInput(MedVar::makeConst(SlotOffset % 8, 4));
      MedOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(Read.Output);
      MedBlock Block;
      Block.Id = 0;
      Block.Ops = {Read, Return};
      Func.Blocks.push_back(Block);
      inferMedTypes(Func, Architecture);
      ASSERT_TRUE(Func.SourceTypeHint);
      ASSERT_EQ(Func.Params[Index].RegOff, kNoParamReg);
      auto High = MedToHighConverter().convert(Func, Architecture);
      ASSERT_TRUE(High.SourceTypeHint);
      unsigned References = 0;
      auto Check = [&](auto &&Self, const ExprPtr &Expression) -> void {
        if (!Expression)
          return;
        if (Expression->Kind == ExprKind::Var &&
            Expression->Var.Kind == MedVar::Param) {
          ++References;
          EXPECT_EQ(Expression->Var.Id, int(Index));
          EXPECT_EQ(Expression->Var.StackOff, Location.EntryStackOffset);
          EXPECT_NE(Expression->Var.StackOff, -1);
          EXPECT_EQ(Expression->Var.Size, Location.ValueBytes);
        }
        for (const auto &Operand : Expression->Operands)
          Self(Self, Operand);
      };
      walkStmts(High.Body, [&](const HighStmt &Statement) {
        forEachRhsExpr(Statement, [&](const ExprPtr &Expression) {
          Check(Check, Expression);
        });
      });
      EXPECT_GT(References, 0U);
      EXPECT_EQ(Func.Params[Index].RegOff, kNoParamReg);
    }
  }
}

TEST(SourceABI, BitCastRejectsMismatchedWidthsAndDistinguishesTargetTypes) {
  auto Bits = HighExpr::makeConst(0x80000000, 4);
  auto Float = HighExpr::makeBitCast(Bits, NdType::makeFloat(4));
  auto Integer = HighExpr::makeBitCast(Bits, NdType::makeInt(4, true));
  EXPECT_EQ(Float->Kind, ExprKind::BitCast);
  EXPECT_FALSE(Float->structuralEq(*Integer));
  EXPECT_EQ(HighExpr::makeBitCast(Bits, NdType::makeFloat(8))->Kind,
            ExprKind::Undef);
  auto RoundTrip = HighExpr::makeBitCast(Float, Bits->Type);
  EXPECT_TRUE(RoundTrip->structuralEq(*Bits));
}

HighFunc scalarFloatFunction(Arch Architecture, uint16_t Width, bool Add) {
  MedFunc Func;
  Func.Name = std::string(Architecture == Arch::X64 ? "x64_" : "a64_") +
              (Width == 4 ? "f32_" : "f64_") + (Add ? "add" : "identity");
  auto Hint = declaration(NdType::makeFloat(Width));
  Hint.Parameters.push_back({"arg0", NdType::makeFloat(Width)});
  if (Add)
    Hint.Parameters.push_back({"arg1", NdType::makeFloat(Width)});
  Func.SourceTypeHint = Hint;
  const auto &TRI = getTargetRegInfo(Architecture);
  MedBlock Block;
  Block.Id = 0;
  std::vector<MedVar> Incoming;
  for (unsigned I = 0; I != (Add ? 2U : 1U); ++I) {
    MedVar Parameter;
    Parameter.Kind = MedVar::Reg;
    Parameter.TheArch = Architecture;
    Parameter.Id = 20 + I;
    Parameter.RegOff = TRI.FPParamRegs[I];
    Parameter.Size = Architecture == Arch::X64 ? 16 : Width;
    MedOp Marker;
    Marker.Opcode = NdOp::COPY;
    Marker.Output = Parameter;
    Marker.addInput(Parameter);
    Block.Ops.push_back(Marker);
    Incoming.push_back(Parameter);
  }
  if (Add) {
    for (unsigned I = 0; I != 2; ++I) {
      MedOp Slice;
      Slice.Opcode = NdOp::SUBBYTES;
      Slice.Output.Kind = MedVar::Temp;
      Slice.Output.Id = 30 + I;
      Slice.Output.Size = Width;
      Slice.addInput(Incoming[I]);
      Slice.addInput(MedVar::makeConst(0, 4));
      Block.Ops.push_back(Slice);
    }
    MedOp Sum;
    Sum.Opcode = NdOp::FLOAT_ADD;
    Sum.Output.Kind = MedVar::Temp;
    Sum.Output.Id = 32;
    Sum.Output.Size = Width;
    Sum.addInput(Block.Ops[2].Output);
    Sum.addInput(Block.Ops[3].Output);
    Block.Ops.push_back(Sum);
    MedOp Widen;
    Widen.Opcode = NdOp::INT_ZEXT;
    Widen.Output.Kind = MedVar::Reg;
    Widen.Output.Id = 33;
    Widen.Output.Size = 16;
    Widen.Output.RegOff = TRI.FPReturnReg;
    Widen.addInput(Sum.Output);
    Block.Ops.push_back(Widen);
  }
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  // On x64 a generic return exposes RAX even when this declaration's result
  // is in XMM0. That unrelated integer carrier must not supply a float result.
  if (Architecture == Arch::X64) {
    MedVar RAX;
    RAX.Kind = MedVar::Reg;
    RAX.Id = 50;
    RAX.Size = 8;
    RAX.RegOff = TRI.IntReturnReg;
    Return.addInput(RAX);
  }
  Block.Ops.push_back(Return);
  Func.Blocks.push_back(Block);
  inferMedTypes(Func, Architecture);
  EXPECT_TRUE(Func.SourceTypeHint);
  auto High = MedToHighConverter().convert(Func, Architecture);
  EXPECT_TRUE(High.SourceTypeHint);
  EXPECT_EQ(High.ReturnType->Kind, NdTypeKind::Float);
  return High;
}

void executeC(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  auto FoundCompiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(static_cast<bool>(FoundCompiler)) << "clang is required";
  const std::string Compiler = *FoundCompiler;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-source-abi", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "exe",
                                                  BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(llvm::sys::fs::createTemporaryFile("neverd-source-abi", "err",
                                                  ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream OS(SourcePath, EC);
    ASSERT_FALSE(EC) << EC.message();
    OS << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (llvm::StringRef Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 16> Arguments{
        Compiler,   "-std=c11", Optimization, "-Werror=return-type",
        SourcePath, "-o",       BinaryPath};
    std::string Error;
    int Status = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 30, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    llvm::SmallVector<llvm::StringRef, 1> RunArguments{BinaryPath};
    Status = llvm::sys::ExecuteAndWait(BinaryPath, RunArguments, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    EXPECT_EQ(Status, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

TEST(SourceABI,
     FloatProjectionExecutesNumericOperationsAndPreservesIdentityBits) {
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  std::string Checks;
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    std::vector<HighFunc> Functions;
    for (uint16_t Width : {4, 8})
      for (bool Add : {false, true})
        Functions.push_back(scalarFloatFunction(Architecture, Width, Add));
    CEmitterOptions Options;
    Options.TheArch = Architecture;
    ASSERT_TRUE(HighCEmitter().emit(Functions, OS, Options));
    const std::string Prefix = Architecture == Arch::X64 ? "x64_" : "a64_";
    Checks +=
        "if (" + Prefix + "f32_add(0, 0, -7.5f, 2.25f) != -5.25f) return 1;\n";
    Checks += "if (" + Prefix +
              "f64_add(0, 0, -1024.5, 3.125) != -1021.375) return 2;\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(fbits)/sizeof(fbits[0]); ++i) {\n"
        "float v; memcpy(&v, &fbits[i], 4); float r = " +
        Prefix +
        "f32_identity(0, 0, v); uint32_t bits; memcpy(&bits, &r, 4);"
        "if (bits != fbits[i]) return 3; }\n";
    Checks +=
        "for (unsigned i = 0; i < sizeof(dbits)/sizeof(dbits[0]); ++i) {\n"
        "double v; memcpy(&v, &dbits[i], 8); double r = " +
        Prefix +
        "f64_identity(0, 0, v); uint64_t bits; memcpy(&bits, &r, 8);"
        "if (bits != dbits[i]) return 4; }\n";
  }
  OS.flush();
  Source += R"(
#include <string.h>
int main(void) {
  const uint32_t fbits[] = {0, 0x80000000U, 0x7f800000U, 0xff800000U,
                           0x7fc12345U, 1, 0xc0f00000U};
  const uint64_t dbits[] = {0, UINT64_C(0x8000000000000000),
                           UINT64_C(0x7ff0000000000000),
                           UINT64_C(0xfff0000000000000),
                           UINT64_C(0x7ff8000000001234), 1,
                           UINT64_C(0xc020800000000000)};
)" + Checks +
            "return 0;\n}\n";
  executeC(Source);
}

} // namespace
