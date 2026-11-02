//===- MachineInstructionTextTests.cpp - An intrinsic's instruction -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A trap, a breakpoint, a hypervisor or secure-monitor call, a barrier, an
/// AArch64 MRS, MSR or SYS and an ARM coprocessor transfer keep their
/// instruction's operand through the lift (the code of `brk #0x3e8`, the
/// option of `dmb ishld`, the register of `msr tpidr_el0, x1`, the numbers
/// of `mrc p15, #0, r0, c13, c0, #3`), and both C routes print it: beside a
/// builtin that leaves it out (`__builtin_trap(); /* brk #0x3E8 */`), as the
/// ACLE intrinsic that names it (`__arm_rsr64("cntvct_el0")`,
/// `__arm_mrc(15, 0, 13, 0, 3)`), or as the instruction an `asm` statement
/// runs.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/c/render/ARM/ArmCoprocessorAcle.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/ir/intrinsics/Intrinsics.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <capstone/capstone.h>
#include <cstdint>
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
  A64({0x00, 0x00, 0x3e, 0xd4}, Intrinsic::Brk, 0xF000, "brk #0xF000");
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
               Intrinsic::ArmUdf, 0xFDEE, "udf.w #0xFDEE");
  expectLifted(Arch::ARM, InstructionMode::Thumb, {0xf1, 0xf7, 0x34, 0xa2},
               Intrinsic::ArmUdf, 0x1234, "udf.w #0x1234");
  expectLifted(Arch::ARM, InstructionMode::Thumb, {0x01, 0xbe},
               Intrinsic::ArmBkpt, 0x1, "bkpt #0x1");
  expectLifted(Arch::ARM, InstructionMode::ARM, {0x5b, 0xf0, 0x7f, 0xf5},
               Intrinsic::ArmDmb, 0xB, "dmb ish");
  expectLifted(Arch::ARM, InstructionMode::ARM, {0x4f, 0xf0, 0x7f, 0xf5},
               Intrinsic::ArmDsb, 0xF, "dsb sy");
}

TEST(MachineInstructionText, ARMKeepsCoprocessorTransfers) {
  // The lift keeps the instruction's numbers as one field, in ARM and Thumb;
  // the LLVM route runs its assembly, which reads back to the same field,
  // and both C routes call ACLE with the numbers.
  const struct {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    Intrinsic Id;
    uint64_t Field;
    const char *Asm;
    const char *Acle;
  } Cases[] = {
      {InstructionMode::ARM,
       {0x70, 0x0f, 0x1d, 0xee},
       Intrinsic::ArmMrc,
       0xF0D03,
       "mrc p15, #0, $0, c13, c0, #3",
       "__arm_mrc(15, 0, 13, 0, 3)"},
      {InstructionMode::Thumb,
       {0x1d, 0xee, 0x70, 0x0f},
       Intrinsic::ArmMrc,
       0xF0D03,
       "mrc p15, #0, $0, c13, c0, #3",
       "__arm_mrc(15, 0, 13, 0, 3)"},
      {InstructionMode::ARM,
       {0x50, 0x0f, 0x0d, 0xee},
       Intrinsic::ArmMcr,
       0xF0D02,
       "mcr p15, #0, $0, c13, c0, #2",
       "__arm_mcr(15, 0, value, 13, 0, 2)"},
      {InstructionMode::ARM,
       {0x15, 0x1e, 0x10, 0xee},
       Intrinsic::ArmMrc,
       0xE0050,
       "mrc p14, #0, $0, c0, c5, #0",
       "__arm_mrc(14, 0, 0, 5, 0)"},
      {InstructionMode::ARM,
       {0x1e, 0x0f, 0x51, 0xec},
       Intrinsic::ArmMrrc,
       0xF10E0,
       "mrrc p15, #1, ${0:Q}, ${0:R}, c14",
       "__arm_mrrc(15, 1, 14)"},
      {InstructionMode::ARM,
       {0x3e, 0x0f, 0x41, 0xec},
       Intrinsic::ArmMcrr,
       0xF30E0,
       "mcrr p15, #3, ${0:Q}, ${0:R}, c14",
       "__arm_mcrr(15, 3, value, 14)"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Asm);
    const Lifted Result = liftOne(Arch::ARM, Case.Mode, Case.Bytes);
    EXPECT_EQ(Result.Id, Case.Id);
    EXPECT_EQ(Result.Operand, Case.Field);
    EXPECT_EQ(armCoprocessorInstructionAsm(Case.Id, Case.Field), Case.Asm);
    EXPECT_EQ(armCoprocessorAsmField(Case.Asm), std::pair(Case.Id, Case.Field));
    EXPECT_EQ(armCoprocessorAcleCall(Case.Id, Case.Field, "value"), Case.Acle);
  }
  // A field no transfer has, or other assembly, reads as none.
  EXPECT_EQ(armCoprocessorInstructionAsm(Intrinsic::ArmMrc, 0xF8D03), "");
  EXPECT_EQ(armCoprocessorInstructionAsm(Intrinsic::ArmMrrc, 0xF1DE0), "");
  EXPECT_EQ(armCoprocessorAsmField("mrc p15, #8, $0, c13, c0, #3"),
            std::nullopt);
  EXPECT_EQ(armCoprocessorAsmField("mrc p15, #0, $0, c13, c0"), std::nullopt);
  EXPECT_EQ(armCoprocessorAsmField("mrs $0, apsr"), std::nullopt);

  // The MRC that writes the flags, and an MRRC that writes one register
  // twice, have no typed lift.
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::ARM, InstructionMode::ARM));
  for (const std::vector<uint8_t> &Bytes :
       {std::vector<uint8_t>{0x11, 0xfe, 0x10, 0xee},
        std::vector<uint8_t>{0x1e, 0x0f, 0x50, 0xec}}) {
    DecodedInsn Instruction;
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                      Instruction),
              4);
    std::vector<LowOp> Ops;
    EXPECT_THROW(Decode.liftToLow(Instruction, Ops), UnliftedInstruction);
  }
}

