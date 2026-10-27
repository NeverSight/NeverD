//===- X86NonlocalControlTests.cpp - Nonlocal transfer admission ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Privilege and SMM transitions cannot use an ordinary function's return
/// or syscall contract. Exercise decoding and direct IR/source admission.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/lift/X86/X86Lifter.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kEntry = 0x1000;

LowFunc lowFunction(Intrinsic Id, bool Result) {
  LowFunc Function;
  Function.Entry = kEntry;
  Function.Name = "nonlocal_transfer";
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.StartAddr = kEntry;
  Block.EndAddr = kEntry + 3;
  LowOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kEntry;
  Op.addInput(NdVar::cst(static_cast<uint64_t>(Id), 2));
  if (Result)
    Op.Output = NdVar::tmp(1, 8);
  Block.Ops.push_back(Op);
  LowInstructionBoundary Boundary;
  Boundary.Address = kEntry;
  Boundary.Size = 3;
  Boundary.OpCount = 1;
  Block.InstructionBoundaries.push_back(Boundary);
  return Function;
}

MedFunc medFunction(Intrinsic Id, bool Result) {
  MedFunc Function;
  Function.Entry = kEntry;
  Function.Name = "nonlocal_transfer";
  Function.ReturnType = NdType::makeVoid();
  Function.Blocks.emplace_back();
  auto &Block = Function.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = kEntry;
  MedOp Op;
  Op.Opcode = NdOp::INTRINSIC;
  Op.Addr = kEntry;
  Op.addInput(MedVar::makeConst(static_cast<uint64_t>(Id), 2));
  if (Result) {
    Op.Output.Kind = MedVar::Temp;
    Op.Output.Id = 1;
    Op.Output.Size = 8;
    Op.Output.SSAVer = 1;
  }
  Block.Ops.push_back(Op);
  MedOp Return;
  Return.Opcode = NdOp::RETURN;
  Block.Ops.push_back(Return);
  return Function;
}

} // namespace

TEST(X86NonlocalControl, RawTransitionsCannotBecomeReturnsOrSyscalls) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (bool Fast : {false, true}) {
        Decoder D;
        ASSERT_TRUE(D.init(Target));
        D.setStrict(Strict);
        const auto Decode = [&](const std::vector<uint8_t> &Bytes,
                                DecodedInsn &Instruction) {
          return Fast ? D.decodeOneForLift(Bytes.data(), Bytes.size(), kEntry,
                                           Instruction)
                      : D.decodeOne(Bytes.data(), Bytes.size(), kEntry,
                                    Instruction);
        };
        // SYSENTER and RSM take their target from an MSR or SMRAM, and the
        // 16-bit XBEGIN truncates its fallback: nothing names the
        // continuation, so nothing is published.
        for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
                 {0x0f, 0x34}, {0x0f, 0xaa}, {0x66, 0xc7, 0xf8, 0, 0}}) {
          SCOPED_TRACE(testing::Message() << unsigned(Target) << Strict << Fast
                                          << unsigned(Bytes[1]));
          DecodedInsn Instruction{};
          ASSERT_EQ(Decode(Bytes, Instruction), Bytes.size());
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(Instruction, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
        // Architectural returns end in a terminal state-transfer intrinsic
        // and XBEGIN branches to its fallback; none is an ordinary return or
        // syscall.
        std::vector<std::vector<uint8_t>> Lifted{
            {0x0f, 0x35},
            {0x0f, 0x07},
            {0xcf},
            {0xcb},
            {0xca, 8, 0},
            {0x66, 0xcf},
            {0xc7, 0xf8, 0, 0, 0, 0},
            {0xc7, 0xf8, 0xfa, 0xff, 0xff, 0xff}};
        if (Target == Arch::X64)
          for (const auto &Encoding :
               std::vector<std::vector<uint8_t>>{{0x48, 0x0f, 0x35},
                                                 {0x48, 0x0f, 0x07},
                                                 {0x48, 0xcf},
                                                 {0x48, 0xcb}})
            Lifted.push_back(Encoding);
        for (const auto &Bytes : Lifted) {
          SCOPED_TRACE(testing::Message() << unsigned(Target) << Strict << Fast
                                          << testing::PrintToString(Bytes));
          DecodedInsn Instruction{};
          ASSERT_EQ(Decode(Bytes, Instruction), Bytes.size());
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(Instruction, Ops));
          ASSERT_FALSE(Ops.empty());
          for (const LowOp &Op : Ops) {
            EXPECT_NE(Op.Opcode, NdOp::RETURN);
            if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs)
              EXPECT_NE(Op.Inputs[0].Offset,
                        static_cast<uint64_t>(Intrinsic::X64Syscall));
          }
          if (Bytes[0] != 0xc7)
            continue;
          // Only an abort takes the fallback, which the rel32 operand names.
          const auto Rel = static_cast<int32_t>(
              uint32_t(Bytes[2]) | uint32_t(Bytes[3]) << 8 |
              uint32_t(Bytes[4]) << 16 | uint32_t(Bytes[5]) << 24);
          const va_t Fallback = kEntry + Bytes.size() + int64_t(Rel);
          ASSERT_EQ(Ops.back().Opcode, NdOp::COND_BR);
          ASSERT_TRUE(Ops.back().Inputs[0].isConst());
          EXPECT_EQ(Ops.back().Inputs[0].Offset, Fallback);
          EXPECT_EQ(Ops.front().Opcode, NdOp::INTRINSIC);
          EXPECT_EQ(Ops.front().Inputs[0].Offset,
                    static_cast<uint64_t>(Intrinsic::Xbegin));
          EXPECT_EQ(Ops.front().Output.Size, 4);
        }
        const uint8_t Ret = 0xc3;
        DecodedInsn Instruction{};
        ASSERT_EQ(D.decodeOneForLift(&Ret, 1, kEntry, Instruction), 1u);
        std::vector<LowOp> Ops;
        EXPECT_NO_THROW(D.liftToLow(Instruction, Ops));
        ASSERT_FALSE(Ops.empty());
        EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
      }
}

