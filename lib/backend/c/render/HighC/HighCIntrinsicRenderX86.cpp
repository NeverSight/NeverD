//===- HighCIntrinsicRenderX86.cpp - x86 intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific HighIR intrinsic rendering: multi-output CPUID/RDTSC/XGETBV
/// emission, `__fastfail`, GS/FS reads, single-output x86 intrinsic calls,
/// and hi/lo collapse patterns.
///
//===----------------------------------------------------------------------===//

#include "../X86FPStateHelpers.h"

#include "neverd/Limits.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/backend/c/render/X86SegmentAsm.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/LLVMX86X87StateAsm.h"
#include "neverd/ir/high/X86FPStateShape.h"
#include "neverd/ir/intrinsics/X86Interrupts.h"
#include "neverd/ir/intrinsics/X86SegmentRegisters.h"
#include "neverd/ir/intrinsics/X86StringCompare.h"
#include "neverd/lift/X86Regs.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <string>
#include <utility>

namespace neverd {

llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders() {
  return {"immintrin.h"};
}

const char *x86SegmentedReadIntrinsic(bool GS, unsigned SizeBytes) {
  if (GS) {
    switch (SizeBytes) {
    case 8:
      return "__readgsqword";
    case 4:
      return "__readgsdword";
    case 2:
      return "__readgsword";
    case 1:
      return "__readgsbyte";
    default:
      return nullptr;
    }
  }
  switch (SizeBytes) {
  case 8:
    return "__readfsqword";
  case 4:
    return "__readfsdword";
  case 2:
    return "__readfsword";
  case 1:
    return "__readfsbyte";
  default:
    return nullptr;
  }
}

llvm::ArrayRef<const char *> x86DebugServiceRegisters() {
  static const char *const Regs[] = {"rax", "rcx", "rdx", "r8", "r9"};
  return Regs;
}

std::string renderX86InterruptAsm(
    unsigned Vector,
    llvm::ArrayRef<std::pair<const char *, std::string>> Inputs,
    llvm::StringRef ResultVar, llvm::StringRef ResultReg,
    llvm::StringRef Instruction) {
  // MASM hex: a leading digit, then an `h` suffix (`int 2Dh`, `int 0CCh`).
  std::string Hex = llvm::utohexstr(Vector & 0xFF);
  if (!std::isdigit(static_cast<unsigned char>(Hex.front())))
    Hex = "0" + Hex;
  const std::string Pad = Inputs.empty() ? "" : "    ";
  std::string Asm = Pad + "__asm {\n";
  for (const auto &[Reg, Value] : Inputs)
    Asm += Pad + "    mov " + std::string(Reg) + ", _" + Reg + "\n";
  Asm += Pad + "    " +
         (Instruction.empty() ? "int " + Hex + "h" : Instruction.str()) + "\n";
  if (!ResultVar.empty())
    Asm += Pad + "    mov " + ResultVar.str() + ", " + ResultReg.str() + "\n";
  Asm += Pad + "}\n";
  if (Inputs.empty())
    return Asm;
  std::string Result = "{\n";
  for (const auto &[Reg, Value] : Inputs)
    Result +=
        "    uint64_t _" + std::string(Reg) + " = (uint64_t)(" + Value + ");\n";
  return Result + Asm + "}\n";
}

const char *x87CHelperName(X87CHelper Helper) {
  switch (Helper) {
  case X87CHelper::Frndint:
    return "neverd_x87_frndint";
  case X87CHelper::Fsqrt:
    return "neverd_x87_fsqrt";
  case X87CHelper::ControlWord:
    return "neverd_x87_control_word";
  case X87CHelper::Fprem:
    return "neverd_x87_fprem";
#define NEVERD_X87_VALUE_HELPER(ID, Name, Asm, Operands, PopsST1)              \
  case X87CHelper::ID:                                                         \
    return Name;
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
  }
  llvm_unreachable("unknown x87 C helper");
}

bool isX87ControlWord(Arch TheArch, const MedVar &V) {
  return (TheArch == Arch::X86 || TheArch == Arch::X64) &&
         V.Kind == MedVar::Reg && V.RegOff == x86reg::FPU_CW && V.Size == 2;
}

/// The operands the x87 value instruction of \p Helper reads.
static unsigned x87ValueHelperOperands(X87CHelper Helper) {
  switch (Helper) {
#define NEVERD_X87_VALUE_HELPER(ID, Name, Asm, Operands, PopsST1)              \
  case X87CHelper::ID:                                                         \
    return Operands;
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
  default:
    return 0;
  }
}

std::optional<X87CHelper> x87ValueHelper(Intrinsic Id) {
  switch (Id) {
#define NEVERD_X87_VALUE_HELPER(ID, Name, Asm, Operands, PopsST1)              \
  case Intrinsic::ID:                                                          \
    return X87CHelper::ID;
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
  default:
    return std::nullopt;
  }
}

void writeX87CHelpers(llvm::raw_ostream &OS, bool UsesExtended,
                      const std::set<X87CHelper> &Used) {
  // An x87 register holds 80 bits; C computes with them as `long double`
  // only where that is the x87 extended format, which the unit requires.
  // The bits travel in an `unsigned _BitInt(80)` of the same size, so one
  // __builtin_bit_cast turns either into the other.
  if (UsesExtended)
    OS << "_Static_assert(__LDBL_MANT_DIG__ == 64,\n"
          "               \"x87 values need the 80-bit extended long "
          "double\");\n"
          "_Static_assert(sizeof(long double) == sizeof(unsigned "
          "_BitInt(80)),\n"
          "               \"x87 bits travel in a long double's "
          "storage\");\n\n";
  // C's rint, nearbyint and sqrtl live in the C library; each instruction
  // computes in place as the machine did.
  for (const auto &[Helper, Mnemonic] :
       {std::pair{X87CHelper::Frndint, "frndint"},
        std::pair{X87CHelper::Fsqrt, "fsqrt"}})
    if (Used.count(Helper))
      OS << "static inline long double " << x87CHelperName(Helper)
         << "(long double value) {\n"
         << "    __asm__(\"" << Mnemonic << "\" : \"+t\"(value));\n"
         << "    return value;\n"
         << "}\n\n";
  // Each instruction runs on its operands in st(0) and st(1), as the GCC
  // manual's x87 operand rules spell it: an instruction that pops st(1)
  // clobbers it.
  auto WriteValueHelper = [&](X87CHelper Helper, const char *Asm,
                              unsigned Operands, bool PopsST1) {
    if (!Used.count(Helper))
      return;
    OS << "static inline unsigned _BitInt(80) " << x87CHelperName(Helper)
       << "(unsigned _BitInt(80) x"
       << (Operands == 2 ? ", unsigned _BitInt(80) y" : "") << ") {\n"
       << "    long double value = 0" << (Operands == 2 ? ", other = 0" : "")
       << ";\n"
       << "    __builtin_memcpy(&value, &x, 10);\n";
    if (Operands == 2)
      OS << "    __builtin_memcpy(&other, &y, 10);\n";
    OS << "    __asm__(\"" << Asm << "\" : \"=t\"(value) : \"0\"(value)"
       << (Operands == 2 ? ", \"u\"(other)" : "")
       << (PopsST1 ? " : \"st(1)\"" : "") << ");\n"
       << "    unsigned _BitInt(80) bits = 0;\n"
       << "    __builtin_memcpy(&bits, &value, 10);\n"
       << "    return bits;\n"
       << "}\n\n";
  };
#define NEVERD_X87_VALUE_HELPER(ID, Name, Asm, Operands, PopsST1)              \
  WriteValueHelper(X87CHelper::ID, Asm, Operands, PopsST1);
#include "neverd/ir/intrinsics/X87ValueInstructions.def"
  // The program runs with the control word the unit holds.
  if (Used.count(X87CHelper::ControlWord))
    OS << "static inline uint16_t neverd_x87_control_word(void) {\n"
          "    uint16_t word;\n"
          "    __asm__ volatile(\"fnstcw %0\" : \"=m\"(word));\n"
          "    return word;\n"
          "}\n\n";
  if (!Used.count(X87CHelper::Fprem))
    return;
  // The status must be sampled inside the same asm block as FPREM. A separate
  // C expression could let the compiler spill an x87 value before FNSTSW and
  // thereby change the condition codes observed by the source program.
  OS << "static _Thread_local uint16_t neverd_x87_fprem_status;\n"
        "static _Thread_local unsigned char "
        "neverd_x87_fprem_status_pending;\n\n"
        "static inline _BitInt(80) neverd_x87_partial_remainder(\n"
        "    _BitInt(80) dividend, _BitInt(80) divisor, int nearest) {\n"
        "    unsigned char lhs[10], rhs[10], result[10];\n"
        "    uint16_t status;\n"
        "    __builtin_memcpy(lhs, &dividend, 10);\n"
        "    __builtin_memcpy(rhs, &divisor, 10);\n"
        "    if (nearest) {\n"
        "        __asm__ volatile(\"fldt %[rhs]\\n\\t"
        "fldt %[lhs]\\n\\tfprem1\\n\\tfnstsw %%ax\\n\\t"
        "fstpt %[result]\\n\\tfstp %%st(0)\"\n"
        "            : [result] \"=m\"(result), \"=a\"(status)\n"
        "            : [lhs] \"m\"(lhs), [rhs] \"m\"(rhs)\n"
        "            : \"cc\", \"memory\", \"st\", \"st(1)\");\n"
        "    } else {\n"
        "        __asm__ volatile(\"fldt %[rhs]\\n\\t"
        "fldt %[lhs]\\n\\tfprem\\n\\tfnstsw %%ax\\n\\t"
        "fstpt %[result]\\n\\tfstp %%st(0)\"\n"
        "            : [result] \"=m\"(result), \"=a\"(status)\n"
        "            : [lhs] \"m\"(lhs), [rhs] \"m\"(rhs)\n"
        "            : \"cc\", \"memory\", \"st\", \"st(1)\");\n"
        "    }\n"
        "    neverd_x87_fprem_status = status;\n"
        "    neverd_x87_fprem_status_pending = 1;\n"
        "    _BitInt(80) bits = 0;\n"
        "    __builtin_memcpy(&bits, result, 10);\n"
        "    return bits;\n"
        "}\n\n"
        "static inline _BitInt(80) neverd_x87_fprem(\n"
        "    _BitInt(80) dividend, _BitInt(80) divisor) {\n"
        "    return neverd_x87_partial_remainder(dividend, divisor, 0);\n"
        "}\n\n"
        "static inline _BitInt(80) neverd_x87_fprem1(\n"
        "    _BitInt(80) dividend, _BitInt(80) divisor) {\n"
        "    return neverd_x87_partial_remainder(dividend, divisor, 1);\n"
        "}\n\n"
        "static inline uint16_t neverd_x87_read_status(void) {\n"
        "    if (!neverd_x87_fprem_status_pending) __builtin_trap();\n"
        "    neverd_x87_fprem_status_pending = 0;\n"
        "    return neverd_x87_fprem_status;\n"
        "}\n\n";
}

namespace {

bool isAlive(const MedVar &V, const IsAliveFn &Fn) { return !Fn || Fn(V); }

const char *segmentPrefix(NdMemoryAddressSpace AddressSpace) {
  switch (AddressSpace) {
  case NdMemoryAddressSpace::X86FS:
    return "fs";
  case NdMemoryAddressSpace::X86GS:
    return "gs";
  case NdMemoryAddressSpace::Default:
    return "";
  }
  return nullptr;
}

const char *stringMnemonic(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Movsb:
    return "movsb";
  case I::Movsw:
    return "movsw";
  case I::Movsd:
    return "movsl";
  case I::Movsq:
    return "movsq";
  case I::Stosb:
    return "stosb";
  case I::Stosw:
    return "stosw";
  case I::Stosd:
    return "stosl";
  case I::Stosq:
    return "stosq";
  case I::Lodsb:
    return "lodsb";
  case I::Lodsw:
    return "lodsw";
  case I::Lodsd:
    return "lodsl";
  case I::Lodsq:
    return "lodsq";
  case I::Cmpsb:
    return "cmpsb";
  case I::Cmpsw:
    return "cmpsw";
  case I::Cmpsd_str:
    return "cmpsl";
  case I::Cmpsq:
    return "cmpsq";
  case I::Scasb:
    return "scasb";
  case I::Scasw:
    return "scasw";
  case I::Scasd:
    return "scasl";
  case I::Scasq:
    return "scasq";
  case I::Outsb:
    return "outsb";
  case I::Outsw:
    return "outsw";
  case I::Outsd:
    return "outsl";
  case I::Insb:
    return "insb";
  case I::Insw:
    return "insw";
  case I::Insd:
    return "insl";
  default:
    return nullptr;
  }
}

bool isMovs(Intrinsic Id) {
  return Id == Intrinsic::Movsb || Id == Intrinsic::Movsw ||
         Id == Intrinsic::Movsd || Id == Intrinsic::Movsq;
}

bool isStos(Intrinsic Id) {
  return Id == Intrinsic::Stosb || Id == Intrinsic::Stosw ||
         Id == Intrinsic::Stosd || Id == Intrinsic::Stosq;
}

/// The <intrin.h> element type a MOVS/STOS intrinsic copies or stores.
const char *stringElementType(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Movsb:
  case I::Stosb:
    return "unsigned char";
  case I::Movsw:
  case I::Stosw:
    return "unsigned short";
  case I::Movsd:
  case I::Stosd:
    return "unsigned long";
  case I::Movsq:
  case I::Stosq:
    return "unsigned long long";
  default:
    return nullptr;
  }
}