TEST(MachineInstructionText, ARMKeepsStatusRegisterFields) {
  // An MRS keeps the register it reads, an MSR the fields the machine keeps
  // (the flags and GE are lifted), a CPS its masks and mode.
  const struct {
    InstructionMode Mode;
    std::vector<uint8_t> Bytes;
    Intrinsic Id;
    uint64_t Field;
    const char *Asm;
  } Cases[] = {
      {InstructionMode::ARM,
       {0x00, 0x00, 0x0f, 0xe1},
       Intrinsic::ArmMrs,
       ArmMrsApsr,
       "mrs $0, apsr"},
      {InstructionMode::Thumb,
       {0xef, 0xf3, 0x00, 0x80},
       Intrinsic::ArmMrs,
       ArmMrsApsr,
       "mrs $0, apsr"},
      {InstructionMode::ARM,
       {0x00, 0x00, 0x4f, 0xe1},
       Intrinsic::ArmMrs,
       ArmMrsSpsr,
       "mrs $0, spsr"},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x21, 0xe1},
       Intrinsic::ArmMsr,
       0x1,
       "msr cpsr_c, $0"},
      {InstructionMode::ARM,
       {0xd3, 0xf0, 0x21, 0xe3},
       Intrinsic::ArmMsr,
       0x1,
       "msr cpsr_c, $0"},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x29, 0xe1},
       Intrinsic::ArmMsr,
       0x9,
       "msr cpsr_fc, $0"},
      // The f field also writes Q, which the machine keeps.
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x28, 0xe1},
       Intrinsic::ArmMsr,
       0x8,
       "msr cpsr_f, $0"},
      {InstructionMode::ARM,
       {0x00, 0xf0, 0x6f, 0xe1},
       Intrinsic::ArmMsr,
       0x1F,
       "msr spsr_fsxc, $0"},
      {InstructionMode::ARM,
       {0x80, 0x00, 0x0c, 0xf1},
       Intrinsic::ArmCps,
       0x680,
       "cpsid i"},
      {InstructionMode::Thumb,
       {0x72, 0xb6},
       Intrinsic::ArmCps,
       0x680,
       "cpsid i"},
      {InstructionMode::ARM,
       {0xc0, 0x00, 0x08, 0xf1},
       Intrinsic::ArmCps,
       0x4C0,
       "cpsie if"},
      {InstructionMode::ARM,
       {0x13, 0x00, 0x02, 0xf1},
       Intrinsic::ArmCps,
       0x33,
       "cps #0x13"},
      {InstructionMode::ARM,
       {0xd3, 0x00, 0x0e, 0xf1},
       Intrinsic::ArmCps,
       0x6F3,
       "cpsid if, #0x13"},
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Asm);
    const Lifted Result = liftOne(Arch::ARM, Case.Mode, Case.Bytes);
    EXPECT_EQ(Result.Id, Case.Id);
    EXPECT_EQ(Result.Operand, Case.Field);
    EXPECT_EQ(armStatusInstructionAsm(Case.Id, Case.Field), Case.Asm);
  }
  // GE is lifted whole: `msr apsr_g, r0` leaves the machine alone.
  EXPECT_EQ(
      liftOne(Arch::ARM, InstructionMode::ARM, {0x00, 0xf0, 0x24, 0xe1}).Id,
      Intrinsic::None);
  // Fields no such instruction has.
  EXPECT_EQ(armStatusInstructionAsm(Intrinsic::ArmMrs, 2), "");
  EXPECT_EQ(armStatusInstructionAsm(Intrinsic::ArmMsr, 0), "");
  EXPECT_EQ(armStatusInstructionAsm(Intrinsic::ArmMsr, 0x21), "");
  EXPECT_EQ(
      armStatusInstructionAsm(Intrinsic::ArmCps, armCpsField(3, 0, true, 0x13)),
      "");
  EXPECT_EQ(
      armStatusInstructionAsm(Intrinsic::ArmCps, armCpsField(0, 2, false, 0)),
      "");

  // CPSID without a mask to change and a banked register's MSR have no
  // typed lift.
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::ARM, InstructionMode::ARM));
  for (const std::vector<uint8_t> &Bytes :
       {std::vector<uint8_t>{0x13, 0x00, 0x0e, 0xf1},
        std::vector<uint8_t>{0x00, 0xf2, 0x29, 0xe1}}) {
    DecodedInsn Instruction;
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                      Instruction),
              4);
    std::vector<LowOp> Ops;
    EXPECT_THROW(Decode.liftToLow(Instruction, Ops), UnliftedInstruction);
  }
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

