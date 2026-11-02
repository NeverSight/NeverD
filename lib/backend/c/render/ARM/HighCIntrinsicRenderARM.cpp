//===- HighCIntrinsicRenderARM.cpp - ARM intrinsic rendering ----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific HighIR intrinsic rendering: ACLE barrier and
/// exclusive-monitor intrinsics (DMB, DSB, ISB, CLREX).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/AArch64/AArch64AddressMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64CPAC.h"
#include "neverd/backend/c/render/AArch64/AArch64ExclusiveC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthC.h"
#include "neverd/backend/c/render/AArch64/AArch64PAuthLoadC.h"
#include "neverd/backend/c/render/AArch64/AArch64SVECPAC.h"
#include "neverd/backend/c/render/AArch64/AArch64StructureMemoryC.h"
#include "neverd/backend/c/render/AArch64/AArch64WritebackMemoryC.h"
#include "neverd/backend/c/render/ARM/ArmCoprocessorAcle.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"
#include "neverd/ir/intrinsics/AArch64/AArch64ExceptionReturn.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPredicate.h"
#include "neverd/ir/intrinsics/AArch64/AArch64SVEVector.h"
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"
#include "neverd/support/IntegerLiterals.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <climits>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace neverd {

namespace {

#define NEVERD_A64_SVE_PREDICATE_C_TEXT(Name, Text)                            \
  constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64SVEPredicateC.def"

#define NEVERD_A64_SVE_VECTOR_C_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64SVEVectorC.def"

#define NEVERD_A64_SVE_PREFIX_C_TEXT(Name, Text) constexpr char Name[] = Text;
#include "neverd/backend/c/render/AArch64/AArch64SVEPrefixC.def"

struct SVEVectorCWidth {
  unsigned Bytes;
  const char *Scalar;
  const char *Dup;
  const char *Index;
  const char *Last;
  const char *ToCarrier;
  const char *FromCarrier;
};

const SVEVectorCWidth *sveVectorCWidth(uint64_t Bytes) {
  static constexpr SVEVectorCWidth Widths[] = {
#define NEVERD_A64_SVE_VECTOR_C_WIDTH(Bytes, Scalar, Dup, Index, Last, To,     \
                                      From)                                    \
  {Bytes, Scalar, Dup, Index, Last, To, From},
#include "neverd/backend/c/render/AArch64/AArch64SVEVectorC.def"
  };
  const auto *Width = llvm::find_if(
      Widths, [&](const auto &Entry) { return Entry.Bytes == Bytes; });
  return Width == std::end(Widths) ? nullptr : Width;
}

bool isAlive(const MedVar &V, const IsAliveFn &Fn) { return !Fn || Fn(V); }

/// The value of the constant operand \p Text, in whichever base the writer
/// spelled it (neverd/support/IntegerLiterals.h); none for any other operand.
/// Constants up to 10 always read in decimal, so comparing such an operand
/// with its decimal digits is exact.
std::optional<uint64_t> constantOperand(llvm::StringRef Text) {
  const std::string_view Operand(Text.data(), Text.size());
  bool InComment = false;
  const auto Literals = integerLiterals(Operand, LiteralSyntax::C, InComment);
  if (Literals.size() != 1 || Literals.front().Begin != 0 ||
      Literals.front().End != Operand.size())
    return std::nullopt;
  return Literals.front().Value;
}

/// Reads the constant operand \p Text into \p Value; false for any other
/// operand or a value \p Value cannot hold.
bool readConstant(llvm::StringRef Text, unsigned &Value) {
  const auto Constant = constantOperand(Text);
  if (!Constant || *Constant > std::numeric_limits<unsigned>::max())
    return false;
  Value = static_cast<unsigned>(*Constant);
  return true;
}

/// An AArch64 system instruction the lift kept the op0:op1:CRn:CRm:op2 field
/// of as \p Ops[0]: an MRS or MSR of a register as ACLE's __arm_rsr64 or
/// __arm_wsr64, which name it, and any other as an `asm` statement that runs
/// it (`dc civac, %0`).  Empty for any other shape.
std::string renderA64SystemInstruction(Intrinsic Id,
                                       const std::vector<std::string> &Ops,
                                       uint16_t ResultBytes,
                                       bool &HasCIntrinsics) {
  const std::optional<uint64_t> Field =
      Ops.empty() ? std::nullopt : constantOperand(Ops[0]);
  if (!Field || Ops.size() > 2)
    return {};
  const bool HasOutput = ResultBytes != 0;
  const bool HasRegister = Ops.size() == 2;
  const std::string Asm =
      aarch64SystemInstructionAsm(Id, *Field, HasOutput, HasRegister);
  if (Asm.empty())
    return {};
  if (Id == Intrinsic::Mrs) {
    HasCIntrinsics = true;
    return "__arm_rsr64(\"" +
           aarch64SystemRegisterName(*Field, /*Write=*/false) + "\")";
  }
  if (Id == Intrinsic::Msr && HasRegister) {
    HasCIntrinsics = true;
    return "__arm_wsr64(\"" +
           aarch64SystemRegisterName(*Field, /*Write=*/true) + "\", " + Ops[1] +
           ")";
  }
  // The `$0` of the assembly is GNU C's `%0`.
  std::string Template = Asm;
  if (const size_t Operand = Template.find("$0"); Operand != std::string::npos)
    Template[Operand] = '%';
  if (HasOutput)
    return "({ uint64_t __result; __asm__ volatile(\"" + Template +
           "\" : \"=r\"(__result) : : \"memory\"); __result; })";
  if (HasRegister)
    return "__asm__ volatile(\"" + Template + "\" : : \"r\"(" + Ops[1] +
           ") : \"memory\")";
  return "__asm__ volatile(\"" + Template + "\" ::: \"memory\")";
}

/// An ARM status register instruction the lift kept the field of as
/// \p Ops[0], as the `asm` statement that runs it: an MRS as a statement
/// expression of the word it reads, an MSR with the word it writes, a CPS
/// alone.  C and ACLE have no spelling of their own for these on AArch32.
std::string renderArmStatusInstruction(Intrinsic Id,
                                       const std::vector<std::string> &Ops,
                                       uint16_t ResultBytes) {
  const bool Writes = Id == Intrinsic::ArmMsr;
  const std::optional<uint64_t> Field =
      Ops.size() == (Writes ? 2u : 1u) ? constantOperand(Ops[0]) : std::nullopt;
  std::string Template =
      Field ? armStatusInstructionAsm(Id, *Field) : std::string();
  if (Template.empty() || (Id == Intrinsic::ArmMrs) != (ResultBytes == 4))
    throw std::invalid_argument(
        "ARM status register instruction without its field");
  // The `$0` of the assembly is GNU C's `%0`.
  if (const size_t Operand = Template.find("$0"); Operand != std::string::npos)
    Template[Operand] = '%';
  if (Id == Intrinsic::ArmMrs)
    return "({ uint32_t __status; __asm__ volatile(\"" + Template +
           "\" : \"=r\"(__status) : : \"memory\"); __status; })";
  if (Writes)
    return "__asm__ volatile(\"" + Template + "\" : : \"r\"((uint32_t)(" +
           Ops[1] + ")) : \"memory\", \"cc\")";
  return "__asm__ volatile(\"" + Template + "\" ::: \"memory\")";
}

/// The C type an exclusive access of \p Size bytes moves, or null for a
/// size no exclusive access has.
const char *exclusiveAccessType(llvm::StringRef Size) {
  return aarch64ExclusiveAccessType(
      static_cast<unsigned>(constantOperand(Size).value_or(0)),
      "unsigned __int128");
}

std::string
renderMopsSetPrologue(Intrinsic IID, const std::vector<MedVar> &Outputs,
                      const std::vector<ExprPtr> &Operands,
                      std::function<std::string(const HighExpr &)> ExprFn,
                      std::function<std::string(const MedVar &)> VarFn,
                      const IsAliveFn &IsAlive) {
  if (Outputs.size() < 2 || Operands.size() < 3)
    return {};
  const char *Mnemonic = intrinsicAsmMnemonic(IID);
  if (!Mnemonic)
    return {};

  std::string Result = "{\n";
  Result += "        uint64_t _nd_mops_dst = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[0]) + ");\n";
  Result += "        uint64_t _nd_mops_count = (uint64_t)(" +
            ExprFn(*Operands[1]) + ");\n";
  Result += "        uint64_t _nd_mops_nzcv;\n";
  Result += "        __asm__ volatile(\"" + std::string(Mnemonic) +
            " [%0]!, %1!, %3\\n\\tmrs %2, nzcv\"\n";
  Result += "                         : \"+r\"(_nd_mops_dst), "
            "\"+r\"(_nd_mops_count),\n";
  Result += "                           \"=r\"(_nd_mops_nzcv)\n";
  Result += "                         : \"r\"(" + ExprFn(*Operands[2]) + ")\n";
  Result += "                         : \"memory\", \"cc\");\n";
  const char *Names[] = {"_nd_mops_dst", "_nd_mops_count", "_nd_mops_nzcv"};
  for (size_t I = 0; I < Outputs.size() && I < 3; ++I)
    if (isAlive(Outputs[I], IsAlive))
      Result += "        " + VarFn(Outputs[I]) + " = " + Names[I] + ";\n";
  Result += "    }\n";
  return Result;
}