bool isLods(Intrinsic Id) {
  return Id == Intrinsic::Lodsb || Id == Intrinsic::Lodsw ||
         Id == Intrinsic::Lodsd || Id == Intrinsic::Lodsq;
}

bool isCmps(Intrinsic Id) {
  return Id == Intrinsic::Cmpsb || Id == Intrinsic::Cmpsw ||
         Id == Intrinsic::Cmpsd_str || Id == Intrinsic::Cmpsq;
}

bool isScas(Intrinsic Id) {
  return Id == Intrinsic::Scasb || Id == Intrinsic::Scasw ||
         Id == Intrinsic::Scasd || Id == Intrinsic::Scasq;
}

bool isOuts(Intrinsic Id) {
  return Id == Intrinsic::Outsb || Id == Intrinsic::Outsw ||
         Id == Intrinsic::Outsd;
}

bool isIns(Intrinsic Id) {
  return Id == Intrinsic::Insb || Id == Intrinsic::Insw ||
         Id == Intrinsic::Insd;
}

std::string assignPrimary(const HighExpr *Dst, const std::string &Value,
                          std::function<std::string(const HighExpr &)> ExprFn,
                          const IsAliveFn &IsAlive) {
  if (!Dst || Dst->Kind != ExprKind::Var || !isAlive(Dst->Var, IsAlive))
    return {};
  return "    " + ExprFn(*Dst) + " = (" + typeToC(Dst->Type) + ")" + Value +
         ";\n";
}

std::string
renderSegmentedString(Arch TheArch, const HighExpr &Call,
                      const HighExpr *PrimaryDst,
                      std::function<std::string(const HighExpr &)> ExprFn,
                      std::function<std::string(const MedVar &)> VarFn,
                      const IsAliveFn &IsAlive, bool MsvcIntrinsics) {
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  const char *Mnemonic = stringMnemonic(Call.IntrinsicId);
  if (!Mnemonic)
    return {};
  if (!Segment)
    llvm::report_fatal_error(
        "x86 REP string intrinsic has an unknown memory address space");

  const bool IsMovs = isMovs(Call.IntrinsicId);
  const bool IsStos = isStos(Call.IntrinsicId);
  const bool IsLods = isLods(Call.IntrinsicId);
  const bool IsCmps = isCmps(Call.IntrinsicId);
  const bool IsScas = isScas(Call.IntrinsicId);
  const bool IsOuts = isOuts(Call.IntrinsicId);
  const bool IsIns = isIns(Call.IntrinsicId);
  const size_t RequiredOperands = (IsCmps || IsScas) ? 5 : 4;
  if ((!IsMovs && !IsStos && !IsLods && !IsCmps && !IsScas && !IsOuts &&
       !IsIns) ||
      Call.Operands.size() < RequiredOperands)
    llvm::report_fatal_error(
        "x86 REP string intrinsic is missing architectural operands");
  for (size_t I = 0; I < RequiredOperands; ++I)
    if (!Call.Operands[I])
      llvm::report_fatal_error(
          "x86 REP string intrinsic is missing architectural operands");
  if ((IsLods || IsCmps || IsScas) &&
      (!PrimaryDst || PrimaryDst->Kind != ExprKind::Var))
    llvm::report_fatal_error(
        "x86 REP string intrinsic is missing an architectural result");
  if (*Segment != '\0' && (IsStos || IsScas || IsIns))
    llvm::report_fatal_error(
        "x86 REP string intrinsic has an invalid segment override");
  if (!Call.Operands[0]->Type)
    llvm::report_fatal_error(
        "x86 REP string intrinsic has an untyped address operand");
  const unsigned NativeAddressBytes = TheArch == Arch::X64 ? 8 : 4;
  const unsigned StringAddressBytes = Call.Operands[0]->Type->Size;
  std::string AddressPrefix;
  if (StringAddressBytes != NativeAddressBytes) {
    if (StringAddressBytes == 4 && NativeAddressBytes == 8)
      AddressPrefix = "addr32 ";
    else if (StringAddressBytes == 2 && NativeAddressBytes == 4)
      AddressPrefix = "addr16 ";
    else
      llvm::report_fatal_error("unsupported x86 string address size");
  }

  // Windows code clears DF across calls. A flat forward MOVS/STOS is the
  // <intrin.h> copy or fill of the same elements; the register results are
  // separate IR values, so nothing below reads the updated pointers.
  const bool Forward = Call.Operands[3]->Kind == ExprKind::Const &&
                       Call.Operands[3]->ConstVal == 0;
  const char *ElementType = stringElementType(Call.IntrinsicId);
  const char *Builtin = intrinsicCName(Call.IntrinsicId);
  const bool UseBuiltin = MsvcIntrinsics && (IsMovs || IsStos) && Forward &&
                          *Segment == '\0' && AddressPrefix.empty() &&
                          ElementType && Builtin;

  std::string Result = "do {\n";
  if (IsMovs || IsLods || IsCmps || IsOuts)
    Result += "    uintptr_t neverd_si = (uintptr_t)(" +
              ExprFn(*Call.Operands[0]) + ");\n";
  if (IsMovs || IsStos || IsCmps || IsScas || IsIns) {
    const size_t DiIndex = (IsMovs || IsCmps) ? 1 : 0;
    Result += "    uintptr_t neverd_di = (uintptr_t)(" +
              ExprFn(*Call.Operands[DiIndex]) + ");\n";
  }
  size_t CountIndex = 1;
  if (IsMovs || IsCmps) {
    CountIndex = 2;
  }
  Result += "    uintptr_t neverd_cx = (uintptr_t)(" +
            ExprFn(*Call.Operands[CountIndex]) + ");\n";
  if (IsLods || IsStos || IsScas)
    Result += "    uintptr_t neverd_ax = (uintptr_t)(" +
              ExprFn(*Call.Operands[2]) + ");\n";
  if (IsCmps)
    Result += "    uint16_t neverd_flags;\n";
  if (IsOuts || IsIns)
    Result += "    uint16_t neverd_dx = (uint16_t)(" +
              ExprFn(*Call.Operands[2]) + ");\n";
  if (UseBuiltin) {
    const std::string Type = ElementType;
    Result += "    " + std::string(Builtin) + "((" + Type + " *)neverd_di, " +
              (IsMovs ? "(const " + Type + " *)neverd_si"
                      : "(" + Type + ")neverd_ax") +
              ", neverd_cx);\n} while (0);\n";
    return Result;
  }

  auto EmitAsm = [&](const char *Repeat, bool Backward) {
    Result += "        __asm__ volatile(\"";
    Result += Backward ? "std\\n\\t" : "cld\\n\\t";
    Result += AddressPrefix;
    if (*Segment != '\0')
      Result += std::string(Segment) + " ";
    Result += std::string(Repeat) + " " + Mnemonic;
    if (IsCmps || IsScas)
      Result += "\\n\\tlahf\\n\\tseto %%al";
    if (Backward)
      Result += "\\n\\tcld";
    Result += "\"\n";
    if (IsCmps)
      Result += "            : \"+S\"(neverd_si), \"+D\"(neverd_di), "
                "\"+c\"(neverd_cx), \"=a\"(neverd_flags)\n";
    else if (IsScas)
      Result += "            : \"+D\"(neverd_di), \"+c\"(neverd_cx), "
                "\"+a\"(neverd_ax)\n";
    else if (IsMovs)
      Result += "            : \"+S\"(neverd_si), \"+D\"(neverd_di), "
                "\"+c\"(neverd_cx)\n";
    else if (IsOuts)
      Result += "            : \"+S\"(neverd_si), \"+c\"(neverd_cx)\n"
                "            : \"d\"(neverd_dx)\n";
    else if (IsIns)
      Result += "            : \"+D\"(neverd_di), \"+c\"(neverd_cx)\n"
                "            : \"d\"(neverd_dx)\n";
    else if (IsStos)
      Result += "            : \"+D\"(neverd_di), \"+c\"(neverd_cx)\n";
    else
      Result += "            : \"+S\"(neverd_si), \"+c\"(neverd_cx), "
                "\"+a\"(neverd_ax)\n";
    if (IsStos)
      Result += "            : \"a\"(neverd_ax)\n";
    else if (!IsOuts && !IsIns)
      Result += "            :\n";
    Result += "            : \"memory\", \"cc\");\n";
  };

  // A constant direction (DF is clear on entry) or repeat condition selects
  // one form; print only that one.
  auto ConstOperand = [&](size_t I) -> std::optional<bool> {
    if (I < Call.Operands.size() && Call.Operands[I] &&
        Call.Operands[I]->Kind == ExprKind::Const)
      return Call.Operands[I]->ConstVal != 0;
    return std::nullopt;
  };
  auto EmitDirection = [&](bool Backward) {
    if (!(IsCmps || IsScas)) {
      EmitAsm("rep", Backward);
      return;
    }
    if (auto Repne = ConstOperand(4)) {
      EmitAsm(*Repne ? "repnz" : "repz", Backward);
      return;
    }
    Result += "        if (" + ExprFn(*Call.Operands[4]) + ") {\n";
    EmitAsm("repnz", Backward);
    Result += "        } else {\n";
    EmitAsm("repz", Backward);
    Result += "        }\n";
  };
  if (auto Backward = ConstOperand(3)) {
    EmitDirection(*Backward);
  } else {
    Result += "    if (" + ExprFn(*Call.Operands[3]) + ") {\n";
    EmitDirection(true);
    Result += "    } else {\n";
    EmitDirection(false);
    Result += "    }\n";
  }

  if (IsLods)
    Result += assignPrimary(PrimaryDst, "neverd_ax", ExprFn, IsAlive);
  if (IsCmps || IsScas) {
    Result += assignPrimary(PrimaryDst, "neverd_cx", ExprFn, IsAlive);
    // No flag snapshot when no later instruction reads the flags.
    if (!Call.IntrinsicOutputs.empty()) {
      const MedVar &Flags = Call.IntrinsicOutputs.front();
      if (!IsAlive || IsAlive(Flags))
        Result += "    " + VarFn(Flags) + " = (uint16_t)" +
                  (IsScas ? "neverd_ax" : "neverd_flags") + ";\n";
    }
  }
  Result += "} while (0);\n";
  return Result;
}

