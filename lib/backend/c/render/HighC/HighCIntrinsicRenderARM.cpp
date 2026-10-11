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

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/HighC/HighCIntrinsicRender.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"

#include <iterator>

namespace neverd {

namespace {

bool isAlive(const MedVar &V, const IsAliveFn &Fn) { return !Fn || Fn(V); }

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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  };
  struct Width {
    uint16_t Bytes;
    const char *Letter;
    const char *Bits;
  };
  static constexpr Width Widths[] = {
#define NEVERD_NEON_SATURATING_WIDTH(Bytes, Letter, Bits) {Bytes, Letter, Bits},
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  };
  struct Width {
    uint16_t Bytes;
    const char *Letter;
    const char *Bits;
  };
  static constexpr Width Widths[] = {
#define NEVERD_NEON_SATURATING_WIDTH(Bytes, Letter, Bits) {Bytes, Letter, Bits},
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned ElementBytes = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 ||
      llvm::StringRef(Ops[2]).getAsInteger(10, ElementBytes))
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned ResultElement = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 ||
      llvm::StringRef(Ops[2]).getAsInteger(10, ResultElement))
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  };
  const auto *Op = llvm::find_if(
      Operations, [&](const Operation &Entry) { return Entry.Id == Id; });
  unsigned NarrowBytes = 0;
  if (Op == std::end(Operations) || Ops.size() != 3 || ResultBytes != 8 ||
      llvm::StringRef(Ops[2]).getAsInteger(10, NarrowBytes) ||
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
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_U32_OPERATION(ID, Base) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_SATURATING_OPERATION(ID, Base, Signed) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_SHIFT_OPERATION(ID, Base, Element) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_NARROW_SHIFT(ID, Base, SourceElement) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
#define NEVERD_NEON_DOUBLING_MULTIPLY(ID, Base, Widening) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
  case I::A64_Fcvtxn:
    return true;
  default:
    return false;
  }
}

llvm::SmallVector<const char *, 3> getARMIntrinsicHeaders() {
  return {"arm_acle.h", "arm_neon.h"};
}

std::string
renderARMMultiOutput(Intrinsic IID, const std::vector<MedVar> &Outputs,
                     const std::vector<ExprPtr> &Operands,
                     std::function<std::string(const HighExpr &)> ExprFn,
                     std::function<std::string(const MedVar &)> VarFn,
                     IsAliveFn IsAlive) {
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
  default:
    return {};
  }
}

