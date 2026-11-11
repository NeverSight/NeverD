//===- MIPSScalar.h - Portable MIPS scalar C helpers ---------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Emit scalar helpers with explicit target byte order and bounded reads.
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_BACKEND_C_RENDER_MIPS_MIPSSCALAR_H
#define NEVERD_LIB_BACKEND_C_RENDER_MIPS_MIPSSCALAR_H

#include "neverd/backend/MIPS/MIPSBarrierSource.h"
#include "neverd/backend/MIPS/MIPSInstructionModeSource.h"
#include "neverd/backend/MIPS/MIPSMSASource.h"
#include "neverd/backend/MIPS/MIPSSourceCompiler.h"
#include "neverd/ir/high/HighIR.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalar.h"

#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace neverd {

#include "neverd/backend/MIPS/MIPSMemorySource.inc"

inline bool mipsScalarHighShapeIsValid(const HighExpr &E, Arch Target) {
  if (E.Kind != ExprKind::Call || E.Operands.size() > 3)
    return false;
  for (const auto &Operand : E.Operands)
    if (!Operand || !Operand->Type ||
        (Operand->Type->Kind != NdTypeKind::Int &&
         Operand->Type->Kind != NdTypeKind::Ptr))
      return false;
  return mips::scalarShapeIsValid(
      E.IntrinsicId,
      {Target, uint8_t(E.Operands.size() + 1), true, true,
       E.Type ? uint16_t(E.Type->Size) : uint16_t{0},
       E.Operands.size() > 0 ? uint16_t(E.Operands[0]->Type->Size)
                             : uint16_t{0},
       E.Operands.size() > 1 ? uint16_t(E.Operands[1]->Type->Size)
                             : uint16_t{0},
       E.Operands.size() > 2 ? uint16_t(E.Operands[2]->Type->Size)
                             : uint16_t{0},
       E.MemoryOrdering == NdMemoryOrdering::None &&
           E.MemoryAddressSpace == NdMemoryAddressSpace::Default,
       !E.Operands.empty() && E.Operands[0]->Kind == ExprKind::Const
           ? std::optional<uint64_t>(E.Operands[0]->ConstVal)
           : std::nullopt});
}

inline std::pair<Intrinsic, unsigned> mipsScalarHelperKey(const HighExpr &E) {
  return {E.IntrinsicId, mips::isMSAControl(E.IntrinsicId)
                             ? unsigned(E.Operands[0]->ConstVal)
                             : 0};
}

