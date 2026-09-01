//===- HighCIntrinsicRenderX86.cpp - x86 intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific HighIR intrinsic rendering: multi-output CPUID/RDTSC/XGETBV
/// emission, single-output x86 intrinsic calls, and hi/lo collapse patterns.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include "llvm/Support/ErrorHandling.h"

#include <string>
#include <utility>

namespace neverd {

llvm::SmallVector<const char *, 3> getX86IntrinsicHeaders() {
  return {"immintrin.h"};
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
                      const IsAliveFn &IsAlive) {
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
  if ((IsCmps || IsScas) && Call.IntrinsicOutputs.empty())
    llvm::report_fatal_error(
        "x86 REP string intrinsic is missing its flags result");
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

  Result += "    if (" + ExprFn(*Call.Operands[3]) + ") {\n";
  if (IsCmps || IsScas) {
    Result += "        if (" + ExprFn(*Call.Operands[4]) + ") {\n";
    EmitAsm("repnz", true);
    Result += "        } else {\n";
    EmitAsm("repz", true);
    Result += "        }\n";
  } else {
    EmitAsm("rep", true);
  }
  Result += "    } else {\n";
  if (IsCmps || IsScas) {
    Result += "        if (" + ExprFn(*Call.Operands[4]) + ") {\n";
    EmitAsm("repnz", false);
    Result += "        } else {\n";
    EmitAsm("repz", false);
    Result += "        }\n";
  } else {
    EmitAsm("rep", false);
  }
  Result += "    }\n";

  if (IsLods)
    Result += assignPrimary(PrimaryDst, "neverd_ax", ExprFn, IsAlive);
  if (IsCmps || IsScas) {
    Result += assignPrimary(PrimaryDst, "neverd_cx", ExprFn, IsAlive);
    const MedVar &Flags = Call.IntrinsicOutputs.front();
    if (!IsAlive || IsAlive(Flags))
      Result += "    " + VarFn(Flags) + " = (uint16_t)" +
                (IsScas ? "neverd_ax" : "neverd_flags") + ";\n";
  }
  Result += "} while (0);\n";
  return Result;
}

std::string
renderMaskedByteStore(const HighExpr &Call,
                      std::function<std::string(const HighExpr &)> ExprFn) {
  if (Call.IntrinsicId != Intrinsic::MaskedStoreB ||
      Call.Operands.size() < 3 || !Call.Operands[0] || !Call.Operands[1] ||
      !Call.Operands[2] || !Call.Operands[1]->Type ||
      !Call.Operands[2]->Type)
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
    PointerType = "uint8_t __attribute__((address_space(257))) *";
    break;
  case NdMemoryAddressSpace::X86GS:
    PointerType = "uint8_t __attribute__((address_space(256))) *";
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
            std::to_string(VectorBytes) + "; ++neverd_i)\n"
            "        if (((neverd_mask >> (neverd_i * 8)) & 0x80u) != 0)\n"
            "            *(" +
            PointerType + ")(uintptr_t)(neverd_address + neverd_i) = "
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
  if (!Call.Operands[1]->Type ||
      (Call.Operands[1]->Type->Size != 16 &&
       Call.Operands[1]->Type->Size != 32))
    return {};
  const unsigned VectorBytes = Call.Operands[1]->Type->Size;
  if (IsLoad && (!PrimaryDst || PrimaryDst->Kind != ExprKind::Var ||
                 !PrimaryDst->Type ||
                 PrimaryDst->Type->Size != VectorBytes))
    return {};
  if (IsStore && (!Call.Operands[2]->Type ||
                  Call.Operands[2]->Type->Size != VectorBytes))
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
      Result += "    uint256_t neverd_mask_bits = " +
                ExprFn(*Call.Operands[1]) + ";\n";
    Result += "    __m256i neverd_mask;\n"
              "    __builtin_memcpy(&neverd_mask, &neverd_mask_bits, "
              "sizeof(neverd_mask));\n";
  }
  if (IsLoad) {
    Result += VectorBytes == 16
                  ? "    unsigned __int128 neverd_result;\n"
                  : "    __m256i neverd_result_vector;\n";
    Result += "    __asm__ volatile(\"vmaskmov" +
              std::string(IsQword ? "pd" : "ps") + " " + MemoryOperand +
              ", %[mask], %[result]\"\n"
              "        : [result] \"=x\"(" +
              (VectorBytes == 16 ? "neverd_result" :
                                   "neverd_result_vector") +
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
        Result += "    uint256_t neverd_data_bits = " +
                  ExprFn(*Call.Operands[2]) + ";\n";
      Result += "    __m256i neverd_data;\n"
                "    __builtin_memcpy(&neverd_data, &neverd_data_bits, "
                "sizeof(neverd_data));\n";
    }
    Result += "    __asm__ volatile(\"vmaskmov" +
              std::string(IsQword ? "pd" : "ps") +
              " %[data], %[mask], " + MemoryOperand + "\"\n"
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
                      std::function<std::string(const HighExpr &)> ExprFn) {
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
      .KindSize = static_cast<uint16_t>(
          Kind && Kind->Type ? Kind->Type->Size : 0),
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

  std::string Result = "do {\n";
  Result += "    " + FullTy + " neverd_dividend = (" + FullTy + ")(" +
            ExprFn(*Dividend) + ");\n";
  Result += "    " + HalfTy + " neverd_divisor = (" + HalfTy + ")(" +
            ExprFn(*Divisor) + ");\n";

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
              ") + (((neverd_dividend >> " +
              std::to_string(FullBits - 1) +
              ") ^ (neverd_divisor >> " +
              std::to_string(HalfBits - 1) + ")) & 1);\n";
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

std::string
renderMemoryIntrinsic(Arch TheArch, const HighExpr &Call,
                      std::function<std::string(const HighExpr &)> ExprFn) {
  if (isStateSnapshotMemoryIntrinsic(Call.IntrinsicId))
    llvm::report_fatal_error(
        "x86 state save/restore requires an explicit architectural state "
        "layout; host inline asm is not a sound High-C lowering");

  // Flat cache-maintenance operands already have portable C intrinsic
  // spellings.  Leave those to the ordinary call renderer; this path is only
  // needed when an FS/GS override must remain attached to the memory operand.
  if (Call.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
      (Call.IntrinsicId == Intrinsic::Clflush ||
       Call.IntrinsicId == Intrinsic::Clflushopt ||
       Call.IntrinsicId == Intrinsic::Clwb))
    return {};

  const char *Mnemonic = memoryIntrinsicMnemonic(Call.IntrinsicId);
  const char *Segment = segmentPrefix(Call.MemoryAddressSpace);
  if (!Mnemonic || !Segment || Call.Operands.empty() || !Call.Operands[0] ||
      !Call.Operands[0]->Type)
    return {};

  // These six opcodes also have a register form.  Only an address-width first
  // operand denotes memory; a selector/MSW-width operand must keep using the
  // register renderer.
  const bool HasRegisterForm =
      Call.IntrinsicId == Intrinsic::Lldt ||
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
            MemoryOperand + "\"\n"
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
  std::string Result = "int cpuInfo[4];\n";
  Result += "    __cpuid(cpuInfo, " + Leaf + ");\n";
  const char *Names[] = {"cpuInfo[0]", "cpuInfo[1]", "cpuInfo[2]",
                         "cpuInfo[3]"};
  for (size_t I = 0; I < Outs.size() && I < 4; ++I)
    if (isAlive(Outs[I], IsAlive))
      Result += "    " + VarFn(Outs[I]) + " = " + Names[I] + ";\n";
  return Result;
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
  std::string Result = "uint64_t _xcr = _xgetbv(" + ECX + ");\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_xcr;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_xcr >> 32);\n";
  return Result;
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
  std::string Result = std::string("uint64_t _tsc = __") + FnName + "();\n";
  if (LoAlive)
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (HiAlive)
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  return Result;
}

std::string renderRdtscp(const std::vector<MedVar> &Outs,
                         const std::vector<ExprPtr> & /*Ops*/,
                         std::function<std::string(const HighExpr &)> ExprFn,
                         std::function<std::string(const MedVar &)> VarFn,
                         const IsAliveFn &IsAlive) {
  if (Outs.size() < 3)
    return renderRdtsc(Outs, "rdtscp", ExprFn, VarFn, IsAlive);
  std::string Result = "uint32_t _aux;\n";
  Result += "    uint64_t _tsc = __rdtscp(&_aux);\n";
  if (isAlive(Outs[0], IsAlive))
    Result += "    " + VarFn(Outs[0]) + " = (uint32_t)_tsc;\n";
  if (isAlive(Outs[1], IsAlive))
    Result += "    " + VarFn(Outs[1]) + " = (uint32_t)(_tsc >> 32);\n";
  if (isAlive(Outs[2], IsAlive))
    Result += "    " + VarFn(Outs[2]) + " = _aux;\n";
  return Result;
}

} // anonymous namespace