/// The intrinsic the AArch64 instruction \p Word lifts to.
LowOp liftA64(uint32_t Word) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Arch::AArch64, InstructionMode::Default));
  const uint8_t Bytes[] = {
      static_cast<uint8_t>(Word), static_cast<uint8_t>(Word >> 8),
      static_cast<uint8_t>(Word >> 16), static_cast<uint8_t>(Word >> 24)};
  DecodedInsn Instruction;
  EXPECT_EQ(Decode.decodeOneForLift(Bytes, 4, 0x1000, Instruction), 4);
  std::vector<LowOp> Ops;
  Decode.liftToLow(Instruction, Ops);
  for (const LowOp &Op : Ops)
    if (Op.Opcode == NdOp::INTRINSIC)
      return Op;
  return {};
}

TEST(MachineInstructionText, AArch64KeepsSystemRegistersAndOperations) {
  struct Case {
    uint32_t Word;
    Intrinsic Id;
    bool Output;
    bool Register;
    const char *Asm;
  };
  const Case Cases[] = {
      {0xd53be040, Intrinsic::Mrs, true, false, "mrs $0, cntvct_el0"},
      {0xd53bf200, Intrinsic::Mrs, true, false, "mrs $0, s3_3_c15_c2_0"},
      // An MSR writes no general register: `x0` keeps its value.
      {0xd51bd041, Intrinsic::Msr, false, true, "msr tpidr_el0, $0"},
      {0xd50342df, Intrinsic::Msr, false, false, "msr daifset, #0x2"},
      {0xd503477f, Intrinsic::Msr, false, false, "smstart"},
      // Nor does a SYS, whose register is its operand.
      {0xd50b7e21, Intrinsic::Dc, false, true, "dc civac, $0"},
      {0xd50b7521, Intrinsic::Ic, false, true, "ic ivau, $0"},
      {0xd508751f, Intrinsic::Ic, false, false, "ic iallu"},
      {0xd5088720, Intrinsic::Tlbi, false, true, "tlbi vae1, $0"},
      {0xd508831f, Intrinsic::Tlbi, false, false, "tlbi vmalle1is"},
      {0xd5087800, Intrinsic::At, false, true, "at s1e1r, $0"},
      {0xd50bf021, Intrinsic::Dc, false, true, "sys #3, c15, c0, #1, $0"},
      {0xd52bf020, Intrinsic::Dc, true, false, "sysl $0, #3, c15, c0, #1"},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(C.Asm);
    const LowOp Op = liftA64(C.Word);
    ASSERT_EQ(Op.Opcode, NdOp::INTRINSIC);
    EXPECT_EQ(static_cast<Intrinsic>(Op.Inputs[0].Offset), C.Id);
    EXPECT_EQ(Op.Output.Size != 0, C.Output);
    ASSERT_GE(Op.NumInputs, 2);
    ASSERT_TRUE(Op.Inputs[1].isConst());
    EXPECT_EQ(Op.NumInputs == 3, C.Register);
    EXPECT_EQ(aarch64SystemInstructionAsm(C.Id, Op.Inputs[1].Offset, C.Output,
                                          C.Register),
              C.Asm);
  }
  // A register pair, which no intrinsic carries, fails clearly.
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::AArch64, InstructionMode::Default));
  Decode.setStrict(true);
  // msrr s3_3_c13_c0_0, x0, x1
  const uint8_t Msrr[] = {0x00, 0xd0, 0x5b, 0xd5};
  DecodedInsn Instruction;
  if (Decode.decodeOneForLift(Msrr, 4, 0x1000, Instruction) == 4) {
    std::vector<LowOp> Ops;
    EXPECT_ANY_THROW(Decode.liftToLow(Instruction, Ops));
  }
}