std::string
renderMopsCopyPrologue(const std::vector<MedVar> &Outputs,
                       const std::vector<ExprPtr> &Operands,
                       std::function<std::string(const HighExpr &)> ExprFn,
                       std::function<std::string(const MedVar &)> VarFn,
                       const IsAliveFn &IsAlive) {
  if (Outputs.size() < 2 || Operands.size() < 3)
    return {};

  std::string Result = "{\n";
  Result += "        uint64_t _nd_mops_dst = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[0]) + ");\n";
  Result += "        uint64_t _nd_mops_src = (uint64_t)(uintptr_t)(" +
            ExprFn(*Operands[1]) + ");\n";
  Result += "        uint64_t _nd_mops_count = (uint64_t)(" +
            ExprFn(*Operands[2]) + ");\n";
  Result += "        uint64_t _nd_mops_nzcv;\n";
  Result += "        __asm__ volatile(\"cpyfp [%0]!, [%1]!, %2!\\n\\t"
            "mrs %3, nzcv\"\n";
  Result += "                         : \"+r\"(_nd_mops_dst), "
            "\"+r\"(_nd_mops_src),\n";
  Result += "                           \"+r\"(_nd_mops_count), "
            "\"=r\"(_nd_mops_nzcv)\n";
  Result += "                         :\n";
  Result += "                         : \"memory\", \"cc\");\n";
  const char *Names[] = {"_nd_mops_dst", "_nd_mops_src", "_nd_mops_count",
                         "_nd_mops_nzcv"};
  for (size_t I = 0; I < Outputs.size() && I < 4; ++I)
    if (isAlive(Outputs[I], IsAlive))
      Result += "        " + VarFn(Outputs[I]) + " = " + Names[I] + ";\n";
  Result += "    }\n";
  return Result;
}

/// A value of an ACLE type, bit cast from the integer type of the same width
/// HighC carries it in.
std::string neonValue(const char *ValueType, const char *RawType,
                      const std::string &Operand) {
  return std::string("__builtin_bit_cast(") + ValueType + ", (" + RawType +
         ")(" + Operand + "))";
}