std::string
renderMaskedByteStore(const HighExpr &Call,
                      std::function<std::string(const HighExpr &)> ExprFn) {
  if (Call.IntrinsicId != Intrinsic::MaskedStoreB || Call.Operands.size() < 3 ||
      !Call.Operands[0] || !Call.Operands[1] || !Call.Operands[2] ||
      !Call.Operands[1]->Type || !Call.Operands[2]->Type)
    return {};
  const unsigned VectorBytes = Call.Operands[1]->Type->Size;
  if ((VectorBytes != 8 && VectorBytes != 16) ||
      Call.Operands[2]->Type->Size != VectorBytes)
    return {};

  std::string PointerType = "uint8_t *";
  switch (Call.MemoryAddressSpace) {
  case NdMemoryAddressSpace::Default:
    break;
  case NdMemoryAddressSpace::X86FS:
    PointerType = "uint8_t __attribute__((address_space(" +
                  std::to_string(kLLVMX86FSAddressSpace) + "))) *";
    break;
  case NdMemoryAddressSpace::X86GS:
    PointerType = "uint8_t __attribute__((address_space(" +
                  std::to_string(kLLVMX86GSAddressSpace) + "))) *";
    break;
  default:
    return {};
  }

  std::string Result = "do {\n";
  Result += "    uintptr_t neverd_address = (uintptr_t)(" +
            ExprFn(*Call.Operands[0]) + ");\n";
  Result += "    unsigned __int128 neverd_mask = (unsigned __int128)(" +
            ExprFn(*Call.Operands[1]) + ");\n";
  Result += "    unsigned __int128 neverd_data = (unsigned __int128)(" +
            ExprFn(*Call.Operands[2]) + ");\n";
  Result += "    for (unsigned neverd_i = 0; neverd_i < " +
            std::to_string(VectorBytes) +
            "; ++neverd_i)\n"
            "        if (((neverd_mask >> (neverd_i * 8)) & 0x80u) != 0)\n"
            "            *(" +
            PointerType +
            ")(uintptr_t)(neverd_address + neverd_i) = "
            "(uint8_t)(neverd_data >> (neverd_i * 8));\n";
  Result += "} while (0);\n";
  return Result;
}

std::string
renderSegmentedMaskedMemory(const HighExpr &Call, const HighExpr *PrimaryDst,
                            std::function<std::string(const HighExpr &)> ExprFn,
                            const IsAliveFn &IsAlive) {
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  if (!Segment)
    return {};
  const std::string MemoryOperand =
      *Segment == '\0' ? "(%[address])"
                       : "%%" + std::string(Segment) + ":(%[address])";
  const bool IsLoad = Call.IntrinsicId == Intrinsic::MaskedLoadD ||
                      Call.IntrinsicId == Intrinsic::MaskedLoadQ;
  const bool IsStore = Call.IntrinsicId == Intrinsic::MaskedStoreD ||
                       Call.IntrinsicId == Intrinsic::MaskedStoreQ;
  const bool IsQword = Call.IntrinsicId == Intrinsic::MaskedLoadQ ||
                       Call.IntrinsicId == Intrinsic::MaskedStoreQ;
  const size_t RequiredOperands = IsLoad ? 2 : 3;
  if ((!IsLoad && !IsStore) || Call.Operands.size() < RequiredOperands)
    return {};
  for (size_t I = 0; I < RequiredOperands; ++I)
    if (!Call.Operands[I])
      return {};
  if (!Call.Operands[1]->Type || (Call.Operands[1]->Type->Size != 16 &&
                                  Call.Operands[1]->Type->Size != 32))
    return {};
  const unsigned VectorBytes = Call.Operands[1]->Type->Size;
  if (IsLoad && (!PrimaryDst || PrimaryDst->Kind != ExprKind::Var ||
                 !PrimaryDst->Type || PrimaryDst->Type->Size != VectorBytes))
    return {};
  if (IsStore &&
      (!Call.Operands[2]->Type || Call.Operands[2]->Type->Size != VectorBytes))
    return {};

  std::string Result = "do {\n";
  Result += "    uintptr_t neverd_address = (uintptr_t)(" +
            ExprFn(*Call.Operands[0]) + ");\n";
  if (VectorBytes == 16) {
    Result += "    unsigned __int128 neverd_mask = (unsigned __int128)(" +
              ExprFn(*Call.Operands[1]) + ");\n";
  } else {
    if (Call.Operands[1]->Kind == ExprKind::Const)
      Result += "    uint256_t neverd_mask_bits = "
                "(uint256_t)(unsigned __int128)(" +
                ExprFn(*Call.Operands[1]) + ");\n";
    else
      Result +=
          "    uint256_t neverd_mask_bits = " + ExprFn(*Call.Operands[1]) +
          ";\n";
    Result += "    __m256i neverd_mask;\n"
              "    __builtin_memcpy(&neverd_mask, &neverd_mask_bits, "
              "sizeof(neverd_mask));\n";
  }
  if (IsLoad) {
    Result += VectorBytes == 16 ? "    unsigned __int128 neverd_result;\n"
                                : "    __m256i neverd_result_vector;\n";
    Result += "    __asm__ volatile(\"vmaskmov" +
              std::string(IsQword ? "pd" : "ps") + " " + MemoryOperand +
              ", %[mask], %[result]\"\n"
              "        : [result] \"=x\"(" +
              (VectorBytes == 16 ? "neverd_result" : "neverd_result_vector") +
              ")\n"
              "        : [address] \"r\"(neverd_address),\n"
              "          [mask] \"x\"(neverd_mask)\n"
              "        : \"memory\");\n";
    if (VectorBytes == 16) {
      Result += assignPrimary(PrimaryDst, "neverd_result", ExprFn, IsAlive);
    } else {
      // HighIR represents a YMM value as one opaque 256-bit integer.  C has no
      // scalar 256-bit type, so keep its storage as two u128 halves and bridge
      // to the compiler's __m256i register class without aliasing or alignment
      // assumptions.
      Result += "    uint256_t neverd_result;\n"
                "    __builtin_memcpy(&neverd_result, "
                "&neverd_result_vector, sizeof(neverd_result));\n";
      if (PrimaryDst && PrimaryDst->Kind == ExprKind::Var &&
          isAlive(PrimaryDst->Var, IsAlive))
        Result += "    " + ExprFn(*PrimaryDst) + " = neverd_result;\n";
    }
  } else {
    if (VectorBytes == 16) {
      Result += "    unsigned __int128 neverd_data = (unsigned __int128)(" +
                ExprFn(*Call.Operands[2]) + ");\n";
    } else {
      if (Call.Operands[2]->Kind == ExprKind::Const)
        Result += "    uint256_t neverd_data_bits = "
                  "(uint256_t)(unsigned __int128)(" +
                  ExprFn(*Call.Operands[2]) + ");\n";
      else
        Result +=
            "    uint256_t neverd_data_bits = " + ExprFn(*Call.Operands[2]) +
            ";\n";
      Result += "    __m256i neverd_data;\n"
                "    __builtin_memcpy(&neverd_data, &neverd_data_bits, "
                "sizeof(neverd_data));\n";
    }
    Result += "    __asm__ volatile(\"vmaskmov" +
              std::string(IsQword ? "pd" : "ps") + " %[data], %[mask], " +
              MemoryOperand +
              "\"\n"
              "        :\n"
              "        : [address] \"r\"(neverd_address),\n"
              "          [mask] \"x\"(neverd_mask),\n"
              "          [data] \"x\"(neverd_data)\n"
              "        : \"memory\");\n";
  }
  Result += "} while (0);\n";
  return Result;
}

