//===- IntrinsicInstructionText.cpp - The instruction behind one ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"

namespace neverd {

namespace {

std::string hexImmediate(uint64_t Value) {
  return "0x" + llvm::utohexstr(Value);
}

struct SystemRegister {
  const char *Name;
  uint16_t Encoding;
  bool Readable;
  bool Writable;
};

constexpr SystemRegister SystemRegisters[] = {
#define NEVERD_A64_SYSREG(Name, Encoding, Readable, Writable)                  \
  {Name, Encoding, Readable, Writable},
#include "neverd/ir/intrinsics/AArch64/AArch64SystemOperands.def"
};

struct SystemAlias {
  const char *Mnemonic;
  const char *Name;
  uint16_t Encoding;
  bool TakesRegister;
};

constexpr SystemAlias SystemAliases[] = {
#define NEVERD_A64_SYS_ALIAS(Mnemonic, Name, Encoding, TakesRegister)          \
  {Mnemonic, Name, Encoding, TakesRegister},
#include "neverd/ir/intrinsics/AArch64/AArch64SystemOperands.def"
};

struct PStateField {
  const char *Name;
  unsigned Op1;
  unsigned Op2;
  unsigned CRmHigh;
  unsigned ImmediateBits;
};

constexpr PStateField PStateFields[] = {
#define NEVERD_A64_PSTATE(Name, Op1, Op2, CRmHigh, ImmediateBits)              \
  {Name, Op1, Op2, CRmHigh, ImmediateBits},
#include "neverd/ir/intrinsics/AArch64/AArch64SystemOperands.def"
};

/// The fields of an op0:op1:CRn:CRm:op2 encoding.
struct SystemField {
  unsigned Op0;
  unsigned Op1;
  unsigned CRn;
  unsigned CRm;
  unsigned Op2;

  explicit SystemField(uint64_t Field)
      : Op0(Field >> 14 & 3), Op1(Field >> 11 & 7), CRn(Field >> 7 & 0xF),
        CRm(Field >> 3 & 0xF), Op2(Field & 7) {}
};

/// The operation operands of a SYS or SYSL, `#3, c7, c14, #1`.
std::string sysOperation(const SystemField &F) {
  return "#" + std::to_string(F.Op1) + ", c" + std::to_string(F.CRn) + ", c" +
         std::to_string(F.CRm) + ", #" + std::to_string(F.Op2);
}

/// An MSR (immediate), which writes a PSTATE field: CRn is 4 and CRm holds
/// the immediate.
std::string pstateWrite(const SystemField &F) {
  for (const PStateField &Field : PStateFields) {
    if (Field.Op1 != F.Op1 || Field.Op2 != F.Op2 ||
        (Field.ImmediateBits == 1 && F.CRm >> 1 != Field.CRmHigh))
      continue;
    const unsigned Immediate = Field.ImmediateBits == 1 ? F.CRm & 1 : F.CRm;
    // The listing spells an SVCR write as SMSTART or SMSTOP.
    if (Field.Op1 == 3 && Field.Op2 == 3)
      return std::string(Immediate ? "smstart" : "smstop") +
             (Field.CRmHigh == 1   ? " sm"
              : Field.CRmHigh == 2 ? " za"
                                   : "");
    return std::string("msr ") + Field.Name + ", #" + hexImmediate(Immediate);
  }
  // A field no table names runs as its encoding.
  return ".inst 0x" +
         llvm::utohexstr(0xD500401FU | F.Op1 << 16 | F.CRm << 8 | F.Op2 << 5);
}

} // namespace

bool intrinsicCarriesInstructionOperand(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Brk:
  case Intrinsic::Hlt_A64:
  case Intrinsic::Udf_A64:
  case Intrinsic::Hvc:
  case Intrinsic::Smc:
  case Intrinsic::Dmb:
  case Intrinsic::Dsb:
  case Intrinsic::Isb:
  case Intrinsic::ArmBkpt:
  case Intrinsic::ArmHlt:
  case Intrinsic::ArmUdf:
  case Intrinsic::ArmHvc:
  case Intrinsic::ArmSmc:
  case Intrinsic::ArmDmb:
  case Intrinsic::ArmDsb:
  case Intrinsic::ArmIsb:
  case Intrinsic::Ud0:
  case Intrinsic::Ud1:
    return true;
  default:
    return false;
  }
}