/// The operations the instruction \p Bytes lifts to.
std::vector<LowOp> liftOps(Arch TheArch, InstructionMode Mode,
                           std::vector<uint8_t> Bytes) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(TheArch, Mode));
  DecodedInsn Instruction;
  EXPECT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
      static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  Decode.liftToLow(Instruction, Ops);
  return Ops;
}

/// The registers \p Ops copies a temporary into after their intrinsic: the
/// results bound to it.
std::vector<uint64_t> boundResults(const std::vector<LowOp> &Ops) {
  std::vector<uint64_t> Registers;
  for (const LowOp &Op : Ops)
    if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1 && Op.Inputs[0].isTemp() &&
        Op.Output.isReg())
      Registers.push_back(Op.Output.Offset);
  return Registers;
}

TEST(MachineInstructionText, SmcccCallsBindTheirResults) {
  // SMC and HVC #0 return x0-x3 (r0-r3); another immediate is no call of
  // the convention.
  const std::vector<uint64_t> X0X3 = {0, 8, 16, 24};
  EXPECT_EQ(boundResults(liftOps(Arch::AArch64, InstructionMode::Default,
                                 {0x03, 0x00, 0x00, 0xd4})),
            X0X3); // smc #0
  EXPECT_EQ(boundResults(liftOps(Arch::AArch64, InstructionMode::Default,
                                 {0x02, 0x00, 0x00, 0xd4})),
            X0X3); // hvc #0
  EXPECT_TRUE(boundResults(liftOps(Arch::AArch64, InstructionMode::Default,
                                   {0x23, 0x00, 0x00, 0xd4}))
                  .empty()); // smc #1
  EXPECT_EQ(boundResults(liftOps(Arch::ARM, InstructionMode::ARM,
                                 {0x70, 0x00, 0x60, 0xe1})),
            (std::vector<uint64_t>{0, 4, 8, 12})); // smc #0
}

/// The listing's text of the AArch64 instruction \p Word in lowercase, with
/// capstone's `c0xf` spelled `c15`; empty where it does not decode.
std::string listingText(csh Handle, uint32_t Word) {
  const uint8_t Bytes[] = {
      static_cast<uint8_t>(Word), static_cast<uint8_t>(Word >> 8),
      static_cast<uint8_t>(Word >> 16), static_cast<uint8_t>(Word >> 24)};
  cs_insn *Insn = nullptr;
  if (cs_disasm(Handle, Bytes, 4, 0x1000, 1, &Insn) != 1)
    return {};
  std::string Text = Insn->mnemonic;
  if (Insn->op_str[0])
    Text += std::string(" ") + Insn->op_str;
  cs_free(Insn, 1);
  Text = llvm::StringRef(Text).lower();
  for (size_t At = Text.find(" c0x"); At != std::string::npos;
       At = Text.find(" c0x", At + 1)) {
    size_t End = At + 4;
    while (End < Text.size() && llvm::isHexDigit(Text[End]))
      ++End;
    unsigned Value = 0;
    llvm::StringRef(Text).slice(At + 4, End).getAsInteger(16, Value);
    Text.replace(At, End - At, " c" + std::to_string(Value));
  }
  return Text;
}