std::string
renderDivPrecondition(Arch TheArch, const HighExpr &Call,
                      std::function<std::string(const HighExpr &)> ExprFn,
                      SameWidthUnsignedFn SameWidthUnsigned) {
  const HighExpr *Dividend =
      Call.Operands.size() > 0 ? Call.Operands[0].get() : nullptr;
  const HighExpr *Divisor =
      Call.Operands.size() > 1 ? Call.Operands[1].get() : nullptr;
  const HighExpr *Kind =
      Call.Operands.size() > 2 ? Call.Operands[2].get() : nullptr;
  const auto IsScalarInteger = [](const HighExpr *Expr) {
    return Expr && Expr->Type && Expr->Type->Kind == NdTypeKind::Int;
  };

  const X86DivPreconditionIntrinsicShape Shape{
      .TargetArch = TheArch,
      .MemoryOrdering = Call.MemoryOrdering,
      .MemoryAddressSpace = Call.MemoryAddressSpace,
      .NumInputs = Call.Operands.size() == 3 ? uint8_t{4} : uint8_t{0},
      .IntrinsicIdIsConst = true,
      .IntrinsicIdSize = 2,
      .OutputSize = 0,
      .DividendIsScalar = IsScalarInteger(Dividend),
      .DividendSize = static_cast<uint16_t>(
          Dividend && Dividend->Type ? Dividend->Type->Size : 0),
      .DivisorIsScalar = IsScalarInteger(Divisor),
      .DivisorSize = static_cast<uint16_t>(
          Divisor && Divisor->Type ? Divisor->Type->Size : 0),
      .KindIsConst = Kind && Kind->Kind == ExprKind::Const,
      .Kind = Kind ? Kind->ConstVal : 0,
      .KindSize =
          static_cast<uint16_t>(Kind && Kind->Type ? Kind->Type->Size : 0),
  };
  if (!intrinsicX86DivPreconditionShapeIsValid(
          Intrinsic::X86RequireDivPrecondition, Shape))
    llvm::report_fatal_error("invalid x86 division precondition intrinsic");

  const uint16_t FullBytes = Dividend->Type->Size;
  const uint16_t HalfBytes = Divisor->Type->Size;
  const unsigned FullBits = FullBytes * 8;
  const unsigned HalfBits = HalfBytes * 8;
  const std::string FullTy = typeToC(NdType::makeInt(FullBytes, false));
  const std::string HalfTy = typeToC(NdType::makeInt(HalfBytes, false));

  auto Operand = [&](const HighExpr *Expr, uint16_t Width,
                     const std::string &Ty) {
    const std::string Text = ExprFn(*Expr);
    if (SameWidthUnsigned && SameWidthUnsigned(*Expr, Width))
      return Text;
    return "(" + Ty + ")(" + Text + ")";
  };
  std::string Result = "do {\n";
  Result += "    " + FullTy +
            " neverd_dividend = " + Operand(Dividend, FullBytes, FullTy) +
            ";\n";
  Result += "    " + HalfTy +
            " neverd_divisor = " + Operand(Divisor, HalfBytes, HalfTy) + ";\n";

  // Decide quotient representability without executing C division: the
  // exceptional divisor-zero and signed-min/-1 cases would otherwise be UB.
  if (Kind->ConstVal == static_cast<uint64_t>(X86DivKind::Unsigned)) {
    Result += "    " + HalfTy + " neverd_dividend_high = (" + HalfTy +
              ")(neverd_dividend >> " + std::to_string(HalfBits) + ");\n";
    Result += "    if (neverd_divisor == 0 || "
              "neverd_dividend_high >= neverd_divisor)\n"
              "        __builtin_trap();\n";
  } else {
    Result += "    " + FullTy +
              " neverd_dividend_magnitude = "
              "(neverd_dividend >> " +
              std::to_string(FullBits - 1) + ") != 0 ? (" + FullTy +
              ")(0 - neverd_dividend) : neverd_dividend;\n";
    Result += "    " + HalfTy +
              " neverd_divisor_magnitude_half = "
              "(neverd_divisor >> " +
              std::to_string(HalfBits - 1) + ") != 0 ? (" + HalfTy +
              ")(0 - neverd_divisor) : neverd_divisor;\n";
    Result += "    " + FullTy + " neverd_divisor_magnitude = (" + FullTy +
              ")neverd_divisor_magnitude_half;\n";
    Result += "    " + FullTy + " neverd_quotient_limit = ((" + FullTy +
              ")1 << " + std::to_string(HalfBits - 1) +
              ") + (((neverd_dividend >> " + std::to_string(FullBits - 1) +
              ") ^ (neverd_divisor >> " + std::to_string(HalfBits - 1) +
              ")) & 1);\n";
    Result += "    if (neverd_divisor == 0 || "
              "neverd_dividend_magnitude >= "
              "neverd_divisor_magnitude * neverd_quotient_limit)\n"
              "        __builtin_trap();\n";
  }
  Result += "} while (0);\n";
  return Result;
}

const char *memoryIntrinsicMnemonic(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Clflush:
    return "clflush";
  case I::Clflushopt:
    return "clflushopt";
  case I::Clwb:
    return "clwb";
  case I::Prefetch:
  case I::PrefetchT0:
    return "prefetcht0";
  case I::PrefetchT1:
    return "prefetcht1";
  case I::PrefetchT2:
    return "prefetcht2";
  case I::PrefetchNta:
    return "prefetchnta";
  case I::PrefetchW:
    return "prefetchw";
  case I::PrefetchWT1:
    return "prefetchwt1";
  case I::Ldmxcsr:
    return "ldmxcsr";
  case I::Stmxcsr:
    return "stmxcsr";
  case I::Lgdt:
  case I::Lidt:
  case I::Sgdt:
  case I::Sidt:
  case I::Invlpg:
  case I::Lldt:
  case I::Ltr:
  case I::Lmsw:
  case I::Sldt:
  case I::Str:
  case I::Smsw:
    return intrinsicAsmMnemonic(Id);
  default:
    return nullptr;
  }
}

bool isStateSnapshotMemoryIntrinsic(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Fxsave:
  case I::Fxrstor:
  case I::Fxsave64Mem:
  case I::Fxrstor64Mem:
  case I::Xsave:
  case I::Xsavec:
  case I::Xsaves:
  case I::Xsaveopt:
  case I::Xrstor:
  case I::Xrstors:
  case I::Xsave64:
  case I::Xsavec64:
  case I::Xsaves64:
  case I::Xsaveopt64:
  case I::Xrstor64:
  case I::Xrstors64:
  case I::X87Fldenv:
  case I::X87Fnstenv:
  case I::X87Frstor:
  case I::X87Fnsave:
    return true;
  default:
    return false;
  }
}

/// The <immintrin.h> spelling of a state save/restore instruction, or null
/// for the x87 environment forms, which have no compiler intrinsic.
const char *stateSnapshotCIntrinsic(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Fxsave:
    return "_fxsave";
  case I::Fxrstor:
    return "_fxrstor";
  case I::Fxsave64Mem:
    return "_fxsave64";
  case I::Fxrstor64Mem:
    return "_fxrstor64";
  case I::Xsave:
    return "_xsave";
  case I::Xsavec:
    return "_xsavec";
  case I::Xsaves:
    return "_xsaves";
  case I::Xsaveopt:
    return "_xsaveopt";
  case I::Xrstor:
    return "_xrstor";
  case I::Xrstors:
    return "_xrstors";
  case I::Xsave64:
    return "_xsave64";
  case I::Xsavec64:
    return "_xsavec64";
  case I::Xsaves64:
    return "_xsaves64";
  case I::Xsaveopt64:
    return "_xsaveopt64";
  case I::Xrstor64:
    return "_xrstor64";
  case I::Xrstors64:
    return "_xrstors64";
  default:
    return nullptr;
  }
}

/// XSAVE-family forms take the EDX:EAX requested-feature bitmap.
bool stateSnapshotTakesMask(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Xsave:
  case I::Xsavec:
  case I::Xsaves:
  case I::Xsaveopt:
  case I::Xrstor:
  case I::Xrstors:
  case I::Xsave64:
  case I::Xsavec64:
  case I::Xsaves64:
  case I::Xsaveopt64:
  case I::Xrstor64:
  case I::Xrstors64:
    return true;
  default:
    return false;
  }
}

/// These instructions move the processor's own x87/SSE/AVX state.  Lifted
/// code keeps that state in C locals, so the emitted statement says which
/// state it touches instead of pretending the locals are saved or restored.
constexpr const char *kHardwareStateNote =
    "/* neverd: hardware x87/SSE/AVX state, not the lifted locals */\n";

std::string
renderStateSnapshot(const HighExpr &Call,
                    std::function<std::string(const HighExpr &)> ExprFn) {
  const bool TakesMask = stateSnapshotTakesMask(Call.IntrinsicId);
  const size_t Required = TakesMask ? 3 : 1;
  if (Call.Operands.size() < Required)
    llvm::report_fatal_error(
        "x86 state save/restore intrinsic is missing its operands");
  for (size_t I = 0; I < Required; ++I)
    if (!Call.Operands[I])
      llvm::report_fatal_error(
          "x86 state save/restore intrinsic is missing its operands");
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  if (!Segment)
    llvm::report_fatal_error(
        "x86 state save/restore intrinsic has an unknown memory address space");
  const std::string Address = ExprFn(*Call.Operands[0]);
  const std::string Mask =
      TakesMask ? "((uint64_t)(uint32_t)(" + ExprFn(*Call.Operands[2]) +
                      ") << 32 | (uint32_t)(" + ExprFn(*Call.Operands[1]) + "))"
                : std::string();

  const char *CName = stateSnapshotCIntrinsic(Call.IntrinsicId);
  if (CName && *Segment == '\0')
    return std::string(kHardwareStateNote) + CName + "((void *)(uintptr_t)(" +
           Address + ")" + (TakesMask ? ", " + Mask : std::string()) + ");\n";

  const char *Mnemonic = intrinsicAsmMnemonic(Call.IntrinsicId);
  if (!Mnemonic)
    llvm::report_fatal_error(
        "x86 state save/restore intrinsic has no assembler mnemonic");
  const std::string MemoryOperand =
      *Segment == '\0' ? "(%[address])"
                       : "%%" + std::string(Segment) + ":(%[address])";
  std::string Result = std::string(kHardwareStateNote) + "do {\n";
  Result += "    uintptr_t neverd_address = (uintptr_t)(" + Address + ");\n";
  if (TakesMask)
    Result += "    uint64_t neverd_mask = " + Mask + ";\n";
  Result += "    __asm__ volatile(\"" + std::string(Mnemonic) + " " +
            MemoryOperand +
            "\"\n        :\n        : [address] \"r\"(neverd_address)";
  if (TakesMask)
    Result += ", \"a\"((uint32_t)neverd_mask), "
              "\"d\"((uint32_t)(neverd_mask >> 32))";
  Result += "\n        : \"memory\");\n} while (0);\n";
  return Result;
}