std::string armBarrierOptionText(uint64_t CRm) {
  switch (CRm) {
#define ARM_BARRIER_OPTION(CRM, NAME)                                          \
  case CRM:                                                                    \
    return NAME;
#include "neverd/ir/intrinsics/ARM/ARMBarrierOptions.def"
  default:
    return "#" + hexImmediate(CRm);
  }
}

std::optional<uint64_t> armBarrierOptionValue(llvm::StringRef Text) {
  Text = Text.trim();
#define ARM_BARRIER_OPTION(CRM, NAME)                                          \
  if (Text.equals_insensitive(NAME))                                           \
    return CRM;
#include "neverd/ir/intrinsics/ARM/ARMBarrierOptions.def"
  uint64_t Value = 0;
  if (Text.consume_front("#") && !Text.getAsInteger(0, Value) && Value <= 0xF)
    return Value;
  return std::nullopt;
}

std::string aarch64SystemRegisterName(uint64_t Encoding, bool Write) {
  // Where two names share an encoding, the first that may be read, or
  // written, is the listing's.
  for (const SystemRegister &Register : SystemRegisters)
    if (Register.Encoding == Encoding &&
        (Write ? Register.Writable : Register.Readable))
      return Register.Name;
  const SystemField F(Encoding);
  return "s" + std::to_string(F.Op0) + "_" + std::to_string(F.Op1) + "_c" +
         std::to_string(F.CRn) + "_c" + std::to_string(F.CRm) + "_" +
         std::to_string(F.Op2);
}

std::string aarch64SystemInstructionAsm(Intrinsic Id, uint64_t Field,
                                        bool HasOutput, bool HasRegister) {
  if (Field > 0xFFFF)
    return {};
  const SystemField F(Field);
  switch (Id) {
  case Intrinsic::Mrs:
    // MRS reads a register of op0 2 or 3 into its destination.
    if (!HasOutput || HasRegister || F.Op0 < 2)
      return {};
    return "mrs $0, " + aarch64SystemRegisterName(Field, /*Write=*/false);
  case Intrinsic::Msr:
    if (HasOutput)
      return {};
    if (F.Op0 >= 2)
      return HasRegister
                 ? "msr " + aarch64SystemRegisterName(Field, /*Write=*/true) +
                       ", $0"
                 : std::string();
    if (F.Op0 != 0 || F.CRn != 4 || HasRegister)
      return {};
    return pstateWrite(F);
  case Intrinsic::Dc:
  case Intrinsic::Ic:
  case Intrinsic::Tlbi:
  case Intrinsic::At: {
    // The operations of op0 1: SYS, its DC, IC, AT and TLBI aliases, and
    // SYSL, which reads a result into its register.
    if (F.Op0 != 1)
      return {};
    if (HasOutput)
      return HasRegister ? std::string() : "sysl $0, " + sysOperation(F);
    for (const SystemAlias &Alias : SystemAliases)
      if (Alias.Encoding == (Field & 0x3FFF) &&
          Alias.TakesRegister == HasRegister)
        return std::string(Alias.Mnemonic) + " " + Alias.Name +
               (HasRegister ? ", $0" : "");
    return "sys " + sysOperation(F) + (HasRegister ? ", $0" : "");
  }
  default:
    return {};
  }
}