/// \p Asm with its `$0` operand named \p Register and its immediate as the
/// listing prints it: in decimal below 10, and in hexadecimal from 10.
std::string asListed(std::string Asm, llvm::StringRef Register) {
  if (const size_t Operand = Asm.find("$0"); Operand != std::string::npos)
    Asm.replace(Operand, 2, Register.str());
  if (const size_t Immediate = Asm.find("#0x");
      Immediate != std::string::npos) {
    unsigned Value = 0;
    llvm::StringRef(Asm).drop_front(Immediate + 3).getAsInteger(16, Value);
    Asm = Asm.substr(0, Immediate + 1) +
          (Value < 10 ? std::to_string(Value)
                      : "0x" + llvm::utohexstr(Value, /*LowerCase=*/true));
  }
  return Asm;
}

// The C writers name every system register, SYS operation and PSTATE field
// as the listing does.
TEST(MachineInstructionText, AArch64SystemOperandsReadAsListed) {
  csh Handle;
  ASSERT_EQ(cs_open(CS_ARCH_AARCH64, CS_MODE_LITTLE_ENDIAN, &Handle),
            CS_ERR_OK);
  std::vector<std::string> Mismatches;
  unsigned Compared = 0;
  const auto compare = [&](const std::string &Listed, const std::string &Ours) {
    ++Compared;
    if (Listed != Ours && Mismatches.size() < 20)
      Mismatches.push_back(Listed + " | " + Ours);
  };
  // MRS and MSR (register): op0 2 and 3.
  for (uint32_t Field = 0x8000; Field <= 0xFFFF; ++Field) {
    const std::string Read = listingText(Handle, 0xD5200000 | Field << 5);
    if (llvm::StringRef(Read).starts_with("mrs x0, "))
      compare(Read, asListed(aarch64SystemInstructionAsm(Intrinsic::Mrs, Field,
                                                         true, false),
                             "x0"));
    const std::string Write = listingText(Handle, 0xD5000001 | Field << 5);
    if (llvm::StringRef(Write).starts_with("msr ") &&
        llvm::StringRef(Write).ends_with(", x1"))
      compare(Write, asListed(aarch64SystemInstructionAsm(Intrinsic::Msr, Field,
                                                          false, true),
                              "x1"));
  }
  // SYS, with a register and without one: op0 1.
  for (uint32_t Field = 0x4000; Field <= 0x7FFF; ++Field)
    for (uint32_t Rt : {1u, 31u}) {
      const std::string Listed =
          listingText(Handle, 0xD5000000 | Field << 5 | Rt);
      const llvm::StringRef Mnemonic =
          llvm::StringRef(Listed).take_until([](char C) { return C == ' '; });
      if (Mnemonic != "sys" && Mnemonic != "dc" && Mnemonic != "ic" &&
          Mnemonic != "at" && Mnemonic != "tlbi")
        continue;
      const bool Register = llvm::StringRef(Listed).ends_with(", x1") ||
                            llvm::StringRef(Listed).ends_with(", xzr");
      compare(Listed, asListed(aarch64SystemInstructionAsm(Intrinsic::Dc, Field,
                                                           false, Register),
                               Rt == 31 ? "xzr" : "x1"));
    }
  // MSR (immediate): op0 0, CRn 4.
  for (uint32_t Op1 = 0; Op1 < 8; ++Op1)
    for (uint32_t CRm = 0; CRm < 16; ++CRm)
      for (uint32_t Op2 = 0; Op2 < 8; ++Op2) {
        const uint32_t Field = Op1 << 11 | 4 << 7 | CRm << 3 | Op2;
        const std::string Listed = listingText(Handle, 0xD500001F | Field << 5);
        const std::string Ours =
            aarch64SystemInstructionAsm(Intrinsic::Msr, Field, false, false);
        if ((llvm::StringRef(Listed).starts_with("msr ") ||
             llvm::StringRef(Listed).starts_with("smst")) &&
            !llvm::StringRef(Ours).starts_with(".inst"))
          compare(Listed, asListed(Ours, ""));
      }
  cs_close(&Handle);
  EXPECT_GT(Compared, 60000u);
  EXPECT_TRUE(Mismatches.empty()) << "listing | ours:\n"
                                  << llvm::join(Mismatches, "\n");
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
  C = highC(Arch::AArch64, Intrinsic::Brk, 0xF000, 2);
  EXPECT_NE(C.find("__builtin_debugtrap(); /* brk #0xF000 */"),
            std::string::npos)
      << C;
  C = highC(Arch::ARM, Intrinsic::ArmBkpt, 0x1, 2);
  EXPECT_NE(C.find("__asm__ volatile(\"bkpt #0x1\" ::: \"memory\")"),
            std::string::npos)
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
  C = highC(Arch::ARM, Intrinsic::ArmHlt, 0x1234, 2);
  EXPECT_NE(C.find("__asm__ volatile(\"hlt #0x1234\" ::: \"memory\")"),
            std::string::npos)
      << C;
}