/// The <immintrin.h> locality hint of a prefetch, or null for any other
/// intrinsic.
const char *prefetchHint(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Prefetch:
  case I::PrefetchT0:
    return "_MM_HINT_T0";
  case I::PrefetchT1:
    return "_MM_HINT_T1";
  case I::PrefetchT2:
    return "_MM_HINT_T2";
  case I::PrefetchNta:
    return "_MM_HINT_NTA";
  default:
    return nullptr;
  }
}

/// A flat prefetch or MXCSR transfer through its <immintrin.h> intrinsic, or
/// empty when the intrinsic has none. The MXCSR image moves as four bytes.
std::string
renderFlatMemoryIntrinsic(const HighExpr &Call,
                          std::function<std::string(const HighExpr &)> &ExprFn,
                          bool MsvcIntrinsics) {
  const std::string Address = "(uintptr_t)(" + ExprFn(*Call.Operands[0]) + ")";
  // <intrin.h> declares these for Windows targets; LGDT/SGDT have no
  // declaration there and keep their assembly form.
  if (MsvcIntrinsics && (Call.IntrinsicId == Intrinsic::Lidt ||
                         Call.IntrinsicId == Intrinsic::Sidt ||
                         Call.IntrinsicId == Intrinsic::Invlpg))
    if (const char *Builtin = intrinsicCName(Call.IntrinsicId))
      return std::string(Builtin) + "((void *)" + Address + ");\n";
  if (const char *Hint = prefetchHint(Call.IntrinsicId))
    return "_mm_prefetch((const char *)" + Address + ", " + Hint + ");\n";
  switch (Call.IntrinsicId) {
  case Intrinsic::PrefetchW:
    return "_m_prefetchw((void *)" + Address + ");\n";
  case Intrinsic::Ldmxcsr:
    return "do {\n    uint32_t neverd_csr;\n"
           "    __builtin_memcpy(&neverd_csr, (const void *)" +
           Address +
           ", sizeof(neverd_csr));\n"
           "    _mm_setcsr(neverd_csr);\n} while (0);\n";
  case Intrinsic::Stmxcsr:
    return "do {\n    uint32_t neverd_csr = _mm_getcsr();\n"
           "    __builtin_memcpy((void *)" +
           Address + ", &neverd_csr, sizeof(neverd_csr));\n} while (0);\n";
  default:
    return {};
  }
}

std::string
renderMemoryIntrinsic(Arch TheArch, const HighExpr &Call,
                      std::function<std::string(const HighExpr &)> ExprFn,
                      bool MsvcIntrinsics) {
  if (isStateSnapshotMemoryIntrinsic(Call.IntrinsicId))
    return renderStateSnapshot(Call, ExprFn);

  // Flat cache-maintenance operands already have portable C intrinsic
  // spellings.  Leave those to the ordinary call renderer; this path is only
  // needed when an FS/GS override must remain attached to the memory operand.
  if (Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      (Call.IntrinsicId == Intrinsic::Clflush ||
       Call.IntrinsicId == Intrinsic::Clflushopt ||
       Call.IntrinsicId == Intrinsic::Clwb))
    return {};
  // A flat prefetch or MXCSR transfer has an intrinsic too; an FS/GS override
  // keeps the assembly form, which carries the segment.
  if (Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      !Call.Operands.empty() && Call.Operands[0])
    if (auto Rendered = renderFlatMemoryIntrinsic(Call, ExprFn, MsvcIntrinsics);
        !Rendered.empty())
      return Rendered;

  // An FS/GS MXCSR transfer moves one DWORD at a segment offset, which the
  // <intrin.h> segment accessors read and write with the override attached.
  if (MsvcIntrinsics &&
      (Call.IntrinsicId == Intrinsic::Ldmxcsr ||
       Call.IntrinsicId == Intrinsic::Stmxcsr) &&
      (Call.MemoryAddressSpace == NdMemoryAddressSpace::X86GS ||
       Call.MemoryAddressSpace == NdMemoryAddressSpace::X86FS) &&
      !Call.Operands.empty() && Call.Operands[0] &&
      Call.Operands[0]->Kind == ExprKind::Const &&
      Call.Operands[0]->ConstVal <= std::numeric_limits<uint32_t>::max()) {
    const bool GS = Call.MemoryAddressSpace == NdMemoryAddressSpace::X86GS;
    const std::string Offset = ExprFn(*Call.Operands[0]);
    if (Call.IntrinsicId == Intrinsic::Ldmxcsr)
      return std::string("_mm_setcsr(") + x86SegmentedReadIntrinsic(GS, 4) +
             "(" + Offset + "));\n";
    return std::string(GS ? "__writegsdword(" : "__writefsdword(") + Offset +
           ", _mm_getcsr());\n";
  }

  const char *Mnemonic = memoryIntrinsicMnemonic(Call.IntrinsicId);
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  if (!Mnemonic || !Segment || Call.Operands.empty() || !Call.Operands[0] ||
      !Call.Operands[0]->Type)
    return {};

  // These six opcodes also have a register form.  Only an address-width first
  // operand denotes memory; a selector/MSW-width operand must keep using the
  // register renderer.
  const bool HasRegisterForm = Call.IntrinsicId == Intrinsic::Lldt ||
                               Call.IntrinsicId == Intrinsic::Ltr ||
                               Call.IntrinsicId == Intrinsic::Lmsw ||
                               Call.IntrinsicId == Intrinsic::Sldt ||
                               Call.IntrinsicId == Intrinsic::Str ||
                               Call.IntrinsicId == Intrinsic::Smsw;
  // computeEA's public Low/Med carrier is uniformly 64-bit, including i386;
  // the architectural addr16/addr32 wrap happens before that final zext.
  if (HasRegisterForm && Call.Operands[0]->Type->Size != 8)
    return {};

  const std::string MemoryOperand =
      *Segment == '\0' ? "(%[address])"
                       : "%%" + std::string(Segment) + ":(%[address])";
  std::string Result = "do {\n";
  Result += "    uintptr_t neverd_address = (uintptr_t)(" +
            ExprFn(*Call.Operands[0]) + ");\n";
  Result += "    __asm__ volatile(\"" + std::string(Mnemonic) + " " +
            MemoryOperand +
            "\"\n"
            "        :\n"
            "        : [address] \"r\"(neverd_address)";
  Result += "\n        : \"memory\");\n"
            "} while (0);\n";
  return Result;
}

std::string renderCpuid(const std::vector<MedVar> &Outs,
                        const std::vector<ExprPtr> &Ops,
                        std::function<std::string(const HighExpr &)> ExprFn,
                        std::function<std::string(const MedVar &)> VarFn,
                        const IsAliveFn &IsAlive) {
  std::string Leaf = Ops.empty() ? "0" : ExprFn(*Ops[0]);
  // A block scope, so a second CPUID in the function does not redeclare
  // cpuInfo.
  std::string Result = "{\n    int cpuInfo[4];\n";
  Result += "    __cpuid(cpuInfo, " + Leaf + ");\n";
  const char *Names[] = {"cpuInfo[0]", "cpuInfo[1]", "cpuInfo[2]",
                         "cpuInfo[3]"};
  for (size_t I = 0; I < Outs.size() && I < 4; ++I)
    if (isAlive(Outs[I], IsAlive))
      Result += "    " + VarFn(Outs[I]) + " = " + Names[I] + ";\n";
  return Result + "}\n";
}

std::string renderXgetbv(const std::vector<MedVar> &Outs,
                         const std::vector<ExprPtr> &Ops,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         const IsAliveFn &IsAlive) {
  std::string ECX = Ops.empty() ? "0" : ExprFn(*Ops[0]);
  if (Outs.size() < 2)
    return "_xgetbv(" + ECX + ");\n";
  bool LoAlive = isAlive(Outs[0], IsAlive);
  bool HiAlive = isAlive(Outs[1], IsAlive);
  if (!LoAlive && !HiAlive)
    return "_xgetbv(" + ECX + ");\n";
  // Block scope: a function may read an extended control register twice.
  std::string Result = "{\n    uint64_t _xcr = _xgetbv(" + ECX + ");\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_xcr;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_xcr >> 32);\n";
  return Result + "}\n";
}

std::string renderRdtsc(const std::vector<MedVar> &Outs, const char *FnName,
                        std::function<std::string(const HighExpr &)> /*ExprFn*/,
                        std::function<std::string(const MedVar &)> VarFn,
                        const IsAliveFn &IsAlive) {
  if (Outs.size() < 2)
    return std::string("__") + FnName + "();\n";
  bool LoAlive = isAlive(Outs[0], IsAlive);
  bool HiAlive = isAlive(Outs[1], IsAlive);
  if (!LoAlive && !HiAlive)
    return std::string("__") + FnName + "();\n";
  // Block scope: a function may read the counter more than once.
  std::string Result =
      std::string("{\n    uint64_t _tsc = __") + FnName + "();\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  return Result + "}\n";
}

std::string renderRdtscp(const std::vector<MedVar> &Outs,
                         const std::vector<ExprPtr> & /*Ops*/,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         const IsAliveFn &IsAlive) {
  if (Outs.size() < 3)
    return renderRdtsc(Outs, "rdtscp", ExprFn, VarFn, IsAlive);
  std::string Result = "{\n    uint32_t _aux;\n";
  Result += "    uint64_t _tsc = __rdtscp(&_aux);\n";
  if (isAlive(Outs[0], IsAlive))
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (isAlive(Outs[1], IsAlive))
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  if (isAlive(Outs[2], IsAlive))
    Result += "    " + VarFn(Outs[2]) + " = _aux;\n";
  return Result + "}\n";
}

const HighExpr *unwrapX86IntegerView(const HighExpr *E) {
  unsigned Depth = 0;
  while (E && Depth++ < limits::kMaxIntegerViewUnwrapDepth &&
         !E->Operands.empty() && E->Operands[0]) {
    if (E->Kind == ExprKind::Cast || E->Kind == ExprKind::BitCast) {
      E = E->Operands[0].get();
      continue;
    }
    if (E->Kind == ExprKind::UnaryOp &&
        (E->Op == NdOp::INT_ZEXT || E->Op == NdOp::INT_SEXT)) {
      E = E->Operands[0].get();
      continue;
    }
    break;
  }
  return E;
}

} // anonymous namespace

bool x86UsesMsvcIntrinsicHeader(Intrinsic Id) {
  return isMovs(Id) || isStos(Id) || Id == Intrinsic::Lidt ||
         Id == Intrinsic::Sidt || Id == Intrinsic::Invlpg ||
         Id == Intrinsic::Ldmxcsr || Id == Intrinsic::Stmxcsr ||
         Id == Intrinsic::Pushf || Id == Intrinsic::Popf;
}