std::optional<ArmCoprocessorOperands> armCoprocessorOperands(Intrinsic Id,
                                                             uint64_t Field) {
  const ArmCoprocessorOperands Operands{static_cast<unsigned>(Field >> 16),
                                        static_cast<unsigned>(Field >> 12) &
                                            0xF,
                                        static_cast<unsigned>(Field >> 8) & 0xF,
                                        static_cast<unsigned>(Field >> 4) & 0xF,
                                        static_cast<unsigned>(Field) & 0xF};
  if (Operands.Coprocessor > 15)
    return std::nullopt;
  switch (Id) {
  case Intrinsic::ArmMrc:
  case Intrinsic::ArmMcr:
    if (Operands.Opc1 > 7 || Operands.Opc2 > 7)
      return std::nullopt;
    return Operands;
  case Intrinsic::ArmMrrc:
  case Intrinsic::ArmMcrr:
    if (Operands.CRn != 0 || Operands.Opc2 != 0)
      return std::nullopt;
    return Operands;
  default:
    return std::nullopt;
  }
}

std::string armCoprocessorInstructionAsm(Intrinsic Id, uint64_t Field) {
  const std::optional<ArmCoprocessorOperands> O =
      armCoprocessorOperands(Id, Field);
  if (!O)
    return {};
  const std::string Head = std::string(intrinsicAsmMnemonic(Id)) + " p" +
                           std::to_string(O->Coprocessor) + ", #" +
                           std::to_string(O->Opc1) + ", ";
  if (Id == Intrinsic::ArmMrrc || Id == Intrinsic::ArmMcrr)
    return Head + "${0:Q}, ${0:R}, c" + std::to_string(O->CRm);
  return Head + "$0, c" + std::to_string(O->CRn) + ", c" +
         std::to_string(O->CRm) + ", #" + std::to_string(O->Opc2);
}

std::optional<std::pair<Intrinsic, uint64_t>>
armCoprocessorAsmField(llvm::StringRef Text) {
  const auto [Mnemonic, Rest] = Text.split(' ');
  const std::optional<Intrinsic> Id =
      llvm::StringSwitch<std::optional<Intrinsic>>(Mnemonic)
          .Case("mrc", Intrinsic::ArmMrc)
          .Case("mcr", Intrinsic::ArmMcr)
          .Case("mrrc", Intrinsic::ArmMrrc)
          .Case("mcrr", Intrinsic::ArmMcrr)
          .Default(std::nullopt);
  if (!Id)
    return std::nullopt;
  // Each number follows its prefix (`p15`, `#0`, `c13`); the text is the
  // one armCoprocessorInstructionAsm writes for the field they make.
  llvm::SmallVector<llvm::StringRef, 6> Operands;
  Rest.split(Operands, ", ");
  auto number = [](llvm::StringRef Operand, llvm::StringRef Prefix) {
    unsigned Value = 0;
    return Operand.consume_front(Prefix) && !Operand.getAsInteger(10, Value)
               ? std::optional<unsigned>(Value)
               : std::nullopt;
  };
  const bool Pair = *Id == Intrinsic::ArmMrrc || *Id == Intrinsic::ArmMcrr;
  if (Operands.size() != (Pair ? 5u : 6u))
    return std::nullopt;
  const std::optional<unsigned> Coprocessor = number(Operands[0], "p");
  const std::optional<unsigned> Opc1 = number(Operands[1], "#");
  const std::optional<unsigned> CRn =
      Pair ? std::optional<unsigned>(0) : number(Operands[3], "c");
  const std::optional<unsigned> CRm = number(Operands[4], "c");
  const std::optional<unsigned> Opc2 =
      Pair ? std::optional<unsigned>(0) : number(Operands[5], "#");
  if (!Coprocessor || !Opc1 || !CRn || !CRm || !Opc2 || *Coprocessor > 15 ||
      *Opc1 > 15 || *CRn > 15 || *CRm > 15 || *Opc2 > 15)
    return std::nullopt;
  const uint64_t Field =
      armCoprocessorField(*Coprocessor, *Opc1, *CRn, *CRm, *Opc2);
  if (armCoprocessorInstructionAsm(*Id, Field) != Text)
    return std::nullopt;
  return std::pair(*Id, Field);
}