/// HighC of one AArch64 function returning \p ReturnType with \p Body.
std::string highCOf(HighStmt Body, TypeRef ReturnType) {
  HighFunc Func;
  Func.Name = "f";
  Func.ReturnType = std::move(ReturnType);
  Body.Addr = 0x1000;
  Func.Body.push_back(std::move(Body));
  std::string C;
  llvm::raw_string_ostream OS(C);
  CEmitterOptions Options;
  Options.TheArch = Arch::AArch64;
  EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  return C;
}

ExprPtr intrinsicCall(Intrinsic Id, std::vector<ExprPtr> Operands,
                      TypeRef Type) {
  auto Call = HighExpr::makeCall("", 0x1000, std::move(Operands));
  Call->IntrinsicId = Id;
  Call->Type = std::move(Type);
  return Call;
}

HighStmt callOf(ExprPtr Call) {
  HighStmt Statement;
  Statement.Kind = StmtKind::Call;
  Statement.CallExpr = std::move(Call);
  return Statement;
}

// The code a trap keeps does not make it return: `brk #1` is clang's
// __builtin_trap() and `brk #0x3e8` GCC's, and LowIR and MedIR end the flow
// at both.  A breakpoint a debugger resumes from does not end it, nor does an
// ARM `hlt`, which LowIR and MedIR do not end the flow at either.
TEST(MachineInstructionText, TrapsEndTheFlowWithTheirCode) {
  const auto Terminates = [](Intrinsic Id, uint64_t Code) {
    return isTerminatingHighCall(
        intrinsicCall(Id, {HighExpr::makeConst(Code, 2)}, NdType::makeVoid()));
  };
  EXPECT_TRUE(Terminates(Intrinsic::Brk, 0x1));
  EXPECT_TRUE(Terminates(Intrinsic::Brk, 0x3E8));
  EXPECT_TRUE(Terminates(Intrinsic::Brk, 0xEFFF));
  EXPECT_FALSE(Terminates(Intrinsic::Brk, 0xF000));
  EXPECT_TRUE(Terminates(Intrinsic::Brk, 0xF001));
  EXPECT_TRUE(Terminates(Intrinsic::Udf_A64, 0x0));
  EXPECT_TRUE(Terminates(Intrinsic::Hlt_A64, 0x0));
  EXPECT_FALSE(Terminates(Intrinsic::ArmBkpt, 0x1));
  EXPECT_FALSE(Terminates(Intrinsic::ArmHlt, 0x1));
  // Without its code, as before.
  EXPECT_TRUE(isTerminatingHighCall(
      intrinsicCall(Intrinsic::Brk, {}, NdType::makeVoid())));
}