bool x86UsesImplicitRegisterAsm(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::Vmcall:
  case Intrinsic::Vmmcall:
  case Intrinsic::Monitor:
  case Intrinsic::Mwait:
  case Intrinsic::Monitorx:
  case Intrinsic::Mwaitx:
  case Intrinsic::Xsetbv:
    return true;
  default:
    return false;
  }
}

bool x86UsesGnuIntrinsicHeader(Intrinsic Id) {
  switch (Id) {
  case Intrinsic::PrefetchW:
  // __readeflags and __writeeflags (ia32intrin.h), which immintrin.h does
  // not declare.
  case Intrinsic::Pushf:
  case Intrinsic::Popf:
    return true;
  default:
    return false;
  }
}

bool x86MemoryIntrinsicUsesCHeader(Intrinsic Id) {
  return prefetchHint(Id) || Id == Intrinsic::PrefetchW ||
         Id == Intrinsic::Ldmxcsr || Id == Intrinsic::Stmxcsr;
}

bool isX86FastFailCall(const HighExpr &E) {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::IntN ||
      E.Operands.empty() || !E.Operands[0])
    return false;
  const HighExpr *Vec = unwrapX86IntegerView(E.Operands[0].get());
  return Vec && Vec->Kind == ExprKind::Const &&
         isX86Interrupt(Vec->ConstVal, X86Interrupt::FastFail);
}

std::string renderX86InterruptStatement(
    Arch TheArch, const HighExpr &Call, llvm::StringRef ResultVar,
    unsigned ResultSize, std::function<std::string(const HighExpr &)> ExprFn) {
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
      Call.Kind != ExprKind::Call)
    return {};
  const char *Reg = ResultSize == 1                           ? "al"
                    : ResultSize == 2                         ? "ax"
                    : ResultSize == 4 || TheArch == Arch::X86 ? "eax"
                                                              : "rax";
  if (Call.IntrinsicId == Intrinsic::DebugService) {
    const auto Regs = x86DebugServiceRegisters();
    if (TheArch != Arch::X64 || Call.Operands.size() != Regs.size())
      llvm::report_fatal_error(
          "x64 debug service has an invalid operand shape");
    std::vector<std::pair<const char *, std::string>> Inputs;
    for (size_t I = 0; I < Regs.size(); ++I) {
      if (!Call.Operands[I])
        llvm::report_fatal_error("x64 debug service has a missing operand");
      Inputs.emplace_back(Regs[I], ExprFn(*Call.Operands[I]));
    }
    return renderX86InterruptAsm(0x2D, Inputs, ResultVar, Reg);
  }
  if (Call.IntrinsicId == Intrinsic::Syscall && Call.Operands.empty())
    return renderX86InterruptAsm(0, {}, ResultVar, Reg, "syscall");
  if (x86UsesImplicitRegisterAsm(Call.IntrinsicId)) {
    // The lifter's operand order, register by register.
    static const char *const Hypercall[] = {"rcx", "rdx", "r8"};
    static const char *const MonitorRegs[] = {"rax", "rcx", "rdx"};
    static const char *const MwaitRegs[] = {"rax", "rcx"};
    static const char *const MwaitxRegs[] = {"rax", "rcx", "rbx"};
    llvm::ArrayRef<const char *> Regs;
    switch (Call.IntrinsicId) {
    case Intrinsic::Vmcall:
    case Intrinsic::Vmmcall:
      Regs = Hypercall;
      break;
    case Intrinsic::Monitor:
    case Intrinsic::Monitorx:
      Regs = MonitorRegs;
      break;
    case Intrinsic::Mwait:
      Regs = MwaitRegs;
      break;
    case Intrinsic::Mwaitx:
      Regs = MwaitxRegs;
      break;
    default:
      break;
    }
    std::vector<std::pair<const char *, std::string>> Inputs;
    if (Call.IntrinsicId == Intrinsic::Xsetbv) {
      // XCR[ECX] = EDX:EAX, from the selector and the 64-bit value.
      if (Call.Operands.size() != 2 || !Call.Operands[0] || !Call.Operands[1])
        llvm::report_fatal_error("x86 XSETBV has an invalid operand shape");
      const std::string Value = ExprFn(*Call.Operands[1]);
      Inputs = {{"rcx", ExprFn(*Call.Operands[0])},
                {"rax", "(uint32_t)(" + Value + ")"},
                {"rdx", "(uint32_t)((uint64_t)(" + Value + ") >> 32)"}};
    } else {
      if (Call.Operands.size() != Regs.size())
        llvm::report_fatal_error(
            "x86 implicit-register instruction has an invalid operand shape");
      for (size_t I = 0; I < Regs.size(); ++I) {
        if (!Call.Operands[I])
          llvm::report_fatal_error(
              "x86 implicit-register instruction has a missing operand");
        Inputs.emplace_back(Regs[I], ExprFn(*Call.Operands[I]));
      }
    }
    const bool HasResult = Call.IntrinsicId == Intrinsic::Vmcall ||
                           Call.IntrinsicId == Intrinsic::Vmmcall;
    return renderX86InterruptAsm(0, Inputs, HasResult ? ResultVar : "",
                                 HasResult ? Reg : "",
                                 intrinsicAsmMnemonic(Call.IntrinsicId));
  }
  if (Call.IntrinsicId != Intrinsic::IntN || Call.Operands.size() != 1 ||
      !Call.Operands[0] || isX86FastFailCall(Call))
    return {};
  const HighExpr *Vec = unwrapX86IntegerView(Call.Operands[0].get());
  if (!Vec || Vec->Kind != ExprKind::Const)
    return {};
  return renderX86InterruptAsm(Vec->ConstVal & 0xFF, {}, ResultVar, Reg);
}

std::string renderX86MsvcSegmentedLoad(Arch TheArch, unsigned SizeBytes,
                                       llvm::StringRef Addr,
                                       NdMemoryOrdering Ordering,
                                       NdMemoryAddressSpace AddressSpace) {
  if (Ordering != NdMemoryOrdering::None)
    return {};
  if (TheArch != Arch::X86 && TheArch != Arch::X64)
    return {};
  if (AddressSpace != NdMemoryAddressSpace::X86GS &&
      AddressSpace != NdMemoryAddressSpace::X86FS)
    return {};
  const char *Name = x86SegmentedReadIntrinsic(
      AddressSpace == NdMemoryAddressSpace::X86GS, SizeBytes);
  if (!Name)
    return {};
  return std::string(Name) + "(" + Addr.str() + ")";
}

namespace {
/// The C vector type an `_mm` intrinsic takes and returns in a register of
/// \p Bytes, by the element kind its name gives: integers (`_epi8`,
/// `_si128`), singles (`_ps`, `_ss`) or doubles (`_pd`, `_sd`).  Empty for
/// any other name.
std::string x86VectorCType(llvm::StringRef CName, uint16_t Bytes) {
  if (!CName.starts_with("_mm") || (Bytes != 16 && Bytes != 32 && Bytes != 64))
    return {};
  const bool Integers = CName.contains("_epi") || CName.contains("_epu") ||
                        CName.ends_with("_si128") ||
                        CName.ends_with("_si256") || CName.ends_with("_si512");
  const bool Doubles = CName.ends_with("_pd") || CName.ends_with("_sd");
  if (!Integers && !Doubles && !CName.ends_with("_ps") &&
      !CName.ends_with("_ss"))
    return {};
  return std::string(Bytes == 16   ? "__m128"
                     : Bytes == 32 ? "__m256"
                                   : "__m512") +
         (Integers  ? "i"
          : Doubles ? "d"
                    : "");
}

/// An SSE4.2 string compare (PCMPISTRI/M, PCMPESTRI/M), or a status flag one
/// leaves: its strings convert to __m128i and its explicit lengths to int,
/// its control byte stays the constant it is, and a mask comes back as the
/// 16 bytes HighC carries.  A flag's intrinsic ends with the letter of the
/// flag its selector names (X86StringCompare.h).  Empty for any other call.
std::string
renderX86StringCompare(const HighExpr &Call,
                       std::function<std::string(const HighExpr &)> &ExprFn,
                       bool &HasCIntrinsics) {
  using I = Intrinsic;
  const I Id = Call.IntrinsicId;
  const bool Explicit =
      Id == I::Pcmpestri || Id == I::Pcmpestrm || Id == I::PcmpestrFlag;
  const bool Flag = Id == I::PcmpistrFlag || Id == I::PcmpestrFlag;
  const bool Mask = Id == I::Pcmpistrm || Id == I::Pcmpestrm;
  const char *CName = intrinsicCName(Id);
  if ((!Explicit && !Flag && !Mask && Id != I::Pcmpistri) || !CName ||
      Call.Operands.size() != (Explicit ? 5u : 3u) ||
      llvm::any_of(Call.Operands, [](const ExprPtr &Op) { return !Op; }) ||
      Call.Operands.back()->Kind != ExprKind::Const)
    return {};
  const uint64_t Immediate = Call.Operands.back()->ConstVal;
  std::string Name = CName;
  if (Flag) {
    const char *Suffix =
        x86StringCompareFlagSuffix(Immediate >> kX86StringCompareFlagShift);
    if (!Suffix)
      return {};
    Name += Suffix;
  }
  auto String = [&](size_t Index) {
    return "__builtin_bit_cast(__m128i, (unsigned __int128)(" +
           ExprFn(*Call.Operands[Index]) + "))";
  };
  auto Length = [&](size_t Index) {
    return "(int)(" + ExprFn(*Call.Operands[Index]) + ")";
  };
  const std::string Strings = Explicit ? String(0) + ", " + Length(1) + ", " +
                                             String(2) + ", " + Length(3)
                                       : String(0) + ", " + String(1);
  const std::string Text =
      Name + "(" + Strings + ", " +
      std::to_string(Immediate & kX86StringCompareControlMask) + ")";
  HasCIntrinsics = true;
  return Mask ? "__builtin_bit_cast(unsigned __int128, " + Text + ")" : Text;
}

/// An x86 vector intrinsic over registers HighC carries as same-width
/// integers: each register operand converts to the intrinsic's vector type,
/// and its result back.  Empty when \p Call is no such intrinsic.
std::string
renderX86VectorIntrinsic(Arch TheArch, const HighExpr &Call,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         bool &HasCIntrinsics) {
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) || !Call.Type ||
      Call.Type->Kind != NdTypeKind::Int)
    return {};
  const char *CName = intrinsicCName(Call.IntrinsicId);
  if (!CName)
    return {};
  const llvm::StringRef Callee(CName);
  const uint16_t Width = Call.Type->Size;
  const std::string Vector = x86VectorCType(Callee, Width);
  if (Vector.empty())
    return {};
  const std::string Raw = typeToC(NdType::makeInt(Width, false));
  // A VEX/EVEX-widened form shares the SSE intrinsic ID; spell the
  // intrinsic for the register width.
  std::string Spelled = CName;
  if (Width != 16 && Callee.starts_with("_mm_"))
    Spelled =
        (Width == 32 ? "_mm256_" : "_mm512_") + Callee.drop_front(4).str();
  std::string S = "__builtin_bit_cast(" + Raw + ", " + Spelled + "(";
  for (size_t I = 0; I < Call.Operands.size(); ++I) {
    const HighExpr *Op = Call.Operands[I].get();
    if (I > 0)
      S += ", ";
    if (!Op) {
      S += "0";
      continue;
    }
    if (Op->Type && Op->Type->Kind == NdTypeKind::Int &&
        Op->Type->Size == Width)
      S += "__builtin_bit_cast(" + Vector + ", (" + Raw + ")(" + ExprFn(*Op) +
           "))";
    else
      S += ExprFn(*Op);
  }
  HasCIntrinsics = true;
  return S + "))";
}
} // namespace