/// An AArch64 floating-point estimate or step (NeonIntrinsicSpellings.def):
/// its last operand is the element width, which with the result's width
/// picks the intrinsic's shape.
std::string renderNeonFloatOperation(Intrinsic Id,
                                     const std::vector<std::string> &Ops,
                                     uint16_t ResultBytes,
                                     bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    size_t Operands;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_FLOAT_OPERATION(ID, Base, Operands)                        \
  {Intrinsic::ID, Base, Operands},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  struct Shape {
    uint16_t ResultBytes;
    const char *ElementBytes;
    const char *RawType;
    const char *ValueType;
    const char *Suffix;
  };
  static constexpr Shape Shapes[] = {
#define NEVERD_NEON_FLOAT_SHAPE(ResultBytes, ElementBytes, RawType, ValueType, \
                                Suffix)                                        \
  {ResultBytes, #ElementBytes, RawType, ValueType, Suffix},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  if (Op == std::end(Operations) || Ops.size() != Op->Operands + 1)
    return {};
  const auto *S = llvm::find_if(Shapes, [&](const Shape &Entry) {
    return Entry.ResultBytes == ResultBytes && Ops.back() == Entry.ElementBytes;
  });
  if (S == std::end(Shapes))
    return {};
  HasCIntrinsics = true;
  std::string Call = std::string(Op->Base) + S->Suffix + "(";
  for (size_t I = 0; I < Op->Operands; ++I)
    Call += (I ? ", " : "") + neonValue(S->ValueType, S->RawType, Ops[I]);
  return std::string("__builtin_bit_cast(") + S->RawType + ", " + Call + "))";
}

/// URECPE or URSQRTE on the 32-bit lanes of a 64- or 128-bit vector.
std::string renderNeonU32Estimate(Intrinsic Id,
                                  const std::vector<std::string> &Ops,
                                  uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_U32_OPERATION(ID, Base) {Intrinsic::ID, Base},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  struct Shape {
    uint16_t ResultBytes;
    const char *RawType;
    const char *ValueType;
    const char *Suffix;
  };
  static constexpr Shape Shapes[] = {
#define NEVERD_NEON_U32_SHAPE(ResultBytes, RawType, ValueType, Suffix)         \
  {ResultBytes, RawType, ValueType, Suffix},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  const auto *S = llvm::find_if(Shapes, [&](const Shape &Entry) {
    return Entry.ResultBytes == ResultBytes;
  });
  if (Op == std::end(Operations) || S == std::end(Shapes) || Ops.size() != 1)
    return {};
  HasCIntrinsics = true;
  return std::string("__builtin_bit_cast(") + S->RawType + ", " + Op->Base +
         S->Suffix + "(" + neonValue(S->ValueType, S->RawType, Ops[0]) + "))";
}

/// The C element type prefix (`uint`) an ACLE suffix letter (`u`) names, or
/// null.
const char *neonElementTypePrefix(llvm::StringRef Letter) {
  struct Kind {
    const char *Letter;
    const char *TypePrefix;
  };
  static constexpr Kind Kinds[] = {
#define NEVERD_NEON_ELEMENT_KIND(Letter, TypePrefix) {Letter, TypePrefix},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *K = llvm::find_if(
      Kinds, [&](const Kind &Entry) { return Letter == Entry.Letter; });
  return K == std::end(Kinds) ? nullptr : K->TypePrefix;
}

/// A saturating add or subtract the lifter keeps as one scalar intrinsic of
/// the result's width (vqaddd_s64).
std::string renderNeonSaturating(Intrinsic Id,
                                 const std::vector<std::string> &Ops,
                                 uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    bool Signed;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_SATURATING_OPERATION(ID, Base, Signed)                     \
  {Intrinsic::ID, Base, Signed},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  struct Width {
    uint16_t Bytes;
    const char *Letter;
    const char *Bits;
  };
  static constexpr Width Widths[] = {
#define NEVERD_NEON_SATURATING_WIDTH(Bytes, Letter, Bits) {Bytes, Letter, Bits},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  const auto *W = llvm::find_if(
      Widths, [&](const Width &Entry) { return Entry.Bytes == ResultBytes; });
  if (Op == std::end(Operations) || W == std::end(Widths) || Ops.size() != 2)
    return {};
  const llvm::StringRef Kind = Op->Signed ? "s" : "u";
  const char *Prefix = neonElementTypePrefix(Kind);
  if (!Prefix)
    return {};
  const std::string Type = (llvm::Twine(Prefix) + W->Bits + "_t").str();
  HasCIntrinsics = true;
  return (llvm::Twine(Op->Base) + W->Letter + "_" + Kind + W->Bits + "((" +
          Type + ")(" + Ops[0] + "), (" + Type + ")(" + Ops[1] + "))")
      .str();
}

/// FCVTXN, narrowing doubles to floats rounding to odd.
std::string renderNeonFcvtxn(const std::vector<std::string> &Ops,
                             uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Form {
    uint16_t ResultBytes;
    const char *Name;
    const char *SourceRawType;
    const char *SourceType;
    const char *ResultRawType;
  };
  static constexpr Form Forms[] = {
#define NEVERD_NEON_FCVTXN(ResultBytes, Name, SourceRawType, SourceType,       \
                           ResultRawType)                                      \
  {ResultBytes, Name, SourceRawType, SourceType, ResultRawType},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *F = llvm::find_if(Forms, [&](const Form &Entry) {
    return Entry.ResultBytes == ResultBytes;
  });
  if (F == std::end(Forms) || Ops.size() != 1)
    return {};
  HasCIntrinsics = true;
  return std::string("__builtin_bit_cast(") + F->ResultRawType + ", " +
         F->Name + "(" + neonValue(F->SourceType, F->SourceRawType, Ops[0]) +
         "))";
}

/// A saturating or rounding shift by per-lane signed amounts
/// (NeonIntrinsicSpellings.def); its last operand is the element width.
std::string renderNeonShift(Intrinsic Id, const std::vector<std::string> &Ops,
                            uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    const char *Element;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_SHIFT_OPERATION(ID, Base, Element)                         \
  {Intrinsic::ID, Base, Element},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  struct Width {
    uint16_t Bytes;
    const char *Letter;
    const char *Bits;
  };
  static constexpr Width Widths[] = {
#define NEVERD_NEON_SATURATING_WIDTH(Bytes, Letter, Bits) {Bytes, Letter, Bits},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned ElementBytes = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 ||
      !readConstant(Ops[2], ElementBytes))
    return {};
  const auto *W = llvm::find_if(
      Widths, [&](const Width &Entry) { return Entry.Bytes == ElementBytes; });
  const llvm::StringRef Kind = Op->Element;
  const char *Prefix = neonElementTypePrefix(Kind);
  const char *SignedPrefix = neonElementTypePrefix("s");
  if (W == std::end(Widths) || !Prefix || !SignedPrefix)
    return {};
  if (ResultBytes == ElementBytes && ResultBytes < 8) {
    const std::string Type = (llvm::Twine(Prefix) + W->Bits + "_t").str();
    const std::string Amount =
        (llvm::Twine(SignedPrefix) + W->Bits + "_t").str();
    HasCIntrinsics = true;
    return (llvm::Twine(Op->Base) + W->Letter + "_" + Kind + W->Bits + "((" +
            Type + ")(" + Ops[0] + "), (" + Amount + ")(" + Ops[1] + "))")
        .str();
  }
  if (ResultBytes != 8 && ResultBytes != 16)
    return {};
  const unsigned Lanes = ResultBytes / ElementBytes;
  const std::string Value =
      (llvm::Twine(Prefix) + W->Bits + "x" + llvm::Twine(Lanes) + "_t").str();
  const std::string Amount =
      (llvm::Twine(SignedPrefix) + W->Bits + "x" + llvm::Twine(Lanes) + "_t")
          .str();
  const std::string Raw = typeToC(NdType::makeInt(ResultBytes, false));
  HasCIntrinsics = true;
  return "__builtin_bit_cast(" + Raw + ", " + Op->Base +
         (ResultBytes == 16 ? "q" : "") + "_" + Kind.str() + W->Bits + "(" +
         neonValue(Value.c_str(), Raw.c_str(), Ops[0]) + ", " +
         neonValue(Amount.c_str(), Raw.c_str(), Ops[1]) + "))";
}

/// A saturating shift left by an immediate of signed lanes to an unsigned
/// result (NeonIntrinsicSpellings.def); its operands are the value, the
/// immediate, which the intrinsic needs as a constant, and the element width.
std::string renderNeonShiftLeftUnsigned(Intrinsic Id,
                                        const std::vector<std::string> &Ops,
                                        uint16_t ResultBytes,
                                        bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_SHIFT_LEFT_UNSIGNED(ID, Base) {Intrinsic::ID, Base},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  struct Width {
    uint16_t Bytes;
    const char *Letter;
    const char *Bits;
  };
  static constexpr Width Widths[] = {
#define NEVERD_NEON_SATURATING_WIDTH(Bytes, Letter, Bits) {Bytes, Letter, Bits},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned Shift = 0, ElementBytes = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 ||
      !readConstant(Ops[1], Shift) || !readConstant(Ops[2], ElementBytes))
    return {};
  const auto *W = llvm::find_if(
      Widths, [&](const Width &Entry) { return Entry.Bytes == ElementBytes; });
  if (W == std::end(Widths))
    return {};
  if (ResultBytes == ElementBytes && ResultBytes < 8) {
    HasCIntrinsics = true;
    return (llvm::Twine(Op->Base) + W->Letter + "_n_s" + W->Bits + "((int" +
            W->Bits + "_t)(" + Ops[0] + "), " + llvm::Twine(Shift) + ")")
        .str();
  }
  if ((ResultBytes != 8 && ResultBytes != 16) || ResultBytes % ElementBytes)
    return {};
  const std::string Value = (llvm::Twine("int") + W->Bits + "x" +
                             llvm::Twine(ResultBytes / ElementBytes) + "_t")
                                .str();
  const std::string Raw = typeToC(NdType::makeInt(ResultBytes, false));
  HasCIntrinsics = true;
  return "__builtin_bit_cast(" + Raw + ", " + Op->Base +
         (ResultBytes == 16 ? "q" : "") + "_n_s" + W->Bits + "(" +
         neonValue(Value.c_str(), Raw.c_str(), Ops[0]) + ", " +
         std::to_string(Shift) + "))";
}

/// A polynomial multiply (NeonIntrinsicSpellings.def): bytes lane by lane,
/// byte pairs widened to 16 bits, or one 64-bit pair to 128 bits.
std::string renderNeonPolyMultiply(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    bool Long;
    bool ElementWidthOperand;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_POLY_MULTIPLY(ID, Base, Long, ElementWidthOperand)         \
  {Intrinsic::ID, Base, Long, ElementWidthOperand},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  if (Op == std::end(Operations) ||
      Ops.size() != (Op->ElementWidthOperand ? 3u : 2u))
    return {};
  unsigned ElementBytes = 1;
  if (Op->ElementWidthOperand && !readConstant(Ops[2], ElementBytes))
    return {};
  const std::string Raw = typeToC(NdType::makeInt(ResultBytes, false));
  if (!Op->Long) {
    if (ResultBytes != 8 && ResultBytes != 16)
      return {};
    const char *Vector = ResultBytes == 16 ? "poly8x16_t" : "poly8x8_t";
    HasCIntrinsics = true;
    return "__builtin_bit_cast(" + Raw + ", " + Op->Base +
           (ResultBytes == 16 ? "q" : "") + "_p8(" +
           neonValue(Vector, Raw.c_str(), Ops[0]) + ", " +
           neonValue(Vector, Raw.c_str(), Ops[1]) + "))";
  }
  if (ResultBytes != 16 || (ElementBytes != 1 && ElementBytes != 8))
    return {};
  const std::string Half = typeToC(NdType::makeInt(8, false));
  HasCIntrinsics = true;
  if (ElementBytes == 8)
    return "__builtin_bit_cast(" + Raw + ", " + Op->Base + "_p64((poly64_t)(" +
           Ops[0] + "), (poly64_t)(" + Ops[1] + ")))";
  return "__builtin_bit_cast(" + Raw + ", " + Op->Base + "_p8(" +
         neonValue("poly8x8_t", Half.c_str(), Ops[0]) + ", " +
         neonValue("poly8x8_t", Half.c_str(), Ops[1]) + "))";
}

/// A saturating doubling multiply (NeonIntrinsicSpellings.def); its last
/// operand is the result's element width.
std::string renderNeonDoublingMultiply(Intrinsic Id,
                                       const std::vector<std::string> &Ops,
                                       uint16_t ResultBytes,
                                       bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    bool Widening;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_DOUBLING_MULTIPLY(ID, Base, Widening)                      \
  {Intrinsic::ID, Base, Widening},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned ResultElement = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 ||
      !readConstant(Ops[2], ResultElement))
    return {};
  const unsigned SourceElement =
      Op->Widening ? ResultElement / 2 : ResultElement;
  const uint16_t SourceBytes = Op->Widening ? 8 : ResultBytes;
  if ((SourceElement != 2 && SourceElement != 4) ||
      (SourceBytes != 8 && SourceBytes != 16) ||
      (Op->Widening && ResultBytes != 16))
    return {};
  const unsigned Bits = SourceElement * 8;
  const std::string Source = (llvm::Twine("int") + llvm::Twine(Bits) + "x" +
                              llvm::Twine(SourceBytes * 8 / Bits) + "_t")
                                 .str();
  const std::string SourceRaw = typeToC(NdType::makeInt(SourceBytes, false));
  const std::string ResultRaw = typeToC(NdType::makeInt(ResultBytes, false));
  HasCIntrinsics = true;
  return "__builtin_bit_cast(" + ResultRaw + ", " + Op->Base +
         (!Op->Widening && SourceBytes == 16 ? "q" : "") + "_s" +
         std::to_string(Bits) + "(" +
         neonValue(Source.c_str(), SourceRaw.c_str(), Ops[0]) + ", " +
         neonValue(Source.c_str(), SourceRaw.c_str(), Ops[1]) + "))";
}

/// A saturating shift right by an immediate that narrows a 128-bit vector to
/// 64 bits (NeonIntrinsicSpellings.def); its operands are the vector, the
/// shift, and the narrow element width.
std::string renderNeonNarrowShift(Intrinsic Id,
                                  const std::vector<std::string> &Ops,
                                  uint16_t ResultBytes, bool &HasCIntrinsics) {
  struct Operation {
    Intrinsic Id;
    const char *Base;
    const char *SourceElement;
  };
  static constexpr Operation Operations[] = {
#define NEVERD_NEON_NARROW_SHIFT(ID, Base, SourceElement)                      \
  {Intrinsic::ID, Base, SourceElement},
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned NarrowBytes = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 || ResultBytes != 8 ||
      !readConstant(Ops[2], NarrowBytes) ||
      (NarrowBytes != 1 && NarrowBytes != 2 && NarrowBytes != 4))
    return {};
  const char *Prefix = neonElementTypePrefix(Op->SourceElement);
  if (!Prefix)
    return {};
  const unsigned SourceBits = NarrowBytes * 16;
  const std::string Source = (llvm::Twine(Prefix) + llvm::Twine(SourceBits) +
                              "x" + llvm::Twine(128 / SourceBits) + "_t")
                                 .str();
  HasCIntrinsics = true;
  return (llvm::Twine("__builtin_bit_cast(uint64_t, ") + Op->Base + "_" +
          Op->SourceElement + llvm::Twine(SourceBits) + "(" +
          neonValue(Source.c_str(), "unsigned __int128", Ops[0]) + ", " +
          Ops[1] + "))")
      .str();
}

} // anonymous namespace