TEST(MachineInstructionText, HighCPrintsSystemInstructions) {
  // ACLE names the register an MRS reads and an MSR writes.
  HighStmt Read;
  Read.Kind = StmtKind::Return;
  Read.RetVal = intrinsicCall(Intrinsic::Mrs, {HighExpr::makeConst(0xDF02, 2)},
                              NdType::makeInt(8, false));
  std::string C = highCOf(std::move(Read), NdType::makeInt(8, false));
  EXPECT_NE(C.find("__arm_rsr64(\"cntvct_el0\")"), std::string::npos) << C;
  EXPECT_NE(C.find("#include <arm_acle.h>"), std::string::npos) << C;
  C = highCOf(callOf(intrinsicCall(
                  Intrinsic::Msr,
                  {HighExpr::makeConst(0xDE82, 2), HighExpr::makeConst(5, 8)},
                  NdType::makeVoid())),
              NdType::makeVoid());
  EXPECT_NE(C.find("__arm_wsr64(\"tpidr_el0\", 5);"), std::string::npos) << C;
  // Any other runs as the instruction, with its register.
  C = highCOf(callOf(intrinsicCall(Intrinsic::Dc,
                                   {HighExpr::makeConst(0x5BF1, 2),
                                    HighExpr::makeConst(0x4000, 8)},
                                   NdType::makeVoid())),
              NdType::makeVoid());
  EXPECT_NE(C.find("__asm__ volatile(\"dc civac, %0\" : : \"r\"("),
            std::string::npos)
      << C;
  C = highCOf(
      callOf(intrinsicCall(Intrinsic::Msr, {HighExpr::makeConst(0x1A16, 2)},
                           NdType::makeVoid())),
      NdType::makeVoid());
  EXPECT_NE(C.find("__asm__ volatile(\"msr daifset, #0x2\" ::: \"memory\")"),
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
  call void asm sideeffect "hlt #1", "~{memory}"()
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
  EXPECT_NE(C.find("__asm__ volatile(\"udf #0xFE\" ::: \"memory\");"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("__asm__ volatile(\"hlt #1\" ::: \"memory\");"),
            std::string::npos)
      << C;
}

TEST(MachineInstructionText, LLVMCPrintsSystemInstructions) {
  const std::string C = llvmC(R"(
define i64 @system(i64 %value) {
entry:
  %counter = call i64 asm sideeffect "mrs $0, cntvct_el0", "=r,~{memory}"()
  call void asm sideeffect "msr tpidr_el0, $0", "r,~{memory}"(i64 %value)
  call void asm sideeffect "dc civac, $0", "r,~{memory}"(i64 %value)
  %implementation = call i64 asm sideeffect "sysl $0, #3, c15, c0, #1", "=r,~{memory}"()
  %sum = add i64 %counter, %implementation
  ret i64 %sum
}
)",
                              Arch::AArch64);
  EXPECT_NE(C.find("__arm_rsr64(\"cntvct_el0\");"), std::string::npos) << C;
  EXPECT_NE(C.find("__arm_wsr64(\"tpidr_el0\", "), std::string::npos) << C;
  EXPECT_NE(C.find("#include <arm_acle.h>"), std::string::npos) << C;
  // GNU C spells LLVM's `$0` `%0`, and an output comes first.
  EXPECT_NE(C.find("__asm__ volatile(\"dc civac, %0\" : : \"r\"("),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("__asm__ volatile(\"sysl %0, #3, c15, c0, #1\" : \"=r\"("),
            std::string::npos)
      << C;
  EXPECT_EQ(C.find("$0"), std::string::npos) << C;
}

TEST(MachineInstructionText, LLVMCPortIoTakesThePortFirst) {
  // AT&T assembly writes OUT's data first; MSVC's intrinsics take the port
  // first, an immediate one as the byte the instruction holds.
  const std::string C = llvmC(R"(
define i8 @port_io(i16 %port, i32 %value) {
entry:
  %byte = call i8 asm sideeffect "inb $1, $0", "={ax},N,~{memory}"(i8 96)
  call void asm sideeffect "outb $0, $1", "{ax},N,~{memory}"(i8 %byte, i8 -128)
  call void asm sideeffect "outl $0, $1", "{ax},{dx},~{memory}"(i32 %value, i16 %port)
  %word = call i16 asm sideeffect "inw $1, $0", "={ax},{dx},~{memory}"(i16 %port)
  ret i8 %byte
}
)",
                              Arch::X64);
  EXPECT_NE(C.find("__inbyte((uint16_t)(0x60))"), std::string::npos) << C;
  EXPECT_NE(C.find("__outbyte((uint16_t)(0x80), "), std::string::npos) << C;
  EXPECT_NE(C.find("__outdword((uint16_t)(port), (uint32_t)(value))"),
            std::string::npos)
      << C;
  EXPECT_NE(C.find("__inword((uint16_t)(port))"), std::string::npos) << C;
  EXPECT_EQ(C.find("-128"), std::string::npos) << C;
}

} // namespace