std::string renderX86TypedIntrinsicCall(
    Arch TheArch, const HighExpr &Call,
    std::function<std::string(const HighExpr &)> ExprFn, bool &HasCIntrinsics,
    bool GnuToolchain,
    std::function<std::string(Intrinsic, unsigned)> FPHelperName) {
  using I = Intrinsic;
  if (Call.IntrinsicId == I::CetRdSsp)
    llvm::report_fatal_error(
        "RDSSP C projection requires a caller-scoped full-GPR expression");
  if (isX86FPStateIntrinsic(Call.IntrinsicId)) {
    const unsigned ResultBytes = Call.Type ? Call.Type->Size : 0;
    if (ResultBytes && Call.Type->Kind != NdTypeKind::Int)
      llvm::report_fatal_error(
          "x86 FP numerical/state result requires raw bits");
    const auto Shape = x86FPStateHighShape(Call, TheArch);
    if (!x86FPStateShapeIsValid(Call.IntrinsicId, Shape))
      llvm::report_fatal_error("invalid x86 FP state C contract");
    const unsigned Bytes = x86FPStateHelperLayout(Call.IntrinsicId, Shape);
    std::string Result =
        (FPHelperName ? FPHelperName(Call.IntrinsicId, Bytes)
                      : x86FPStateCHelper(Call.IntrinsicId, Bytes)) +
        "(";
    const bool Round = isX86FPRoundStateIntrinsic(Call.IntrinsicId);
    const bool Memory = Call.IntrinsicId == I::X86FPRoundMemoryState;
    const unsigned Operands =
        Round || isX86FPConversionStateIntrinsic(Call.IntrinsicId)
            ? 2
            : Call.Operands.size();
    for (unsigned Index = 0; Index < Operands; ++Index) {
      if (Index)
        Result += ", ";
      const auto &Operand =
          *Call.Operands[Round ? (Index == 0 ? (Memory ? 0 : 1) : 3) : Index];
      const auto Text = ExprFn(Operand);
      if (Memory && Index == 0)
        Result += "(void *)(uintptr_t)(" + Text + ")";
      else if (Operand.Type && Operand.Type->Kind == NdTypeKind::Float)
        Result += "__builtin_bit_cast(" +
                  x86FPStateRawCType(Operand.Type->Size) + ", " + Text + ")";
      else
        Result += Text;
    }
    return Result + ")";
  }
  if (Call.IntrinsicId == I::X87Ffree) {
    if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
        Call.Operands.size() != 2 || !Call.Operands[1] ||
        Call.Operands[1]->Kind != ExprKind::Const ||
        Call.Operands[1]->ConstVal >= 8)
      llvm::report_fatal_error("invalid x87 FFREE HighC operand");
    const std::string Register = std::to_string(Call.Operands[1]->ConstVal);
    if (GnuToolchain)
      return "__asm__ volatile(\"ffree %%st(" + Register +
             ")\" ::: " + x87StateCClobbers(X87StateEffect::Stack).str() + ")";
    return "__asm { ffree st(" + Register + ") }";
  }
  // LLDT/LTR/LMSW with a register operand: an `__asm` block cannot take the
  // computed value, so the asm statement loads it into a register.  The
  // memory forms render with their address elsewhere.
  if ((Call.IntrinsicId == I::Lldt || Call.IntrinsicId == I::Ltr ||
       Call.IntrinsicId == I::Lmsw) &&
      Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      Call.Operands.size() == 1 && Call.Operands[0] && Call.Operands[0]->Type &&
      Call.Operands[0]->Type->Size != 8)
    return std::string("__asm__ volatile(\"") +
           intrinsicAsmMnemonic(Call.IntrinsicId) +
           " %w0\" : : \"r\"((uint32_t)(" + ExprFn(*Call.Operands[0]) +
           ")) : \"memory\")";
  if (Call.IntrinsicId == I::ReadSegment ||
      Call.IntrinsicId == I::WriteSegment) {
    // A segment selector by its register's encoding number.
    const size_t Operands = Call.IntrinsicId == I::ReadSegment ? 1 : 2;
    const char *Name = Call.Operands.size() == Operands && Call.Operands[0] &&
                               Call.Operands[0]->Kind == ExprKind::Const
                           ? x86SegmentRegisterName(Call.Operands[0]->ConstVal)
                           : nullptr;
    if (!Name || (Operands == 2 && !Call.Operands[1]))
      llvm::report_fatal_error("x86 segment move has an invalid operand shape");
    if (Call.IntrinsicId == I::ReadSegment)
      return x86SegmentReadText(Name);
    return x86SegmentWriteText(Name, ExprFn(*Call.Operands[1]));
  }
  if (Call.IntrinsicId == I::SegmentLimitValid) {
    // LSL's ZF: whether the selector names a segment whose limit is visible.
    if (Call.Operands.size() != 1 || !Call.Operands[0])
      llvm::report_fatal_error("x86 LSL check has an invalid operand shape");
    return "({ uint32_t neverd_limit; uint8_t neverd_valid; "
           "__asm__(\"lsl %w2, %0\\n\\tsetz %1\" : \"=r\"(neverd_limit), "
           "\"=q\"(neverd_valid) : \"r\"((uint32_t)(" +
           ExprFn(*Call.Operands[0]) + ")) : \"cc\"); neverd_valid; })";
  }
  if (Call.IntrinsicId == I::Rdrand || Call.IntrinsicId == I::Rdseed) {
    // The value, with the CF success byte above it.
    if (!Call.Type ||
        (Call.Type->Size != 4 && Call.Type->Size != 8 && Call.Type->Size != 16))
      llvm::report_fatal_error("x86 RDRAND/RDSEED has an invalid result");
    const unsigned Bytes = Call.Type->Size / 2;
    const std::string ValueTy = typeToC(NdType::makeInt(Bytes, false));
    const std::string PairTy = typeToC(NdType::makeInt(Call.Type->Size, false));
    return "({ " + ValueTy + " neverd_value; uint8_t neverd_ok; " +
           "__asm__ volatile(\"" +
           std::string(Call.IntrinsicId == I::Rdrand ? "rdrand" : "rdseed") +
           " %0\\n\\tsetc %1\" : \"=r\"(neverd_value), \"=q\"(neverd_ok) : : "
           "\"cc\"); (" +
           PairTy + ")neverd_ok << " + std::to_string(Bytes * 8) +
           " | neverd_value; })";
  }
  if (isX86FastFailCall(Call)) {
    if (TheArch != Arch::X86 && TheArch != Arch::X64)
      return {};
    std::string Code = "0";
    if (Call.Operands.size() > 1 && Call.Operands[1])
      Code = ExprFn(*Call.Operands[1]);
    HasCIntrinsics = true;
    return "__fastfail(" + Code + ")";
  }
  if (Call.IntrinsicId == I::IntN && !Call.Operands.empty() &&
      Call.Operands[0]) {
    const HighExpr *Vec = unwrapX86IntegerView(Call.Operands[0].get());
    if (Vec && Vec->Kind == ExprKind::Const && (Vec->ConstVal & 0xFF) == 0x2C) {
      HasCIntrinsics = true;
      return "__int2c()";
    }
  }
  // Shadow-stack writes take C operands through the MSVC intrinsics; an
  // inline-asm operand cannot be a C expression.
  if ((Call.IntrinsicId == I::CetWrss || Call.IntrinsicId == I::CetWruss) &&
      Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      Call.Operands.size() == 2 && Call.Operands[0] && Call.Operands[1] &&
      Call.Operands[1]->Type &&
      (Call.Operands[1]->Type->Size == 4 ||
       Call.Operands[1]->Type->Size == 8)) {
    const bool Is64 = Call.Operands[1]->Type->Size == 8;
    HasCIntrinsics = true;
    return std::string(Call.IntrinsicId == I::CetWrss ? "_wrss" : "_wruss") +
           (Is64 ? "q((unsigned __int64)(" : "d((unsigned int)(") +
           ExprFn(*Call.Operands[1]) + "), (void *)(uintptr_t)(" +
           ExprFn(*Call.Operands[0]) + "))";
  }
  if (TheArch == Arch::X86 || TheArch == Arch::X64)
    if (std::string Compare =
            renderX86StringCompare(Call, ExprFn, HasCIntrinsics);
        !Compare.empty())
      return Compare;
  const bool IsGfni = Call.IntrinsicId == I::Gf2p8MulB ||
                      Call.IntrinsicId == I::Gf2p8AffineQb ||
                      Call.IntrinsicId == I::Gf2p8AffineInvQb;
  const bool IsVdbpsadbw = Call.IntrinsicId == I::Vdbpsadbw;
  if (!IsGfni && !IsVdbpsadbw)
    return renderX86VectorIntrinsic(TheArch, Call, ExprFn, HasCIntrinsics);
  if (TheArch != Arch::X86 && TheArch != Arch::X64)
    llvm::report_fatal_error(
        "typed x86 vector intrinsic requires an x86 target");

  const size_t RequiredOperands = Call.IntrinsicId == I::Gf2p8MulB ? 2 : 3;
  const bool HasValidResult =
      Call.Kind == ExprKind::Call && Call.Type &&
      Call.Type->Kind == NdTypeKind::Int &&
      (Call.Type->Size == 16 || Call.Type->Size == 32 || Call.Type->Size == 64);
  if (!HasValidResult || Call.Operands.size() != RequiredOperands ||
      !Call.IntrinsicOutputs.empty() ||
      Call.MemoryOrdering != NdMemoryOrdering::None ||
      Call.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error("invalid typed x86 vector intrinsic shape");

  const uint16_t Width = Call.Type->Size;
  for (size_t Index = 0; Index < 2; ++Index) {
    const ExprPtr &Operand = Call.Operands[Index];
    if (!Operand || !Operand->Type || Operand->Type->Kind != NdTypeKind::Int ||
        Operand->Type->Size != Width)
      llvm::report_fatal_error("invalid typed x86 vector intrinsic shape");
  }
  if (RequiredOperands == 3) {
    const ExprPtr &Immediate = Call.Operands[2];
    if (!Immediate || Immediate->Kind != ExprKind::Const || !Immediate->Type ||
        Immediate->Type->Kind != NdTypeKind::Int ||
        Immediate->Type->Size != 1 || Immediate->ConstVal > 0xff)
      llvm::report_fatal_error("invalid typed x86 vector intrinsic shape");
  }

  const char *RawType = Width == 16   ? "unsigned __int128"
                        : Width == 32 ? "uint256_t"
                                      : "uint512_t";
  const char *VectorType = Width == 16   ? "__m128i"
                           : Width == 32 ? "__m256i"
                                         : "__m512i";
  const char *Prefix = Width == 16 ? "_mm" : Width == 32 ? "_mm256" : "_mm512";
  const char *Suffix = nullptr;
  switch (Call.IntrinsicId) {
  case I::Gf2p8MulB:
    Suffix = "_gf2p8mul_epi8";
    break;
  case I::Gf2p8AffineQb:
    Suffix = "_gf2p8affine_epi64_epi8";
    break;
  case I::Gf2p8AffineInvQb:
    Suffix = "_gf2p8affineinv_epi64_epi8";
    break;
  case I::Vdbpsadbw:
    Suffix = "_dbsad_epu8";
    break;
  default:
    llvm_unreachable("typed intrinsic was validated above");
  }

  auto VectorOperand = [&](size_t Index) {
    return std::string("__builtin_bit_cast(") + VectorType + ", (" + RawType +
           ")(" + ExprFn(*Call.Operands[Index]) + "))";
  };
  std::string Result = std::string("__builtin_bit_cast(") + RawType + ", " +
                       Prefix + Suffix + "(" + VectorOperand(0) + ", " +
                       VectorOperand(1);
  if (RequiredOperands == 3)
    Result += ", " + ExprFn(*Call.Operands[2]);
  Result += "))";
  HasCIntrinsics = true;
  return Result;
}