TEST(X86NonlocalControl, LegacyIRRequiresTheMissingStateBoundary) {
  for (Intrinsic Id : {Intrinsic::Rsm})
    for (bool Result : {false, true}) {
      SCOPED_TRACE(testing::Message() << unsigned(Id) << Result);
      const auto Low = lowFunction(Id, Result);
      auto Error = validateLowInstructionBoundaries(Low.Blocks.front());
      EXPECT_TRUE(bool(Error));
      if (Error)
        EXPECT_NE(llvm::toString(std::move(Error)).find("machine-state"),
                  std::string::npos);
      for (Arch Target : {Arch::X86, Arch::X64, Arch::AArch64}) {
        EXPECT_THROW(
            LowToMedConverter().convert(Low, Target, BinaryFormat::COFF),
            LowToMedConversionError);
        EXPECT_FALSE(verifyMedFunc(medFunction(Id, Result), "nonlocal"));
      }
    }
}

TEST(X86NonlocalControl, LLVMAdmissionAlsoChecksMaskedFunctions) {
  for (Intrinsic Id : {Intrinsic::Rsm})
    for (bool Result : {false, true})
      for (bool Masked : {false, true}) {
        const auto Function = medFunction(Id, Result);
        llvm::LLVMContext Context;
        const std::vector<char> Mask{0};
        EXPECT_THROW(MedLLVMEmitter().emit({Function}, Context, Function.Name,
                                           Arch::X64, {}, nullptr,
                                           BinaryFormat::COFF, false,
                                           Masked ? &Mask : nullptr),
                     std::runtime_error);
      }
}

TEST(X86NonlocalControl, CRenderingCannotPublishBareAssembly) {
  for (Intrinsic Id : {Intrinsic::Rsm})
    for (bool Gnu : {false, true}) {
      bool HasIntrinsics = false;
      EXPECT_THROW(renderX86IntrinsicCall(Id, {}, HasIntrinsics, Gnu),
                   std::invalid_argument);
      EXPECT_THROW(renderX86MultiOutput(Id, {}, {}, {}, {}, {}, Gnu),
                   std::invalid_argument);
      for (Arch Target : {Arch::X86, Arch::X64, Arch::AArch64}) {
        EXPECT_THROW(renderIntrinsicCall(Id, Target, {}, 0, HasIntrinsics, Gnu),
                     std::invalid_argument);
        EXPECT_THROW(MultiOutputRender{}(Target, Id, {}, {}, {}, {}),
                     std::invalid_argument);
        for (bool Hidden : {false, true}) {
          HighFunc Function;
          Function.Entry = kEntry;
          Function.Name = "nonlocal_transfer";
          Function.ReturnType = NdType::makeVoid();
          HighStmt Invoke;
          Invoke.Kind = StmtKind::ExprStmt;
          Invoke.Val = std::make_shared<HighExpr>();
          Invoke.Val->Kind = ExprKind::Call;
          Invoke.Val->Type = NdType::makeVoid();
          Invoke.Val->IntrinsicId = Id;
          if (Hidden) {
            HighStmt Branch;
            Branch.Kind = StmtKind::If;
            Branch.Cond = HighExpr::makeConst(0, 1);
            Branch.Body.push_back(Invoke);
            Function.Body.push_back(Branch);
          } else
            Function.Body.push_back(Invoke);
          CEmitterOptions Options;
          Options.TheArch = Target;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          EXPECT_THROW(HighCEmitter().emit({Function}, Out, Options),
                       std::invalid_argument);
          EXPECT_TRUE(Source.empty());
        }
      }
    }
}