bool armIntrinsicUsesCHeader(Intrinsic Id) {
  using I = Intrinsic;
  switch (Id) {
#define NEVERD_NEON_FLOAT_OPERATION(ID, Base, Operands) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_U32_OPERATION(ID, Base) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_SATURATING_OPERATION(ID, Base, Signed) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_SHIFT_OPERATION(ID, Base, Element) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_NARROW_SHIFT(ID, Base, SourceElement) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_SHIFT_LEFT_UNSIGNED(ID, Base) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_DOUBLING_MULTIPLY(ID, Base, Widening) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
  case I::A64_Fcvtxn:
  // An MRS or MSR of a register is ACLE's __arm_rsr64 or __arm_wsr64, and
  // a coprocessor transfer its __arm_mrc and kin.
  case I::Mrs:
  case I::Msr:
  case I::ArmMrc:
  case I::ArmMcr:
  case I::ArmMrrc:
  case I::ArmMcrr:
    return true;
  default:
    return false;
  }
}

llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders() {
  return {"arm_acle.h", "arm_neon.h"};
}

namespace {
/// An SMC or HVC #0 under the SMC Calling Convention: its function identifier
/// and arguments in x0-x7 (r0-r3) and its results in x0-x3 (r0-r3), which
/// register variables pin for the instruction.  \p Operands are the
/// immediate, then the argument registers.
std::string renderSmcccCall(Intrinsic IID, const std::vector<MedVar> &Outputs,
                            const std::vector<ExprPtr> &Operands,
                            std::function<std::string(const HighExpr &)> ExprFn,
                            std::function<std::string(const MedVar &)> VarFn,
                            const IsAliveFn &IsAlive) {
  const size_t Arguments = Operands.empty() ? 0 : Operands.size() - 1;
  if (Outputs.size() != 4 || (Arguments != 4 && Arguments != 8))
    return {};
  const bool AArch64 = IID == Intrinsic::Smc || IID == Intrinsic::Hvc;
  const std::string Type = AArch64 ? "uint64_t" : "uint32_t";
  const std::string Register = AArch64 ? "x" : "r";
  const auto Name = [&](size_t Index) {
    return "_nd_smccc_" + Register + std::to_string(Index);
  };
  std::string Result = "{\n";
  for (size_t Index = 0; Index < Arguments; ++Index)
    Result += "    register " + Type + " " + Name(Index) + " __asm__(\"" +
              Register + std::to_string(Index) + "\") = (" + Type + ")(" +
              ExprFn(*Operands[Index + 1]) + ");\n";
  Result +=
      "    __asm__ volatile(\"" + intrinsicInstructionText(IID, 0) + "\"\n";
  Result += "        : \"+r\"(" + Name(0) + "), \"+r\"(" + Name(1) +
            "), \"+r\"(" + Name(2) + "), \"+r\"(" + Name(3) + ")\n";
  std::string Inputs;
  for (size_t Index = 4; Index < Arguments; ++Index)
    Inputs += (Inputs.empty() ? "\"r\"(" : ", \"r\"(") + Name(Index) + ")";
  Result += "        : " + Inputs + "\n";
  Result += "        : \"memory\");\n";
  for (size_t Index = 0; Index < 4; ++Index)
    if (isAlive(Outputs[Index], IsAlive))
      Result += "    " + VarFn(Outputs[Index]) + " = " + Name(Index) + ";\n";
  Result += "}\n";
  return Result;
}
} // namespace

std::string
renderARMMultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive) {
  if (const auto Op = aarch64StructureOperation(IID); Op && Op->Load) {
    if (!aarch64WritebackMemoryOutputsAreValid(
            IID, Outputs, [](const MedVar &V) { return V.Size; }) ||
        std::any_of(Operands.begin(), Operands.end(),
                    [](const auto &V) { return !V || !V->Type; }) ||
        !aarch64StructureShapeIsValid(
            IID, Operands.size(), 0,
            [&](unsigned I) { return Operands[I]->Type->Size; },
            [&](unsigned I) -> std::optional<uint64_t> {
              return Operands[I]->Kind == ExprKind::Const
                         ? std::optional<uint64_t>(Operands[I]->ConstVal)
                         : std::nullopt;
            }))
      throw std::invalid_argument(kWritebackMalformed);
    const auto I = *aarch64StructureMemoryInstruction(
        Operands[kWritebackWordOperand]->ConstVal);
    std::vector<std::string> Data, Names;
    for (unsigned N = kStructureDataOperand; N < Operands.size(); ++N)
      Data.push_back(ExprFn(*Operands[N]));
    for (const auto &Output : Outputs)
      Names.push_back(!IsAlive || IsAlive(Output) ? VarFn(Output)
                                                  : std::string());
    return aarch64StructureMemoryC(I, ExprFn(*Operands[0]),
                                   ExprFn(*Operands[kStructureIndexOperand]),
                                   Data, Names);
  }
  if (isAArch64WritebackAuxiliaryLoad(IID)) {
    if (Operands.size() != kWritebackLoadOperands ||
        !aarch64WritebackMemoryOutputsAreValid(
            IID, Outputs, [](const MedVar &V) { return V.Size; }) ||
        !Operands[kWritebackBaseOperand] || !Operands[kWritebackWordOperand] ||
        !Operands[kWritebackBaseOperand]->Type ||
        !Operands[kWritebackWordOperand]->Type ||
        Operands[kWritebackWordOperand]->Kind != ExprKind::Const ||
        !aarch64WritebackMemoryShapeIsValid(
            IID, Operands.size() + 1, 0,
            Operands[kWritebackBaseOperand]->Type->Size,
            Operands[kWritebackWordOperand]->Type->Size,
            Operands[kWritebackWordOperand]->ConstVal))
      throw std::invalid_argument(kWritebackMalformed);
    const auto Instruction = *aarch64WritebackMemoryInstruction(
        Operands[kWritebackWordOperand]->ConstVal);
    if (Instruction.isPair()) {
      std::array<std::string, kPairBaseIndex + 1> Names;
      for (unsigned I = 0; I != Names.size(); ++I)
        if (!IsAlive || IsAlive(Outputs[I]))
          Names[I] = VarFn(Outputs[I]);
      return aarch64PairMemoryCStatement(
          Instruction, ExprFn(*Operands[kWritebackBaseOperand]), Names);
    }
    return aarch64VectorWritebackMemoryCStatement(
        Instruction, ExprFn(*Operands[kWritebackBaseOperand]),
        !IsAlive || IsAlive(Outputs[kWritebackValueIndex])
            ? VarFn(Outputs[kWritebackValueIndex])
            : std::string(),
        !IsAlive || IsAlive(Outputs[kWritebackBaseIndex])
            ? VarFn(Outputs[kWritebackBaseIndex])
            : std::string());
  }
  using I = Intrinsic;
  switch (IID) {
  case I::A64_MopsSetP:
  case I::A64_MopsSetPN:
  case I::A64_MopsSetPT:
  case I::A64_MopsSetPTN:
    return renderMopsSetPrologue(IID, Outputs, Operands, ExprFn, VarFn,
                                 IsAlive);
  case I::A64_MopsCpyFP:
    return renderMopsCopyPrologue(Outputs, Operands, ExprFn, VarFn, IsAlive);
  case I::Smc:
  case I::Hvc:
  case I::ArmSmc:
  case I::ArmHvc:
    return renderSmcccCall(IID, Outputs, Operands, ExprFn, VarFn, IsAlive);
  default:
    return {};
  }
}