std::string
renderX86TypedIntrinsicCall(Arch TheArch, const HighExpr &Call,
                            std::function<std::string(const HighExpr &)> ExprFn,
                            bool &HasCIntrinsics) {
  using I = Intrinsic;
  const bool IsGfni = Call.IntrinsicId == I::Gf2p8MulB ||
                      Call.IntrinsicId == I::Gf2p8AffineQb ||
                      Call.IntrinsicId == I::Gf2p8AffineInvQb;
  const bool IsVdbpsadbw = Call.IntrinsicId == I::Vdbpsadbw;
  if (!IsGfni && !IsVdbpsadbw)
    return {};
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

std::string renderX86SegmentedIntrinsicStatement(
    Arch TheArch, const HighExpr &Call, const HighExpr *PrimaryDst,
    std::function<std::string(const HighExpr &)> ExprFn,
    std::function<std::string(const MedVar &)> VarFn, IsAliveFn IsAlive) {
  if (Call.Kind != ExprKind::Call ||
      (TheArch != Arch::X86 && TheArch != Arch::X64))
    return {};
  if (Call.IntrinsicId == Intrinsic::X86RequireDivPrecondition)
    return renderDivPrecondition(TheArch, Call, std::move(ExprFn));
  if (auto Rendered =
          renderMemoryIntrinsic(TheArch, Call, ExprFn);
      !Rendered.empty())
    return Rendered;
  if (isMovs(Call.IntrinsicId) || isStos(Call.IntrinsicId) ||
      isLods(Call.IntrinsicId) || isCmps(Call.IntrinsicId) ||
      isScas(Call.IntrinsicId) || isOuts(Call.IntrinsicId) ||
      isIns(Call.IntrinsicId))
    return renderSegmentedString(TheArch, Call, PrimaryDst, std::move(ExprFn),
                                 std::move(VarFn), IsAlive);
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
  case I::X86Invalidate:
    return "x86 address-translation invalidation requires an authenticated "
           "architectural execution environment";
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
                                   bool &HasCIntrinsics) {
  if (const char *Reason = x86HighCIntrinsicFatalReason(Id))
    llvm::report_fatal_error(Reason);

  using I = Intrinsic;
  switch (Id) {
  case I::Cpuid: {
    std::string Leaf = Ops.empty() ? "0" : Ops[0];
    HasCIntrinsics = true;
    return "{{ int cpuInfo[4]; __cpuid(cpuInfo, " + Leaf + "); }}";
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
  return "__asm {{ " + AsmStmt + " }}";
}

} // namespace neverd