std::string armStatusInstructionAsm(Intrinsic Id, uint64_t Field) {
  switch (Id) {
  case Intrinsic::ArmMrs:
    if (Field == ArmMrsApsr)
      return "mrs $0, apsr";
    if (Field == ArmMrsSpsr)
      return "mrs $0, spsr";
    return {};
  case Intrinsic::ArmMsr: {
    const uint64_t Mask = Field & 0xF;
    if (Mask == 0 || (Field & ~(ArmMsrSpsr | 0xF)) != 0)
      return {};
    // The assembler spells the fields in the order f, s, x, c.
    std::string Text = Field & ArmMsrSpsr ? "msr spsr_" : "msr cpsr_";
    for (const auto &[Bit, Letter] :
         {std::pair{8u, 'f'}, {4u, 's'}, {2u, 'x'}, {1u, 'c'}})
      if (Mask & Bit)
        Text += Letter;
    return Text + ", $0";
  }
  case Intrinsic::ArmCps: {
    const uint64_t Imod = Field >> 9, Masks = (Field >> 6) & 7;
    const bool ChangesMode = (Field >> 5) & 1;
    const uint64_t Mode = Field & 0x1F;
    // A CPS that enables or disables changes a mask, and one that does
    // neither enters a mode; any other is UNPREDICTABLE.
    if (Field > 0x7FF || Imod == 1 || (Imod == 0) != (Masks == 0) ||
        (Imod == 0 && !ChangesMode) || (!ChangesMode && Mode != 0))
      return {};
    std::string Text = Imod == 2 ? "cpsie " : Imod == 3 ? "cpsid " : "cps ";
    for (const auto &[Bit, Letter] : {std::pair{4u, 'a'}, {2u, 'i'}, {1u, 'f'}})
      if (Masks & Bit)
        Text += Letter;
    if (ChangesMode)
      Text += (Imod ? ", #" : "#") + hexImmediate(Mode);
    return Text;
  }
  default:
    return {};
  }
}

std::string intrinsicInstructionText(Intrinsic Id,
                                     std::optional<uint64_t> Operand) {
  if (!Operand || !intrinsicCarriesInstructionOperand(Id))
    return {};
  // A barrier's C name (`__dmb`) is not its mnemonic.
  switch (Id) {
  case Intrinsic::Dmb:
  case Intrinsic::ArmDmb:
    return "dmb " + armBarrierOptionText(*Operand);
  case Intrinsic::Dsb:
  case Intrinsic::ArmDsb:
    return "dsb " + armBarrierOptionText(*Operand);
  case Intrinsic::Isb:
  case Intrinsic::ArmIsb:
    // SY is the only ISB option; the assembler spells it without one.
    return *Operand == 0xF ? std::string("isb")
                           : "isb #" + hexImmediate(*Operand);
  case Intrinsic::ArmUdf:
    // Thumb requires an explicit wide suffix for a code that does not fit
    // imm8. Unified assembly accepts it in ARM mode as well.
    return std::string(*Operand > 0xFF ? "udf.w #" : "udf #") +
           hexImmediate(*Operand);
  default:
    break;
  }
  const char *Mnemonic = intrinsicAsmMnemonic(Id);
  if (!Mnemonic)
    return {};
  switch (Id) {
  case Intrinsic::Ud0:
  case Intrinsic::Ud1:
    // The lifters keep the displacement of the form clang's trapping
    // sanitizers emit, `ud1 eax, [eax + kind]`, whose kind names the check
    // that failed.
    return std::string(Mnemonic) + " eax, dword ptr [eax + " +
           hexImmediate(*Operand) + "]";
  default:
    return std::string(Mnemonic) + " #" + hexImmediate(*Operand);
  }
}

} // namespace neverd