namespace {
/// VERR/VERW as inline assembly, moving ZF into \p PrimaryDst when it is
/// live.  A memory selector keeps its FS/GS override.
std::string
renderSelectorAccessCheck(const HighExpr &Call, const HighExpr *PrimaryDst,
                          std::function<std::string(const HighExpr &)> &ExprFn,
                          std::function<std::string(const MedVar &)> &VarFn,
                          const IsAliveFn &IsAlive) {
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  if (Call.Operands.size() != 1 || !Call.Operands[0] ||
      !Call.Operands[0]->Type || !Segment)
    llvm::report_fatal_error("x86 VERR/VERW has an invalid operand shape");
  std::string Zf;
  if (PrimaryDst &&
      (PrimaryDst->Kind == ExprKind::Var ||
       PrimaryDst->Kind == ExprKind::Phi) &&
      isAlive(PrimaryDst->Var, IsAlive))
    Zf = VarFn(PrimaryDst->Var);
  // Only an address-width operand is a memory selector.
  const bool Memory = Call.Operands[0]->Type->Size == 8;
  std::string Result = "do {\n";
  std::string Operand;
  std::string Input;
  if (Memory) {
    Result += "    uintptr_t neverd_address = (uintptr_t)(" +
              ExprFn(*Call.Operands[0]) + ");\n";
    Operand = *Segment == '\0' ? "(%[address])"
                               : "%%" + std::string(Segment) + ":(%[address])";
    Input = "[address] \"r\"(neverd_address)";
  } else {
    Result += "    uint16_t neverd_selector = (uint16_t)(" +
              ExprFn(*Call.Operands[0]) + ");\n";
    Operand = "%[selector]";
    Input = "[selector] \"r\"(neverd_selector)";
  }
  if (!Zf.empty())
    Result += "    uint8_t neverd_zf;\n";
  Result +=
      "    __asm__ volatile(\"" +
      std::string(Call.IntrinsicId == Intrinsic::Verr ? "verr " : "verw ") +
      Operand + (Zf.empty() ? "" : "\\n\\tsetz %[zf]") + "\"\n";
  Result += "        : " +
            (Zf.empty() ? std::string() : "[zf] \"=q\"(neverd_zf)") + "\n";
  Result += "        : " + Input + "\n        : \"cc\", \"memory\");\n";
  if (!Zf.empty())
    Result += "    " + Zf + " = neverd_zf;\n";
  return Result + "} while (0);\n";
}
} // namespace

std::string renderX86SegmentedIntrinsicStatement(
    Arch TheArch, const HighExpr &Call, const HighExpr *PrimaryDst,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive,
    SameWidthUnsignedFn SameWidthUnsigned, bool MsvcIntrinsics) {
  if (Call.Kind != ExprKind::Call ||
      (TheArch != Arch::X86 && TheArch != Arch::X64))
    return {};
  if (Call.IntrinsicId == Intrinsic::Verr ||
      Call.IntrinsicId == Intrinsic::Verw)
    return renderSelectorAccessCheck(Call, PrimaryDst, ExprFn, VarFn, IsAlive);
  if (Call.IntrinsicId == Intrinsic::X86RequireDivPrecondition)
    return renderDivPrecondition(TheArch, Call, std::move(ExprFn),
                                 std::move(SameWidthUnsigned));
  if (auto Rendered =
          renderMemoryIntrinsic(TheArch, Call, ExprFn, MsvcIntrinsics);
      !Rendered.empty())
    return Rendered;
  if (isMovs(Call.IntrinsicId) || isStos(Call.IntrinsicId) ||
      isLods(Call.IntrinsicId) || isCmps(Call.IntrinsicId) ||
      isScas(Call.IntrinsicId) || isOuts(Call.IntrinsicId) ||
      isIns(Call.IntrinsicId))
    return renderSegmentedString(TheArch, Call, PrimaryDst, std::move(ExprFn),
                                 std::move(VarFn), IsAlive, MsvcIntrinsics);
  if (Call.IntrinsicId == Intrinsic::MaskedStoreB)
    return renderMaskedByteStore(Call, std::move(ExprFn));
  return renderSegmentedMaskedMemory(Call, PrimaryDst, std::move(ExprFn),
                                     IsAlive);
}

std::string
renderX86MultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive) {
  using I = Intrinsic;
  switch (IID) {
  case I::Cpuid:
    return renderCpuid(Outputs, Operands, ExprFn, VarFn, IsAlive);
  case I::Xgetbv:
    return renderXgetbv(Outputs, Operands, ExprFn, VarFn, IsAlive);
  case I::Rdtsc:
    return renderRdtsc(Outputs, "rdtsc", ExprFn, VarFn, IsAlive);
  case I::Rdtscp:
    return renderRdtscp(Outputs, Operands, ExprFn, VarFn, IsAlive);
  default:
    return {};
  }
}

const char *x86HighCIntrinsicFatalReason(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::X86MsrAccess:
    return "x86 MSR access requires an authenticated architectural execution "
           "environment";
  case I::X86RequireDivPrecondition:
    return "x86 division precondition requires an architectural fault "
           "environment";
  case I::X86FourFMA:
    return "x86 four-source FMA requires explicit architectural source and "
           "floating-point state";
  default:
    return nullptr;
  }
}

std::string renderX86IntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   bool &HasCIntrinsics, bool GnuToolchain) {
  if (const char *Reason = x86HighCIntrinsicFatalReason(Id))
    llvm::report_fatal_error(Reason);
  // An x87 value instruction runs in its helper, on its operands' bits.
  if (const auto Helper = x87ValueHelper(Id)) {
    if (Ops.size() != x87ValueHelperOperands(*Helper))
      llvm::report_fatal_error("x87 value intrinsic has an invalid operand "
                               "count");
    std::string Call = std::string(x87CHelperName(*Helper)) + "(";
    for (size_t I = 0; I < Ops.size(); ++I)
      Call += (I ? ", " : "") + Ops[I];
    return Call + ")";
  }
  // An x87 state instruction runs where the compiler keeps no value of its
  // own in the register stack it changes: the clobbers say which.
  if (GnuToolchain && Ops.empty())
    if (const auto Effect = x87StateEffectOfIntrinsic(Id))
      return std::string("__asm__ volatile(\"") + intrinsicAsmMnemonic(Id) +
             "\" ::: " + x87StateCClobbers(*Effect).str() + ")";
  if (GnuToolchain && Ops.empty())
    switch (Id) {
#define G(ID, MNEMONIC)                                                        \
  case ID:                                                                     \
    return "__asm__ volatile(\"" MNEMONIC "\" ::: \"memory\")";
#include "neverd/ir/intrinsics/intrinsics_x86_gnu_asm.inc"
#undef G
    default:
      break;
    }

  using I = Intrinsic;
  switch (Id) {
  case I::X86Invalidate: {
    // Operands: descriptor address, invalidation kind, type register.  The
    // source spells INVPCID as the MSVC intrinsic _invpcid(type, descriptor).
    if (Ops.size() != 3 || Ops[1] != std::to_string(static_cast<unsigned>(
                                         X86InvalidateKind::Invpcid)))
      llvm::report_fatal_error("x86 invalidation has an unknown kind");
    HasCIntrinsics = true;
    return "_invpcid((unsigned int)(" + Ops[2] + "), (void *)(uintptr_t)(" +
           Ops[0] + "))";
  }
  case I::Cpuid: {
    std::string Leaf = Ops.empty() ? "0" : Ops[0];
    HasCIntrinsics = true;
    return "{ int cpuInfo[4]; __cpuid(cpuInfo, " + Leaf + "); }";
  }
  case I::Rdtscp: {
    HasCIntrinsics = true;
    return "({ uint32_t _aux; uint64_t _tsc = __rdtscp(&_aux); "
           "_tsc; })";
  }
  default:
    return {};
  }
}

const char *hiloCollapseExpr(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
  case I::Rdtsc:
    return "__rdtsc()";
  case I::Rdtscp:
    return "__rdtscp()";
  default:
    return nullptr;
  }
}

std::string renderX86AsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops) {
  std::string AsmStmt = Mnemonic;
  for (size_t I = 0; I < Ops.size(); ++I) {
    AsmStmt += (I == 0 ? " " : ", ");
    AsmStmt += Ops[I];
  }
  return "__asm { " + AsmStmt + " }";
}

} // namespace neverd
