//===- MachineInstructionTextTests.cpp - An intrinsic's instruction -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A trap, a breakpoint, a hypervisor or secure-monitor call and a barrier
/// keep their instruction's operand through the lift (the code of `brk
/// #0x3e8`, the option of `dmb ishld`), and both C routes print it: beside
/// a builtin that leaves it out (`__builtin_trap(); /* brk #0x3E8 */`), or
/// as the instruction an `asm` statement runs.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

namespace {

using namespace neverd;

/// The intrinsic \p Bytes lifts to and the operand it keeps, if any.
struct Lifted {
  Intrinsic Id = Intrinsic::None;
  std::optional<uint64_t> Operand;
};

Lifted liftOne(Arch TheArch, InstructionMode Mode, std::vector<uint8_t> Bytes) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(TheArch, Mode));
  DecodedInsn Instruction;
  EXPECT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
      static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  Decode.liftToLow(Instruction, Ops);
  Lifted Result;
  for (const LowOp &Op : Ops) {
    if (Op.Opcode != NdOp::INTRINSIC || Op.NumInputs == 0 ||
        !Op.Inputs[0].isConst())
      continue;
    Result.Id = static_cast<Intrinsic>(Op.Inputs[0].Offset);
    if (Op.NumInputs >= 2 && Op.Inputs[1].isConst())
      Result.Operand = Op.Inputs[1].Offset;
  }
  return Result;
}

void expectLifted(Arch TheArch, InstructionMode Mode,
                  std::vector<uint8_t> Bytes, Intrinsic Id,
                  std::optional<uint64_t> Operand, const char *Text) {
  SCOPED_TRACE(Text);
  const Lifted Result = liftOne(TheArch, Mode, std::move(Bytes));
  EXPECT_EQ(Result.Id, Id);
  EXPECT_EQ(Result.Operand, Operand);
  EXPECT_EQ(intrinsicInstructionText(Result.Id, Result.Operand), Text);
}

TEST(MachineInstructionText, AArch64KeepsTrapCodesAndBarrierOptions) {
  const auto A64 = [](std::vector<uint8_t> Bytes, Intrinsic Id,
                      uint64_t Operand, const char *Text) {
    expectLifted(Arch::AArch64, InstructionMode::Default, std::move(Bytes), Id,
                 Operand, Text);
  };
  // clang's __builtin_trap() and GCC's.
  A64({0x20, 0x00, 0x20, 0xd4}, Intrinsic::Brk, 0x1, "brk #0x1");
  A64({0x00, 0x7d, 0x20, 0xd4}, Intrinsic::Brk, 0x3E8, "brk #0x3E8");
  // An undefined instruction is not a breakpoint.
  A64({0x34, 0x12, 0x00, 0x00}, Intrinsic::Udf_A64, 0x1234, "udf #0x1234");
  A64({0x00, 0x00, 0x40, 0xd4}, Intrinsic::Hlt_A64, 0x0, "hlt #0x0");
  A64({0x82, 0x46, 0x02, 0xd4}, Intrinsic::Hvc, 0x1234, "hvc #0x1234");
  A64({0x23, 0x00, 0x00, 0xd4}, Intrinsic::Smc, 0x1, "smc #0x1");
  A64({0xbf, 0x39, 0x03, 0xd5}, Intrinsic::Dmb, 0x9, "dmb ishld");
  A64({0x9f, 0x3f, 0x03, 0xd5}, Intrinsic::Dsb, 0xF, "dsb sy");
  A64({0xdf, 0x3f, 0x03, 0xd5}, Intrinsic::Isb, 0xF, "isb");
}

TEST(MachineInstructionText, ARMKeepsTrapCodesAndBarrierOptions) {
  // Capstone spells a compiler's trap `trap`, without its operand.
  expectLifted(Arch::ARM, InstructionMode::Thumb, {0xfe, 0xde},
               Intrinsic::ArmUdf, 0xFE, "udf #0xFE");
  expectLifted(Arch::ARM, InstructionMode::ARM, {0xfe, 0xde, 0xff, 0xe7},
               Intrinsic::ArmUdf, 0xFDEE, "udf #0xFDEE");
  expectLifted(Arch::ARM, InstructionMode::Thumb, {0x01, 0xbe},
               Intrinsic::ArmBkpt, 0x1, "bkpt #0x1");
  expectLifted(Arch::ARM, InstructionMode::ARM, {0x5b, 0xf0, 0x7f, 0xf5},
               Intrinsic::ArmDmb, 0xB, "dmb ish");
  expectLifted(Arch::ARM, InstructionMode::ARM, {0x4f, 0xf0, 0x7f, 0xf5},
               Intrinsic::ArmDsb, 0xF, "dsb sy");
}