std::string renderARMIntrinsicCall(Intrinsic Id,
                                   const std::vector<std::string> &Ops,
                                   uint16_t ResultBytes, bool &HasCIntrinsics) {
  using I = Intrinsic;
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
    if (Ops.size() < 2)
      return {};
    HasCIntrinsics = true;
    if (Ops[1] == "2")
      return "svptrue_b16()";
    if (Ops[1] == "4")
      return "svptrue_b32()";
    if (Ops[1] == "8")
      return "svptrue_b64()";
    return "svptrue_b8()";
  }
  case I::A64_SveDup: {
    if (Ops.size() < 2)
      return {};
    HasCIntrinsics = true;
    const char *Name = Ops[1] == "2"   ? "svdup_n_u16"
                       : Ops[1] == "4" ? "svdup_n_u32"
                       : Ops[1] == "8" ? "svdup_n_u64"
                                       : "svdup_n_u8";
    return std::string(Name) + "(" + Ops[0] + ")";
  }
  case I::A64_SveIndex: {
    if (Ops.size() < 3)
      return {};
    HasCIntrinsics = true;
    const char *Name = Ops[2] == "2"   ? "svindex_u16"
                       : Ops[2] == "4" ? "svindex_u32"
                       : Ops[2] == "8" ? "svindex_u64"
                                       : "svindex_u8";
    const char *Type = Ops[2] == "2"   ? "uint16_t"
                       : Ops[2] == "4" ? "uint32_t"
                       : Ops[2] == "8" ? "uint64_t"
                                       : "uint8_t";
    return std::string(Name) + "((" + Type + ")(" + Ops[0] + "), (" + Type +
           ")(" + Ops[1] + "))";
  }
  case I::A64_SveLastb: {
    if (Ops.size() < 3)
      return {};
    HasCIntrinsics = true;
    const char *Name = Ops[2] == "2"   ? "svlastb_u16"
                       : Ops[2] == "4" ? "svlastb_u32"
                       : Ops[2] == "8" ? "svlastb_u64"
                                       : "svlastb_u8";
    return std::string(Name) + "(" + Ops[0] + ", " + Ops[1] + ")";
  }
  case I::A64_SveSt1:
  case I::A64_SveStnt1: {
    if (Ops.size() < 5)
      return {};
    HasCIntrinsics = true;
    const std::string &LaneBytes = Ops[3];
    const std::string &StoreBytes = Ops[4];
    const char *LaneSuffix = LaneBytes == "2"   ? "16"
                             : LaneBytes == "4" ? "32"
                             : LaneBytes == "8" ? "64"
                                                : "8";
    const char *PtrType = StoreBytes == "2"   ? "uint16_t"
                          : StoreBytes == "4" ? "uint32_t"
                          : StoreBytes == "8" ? "uint64_t"
                                              : "uint8_t";
    std::string Name = Id == I::A64_SveStnt1 ? "svstnt1" : "svst1";
    if (Id == I::A64_SveSt1 && StoreBytes != LaneBytes) {
      Name += StoreBytes == "1" ? "b" : StoreBytes == "2" ? "h" : "w";
    }
    Name += "_u";
    Name += LaneSuffix;
    return Name + "(" + Ops[0] + ", (" + PtrType + " *)(uintptr_t)(" + Ops[2] +
           "), " + Ops[1] + ")";
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
    if (Ops.size() < 2)
      return {};
    HasCIntrinsics = true;
    return "__builtin_arm_pacga((uint64_t)(" + Ops[0] + "), (uint64_t)(" +
           Ops[1] + "))";
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
    const char *Type = nullptr;
    if (Ops[1] == "1")
      Type = "uint8_t";
    else if (Ops[1] == "2")
      Type = "uint16_t";
    else if (Ops[1] == "4")
      Type = "uint32_t";
    else if (Ops[1] == "8")
      Type = "uint64_t";
    else if (Ops[1] == "16")
      Type = "unsigned __int128";
    else
      return {};
    const bool Acquire = Id == I::A64_Ldaxr || Id == I::A64_Ldaxp;
    HasCIntrinsics = true;
    return std::string(Acquire ? "__builtin_arm_ldaex"
                               : "__builtin_arm_ldrex") +
           "((const volatile " + Type + " *)(uintptr_t)(" + Ops[0] + "))";
  }
  case I::A64_Stxr:
  case I::A64_Stlxr:
  case I::A64_Stxp:
  case I::A64_Stlxp: {
    if (Ops.size() < 3)
      return {};
    const char *Type = nullptr;
    if (Ops[2] == "1")
      Type = "uint8_t";
    else if (Ops[2] == "2")
      Type = "uint16_t";
    else if (Ops[2] == "4")
      Type = "uint32_t";
    else if (Ops[2] == "8")
      Type = "uint64_t";
    else if (Ops[2] == "16")
      Type = "unsigned __int128";
    else
      return {};
    const bool Release = Id == I::A64_Stlxr || Id == I::A64_Stlxp;
    HasCIntrinsics = true;
    return std::string(Release ? "__builtin_arm_stlex"
                               : "__builtin_arm_strex") +
           "((" + Type + ")(" + Ops[0] + "), (volatile " + Type +
           " *)(uintptr_t)(" + Ops[1] + "))";
  }
  case I::A64_Famax:
  case I::A64_Famin: {
    if (Ops.size() < 4)
      return {};
    const bool IsMax = Id == I::A64_Famax;
    const bool IsQ = Ops[2] == "16";
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
  case I::Dmb:
  case I::ArmDmb:
    return "__dmb(0xF)";
  case I::Dsb:
  case I::ArmDsb:
    return "__dsb(0xB)";
  case I::Isb:
  case I::ArmIsb:
    return "__isb(0xF)";
  case I::A64_Clrex:
  case I::ArmClrex:
    HasCIntrinsics = true;
    return "__builtin_arm_clrex()";
#define NEVERD_NEON_FLOAT_OPERATION(ID, Base, Operands) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonFloatOperation(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_U32_OPERATION(ID, Base) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonU32Estimate(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_SATURATING_OPERATION(ID, Base, Signed) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonSaturating(Id, Ops, ResultBytes, HasCIntrinsics);
  case I::A64_Fcvtxn:
    return renderNeonFcvtxn(Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_SHIFT_OPERATION(ID, Base, Element) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonShift(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_NARROW_SHIFT(ID, Base, SourceElement) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonNarrowShift(Id, Ops, ResultBytes, HasCIntrinsics);
#define NEVERD_NEON_DOUBLING_MULTIPLY(ID, Base, Widening) case I::ID:
#include "neverd/backend/c/render/HighC/NeonIntrinsicSpellings.def"
    return renderNeonDoublingMultiply(Id, Ops, ResultBytes, HasCIntrinsics);
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

} // namespace neverd