std::string renderA64FixedHalfConversion(Intrinsic Id,
                                         const std::vector<std::string> &Ops) {
  using I = Intrinsic;
  const bool ToHalf = Id == I::A64_ScvtfFixed || Id == I::A64_UcvtfFixed;
  const bool FromHalf = Id == I::A64_FcvtzsFixed || Id == I::A64_FcvtzuFixed;
  const char *Mnemonic = intrinsicAsmMnemonic(Id);
  if ((!ToHalf && !FromHalf) || !Mnemonic || Ops.size() != 3 ||
      (Ops[2] != "0" && Ops[2] != "1"))
    return {};
  const bool Wide = Ops[2] == "1";
  unsigned FBits = 0;
  if (!readConstant(Ops[1], FBits) || FBits == 0 || FBits > (Wide ? 64u : 32u))
    return {};
  const bool Signed = Id == I::A64_ScvtfFixed || Id == I::A64_FcvtzsFixed;
  const std::string Integer =
      std::string(Signed ? "int" : "uint") + (Wide ? "64_t" : "32_t");
  const std::string Register = Wide ? "%x" : "%w";
  const std::string Shift = ", #" + std::to_string(FBits);
  if (ToHalf)
    return "({ float16_t neverd_half; __asm__(\"" + std::string(Mnemonic) +
           " %h0, " + Register + "1" + Shift +
           "\" : \"=w\"(neverd_half) : \"r\"((" + Integer + ")(" + Ops[0] +
           "))); __builtin_bit_cast(uint16_t, neverd_half); })";
  return "({ " + Integer + " neverd_int; __asm__(\"" + std::string(Mnemonic) +
         " " + Register + "0, %h1" + Shift +
         "\" : \"=r\"(neverd_int) : \"w\"(__builtin_bit_cast(float16_t, "
         "(uint16_t)(" +
         Ops[0] + ")))); neverd_int; })";
}