TEST(MachineInstructionText, X86KeepsTheSanitizerCheckOfUD1) {
  // clang's -fsanitize-trap raises `ud1 eax, [eax + kind]`.
  expectLifted(Arch::X86, InstructionMode::Default, {0x0f, 0xb9, 0x40, 0x15},
               Intrinsic::Ud1, 0x15, "ud1 eax, dword ptr [eax + 0x15]");
  expectLifted(Arch::X64, InstructionMode::Default,
               {0x67, 0x0f, 0xb9, 0x40, 0x15}, Intrinsic::Ud1, 0x15,
               "ud1 eax, dword ptr [eax + 0x15]");
  // Another form keeps no operand, and prints as before.
  expectLifted(Arch::X64, InstructionMode::Default, {0x0f, 0xb9, 0xc1},
               Intrinsic::Ud1, std::nullopt, "");
}

TEST(MachineInstructionText, BarrierOptionsReadBack) {
  for (uint64_t CRm = 0; CRm <= 0xF; ++CRm) {
    SCOPED_TRACE(CRm);
    EXPECT_EQ(armBarrierOptionValue(armBarrierOptionText(CRm)), CRm);
  }
  EXPECT_EQ(armBarrierOptionText(0x4), "#0x4");
  EXPECT_EQ(armBarrierOptionValue("ISH"), 0xBu);
  EXPECT_EQ(armBarrierOptionValue("osh2"), std::nullopt);
  EXPECT_EQ(intrinsicInstructionText(Intrinsic::Isb, 0x4), "isb #0x4");
  // A supervisor call prints its number already.
  EXPECT_EQ(intrinsicInstructionText(Intrinsic::Svc, 0x80), "");
}

/// HighC of one AArch64 or ARM function that calls \p Id with \p Operand.
std::string highC(Arch TheArch, Intrinsic Id, uint64_t Operand,
                  uint16_t Bytes) {
  HighFunc Func;
  Func.Name = "f";
  Func.ReturnType = NdType::makeVoid();
  auto Call =
      HighExpr::makeCall("", 0x1000, {HighExpr::makeConst(Operand, Bytes)});
  Call->IntrinsicId = Id;
  Call->Type = NdType::makeVoid();
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.Addr = 0x1000;
  Statement.CallExpr = std::move(Call);
  Func.Body.push_back(std::move(Statement));
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  return C;
}

TEST(MachineInstructionText, HighCPrintsTheInstruction) {
  // A trap's builtin takes no code; the instruction goes beside it.
  std::string C = highC(Arch::AArch64, Intrinsic::Brk, 0x3E8, 2);
  EXPECT_NE(C.find("__builtin_trap(); /* brk #0x3E8 */"), std::string::npos)
      << C;
  C = highC(Arch::ARM, Intrinsic::ArmBkpt, 0x1, 2);
  EXPECT_NE(C.find("__builtin_debugtrap(); /* bkpt #0x1 */"), std::string::npos)
      << C;
  // A barrier's ACLE intrinsic takes the option.
  C = highC(Arch::AArch64, Intrinsic::Dmb, 0x9, 1);
  EXPECT_NE(C.find("__dmb("), std::string::npos) << C;
  EXPECT_NE(C.find("); /* dmb ishld */"), std::string::npos) << C;
  // One without a C name runs as its instruction.
  C = highC(Arch::AArch64, Intrinsic::Hvc, 0x1234, 2);
  EXPECT_NE(C.find("__asm__ volatile(\"hvc #0x1234\" ::: \"memory\")"),
            std::string::npos)
      << C;
  C = highC(Arch::ARM, Intrinsic::ArmUdf, 0xFE, 2);
  EXPECT_NE(C.find("__asm__ volatile(\"udf #0xFE\" ::: \"memory\")"),
            std::string::npos)
      << C;
}

std::string llvmC(const char *IR, Arch TheArch) {
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(IR, Diagnostic, Context);
  EXPECT_TRUE(Module) << Diagnostic.getMessage().str();
  if (!Module)
    return {};
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = TheArch;
  EXPECT_TRUE(LLVMCEmitter().emit(*Module, OS, Options));
  return C;
}

TEST(MachineInstructionText, LLVMCPrintsTheInstruction) {
  const std::string C = llvmC(R"(
declare void @llvm.trap()
define void @trapped() {
entry:
  call void @llvm.trap(), !neverd.instruction !0
  unreachable
}
define void @ordered() {
entry:
  fence seq_cst, !neverd.instruction !1
  call void asm sideeffect "dsb ishst", "~{memory}"()
  call void asm sideeffect "udf #0xFE", "~{memory}"()
  ret void
}
!0 = !{!"brk #0x3E8"}
!1 = !{!"dmb ishld"}
)",
                              Arch::AArch64);
  EXPECT_NE(C.find("__builtin_trap(); /* brk #0x3E8 */"), std::string::npos)
      << C;
  // The fence orders at least what the barrier did; the barrier is beside.
  EXPECT_NE(C.find("; /* dmb ishld */"), std::string::npos) << C;
  // A barrier's own option, not its mnemonic's default.
  EXPECT_NE(C.find("__dsb(0xA);"), std::string::npos) << C;
  EXPECT_NE(C.find("__builtin_trap(); /* udf #0xFE */"), std::string::npos)
      << C;
}

} // namespace