template <typename Stream>
void writeMIPSScalarHelpers(
    Stream &OS, Arch Target,
    const std::map<std::pair<Intrinsic, unsigned>, std::string> &Helpers,
    const BinaryImage *Image) {
  const bool Wide = isMIPS64(Target);
  const bool Little = archLittleEndian(Target);
  const char *Reg = Wide ? "uint64_t" : "uint32_t";
  for (const auto &[Key, Name] : Helpers) {
    const auto [Id, Selector] = Key;
    if (mips::isMSAFloat(Id) || mips::isMSAControl(Id)) {
      if (!Image || !Image->MIPS || !Image->MIPS->HasMSA ||
          Image->MIPS->ISARevision != 5)
        throw std::runtime_error(
            "MSA native source requires an enabled MIPS32r5 MSA image");
      const auto Asm = mips::msaAssembly(Id, Selector, Little);
      OS << "static __attribute__((noinline)) ";
      if (Asm.Read || Asm.Write) {
        OS << (Asm.Read ? "uint32_t " : "void ") << Name
           << (Asm.Read ? "(void) { uint32_t v; " : "(uint32_t v) { ")
           << (Asm.Read ? Asm.cStatement({}, "v")
                        : Asm.cStatement({std::string("v")}))
           << (Asm.Read ? " return v; }\n" : " }\n");
        continue;
      }
      OS << "unsigned _BitInt(128) " << Name << "(";
      std::vector<std::string> Pointers{"&result"};
      for (unsigned I = 1; I < Asm.Pointers; ++I) {
        if (I != 1)
          OS << ", ";
        OS << "unsigned _BitInt(128) a" << I;
        Pointers.push_back("&a" + std::to_string(I));
      }
      OS << ") { unsigned _BitInt(128) result; " << Asm.cStatement(Pointers)
         << " return result; }\n";
      OS << "\n";
      continue;
    }
    if (mips::isStatefulScalar(Id))
      throw std::runtime_error(mips::kStatefulSourceUnsupported);
    if (mips::isBarrier(Id)) {
      for (unsigned Stype = 0; Stype != (Id == Intrinsic::MIPSSync ? 32u : 1u);
           ++Stype) {
        OS << "static " << mips::BarrierNoInlineC
           << mips::instructionModeCAttribute(InstructionMode::MIPSClassic)
           << "void " << Name;
        if (Id == Intrinsic::MIPSSync)
          OS << "_" << Stype;
        OS << "(void) { __asm__ volatile(\"" << mips::barrierAssembly(Id, Stype)
           << "\" ::: \"memory\"); }\n";
      }
      OS << "\n";
      continue;
    }
    if (Id == Intrinsic::MIPSReadDSPControl) {
      OS << "static inline "
         << mips::sourceCompilerCAttribute(Target, Image, true) << "uint32_t "
         << Name << "(void) {\n    uint32_t value;\n    __asm__ volatile("
         << mips::DSPReadCAsm
         << " : \"=r\"(value) :: \"memory\");\n    return value;\n}\n\n";
      continue;
    }
    if (Id == Intrinsic::MIPSWriteDSPControl) {
      OS << "static inline "
         << mips::sourceCompilerCAttribute(Target, Image, true) << "void "
         << Name << "(uint32_t value) {\n    __asm__ volatile("
         << mips::DSPWriteCAsm << " :: \"r\"(value) : \"memory\");\n}\n\n";
      continue;
    }
    if (Id == Intrinsic::MIPSUnpredictable)
      throw std::runtime_error(mips::kUnpredictableResult);
    if (Id == Intrinsic::MIPSAccumulatorAfterWrite)
      throw std::runtime_error(mips::kAccumulatorHistoryUnknown);
    if (Id == Intrinsic::MIPSLwrUpper)
      throw std::runtime_error(mips::kPartialLwrRequiresWord);
    if (Id == Intrinsic::MIPSExtractResult) {
      OS << "static inline uint32_t " << Name
         << "(uint32_t failed, uint32_t value) {\n"
            "    if (failed) __builtin_trap();\n    return value;\n}\n\n";
      continue;
    }
    if (Id == Intrinsic::MIPSRequireWord) {
      OS << "static inline void " << Name
         << "(uint64_t value) {\n"
            "    uint64_t word = value & 0xffffffffULL;\n"
            "    if (word & 0x80000000ULL) word |= 0xffffffff00000000ULL;\n"
            "    if (value != word) __builtin_trap();\n}\n\n";
      continue;
    }
    if (mips::isWordDivision(Id)) {
      OS << "/* Qualified source domain: every retained division reached here\n"
            "   requires a nonzero divisor; this is not a MIPS exception. */\n"
         << "static inline "
         << mips::instructionModeCAttribute(InstructionMode::MIPSClassic)
         << "uint64_t " << Name
         << "(uint32_t a, uint32_t b) {\n"
            "    if (b == 0) __builtin_trap();\n";
      if (Id == Intrinsic::MIPSDivWord)
        OS << "    uint32_t x = (a >> 31) ? 0u - a : a;\n"
              "    uint32_t y = (b >> 31) ? 0u - b : b;\n"
              "    uint32_t q = x / y, r = x % y;\n"
              "    if ((a ^ b) >> 31) q = 0u - q;\n"
              "    if (a >> 31) r = 0u - r;\n";
      else
        OS << "    uint32_t q = a / b, r = a % b;\n";
      OS << "    return ((uint64_t)r << 32) | q;\n}\n\n";
      continue;
    }
    const auto Load = *mips::mergeMemory(Id);
    const bool Store = mips::mergeStore(Id).has_value();
    const bool Forward = Load.Left != Little;
    const char *Value = Load.Bytes == 8 ? "uint64_t" : "uint32_t";
    OS << "static inline "
       << mips::instructionModeCAttribute(InstructionMode::MIPSClassic)
       << (Store ? "void" : Value) << " " << Name << "(" << Reg << " address, "
       << Value << " old, unsigned offset) {\n"
       << "    if (offset == " << unsigned(Load.Bytes)
       << "u) offset = address & " << unsigned(Load.Bytes - 1)
       << "u;\n    unsigned count = ";
    if (Forward)
      OS << unsigned(Load.Bytes) << "u - offset";
    else
      OS << "offset + 1u";
    OS << ";\n    " << Reg << " first = address" << (Forward ? "" : " - offset")
       << ";\n";
    if (!Store)
      OS << "    " << Value << " result = old;\n";
    OS << "    for (unsigned i = 0; i != count; ++i) {\n"
       << "        unsigned lane = ";
    if (Load.Left)
      OS << unsigned(Load.Bytes) << "u - count + ";
    OS << (Little ? "i" : "(count - 1u - i)")
       << ";\n        unsigned shift = lane * 8u;\n";
    if (Store) {
      OS << "        *(volatile uint8_t *)(uintptr_t)(" << Reg
         << ")(first + i) = (uint8_t)(old >> shift);\n    }\n}\n\n";
      continue;
    }
    OS << "        " << Value
       << " byte = *(const volatile uint8_t *)(uintptr_t)"
       << "(" << Reg << ")(first + i);\n"
       << "        result = (result & ~((" << Value << ")255u << shift)) | "
       << "(byte << shift);\n    }\n";
    OS << "    return result;\n}\n\n";
  }
}