std::string renderARMIntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   uint16_t ResultBytes, bool &HasCIntrinsics,
                                   llvm::ArrayRef<uint16_t> OperandBytes) {
  using I = Intrinsic;
  if (isAArch64ExceptionReturnIntrinsic(Id))
    throw std::invalid_argument(kAArch64ExceptionReturnUnsupported);
  if (isAArch64AddressMemoryIntrinsic(Id)) {
    const unsigned Count = aarch64AddressMemoryOperandCount(Id);
    const bool Store = isAArch64AddressStoreIntrinsic(Id);
    const auto Word = Ops.size() == Count
                          ? constantOperand(Ops[kAddressMemoryWordOperand])
                          : std::nullopt;
    if (OperandBytes.size() != Count || Ops.size() != Count ||
        !aarch64AddressMemoryShapeIsValid(
            Id, Ops.size() + 1, ResultBytes,
            OperandBytes[kAddressMemoryBaseOperand],
            OperandBytes[kAddressMemoryIndexOperand],
            OperandBytes[kAddressMemoryWordOperand], Word,
            constantOperand(Ops[kAddressMemoryIndexOperand]),
            Store ? OperandBytes[kAddressMemoryDataOperand] : 0,
            Store ? constantOperand(Ops[kAddressMemoryDataOperand])
                  : std::nullopt))
      throw std::invalid_argument(kAddressMemoryMalformed);
    const auto Instruction = *aarch64AddressMemoryInstruction(*Word);
    const auto Value = aarch64AddressMemoryC(
        Instruction, Ops[kAddressMemoryBaseOperand],
        Ops[kAddressMemoryIndexOperand],
        Store ? Ops[kAddressMemoryDataOperand] : llvm::StringRef{});
    return !Store && ResultBytes < kAddressMemoryPointerBytes
               ? llvm::formatv(kAddressLoadCDataCast, ResultBytes * CHAR_BIT,
                               Value)
                     .str()
               : Value;
  }
  if (const auto Op = aarch64StructureOperation(Id)) {
    if (Op->Load || Ops.size() != OperandBytes.size() ||
        !aarch64StructureShapeIsValid(
            Id, Ops.size(), ResultBytes,
            [&](unsigned I) { return OperandBytes[I]; },
            [&](unsigned I) { return constantOperand(Ops[I]); }))
      throw std::invalid_argument(kWritebackMalformed);
    const auto I = *aarch64StructureMemoryInstruction(
        *constantOperand(Ops[kWritebackWordOperand]));
    std::vector<std::string> Data(Ops.begin() + kStructureDataOperand,
                                  Ops.end());
    return aarch64StructureMemoryC(I, Ops[0], Ops[kStructureIndexOperand],
                                   Data);
  }
  if (isAArch64WritebackMemoryIntrinsic(Id)) {
    const unsigned Count = aarch64WritebackMemoryOperandCount(Id);
    const bool Store = isAArch64WritebackStoreIntrinsic(Id);
    const auto Word = Ops.size() == Count
                          ? constantOperand(Ops[kWritebackWordOperand])
                          : std::nullopt;
    if (Ops.size() != Count || OperandBytes.size() != Count ||
        !aarch64WritebackMemoryShapeIsValid(
            Id, Count + 1, ResultBytes, OperandBytes[kWritebackBaseOperand],
            OperandBytes[kWritebackWordOperand], Word,
            Store ? OperandBytes[kWritebackDataOperand] : 0,
            Store ? constantOperand(Ops[kWritebackDataOperand]) : std::nullopt,
            Store && isAArch64PairMemoryIntrinsic(Id)
                ? OperandBytes[kPairSecondDataOperand]
                : 0,
            Store && isAArch64PairMemoryIntrinsic(Id)
                ? constantOperand(Ops[kPairSecondDataOperand])
                : std::nullopt))
      throw std::invalid_argument(kWritebackMalformed);
    const auto Instruction = *aarch64WritebackMemoryInstruction(*Word);
    if (Instruction.isPair() && Store)
      return aarch64PairStoreMemoryC(Instruction, Ops[kWritebackBaseOperand],
                                     Ops[kWritebackDataOperand],
                                     Ops[kPairSecondDataOperand]);
    return aarch64WritebackMemoryC(Instruction, Ops[kWritebackBaseOperand],
                                   Store ? Ops[kWritebackDataOperand]
                                         : llvm::StringRef{});
  }
  if (isAArch64PAuthLoadIntrinsic(Id)) {
    const auto Word = Ops.size() == kPAuthLoadOperandCount
                          ? constantOperand(Ops[1])
                          : std::nullopt;
    if (OperandBytes.size() != kPAuthLoadOperandCount ||
        !aarch64PAuthLoadShapeIsValid(Id, Ops.size() + 1, ResultBytes,
                                      OperandBytes[0], OperandBytes[1], Word))
      throw std::invalid_argument(kPAuthLoadMalformed);
    return aarch64PAuthLoadC(*aarch64PAuthLoadInstruction(*Word), Ops[0]);
  }
  if (Id == Intrinsic::A64_PACMPending)
    throw std::invalid_argument(kAArch64PAuthMalformedOperation);
  if (const auto Auth = aarch64PAuthExtendedOperation(Id)) {
    if (ResultBytes != kAArch64PAuthPointerBytes)
      throw std::invalid_argument(kAArch64PAuthMalformedOperation);
    return aarch64PAuthLRC(*Auth, Ops);
  }
  if (const auto Operation = aarch64CPAOperation(Id)) {
    if (ResultBytes != kAArch64CPAPointerBytes)
      throw std::invalid_argument(kAArch64CPAMalformedOperation);
    return aarch64CPAC(*Operation, Ops);
  }
  if (const auto Auth = aarch64PAuthOperation(Id)) {
    if (ResultBytes != kAArch64PAuthPointerBytes ||
        Ops.size() != (Auth->Strip ? 1 : 2))
      throw std::invalid_argument(kAArch64PAuthMalformedOperation);
    return aarch64PAuthMachineC(*Auth, Ops[0], Auth->Strip ? "" : Ops[1]);
  }
  if (Id == I::A64_AuthBranch) {
    if (ResultBytes != kAArch64PAuthPointerBytes || Ops.size() != 3 ||
        !constantOperand(Ops[1]) ||
        !aarch64PAuthCodeKeyIsValid(*constantOperand(Ops[1])))
      throw std::invalid_argument(kAArch64PAuthMalformedBranch);
    const std::string Name =
        aarch64PAuthTemporary(kAArch64PAuthCTarget, {Ops[0], Ops[1], Ops[2]});
    return "({ " +
           llvm::formatv(kAArch64PAuthCTargetDeclaration, Name,
                         aarch64PAuthAuthenticateC(Ops[0], Ops[1], Ops[2]))
               .str() +
           aarch64PAuthTargetUseC(Name) + Name + "; })";
  }
  switch (Id) {
  case I::A64_Rbit: {
    if (Ops.size() != 1)
      return {};
    const char *Builtin = ResultBytes == 1   ? "__builtin_bitreverse8"
                          : ResultBytes == 2 ? "__builtin_bitreverse16"
                          : ResultBytes == 4 ? "__builtin_bitreverse32"
                          : ResultBytes == 8 ? "__builtin_bitreverse64"
                                             : nullptr;
    if (!Builtin)
      return {};
    return std::string(Builtin) + "(" + Ops[0] + ")";
  }
  case I::A64_SvePtrue: {
    if (Ops.size() != aarch64_sve::kPtrueOperandCount)
      return {};
    const auto Pattern = constantOperand(Ops[0]);
    const auto Element = constantOperand(Ops[1]);
    if (!aarch64_sve::isValidPtrueShape(
            {static_cast<unsigned>(Ops.size()), ResultBytes,
             aarch64_sve::kPatternOperandBytes,
             aarch64_sve::kElementOperandBytes, Pattern, Element}))
      return {};
    const bool All = *Pattern == aarch64_sve::kPtruePatternAll;
    const char *Name = nullptr;
    switch (*Element) {
#define NEVERD_A64_SVE_PREDICATE_C(Bytes, AllName, PatternName)                \
  case Bytes:                                                                  \
    Name = All ? AllName : PatternName;                                        \
    break;
#include "neverd/backend/c/render/AArch64/AArch64SVEPredicateC.def"
    }
    HasCIntrinsics = true;
    return All ? llvm::formatv(kPtrueAllFormat, Name).str()
               : llvm::formatv(kPtruePatternFormat, Name, Ops[0]).str();
  }
  case I::A64_SveDup:
  case I::A64_SveIndex:
  case I::A64_SveLastb:
  case I::A64_SveSt1:
  case I::A64_SveStnt1:
#define NEVERD_A64_SVE_CPA_OPERATION(Id, ...) case I::Id:
#include "neverd/ir/intrinsics/AArch64/AArch64SVECPA.def"
#define NEVERD_A64_SVE_PREFIX_OPERATION(Id, ...) case I::Id:
#include "neverd/ir/intrinsics/AArch64/AArch64SVEPrefix.def"
  {
    const auto Operation = aarch64_sve::vectorOperation(Id);
    if (!Operation || Ops.size() != Operation->OperandCount ||
        OperandBytes.size() != Ops.size())
      return {};
    aarch64_sve::VectorShape Shape;
    Shape.NumOperands = Ops.size();
    Shape.OutputBytes = ResultBytes;
    for (size_t I = 0; I != Ops.size(); ++I)
      Shape.Operands[I] = {OperandBytes[I], constantOperand(Ops[I])};
    if (!aarch64_sve::isValidVectorShape(Id, Shape))
      return {};
    if (const auto CPA = aarch64_sve::cpaOperation(Id)) {
      HasCIntrinsics = true;
      return aarch64SVECPAC(*CPA, llvm::ArrayRef<std::string>(Ops).drop_back());
    }
    if (const auto Prefix = aarch64_sve::prefixOperation(Id)) {
      HasCIntrinsics = true;
      const auto &Predicate = Ops[aarch64_sve::kPrefixPredicateInput];
      const auto &Source = Ops[aarch64_sve::kPrefixSourceInput];
      return Prefix->Form == aarch64_sve::PrefixForm::Merging
                 ? llvm::formatv(kPrefixMergeC, Predicate, Source,
                                 Ops[aarch64_sve::kPrefixMergeInput])
                       .str()
                 : llvm::formatv(kPrefixZeroC, Predicate, Source).str();
    }
    const auto Element = constantOperand(Ops[Operation->ElementIndex]);
    const auto *Width = Element ? sveVectorCWidth(*Element) : nullptr;
    if (!Width)
      return {};
    if (Operation->ProducesVector) {
      if (ResultBytes != aarch64_sve::kVectorStorageBytes)
        return {};
      // LLVM zero-extends the original scalar bit width before widening a
      // lane. C's usual signed conversion must not sign-extend it instead.
      auto ScalarBits = [&](size_t I) {
        const auto *SourceWidth = sveVectorCWidth(OperandBytes[I]);
        return llvm::formatv(kVectorScalarBits, SourceWidth->Scalar, Ops[I])
            .str();
      };
      const std::string Value =
          Id == I::A64_SveDup
              ? llvm::formatv(kVectorDup, Width->Dup, Width->Scalar,
                              ScalarBits(0))
                    .str()
              : llvm::formatv(kVectorIndex, Width->Index, Width->Scalar,
                              ScalarBits(0), ScalarBits(1))
                    .str();
      HasCIntrinsics = true;
      return llvm::formatv(kVectorCall, Width->ToCarrier, Value).str();
    }
    const std::string Value = llvm::formatv(kVectorCall, Width->FromCarrier,
                                            Ops[aarch64_sve::kVectorInputIndex])
                                  .str();
    if (Id == I::A64_SveLastb) {
      if (!aarch64_sve::isValidElementBytes(ResultBytes) ||
          ResultBytes < *Element)
        return {};
      HasCIntrinsics = true;
      return llvm::formatv(kVectorLast, Width->Last, Ops[0], Value).str();
    }
    const auto Store = constantOperand(Ops.back());
    const auto *StoreWidth = Store ? sveVectorCWidth(*Store) : nullptr;
    if (ResultBytes || !StoreWidth)
      return {};
    const char *Name = nullptr;
#define NEVERD_A64_SVE_VECTOR_C_STORE(Lane, Bytes, Normal, NonTemporal)        \
  if (*Element == Lane && *Store == Bytes)                                     \
    Name = Id == I::A64_SveSt1 ? Normal : NonTemporal;
#include "neverd/backend/c/render/AArch64/AArch64SVEVectorC.def"
    if (!Name)
      return {};
    HasCIntrinsics = true;
    return llvm::formatv(kVectorStore, Name, Ops[0], StoreWidth->Scalar, Ops[2],
                         Value)
        .str();
  }
  case I::A64_SveCntb:
  case I::A64_SveCnth:
  case I::A64_SveCntw:
  case I::A64_SveCntd: {
    HasCIntrinsics = true;
    const char *Name = intrinsicCName(Id);
    return Name ? std::string(Name) + "()" : std::string();
  }
  case I::A64_AtomicAnd:
  case I::A64_AtomicOr:
  case I::A64_AtomicXor:
  case I::A64_AtomicSmax:
  case I::A64_AtomicSmin:
  case I::A64_AtomicUmax:
  case I::A64_AtomicUmin: {
    if (Ops.size() < 4)
      return {};
    const char *Ordering = nullptr;
    if (Ops[3] == "0")
      Ordering = "__ATOMIC_RELAXED";
    else if (Ops[3] == "2")
      Ordering = "__ATOMIC_ACQUIRE";
    else if (Ops[3] == "3")
      Ordering = "__ATOMIC_RELEASE";
    else if (Ops[3] == "4")
      Ordering = "__ATOMIC_ACQ_REL";
    else if (Ops[3] == "5")
      Ordering = "__ATOMIC_SEQ_CST";
    if (!Ordering)
      return {};

    const char *Builtin = intrinsicCName(Id);
    if (!Builtin)
      return {};
    return std::string(Builtin) + "((uint64_t)(" + Ops[0] +
           "), (void *)(uintptr_t)(" + Ops[1] + "), " + Ops[2] + ", " +
           Ordering + ")";
  }
  case I::A64_Pacga:
    if (ResultBytes != kAArch64PAuthPointerBytes || Ops.size() != 2)
      throw std::invalid_argument(kAArch64PAuthMalformedOperation);
    return aarch64PAuthGenericSignC(Ops[0], Ops[1]);
  case I::Addg:
  case I::Subg: {
    if (Ops.size() < 3)
      return {};
    HasCIntrinsics = true;
    if (Id == I::Subg)
      return "((uint64_t)(uintptr_t)__builtin_arm_subg((void *)(uintptr_t)(" +
             Ops[0] + "), " + Ops[1] + ", " + Ops[2] + "))";
    return "((uint64_t)(uintptr_t)__builtin_arm_addg((void *)(uintptr_t)(" +
           Ops[0] + " + " + Ops[1] + "), " + Ops[2] + "))";
  }
  case I::Stg: {
    if (Ops.size() < 2)
      return {};
    HasCIntrinsics = true;
    return "__arm_mte_set_tag((void *)(uintptr_t)(((uint64_t)(" + Ops[1] +
           ") & 0xf0ffffffffffffffULL) | ((uint64_t)(" + Ops[0] +
           ") & 0x0f00000000000000ULL)))";
  }
  case I::Ldg: {
    if (Ops.size() < 2)
      return {};
    HasCIntrinsics = true;
    return "(((uint64_t)(" + Ops[0] +
           ") & 0xf0ffffffffffffffULL) | ((uint64_t)(uintptr_t)"
           "__arm_mte_get_tag((void *)(uintptr_t)(" +
           Ops[1] + ")) & 0x0f00000000000000ULL))";
  }
  case I::A64_Bfmmla: {
    if (Ops.size() < 3)
      return {};
    auto BitCastArg = [&](const char *Type, const std::string &Arg) {
      return std::string("__builtin_bit_cast(") + Type +
             ", (unsigned __int128)(" + Arg + "))";
    };
    HasCIntrinsics = true;
    return "__builtin_bit_cast(unsigned __int128, vbfmmlaq_f32(" +
           BitCastArg("float32x4_t", Ops[0]) + ", " +
           BitCastArg("bfloat16x8_t", Ops[1]) + ", " +
           BitCastArg("bfloat16x8_t", Ops[2]) + "))";
  }
  case I::A64_Frecpx: {
    if (Ops.size() < 2)
      return {};
    const char *RawTy = nullptr;
    const char *FloatTy = nullptr;
    const char *Builtin = nullptr;
    if (Ops[1] == "2") {
      RawTy = "uint16_t";
      FloatTy = "float16_t";
      Builtin = "vrecpxh_f16";
    } else if (Ops[1] == "4") {
      RawTy = "uint32_t";
      FloatTy = "float";
      Builtin = "vrecpxs_f32";
    } else if (Ops[1] == "8") {
      RawTy = "uint64_t";
      FloatTy = "double";
      Builtin = "vrecpxd_f64";
    } else {
      return {};
    }
    HasCIntrinsics = true;
    return std::string("__builtin_bit_cast(") + RawTy + ", " + Builtin +
           "(__builtin_bit_cast(" + FloatTy + ", (" + RawTy + ")(" + Ops[0] +
           "))))";
  }
  case I::A64_Frinti: {
    if (Ops.size() < 2)
      return {};
    const char *RawTy = nullptr;
    const char *FloatTy = nullptr;
    if (Ops[1] == "2") {
      RawTy = "uint16_t";
      FloatTy = "_Float16";
    } else if (Ops[1] == "4") {
      RawTy = "uint32_t";
      FloatTy = "float";
    } else if (Ops[1] == "8") {
      RawTy = "uint64_t";
      FloatTy = "double";
    } else {
      return {};
    }
    return std::string("__builtin_bit_cast(") + RawTy +
           ", __builtin_elementwise_nearbyint(__builtin_bit_cast(" + FloatTy +
           ", (" + RawTy + ")(" + Ops[0] + "))))";
  }
  case I::A64_Fjcvtzs:
    if (Ops.empty())
      return {};
    return "__jcvt(__builtin_bit_cast(double, (uint64_t)(" + Ops[0] + ")))";
  case I::A64_Rcwcasp:
  case I::A64_Rcwcaspa:
  case I::A64_Rcwcaspal:
  case I::A64_Rcwcaspl:
  case I::A64_Rcwscasp:
  case I::A64_Rcwscaspa:
  case I::A64_Rcwscaspal:
  case I::A64_Rcwscaspl: {
    if (Ops.size() < 3)
      return {};
    const char *Builtin = intrinsicCName(Id);
    if (!Builtin)
      return {};
    return std::string(Builtin) + "((unsigned __int128)(" + Ops[0] +
           "), (unsigned __int128)(" + Ops[1] + "), (void *)(uintptr_t)(" +
           Ops[2] + "))";
  }
  case I::A64_Ldclrp:
  case I::A64_Ldclrpa:
  case I::A64_Ldclrpal:
  case I::A64_Ldclrpl:
  case I::A64_Ldsetp:
  case I::A64_Ldsetpa:
  case I::A64_Ldsetpal:
  case I::A64_Ldsetpl: {
    if (Ops.size() < 2)
      return {};
    const bool IsSet = Id == I::A64_Ldsetp || Id == I::A64_Ldsetpa ||
                       Id == I::A64_Ldsetpal || Id == I::A64_Ldsetpl;
    const char *Ordering = "__ATOMIC_RELAXED";
    if (Id == I::A64_Ldclrpa || Id == I::A64_Ldsetpa)
      Ordering = "__ATOMIC_ACQUIRE";
    else if (Id == I::A64_Ldclrpal || Id == I::A64_Ldsetpal)
      Ordering = "__ATOMIC_ACQ_REL";
    else if (Id == I::A64_Ldclrpl || Id == I::A64_Ldsetpl)
      Ordering = "__ATOMIC_RELEASE";
    const char *Builtin = IsSet ? "__atomic_fetch_or" : "__atomic_fetch_and";
    const char *Negate = IsSet ? "" : "~";
    return std::string(Builtin) + "((unsigned __int128 *)(uintptr_t)(" +
           Ops[1] + "), " + Negate + "(unsigned __int128)(" + Ops[0] + "), " +
           Ordering + ")";
  }
  case I::A64_Ldxr:
  case I::A64_Ldaxr:
  case I::A64_Ldxp:
  case I::A64_Ldaxp: {
    if (Ops.size() < 2)
      return {};
    const char *Type = exclusiveAccessType(Ops[1]);
    if (!Type)
      return {};
    HasCIntrinsics = true;
    return aarch64ExclusiveLoadC(Id == I::A64_Ldaxr || Id == I::A64_Ldaxp, Type,
                                 Ops[0]);
  }
  case I::A64_Stxr:
  case I::A64_Stlxr:
  case I::A64_Stxp:
  case I::A64_Stlxp: {
    if (Ops.size() < 3)
      return {};
    const char *Type = exclusiveAccessType(Ops[2]);
    if (!Type)
      return {};
    HasCIntrinsics = true;
    return aarch64ExclusiveStoreC(Id == I::A64_Stlxr || Id == I::A64_Stlxp,
                                  Type, Ops[0], Ops[1]);
  }
  case I::A64_Famax:
  case I::A64_Famin: {
    if (Ops.size() < 4)
      return {};
    const bool IsMax = Id == I::A64_Famax;
    const bool IsQ = constantOperand(Ops[2]) == 16u;
    const std::string &LaneBytes = Ops[3];
    const char *VecTy = nullptr;
    const char *Suffix = nullptr;
    if (LaneBytes == "2") {
      VecTy = IsQ ? "float16x8_t" : "float16x4_t";
      Suffix = "f16";
    } else if (LaneBytes == "4") {
      VecTy = IsQ ? "float32x4_t" : "float32x2_t";
      Suffix = "f32";
    } else if (LaneBytes == "8" && IsQ) {
      VecTy = "float64x2_t";
      Suffix = "f64";
    } else {
      return {};
    }
    const char *RawTy = IsQ ? "unsigned __int128" : "uint64_t";
    std::string Fn = IsMax ? "vamax" : "vamin";
    if (IsQ)
      Fn += "q";
    Fn += "_";
    Fn += Suffix;
    HasCIntrinsics = true;
    auto BitCastArg = [&](const std::string &Arg) {
      return std::string("__builtin_bit_cast(") + VecTy + ", (" + RawTy + ")(" +
             Arg + "))";
    };
    return std::string("__builtin_bit_cast(") + RawTy + ", " + Fn + "(" +
           BitCastArg(Ops[0]) + ", " + BitCastArg(Ops[1]) + "))";
  }
  case I::Mrs:
  case I::Msr:
  case I::Dc:
  case I::Ic:
  case I::Tlbi:
  case I::At:
    return renderA64SystemInstruction(Id, Ops, ResultBytes, HasCIntrinsics);
  case I::ArmMrs:
  case I::ArmMsr:
  case I::ArmCps:
    return renderArmStatusInstruction(Id, Ops, ResultBytes);
  case I::ArmThreadPointer:
    // The builtin's `void *` is the word the routine returns in r0.
    if (!Ops.empty())
      throw std::invalid_argument("ARM thread pointer read with operands");
    return "(uint32_t)__builtin_thread_pointer()";
  case I::ArmMrc:
  case I::ArmMcr:
  case I::ArmMrrc:
  case I::ArmMcrr: {
    // The lift keeps the instruction's numbers as the field, then an MCR's
    // or MCRR's value.
    const bool Writes = Id == I::ArmMcr || Id == I::ArmMcrr;
    const std::optional<uint64_t> Field = Ops.size() == (Writes ? 2u : 1u)
                                              ? constantOperand(Ops[0])
                                              : std::nullopt;
    std::string Call = Field ? armCoprocessorAcleCall(
                                   Id, *Field, Writes ? Ops[1] : std::string())
                             : std::string();
    if (Call.empty())
      throw std::invalid_argument(
          "ARM coprocessor transfer without its coprocessor field");
    HasCIntrinsics = true;
    return Call;
  }
  case I::Dmb:
  case I::ArmDmb:
  case I::Dsb:
  case I::ArmDsb:
  case I::Isb:
  case I::ArmIsb: {
    // The barrier's own option, which the lift keeps (`dsb sy` is
    // `__dsb(0xF)`); a lift that kept none ran these.
    const bool Dmb = Id == I::Dmb || Id == I::ArmDmb;
    const bool Dsb = Id == I::Dsb || Id == I::ArmDsb;
    std::string Option = Dsb ? "0xB" : "0xF";
    if (!Ops.empty()) {
      const std::optional<uint64_t> Value = constantOperand(Ops[0]);
      Option = Value ? "0x" + llvm::utohexstr(*Value) : Ops[0];
    }
    return std::string(Dmb   ? "__dmb("
                       : Dsb ? "__dsb("
                             : "__isb(") +
           Option + ")";
  }
  case I::A64_Clrex:
  case I::ArmClrex:
    HasCIntrinsics = true;
    return "__builtin_arm_clrex()";
#define NEVERD_NEON_FLOAT_OPERATION(ID, Base, Operands) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonFloatOperation(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_U32_OPERATION(ID, Base) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonU32Estimate(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_SATURATING_OPERATION(ID, Base, Signed) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonSaturating(Id, Ops, ResultBytes, HasCIntrinsics);
  case I::A64_Fcvtxn:
    return renderNeonFcvtxn(Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_SHIFT_OPERATION(ID, Base, Element) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonShift(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_NARROW_SHIFT(ID, Base, SourceElement) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonNarrowShift(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_SHIFT_LEFT_UNSIGNED(ID, Base) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonShiftLeftUnsigned(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_DOUBLING_MULTIPLY(ID, Base, Widening) case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonDoublingMultiply(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_POLY_MULTIPLY(ID, Base, Long, ElementWidthOperand)         \
  case I::ID:
#include "neverd/backend/c/render/ARM/NeonIntrinsicSpellings.def"
    return renderNeonPolyMultiply(Id, Ops, ResultBytes, HasCIntrinsics);
  default:
    return {};
  }
}

std::string renderNeonVectorIntrinsic(Intrinsic Id,
                                      const std::vector<std::string> &Ops,
                                      llvm::ArrayRef<uint16_t> OperandBytes,
                                      uint16_t ResultBytes,
                                      bool &HasCIntrinsics) {
  const char *CName = intrinsicCName(Id);
  if (!CName || OperandBytes.size() != Ops.size())
    return {};
  const llvm::StringRef Name(CName);
  const size_t Underscore = Name.rfind('_');
  if (!Name.starts_with("v") || Underscore == llvm::StringRef::npos)
    return {};
  const llvm::StringRef Element = Name.drop_front(Underscore + 1);
  if (Element.empty())
    return {};
  const char *Prefix = neonElementTypePrefix(Element.take_front(1));
  unsigned Bits = 0;
  if (!Prefix || Element.drop_front(1).getAsInteger(10, Bits) ||
      (Bits != 8 && Bits != 16 && Bits != 32 && Bits != 64))
    return {};
  // A `q` ends the name before the element of a 128-bit vector intrinsic.
  const uint16_t VectorBytes =
      Name.take_front(Underscore).ends_with("q") ? 16 : 8;
  if (ResultBytes != VectorBytes)
    return {};
  const std::string Vector = (llvm::Twine(Prefix) + llvm::Twine(Bits) + "x" +
                              llvm::Twine(VectorBytes * 8 / Bits) + "_t")
                                 .str();
  const std::string Raw = typeToC(NdType::makeInt(VectorBytes, false));
  std::string Call = std::string(CName) + "(";
  for (size_t I = 0; I < Ops.size(); ++I) {
    if (I > 0)
      Call += ", ";
    Call += OperandBytes[I] == VectorBytes
                ? neonValue(Vector.c_str(), Raw.c_str(), Ops[I])
                : Ops[I];
  }
  HasCIntrinsics = true;
  return "__builtin_bit_cast(" + Raw + ", " + Call + "))";
}

std::string renderARMAsmStatement(const char *Mnemonic,
                                  const std::vector<std::string> &Ops) {
  if (Ops.empty())
    return std::string("__asm__ volatile(\"") + Mnemonic + "\" ::: \"memory\")";
  std::string Inputs;
  for (size_t I = 0; I < Ops.size(); ++I) {
    if (I > 0)
      Inputs += ", ";
    Inputs += "\"r\"(" + Ops[I] + ")";
  }
  return std::string("__asm__ volatile(\"") + Mnemonic + "\" : : " + Inputs +
         " : \"memory\")";
}

namespace {
struct ScalarLSEFallback {
  Intrinsic Id;
  const char *Operation;
  bool Signed;
  bool Complement;
};

constexpr ScalarLSEFallback ScalarLSEFallbacks[] = {
#define NEVERD_SCALAR_LSE(ID, Operation, Signed, Complement)                   \
  {Intrinsic::ID, Operation, Signed, Complement},
#include "neverd/backend/c/render/AArch64/ScalarLSEFallbacks.def"
};

const ScalarLSEFallback *scalarLSEFallback(Intrinsic Id) {
  const auto *It = llvm::find_if(ScalarLSEFallbacks,
                                 [&](const auto &F) { return F.Id == Id; });
  return It == std::end(ScalarLSEFallbacks) ? nullptr : It;
}
} // namespace

bool isScalarLSEBuiltin(Intrinsic Id) { return scalarLSEFallback(Id); }

void writeScalarLSEFallbacks(llvm::raw_ostream &OS,
                             const std::set<Intrinsic> &Ids) {
  static constexpr unsigned Widths[] = {8, 16, 32, 64};
  bool Guarded = false;
  for (Intrinsic Id : Ids) {
    const ScalarLSEFallback *F = scalarLSEFallback(Id);
    const char *Builtin = intrinsicCName(Id);
    if (!F || !Builtin)
      continue;
    if (!Guarded) {
      OS << "#ifndef __has_builtin\n#define __has_builtin(x) 0\n#endif\n";
      Guarded = true;
    }
    const std::string Value = F->Complement ? "~(uint64_t)(value)" : "(value)";
    // One access per width the instruction has, picked by the constant
    // width operand; the old value returns zero-extended.
    auto Define = [&](auto Access) {
      OS << "#define " << Builtin << "(value, pointer, bytes, order) \\\n  (";
      for (unsigned Bits : Widths) {
        const std::string Width = std::to_string(Bits);
        const std::string Type = (F->Signed ? "int" : "uint") + Width + "_t";
        if (Bits != Widths[std::size(Widths) - 1])
          OS << "(bytes) == " << Bits / 8 << " ? ";
        OS << "(uint64_t)(uint" << Width << "_t)" << Access(Type)
           << (Bits != Widths[std::size(Widths) - 1] ? " : \\\n   " : ")\n");
      }
    };
    auto AtomicBuiltin = [&](const std::string &Type) {
      return std::string("__atomic_fetch_") + F->Operation + "((" + Type +
             " *)(pointer), (" + Type + ")" + Value + ", (order))";
    };
    const llvm::StringRef Operation = F->Operation;
    const bool Extremum = Operation == "max" || Operation == "min";
    OS << "#if !__has_builtin(" << Builtin << ")\n";
    if (Extremum)
      OS << "#if __has_builtin(__atomic_fetch_" << Operation << ")\n";
    Define(AtomicBuiltin);
    if (Extremum) {
      OS << "#else\n";
      Define([&](const std::string &Type) {
        return "__extension__({ " + Type + " *nd_lse_p = (" + Type +
               " *)(pointer); " + Type + " nd_lse_v = (" + Type + ")" + Value +
               "; " + Type +
               " nd_lse_old = __atomic_load_n(nd_lse_p, __ATOMIC_RELAXED); "
               "while (!__atomic_compare_exchange_n(nd_lse_p, &nd_lse_old, "
               "nd_lse_old " +
               (Operation == "max" ? ">" : "<") +
               " nd_lse_v ? nd_lse_old : nd_lse_v, 0, (order), "
               "__ATOMIC_RELAXED)) {} nd_lse_old; })";
      });
      OS << "#endif\n";
    }
    OS << "#endif\n";
  }
  if (Guarded)
    OS << "\n";
}

} // namespace neverd