template <typename Render>
std::string
renderMIPSScalar(const HighExpr &E, Arch Target, const std::string &Name,
                 std::optional<uint64_t> GuestAddress, Render &&Expression) {
  if (!mipsScalarHighShapeIsValid(E, Target))
    throw std::runtime_error(mips::kMalformedScalar);
  if (mips::isMSAControl(E.IntrinsicId)) {
    return Name + (E.IntrinsicId == Intrinsic::MIPSReadMSAControl
                       ? "()"
                       : "(" + Expression(*E.Operands[1], 1) + ")");
  }
  if (mips::isMSAFloat(E.IntrinsicId)) {
    std::string Call = Name + "(";
    for (unsigned I = 0; I < E.Operands.size(); ++I) {
      if (I)
        Call += ", ";
      Call += Expression(*E.Operands[I], I);
    }
    return Call + ")";
  }
  if (mips::isStatefulScalar(E.IntrinsicId))
    throw std::runtime_error(mips::kStatefulSourceUnsupported);
  if (E.IntrinsicId == Intrinsic::MIPSUnpredictable)
    throw std::runtime_error(mips::kUnpredictableResult);
  if (E.IntrinsicId == Intrinsic::MIPSAccumulatorAfterWrite)
    throw std::runtime_error(mips::kAccumulatorHistoryUnknown);
  if (E.IntrinsicId == Intrinsic::MIPSLwrUpper)
    throw std::runtime_error(mips::kPartialLwrRequiresWord);
  if (E.IntrinsicId == Intrinsic::MIPSSync)
    return Name + "_" + std::to_string(E.Operands[0]->ConstVal) + "()";
  std::string Result = Name + "(";
  const char *Reg = isMIPS64(Target) ? "uint64_t" : "uint32_t";
  for (unsigned I = 0; I != E.Operands.size(); ++I) {
    if (I)
      Result += ", ";
    Result += "(" + std::string(Reg) + ")";
    if (E.Operands[I]->Type->Kind == NdTypeKind::Ptr)
      Result += "(uintptr_t)";
    Result += "(" + Expression(*E.Operands[I], I) + ")";
  }
  if (const auto Load = mips::mergeMemory(E.IntrinsicId))
    Result += ", " +
              std::to_string(GuestAddress ? *GuestAddress & (Load->Bytes - 1)
                                          : Load->Bytes) +
              "u";
  return Result + ")";
}

} // namespace neverd

#endif // NEVERD_LIB_BACKEND_C_RENDER_MIPS_MIPSSCALAR_H
