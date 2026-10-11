//===- HighCExprWriter.cpp - HighIR expression rendering --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// General expression rendering for the HighIR C emitter.  Binary operator
/// rendering and precedence handling live in HighCExprBinOp.cpp.
///
//===----------------------------------------------------------------------===//

#include "../UnalignedMemory.h"
#include "../VariadicImportStub.h"
#include "HighCWriter.h"

#include "neverd/ArchSupport.h"
#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/high/HighSwiftErrorProjection.h"
#include "neverd/ir/high/X86ShadowStackShape.h"
#include "neverd/ir/intrinsics/X64Syscall.h"
#include "neverd/libc/LibCNames.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <cstdint>

namespace neverd {

namespace {
/// A load through an access pointer, `(*(_QWORD *)p)`, without its
/// parentheses where no unary or postfix operator applies to it: the
/// dereference is itself a unary expression.
std::string bareLoad(std::string Text, int ParentPrec) {
  constexpr int UnaryOperand = 99;
  return ParentPrec < UnaryOperand && llvm::StringRef(Text).starts_with("(*(")
             ? c_memory::unparenthesized(Text)
             : Text;
}

/// Whether C may compute an operation of float type \p Ty in a wider type
/// until a cast or an assignment rounds it (CFloatTypes.def).
bool computesWider(const TypeRef &Ty) {
  if (!Ty || Ty->Kind != NdTypeKind::Float)
    return false;
#define NEVERD_C_FLOAT_TYPE(Bytes, Spelling, FusedMultiplyAdd, ComputesWider)  \
  if (Ty->Size == Bytes)                                                       \
    return ComputesWider;
#include "neverd/backend/c/render/CFloatTypes.def"
  return false;
}

/// Whether \p E prints as a C arithmetic operator on floats, whose result
/// may keep excess precision into an enclosing one.
bool isFloatArithmetic(const HighExpr &E) {
  if (E.Kind == ExprKind::UnaryOp)
    return E.Op == NdOp::FLOAT_NEG;
  if (E.Kind != ExprKind::BinOp)
    return false;
  switch (E.Op) {
  case NdOp::FLOAT_ADD:
  case NdOp::FLOAT_SUB:
  case NdOp::FLOAT_MULT:
  case NdOp::FLOAT_DIV:
    return true;
  default:
    return false;
  }
}
} // namespace

std::string frameStorageAddress(int64_t Displacement) {
  if (Displacement == 0)
    return "frame_base";
  const uint64_t Magnitude =
      Displacement < 0 ? uint64_t{0} - static_cast<uint64_t>(Displacement)
                       : static_cast<uint64_t>(Displacement);
  return "(frame_base " + std::string(Displacement < 0 ? "- " : "+ ") +
         std::to_string(Magnitude) + ")";
}

std::string HighCWriter::debugNameForDisplacement(va_t Entry,
                                                  int64_t Disp) const {
  if (!Dbg)
    return {};
  auto Accept = [](const std::optional<VariableSym> &Var) -> std::string {
    if (!Var || Var->Name.empty() || Var->IsParam)
      return {};
    return Var->Name;
  };
  if (CurrentFunc && CurrentFunc->FrameSize > 0) {
    if (const std::string Name = Accept(Dbg->resolveStackPointerVariable(
            Entry, Disp + static_cast<int64_t>(CurrentFunc->FrameSize)));
        !Name.empty())
      return Name;
  }
  if (const std::string Name = Accept(Dbg->resolveVariable(Entry, Disp));
      !Name.empty())
    return Name;
  return {};
}

TypeRef HighCWriter::debugTypeForDisplacement(va_t Entry, int64_t Disp) const {
  if (!Dbg)
    return {};
  auto Accept = [](const std::optional<VariableSym> &Var) -> TypeRef {
    if (!Var || !Var->Type || Var->IsParam)
      return {};
    return Var->Type;
  };
  TypeRef Ty;
  if (CurrentFunc && CurrentFunc->FrameSize > 0)
    Ty = Accept(Dbg->resolveStackPointerVariable(
        Entry, Disp + static_cast<int64_t>(CurrentFunc->FrameSize)));
  if (!Ty)
    Ty = Accept(Dbg->resolveVariable(Entry, Disp));
  if (Ty)
    Dbg->completeType(Ty);
  // A local whose type C cannot spell keeps the type its accesses give it,
  // as a parameter keeps its machine type.
  TypeRef Display = cDisplayType(Ty);
  if (Display && !hasCSpelling(Display))
    return {};
  return Display;
}

std::string HighCWriter::varName(const MedVar &V) const {
  if (CurrentFunc &&
      isSyntheticEntryStackPointer(V, *CurrentFunc, Opts.TheArch))
    return "frame_base";
  if (V.RenameTag >= 0)
    return "v" + std::to_string(V.RenameTag);
  switch (V.Kind) {
  case MedVar::Stack:
    // After frame-slot collection, use the displacement's resolved name.
    // PDB frame-relative and stack-pointer-relative records can describe the
    // same local at different offsets when the prologue saved registers.
    if (auto It = FrameSlots.find(V.StackOff); It != FrameSlots.end())
      return It->second.Name;
    if (Dbg && CurrentFunc) {
      const int64_t Candidates[] = {V.StackOff, V.StackOff + 4, V.StackOff - 4};
      for (int64_t Off : Candidates) {
        if (const std::string Name =
                debugNameForDisplacement(CurrentFunc->Entry, Off);
            !Name.empty() && !isReservedParamDisplayName(Name))
          return Name;
      }
    }
    return "var_" + llvm::utohexstr(static_cast<uint64_t>(
                        V.StackOff < 0 ? -V.StackOff : V.StackOff));
  case MedVar::Param:
    // The debug signature names the parameter by where it arrives.
    if (Dbg && CurrentFunc && V.Id >= 0 && !CurrentFunc->SourceTypeHint)
      if (std::string Name =
              debugParamName(*CurrentFunc, static_cast<size_t>(V.Id));
          !Name.empty())
        return Name;
    if (auto It = ParamDisplayNames.find(V.Id); It != ParamDisplayNames.end())
      return It->second;
    if (CurrentFunc && V.Id >= 0 &&
        static_cast<size_t>(V.Id) < CurrentFunc->Params.size() &&
        !CurrentFunc->Params[static_cast<size_t>(V.Id)].Name.empty())
      return CurrentFunc->Params[static_cast<size_t>(V.Id)].Name;
    return "arg" + std::to_string(V.Id);
  case MedVar::RetVal:
    return "retval";
  case MedVar::EHException:
    return "eh_exception";
  case MedVar::EHSelector:
    return "eh_selector";
  case MedVar::SEHExceptionCode:
    return V.SSAVer <= 1 ? std::string("exception_code")
                         : "exception_code_" + std::to_string(V.SSAVer);
  case MedVar::Temp:
    return "t" + std::to_string(V.Id) +
           (V.SSAVer == 0 ? "" : "_" + std::to_string(V.SSAVer));
  default:
    if (V.Id < 0)
      return "v_" + std::to_string(static_cast<unsigned>(-V.Id)) + "_" +
             std::to_string(V.SSAVer);
    return "v" + std::to_string(V.Id) + "_" + std::to_string(V.SSAVer);
  }
}

namespace {
/// \p Text as the operand of a cast: in parentheses unless it is a unary
/// expression already, with no operator outside parentheses and quotes.
std::string castOperand(const std::string &Text) {
  int Depth = 0;
  bool Quoted = false;
  for (size_t I = 0; I < Text.size(); ++I) {
    const char Ch = Text[I];
    if (Quoted) {
      if (Ch == '\\')
        ++I;
      else if (Ch == '"')
        Quoted = false;
    } else if (Ch == '"') {
      Quoted = true;
    } else if (Ch == '(' || Ch == '[') {
      ++Depth;
    } else if (Ch == ')' || Ch == ']') {
      --Depth;
    } else if (Ch == ' ' && Depth == 0) {
      return "(" + Text + ")";
    }
  }
  return Text;
}
} // namespace

std::string HighCWriter::constStr(uint64_t Val, TypeRef Type) {
  // A narrow bit pattern is negative only in a signed integer type.
  // Wider masks must retain their zero upper bytes.
  if (Type && Type->Kind == NdTypeKind::Int && Type->Size && Type->Size < 8) {
    const unsigned Bits = Type->Size * 8;
    const uint64_t Mask = (UINT64_C(1) << Bits) - 1;
    Val &= Mask;
    if (Type->IsSigned && (Val & (UINT64_C(1) << (Bits - 1)))) {
      Val |= ~Mask;
      // A sign-extended hex word is unsigned in C and changes value when
      // implicitly narrowed. The signed value itself fits the declared type.
      return std::to_string(static_cast<int64_t>(Val));
    }
  }
  if (Val == 0)
    return "0";
  if (Val <= limits::kDecimalConstThreshold)
    return std::to_string(Val);

  if (Val == 0xFFFFFFFFFFFFFFFFULL)
    return "-1";

  int64_t SV = static_cast<int64_t>(Val);
  if (SV < 0 && SV >= -static_cast<int64_t>(limits::kDecimalConstThreshold))
    return std::to_string(SV);

  return "0x" + llvm::utohexstr(Val);
}

bool HighCWriter::declaredLocalInteger(const HighExpr &E, uint16_t Width,
                                       bool Signed) {
  if (E.Kind != ExprKind::Var && E.Kind != ExprKind::Phi)
    return false;
  const auto It = DeclaredCTypes.find(exprStr(E));
  return It != DeclaredCTypes.end() && It->second &&
         It->second->Kind == NdTypeKind::Int && !It->second->IsEnum &&
         It->second->IsSigned == Signed && It->second->Size == Width;
}

namespace {
/// A one- to eight-byte integer that typeToC prints as an exact-width type.
bool isPlainInteger(const TypeRef &T) {
  return T && T->Kind == NdTypeKind::Int && !T->IsEnum &&
         (T->Size == 1 || T->Size == 2 || T->Size == 4 || T->Size == 8);
}

} // anonymous namespace

bool printsIntegerArithmetic(const HighExpr &E) {
  if (E.Kind == ExprKind::UnaryOp)
    return E.Op == NdOp::INT_NEGATE || E.Op == NdOp::INT_NOT;
  if (E.Kind != ExprKind::BinOp)
    return false;
  switch (E.Op) {
  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return true;
  default:
    return false;
  }
}

bool printsTruthValue(const HighExpr &E) {
  if (E.Kind == ExprKind::UnaryOp)
    return E.Op == NdOp::BOOL_NOT;
  if (E.Kind != ExprKind::BinOp)
    return false;
  switch (E.Op) {
  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESS:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return true;
  default:
    return false;
  }
}

std::optional<std::pair<uint16_t, bool>>
HighCWriter::printedIntegerType(const HighExpr &E) const {
  const auto It = PrintedIntegerTypes.find(&E);
  if (It == PrintedIntegerTypes.end())
    return std::nullopt;
  return It->second;
}

std::string HighCWriter::typedText(const HighExpr &E, std::string Text,
                                   uint16_t Width, bool Signed) {
  if (Width == 1 || Width == 2 || Width == 4 || Width == 8)
    PrintedIntegerTypes[&E] = {Width, Signed};
  return Text;
}

std::string HighCWriter::signedCarrierResult(const HighExpr &E,
                                             std::string Value, int ValuePrec,
                                             uint16_t Size) {
  std::string Text =
      "__builtin_bit_cast(" + typeToC(E.Type) + ", " + Value + ")";
  UnsignedCarrierTexts[&E] = {std::move(Value), ValuePrec};
  if (!isPlainInteger(E.Type))
    return Text;
  return typedText(E, std::move(Text), Size, true);
}

std::optional<std::string>
HighCWriter::unsignedCarrierText(const HighExpr &E, int ParentPrec) const {
  const auto It = UnsignedCarrierTexts.find(&E);
  if (It == UnsignedCarrierTexts.end())
    return std::nullopt;
  const auto &[Text, Prec] = It->second;
  return Prec <= ParentPrec ? "(" + Text + ")" : Text;
}

const HighExpr &HighCWriter::lowBytesSource(const HighExpr &E,
                                            uint16_t Width) const {
  const HighExpr *Cur = &E;
  for (unsigned Peel = 0; Peel < limits::kMaxIntegerViewUnwrapDepth; ++Peel) {
    const bool Extension =
        Cur->Kind == ExprKind::UnaryOp &&
        (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT);
    const bool LowPart = Cur->Kind == ExprKind::BinOp &&
                         Cur->Op == NdOp::SUBBYTES &&
                         Cur->Operands.size() == 2 && Cur->Operands[1] &&
                         Cur->Operands[1]->Kind == ExprKind::Const &&
                         Cur->Operands[1]->ConstVal == 0;
    if ((Cur->Kind != ExprKind::Cast && Cur->Kind != ExprKind::BitCast &&
         !Extension && !LowPart) ||
        Cur->Operands.empty() || !Cur->Operands[0] || typedCallResult(Cur))
      break;
    const HighExpr &Source = *Cur->Operands[0];
    const TypeRef &To =
        Cur->Kind == ExprKind::Cast && Cur->CastTo ? Cur->CastTo : Cur->Type;
    // A conversion to at least Width bytes keeps the low Width bytes of its
    // source; an extension supplies the bytes above its source itself.
    if (!isPlainInteger(To) || !isPlainInteger(Source.Type) ||
        To->Size < Width || (Extension && Source.Type->Size < Width) ||
        Source.Kind == ExprKind::Call)
      break;
    Cur = &Source;
  }
  return *Cur;
}

bool HighCWriter::integerText(const HighExpr &E, llvm::StringRef Text) const {
  if (printedIntegerType(E))
    return true;
  if (E.Kind == ExprKind::Const) {
    llvm::StringRef Digits = Text;
    Digits.consume_front("-");
    const bool Hex = Digits.consume_front("0x");
    return !Digits.empty() && llvm::all_of(Digits, [Hex](char C) {
      return Hex ? llvm::isHexDigit(C) : llvm::isDigit(C);
    });
  }
  // The integer aliases of a memory access.
  if (E.Kind == ExprKind::Load)
    return c_memory::integerAccess(Text);
  return false;
}

std::string HighCWriter::integerView(const HighExpr &E, const TypeRef &To,
                                     int ParentPrec) {
  const uint16_t Width = To->Size;
  const bool Signed = To->IsSigned;
  const HighExpr *Source = &lowBytesSource(E, Width);
  std::string Text = exprStr(*Source, ParentPrec);
  if (Source != &E && !integerText(*Source, Text)) {
    Source = &E;
    Text = exprStr(E, ParentPrec);
  }
  if (const auto Printed = printedIntegerType(*Source);
      Printed && Printed->first == Width && Printed->second == Signed)
    return Text;
  // The unsigned carrier under a signed result has the same low bytes.
  if (Source->Type && Width <= Source->Type->Size)
    if (auto Carrier = unsignedCarrierText(*Source, ParentPrec)) {
      if (!Signed && Width == Source->Type->Size)
        return *Carrier;
      return "(" + typeToC(To) + ")" + c_memory::castOperand(*Carrier);
    }
  return "(" + typeToC(To) + ")" + c_memory::castOperand(Text);
}

std::optional<std::string>
HighCWriter::implicitIntegerConversion(const HighExpr &E, const TypeRef &To) {
  if (!isPlainInteger(To))
    return std::nullopt;
  const uint16_t Width = To->Size;
  const HighExpr &Source = lowBytesSource(E, Width);
  // A literal is spelled as the value it converts to.
  if (Source.Kind == ExprKind::Const && isPlainInteger(Source.Type)) {
    if (exprStr(Source) != constStr(Source.ConstVal, Source.Type))
      return std::nullopt;
    const unsigned Bits = Source.Type->Size * 8;
    uint64_t Value = Source.ConstVal;
    if (Bits < 64) {
      const uint64_t Mask = (uint64_t{1} << Bits) - 1;
      Value &= Mask;
      if (Source.Type->IsSigned && (Value >> (Bits - 1)))
        Value |= ~Mask;
    }
    return constStr(Value, To);
  }
  // Converting a narrower integer to To is its zero or sign extension.
  if (Source.Kind == ExprKind::UnaryOp &&
      (Source.Op == NdOp::INT_ZEXT || Source.Op == NdOp::INT_SEXT) &&
      !typedCallResult(&Source) && isPlainInteger(Source.Type) &&
      Source.Type->Size >= Width && !Source.Operands.empty() &&
      Source.Operands[0] && Source.Operands[0]->Kind != ExprKind::Const &&
      isPlainInteger(Source.Operands[0]->Type) &&
      Source.Operands[0]->Type->Size < Width)
    return integerView(*Source.Operands[0],
                       NdType::makeInt(Source.Operands[0]->Type->Size,
                                       Source.Op == NdOp::INT_SEXT),
                       0);
  std::string Text = exprStr(Source);
  if (!integerText(Source, Text))
    return std::nullopt;
  // C's conversion reads the carrier under a signed result just the same,
  // unless it widens.
  if (Source.Type && Width <= Source.Type->Size)
    if (auto Carrier = unsignedCarrierText(Source, 0))
      return *Carrier;
  return Text;
}

std::string HighCWriter::storedValueText(const HighExpr &Value,
                                         const TypeRef &To,
                                         NdMemoryOrdering Ordering) {
  // A plain store converts the value to the memory's type like an
  // assignment does.
  if (Ordering == NdMemoryOrdering::None && Value.Type &&
      Value.Type->Kind == NdTypeKind::Int)
    if (auto Converted = implicitIntegerConversion(Value, To))
      return *Converted;
  return exprStr(Value);
}

std::string HighCWriter::renderUnaryOp(const HighExpr &E, int ParentPrec) {
  if (E.Operands.empty())
    return "/* bad unary */";

  switch (E.Op) {
  case NdOp::INT_NOT:
  case NdOp::INT_NEGATE:
    // C promotes byte/word complements to int. Keep the machine result width
    // even when it is subsequently widened or substituted for a stored value.
    if (E.Type && E.Type->Kind == NdTypeKind::Int &&
        (E.Type->Size == 1 || E.Type->Size == 2))
      return "((" + typeToC(E.Type) + ")(~" + exprStr(*E.Operands[0], 99) +
             "))";
    return "~" + exprStr(*E.Operands[0], 99);
  case NdOp::INT_NEG2: {
    // Reuse the integer subtraction rule for -x, including signed-minimum
    // wrapping and narrow promotions. Concatenating '-' with a negative
    // constant would also accidentally spell C's decrement token.
    auto Negation = HighExpr::makeBinop(
        NdOp::INT_SUB, HighExpr::makeConst(0, E.Type ? E.Type->Size : 8),
        E.Operands[0]);
    Negation->Type = E.Type;
    return renderBinOp(*Negation, ParentPrec);
  }
  case NdOp::BOOL_NOT: {
    const HighExpr *Inner = forwardedExpr(E.Operands[0].get());
    auto IsZero = [&](const ExprPtr &Op) {
      const HighExpr *Cur = unwrapIntegerView(Op.get());
      return Cur && Cur->Kind == ExprKind::Const && Cur->ConstVal == 0;
    };
    auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
      auto Peel = [&](const HighExpr *E) {
        unsigned Depth = 0;
        while (E && Depth++ < 6) {
          const HighExpr *Fwd = forwardedExpr(E);
          if (Fwd && (Fwd->Kind == ExprKind::Var || Fwd->Kind == ExprKind::Phi))
            E = Fwd;
          E = unwrapIntegerView(E);
          if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2)
            return E;
          if (E->Op == NdOp::INT_EQUAL || E->Op == NdOp::INT_NOTEQUAL ||
              E->Op == NdOp::INT_LESS || E->Op == NdOp::INT_LESSEQUAL ||
              E->Op == NdOp::INT_SLESS || E->Op == NdOp::INT_SLESSEQUAL)
            return E;
          if (IsZero(E->Operands[1]) && E->Operands[0]) {
            E = E->Operands[0].get();
            continue;
          }
          if (IsZero(E->Operands[0]) && E->Operands[1]) {
            E = E->Operands[1].get();
            continue;
          }
          return E;
        }
        return E;
      };
      A = Peel(A);
      B = Peel(B);
      return A && B && (A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) &&
             A->Kind == B->Kind && A->Var.Kind == B->Var.Kind &&
             A->Var.Id == B->Var.Id;
    };
    auto AsEqZero = [&](const HighExpr *Op, ExprPtr &X) {
      Op = forwardedExpr(Op);
      Op = unwrapIntegerView(Op);
      if (!Op || Op->Kind != ExprKind::BinOp || Op->Op != NdOp::INT_EQUAL ||
          Op->Operands.size() != 2)
        return false;
      if (IsZero(Op->Operands[1]) && Op->Operands[0]) {
        X = Op->Operands[0];
        return true;
      }
      if (IsZero(Op->Operands[0]) && Op->Operands[1]) {
        X = Op->Operands[1];
        return true;
      }
      return false;
    };
    auto AsSignedLtZero = [&](const HighExpr *Op, ExprPtr &X) {
      Op = forwardedExpr(Op);
      Op = unwrapIntegerView(Op);
      if (Op && Op->Kind == ExprKind::BinOp && Op->Op == NdOp::INT_NOTEQUAL &&
          Op->Operands.size() == 2) {
        const HighExpr *A = unwrapIntegerView(Op->Operands[0].get());
        const HighExpr *B = unwrapIntegerView(Op->Operands[1].get());
        if (A && A->Kind == ExprKind::BinOp && A->Op == NdOp::INT_SLESS &&
            IsZero(Op->Operands[1]))
          Op = A;
        else if (B && B->Kind == ExprKind::BinOp && B->Op == NdOp::INT_SLESS &&
                 IsZero(Op->Operands[0]))
          Op = B;
      }
      if (!Op || Op->Kind != ExprKind::BinOp || Op->Op != NdOp::INT_SLESS ||
          Op->Operands.size() != 2 || !IsZero(Op->Operands[1]) ||
          !Op->Operands[0])
        return false;
      X = Op->Operands[0];
      return true;
    };
    auto PrintGtZero = [&](const ExprPtr &X) {
      const HighExpr *V = forwardedExpr(X.get());
      V = unwrapIntegerView(V);
      if (V && V->Kind == ExprKind::BinOp && V->Operands.size() == 2 &&
          V->Op != NdOp::INT_EQUAL && V->Op != NdOp::INT_NOTEQUAL &&
          V->Op != NdOp::INT_SLESS && V->Op != NdOp::INT_SLESSEQUAL &&
          IsZero(V->Operands[1]) && V->Operands[0])
        V = unwrapIntegerView(V->Operands[0].get());
      const uint16_t Sz = V && V->Type && V->Type->Size && V->Type->Size <= 4
                              ? V->Type->Size
                              : 4;
      std::string LHS;
      const HighExpr *Print = V ? V : X.get();
      if (const HighExpr *Call = typedCallResult(Print)) {
        TypeRef Ret = knownCallReturnType(*Call);
        if (!Ret)
          Ret = Call->Type;
        if (Ret && Ret->Kind == NdTypeKind::Int && Ret->Size == Sz)
          Print = Call;
      }
      if (Print->Type && Print->Type->Kind == NdTypeKind::Int &&
          Print->Type->Size == Sz && Print->Type->IsSigned)
        LHS = exprStr(*Print, 7);
      else if (const HighExpr *Call = typedCallResult(Print);
               Call && Call == Print)
        LHS = exprStr(*Print, 7);
      else
        LHS = "(" + typeToC(NdType::makeInt(Sz, true)) + ")" +
              exprStr(*Print, 99);
      std::string Result = LHS + " > 0";
      if (ParentPrec >= 7)
        Result = "(" + Result + ")";
      return Result;
    };
    if (Inner && Inner->Kind == ExprKind::BinOp &&
        (Inner->Op == NdOp::BOOL_OR || Inner->Op == NdOp::INT_OR) &&
        Inner->Operands.size() == 2) {
      ExprPtr XEq;
      ExprPtr XLt;
      const bool LeftEq = AsEqZero(Inner->Operands[0].get(), XEq) &&
                          AsSignedLtZero(Inner->Operands[1].get(), XLt);
      const bool RightEq = AsEqZero(Inner->Operands[1].get(), XEq) &&
                           AsSignedLtZero(Inner->Operands[0].get(), XLt);
      if ((LeftEq || RightEq) && SameScalar(XEq.get(), XLt.get()))
        return PrintGtZero(XEq);
    }
    if (Inner && Inner->Kind == ExprKind::BinOp &&
        Inner->Op == NdOp::INT_SLESSEQUAL && Inner->Operands.size() == 2 &&
        IsZero(Inner->Operands[1]) && Inner->Operands[0])
      return PrintGtZero(Inner->Operands[0]);
    if (Inner)
      return invertCondStr(*Inner);
    return "!" + exprStr(*E.Operands[0], 99);
  }
  case NdOp::INT_ZEXT: {
    if (const HighExpr *Call = typedCallResult(&E)) {
      TypeRef Printed =
          Call->SourceCallHint ? Call->Type : knownCallReturnType(*Call);
      if (!Printed)
        Printed = Call->Type;
      if (Printed && Printed->Kind == NdTypeKind::Int && !Printed->IsSigned &&
          !E.Operands.empty() && E.Operands[0] && E.Operands[0]->Type &&
          Printed->Size == E.Operands[0]->Type->Size)
        return exprStr(*Call, ParentPrec);
    }
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const)
      return exprStr(Inner, ParentPrec);
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    if (isPlainInteger(E.Type) && printsTruthValue(Inner))
      return typedText(E, "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99),
                       E.Type->Size, E.Type->IsSigned);
    // Zero extension converts the source's unsigned view.
    if (isPlainInteger(Inner.Type) && isPlainInteger(E.Type))
      return typedText(
          E,
          "(" + typeToC(E.Type) + ")" +
              integerView(Inner, NdType::makeInt(Inner.Type->Size, false), 99),
          E.Type->Size, E.Type->IsSigned);
    if (Inner.Type)
      return "(" + typeToC(E.Type) + ")(" +
             typeToC(NdType::makeInt(Inner.Type->Size, false)) + ")" +
             exprStr(Inner, 99);
    return "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99);
  }
  case NdOp::INT_SEXT: {
    if (const HighExpr *Call = typedCallResult(&E)) {
      TypeRef Printed =
          Call->SourceCallHint ? Call->Type : knownCallReturnType(*Call);
      if (!Printed)
        Printed = Call->Type;
      if (Printed && Printed->Kind == NdTypeKind::Int && Printed->IsSigned &&
          !E.Operands.empty() && E.Operands[0] && E.Operands[0]->Type &&
          Printed->Size == E.Operands[0]->Type->Size)
        return exprStr(*Call, ParentPrec);
    }
    auto &Inner = *E.Operands[0];
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    if (isPlainInteger(E.Type) && printsTruthValue(Inner))
      return typedText(E, "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99),
                       E.Type->Size, E.Type->IsSigned);
    // Sign extension converts the source's signed view.
    if (isPlainInteger(Inner.Type) && isPlainInteger(E.Type))
      return typedText(
          E,
          "(" + typeToC(E.Type) + ")" +
              integerView(Inner, NdType::makeInt(Inner.Type->Size, true), 99),
          E.Type->Size, E.Type->IsSigned);
    return "(" + typeToC(E.Type) + ")(" +
           (Inner.Type ? typeToC(NdType::makeInt(Inner.Type->Size, true))
                       : "int32_t") +
           ")" + exprStr(Inner, 99);
  }
  case NdOp::FLOAT_TRUNC:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT: {
    const auto Shape = floatToIntegerConversion(E);
    if (!Shape)
      throw std::runtime_error("missing HighC float conversion shape");
    auto It = FloatToIntegerHelpers.find(Shape->key());
    if (It == FloatToIntegerHelpers.end())
      throw std::runtime_error("HighC float conversion was not collected");
    std::string Call = It->second + "(" + exprStr(*E.Operands[0]) + ")";
    return E.Type->IsSigned
               ? "__builtin_bit_cast(" + typeToC(E.Type) + ", " + Call + ")"
               : Call;
  }
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT: {
    // The machine counts in the operand's own width; C's builtins count in
    // unsigned int or unsigned long long.  The operand converts to that
    // type through its unsigned view, so no sign extension adds ones, and
    // a narrower operand's extra leading zeros are taken off.
    const HighExpr &Operand = *E.Operands[0];
    const unsigned Bits = countedBits(Operand);
    const unsigned Width = countedBytes(Operand) * 8u;
    if (!Bits)
      throw std::runtime_error("HighC cannot count the bits of a " +
                               std::to_string(Width) + "-bit value");
    const std::string Value =
        integerView(Operand, NdType::makeInt(Width / 8, false), 0);
    std::string Text;
    if (E.Op == NdOp::POPCOUNT)
      Text = (Bits == 32 ? "__builtin_popcount(" : "__builtin_popcountll(") +
             Value + ")";
    else {
      const auto Helper = LeadingZeroHelpers.find(Bits);
      if (Helper == LeadingZeroHelpers.end() || Helper->second.empty())
        throw std::runtime_error("HighC leading-zero count was not collected");
      Text = Helper->second + "(" + Value + ")";
      if (Width < Bits)
        Text = "(" + Text + " - " + std::to_string(Bits - Width) + ")";
    }
    return typedText(E, std::move(Text), sizeof(int32_t), true);
  }
  case NdOp::FLOAT_NEG:
    return "-" + floatOperandStr(*E.Operands[0], 99);
  case NdOp::FLOAT_ABS:
  case NdOp::FLOAT_SQRT:
  case NdOp::FLOAT_CEIL:
  case NdOp::FLOAT_FLOOR:
  case NdOp::FLOAT_ROUND:
  case NdOp::FLOAT_ROUNDEVEN: {
    // A `long double` computes through the `l` builtins; the x87 rounds and
    // takes roots with its own instructions, which no C library need link.
    const bool Extended = isX87Value(*E.Operands[0]);
    if (const auto Helper = x87HelperFor(E))
      return useX87Helper(*Helper) + "(" + exprStr(*E.Operands[0]) + ")";
    const char *Name = E.Op == NdOp::FLOAT_ABS     ? "__builtin_fabs"
                       : E.Op == NdOp::FLOAT_SQRT  ? "__builtin_sqrt"
                       : E.Op == NdOp::FLOAT_CEIL  ? "__builtin_ceil"
                       : E.Op == NdOp::FLOAT_FLOOR ? "__builtin_floor"
                       : E.Op == NdOp::FLOAT_ROUND ? "__builtin_round"
                                                   : "__builtin_nearbyint";
    return std::string(Name) + (Extended ? "l(" : "(") +
           exprStr(*E.Operands[0]) + ")";
  }
  case NdOp::FLOAT_ISNAN:
    return "__builtin_isnan(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2FLOAT:
    return "(" + typeToC(E.Type) + ")" + exprStr(*E.Operands[0], 99);
  default:
    return "/* unary " + std::to_string(static_cast<int>(E.Op)) + " */ " +
           exprStr(*E.Operands[0]);
  }
}

bool HighCWriter::isX87Value(const HighExpr &E) {
  return E.Type && E.Type->Kind == NdTypeKind::Float && E.Type->Size == 10;
}

std::optional<X87CHelper> HighCWriter::x87HelperFor(const HighExpr &E) const {
  // The control word the function reads before setting it is the unit's.
  if (E.Kind == ExprKind::Var && E.Var.SSAVer == 0 &&
      isX87ControlWord(Opts.TheArch, E.Var))
    return X87CHelper::ControlWord;
  if (E.Kind == ExprKind::UnaryOp &&
      (E.Op == NdOp::FLOAT_ROUNDEVEN || E.Op == NdOp::FLOAT_SQRT) &&
      !E.Operands.empty() && E.Operands[0] && isX87Value(*E.Operands[0]))
    return E.Op == NdOp::FLOAT_SQRT ? X87CHelper::Fsqrt : X87CHelper::Frndint;
  if (E.Kind != ExprKind::Call)
    return std::nullopt;
  if (E.IntrinsicId == Intrinsic::X87Fprem ||
      E.IntrinsicId == Intrinsic::X87Fprem1 ||
      E.IntrinsicId == Intrinsic::X87ReadStatus)
    return X87CHelper::Fprem;
  return x87ValueHelper(E.IntrinsicId);
}

std::string HighCWriter::useX87Helper(X87CHelper Helper) const {
  if (!X87Helpers.count(Helper))
    throw std::runtime_error("HighC x87 helper was not collected");
  return x87CHelperName(Helper);
}

/// A callee whose name renders to the identifier of a different function
/// defined in this unit gets its own identifier: bound to that definition,
/// the call would run the wrong code (`SMKM_STORE_MGR<...>::SmPageRead` and
/// `SmPageRead` are both `SmPageRead`).
std::string HighCWriter::callIdentifier(const HighExpr &E) const {
  std::string Name = functionIdentifier(resolvedCallTarget(E));
  if (!E.CallAddr || E.CallAddr == InvalidVA ||
      E.IntrinsicId != Intrinsic::None)
    return Name;
  auto It = DefinedFunctionsByIdentifier.find(Name);
  if (It == DefinedFunctionsByIdentifier.end() || !It->second ||
      !It->second->Entry)
    return Name;
  if (It->second->Entry == E.CallAddr) {
    // A variadic import's stub (`fprintf: jmp [__imp_fprintf]`) has no C
    // signature that passes `...` on, so its definition takes none of the
    // call's arguments; the call goes through the slot it jumps through.
    if (isVariadicImportStub(*It->second))
      if (std::string Slot = importSlotIdentifier(E); !Slot.empty()) {
        auto Declared = ExternalFunctionIdentifiers.find(Slot);
        return Declared == ExternalFunctionIdentifiers.end() ? Slot
                                                             : Declared->second;
      }
    return Name;
  }
  // A thunk named like the import it jumps to (`calloc: jmp [__imp_calloc]`)
  // calls through the import's slot, as IDA prints it, by the identifier its
  // declaration holds.
  if (std::string Slot = importSlotIdentifier(E); !Slot.empty()) {
    auto Declared = ExternalFunctionIdentifiers.find(Slot);
    return Declared == ExternalFunctionIdentifiers.end() ? Slot
                                                         : Declared->second;
  }
  return Name + "_" + llvm::utohexstr(E.CallAddr);
}

bool HighCWriter::isVariadicImportStub(const HighFunc &Func) const {
  return Opts.Image &&
         !c_stub::variadicImportOfStub(*Opts.Image, Func.Entry).empty();
}

const HighFunc *HighCWriter::calledDefinition(const HighExpr &E) const {
  if (E.IntrinsicId != Intrinsic::None || E.CallTarget.empty())
    return nullptr;
  auto It = DefinedFuncs.find(E.CallTarget);
  if (It == DefinedFuncs.end() || !It->second)
    return nullptr;
  return functionIdentifier(*It->second) == callIdentifier(E) ? It->second
                                                              : nullptr;
}

std::string HighCWriter::importSlotIdentifier(const HighExpr &E) const {
  if (!Opts.Image || !E.CallAddr)
    return {};
  // The call goes through an import's slot, or to a stub that jumps through
  // it, which the linker names on formats that name slots.
  const Import *Imp = Opts.Image->findImportAt(E.CallAddr);
  const llvm::StringRef Prefix = importSlotPrefix(Opts.Format);
  if (!Imp || !Imp->IATAddr || Imp->Name.empty() || Prefix.empty())
    return {};
  // The image's own symbol for the slot names it as the program linked it:
  // MinGW binds msvcrt's `__getmainargs` to `__imp____msvcrt_getmainargs`,
  // and its `__imp____getmainargs` points to its own wrapper.  Otherwise the
  // slot is the prefix and the import's symbol, `__imp_calloc` or
  // `__imp___initterm` on 32-bit Windows, unless that names another object.
  // C spells it without the format's underscore.
  std::string SlotSymbol;
  for (const Symbol &Sym : Opts.Image->Symbols)
    if (Sym.Addr == Imp->IATAddr &&
        llvm::StringRef(Sym.Name).starts_with(Prefix)) {
      SlotSymbol = Sym.Name;
      break;
    }
  if (SlotSymbol.empty()) {
    SlotSymbol =
        (Prefix + symbolOfImportName(Imp->Name, Opts.Format, Opts.TheArch))
            .str();
    if (const Symbol *Other = Opts.Image->findSymbol(SlotSymbol);
        Other && Other->Addr != Imp->IATAddr)
      return {};
  }
  std::string Identifier =
      cNameOfSymbol(SlotSymbol, Opts.Format, Opts.TheArch).str();
  for (char &Ch : Identifier)
    if (!isCProjectionIdentifierByte(static_cast<unsigned char>(Ch)))
      Ch = '_';
  return Identifier;
}

std::string HighCWriter::importDataSlotIdentifier(va_t Addr) const {
  if (!Opts.Image)
    return {};
  const Import *Imp = Opts.Image->findImportAt(Addr);
  if (!Imp || Imp->IATAddr != Addr)
    return {};
  HighExpr Slot;
  Slot.CallAddr = Addr;
  return importSlotIdentifier(Slot);
}

std::optional<va_t> HighCWriter::importDataSlotRead(const HighExpr &Address,
                                                    uint16_t Size) const {
  if (!Opts.Image || Size == 0 || Size != Opts.Image->getPointerSize())
    return std::nullopt;
  std::optional<va_t> Slot = constAddress(Address);
  if (!Slot) {
    const HighExpr *Pointer = unwrapIntegerView(&Address);
    if (Pointer && Pointer->Kind == ExprKind::Load &&
        Pointer->MemoryOrdering == NdMemoryOrdering::None &&
        Pointer->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        Pointer->Operands.size() == 1 && Pointer->Operands[0])
      if (const auto Holder = constAddress(*Pointer->Operands[0]))
        Slot = foldReadonlyScalar(*Holder, Size);
  }
  if (!Slot || importDataSlotIdentifier(*Slot).empty())
    return std::nullopt;
  return Slot;
}

std::string HighCWriter::resolvedCallTarget(const HighExpr &E) const {
  std::string Name = E.CallTarget;
  if (Name.empty() && E.CallAddr)
    Name = (kAutoFuncPrefix + llvm::utohexstr(E.CallAddr)).str();
  // Every name here is a symbol, but an import entry may name its function
  // by the C name (SymbolDecorations.def): spell that as the symbol it links
  // as, or the format's underscore would come off a C name (`_initterm`).
  auto Spelled = [&](std::string Symbol) {
    if (Opts.Image && E.CallAddr && Symbol == E.CallTarget)
      if (const Import *Imp = Opts.Image->findImportAt(E.CallAddr);
          Imp && Imp->Name == Symbol)
        return symbolOfImportName(Symbol, Opts.Format, Opts.TheArch);
    return Symbol;
  };
  auto imageFunctionName = [&]() -> std::string {
    if (!Opts.Image || !E.CallAddr)
      return {};
    if (!Name.empty() && !isSynthesizedFuncName(Name))
      return {};
    std::string FromImage = Opts.Image->getFunctionNameAt(E.CallAddr);
    if (FromImage.empty() || isSynthesizedFuncName(FromImage))
      return {};
    return FromImage;
  };
  if (!Dbg) {
    if (std::string FromImage = imageFunctionName(); !FromImage.empty())
      return FromImage;
    return Spelled(Name);
  }
  std::string DebugName;
  const bool OrdinalName = llvm::StringRef(Name).starts_with(kOrdinalPrefix);
  // jmp [IAT] / cleanup thunks keep the call-site VA, not the import slot.
  // resolveFunction(site) then names the enclosing pdata function. Look the
  // ordinal up on the image first.
  if (OrdinalName && Opts.Image) {
    va_t Slot = 0;
    if (E.CallAddr) {
      if (const Import *Imp = Opts.Image->findImportAt(E.CallAddr);
          Imp && Imp->IATAddr)
        Slot = Imp->IATAddr;
    }
    if (!Slot) {
      for (const Import &Imp : Opts.Image->Imports) {
        if (Imp.Name == Name && Imp.IATAddr) {
          Slot = Imp.IATAddr;
          break;
        }
      }
    }
    if (Slot) {
      if (auto Data = Dbg->resolveDataObject(Slot); Data && !Data->Name.empty())
        DebugName = Data->Name;
      else if (auto FS = Dbg->resolveFunction(Slot); FS && !FS->Name.empty())
        DebugName = FS->Name;
    }
  }
  if (DebugName.empty() && E.CallAddr) {
    if (auto FS = Dbg->resolveFunction(E.CallAddr); FS && !FS->Name.empty())
      DebugName = FS->Name;
    else if (auto Data = Dbg->resolveDataObject(E.CallAddr);
             Data && !Data->Name.empty())
      DebugName = Data->Name;
  }
  if (DebugName.empty()) {
    if (std::string FromImage = imageFunctionName(); !FromImage.empty())
      return FromImage;
    return Spelled(Name);
  }
  if (Name.empty() || llvm::StringRef(Name).starts_with(kAutoFuncPrefix) ||
      OrdinalName || llvm::StringRef(Name).starts_with("__imp_") ||
      llvm::StringRef(Name).starts_with("_imp_"))
    return DebugName;
  return Spelled(Name);
}

std::string HighCWriter::renderCallExpr(const HighExpr &E) {
  if (E.IntrinsicId == Intrinsic::CetRdSsp) {
    const auto Shape = x86ShadowStackReadHighShape(E, Opts.TheArch);
    if (!x86ShadowStackReadShapeIsValid(Shape))
      llvm::report_fatal_error("invalid HighC shadow stack read contract");
    const std::string Temp = MemoryIdentifiers.allocate("nd_ssp_value");
    const std::string Type =
        "uint" + std::to_string(Shape.OutputSize * 8) + "_t";
    const std::string Op = Shape.ReadWidth == 8    ? "rdsspq %0"
                           : Shape.OutputSize == 8 ? "rdsspd %k0"
                                                   : "rdsspd %0";
    const std::string Result =
        E.Type->Kind == NdTypeKind::Ptr
            ? "(" + typeToC(E.Type) + ")(uintptr_t)" + Temp
            : Temp;
    // A helper CALL would change SSP. The tied full-width local preserves the
    // old high half when RDSSPD is a NOP, and evaluates the input exactly once.
    return "({ " + Type + " " + Temp + " = (" + Type + ")(" +
           exprStr(*E.Operands[0]) + "); __asm__ volatile(\"" + Op +
           "\" : \"+r\"(" + Temp + ") :: \"memory\"); " + Result + "; })";
  }
  if (E.SourceCallHint)
    return renderSourceCallExpr(E);
  if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default &&
      !isX86FPStateMemoryIntrinsic(E.IntrinsicId))
    llvm::report_fatal_error(
        "HighC cannot safely render a segmented-memory intrinsic");
  std::string Name = resolvedCallTarget(E);

  if (E.IntrinsicId != Intrinsic::None) {
    if (const auto Bits = x86SaturatingLaneBits(E)) {
      const auto Helper = SaturatingLaneHelpers.find({E.IntrinsicId, *Bits});
      if (Helper == SaturatingLaneHelpers.end())
        llvm::report_fatal_error("uncollected x86 saturating lane helper");
      return renderX86SaturatingLane(
          E, Helper->second,
          [this](const HighExpr &Expr) { return exprStr(Expr); });
    }
    auto Typed = renderX86TypedIntrinsicCall(
        Opts.TheArch, E, [this](const HighExpr &Expr) { return exprStr(Expr); },
        HasCIntrinsics, Opts.Format != BinaryFormat::COFF,
        [this](Intrinsic Id, unsigned Bytes) {
          const auto It = X86FPStateHelpers.find({Id, Bytes});
          if (It == X86FPStateHelpers.end())
            llvm::report_fatal_error("uncollected x86 FP state helper");
          return It->second;
        });
    if (!Typed.empty())
      return Typed;

    std::optional<unsigned> LinuxSyscallArgs;
    if (E.IntrinsicId == Intrinsic::X64Syscall && E.Operands.size() == 5 &&
        E.Operands[0]) {
      const HighExpr *Number = unwrapIntegerView(E.Operands[0].get());
      if (Number && Number->Kind == ExprKind::Const)
        LinuxSyscallArgs = linuxX64SyscallArgumentCount(Number->ConstVal);
    }
    // An unconsumed register read is not part of the syscall's behavior. The
    // helper has a fixed six-register signature, so fill those positions with
    // zero only when the omitted expression cannot fault or have side effects.
    auto CanOmit = [&](auto &&Self, const HighExpr *Op,
                       unsigned Depth) -> bool {
      if (!Op || Depth > limits::kMaxIntegerViewUnwrapDepth)
        return false;
      Op = forwardedExpr(Op);
      if (!Op)
        return false;
      switch (Op->Kind) {
      case ExprKind::Load:
      case ExprKind::Store:
      case ExprKind::Call:
        return false;
      default:
        break;
      }
      for (const auto &Child : Op->Operands)
        if (Child && !Self(Self, Child.get(), Depth + 1))
          return false;
      return true;
    };
    std::vector<std::string> OpStrs;
    std::vector<uint16_t> OpBytes;
    for (size_t I = 0; I < E.Operands.size(); ++I) {
      const auto &Op = E.Operands[I];
      if (!Op)
        continue;
      OpBytes.push_back(Op->Type ? Op->Type->Size : 0);
      if (LinuxSyscallArgs == 1 && I >= 2 && CanOmit(CanOmit, Op.get(), 0)) {
        OpStrs.push_back("0");
        continue;
      }
      OpStrs.push_back(exprStr(*Op));
    }

    using I = Intrinsic;
    if (E.IntrinsicId == I::A64_GetFPSR || E.IntrinsicId == I::A64_SetFPSR)
      OpStrs.insert(OpStrs.begin(), "\"FPSR\"");
    if (E.IntrinsicId == I::A64_GetFPCR || E.IntrinsicId == I::A64_SetFPCR)
      OpStrs.insert(OpStrs.begin(), "\"FPCR\"");

    bool IsFixedIntToFP = E.IntrinsicId == I::A64_ScvtfFixed ||
                          E.IntrinsicId == I::A64_UcvtfFixed;
    bool IsFixedFPToInt = E.IntrinsicId == I::A64_FcvtzsFixed ||
                          E.IntrinsicId == I::A64_FcvtzuFixed;
    if ((IsFixedIntToFP || IsFixedFPToInt) && OpStrs.size() == 2) {
      uint16_t GPRBytes = 0;
      if (IsFixedIntToFP && !E.Operands.empty() && E.Operands[0] &&
          E.Operands[0]->Type)
        GPRBytes = E.Operands[0]->Type->Size;
      else if (IsFixedFPToInt && E.Type)
        GPRBytes = E.Type->Size;

      if (GPRBytes == 4 || GPRBytes == 8)
        OpStrs.push_back(GPRBytes == 8 ? "1" : "0");
    }

    // Operands added for the intrinsic's spelling have no width.
    if (OpBytes.size() != OpStrs.size())
      OpBytes.clear();
    auto Rendered = renderIntrinsicCall(
        E.IntrinsicId, Opts.TheArch, OpStrs, E.Type ? E.Type->Size : 0,
        HasCIntrinsics, Opts.Format != BinaryFormat::COFF, OpBytes);
    if (!Rendered.empty())
      return Rendered;
  }

  // The identifier the declarations use: a callee named like a different
  // definition takes its own, and does not run that definition.
  if (E.IntrinsicId == Intrinsic::None)
    Name = callIdentifier(E);
  // A call through a parameter is named after it, but the parameter is only
  // callable in C when it is typed as a function pointer.
  if (E.IsIndirectCall && E.IndirectTarget &&
      (Name.empty() || Name == "indirect" || Name == "indirect_call" ||
       E.IndirectParamIdx >= 0))
    Name = indirectCalleeStr(*E.IndirectTarget, E.Type);

  // A callee defined in this file has a prototype: convert between pointer
  // and integer arguments the way the machine passed them, in the register.
  const HighFunc *Defined = calledDefinition(E);
  // Its printed signature also fixes how many arguments the call passes: a
  // value past its parameters is not read by it, and a parameter the call
  // site did not determine is an unknown value.
  const size_t ArgCount = Defined && !Defined->SourceTypeHint
                              ? emittedParamCount(*Defined)
                              : E.Operands.size();
  std::string S = Name + "(";
  const auto Callee = debugCallee(E);
  const libc::LibCPrototype *Prototype = calleePrototype(E);
  // A routine known only by its argument counts: the call collects its
  // integer arguments first, while a libm helper such as ldexp declares its
  // floating ones first.
  const std::optional<libc::LibCArity> Arity =
      !Prototype && !Defined && E.IntrinsicId == Intrinsic::None
          ? libc::libcArityForSymbol(Name)
          : std::nullopt;
  const size_t ArityIntegers = Arity ? std::max(0, Arity->IntArgs) : 0;
  const size_t ArityFloats = Arity ? std::max(0, Arity->FpArgs) : 0;
  auto OperandIndex = [&](size_t I) {
    if (!Arity || !Arity->FpFirst || I >= ArityIntegers + ArityFloats)
      return I;
    return I < ArityFloats ? ArityIntegers + I : I - ArityFloats;
  };
  const llvm::StringRef HeaderCallee = headerDeclaredCallee(E);
  const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format);
  // A callee printed in this file fixes the count by its signature; otherwise
  // debug info and MSVC member knowledge may trim ABI-only extra operands.
  size_t PrintedArgs =
      Defined && !Defined->SourceTypeHint ? ArgCount : debugCallArgLimit(E);
  // So does a known external function's declaration.
  if (const auto Declared = plainDeclarationArity(E))
    PrintedArgs = std::max(PrintedArgs, *Declared);
  // The string an argument points to reads beside it, before the comma.
  std::string Note;
  for (size_t I = 0; I < PrintedArgs; ++I) {
    S += Note;
    if (I > 0)
      S += ", ";
    ExprPtr Missing;
    const size_t Source = OperandIndex(I);
    const HighExpr *Op =
        Source < E.Operands.size() ? E.Operands[Source].get() : nullptr;
    if (!Op) {
      // Keep absent required operands on the same failure and type-conversion
      // path as explicit unknowns, including pointer parameters.
      Missing = HighExpr::makeUndef(8);
      Op = Missing.get();
    }
    Note = stringArgumentNote(*Op);
    if (const HighExpr *Imm = unwrapIntegerView(Op)) {
      if ((Imm->Kind == ExprKind::Var || Imm->Kind == ExprKind::Phi)) {
        const std::string Fwd = copyForwardName(varName(Imm->Var));
        if (auto It = ValueForward.find(Fwd);
            It != ValueForward.end() && It->second &&
            It->second->Kind == ExprKind::Const)
          Imm = It->second;
      }
      if (Imm->Kind == ExprKind::Const) {
        if (TypeRef EnumTy = enumTypeForCallArg(E, I, Imm->ConstVal)) {
          if (auto Name = enumeratorDisplay(EnumTy, Imm->ConstVal)) {
            S += *Name;
            continue;
          }
        }
      }
    }
    if (Callee) {
      // A callee this file defines takes what its prototype declares.
      if (const TypeRef Expected = Defined
                                       ? emittedParamType(*Defined, I)
                                       : expectedDebugCallArgType(*Callee, I)) {
        S += exprStrAsTypedArg(*Op, Expected);
        continue;
      }
    }
    if (Msvc) {
      if (const TypeRef Expected = I == 0 ? msvcSyntheticThis(Name, *Msvc)
                                          : msvcExpectedCallArgType(*Msvc, I)) {
        S += exprStrAsTypedArg(*Op, Expected);
        continue;
      }
    }
    if (const HighExpr *SlotAddr = unwrapIntegerView(Op)) {
      if (auto Slot = namedFrameSlot(*SlotAddr)) {
        S += "&" + *Slot;
        continue;
      }
      if (SlotAddr->Kind == ExprKind::Addr) {
        S += exprStr(*SlotAddr);
        continue;
      }
    }
    // Untyped call immediates keep the ABI widening in HighIR
    // (`(int64_t)(uint32_t)1`). Print the constant; sanitizer wrapping
    // stays on returns, not on these call-site immediates. A ValueForwarded
    // readonly load (`t = *0x...`) still prints as that immediate.
    auto Immediate = [&](auto &&Self, const HighExpr &Arg,
                         unsigned Depth) -> const HighExpr * {
      if (Depth > limits::kMaxIntegerViewUnwrapDepth)
        return nullptr;
      const HighExpr *Inner = unwrapIntegerView(&Arg);
      if (!Inner)
        return nullptr;
      if (Inner->Kind == ExprKind::Const)
        return Inner;
      if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
        const std::string Name = copyForwardName(varName(Inner->Var));
        if (auto Fwd = ValueForward.find(Name);
            Fwd != ValueForward.end() && Fwd->second)
          return Self(Self, *Fwd->second, Depth + 1);
      }
      if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
          Inner->Operands[0]) {
        if (auto VA = constAddress(*Inner->Operands[0])) {
          const uint16_t Size = Inner->Type ? Inner->Type->Size : 0;
          if (foldReadonlyScalar(*VA, Size))
            return Inner;
        }
      }
      return nullptr;
    };
    const HighExpr *Imm = Immediate(Immediate, *Op, 0);
    // A string object or a function passes as the pointer it is in C where
    // the parameter takes one.
    const HighExpr *PointerArgument = nullptr;
    if (const auto Object = stringObjectArgument(*Op)) {
      const ImageObject &String = *Object->second;
      const ImageCString &Text =
          String.String ? *String.String : *String.PointsTo;
      if (takesPointerArgument(E, I, Text.UnitBytes == 1) &&
          PointerArgumentOperands.insert(Object->first).second)
        PointerArgument = Object->first;
    } else if (const HighExpr *Function = functionAddressArgument(*Op);
               Function && takesPointerArgument(E, I, false) &&
               PointerArgumentOperands.insert(Function).second) {
      PointerArgument = Function;
    }
    // A floating parameter of the callee's prototype takes the bits.
    TypeRef FloatParam = Defined && I < Defined->Params.size()
                             ? emittedParamType(*Defined, I)
                             : nullptr;
    if (!FloatParam && Prototype && I < Prototype->ParamCount) {
      if (Prototype->Params[I] == "double")
        FloatParam = NdType::makeFloat(sizeof(double));
      else if (Prototype->Params[I] == "float")
        FloatParam = NdType::makeFloat(sizeof(float));
    }
    // A routine known by its argument counts takes its floating arguments
    // after its integer ones, as the call collects them.
    if (!FloatParam && Arity && Source >= ArityIntegers &&
        Source < ArityIntegers + ArityFloats)
      FloatParam =
          NdType::makeFloat(Arity->FpIsFloat ? sizeof(float) : sizeof(double));
    if (const auto Float = floatArgumentText(*Op, FloatParam)) {
      if (PointerArgument)
        PointerArgumentOperands.erase(PointerArgument);
      S += *Float;
      continue;
    }
    std::string Arg = exprStr(Imm ? *Imm : *Op);
    if (PointerArgument)
      PointerArgumentOperands.erase(PointerArgument);
    // A signed result of at least an int's width passes the same register
    // bits as its unsigned carrier, unless a prototype widens it.
    const TypeRef DefinedParam = Defined && I < Defined->Params.size()
                                     ? emittedParamType(*Defined, I)
                                     : nullptr;
    if (!Imm && !PointerArgument && Op->Type && Op->Type->Size >= 4 &&
        (!DefinedParam || (DefinedParam->Kind == NdTypeKind::Int &&
                           DefinedParam->Size <= Op->Type->Size)))
      if (auto Carrier = unsignedCarrierText(*Op, 0))
        Arg = *Carrier;
    // An integer narrower than a pointer widens before it converts to one, as
    // does the int an unknown value prints as.
    const bool WidensToPointer =
        !PointerArgument && Op->Type &&
        ((Op->Type->Kind == NdTypeKind::Int &&
          Op->Type->Size < pointerBytes(Opts.TheArch)) ||
         isUnknownCallOperand(Op));
    // A routine with a known prototype takes its pointer parameters through
    // casts of the values the machine passed; C converts its integer
    // parameters itself.
    if (Prototype && I < Prototype->ParamCount) {
      const std::string Param = prototypeType(Prototype->Params[I]);
      const HighExpr *Value = unwrapIntegerView(Op);
      const bool Null =
          Value && Value->Kind == ExprKind::Const && Value->ConstVal == 0;
      const bool Converts =
          !Arg.empty() && Arg.front() == '"' && libc::takesStringLiteral(Param);
      if (libc::isPointerType(Param)) {
        // An address its integer view carries passes as the pointer it is.
        llvm::StringRef Address(Arg);
        if (Address.consume_front("(uintptr_t)") && Address.starts_with("&") &&
            castOperand(Address.str()) == Address)
          Arg = Param == "const void *" ? Address.str()
                                        : "(" + Param + ")" + Address.str();
        else if (!Null && !Converts)
          Arg = "(" + Param + ")" + (WidensToPointer ? "(uintptr_t)" : "") +
                castOperand(Arg);
      } else if (Op->Type && Op->Type->Kind == NdTypeKind::Ptr) {
        Arg = "(" + Param + ")(uintptr_t)" + castOperand(Arg);
      }
    }
    // A standard function's function-pointer parameter takes the value
    // converted to the type its header declares: a recovered function's
    // signature is not the one the parameter names.
    if (!HeaderCallee.empty())
      if (const auto Type = libc::functionPointerParameter(
              HeaderCallee, static_cast<unsigned>(I))) {
        const HighExpr *Value = unwrapIntegerView(Op);
        if (!Value || Value->Kind != ExprKind::Const || Value->ConstVal != 0)
          Arg = "(" + std::string(*Type) + ")" +
                (WidensToPointer ? "(uintptr_t)" : "") + castOperand(Arg);
      }
    // Its object pointer parameter takes a machine integer converted to a
    // pointer, which C does not do itself; any object pointer converts on.
    if (!HeaderCallee.empty() && !PointerArgument &&
        libc::isObjectPointerParameter(HeaderCallee,
                                       static_cast<unsigned>(I)) &&
        printsAsInteger(Imm ? *Imm : *Op, Arg))
      Arg = "(void *)(uintptr_t)" + castOperand(Arg);
    // A callee printed in this file has a prototype: convert between pointer
    // and integer arguments the way the machine passed them, in a register.
    if (Defined && I < Defined->Params.size() && DefinedParam && Op->Type) {
      const TypeRef &Param = DefinedParam;
      const bool ParamPtr = Param->Kind == NdTypeKind::Ptr;
      if (ParamPtr)
        if (const auto Pointer = declaredPointerName(*Op, Arg))
          Arg = *Pointer;
      // A string or another pointer the machine passes as an integer still
      // converts to an integer parameter.
      const bool ArgPtr = Op->Type->Kind == NdTypeKind::Ptr ||
                          (!ParamPtr && printsAsPointer(Arg));
      if (ParamPtr != ArgPtr && (ParamPtr ? Op->Type->Kind == NdTypeKind::Int
                                          : Param->Kind == NdTypeKind::Int))
        Arg = "(" + typeToC(Param) + ")(uintptr_t)(" + Arg + ")";
    }
    S += Arg;
  }
  S += Note;
  S += ")";
  return S;
}

std::optional<std::pair<const HighExpr *, const HighCWriter::ImageObject *>>
HighCWriter::stringObjectArgument(const HighExpr &Arg) {
  // What the argument prints as: through the values it forwards.
  const HighExpr *Cur = &Arg;
  for (unsigned Depth = 0;
       Cur && Depth <= limits::kMaxIntegerViewUnwrapDepth &&
       (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi);
       ++Depth) {
    const auto Forward = ValueForward.find(copyForwardName(varName(Cur->Var)));
    if (Forward == ValueForward.end() || !Forward->second)
      return std::nullopt;
    Cur = Forward->second;
  }
  if (!Cur)
    return std::nullopt;
  std::optional<va_t> Addr;
  if (Cur->Kind == ExprKind::Const)
    Addr = Cur->ConstVal;
  else if (Cur->Kind == ExprKind::Load &&
           Cur->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           Cur->MemoryOrdering == NdMemoryOrdering::None &&
           Cur->Operands.size() == 1 && Cur->Operands[0])
    Addr = constAddress(*Cur->Operands[0]);
  if (!Addr || imageBackingAddress(*Addr))
    return std::nullopt;
  const auto Object = ImageObjects.find(*Addr);
  if (Object == ImageObjects.end() ||
      !(Cur->Kind == ExprKind::Const ? Object->second.String.has_value()
                                     : Object->second.PointsTo.has_value()))
    return std::nullopt;
  return std::make_pair(Cur, &Object->second);
}

const HighExpr *HighCWriter::functionAddressArgument(const HighExpr &Arg) {
  const HighExpr *Cur = &Arg;
  for (unsigned Depth = 0;
       Cur && Depth <= limits::kMaxIntegerViewUnwrapDepth &&
       (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi);
       ++Depth) {
    const auto Forward = ValueForward.find(copyForwardName(varName(Cur->Var)));
    if (Forward == ValueForward.end() || !Forward->second)
      return nullptr;
    Cur = Forward->second;
  }
  return Cur && Cur->Kind == ExprKind::Const &&
                 isAddressProvenance(Cur->ConstProvenance) &&
                 FunctionAddressNames.count(Cur->ConstVal)
             ? Cur
             : nullptr;
}

const libc::LibCPrototype *
HighCWriter::calleePrototype(const HighExpr &E) const {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None ||
      E.IsIndirectCall || E.CallTarget.empty() || calledDefinition(E) ||
      debugCallee(E))
    return nullptr;
  const std::string Name = callIdentifier(E);
  if (SourceNativeSignatures.count(Name) ||
      ConflictingSourceNativeSignatures.count(Name) ||
      DebugExternSigs.count(Name) || msvcCallee(Name, Opts.Format))
    return nullptr;
  return prototypeForSymbol(resolvedCallTarget(E));
}

llvm::StringRef HighCWriter::headerDeclaredCallee(const HighExpr &E) const {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None ||
      E.IsIndirectCall || E.CallTarget.empty() || calledDefinition(E) ||
      debugCallee(E))
    return {};
  const std::string Name = callIdentifier(E);
  if (SourceNativeSignatures.count(Name) ||
      ConflictingSourceNativeSignatures.count(Name) ||
      DebugExternSigs.count(Name) || msvcCallee(Name, Opts.Format))
    return {};
  llvm::StringRef Symbol = E.CallTarget;
  if (!libc::isKnownFunction(Symbol.str()) && Symbol.starts_with("_") &&
      libc::isKnownFunction(Symbol.drop_front().str()))
    Symbol = Symbol.drop_front();
  return libc::isKnownFunction(Symbol.str()) ? Symbol : llvm::StringRef();
}

const libc::LibCPrototype *
HighCWriter::prototypeForSymbol(llvm::StringRef Symbol) const {
  if (const libc::LibCPrototype *Prototype =
          libc::libcPrototype(Symbol, Opts.Format))
    return Prototype;
  const llvm::StringRef CName =
      cNameOfSymbol(Symbol, Opts.Format, Opts.TheArch);
  return CName != Symbol ? libc::libcPrototype(CName, Opts.Format) : nullptr;
}

const libc::LibCPrototype *
HighCWriter::externalPrototype(const std::string &Name) const {
  const auto Sources = ExternalCallSources.find(Name);
  return prototypeForSymbol(Sources != ExternalCallSources.end() &&
                                    Sources->second.size() == 1
                                ? llvm::StringRef(*Sources->second.begin())
                                : llvm::StringRef(Name));
}

std::string HighCWriter::prototypeType(std::string_view Type) const {
  std::string Text(Type);
  const size_t At = Text.find(libc::kWinapiMarker);
  if (At == std::string::npos)
    return Text;
  return Text.replace(At, libc::kWinapiMarker.size(),
                      Opts.TheArch == Arch::X86 ? "__attribute__((stdcall)) "
                                                : "");
}

bool HighCWriter::takesPointerArgument(const HighExpr &E, size_t Index,
                                       bool NarrowString) const {
  // A prototype printed in this file, from debug information or from a
  // source signature converts its arguments itself.
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None ||
      E.IsIndirectCall || E.CallTarget.empty() || calledDefinition(E) ||
      debugCallee(E))
    return false;
  const std::string Name = callIdentifier(E);
  if (SourceNativeSignatures.count(Name) ||
      ConflictingSourceNativeSignatures.count(Name) ||
      DebugExternSigs.count(Name) || msvcCallee(Name, Opts.Format))
    return false;
  // A known prototype's pointer parameter takes a pointer, cast to its type,
  // and its variadic arguments any pointer.
  if (const libc::LibCPrototype *Prototype = calleePrototype(E))
    return Index < Prototype->ParamCount
               ? libc::isPointerType(Prototype->Params[Index])
               : Prototype->Variadic;
  // A known C function is declared by its header: a variadic argument takes
  // any pointer, a function-pointer parameter a function and a string
  // parameter a narrow string.
  if (const llvm::StringRef Symbol = headerDeclaredCallee(E); !Symbol.empty()) {
    if (const unsigned Fixed = libc::varArgFixedCount(Symbol.str());
        Fixed && Index >= Fixed)
      return true;
    if (libc::functionPointerParameter(Symbol, static_cast<unsigned>(Index)))
      return true;
    return NarrowString &&
           libc::isCStringParameter(Symbol.str(), static_cast<unsigned>(Index));
  }
  // This file declares any other function `int f()`, unless a known arity
  // gives it integer parameters.
  return !plainDeclarationArity(E);
}

std::string HighCWriter::stringArgumentNote(const HighExpr &Arg) {
  if (!Opts.Image || !Opts.EmitComments)
    return {};
  // What the argument prints as: through integer views and forwarded values.
  const HighExpr *Inner = unwrapIntegerView(&Arg);
  for (unsigned Depth = 0;
       Inner && Depth <= limits::kMaxIntegerViewUnwrapDepth &&
       (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi);
       ++Depth) {
    const auto Forward =
        ValueForward.find(copyForwardName(varName(Inner->Var)));
    if (Forward == ValueForward.end() || !Forward->second)
      return {};
    Inner = unwrapIntegerView(Forward->second);
  }
  if (!Inner)
    return {};
  std::optional<va_t> Target;
  if (Inner->Kind == ExprKind::Const) {
    // A number is no address, and a literal shows its text itself.
    if (Inner->ConstProvenance == ConstantAddressProvenance::Scalar ||
        Inner->ConstProvenance == ConstantAddressProvenance::AddressFragment ||
        imageStringLiteral(Opts.Image, Inner->ConstVal))
      return {};
    Target = Inner->ConstVal;
  } else if (Inner->Kind == ExprKind::Load && Inner->Operands.size() == 1 &&
             Inner->Operands[0]) {
    // A pointer the image holds, as the loader left it (`u8s`).
    const uint32_t PointerSize = Opts.Image->getPointerSize();
    if (const auto Slot = constAddress(*Inner->Operands[0]);
        Slot && PointerSize && Inner->Type && Inner->Type->Size == PointerSize)
      if (const uint8_t *Bytes = Opts.Image->readVA(*Slot, PointerSize))
        Target = PointerSize == 8 ? readLE<uint64_t>(Bytes)
                                  : readLE<uint32_t>(Bytes);
  }
  if (!Target)
    return {};
  if (auto Text = imageStringComment(Opts.Image, *Target))
    return " /* " + *Text + " */";
  return {};
}

std::optional<FunctionSym> HighCWriter::debugCallee(const HighExpr &E) const {
  if (!Dbg)
    return std::nullopt;
  if (Opts.Image && llvm::StringRef(E.CallTarget).starts_with(kOrdinalPrefix)) {
    va_t Slot = 0;
    if (E.CallAddr) {
      if (const Import *Imp = Opts.Image->findImportAt(E.CallAddr);
          Imp && Imp->IATAddr)
        Slot = Imp->IATAddr;
    }
    if (!Slot) {
      for (const Import &Imp : Opts.Image->Imports) {
        if (Imp.Name == E.CallTarget && Imp.IATAddr) {
          Slot = Imp.IATAddr;
          break;
        }
      }
    }
    if (Slot) {
      if (auto FS = Dbg->resolveFunction(Slot); FS && !FS->Name.empty())
        return FS;
    }
  }
  if (E.CallAddr)
    if (auto FS = Dbg->resolveFunction(E.CallAddr); FS) {
      if (!FS->Params.empty())
        return FS;
      const std::string Name = callIdentifier(E);
      if (auto It = DebugExternSigs.find(Name);
          It != DebugExternSigs.end() && !It->second.Params.empty())
        return It->second;
      return FS;
    }
  const std::string Name = callIdentifier(E);
  if (auto It = DebugExternSigs.find(Name); It != DebugExternSigs.end())
    return It->second;
  return std::nullopt;
}

void HighCWriter::collectUnknownOnlyNames(const HighFunc &Func) {
  UnknownOnlyNames.clear();
  AssignedNames.clear();
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call =
        S.Kind == StmtKind::Call ? S.CallExpr.get() : S.Val.get();
    if (Call && Call->Kind == ExprKind::Call)
      for (const MedVar &Output : Call->IntrinsicOutputs) {
        const std::string Name = varName(Output);
        if (!Name.empty())
          AssignedNames.insert(Name);
      }
    if (S.Kind != StmtKind::Assign || !S.Dst)
      return;
    if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
      return;
    const std::string Name = varName(S.Dst->Var);
    if (!Name.empty())
      AssignedNames.insert(Name);
  });
  // Each `__except` arm assigns GetExceptionCode() to its handler's name.
  for (const auto &[HandlerVA, Name] : SEHExceptionCodeNames) {
    (void)HandlerVA;
    AssignedNames.insert(Name);
  }
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 8) {
    Changed = false;
    std::set<std::string> HasUnknown;
    std::set<std::string> HasKnown;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      // Machine register reuse does not redefine the incoming source
      // parameter; statement rendering omits the same synthetic copy.
      if (isIncomingParamReuseAssign(S))
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      const std::string Name = varName(S.Dst->Var);
      if (Name.empty())
        return;
      if (isUnknownCallOperand(S.Val.get()))
        HasUnknown.insert(Name);
      else
        HasKnown.insert(Name);
    });
    std::set<std::string> Next;
    for (const std::string &Name : HasUnknown)
      if (!HasKnown.count(Name))
        Next.insert(Name);
    if (Next != UnknownOnlyNames) {
      UnknownOnlyNames = std::move(Next);
      Changed = true;
    }
  }
  collectCtorSourceNames(Func);
}

void HighCWriter::collectCtorSourceNames(const HighFunc &Func) {
  CtorSourceNames.clear();
  bool Changed = true;
  unsigned Guard = 0;
  while (Changed && Guard++ < 8) {
    Changed = false;
    walkStmts(Func.Body, [&](const HighStmt &S) {
      if (S.Kind != StmtKind::Assign || !S.Dst || !S.Val)
        return;
      if (S.Dst->Kind != ExprKind::Var && S.Dst->Kind != ExprKind::Phi)
        return;
      const std::string Name = varName(S.Dst->Var);
      if (Name.empty() || CtorSourceNames.count(Name))
        return;
      if (isCtorSourceExpr(S.Val.get())) {
        CtorSourceNames.insert(Name);
        Changed = true;
      }
    });
  }
}

TypeRef HighCWriter::knownCallReturnType(const HighExpr &E) const {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None)
    return {};
  const std::string Name = callIdentifier(E);
  if (const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format))
    return msvcSyntheticReturn(Msvc->ReturnKind);
  const auto Defined = DefinedFunctionsByIdentifier.find(Name);
  // A bound source declaration determines the definition's printed return
  // type, even when optional debug information disagrees.
  if (Defined != DefinedFunctionsByIdentifier.end() && Defined->second &&
      Defined->second->SourceTypeHint)
    if (const auto Return = declaredFunctionReturnType(*Defined->second))
      return Return;
  if (const auto Callee = debugCallee(E)) {
    // A result whose type C cannot spell is the register it arrives in.
    if (!hasCSpelling(cDisplayType(Callee->ReturnType)))
      return {};
    return Callee->ReturnType;
  }
  // A function this unit defines returns the type its definition prints.
  if (Defined != DefinedFunctionsByIdentifier.end() && Defined->second)
    return Defined->second->ReturnType;
  // A C library routine returns the floating type its declaration names,
  // or a math routine the one its arguments have.
  if (const libc::LibCPrototype *Prototype = calleePrototype(E)) {
    if (const uint16_t Bytes = libc::floatReturnBytes(*Prototype))
      return NdType::makeFloat(Bytes);
    return {};
  }
  if (const auto Arity = libc::libcArityForSymbol(Name))
    if (const uint16_t Bytes = libc::floatReturnBytes(*Arity))
      return NdType::makeFloat(Bytes);
  if (E.Type && E.Type->Kind == NdTypeKind::Float)
    return E.Type;
  return {};
}

bool HighCWriter::knownVoidCall(const HighExpr &E) const {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None)
    return false;
  // A function this unit defines returns what its definition declares: a
  // bound source signature decides over debug information.
  if (const HighFunc *Definition = calledDefinition(E))
    if (const TypeRef Declared = declaredFunctionReturnType(*Definition))
      return Declared->Kind == NdTypeKind::Void;
  // Other special-member rows are declared `void` too, but an MSVC
  // constructor or assignment returns `this`, and so does an ARM32 Itanium
  // destructor: a read of their result is real.
  if (const MsvcCallee *Msvc = msvcCallee(callIdentifier(E), Opts.Format))
    return Msvc->Kind == MsvcCalleeKind::Dtor &&
           isMsvcDestructorName(resolvedCallTarget(E));
  if (const auto Callee = debugCallee(E))
    return Callee->ReturnType && Callee->ReturnType->Kind == NdTypeKind::Void;
  const libc::LibCPrototype *Prototype = calleePrototype(E);
  return Prototype && Prototype->Return == "void";
}

std::string HighCWriter::intrinsicOperandStr(const HighExpr &E) {
  const HighExpr *P = peelIntegerViewOps(&E);
  if (!P)
    return exprStr(E);
  if (P->Kind == ExprKind::Var || P->Kind == ExprKind::Phi ||
      P->Kind == ExprKind::Const || P->Kind == ExprKind::Load)
    return exprStr(*P);
  return exprStr(E);
}

const HighExpr *HighCWriter::peelIntegerViewOps(const HighExpr *E) const {
  const HighExpr *Inner = unwrapIntegerView(E);
  unsigned Peel = 0;
  while (Inner && Peel++ < limits::kMaxIntegerViewUnwrapDepth) {
    if (Inner->Kind == ExprKind::BinOp && Inner->Op == NdOp::SUBBYTES &&
        Inner->Operands.size() == 2 && Inner->Operands[0] &&
        Inner->Operands[1] && Inner->Operands[1]->Kind == ExprKind::Const &&
        Inner->Operands[1]->ConstVal == 0) {
      Inner = unwrapIntegerView(Inner->Operands[0].get());
      continue;
    }
    const HighExpr *Next = unwrapIntegerView(Inner);
    if (Next && Next != Inner) {
      Inner = Next;
      continue;
    }
    break;
  }
  return Inner;
}

std::string HighCWriter::addrStr(const HighExpr &E, int ParentPrec,
                                 bool ProjectImageBacking) {
  if (ProjectImageBacking && ProjectFrameAliasesIntoStorage)
    if (const auto Disp = certifiedFrameStorageDisplacement(E))
      return frameStorageAddress(*Disp);
  if (auto VA = constAddress(E)) {
    if (!ProjectImageBacking)
      return constStr(*VA, E.Type);
    if (auto Backing = imageBackingAddress(*VA))
      return *Backing;
  }
  const HighExpr *Inner = peelIntegerViewOps(&E);
  if (!Inner)
    Inner = &E;
  if (Inner->Kind == ExprKind::BinOp &&
      (Inner->Op == NdOp::INT_ADD || Inner->Op == NdOp::INT_SUB) &&
      Inner->Operands.size() == 2 && Inner->Operands[0] && Inner->Operands[1]) {
    const HighExpr *Lhs = peelIntegerViewOps(Inner->Operands[0].get());
    const HighExpr *Rhs = peelIntegerViewOps(Inner->Operands[1].get());
    if (!Lhs)
      Lhs = Inner->Operands[0].get();
    if (!Rhs)
      Rhs = Inner->Operands[1].get();
    const HighExpr *Base = nullptr;
    const HighExpr *Off = nullptr;
    const bool Minus = Inner->Op == NdOp::INT_SUB;
    if (Rhs->Kind == ExprKind::Const) {
      Base = Lhs;
      Off = Rhs;
    } else if (!Minus && Lhs->Kind == ExprKind::Const) {
      Base = Rhs;
      Off = Lhs;
    }
    if (Base && Off && Off->Kind == ExprKind::Const) {
      if (ProjectImageBacking) {
        if (auto Member = typedMemberAddress(*Inner))
          return "&" + *Member;
        if (auto Slot = namedFrameSlot(*Inner))
          return "&" + *Slot;
        // The constant is the table and the rest the offset into it.
        if (Base->Kind != ExprKind::Const && indexedImageBase(*Inner) == Off)
          if (auto Table = imageBackingAddress(Off->ConstVal)) {
            constexpr int AddPrec = 9;
            std::string S =
                "(uintptr_t)" + *Table + " + " +
                integerView(
                    *Base,
                    NdType::makeInt(getTargetRegInfo(Opts.TheArch).PointerSize,
                                    false),
                    AddPrec);
            return ParentPrec >= AddPrec ? "(" + S + ")" : S;
          }
      }
      constexpr int AddPrec = 9;
      std::string B = addrStr(*Base, AddPrec, ProjectImageBacking);
      TypeRef BaseTy = Base->Type;
      if ((Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi))
        if (auto Declared = declaredParamType(Base->Var))
          BaseTy = Declared;
      const bool BytePtr = BaseTy && BaseTy->Kind == NdTypeKind::Ptr &&
                           BaseTy->Pointee &&
                           BaseTy->Pointee->Kind == NdTypeKind::Int &&
                           BaseTy->Pointee->Size == 1;
      // addrStr gives a pointer variable as the pointer itself.  A void or
      // byte pointer offsets by bytes (GNU C for void *), so the access needs
      // no integer round trip: `*(_QWORD *)(p + 8)`; another pointer type
      // takes its integer view for a byte offset.
      if (ProjectImageBacking && isBarePointerName(*Base, B)) {
        if (BytePtr || pointsToVoid(declaredTypeOf(*Base, B))) {
          std::string S = B + (Minus ? " - " : " + ") + constStr(Off->ConstVal);
          return ParentPrec >= AddPrec ? "(" + S + ")" : S;
        }
        B = "(uintptr_t)" + B;
      }
      // Byte offsets from an unsigned pointer-width integer already wrap
      // like the machine address: the frame base, or a local or parameter
      // declared that way.
      const uint16_t PointerBytes = getTargetRegInfo(Opts.TheArch).PointerSize;
      const auto Declared = DeclaredCTypes.find(B);
      const bool UnsignedWord =
          B == "frame_base" ||
          (Declared != DeclaredCTypes.end() && Declared->second &&
           Declared->second->Kind == NdTypeKind::Int &&
           !Declared->second->IsSigned && !Declared->second->IsEnum &&
           Declared->second->Size == PointerBytes) ||
          ((Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi) &&
           Base->Var.Kind == MedVar::Param && BaseTy &&
           BaseTy->Kind == NdTypeKind::Int && !BaseTy->IsSigned &&
           !BaseTy->IsEnum && BaseTy->Size == PointerBytes &&
           B == varName(Base->Var));
      if (!BytePtr && !UnsignedWord &&
          !llvm::StringRef(B).starts_with("(uintptr_t)"))
        B = "(uintptr_t)(" + B + ")";
      std::string S = B + (Minus ? " - " : " + ") + constStr(Off->ConstVal);
      if (ParentPrec >= AddPrec)
        return "(" + S + ")";
      return S;
    }
  }
  if (!ProjectImageBacking) {
    // The generic value renderer may turn an image constant nested in an
    // unfamiliar offset expression into a host pointer. Reject that shape
    // instead of silently changing the FS/GS numeric offset.
    const auto ContainsImageConstant = [this](const HighExpr &Value,
                                              const auto &Self) -> bool {
      if (Value.Kind == ExprKind::Const)
        return isImageDataAddress(Value.ConstVal);
      if (Value.Kind == ExprKind::Load || Value.Kind == ExprKind::Store ||
          Value.Kind == ExprKind::Call)
        return false;
      for (const auto &Operand : Value.Operands)
        if (Operand && Self(*Operand, Self))
          return true;
      return false;
    };
    if (ContainsImageConstant(*Inner, ContainsImageConstant))
      llvm::report_fatal_error(
          "HighC cannot render an image constant in a segmented offset");
  }
  // An address needs only the bits, so a signed carrier result gives its
  // unsigned carrier.
  const HighExpr &Printed = Inner != &E ? *Inner : E;
  std::string Text = exprStr(Printed, ParentPrec);
  // A pointer variable is the address itself: every access converts it.
  if (ProjectImageBacking)
    if (auto Pointer = declaredPointerName(Printed, Text))
      return *Pointer;
  if (auto Carrier = unsignedCarrierText(Printed, ParentPrec))
    return *Carrier;
  return Text;
}

bool HighCWriter::isBarePointerName(const HighExpr &E,
                                    llvm::StringRef Text) const {
  return (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) &&
         !Text.empty() &&
         (llvm::isAlpha(Text.front()) || Text.front() == '_') &&
         llvm::all_of(Text,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; }) &&
         pointerNeedsIntegerView(declaredTypeOf(E, Text));
}

std::optional<std::string>
HighCWriter::declaredPointerName(const HighExpr &E,
                                 llvm::StringRef Text) const {
  // Only the integer view a pointer variable prints with, around its name.
  if (E.Kind != ExprKind::Var && E.Kind != ExprKind::Phi)
    return std::nullopt;
  if (!Text.consume_front("(uintptr_t)") || Text.empty() ||
      !(llvm::isAlpha(Text.front()) || Text.front() == '_') ||
      !llvm::all_of(Text, [](char C) { return llvm::isAlnum(C) || C == '_'; }))
    return std::nullopt;
  if (!pointerNeedsIntegerView(declaredTypeOf(E, Text)))
    return std::nullopt;
  return Text.str();
}

TypeRef HighCWriter::declaredTypeOf(const HighExpr &E,
                                    llvm::StringRef Name) const {
  if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi)
    if (TypeRef Param = declaredParamType(E.Var))
      return Param;
  if (auto It = DeclaredCTypes.find(Name.str()); It != DeclaredCTypes.end())
    return It->second;
  return nullptr;
}

bool HighCWriter::pointsToVoid(const TypeRef &Ty) {
  return Ty && Ty->Kind == NdTypeKind::Ptr &&
         (!Ty->Pointee || Ty->Pointee->Kind == NdTypeKind::Void);
}

bool HighCWriter::isIntegerViewOfScalar(const HighExpr &E) const {
  const HighExpr *Inner = peelIntegerViewOps(&E);
  return Inner &&
         (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi);
}

const HighExpr *HighCWriter::typedCallResult(const HighExpr *E) const {
  const HighExpr *Inner = peelIntegerViewOps(E);
  unsigned Peel = 0;
  while (Inner && Peel++ < limits::kMaxIntegerViewUnwrapDepth) {
    if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
      const std::string Name = copyForwardName(varName(Inner->Var));
      if (auto Fwd = ValueForward.find(Name);
          Fwd != ValueForward.end() && Fwd->second && Fwd->second != Inner) {
        Inner = peelIntegerViewOps(Fwd->second);
        continue;
      }
    }
    break;
  }
  if (!Inner || Inner->Kind != ExprKind::Call)
    return nullptr;
  TypeRef Ret = knownCallReturnType(*Inner);
  if (!Ret)
    Ret = Inner->Type;
  if (!Ret || Ret->Kind != NdTypeKind::Int)
    return nullptr;
  return Inner;
}

bool HighCWriter::isCtorSourceExpr(const HighExpr *Op) const {
  auto Rec = [&](auto &&Self, const HighExpr *E, unsigned Depth) -> bool {
    if (!E || Depth > 8 || isUnknownCallOperand(E))
      return false;
    const HighExpr *Inner = unwrapIntegerView(E);
    if (!Inner)
      return false;
    if (Inner->Type && Inner->Type->Kind == NdTypeKind::Ptr)
      return true;
    if (Inner->Kind == ExprKind::Addr)
      return true;
    if (auto Slot = namedFrameSlot(*Inner); Slot)
      return true;
    if (auto Member = typedMemberAccess(*Inner); Member)
      return true;
    if (Inner->Kind == ExprKind::Const) {
      if (Inner->ConstProvenance == ConstantAddressProvenance::Address ||
          Inner->ConstProvenance == ConstantAddressProvenance::DataAddress ||
          Inner->ConstProvenance == ConstantAddressProvenance::CodeAddress)
        return true;
      if (const auto Addr = constAddress(*Inner)) {
        if (isImageDataAddress(*Addr) || ImageObjects.count(*Addr))
          return true;
        // String/object VAs stay copy/string-ctor sources even when the
        // emitter has no BinaryImage (unit tests use 0x14000...).
        if (*Addr >= 0x10000)
          return true;
      }
      return false;
    }
    if (Inner->Kind == ExprKind::Call) {
      const TypeRef Ret = knownCallReturnType(*Inner);
      return Ret && Ret->Kind == NdTypeKind::Ptr;
    }
    if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
        Inner->Operands[0])
      return Self(Self, Inner->Operands[0].get(), Depth + 1) ||
             (Inner->Type && Inner->Type->Kind == NdTypeKind::Ptr);
    if (Inner->Kind == ExprKind::BinOp &&
        (Inner->Op == NdOp::INT_ADD || Inner->Op == NdOp::INT_SUB) &&
        Inner->Operands.size() == 2) {
      if (Self(Self, Inner->Operands[0].get(), Depth + 1) ||
          Self(Self, Inner->Operands[1].get(), Depth + 1))
        return true;
      if (CurrentFunc && Inner->Operands[0] &&
          Inner->Operands[0]->Kind == ExprKind::Var &&
          isSyntheticEntryStackPointer(Inner->Operands[0]->Var, *CurrentFunc,
                                       Opts.TheArch))
        return true;
    }
    if (Inner->Kind != ExprKind::Var && Inner->Kind != ExprKind::Phi)
      return false;
    if (Inner->Var.Kind == MedVar::Param)
      return true;
    const std::string Name = copyForwardName(varName(Inner->Var));
    if (CtorSourceNames.count(Name) || FieldForward.count(Name))
      return true;
    if (auto Fwd = ValueForward.find(Name);
        Fwd != ValueForward.end() && Fwd->second && Fwd->second != Inner)
      return Self(Self, Fwd->second, Depth + 1);
    const TypeRef ParamTy = declaredParamType(Inner->Var);
    return ParamTy && ParamTy->Kind == NdTypeKind::Ptr;
  };
  return Rec(Rec, Op, 0);
}

bool HighCWriter::isCtorDisplayOperand(const HighExpr *Op) const {
  if (!Op || isUnknownCallOperand(Op))
    return false;
  if (isCtorSourceExpr(Op))
    return true;
  const HighExpr *Inner = unwrapIntegerView(Op);
  return Inner && Inner->Kind == ExprKind::Const && Inner->ConstVal < 0x10000;
}

bool HighCWriter::isUnknownCallOperand(const HighExpr *Op) const {
  if (!Op)
    return true;
  if (FrameStorageActive && certifiedFrameStorageDisplacement(*Op))
    return false;
  const HighExpr *Inner = unwrapIntegerView(Op);
  if (!Inner)
    Inner = Op;
  if (Inner->Kind == ExprKind::Undef)
    return true;
  if (Inner->Kind != ExprKind::Var && Inner->Kind != ExprKind::Phi)
    return false;
  // The stack pointer the function was entered with is frame_base, which no
  // statement assigns: `_start` passes it to __libc_start_main as stack_end.
  if (CurrentFunc &&
      isSyntheticEntryStackPointer(Inner->Var, *CurrentFunc, Opts.TheArch))
    return false;
  const std::string Name = copyForwardName(varName(Inner->Var));
  if (auto It = ValueForward.find(Name); It != ValueForward.end() && It->second)
    return isUnknownCallOperand(It->second);
  if (UnknownOnlyNames.count(Name))
    return true;
  return Inner->Var.Kind != MedVar::Param && !AssignedNames.count(Name) &&
         !AssignedNames.count(varName(Inner->Var));
}

bool HighCWriter::looksLikeHiddenSretOperand(const HighExpr *Op) const {
  if (!Op)
    return false;
  const HighExpr *Inner = unwrapIntegerView(Op);
  if (!Inner)
    Inner = Op;
  if (Inner->Kind == ExprKind::Addr)
    return true;
  if (Inner->Kind == ExprKind::Const && Inner->ConstVal != 0)
    return true;
  if (namedFrameSlot(*Inner) || certifiedFrameStorageDisplacement(*Inner))
    return true;
  if (Inner->Type && Inner->Type->Kind == NdTypeKind::Ptr)
    return true;
  if ((Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) &&
      Inner->Var.Kind == MedVar::Param &&
      isEmittedParamName(copyForwardName(varName(Inner->Var)))) {
    TypeRef ParamTy = debugParamType(Inner->Var);
    if (!ParamTy)
      ParamTy = declaredParamType(Inner->Var);
    return ParamTy && ParamTy->Kind == NdTypeKind::Ptr;
  }
  return false;
}

bool HighCWriter::debugExternUsesHiddenSret(const FunctionSym &FS,
                                            llvm::StringRef ExternName) const {
  if (isMsvcClassValueReturn(FS.ReturnType))
    return true;
  if (!isMsvcPointerEncodedClassReturn(FS.ReturnType))
    return false;
  return DebugExternHiddenSret.count(ExternName.str()) != 0;
}

size_t HighCWriter::debugCallArgLimit(const HighExpr &E) const {
  const size_t Have = E.Operands.size();
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None)
    return Have;
  auto Clamp = [&](size_t Limit) { return std::min(Limit, Have); };
  const std::string Name = callIdentifier(E);
  const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format);
  auto UnknownAt = [&](size_t I) {
    return I < Have && isUnknownCallOperand(E.Operands[I].get());
  };
  auto KeepExtra = [&](size_t I) {
    return I < Have && isCtorDisplayOperand(E.Operands[I].get());
  };
  if (const auto Callee = debugCallee(E)) {
    // The call passes the machine arguments of another signature, whatever
    // their count.
    if (!positionalDebugSignature(*Callee))
      return Have;
    const bool Indirect = isMsvcIndirectReturn(Callee->ReturnType);
    const bool Member = Indirect && isWin64MemberIndirectReturn(*Callee);
    size_t Limit = 0;
    if (Member)
      Limit = Callee->Params.empty() ? 1 : Callee->Params.size() + 1;
    else if (Indirect)
      Limit = Callee->Params.size() + 1;
    else
      Limit = Callee->Params.size();
    if (Limit == 0 && Msvc && Msvc->Kind == MsvcCalleeKind::Dtor)
      Limit = Msvc->MaxArgs;
    if (Limit > 0) {
      if (Msvc && Msvc->ArityKind == MsvcArityKind::Keep)
        return Have;
      if (Indirect &&
          (Member || isMsvcPointerEncodedClassReturn(Callee->ReturnType))) {
        const size_t SretIdx = Member ? 1 : 0;
        if (SretIdx < Have &&
            !looksLikeHiddenSretOperand(E.Operands[SretIdx].get()))
          --Limit;
      }
      const size_t Clamped = Clamp(Limit);
      if (Msvc && Msvc->ArityKind == MsvcArityKind::CtorDrop)
        return msvcPrintedArgLimit(*Msvc, Clamped, UnknownAt, KeepExtra);
      if (Msvc && Msvc->ArityKind == MsvcArityKind::Fixed)
        return std::min(Clamped, static_cast<size_t>(Msvc->MaxArgs));
      return Clamped;
    }
  }
  if (Msvc)
    return msvcPrintedArgLimit(*Msvc, Have, UnknownAt, KeepExtra);
  // A routine with a C library prototype takes its parameters.
  if (const libc::LibCPrototype *Prototype = calleePrototype(E);
      Prototype && !Prototype->Variadic)
    return Clamp(Prototype->ParamCount);
  if (auto Arity = knownArity(resolvedCallTarget(E), Name);
      Arity && Arity->FpArgs == 0 && Arity->IntArgs >= 0)
    return Clamp(static_cast<size_t>(Arity->IntArgs));
  return Have;
}

std::optional<libc::LibCArity>
HighCWriter::knownArity(llvm::StringRef Symbol, llvm::StringRef Identifier) {
  if (auto Arity = libc::libcArityForSymbol(Symbol))
    return Arity;
  return libc::libcArity(Identifier);
}

std::optional<size_t>
HighCWriter::plainDeclarationArity(const HighExpr &E) const {
  if (E.Kind != ExprKind::Call || E.IntrinsicId != Intrinsic::None ||
      E.IsIndirectCall || E.CallTarget.empty() || calledDefinition(E) ||
      debugCallee(E))
    return std::nullopt;
  // Source, debug and MSVC knowledge declare the function their own way.
  const std::string Name = callIdentifier(E);
  if (SourceNativeSignatures.count(Name) ||
      ConflictingSourceNativeSignatures.count(Name) ||
      msvcCallee(Name, Opts.Format))
    return std::nullopt;
  if (const libc::LibCPrototype *Prototype = calleePrototype(E))
    return static_cast<size_t>(Prototype->ParamCount);
  const auto Arity = knownArity(resolvedCallTarget(E), Name);
  if (!Arity || Arity->FpArgs != 0 || Arity->IntArgs < 0)
    return std::nullopt;
  return static_cast<size_t>(Arity->IntArgs);
}

namespace {
bool isNamedPointerDisplay(const TypeRef &Ty) {
  if (!Ty || Ty->Kind != NdTypeKind::Ptr || !Ty->Pointee)
    return false;
  return Ty->Pointee->Kind == NdTypeKind::Struct &&
         !Ty->Pointee->SourceName.empty();
}
} // namespace

TypeRef HighCWriter::displayCallArgType(const HighExpr &Call,
                                        size_t Index) const {
  auto FromFS = [&](const FunctionSym &FS) -> TypeRef {
    TypeRef Ty = cDisplayType(expectedDebugCallArgType(FS, Index));
    return isNamedPointerDisplay(Ty) ? Ty : TypeRef{};
  };
  if (Dbg && Call.CallAddr)
    if (auto FS = Dbg->resolveFunction(Call.CallAddr))
      if (TypeRef Ty = FromFS(*FS))
        return Ty;
  if (auto FS = debugCallee(Call))
    if (TypeRef Ty = FromFS(*FS))
      return Ty;
  const std::string Name = callIdentifier(Call);
  if (auto It = DebugExternSigs.find(Name); It != DebugExternSigs.end())
    if (TypeRef Ty = FromFS(It->second))
      return Ty;
  if (const MsvcCallee *Msvc = msvcCallee(Name, Opts.Format)) {
    if (Index == 0 || msvcTypesCallArgAsPointer(*Msvc, Index) ||
        (Msvc->Kind == MsvcCalleeKind::Ctor && Index == 1)) {
      TypeRef Ty = cDisplayType(msvcSyntheticThis(Name, *Msvc));
      if (isNamedPointerDisplay(Ty))
        return Ty;
    }
  }
  return nullptr;
}

TypeRef HighCWriter::expectedDebugCallArgType(const FunctionSym &FS,
                                              size_t Index) const {
  // A call's arguments come in the convention's order, which matches the
  // signature's only for a positional one.
  if (!positionalDebugSignature(FS))
    return nullptr;
  const bool Indirect = isMsvcIndirectReturn(FS.ReturnType);
  const bool Member = Indirect && isWin64MemberIndirectReturn(FS);
  TypeRef Sret;
  if (Indirect) {
    if (TypeRef Record = msvcIndirectReturnRecordType(FS.ReturnType))
      Sret = NdType::makePtr(cDisplayType(Record));
  }
  // The callee's declaration gives a parameter whose type C cannot spell the
  // register it travels in (debugExternPrototype).
  auto Declared = [&](size_t Param) -> TypeRef {
    if (Param >= FS.Params.size())
      return nullptr;
    const TypeRef &Ty = FS.Params[Param].second;
    if (Ty && !hasCSpelling(cDisplayType(Ty)))
      return NdType::makeInt(pointerBytes(Opts.TheArch), false);
    return Ty;
  };
  if (Member) {
    if (Index == 0)
      return Declared(0);
    if (Index == 1)
      return Sret;
    return Declared(Index - 1);
  }
  if (Indirect) {
    if (Index == 0)
      return Sret;
    return Declared(Index - 1);
  }
  return Declared(Index);
}

const HighExpr *HighCWriter::floatBitsSource(const HighExpr &Bits,
                                             const TypeRef &Float) const {
  if (!Float || Float->Kind != NdTypeKind::Float)
    return nullptr;
  // Each step keeps at least the float's bytes: a view of the low bytes, a
  // reinterpretation or a forwarded copy.  The walk ends at a value of the
  // float's type, or at a call, which carries no type of its own when
  // forwarded.
  const HighExpr *Inner = &Bits;
  for (unsigned Step = 0; Inner && Step < limits::kMaxIntegerViewUnwrapDepth;
       ++Step) {
    if (Inner->Kind == ExprKind::Call)
      return Inner;
    if (!Inner->Type)
      return nullptr;
    if (Inner->Type->Kind == NdTypeKind::Float)
      return equalSourceTypes(Inner->Type, Float) ? Inner : nullptr;
    if (Inner->Type->Size < Float->Size)
      return nullptr;
    const HighExpr *Operand =
        Inner->Operands.empty() ? nullptr : Inner->Operands[0].get();
    const HighExpr *Next = nullptr;
    if (Inner->Kind == ExprKind::BinOp && Inner->Op == NdOp::SUBBYTES &&
        Inner->Operands.size() == 2 && Inner->Operands[1] &&
        Inner->Operands[1]->Kind == ExprKind::Const &&
        Inner->Operands[1]->ConstVal == 0)
      Next = Operand;
    else if (Inner->Kind == ExprKind::UnaryOp &&
             (Inner->Op == NdOp::INT_ZEXT || Inner->Op == NdOp::INT_SEXT))
      Next = Operand;
    // A cast of a floating value converts it; one of integer bits views
    // them.
    else if (Inner->Kind == ExprKind::Cast && Operand &&
             (!Operand->Type || Operand->Type->Kind == NdTypeKind::Int ||
              Operand->Type->Kind == NdTypeKind::Ptr))
      Next = Operand;
    else if (Inner->Kind == ExprKind::BitCast)
      Next = Operand;
    else if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)
      if (const auto Fwd =
              ValueForward.find(copyForwardName(varName(Inner->Var)));
          Fwd != ValueForward.end() && Fwd->second && Fwd->second != Inner)
        Next = Fwd->second;
    if (!Next)
      return Inner;
    Inner = Next;
  }
  return Inner;
}

std::optional<std::string>
HighCWriter::floatCallResultText(const HighExpr &Bits, const TypeRef &Float) {
  const HighExpr *Source = floatBitsSource(Bits, Float);
  if (!Source || Source->Kind != ExprKind::Call)
    return std::nullopt;
  const TypeRef Return = knownCallReturnType(*Source);
  if (!Return || !equalSourceTypes(Return, Float))
    return std::nullopt;
  if (!Source->SourceCallHint)
    return renderCallExpr(*Source);
  // A source-bound call prints its machine carrier, even when the callee
  // returns a float. Decode that carrier before using the floating value.
  // Validate before rendering, which records names and expression types.
  const TypeRef &Carrier = Source->Type;
  if (!Carrier || (Float->Size != 4 && Float->Size != 8) ||
      Carrier->Size != Float->Size ||
      (Carrier->Kind != NdTypeKind::Int && Carrier->Kind != NdTypeKind::Float))
    return std::nullopt;
  return sourceValue(renderCallExpr(*Source), Carrier, Float);
}

const HighExpr *HighCWriter::floatBitsValue(const HighExpr &Bits,
                                            const TypeRef &Float) const {
  const HighExpr *Source = floatBitsSource(Bits, Float);
  return Source && Source->Kind != ExprKind::Call && Source->Type &&
                 Source->Type->Kind == NdTypeKind::Float
             ? Source
             : nullptr;
}

std::optional<std::string>
HighCWriter::floatConstantBitsText(const HighExpr &Bits,
                                   const TypeRef &Float) const {
  const HighExpr *Source = floatBitsSource(Bits, Float);
  if (!Source || !Source->Type || Source->Type->Size < Float->Size)
    return std::nullopt;
  std::optional<uint64_t> Value;
  if (Source->Kind == ExprKind::Const)
    Value = Source->ConstVal;
  else if (Source->Kind == ExprKind::Load &&
           Source->MemoryOrdering == NdMemoryOrdering::None &&
           Source->MemoryAddressSpace == NdMemoryAddressSpace::Default &&
           !Source->Operands.empty() && Source->Operands[0])
    if (const auto VA = constAddress(*Source->Operands[0]))
      Value = foldReadonlyScalar(*VA, Source->Type->Size);
  if (!Value)
    return std::nullopt;
  return floatConstantText(
      *Value & (Float->Size >= 8 ? ~uint64_t{0}
                                 : (uint64_t{1} << (Float->Size * 8)) - 1),
      Float);
}

std::optional<std::string>
HighCWriter::floatArgumentText(const HighExpr &Arg, const TypeRef &Expected) {
  if (!Expected || Expected->Kind != NdTypeKind::Float ||
      (Expected->Size != sizeof(float) && Expected->Size != sizeof(double)) ||
      !Arg.Type || Arg.Type->Kind != NdTypeKind::Int ||
      Arg.Type->Size < Expected->Size)
    return std::nullopt;
  // The bits of a value of the parameter's own type, or of a call returning
  // it, pass as that value.
  if (const HighExpr *Value = floatBitsValue(Arg, Expected))
    return exprStr(*Value);
  if (const auto Call = floatCallResultText(Arg, Expected))
    return *Call;
  if (const auto Literal = floatConstantBitsText(Arg, Expected))
    return *Literal;
  // A register's integer bits are the argument's bits: C would convert
  // their value instead.
  return "__builtin_bit_cast(" + typeToC(Expected) + ", " +
         integerView(Arg, NdType::makeInt(Expected->Size, false), 0) + ")";
}

std::string HighCWriter::exprStrAsTypedArg(const HighExpr &E,
                                           const TypeRef &Expected) {
  std::string Text = typedArgumentText(E, Expected);
  // A pointer converts to an integer parameter, which C does not do itself.
  if (Expected && Expected->Kind == NdTypeKind::Int && !Expected->IsEnum &&
      printsAsPointer(Text))
    return "(" + typeToC(Expected) + ")(uintptr_t)" + castOperand(Text);
  return Text;
}

std::string HighCWriter::typedArgumentText(const HighExpr &E,
                                           const TypeRef &Expected) {
  if (const auto Float = floatArgumentText(E, Expected))
    return *Float;
  // A floating expression retains its conversion or reinterpretation; the
  // integer-view cleanup below must not strip a floating Cast or BitCast.
  if (Expected && Expected->Kind == NdTypeKind::Float && E.Type &&
      E.Type->Kind == NdTypeKind::Float)
    return exprStr(E);
  // C converts an integer argument by the signedness of its own type, so an
  // extension to a wider parameter cannot pass its narrower source unless
  // that source extends the same way.
  if (Expected && Expected->Kind == NdTypeKind::Int && !Expected->IsEnum) {
    const HighExpr &Source = lowBytesSource(E, Expected->Size);
    if (Source.Kind == ExprKind::UnaryOp &&
        (Source.Op == NdOp::INT_ZEXT || Source.Op == NdOp::INT_SEXT) &&
        !Source.Operands.empty() && Source.Operands[0] &&
        Source.Operands[0]->Kind != ExprKind::Const &&
        Source.Operands[0]->Type &&
        Source.Operands[0]->Type->Size < Expected->Size) {
      if (auto Converted = implicitIntegerConversion(E, Expected))
        return *Converted;
      return exprStr(E);
    }
  }
  const HighExpr *Inner = peelIntegerViewOps(&E);
  if (!Inner)
    Inner = &E;
  unsigned Follow = 0;
  while (Inner && Follow++ < limits::kMaxIntegerViewUnwrapDepth) {
    if (Inner->Kind != ExprKind::Var && Inner->Kind != ExprKind::Phi)
      break;
    const std::string Name = copyForwardName(varName(Inner->Var));
    auto Fwd = ValueForward.find(Name);
    if (Fwd == ValueForward.end() || !Fwd->second || Fwd->second == Inner)
      break;
    const HighExpr *Next = peelIntegerViewOps(Fwd->second);
    if (!Next || Next == Inner)
      break;
    if (Next->Kind == ExprKind::BinOp &&
        (Next->Op == NdOp::INT_ADD || Next->Op == NdOp::INT_SUB ||
         Next->Op == NdOp::INT_MULT)) {
      Inner = Fwd->second;
      break;
    }
    Inner = Next;
  }
  if (!Inner)
    Inner = &E;
  if (Inner->Kind == ExprKind::Undef && Expected &&
      Expected->Kind == NdTypeKind::Ptr)
    return "(" + typeToC(Expected) + ")(uintptr_t)(" + exprStr(*Inner) + ")";
  if (Expected && Expected->Kind == NdTypeKind::Ptr) {
    unsigned PeelZero = 0;
    while (Inner && PeelZero++ < limits::kMaxIntegerViewUnwrapDepth &&
           Inner->Kind == ExprKind::BinOp && Inner->Operands.size() == 2 &&
           Inner->Operands[0] && Inner->Operands[1] &&
           Inner->Operands[1]->Kind == ExprKind::Const &&
           Inner->Operands[1]->ConstVal == 0) {
      Inner = unwrapIntegerView(Inner->Operands[0].get());
    }
    if (!Inner)
      Inner = &E;
    if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
        Inner->Operands[0]) {
      if (auto Slot = namedFrameSlot(*Inner->Operands[0]))
        return copyForwardName(*Slot);
    }
    if (auto Slot = namedFrameSlot(*Inner))
      return "&" + *Slot;
    if (const auto Disp = certifiedFrameStorageDisplacement(*Inner))
      return "(" + typeToC(Expected) + ")(uintptr_t)(" +
             frameStorageAddress(*Disp) + ")";
    // An integer, such as a variable C declares as one or the integer
    // expression it forwards, converts to the pointer the parameter takes;
    // a pointer passes as it is.
    auto AsPointer = [&](const HighExpr &Printed, std::string Text) {
      if (!printsAsInteger(Printed, Text))
        return Text;
      return "(" + typeToC(Expected) + ")(uintptr_t)" + castOperand(Text);
    };
    if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
      const std::string Name = copyForwardName(varName(Inner->Var));
      const HighExpr *From = nullptr;
      std::string Text = printedForwardedVar(Name, 16, &From);
      if (Text == Name)
        return AsPointer(*Inner, std::move(Text));
      return From ? AsPointer(*From, std::move(Text)) : Text;
    }
    if (Inner->Kind == ExprKind::Addr)
      return exprStr(*Inner);
    if (Inner != &E)
      return AsPointer(*Inner, exprStr(*Inner));
    return AsPointer(E, exprStr(E));
  }
  if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
    const std::string Name = copyForwardName(varName(Inner->Var));
    if (auto Printed = printedForwardedVar(Name, 16); !Printed.empty())
      return Printed;
    if (Expected)
      return Name;
  }
  if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
      Inner->Operands[0]) {
    if (auto Member = typedMemberAccess(*Inner->Operands[0]))
      return *Member;
  }
  if (Expected && Expected->Kind == NdTypeKind::Struct && Expected->IsEnum) {
    const HighExpr *Const = Inner;
    if ((Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)) {
      const std::string Name = copyForwardName(varName(Inner->Var));
      if (auto It = ValueForward.find(Name);
          It != ValueForward.end() && It->second &&
          It->second->Kind == ExprKind::Const)
        Const = It->second;
    }
    if (Const && Const->Kind == ExprKind::Const) {
      if (auto Name = enumeratorDisplay(Expected, Const->ConstVal))
        return *Name;
    }
    if (Inner != &E && Inner->Kind == ExprKind::Call)
      return exprStr(*Inner);
  }
  if (Expected && Expected->Kind == NdTypeKind::Int && Inner != &E)
    return exprStr(*Inner);
  return exprStr(E);
}

bool HighCWriter::printsAsInteger(const HighExpr &E,
                                  llvm::StringRef Text) const {
  auto Integer = [](const TypeRef &Type) {
    return Type && (Type->Kind == NdTypeKind::Int ||
                    (Type->Kind == NdTypeKind::Struct && Type->IsEnum));
  };
  // A string literal and an address are pointers; a constant 0 is a null
  // pointer constant.
  if (Text.starts_with("\"") || Text.starts_with("L\"") ||
      Text.starts_with("u\"") || Text.starts_with("U\"") ||
      Text.starts_with("u8\"") || Text.starts_with("&"))
    return false;
  if (E.Kind == ExprKind::Const)
    return E.ConstVal != 0;
  // A name has the type C declares for it, and a member the type its record
  // gives it, whatever the machine value they print for.
  if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi) {
    const std::string Name = copyForwardName(varName(E.Var));
    if (Text == Name)
      if (const TypeRef Declared = declaredTypeOf(E, Name))
        return Integer(Declared);
  }
  if (const TypeRef Declared = declaredTypeNamed(Text))
    return Integer(Declared);
  if (E.Kind == ExprKind::Load && !E.Operands.empty() && E.Operands[0] &&
      !Text.starts_with("*"))
    if (const TypeRef Member =
            typedMemberType(*E.Operands[0], E.Type ? E.Type->Size : 0))
      return Integer(Member);
  return Integer(E.Type);
}

bool HighCWriter::printsAsPointer(llvm::StringRef Text) const {
  if (Text.starts_with("\"") || Text.starts_with("L\"") ||
      Text.starts_with("u\"") || Text.starts_with("U\"") ||
      Text.starts_with("u8\"") || Text.starts_with("&"))
    return true;
  const TypeRef Declared = declaredTypeNamed(Text);
  return Declared && Declared->Kind == NdTypeKind::Ptr;
}

TypeRef HighCWriter::declaredTypeNamed(llvm::StringRef Text) const {
  if (Text.empty() || !(llvm::isAlpha(Text[0]) || Text[0] == '_') ||
      !llvm::all_of(Text,
                    [](char Ch) { return llvm::isAlnum(Ch) || Ch == '_'; }))
    return nullptr;
  if (auto It = DeclaredCTypes.find(Text.str()); It != DeclaredCTypes.end())
    return It->second;
  if (CurrentFunc)
    for (size_t PI = 0; PI < CurrentFunc->Params.size(); ++PI) {
      MedVar Param;
      Param.Kind = MedVar::Param;
      Param.Id = static_cast<int>(PI);
      if (varName(Param) == Text)
        return emittedParamType(*CurrentFunc, PI);
    }
  for (const auto &[Disp, Slot] : FrameSlots) {
    (void)Disp;
    if (Slot.Name == Text && Slot.Type)
      return Slot.Type;
  }
  return nullptr;
}

std::string HighCWriter::debugSignatureKey(const FunctionSym &FS) {
  // A type C cannot spell is declared as its machine type, the same for
  // every such type.
  auto Spell = [](const TypeRef &Ty) {
    return hasCSpelling(Ty) ? typeToC(Ty) : std::string("?");
  };
  std::string Key;
  if (FS.ReturnType)
    Key += Spell(FS.ReturnType);
  Key += "/";
  for (const auto &Param : FS.Params) {
    Key += Param.first;
    Key += ":";
    if (Param.second)
      Key += Spell(Param.second);
    Key += ";";
  }
  return Key;
}

int HighCWriter::debugSymRichness(const FunctionSym &FS) {
  int Score = static_cast<int>(FS.Params.size()) * 10;
  if (FS.ReturnType) {
    Score += 2;
    if (isMsvcIndirectReturn(FS.ReturnType))
      Score += 5;
  }
  for (const auto &Param : FS.Params) {
    if (!Param.second)
      continue;
    ++Score;
    TypeRef Ty = Param.second;
    while (Ty && Ty->Kind == NdTypeKind::Ptr)
      Ty = Ty->Pointee;
    // Same arity + sret: an enumerator is the display type even when a
    // colliding pointer prototype has otherwise won.
    if (Ty && Ty->Kind == NdTypeKind::Struct && Ty->IsEnum)
      Score += 3;
  }
  return Score;
}

TypeRef HighCWriter::cDisplayType(const TypeRef &Ty) {
  if (!Ty)
    return Ty;
  if (Ty->Kind == NdTypeKind::Ptr)
    return NdType::makePtr(cDisplayType(Ty->Pointee));
  if (Ty->Kind == NdTypeKind::Struct && !Ty->SourceName.empty()) {
    auto Out = NdType::makeNamedRecord(cNamedTypeSpelling(Ty->SourceName),
                                       Ty->Size ? Ty->Size : 8, Ty->IsEnum);
    Out->FieldDisplayNames = Ty->FieldDisplayNames;
    Out->FieldDisplayOffsets = Ty->FieldDisplayOffsets;
    Out->FieldDisplayTypes = Ty->FieldDisplayTypes;
    return Out;
  }
  return Ty;
}

std::string
HighCWriter::debugExternPrototype(const FunctionSym &FS,
                                  const std::string &Identifier,
                                  llvm::StringRef ExternName) const {
  // A symbol the debug information names without a type returns what its
  // register holds, whole: a narrower type would drop the upper bytes of a
  // pointer or a 64-bit result at every call.
  TypeRef ReturnType = FS.ReturnType
                           ? cDisplayType(FS.ReturnType)
                           : NdType::makeInt(pointerBytes(Opts.TheArch), false);
  // So does one whose type C cannot spell.
  if (!hasCSpelling(ReturnType))
    ReturnType = NdType::makeInt(pointerBytes(Opts.TheArch), false);
  TypeRef SretPtr;
  const bool Indirect = debugExternUsesHiddenSret(
      FS, ExternName.empty() ? Identifier : ExternName);
  const bool Member = Indirect && isWin64MemberIndirectReturn(FS);
  if (Indirect) {
    const NdType *Record = msvcIndirectReturnRecord(FS.ReturnType);
    const std::string Spell = cNamedTypeSpelling(Record->SourceName);
    SretPtr = NdType::makePtr(
        NdType::makeNamedRecord(Spell, Record->Size ? Record->Size : 8));
    ReturnType = SretPtr;
  }
  std::string Declarator = Identifier + "(";
  size_t Emitted = 0;
  const MsvcCallee *Msvc = msvcCallee(Identifier, Opts.Format);
  auto Emit = [&](TypeRef Ty, std::string Name) {
    if (Msvc && Msvc->ArityKind == MsvcArityKind::Fixed &&
        Emitted >= Msvc->MaxArgs)
      return;
    Ty = cDisplayType(Ty);
    if (!Ty)
      Ty = NdType::makeInt(8);
    // A parameter whose type C cannot spell is the register it travels in.
    if (!hasCSpelling(Ty))
      Ty = NdType::makeInt(pointerBytes(Opts.TheArch), false);
    // MSVC x64 passes a named class through a hidden pointer. The PDB
    // still records the class. Enums stay in a register.
    if (Opts.TheArch == Arch::X64 && isMsvcClassValueReturn(Ty))
      Ty = NdType::makePtr(Ty);
    if (Name.empty())
      Name = "arg" + std::to_string(Emitted);
    if (Emitted++)
      Declarator += ", ";
    Declarator += declarationToC(Ty, Name);
  };
  if (Member) {
    if (!FS.Params.empty())
      Emit(FS.Params[0].second,
           FS.Params[0].first.empty() ? "this" : FS.Params[0].first);
    Emit(SretPtr, "result");
    for (size_t I = 1; I < FS.Params.size(); ++I)
      Emit(FS.Params[I].second, FS.Params[I].first);
  } else if (Indirect) {
    Emit(SretPtr, "result");
    for (const auto &Param : FS.Params)
      Emit(Param.second, Param.first);
  } else {
    for (const auto &Param : FS.Params)
      Emit(Param.second, Param.first);
  }
  if (Msvc && FS.Params.empty() && !Indirect)
    return msvcSyntheticPrototype(Identifier, *Msvc,
                                  Opts.TheArch == Arch::X64 && Msvc->FastCall);
  if (Msvc)
    ReturnType = msvcSyntheticReturn(Msvc->ReturnKind);
  if (Msvc && Emitted == 0)
    Emit(msvcSyntheticThis(Identifier, *Msvc), "this");
  if (Msvc && Emitted == 1 && msvcTypesCallArgAsPointer(*Msvc, 1))
    Emit(msvcSyntheticThis(Identifier, *Msvc), "src");
  // A debug signature without parameters says nothing about them:
  // debugCallArgLimit keeps every recovered argument for such a callee, so
  // `(void)` would reject the call.  Leave the declaration unprototyped.
  if (Emitted == 0 && !FS.Params.empty())
    Declarator += "void";
  std::string Prefix = "extern ";
  if (Opts.TheArch == Arch::X64 &&
      ((Msvc && Msvc->FastCall) || Member || Indirect ||
       FS.CallConv == DebugCallConv::Thiscall ||
       FS.CallConv == DebugCallConv::Fastcall))
    Prefix += "__fastcall ";
  return Prefix + declarationToC(ReturnType, Declarator + ")");
}

TypeRef HighCWriter::declaredParamType(const MedVar &V) const {
  if (!CurrentFunc || V.Kind != MedVar::Param || V.RenameTag >= 0 || V.Id < 0 ||
      static_cast<size_t>(V.Id) >= CurrentFunc->Params.size())
    return nullptr;
  // The definition may declare the debug information's type, such as
  // `int32_t *out` for a machine word: an offset from `out` is in bytes only
  // through its integer view.
  return emittedParamType(*CurrentFunc, static_cast<size_t>(V.Id));
}

TypeRef HighCWriter::debugParamType(const MedVar &V) const {
  if (!Dbg || !CurrentFunc || V.Kind != MedVar::Param || V.RenameTag >= 0 ||
      V.Id < 0)
    return nullptr;
  // The debug parameter whose whole value the parameter holds.
  const DebugParamBinding B =
      debugParamBinding(*CurrentFunc, static_cast<size_t>(V.Id));
  if (B.Index < 0 || !B.Whole)
    return nullptr;
  auto FS = Dbg->resolveFunction(CurrentFunc->Entry);
  if (!FS || static_cast<size_t>(B.Index) >= FS->Params.size())
    return nullptr;
  TypeRef Ty = FS->Params[B.Index].second;
  if (Ty)
    Dbg->completeType(Ty);
  return Ty;
}

namespace {
void completeDisplayRecord(DebugContext *Dbg, const TypeRef &Ty) {
  if (!Dbg || !Ty)
    return;
  TypeRef Cur = Ty;
  while (Cur && Cur->Kind == NdTypeKind::Ptr)
    Cur = Cur->Pointee;
  if (!Cur)
    return;
  Dbg->completeType(Cur);
  if (Cur->Kind != NdTypeKind::Struct || Cur->IsEnum)
    return;
  if (Cur->FieldDisplayTypes.size() != Cur->FieldDisplayOffsets.size())
    return;
  for (const auto &F : Cur->FieldDisplayTypes) {
    if (!F)
      continue;
    TypeRef Inner = F;
    while (Inner && Inner->Kind == NdTypeKind::Ptr)
      Inner = Inner->Pointee;
    if (Inner && Inner->Kind == NdTypeKind::Struct && !Inner->IsEnum)
      Dbg->completeType(Inner);
  }
}
} // namespace

TypeRef HighCWriter::declaredRecordPointee(const HighExpr &Base) const {
  auto RecordFrom = [](const TypeRef &Ty) -> TypeRef {
    if (!Ty)
      return nullptr;
    if (Ty->Kind == NdTypeKind::Ptr && Ty->Pointee &&
        Ty->Pointee->Kind == NdTypeKind::Struct &&
        !Ty->Pointee->SourceName.empty())
      return Ty->Pointee;
    if (Ty->Kind == NdTypeKind::Struct && !Ty->SourceName.empty())
      return Ty;
    return nullptr;
  };
  auto RicherRecord = [&](const TypeRef &A, const TypeRef &B) -> TypeRef {
    TypeRef RA = RecordFrom(A);
    TypeRef RB = RecordFrom(B);
    if (!RA)
      return RB;
    if (!RB)
      return RA;
    return RB->FieldDisplayNames.size() > RA->FieldDisplayNames.size() ? RB
                                                                       : RA;
  };
  const bool IsParam =
      (Base.Kind == ExprKind::Var || Base.Kind == ExprKind::Phi) &&
      Base.Var.Kind == MedVar::Param && Base.Var.RenameTag < 0;
  if (IsParam) {
    if (TypeRef Best =
            RicherRecord(Base.Type, RicherRecord(declaredParamType(Base.Var),
                                                 debugParamType(Base.Var))))
      return Best;
  }
  if (TypeRef FromExpr = RecordFrom(Base.Type))
    return FromExpr;
  if (Base.Kind == ExprKind::Load && !Base.Operands.empty() && Base.Operands[0])
    if (TypeRef FromMember = RecordFrom(typedMemberType(*Base.Operands[0])))
      return FromMember;
  if (Base.Kind != ExprKind::Var && Base.Kind != ExprKind::Phi)
    return nullptr;
  const std::string FwdName = varName(Base.Var);
  if (auto It = FieldForwardTypes.find(FwdName); It != FieldForwardTypes.end())
    if (TypeRef FromFwd = RecordFrom(It->second))
      return FromFwd;
  const std::string CfName = copyForwardName(FwdName);
  if (CfName != FwdName)
    if (auto It = FieldForwardTypes.find(CfName); It != FieldForwardTypes.end())
      if (TypeRef FromCf = RecordFrom(It->second))
        return FromCf;
  // A debug-typed call result may be declared as a record pointer even though
  // its HighIR register expression still has the machine-width integer type.
  // Use that same declared type when projecting a field through the local.
  if (auto It = CallResultTypes.find(FwdName); It != CallResultTypes.end())
    if (TypeRef FromCall = RecordFrom(It->second))
      return FromCall;
  if (CfName != FwdName)
    if (auto It = CallResultTypes.find(CfName); It != CallResultTypes.end())
      if (TypeRef FromCall = RecordFrom(It->second))
        return FromCall;
  if (auto It = ValueForward.find(FwdName);
      It != ValueForward.end() && It->second)
    if (TypeRef FromVal = declaredRecordPointee(*It->second))
      return FromVal;
  if (TypeRef FromDecl = RecordFrom(declaredParamType(Base.Var)))
    return FromDecl;
  if (TypeRef FromDbg = RecordFrom(debugParamType(Base.Var)))
    return FromDbg;
  const std::string Printed = copyForwardName(varName(Base.Var));
  if (!Printed.empty() || !FwdName.empty()) {
    for (const auto &[Disp, Slot] : FrameSlots) {
      if (Slot.Name != Printed && Slot.Name != FwdName)
        continue;
      if (TypeRef FromSlot = RecordFrom(Slot.Type))
        return FromSlot;
    }
  }
  if (Printed.empty() || !CurrentFunc)
    return nullptr;
  if (Dbg) {
    if (auto FS = Dbg->resolveFunction(CurrentFunc->Entry); FS) {
      for (const auto &Param : FS->Params) {
        if (Param.first == Printed)
          return RecordFrom(Param.second);
      }
    }
  }
  for (size_t I = 0; I < CurrentFunc->Params.size(); ++I) {
    if (CurrentFunc->Params[I].Name == Printed)
      return RecordFrom(CurrentFunc->Params[I].Type);
    MedVar ParamVar;
    ParamVar.Kind = MedVar::Param;
    ParamVar.Id = static_cast<int>(I);
    ParamVar.TheArch = Opts.TheArch;
    if (copyForwardName(varName(ParamVar)) == Printed)
      return RecordFrom(debugParamType(ParamVar));
  }
  return nullptr;
}

std::optional<std::pair<const HighExpr *, uint64_t>>
HighCWriter::typedPointerOffset(const HighExpr &Addr) const {
  const HighExpr *Cur = unwrapIntegerView(&Addr);
  int64_t Acc = 0;
  unsigned Depth = 0;
  while (Cur && Depth++ < limits::kMaxFrameDisplacementDepth) {
    Cur = unwrapIntegerView(Cur);
    if (!Cur)
      return std::nullopt;
    if (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi) {
      const std::string Name = varName(Cur->Var);
      if (auto It = ValueForward.find(Name);
          It != ValueForward.end() && It->second && It->second != Cur) {
        // Keep the proved field projection and its type together. Expanding
        // it back to a raw frame load loses the type when frame slots have
        // been replaced by their shared byte storage.
        if (FieldForward.count(Name) && FieldForwardTypes.count(Name))
          break;
        Cur = It->second;
        continue;
      }
    }
    if (Cur->Kind != ExprKind::BinOp || Cur->Operands.size() != 2 ||
        !Cur->Operands[0] || !Cur->Operands[1] ||
        (Cur->Op != NdOp::INT_ADD && Cur->Op != NdOp::INT_SUB))
      break;
    const HighExpr *LHS = unwrapIntegerView(Cur->Operands[0].get());
    const HighExpr *RHS = unwrapIntegerView(Cur->Operands[1].get());
    if (!LHS || !RHS)
      return std::nullopt;
    const bool Subtract = Cur->Op == NdOp::INT_SUB;
    if (RHS->Kind == ExprKind::Const) {
      int64_t Delta = static_cast<int64_t>(RHS->ConstVal);
      if (RHS->Type && RHS->Type->Size == 4)
        Delta = static_cast<int32_t>(RHS->ConstVal);
      Acc += Subtract ? -Delta : Delta;
      Cur = LHS;
      continue;
    }
    if (!Subtract && LHS->Kind == ExprKind::Const) {
      int64_t Delta = static_cast<int64_t>(LHS->ConstVal);
      if (LHS->Type && LHS->Type->Size == 4)
        Delta = static_cast<int32_t>(LHS->ConstVal);
      Acc += Delta;
      Cur = RHS;
      continue;
    }
    break;
  }
  Cur = unwrapIntegerView(Cur);
  if (!Cur || Acc < 0)
    return std::nullopt;
  return std::make_pair(Cur, static_cast<uint64_t>(Acc));
}

std::optional<HighCWriter::TypedIndexAccess>
HighCWriter::typedIndexAccess(const HighExpr &Addr) {
  const HighExpr *Add = unwrapIntegerView(&Addr);
  if (!Add || Add->Kind != ExprKind::BinOp || Add->Op != NdOp::INT_ADD ||
      Add->Operands.size() != 2 || !Add->Operands[0] || !Add->Operands[1])
    return std::nullopt;

  auto Scale = [&](const HighExpr *E)
      -> std::optional<std::pair<const HighExpr *, uint64_t>> {
    E = unwrapIntegerView(E);
    if (!E || E->Kind != ExprKind::BinOp || E->Operands.size() != 2 ||
        !E->Operands[0] || !E->Operands[1])
      return std::nullopt;
    const HighExpr *L = unwrapIntegerView(E->Operands[0].get());
    const HighExpr *R = unwrapIntegerView(E->Operands[1].get());
    if (!L || !R)
      return std::nullopt;
    if (E->Op == NdOp::INT_MULT) {
      if (R->Kind == ExprKind::Const)
        return std::make_pair(L, R->ConstVal);
      if (L->Kind == ExprKind::Const)
        return std::make_pair(R, L->ConstVal);
    }
    if (E->Op == NdOp::INT_LEFT && R->Kind == ExprKind::Const &&
        R->ConstVal < 8)
      return std::make_pair(L,
                            uint64_t{1} << static_cast<unsigned>(R->ConstVal));
    return std::nullopt;
  };

  const HighExpr *Base = nullptr;
  const HighExpr *Index = nullptr;
  uint64_t Stride = 0;
  if (auto S = Scale(Add->Operands[1].get())) {
    Base = unwrapIntegerView(Add->Operands[0].get());
    Index = S->first;
    Stride = S->second;
  } else if (auto S = Scale(Add->Operands[0].get())) {
    Base = unwrapIntegerView(Add->Operands[1].get());
    Index = S->first;
    Stride = S->second;
  }
  if (!Base || !Index || Stride == 0)
    return std::nullopt;

  std::string BasePath;
  TypeRef PtrTy;
  if (Base->Kind == ExprKind::Load && !Base->Operands.empty() &&
      Base->Operands[0] && Base->MemoryOrdering == NdMemoryOrdering::None &&
      Base->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    if (auto Member = typedMemberAccess(*Base->Operands[0])) {
      BasePath = *Member;
      PtrTy = typedMemberType(*Base->Operands[0]);
      if (PtrTy && PtrTy->Kind != NdTypeKind::Ptr)
        PtrTy = nullptr;
      if (!PtrTy && Base->Type && Base->Type->Kind == NdTypeKind::Ptr)
        PtrTy = Base->Type;
    }
  } else if (Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi) {
    const std::string Raw = varName(Base->Var);
    const std::string Name = copyForwardName(Raw);
    if (auto It = FieldForward.find(Name); It != FieldForward.end())
      BasePath = It->second;
    else if (auto It = FieldForward.find(Raw); It != FieldForward.end())
      BasePath = It->second;
    if (auto It = FieldForwardTypes.find(Name); It != FieldForwardTypes.end())
      PtrTy = It->second;
    else if (auto It = FieldForwardTypes.find(Raw);
             It != FieldForwardTypes.end())
      PtrTy = It->second;
    auto TakeLoad = [&](const HighExpr *E) {
      if (BasePath.empty() && E && E->Kind == ExprKind::Load &&
          !E->Operands.empty() && E->Operands[0]) {
        if (auto Member = typedMemberAccess(*E->Operands[0])) {
          BasePath = *Member;
          TypeRef Ty = typedMemberType(*E->Operands[0]);
          if (Ty && Ty->Kind == NdTypeKind::Ptr)
            PtrTy = std::move(Ty);
        }
      }
    };
    if (auto It = ValueForward.find(Name); It != ValueForward.end())
      TakeLoad(It->second);
    else if (auto It = ValueForward.find(Raw); It != ValueForward.end())
      TakeLoad(It->second);
    if (PtrTy && PtrTy->Kind != NdTypeKind::Ptr &&
        (PtrTy->Kind != NdTypeKind::Struct || PtrTy->IsEnum))
      PtrTy = nullptr;
    if (!PtrTy && Base->Type && Base->Type->Kind == NdTypeKind::Ptr)
      PtrTy = Base->Type;
    if (BasePath.empty()) {
      if (auto Member = typedMemberAccess(*Base))
        BasePath = *Member;
      else
        BasePath = Name;
    }
  }
  if (PtrTy && PtrTy->Kind == NdTypeKind::Struct && !PtrTy->IsEnum &&
      Stride != 0 && Stride <= 0xFFFF) {
    completeDisplayRecord(Dbg, PtrTy);
    const auto FieldSz = static_cast<uint16_t>(Stride);
    if (auto Field = PtrTy->displayFieldPathAt(0, FieldSz)) {
      TypeRef FieldTy = PtrTy->displayFieldTypeAt(0, FieldSz);
      if (FieldTy && FieldTy->Kind == NdTypeKind::Ptr && FieldTy->Pointee &&
          FieldTy->Pointee->Size == Stride) {
        if (!BasePath.empty())
          BasePath += '.';
        BasePath += *Field;
        PtrTy = std::move(FieldTy);
      }
    }
  }
  if (BasePath.empty() || !PtrTy || PtrTy->Kind != NdTypeKind::Ptr ||
      !PtrTy->Pointee || PtrTy->Pointee->Size == 0 ||
      PtrTy->Pointee->Size != Stride)
    return std::nullopt;
  completeDisplayRecord(Dbg, PtrTy);
  completeDisplayRecord(Dbg, PtrTy->Pointee);
  TypedIndexAccess Out;
  Out.Base = std::move(BasePath);
  Out.Index = exprStr(*Index);
  Out.ElemType = PtrTy->Pointee;
  return Out;
}

namespace {
std::optional<std::string> canonicalizeDisplayPath(llvm::StringRef Field) {
  std::string Path;
  llvm::StringRef Rest(Field);
  while (!Rest.empty()) {
    const auto Split = Rest.split('.');
    const std::string Comp = Split.first.str();
    const std::string Name = canonicalizeCProjectionIdentifier(Comp, "");
    if (Name.empty() || Name != Comp)
      return std::nullopt;
    if (!Path.empty())
      Path += '.';
    Path += Name;
    Rest = Split.second;
  }
  if (Path.empty())
    return std::nullopt;
  return Path;
}

std::optional<std::string> interiorFieldPath(const TypeRef &Ty, uint64_t Rel,
                                             uint16_t AccessSize = 0) {
  if (!Ty || Ty->Kind != NdTypeKind::Struct || Ty->IsEnum)
    return std::nullopt;
  if (Rel == 0)
    return std::nullopt;
  if (Ty->Size != 0 && Rel >= Ty->Size)
    return std::nullopt;
  auto Field = Ty->displayFieldPathAt(Rel, AccessSize);
  if (!Field)
    return std::nullopt;
  return canonicalizeDisplayPath(*Field);
}

TypeRef interiorFieldType(const TypeRef &Ty, uint64_t Rel,
                          uint16_t AccessSize = 0) {
  if (!interiorFieldPath(Ty, Rel, AccessSize))
    return nullptr;
  return Ty->displayFieldTypeAt(Rel, AccessSize);
}
} // namespace

std::optional<std::string>
HighCWriter::frameTypedMemberAccess(int64_t Disp, uint16_t AccessSize) const {
  const NamedFrameSlot *Owner = nullptr;
  int64_t OwnerDisp = 0;
  std::string Path;
  for (const auto &[SlotDisp, Slot] : FrameSlots) {
    if (Slot.Name.empty() || SlotDisp > Disp)
      continue;
    const uint64_t Rel = static_cast<uint64_t>(Disp - SlotDisp);
    if (Dbg) {
      Dbg->completeType(Slot.Type);
      Dbg->completeType(Slot.CallType);
    }
    auto Canon = interiorFieldPath(Slot.Type, Rel, AccessSize);
    if (!Canon)
      Canon = interiorFieldPath(Slot.CallType, Rel, AccessSize);
    if (!Canon)
      continue;
    if (Owner && OwnerDisp != SlotDisp)
      return std::nullopt;
    Owner = &Slot;
    OwnerDisp = SlotDisp;
    Path = std::move(*Canon);
  }
  if (!Owner)
    return std::nullopt;
  return Owner->Name + "." + Path;
}

std::optional<std::string>
HighCWriter::scalarRecordFieldDest(llvm::StringRef SlotName,
                                   const HighExpr &Val,
                                   llvm::StringRef PrintedValue) const {
  if (SlotName.empty())
    return std::nullopt;
  uint16_t ValSize = 0;
  if (Val.Type && Val.Type->Size)
    ValSize = Val.Type->Size;
  else if (Val.Kind == ExprKind::Var || Val.Kind == ExprKind::Phi)
    ValSize = Val.Var.Size;
  const bool ConstInt = Val.Kind == ExprKind::Const &&
                        (!Val.Type || Val.Type->Kind == NdTypeKind::Int);
  const uint16_t MachineSize =
      (Val.Kind == ExprKind::Var || Val.Kind == ExprKind::Phi) && Val.Var.Size
          ? Val.Var.Size
          : ValSize;
  TypeRef PrintedType = Val.Type;
  if (Val.Kind == ExprKind::Var || Val.Kind == ExprKind::Phi) {
    const std::string Name = copyForwardName(varName(Val.Var));
    if (auto It = ValueForward.find(Name);
        It != ValueForward.end() && It->second) {
      const HighExpr *Forwarded = peelIntegerViewOps(It->second);
      if (Forwarded && Forwarded->Kind == ExprKind::Call)
        if (TypeRef Ret = knownCallReturnType(*Forwarded);
            Ret && Ret->Kind == NdTypeKind::Ptr && Ret->Size == MachineSize)
          PrintedType = Ret;
    }
  }
  // A machine-width integer view of a named record still prints as that
  // record. Preserve its whole-object copy instead of casting only the
  // destination to an integer and then assigning the record expression.
  if (MachineSize == 16 &&
      (!PrintedType || PrintedType->Kind == NdTypeKind::Int)) {
    const HighExpr *Source = forwardedExpr(&Val);
    std::optional<int64_t> SourceDisp;
    if (Source && Source->Kind == ExprKind::Load && !Source->Operands.empty() &&
        Source->Operands[0])
      SourceDisp = frameDisplacement(*Source->Operands[0]);
    else if (Source &&
             (Source->Kind == ExprKind::Var || Source->Kind == ExprKind::Phi)) {
      const std::string Name = copyForwardName(varName(Source->Var));
      bool Ambiguous = false;
      for (const auto &[Disp, Slot] : FrameSlots) {
        if (Slot.Name != Name)
          continue;
        if (SourceDisp) {
          Ambiguous = true;
          break;
        }
        SourceDisp = Disp;
      }
      if (Ambiguous)
        SourceDisp.reset();
    }
    if (SourceDisp)
      if (auto It = FrameSlots.find(*SourceDisp);
          It != FrameSlots.end() && It->second.Type &&
          It->second.Type->Kind == NdTypeKind::Struct &&
          It->second.Type->Size == MachineSize)
        PrintedType = It->second.Type;
  }
  if (MachineSize == 16 &&
      (!PrintedType || PrintedType->Kind == NdTypeKind::Int) &&
      !PrintedValue.empty()) {
    TypeRef SourceType;
    bool Ambiguous = false;
    for (const auto &[Disp, Slot] : FrameSlots) {
      (void)Disp;
      if (Slot.Name != PrintedValue)
        continue;
      if (SourceType) {
        Ambiguous = true;
        break;
      }
      SourceType = Slot.Type;
    }
    if (!Ambiguous && SourceType && SourceType->Kind == NdTypeKind::Struct &&
        SourceType->Size == MachineSize)
      PrintedType = SourceType;
  }
  auto RawScalarStore = [&]() -> std::optional<std::string> {
    if (MachineSize != 1 && MachineSize != 2 && MachineSize != 4 &&
        MachineSize != 8 && MachineSize != 16)
      return std::nullopt;
    if (PrintedType && PrintedType->Kind == NdTypeKind::Ptr &&
        (MachineSize == 4 || MachineSize == 8))
      return "*(" + typeToC(PrintedType) + " *)&" + std::string(SlotName);
    if (!ConstInt && (!Val.Type || Val.Type->Kind != NdTypeKind::Int))
      return std::nullopt;
    return "*(" + typeToC(NdType::makeInt(MachineSize, false)) + " *)&" +
           std::string(SlotName);
  };
  auto ProjectTy = [&](const TypeRef &Ty) -> std::optional<std::string> {
    if (!Ty || Ty->Kind != NdTypeKind::Struct || Ty->IsEnum)
      return std::nullopt;
    if (PrintedType && PrintedType->Kind == NdTypeKind::Struct)
      return std::nullopt;
    if (Ty->Size && MachineSize > Ty->Size)
      return std::nullopt;
    if (!ConstInt && (ValSize == 0 || Ty->Size == 0 || ValSize > Ty->Size))
      return std::nullopt;
    auto Field = Ty->displayFieldNameAt(0);
    if (!Field) {
      if (Ty->SourceName == "ArgList" && MachineSize == 8 &&
          (ConstInt || (PrintedType && PrintedType->Kind == NdTypeKind::Int)))
        return std::string(SlotName) + ".types_";
      return RawScalarStore();
    }
    TypeRef FieldTy;
    for (size_t I = 0;
         I < Ty->FieldDisplayOffsets.size() && I < Ty->FieldDisplayTypes.size();
         ++I) {
      if (Ty->FieldDisplayOffsets[I] == 0 &&
          Ty->FieldDisplayNames[I] == *Field) {
        FieldTy = Ty->FieldDisplayTypes[I];
        break;
      }
    }
    if (!FieldTy ||
        (FieldTy->Kind != NdTypeKind::Int && FieldTy->Kind != NdTypeKind::Ptr))
      return RawScalarStore();
    if (PrintedType && PrintedType->Kind == NdTypeKind::Ptr &&
        FieldTy->Kind != NdTypeKind::Ptr)
      return RawScalarStore();
    // Assigning the wider field would overwrite bytes the machine store did
    // not touch. Keep a partial store on the record's exact-width byte view.
    if (FieldTy->Size && MachineSize < FieldTy->Size)
      return RawScalarStore();
    if (!ConstInt && FieldTy->Size != 0 && MachineSize > FieldTy->Size)
      return std::nullopt;
    // `CStringT` is an 8-byte wrapper around `m_pszData`.  A same-width
    // pointer store is that field, not a whole-record copy.
    if (!ConstInt && ValSize == Ty->Size && FieldTy->Size != Ty->Size &&
        MachineSize >= Ty->Size)
      return std::nullopt;
    auto Canon = canonicalizeDisplayPath(*Field);
    if (!Canon)
      return std::nullopt;
    return std::string(SlotName) + "." + *Canon;
  };
  auto Project = [&](const NamedFrameSlot &Slot) -> std::optional<std::string> {
    // A call overlay may only carry the record name, while the PDB type has
    // the actual field list. Prefer the richer field evidence before falling
    // back to a raw byte view.
    if (Slot.Type && Slot.CallType &&
        Slot.Type->FieldDisplayNames.size() >
            Slot.CallType->FieldDisplayNames.size()) {
      if (auto Path = ProjectTy(Slot.Type))
        return Path;
    }
    if (auto Path = ProjectTy(Slot.CallType)) {
      const std::string Base = SlotName.str() + ".";
      if (Slot.Type && Slot.CallType &&
          Slot.Type->SourceName != Slot.CallType->SourceName &&
          llvm::StringRef(*Path).starts_with(Base))
        return "((" + typeToC(Slot.CallType) + " *)&" + SlotName.str() + ")->" +
               Path->substr(Base.size());
      return Path;
    }
    return ProjectTy(Slot.Type);
  };
  std::optional<std::string> Found;
  for (const auto &[Disp, Slot] : FrameSlots) {
    if (Slot.Name != SlotName)
      continue;
    auto Path = Project(Slot);
    if (!Path)
      continue;
    if (Found && *Found != *Path)
      return std::nullopt;
    Found = std::move(Path);
  }
  return Found;
}

std::optional<std::string>
HighCWriter::callOverlayIntegerMemberStore(llvm::StringRef Member,
                                           const HighExpr &Val) const {
  if (Member.empty() || !isIntegerOverlayStore(Val))
    return std::nullopt;
  const auto Dot = Member.rfind('.');
  if (Dot == llvm::StringRef::npos || Dot == 0)
    return std::nullopt;
  const llvm::StringRef Base = Member.take_front(Dot);
  const llvm::StringRef Field = Member.drop_front(Dot + 1);
  if (Base.empty() || Field.empty())
    return std::nullopt;
  for (const auto &[Disp, Slot] : FrameSlots) {
    (void)Disp;
    if (Slot.Name != Base || !Slot.Type || !Slot.CallType)
      continue;
    if (Slot.Type->Kind != NdTypeKind::Struct || Slot.Type->IsEnum)
      continue;
    std::optional<uint64_t> Off;
    for (size_t I = 0; I < Slot.Type->FieldDisplayNames.size() &&
                       I < Slot.Type->FieldDisplayOffsets.size();
         ++I) {
      if (Slot.Type->FieldDisplayNames[I] != Field)
        continue;
      if (Off && *Off != Slot.Type->FieldDisplayOffsets[I])
        return std::nullopt;
      Off = Slot.Type->FieldDisplayOffsets[I];
    }
    if (!Off && Field == "p")
      Off = 8;
    if (!Off)
      continue;
    auto Overlay = Slot.CallType->displayFieldNameAt(*Off);
    if (!Overlay || *Overlay == Field)
      continue;
    if (Slot.Type->SourceName != Slot.CallType->SourceName)
      return "((" + typeToC(Slot.CallType) + " *)&" + Base.str() + ")->" +
             *Overlay;
    return Base.str() + "." + *Overlay;
  }
  return std::nullopt;
}

bool HighCWriter::isIntegerOverlayStore(const HighExpr &Val) const {
  const HighExpr *Inner = peelIntegerViewOps(&Val);
  if (!Inner)
    Inner = &Val;
  if ((Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)) {
    const std::string Name = varName(Inner->Var);
    if (auto It = ValueForward.find(Name);
        It != ValueForward.end() && It->second && It->second != Inner)
      return isIntegerOverlayStore(*It->second);
  }
  if (Inner->Kind == ExprKind::Const)
    return false;
  if (Inner->Type && Inner->Type->Kind == NdTypeKind::Ptr)
    return false;
  if (Inner->Kind != ExprKind::Call)
    return false;
  TypeRef Ty = knownCallReturnType(*Inner);
  return Ty && Ty->Kind == NdTypeKind::Int;
}

std::optional<int64_t>
HighCWriter::frameOverlayDisplacement(const HighExpr &Base,
                                      uint64_t Rel) const {
  if (const auto Disp = frameDisplacement(Base))
    return *Disp + static_cast<int64_t>(Rel);
  const HighExpr *Cur = unwrapIntegerView(&Base);
  if (Cur && Cur->Kind == ExprKind::Load && !Cur->Operands.empty() &&
      Cur->Operands[0] && Cur->MemoryOrdering == NdMemoryOrdering::None &&
      Cur->MemoryAddressSpace == NdMemoryAddressSpace::Default)
    if (const auto Disp = frameDisplacement(*Cur->Operands[0]))
      return *Disp + static_cast<int64_t>(Rel);
  return std::nullopt;
}

TypeRef HighCWriter::frameTypedMemberType(int64_t Disp,
                                          uint16_t AccessSize) const {
  const NamedFrameSlot *Owner = nullptr;
  int64_t OwnerDisp = 0;
  TypeRef Ty;
  for (const auto &[SlotDisp, Slot] : FrameSlots) {
    if (Slot.Name.empty() || SlotDisp > Disp)
      continue;
    const uint64_t Rel = static_cast<uint64_t>(Disp - SlotDisp);
    if (Dbg) {
      Dbg->completeType(Slot.Type);
      Dbg->completeType(Slot.CallType);
    }
    TypeRef FieldTy = interiorFieldType(Slot.Type, Rel, AccessSize);
    if (!FieldTy)
      FieldTy = interiorFieldType(Slot.CallType, Rel, AccessSize);
    if (!FieldTy)
      continue;
    if (Owner && OwnerDisp != SlotDisp)
      return nullptr;
    Owner = &Slot;
    OwnerDisp = SlotDisp;
    Ty = std::move(FieldTy);
  }
  return Ty;
}

std::optional<std::string>
HighCWriter::typedMemberAccess(const HighExpr &Addr, uint16_t AccessSize,
                               bool EnterNestedAtZero) const {
  if (const auto Disp = frameDisplacement(Addr))
    if (auto FrameMember = frameTypedMemberAccess(*Disp, AccessSize))
      return FrameMember;
  const auto Peeled = typedPointerOffset(Addr);
  if (!Peeled)
    return std::nullopt;
  const HighExpr *Base = Peeled->first;
  if (const auto OverlayDisp = frameOverlayDisplacement(*Base, Peeled->second))
    if (auto Overlay = frameTypedMemberAccess(*OverlayDisp, AccessSize))
      return Overlay;
  const TypeRef Record = declaredRecordPointee(*Base);
  if (!Record)
    return std::nullopt;
  completeDisplayRecord(Dbg, Record);
  const auto Field =
      Record->displayFieldPathAt(Peeled->second, AccessSize, EnterNestedAtZero);
  if (!Field)
    return std::nullopt;
  auto Path = canonicalizeDisplayPath(*Field);
  if (!Path)
    return std::nullopt;
  std::string BaseName;
  if (Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi) {
    const std::string Raw = varName(Base->Var);
    BaseName = Raw;
    if (auto It = FieldForward.find(Raw); It != FieldForward.end())
      BaseName = It->second;
    else {
      const std::string Cf = copyForwardName(Raw);
      const bool TypedCursor = FieldForwardTypes.count(Raw);
      if (auto It = FieldForward.find(Cf); It != FieldForward.end()) {
        if (!TypedCursor)
          BaseName = It->second;
      } else if (!TypedCursor) {
        BaseName = Cf;
        if (auto It = ValueForward.find(Raw);
            It != ValueForward.end() && It->second &&
            (It->second->Kind == ExprKind::Var ||
             It->second->Kind == ExprKind::Phi)) {
          BaseName = copyForwardName(varName(It->second->Var));
          if (auto Fwd = FieldForward.find(BaseName); Fwd != FieldForward.end())
            BaseName = Fwd->second;
        }
      }
    }
  } else if (Base->Kind == ExprKind::Load && !Base->Operands.empty() &&
             Base->Operands[0]) {
    auto Inner = typedMemberAccess(*Base->Operands[0]);
    if (!Inner)
      return std::nullopt;
    BaseName = *Inner;
  } else
    return std::nullopt;
  if (BaseName.empty())
    return std::nullopt;
  if (Base->Type && Base->Type->Kind == NdTypeKind::Struct)
    return BaseName + "." + *Path;
  return BaseName + "->" + *Path;
}

std::optional<std::string>
HighCWriter::typedMemberAddress(const HighExpr &Addr) const {
  return typedMemberAccess(Addr, 0, false);
}

TypeRef HighCWriter::typedMemberType(const HighExpr &Addr,
                                     uint16_t AccessSize) const {
  if (const auto Disp = frameDisplacement(Addr))
    if (TypeRef FrameTy = frameTypedMemberType(*Disp, AccessSize))
      return FrameTy;
  const auto Peeled = typedPointerOffset(Addr);
  if (!Peeled)
    return nullptr;
  if (const auto OverlayDisp =
          frameOverlayDisplacement(*Peeled->first, Peeled->second))
    if (TypeRef OverlayTy = frameTypedMemberType(*OverlayDisp, AccessSize))
      return OverlayTy;
  const TypeRef Record = declaredRecordPointee(*Peeled->first);
  if (!Record)
    return nullptr;
  completeDisplayRecord(Dbg, Record);
  return Record->displayFieldTypeAt(Peeled->second, AccessSize);
}

std::string HighCWriter::unknownVarUse(const HighExpr &E,
                                       const std::string &Name,
                                       const std::string &RawName) {
  // Definitions consisting only of unknown values are omitted from C.
  // Their observable uses must still fail, including renamed SSA temps.
  if (UnknownOnlyNames.count(Name))
    return "(__builtin_trap(), 0 /* unknown value */)";
  // An unassigned architectural register or flag can be a genuine unknown
  // live-in. Keep the failure at the point of use instead of emitting an
  // undeclared name or inventing zero.
  if ((E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Flag) &&
      E.Var.SSAVer == 0 && Name == RawName && !AssignedNames.count(RawName) &&
      !isEmittedParamName(RawName) &&
      !(CurrentFunc &&
        isSyntheticEntryStackPointer(E.Var, *CurrentFunc, Opts.TheArch))) {
    if (x87HelperFor(E) == X87CHelper::ControlWord)
      return useX87Helper(X87CHelper::ControlWord) + "()";
    return "(__builtin_trap(), 0 /* unknown register */)";
  }
  return {};
}

std::string HighCWriter::pointerObjectStr(const HighExpr &E) {
  const HighExpr *Inner = peelIntegerViewOps(&E);
  if (!Inner)
    Inner = &E;
  if (auto Member = typedMemberAccess(*Inner))
    return *Member;
  if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
      Inner->Operands[0] && Inner->MemoryOrdering == NdMemoryOrdering::None &&
      Inner->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
    if (auto Member = typedMemberAccess(*Inner->Operands[0]))
      return *Member;
  }
  if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
    const std::string Name = copyForwardName(varName(Inner->Var));
    if (auto It = FieldForward.find(Name); It != FieldForward.end())
      return It->second;
    if (auto Printed = printedForwardedVar(Name, 16);
        !Printed.empty() && Printed != Name)
      return Printed;
    // An unknown value fails at this use too, instead of printing a name
    // nothing declares.
    if (std::string Unknown = unknownVarUse(*Inner, Name, varName(Inner->Var));
        !Unknown.empty())
      return Unknown;
    return Name;
  }
  return exprStr(*Inner);
}

std::string HighCWriter::indirectCalleeStr(const HighExpr &E,
                                           const TypeRef &ReturnType) {
  // C calls only function pointers. A code address loaded as `void *` or held
  // in an integer is called through an unprototyped pointer to a function
  // returning what the call yields.
  const std::string CalleeType =
      "(" +
      declarationToC(ReturnType ? cDisplayType(ReturnType) : NdType::makeVoid(),
                     "(*)()") +
      ")";
  auto UntypedCallee = [&](const std::string &Code) {
    return "(" + CalleeType + Code + ")";
  };
  const unsigned PtrSize = Opts.TheArch == Arch::X86 ? 4u : 8u;
  const HighExpr *Cur = peelIntegerViewOps(&E);
  unsigned Depth = 0;
  const HighExpr *Base = Cur;
  while (Cur && Depth < 2) {
    if (Cur->Kind != ExprKind::Load || Cur->Operands.empty() ||
        !Cur->Operands[0] || Cur->MemoryOrdering != NdMemoryOrdering::None ||
        Cur->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      break;
    const uint16_t Size = Cur->Type ? Cur->Type->Size : 0;
    if (Size != 0 && Size != PtrSize)
      break;
    ++Depth;
    Base = Cur->Operands[0].get();
    Cur = peelIntegerViewOps(Base);
  }
  // A slot of this function's frame prints as its local, the way every other
  // load of it does.
  if (Depth == 1 && Base && (namedFrameSlot(*Base) || frameDisplacement(*Base)))
    return UntypedCallee("(uintptr_t)(" + exprStr(E) + ")");
  // Slot load of `*(vtbl + imm)` where `vtbl` is `*obj` (MSVC vfptr at 0).
  // Keep the inner vfptr load; `obj + imm` would name a field, not a method.
  if (Depth >= 1 && Base) {
    const HighExpr *Addr = peelIntegerViewOps(Base);
    if (Addr && Addr->Kind == ExprKind::BinOp && Addr->Op == NdOp::INT_ADD &&
        Addr->Operands.size() == 2 && Addr->Operands[0] && Addr->Operands[1]) {
      const HighExpr *Lhs = peelIntegerViewOps(Addr->Operands[0].get());
      const HighExpr *Rhs = peelIntegerViewOps(Addr->Operands[1].get());
      const HighExpr *Ptr = nullptr;
      const HighExpr *Off = nullptr;
      if (Rhs && Rhs->Kind == ExprKind::Const) {
        Ptr = Lhs;
        Off = Rhs;
      } else if (Lhs && Lhs->Kind == ExprKind::Const) {
        Ptr = Rhs;
        Off = Lhs;
      }
      if (Ptr && Off && Off->Kind == ExprKind::Const && Off->ConstVal != 0) {
        const HighExpr *Vptr = peelIntegerViewOps(Ptr);
        std::string SlotBase;
        if (Vptr && Vptr->Kind == ExprKind::Load && !Vptr->Operands.empty() &&
            Vptr->Operands[0]) {
          // Address of the vfptr load is the object pointer.  If that
          // pointer is itself a field load, print the field; do not call
          // typedMemberAccess on the Load (offset-0 of TPtr would add `->p`).
          const HighExpr *Obj = peelIntegerViewOps(Vptr->Operands[0].get());
          std::string ObjStr;
          if (Obj && Obj->Kind == ExprKind::Load && !Obj->Operands.empty() &&
              Obj->Operands[0]) {
            const HighExpr *FieldAddr =
                peelIntegerViewOps(Obj->Operands[0].get());
            if (FieldAddr)
              if (auto Member = typedMemberAccess(*FieldAddr))
                ObjStr = *Member;
            if (ObjStr.empty())
              ObjStr = pointerObjectStr(*Obj);
          } else if (Obj) {
            if (auto Member = typedMemberAccess(*Obj))
              ObjStr = *Member;
            if (ObjStr.empty())
              ObjStr = pointerObjectStr(*Obj);
          } else {
            ObjStr = pointerObjectStr(*Vptr->Operands[0]);
          }
          SlotBase = "*(void **)(" + ObjStr + ")";
        } else {
          SlotBase = pointerObjectStr(*Ptr);
        }
        return UntypedCallee("(*(void **)((uintptr_t)(" + SlotBase + ") + " +
                             constStr(Off->ConstVal) + "))");
      }
    }
  }
  if (Depth == 0) {
    if (E.Type && E.Type->Kind == NdTypeKind::Ptr && E.Type->Pointee &&
        E.Type->Pointee->Kind == NdTypeKind::Func)
      return "(*" + exprStr(E) + ")";
    return UntypedCallee("(uintptr_t)(" + exprStr(E) + ")");
  }
  // A slot of the image the code calls through prints as its object.
  if (Depth == 1)
    if (auto VA = constAddress(*Base))
      if (auto Name = imageObjectName(*VA))
        return UntypedCallee(*Name);
  std::string B = pointerObjectStr(*Base);
  std::string Stars(Depth, '*');
  std::string Ptrs(Depth + 1, '*');
  return UntypedCallee("(" + Stars + "(void " + Ptrs + ")(" + B + "))");
}

bool HighCWriter::pointerNeedsIntegerView(const TypeRef &Ty) const {
  // Machine carriers also participate in shifts and masks. Even byte
  // pointers, whose addition happens to have scale one, need an integer view.
  return Ty && Ty->Kind == NdTypeKind::Ptr;
}

bool HighCWriter::isSameWidthUnsigned(const HighExpr &E, uint16_t Width) const {
  if (Width == 0 || Width > 16)
    return false;
  const HighExpr *P = forwardedExpr(&E);
  if (!P)
    return false;
  auto Matches = [&](const TypeRef &Ty) {
    return Ty && Ty->Kind == NdTypeKind::Int && !Ty->IsSigned && !Ty->IsEnum &&
           Ty->Size == Width;
  };
  if (P->Kind == ExprKind::Load && !P->Operands.empty() && P->Operands[0])
    return Matches(typedMemberType(*P->Operands[0], Width));
  if (P->Kind == ExprKind::Var || P->Kind == ExprKind::Phi) {
    if (TypeRef Decl = declaredParamType(P->Var))
      return Matches(Decl);
    return Matches(P->Type);
  }
  return false;
}

std::string HighCWriter::floatOperandStr(const HighExpr &Operand,
                                         int ParentPrec) {
  if (computesWider(Operand.Type))
    if (const HighExpr *Printed = forwardedExpr(&Operand);
        Printed && isFloatArithmetic(*Printed))
      return "(" + typeToC(Operand.Type) + ")(" + exprStr(Operand) + ")";
  return exprStr(Operand, ParentPrec);
}

const HighExpr *HighCWriter::forwardedExpr(const HighExpr *E) const {
  unsigned Depth = 0;
  while (E && Depth++ < limits::kMaxIntegerViewUnwrapDepth) {
    E = unwrapIntegerView(E);
    if (!E)
      return nullptr;
    if (E->Kind != ExprKind::Var && E->Kind != ExprKind::Phi)
      return E;
    auto It = ValueForward.find(varName(E->Var));
    if (It == ValueForward.end() || !It->second || It->second == E)
      return E;
    E = It->second;
  }
  return E;
}

const HighExpr *HighCWriter::sameWidthVariable(const HighExpr &E) const {
  // Integers and pointers, which convert by their bits; a float converts by
  // its value.
  auto WidthOf = [](const HighExpr &Node) -> uint16_t {
    if (Node.Type)
      return Node.Type->Kind == NdTypeKind::Int ||
                     Node.Type->Kind == NdTypeKind::Ptr
                 ? Node.Type->Size
                 : 0;
    return Node.Kind == ExprKind::Var || Node.Kind == ExprKind::Phi
               ? Node.Var.Size
               : 0;
  };
  const uint16_t Width = WidthOf(E);
  const HighExpr *Cur = &E;
  for (unsigned Depth = 0;
       Cur && Width && Depth < limits::kMaxIntegerViewUnwrapDepth; ++Depth) {
    if (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi)
      return WidthOf(*Cur) == Width ? Cur : nullptr;
    const bool View =
        Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast ||
        (Cur->Kind == ExprKind::UnaryOp &&
         (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT)) ||
        (Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::SUBBYTES &&
         Cur->Operands.size() == 2 && Cur->Operands[1] &&
         Cur->Operands[1]->Kind == ExprKind::Const &&
         Cur->Operands[1]->ConstVal == 0);
    if (!View || Cur->Operands.empty() || !Cur->Operands[0] ||
        WidthOf(*Cur) != Width || WidthOf(*Cur->Operands[0]) != Width)
      return nullptr;
    Cur = Cur->Operands[0].get();
  }
  return nullptr;
}

const HighExpr *HighCWriter::unwrapIntegerView(const HighExpr *E) const {
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

std::optional<int64_t> HighCWriter::frameDisplacement(const HighExpr &E) const {
  if (!CurrentFunc ||
      FrameDisplacementDepth >= limits::kMaxFrameDisplacementDepth)
    return std::nullopt;
  ++FrameDisplacementDepth;
  struct DepthGuard {
    unsigned &Depth;
    ~DepthGuard() { --Depth; }
  } Guard{FrameDisplacementDepth};
  const auto &Slots =
      ProjectFrameAliasesIntoStorage ? FrameStorageSlots : FrameSlots;
  const HighExpr *Cur = unwrapIntegerView(&E);
  int64_t Acc = 0;
  unsigned Depth = 0;
  while (Cur && Depth++ < limits::kMaxFrameDisplacementDepth) {
    Cur = unwrapIntegerView(Cur);
    if (!Cur)
      return std::nullopt;
    if (Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::INT_AND &&
        Cur->Operands.size() == 2 && Cur->Operands[0] && Cur->Operands[1] &&
        Cur->Type && (Cur->Type->Size == 4 || Cur->Type->Size == 8)) {
      const HighExpr *LHS = unwrapIntegerView(Cur->Operands[0].get());
      const HighExpr *RHS = unwrapIntegerView(Cur->Operands[1].get());
      if (!LHS || !RHS)
        return std::nullopt;
      const HighExpr *Mask = RHS->Kind == ExprKind::Const ? RHS : LHS;
      const HighExpr *Base = Mask == RHS ? LHS : RHS;
      if (Mask->Kind != ExprKind::Const)
        return std::nullopt;
      const uint64_t WidthMask = Cur->Type->Size == 4 ? UINT32_MAX : UINT64_MAX;
      const uint64_t Cleared = ~Mask->ConstVal & WidthMask;
      const uint64_t Alignment = Cleared + 1;
      if (!Cleared || Alignment > kSyntheticStackAlignment ||
          (Alignment & Cleared) != 0 || CurrentFunc->FrameSize <= 0 ||
          Cur->Type->Size != getTargetRegInfo(Opts.TheArch).PointerSize)
        return std::nullopt;
      const auto BaseDisp = certifiedFrameStorageDisplacement(*Base);
      if (!BaseDisp || *BaseDisp < -CurrentFunc->FrameSize || *BaseDisp > 0)
        return std::nullopt;
      const int64_t Residue = static_cast<int64_t>(syntheticEntryStackResidue(
          Opts.TheArch, Opts.Format, CurrentFunc->EntryKind));
      const int64_t Position = *BaseDisp + Residue;
      const int64_t Remainder = (Position % static_cast<int64_t>(Alignment) +
                                 static_cast<int64_t>(Alignment)) %
                                static_cast<int64_t>(Alignment);
      if (*BaseDisp < INT64_MIN + Remainder)
        return std::nullopt;
      const int64_t AlignedDisp = *BaseDisp - Remainder;
      if ((Acc > 0 && AlignedDisp > INT64_MAX - Acc) ||
          (Acc < 0 && AlignedDisp < INT64_MIN - Acc))
        return std::nullopt;
      // The synthetic byte buffer shares this alignment and entry residue;
      // rounding a proved in-frame address stays within its padded lower end.
      return Acc + AlignedDisp;
    }
    if (Cur->Kind == ExprKind::Var) {
      if (isCatchFuncletParentFrame(Cur->Var)) {
        // rdx is the parent's established frame, the same rebase SEH handlers
        // use so [rdx+k] names the try body's var_mN slots.
        if (CurrentFunc->FrameSize > 0)
          return Acc - CurrentFunc->FrameSize;
        return Acc;
      }
      // SSA version zero is always the architectural entry SP. Exceptional
      // establisher adjustments are explicit shared MedIR definitions; doing
      // another frame-size rebase here changes the handler's memory identity.
      if (isSyntheticEntryStackPointer(Cur->Var, *CurrentFunc, Opts.TheArch))
        return Acc;
      auto Alias = FrameAliases.find(varName(Cur->Var));
      if (Alias != FrameAliases.end())
        return Acc + Alias->second;
      if (CurrentFunc->ExceptionMetadata && Cur->Var.Kind == MedVar::Reg &&
          Cur->Var.RenameTag < 0 &&
          Cur->Var.RegOff == getTargetRegInfo(Opts.TheArch).FramePointer) {
        const int64_t Slot =
            static_cast<int64_t>(getTargetRegInfo(Opts.TheArch).PointerSize);
        // The x86 registration frame is restored explicitly by shared MedIR.
        // Without that proof an arbitrary incoming EBP is not entry ESP - 4.
        if (Opts.TheArch == Arch::X86)
          return std::nullopt;
        const int64_t Order[3] = {Acc - Slot, Acc, Acc + Slot};
        for (int64_t Adj : Order)
          if (Slots.count(Adj))
            return Adj;
        return Acc;
      }
      // Incoming SP sometimes survives as an unassigned SSA 0 temp after
      // prologue lowering. Treat it as the frame so `sp+k` can name a slot.
      if (Acc != 0 && CurrentFunc->FrameSize > 0 && Cur->Var.SSAVer == 0 &&
          Cur->Var.RenameTag < 0 && Cur->Var.Kind != MedVar::Param &&
          !Analysis.AssignedVars.count(varName(Cur->Var)))
        return Acc;
    }
    if (Cur->Kind != ExprKind::BinOp || Cur->Operands.size() != 2 ||
        !Cur->Operands[0] || !Cur->Operands[1] ||
        (Cur->Op != NdOp::INT_ADD && Cur->Op != NdOp::INT_SUB))
      return std::nullopt;
    const HighExpr *LHS = unwrapIntegerView(Cur->Operands[0].get());
    const HighExpr *RHS = unwrapIntegerView(Cur->Operands[1].get());
    if (!LHS || !RHS)
      return std::nullopt;
    const HighExpr *Base = nullptr;
    const HighExpr *Imm = nullptr;
    const bool Subtract = Cur->Op == NdOp::INT_SUB;
    if (RHS->Kind == ExprKind::Const) {
      Base = LHS;
      Imm = RHS;
    } else if (!Subtract && LHS->Kind == ExprKind::Const) {
      Base = RHS;
      Imm = LHS;
    } else
      return std::nullopt;
    int64_t Delta = static_cast<int64_t>(Imm->ConstVal);
    if (Imm->Type && Imm->Type->Size == 4)
      Delta = static_cast<int32_t>(Imm->ConstVal);
    Acc += Subtract ? -Delta : Delta;
    Cur = Base;
  }
  return std::nullopt;
}

bool HighCWriter::isRegistrationEstablisherFrame(const MedVar &V) const {
  return InEHClauseBody && Opts.TheArch == Arch::X86 && CurrentFunc &&
         CurrentFunc->ExceptionMetadata &&
         CurrentFunc->ExceptionMetadata->Registration &&
         V.Kind == MedVar::Reg && V.SSAVer == 0 && V.RenameTag < 0 &&
         V.RegOff == getTargetRegInfo(Opts.TheArch).FramePointer;
}

std::optional<int64_t>
HighCWriter::certifiedFrameStorageDisplacement(const HighExpr &E) const {
  if (!CurrentFunc)
    return std::nullopt;
  // frameDisplacement also has a display-only heuristic for an unassigned
  // SSA-0 value. That is insufficient to redirect a real memory address into
  // stack_storage: require a known frame root along a constant-offset chain.
  const HighExpr *Cur = &E;
  unsigned Depth = 0;
  while (Cur && Depth++ < limits::kMaxFrameDisplacementDepth) {
    Cur = unwrapIntegerView(Cur);
    if (!Cur)
      return std::nullopt;
    if (Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::INT_AND)
      return frameDisplacement(E);
    if (Cur->Kind == ExprKind::Var) {
      if (!isSyntheticEntryStackPointer(Cur->Var, *CurrentFunc, Opts.TheArch) &&
          !isCatchFuncletParentFrame(Cur->Var) &&
          !isRegistrationEstablisherFrame(Cur->Var) &&
          !FrameAliases.count(varName(Cur->Var)))
        return std::nullopt;
      return frameDisplacement(E);
    }
    if (Cur->Kind != ExprKind::BinOp || Cur->Operands.size() != 2 ||
        !Cur->Operands[0] || !Cur->Operands[1] ||
        (Cur->Op != NdOp::INT_ADD && Cur->Op != NdOp::INT_SUB))
      return std::nullopt;
    const HighExpr *LHS = unwrapIntegerView(Cur->Operands[0].get());
    const HighExpr *RHS = unwrapIntegerView(Cur->Operands[1].get());
    if (!LHS || !RHS)
      return std::nullopt;
    if (RHS->Kind == ExprKind::Const)
      Cur = LHS;
    else if (Cur->Op == NdOp::INT_ADD && LHS->Kind == ExprKind::Const)
      Cur = RHS;
    else
      return std::nullopt;
  }
  return std::nullopt;
}

std::optional<std::string>
HighCWriter::namedFrameSlot(const HighExpr &E, const TypeRef &Access) const {
  const auto Disp = frameDisplacement(E);
  if (!Disp)
    return std::nullopt;
  const auto It = FrameSlots.find(*Disp);
  if (It == FrameSlots.end())
    return std::nullopt;
  const NamedFrameSlot &Slot = It->second;
  const bool Narrow =
      Access && Access->Size && Slot.Type && Access->Size < Slot.Type->Size;
  if (!Slot.Interior.empty())
    return Narrow
               ? "(*(" + memoryTypeName(Access) + " *)((char *)&" + Slot.Outer +
                     " + " + std::to_string(Slot.OuterOffset) + "))"
               : Slot.Interior;
  if (Narrow)
    return "(*(" + memoryTypeName(Access) + " *)&" + Slot.Name + ")";
  return Slot.Name;
}

bool HighCWriter::isNamedFrameMemory(const HighExpr &E) const {
  if ((E.Kind == ExprKind::Load || E.Kind == ExprKind::Store) &&
      !E.Operands.empty() && E.Operands[0] && namedFrameSlot(*E.Operands[0]))
    return true;
  if (E.Kind == ExprKind::Addr && !E.Operands.empty() && E.Operands[0] &&
      E.Operands[0]->Kind == ExprKind::Load &&
      !E.Operands[0]->Operands.empty() && E.Operands[0]->Operands[0] &&
      namedFrameSlot(*E.Operands[0]->Operands[0]))
    return true;
  return false;
}

std::string HighCWriter::exprStr(const HighExpr &E, int ParentPrec,
                                 MemoryLoadDestination *Destination) {
  PrintedIntegerTypes.erase(&E);
  UntypedArithmeticTexts.erase(&E);
  UnsignedCarrierTexts.erase(&E);
  std::string Text = exprStrImpl(E, ParentPrec, Destination);
  // A local's name has its declared type.
  if (!Text.empty() && (llvm::isAlpha(Text.front()) || Text.front() == '_'))
    if (const auto Declared = DeclaredCTypes.find(Text);
        Declared != DeclaredCTypes.end() && isPlainInteger(Declared->second))
      PrintedIntegerTypes[&E] = {Declared->second->Size,
                                 Declared->second->IsSigned};
  return SourceRecorder && CurrentFunc
             ? SourceRecorder->expression(CurrentFunc->Entry, E,
                                          std::move(Text))
             : Text;
}

std::string HighCWriter::exprStrImpl(const HighExpr &E, int ParentPrec,
                                     MemoryLoadDestination *Destination) {
  // On a 64-bit target the source is built on a 64-bit host, where a
  // pointer converted to uintptr_t is the unsigned 64-bit integer already.
  auto PointerInteger = [&](std::string Text) {
    if (getTargetRegInfo(Opts.TheArch).PointerSize != sizeof(uint64_t))
      return Text;
    return typedText(E, std::move(Text), sizeof(uint64_t), /*IsSigned=*/false);
  };
  static thread_local int Depth = 0;
  struct Guard {
    int &D;
    Guard(int &D_) : D(D_) { ++D; }
    ~Guard() { --D; }
  };
  Guard G(Depth);
  if (Depth > limits::kMaxCExprPrintDepth)
    return "(0 /* truncated: expr too deep */)";

  switch (E.Kind) {
  case ExprKind::EntryRegister:
    return registrationEntryExpression(E);
  case ExprKind::Var:
  case ExprKind::Phi: {
    // HighIR arithmetic still operates on machine bytes when source type
    // recovery gives an ABI parameter a pointed-to type.  Convert the value
    // before any operation: casting the final load address is too late to
    // prevent C's element-scaled pointer arithmetic.  Consult the declaration
    // because a machine-width expression can retain its original integer type.
    auto DeclaredType = declaredParamType(E.Var);
    const std::string RawName = varName(E.Var);
    std::string Name = copyForwardName(RawName);
    // A copy forwarded to a parameter prints as that parameter, so it has
    // the parameter's declared type.
    if (!DeclaredType && CurrentFunc)
      for (const HighParam &Param : CurrentFunc->Params)
        if (Param.Name == Name) {
          DeclaredType = Param.Type;
          break;
        }
    if (auto Reach = ReachingCatchFields.find(Name);
        Reach != ReachingCatchFields.end())
      return Reach->second;
    if (auto Reach = ReachingCatchPtrs.find(Name);
        Reach != ReachingCatchPtrs.end())
      return Reach->second;
    if (InEHClauseBody && isCatchFuncletParentFrame(E.Var))
      return frameStorageAddress(
          CurrentFunc->FrameSize > 0 ? -CurrentFunc->FrameSize : 0);
    // The registration handler's EBP already has an establisher identity in
    // frameDisplacement. Keep that identity when named slots are replaced by
    // byte storage; an independent SSA root does not make it an unknown input.
    if (FrameStorageActive && isRegistrationEstablisherFrame(E.Var))
      if (const auto Disp = frameDisplacement(E))
        return frameStorageAddress(*Disp);
    if (ProjectFrameAliasesIntoStorage) {
      if (CurrentFunc &&
          isSyntheticEntryStackPointer(E.Var, *CurrentFunc, Opts.TheArch))
        return "frame_base";
      // A frame pointer may be assigned only on a normal try path. Project
      // its certified displacement at each use so the exceptional edge also
      // addresses the single byte backing store, rather than reading that
      // potentially skipped C assignment.
      if (auto Alias = FrameAliases.find(RawName); Alias != FrameAliases.end())
        return frameStorageAddress(Alias->second);
      for (const auto &[Disp, Slot] : FrameStorageSlots)
        if (Slot.Name == Name && E.Var.Kind != MedVar::Param &&
            !isEmittedParamName(Name) && !isCxxCatchObjectName(Name))
          return bareLoad(memoryLoadExpr(Slot.Type ? Slot.Type : E.Type,
                                         frameStorageAddress(Disp)),
                          ParentPrec);
    }
    const HighExpr *Forwarded = nullptr;
    if (auto Printed = printedForwardedVar(Name, ParentPrec, &Forwarded);
        Printed != Name) {
      // The definition printed in place keeps the type of its own text;
      // arithmetic of no known type reads as C converts its operands.
      if (Forwarded) {
        if (const auto Typed = printedIntegerType(*Forwarded))
          PrintedIntegerTypes[&E] = *Typed;
        else if (printsIntegerArithmetic(*Forwarded))
          UntypedArithmeticTexts.insert(&E);
      }
      return Printed;
    }
    if (std::string Unknown = unknownVarUse(E, Name, RawName); !Unknown.empty())
      return Unknown;
    if (auto Slot = namedFrameSlot(E)) {
      if (const auto Disp = frameDisplacement(E)) {
        auto It = FrameSlots.find(*Disp);
        if (It != FrameSlots.end() && It->second.UsedAsMemory)
          return IntegerViewOperands.count(&E)
                     ? PointerInteger("(uintptr_t)&" + *Slot)
                     : "&" + *Slot;
      }
    }
    if (pointerNeedsIntegerView(DeclaredType))
      return PointerInteger("(uintptr_t)" + Name);
    if (DeclaredType &&
        DeclaredType->SourceName == kSourceAArch64Vector128CType)
      return "__builtin_bit_cast(__int128, " + Name + ")";
    return Name;
  }
  case ExprKind::Const: {
    // Numeric equality with a collected global or string is not pointer
    // provenance. Explicit scalar/fragment occurrences must stay numeric even
    // when another memory use materializes an object at the same image VA.
    // LOAD/STORE addresses use addrStr() separately to project actual memory.
    if (E.ConstProvenance == ConstantAddressProvenance::Scalar ||
        E.ConstProvenance == ConstantAddressProvenance::AddressFragment)
      return constStr(E.ConstVal, E.Type);
    bool AllowEmpty = false;
    if (Dbg) {
      if (auto Data = Dbg->resolveDataObject(E.ConstVal);
          Data && llvm::StringRef(Data->Name).starts_with("??_C@"))
        AllowEmpty = true;
    }
    if (auto Lit = imageStringLiteral(Opts.Image, E.ConstVal, AllowEmpty))
      return LiteralAddressOperands.count(&E)
                 ? PointerInteger("(uintptr_t)" + *Lit)
                 : *Lit;
    // Preserve the existing exact-object spelling, but do not turn an
    // unrelated numeric immediate that happens to lie inside a backing range
    // into an address.
    // A bitwise, shift or multiplicative operator takes the address as an
    // integer; C has no such operator on a pointer.
    if (ImageObjects.count(E.ConstVal) ||
        E.ConstProvenance == ConstantAddressProvenance::Address ||
        E.ConstProvenance == ConstantAddressProvenance::DataAddress)
      if (auto Backing = imageBackingAddress(E.ConstVal)) {
        // An integer the machine computes with stays an integer: `~` or a
        // multiplication takes no pointer.
        if (E.Type && E.Type->Kind == NdTypeKind::Int && !E.Type->IsEnum)
          return E.Type->Size == sizeof(uint64_t) && !E.Type->IsSigned &&
                         getTargetRegInfo(Opts.TheArch).PointerSize ==
                             sizeof(uint64_t)
                     ? typedText(E, "(uintptr_t)" + *Backing, E.Type->Size,
                                 /*IsSigned=*/false)
                     : "(" + typeToC(E.Type) + ")(uintptr_t)" + *Backing;
        return IntegerViewOperands.count(&E)
                   ? PointerInteger("(uintptr_t)" + *Backing)
                   : *Backing;
      }
    if (auto Name = imageObjectName(E.ConstVal)) {
      // An array is its own address in C, which a string parameter takes.
      const auto Object = ImageObjects.find(E.ConstVal);
      const bool Array = Object != ImageObjects.end() && Object->second.String;
      if (Array && PointerArgumentOperands.count(&E))
        return *Name;
      const std::string Address = Array ? *Name : "&" + *Name;
      // Replacing a machine integer address with a C object pointer must
      // preserve the expression's integer type (for example, a block
      // descriptor address stored through a uint64_t memory helper).
      if (E.Type && E.Type->Kind == NdTypeKind::Int) {
        // A 64-bit target's source is built on a 64-bit host, where uintptr_t
        // is that unsigned integer already.
        if (!E.Type->IsSigned && !E.Type->IsEnum &&
            E.Type->Size == sizeof(uint64_t) &&
            getTargetRegInfo(Opts.TheArch).PointerSize == sizeof(uint64_t))
          return typedText(E, "(uintptr_t)" + Address, E.Type->Size,
                           /*IsSigned=*/false);
        return "(" + typeToC(E.Type) + ")(uintptr_t)(" + Address + ")";
      }
      return IntegerViewOperands.count(&E)
                 ? PointerInteger("(uintptr_t)" + Address)
                 : Address;
    }
    // A function's address prints as the function, whose designator C
    // converts to its address; it keeps the integer type the machine moves.
    if (isAddressProvenance(E.ConstProvenance))
      if (const auto Function = FunctionAddressNames.find(E.ConstVal);
          Function != FunctionAddressNames.end()) {
        if (PointerArgumentOperands.count(&E))
          return Function->second;
        if (E.Type && E.Type->Kind == NdTypeKind::Int)
          return "(" + typeToC(E.Type) + ")(uintptr_t)" + Function->second;
        return IntegerViewOperands.count(&E) ? "(uintptr_t)" + Function->second
                                             : Function->second;
      }
    return constStr(E.ConstVal, E.Type);
  }
  case ExprKind::Undef:
    // An observed unknown value has no recovered semantics. Fail at this
    // use; an unchosen conditional operand must not fail eagerly.
    return "(__builtin_trap(), 0 /* unknown value */)";
  case ExprKind::BinOp:
    if (ProjectFrameAliasesIntoStorage)
      if (const auto Disp = certifiedFrameStorageDisplacement(E))
        return frameStorageAddress(*Disp);
    if (auto Slot = namedFrameSlot(E))
      return IntegerViewOperands.count(&E)
                 ? PointerInteger("(uintptr_t)&" + *Slot)
                 : "&" + *Slot;
    if (auto Member = typedMemberAddress(E))
      return "&" + *Member;
    return renderBinOp(E, ParentPrec);
  case ExprKind::UnaryOp:
    return renderUnaryOp(E, ParentPrec);
  case ExprKind::Load: {
    if (CurrentFunc && E.Operands.size() == 1 &&
        isSwiftErrorEntrySlot(*CurrentFunc, E.Operands.front()) && E.Type &&
        E.Type->Kind == NdTypeKind::Ptr && E.Type->Size == 8 &&
        E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      return "(*" + varName(E.Operands.front()->Var) + ")";
    if (E.Operands.empty())
      return "/* bad load */";
    // A plain access through an integer alias has exactly the alias's type:
    // `*(_QWORD *)p` is a uint64_t.
    auto Access = [&](std::string Text) {
      if (!isPlainInteger(E.Type) ||
          E.MemoryOrdering != NdMemoryOrdering::None ||
          E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
        return Text;
      return typedText(E, std::move(Text), E.Type->Size, E.Type->IsSigned);
    };
    if (E.MemoryAddressSpace == NdMemoryAddressSpace::Default &&
        E.MemoryOrdering == NdMemoryOrdering::None && E.Type && Opts.Image &&
        E.Type->Size == Opts.Image->getPointerSize())
      if (auto Slot = constAddress(*E.Operands[0]))
        if (const auto It = DataSymbolBindings.find(*Slot);
            It != DataSymbolBindings.end() && It->second.Immutable)
          if (const auto Address = dataSymbolAddress(*Slot))
            return "(" + typeToC(E.Type) + ")" + *Address;
    if (E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto VA = constAddress(*E.Operands[0])) {
        if (imageBackingAddress(*VA))
          return Access(bareLoad(
              memoryLoadExpr(E.Type, addrStr(*E.Operands[0]), E.MemoryOrdering,
                             E.MemoryAddressSpace, true, Destination),
              ParentPrec));
      }
    }
    if (E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto Member =
              typedMemberAccess(*E.Operands[0], E.Type ? E.Type->Size : 0))
        return *Member;
      if (auto Index = typedIndexAccess(*E.Operands[0]))
        return Index->Base + "[" + Index->Index + "]";
      if (auto Field = cxxCatchFieldAccess(*E.Operands[0]))
        return *Field;
    }
    std::string Addr =
        addrStr(*E.Operands[0], 0,
                E.MemoryAddressSpace == NdMemoryAddressSpace::Default);
    if (E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto Fwd = forwardedStoreValue(*E.Operands[0], Addr))
        return "(" + typeToC(E.Type) + ")(" + *Fwd + ")";
      if (auto Slot = namedSlotLoadDisplay(E))
        return *Slot;
      // A data import's slot holds the import's address, which the read
      // takes: `(int64_t)__imp__commode`.
      if (const auto Slot =
              importDataSlotRead(*E.Operands[0], E.Type ? E.Type->Size : 0))
        if (const auto Name = ImportDataSlotNames.find(*Slot);
            Name != ImportDataSlotNames.end())
          return "(" + typeToC(E.Type) + ")" + Name->second;
      if (auto VA = constAddress(*E.Operands[0])) {
        const uint16_t Size = E.Type ? E.Type->Size : 0;
        if (auto Imm = foldReadonlyScalar(*VA, Size)) {
          // A float read from constant data is its value: the bits printed
          // as an integer would convert to another one.
          if (const auto Float = floatConstantText(*Imm, E.Type))
            return *Float;
          return constStr(*Imm);
        }
        if (auto Name = imageObjectName(*VA)) {
          // A string's pointer reads as the integer the machine loads where
          // no parameter takes it as the pointer it is.
          if (const auto Object = ImageObjects.find(*VA);
              Object != ImageObjects.end() && Object->second.PointsTo &&
              !PointerArgumentOperands.count(&E) && E.Type &&
              E.Type->Kind != NdTypeKind::Ptr)
            return "(" + typeToC(E.Type) + ")" + *Name;
          return *Name;
        }
      }
      const TypeRef &AddressType = E.Operands[0]->Type;
      if (AddressType && AddressType->Kind == NdTypeKind::Ptr &&
          AddressType->Pointee && E.Type &&
          (E.Type->Kind != NdTypeKind::Int || E.Type->Size == 1 ||
           E.Type->Size == 2 || E.Type->Size == 4 || E.Type->Size == 8 ||
           E.Type->Size == 16 || E.Type->Size == 32 || E.Type->Size == 64) &&
          equalSourceTypes(AddressType->Pointee, E.Type))
        return Access(bareLoad(
            "(*(" + memoryTypeName(E.Type) + " *)(" + Addr + "))", ParentPrec));
    }
    return Access(
        bareLoad(memoryLoadExpr(E.Type, Addr, E.MemoryOrdering,
                                E.MemoryAddressSpace, false, Destination),
                 ParentPrec));
  }
  case ExprKind::Store: {
    if (E.Operands.size() < 2)
      return "/* bad store */";
    std::string Addr =
        addrStr(*E.Operands[0], 0,
                E.MemoryAddressSpace == NdMemoryAddressSpace::Default);
    std::string Val = exprStr(*E.Operands[1]);
    if (E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto VA = constAddress(*E.Operands[0])) {
        if (imageBackingAddress(*VA))
          return memoryStoreExpr(E.Operands[1]->Type, Addr, Val,
                                 E.MemoryOrdering, E.MemoryAddressSpace, true);
      }
    }
    if (E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto Member = typedMemberAccess(*E.Operands[0],
                                          E.Operands[1] && E.Operands[1]->Type
                                              ? E.Operands[1]->Type->Size
                                              : 0))
        return *Member + " = " + Val;
      if (auto Slot = namedFrameSlot(*E.Operands[0]))
        return *Slot + " = " + Val;
    }
    return memoryStoreExpr(E.Operands[1]->Type, Addr, Val, E.MemoryOrdering,
                           E.MemoryAddressSpace);
  }
  case ExprKind::Call: {
    std::string Text = renderCallExpr(E);
    // A routine returning a pointer returns it in the integer register the
    // machine reads, as a function's address is one: arithmetic on it adds
    // bytes.  A call assigned to a variable has the variable's type.
    if (&E != StatementCall && (!E.Type || E.Type->Kind == NdTypeKind::Int))
      if (const libc::LibCPrototype *Prototype = calleePrototype(E);
          Prototype && libc::isPointerType(Prototype->Return))
        return E.Type && (E.Type->Size != pointerBytes(Opts.TheArch) ||
                          E.Type->IsSigned)
                   ? "(" + typeToC(E.Type) + ")(uintptr_t)" + Text
                   : PointerInteger("(uintptr_t)" + Text);
    // A floating result returns in the low lane of a register the HighIR
    // reads as integer bits: the C reads those bits, not the value converted
    // to an integer.  The lane's other bytes are unspecified after a call.
    if (&E != StatementCall && (!E.Type || E.Type->Kind == NdTypeKind::Int))
      if (const TypeRef Return = knownCallReturnType(E);
          Return && Return->Kind == NdTypeKind::Float &&
          (Return->Size == sizeof(float) || Return->Size == sizeof(double)))
        return typedText(E,
                         "__builtin_bit_cast(" +
                             typeToC(NdType::makeInt(Return->Size, false)) +
                             ", " + Text + ")",
                         Return->Size, false);
    // `callee(...)` has the return type its declaration prints.
    if (const TypeRef Return = knownCallReturnType(E); isPlainInteger(Return)) {
      const std::string Prefix = callIdentifier(E) + "(";
      if (llvm::StringRef(Text).starts_with(Prefix) && Text.back() == ')' &&
          c_memory::castOperand(Text.substr(Prefix.size() - 1)) ==
              Text.substr(Prefix.size() - 1))
        return typedText(E, std::move(Text), Return->Size, Return->IsSigned);
    }
    return Text;
  }
  case ExprKind::Cast: {
    if (E.Operands.empty())
      return "/* bad cast */";
    if (const HighExpr *Call = typedCallResult(&E))
      return exprStr(*Call, ParentPrec);
    const TypeRef &To = E.CastTo ? E.CastTo : E.Type;
    if (isPlainInteger(To) && isPlainInteger(E.Operands[0]->Type))
      return typedText(E, integerView(*E.Operands[0], To, ParentPrec), To->Size,
                       To->IsSigned);
    std::string Text = "(" + typeToC(To) + ")" + exprStr(*E.Operands[0], 99);
    // A conversion's text has its type, whatever it converts: `(int8_t)v`
    // of a 128-bit v is a signed byte wherever it is printed in place.
    if (isPlainInteger(To))
      return typedText(E, std::move(Text), To->Size, To->IsSigned);
    return Text;
  }
  case ExprKind::BitCast: {
    if (E.Operands.size() != 1 || !E.Operands[0] || !E.Type ||
        !E.Operands[0]->Type || E.Type->Size != E.Operands[0]->Type->Size)
      llvm::report_fatal_error("HighC cannot render an invalid bit cast");
    const HighExpr &Src = *E.Operands[0];
    if (E.Type->Kind == NdTypeKind::Int && Src.Type->Kind == NdTypeKind::Int) {
      if (E.Type->IsSigned == Src.Type->IsSigned) {
        std::string Text = exprStr(Src, ParentPrec);
        if (const auto Printed = printedIntegerType(Src))
          PrintedIntegerTypes[&E] = *Printed;
        return Text;
      }
      if (isPlainInteger(E.Type) && isPlainInteger(Src.Type))
        return typedText(E, integerView(Src, E.Type, ParentPrec), E.Type->Size,
                         E.Type->IsSigned);
      return "(" + typeToC(E.Type) + ")" + exprStr(Src, 99);
    }
    // The bits of a call that returns this float are its result, and those
    // of a value of this type are that value.
    if (const auto Call = floatCallResultText(Src, E.Type))
      return *Call;
    if (const HighExpr *Value = floatBitsValue(Src, E.Type)) {
      if (computesWider(E.Type) && isFloatArithmetic(*Value))
        return "(" + typeToC(E.Type) + ")(" + exprStr(*Value) + ")";
      return exprStr(*Value, ParentPrec);
    }
    // A float whose bits are a constant, or read from constant data, is that
    // constant.
    if (const auto Literal = floatConstantBitsText(Src, E.Type))
      return *Literal;
    // An x87 value's 80 bits are the low ten bytes of a `long double`, whose
    // size the unit asserts is that of the `unsigned _BitInt(80)` carrying
    // them; the bytes past them are padding to both.
    if (isX87Value(E))
      return "__builtin_bit_cast(long double, (unsigned _BitInt(80))(" +
             exprStr(Src) + "))";
    if (isX87Value(Src))
      return "__builtin_bit_cast(unsigned _BitInt(80), (long double)(" +
             exprStr(Src) + "))";
    // The explicit source cast prevents integer promotions (or an unsuffixed
    // constant) from changing the operand's byte width inside the builtin.
    return "__builtin_bit_cast(" + typeToC(E.Type) + ", (" + typeToC(Src.Type) +
           ")(" + exprStr(Src) + "))";
  }
  case ExprKind::Addr: {
    if (E.Operands.empty())
      return "/* bad addr */";
    const HighExpr &Operand = *E.Operands[0];
    if (Operand.Kind == ExprKind::Load && !Operand.Operands.empty()) {
      if (auto Slot = namedFrameSlot(*Operand.Operands[0]))
        return "&" + *Slot;
      if (auto VA = constAddress(*Operand.Operands[0]))
        if (auto Name = imageObjectName(*VA))
          return "&" + *Name;
      // An integer address converts to the pointer by its bits, which its
      // unsigned view holds without a signed reinterpretation.
      const HighExpr &Address = *Operand.Operands[0];
      return "(" + typeToC(Operand.Type) + " *)(" +
             (Address.Type && Address.Type->Kind == NdTypeKind::Int
                  ? integerView(
                        Address,
                        NdType::makeInt(
                            getTargetRegInfo(Opts.TheArch).PointerSize, false),
                        0)
                  : exprStr(Address)) +
             ")";
    }
    if (Operand.Kind == ExprKind::Var || Operand.Kind == ExprKind::Phi)
      return "&" + varName(Operand.Var);
    return "&" + exprStr(*E.Operands[0], 99);
  }
  case ExprKind::Record: {
    if (!E.Type || sourceAggregateMembers(E.Type).empty() ||
        E.Operands.size() != E.Type->Fields.size())
      llvm::report_fatal_error("HighC cannot render an invalid source record");
    std::string Result = "(" + typeToC(E.Type) + "){";
    for (size_t I = 0; I < E.Operands.size(); ++I) {
      if (!E.Operands[I] ||
          !equalSourceTypes(E.Type->Fields[I], E.Operands[I]->Type))
        llvm::report_fatal_error("HighC source record field type disagrees");
      if (I)
        Result += ", ";
      const auto Text = exprStr(*E.Operands[I]);
      if (E.Type->Fields[I]->Kind == NdTypeKind::Ptr) {
        // Pointer expressions retain their integer machine carrier in HighC.
        // A record initializer needs the same source conversion as a call.
        const auto Value =
            sourceValue(Text, E.Operands[I]->Type, E.Type->Fields[I]);
        if (!Value)
          llvm::report_fatal_error("HighC source pointer field is invalid");
        Result += *Value;
      } else {
        Result += Text;
      }
    }
    return Result + "}";
  }
  case ExprKind::Field:
    if (E.Operands.size() != 1 || !E.Operands[0] || !E.Operands[0]->Type ||
        E.Operands[0]->Type->Kind != NdTypeKind::Struct ||
        E.ConstVal >= E.Operands[0]->Type->Fields.size() ||
        !equalSourceTypes(E.Type, E.Operands[0]->Type->Fields[E.ConstVal]))
      llvm::report_fatal_error("HighC cannot render an invalid source field");
    return "(" + exprStr(*E.Operands[0]) + ").field_" +
           std::to_string(E.ConstVal);
  default:
    return "/* unknown expr */";
  }
}

std::string HighCWriter::unwrapCastVar(const HighExpr &E) {
  if (E.Kind == ExprKind::Var)
    return varName(E.Var);
  if (E.Kind == ExprKind::Load && !E.Operands.empty()) {
    if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
      return {};
    std::string Addr = exprStr(*E.Operands[0]);
    auto Fwd = Analysis.StoreFwd.find(Addr);
    if (Fwd != Analysis.StoreFwd.end())
      return "(" + typeToC(E.Type) + ")(" + Fwd->second + ")";
  }
  if (E.Kind == ExprKind::UnaryOp &&
      (E.Op == NdOp::INT_ZEXT || E.Op == NdOp::INT_SEXT) && !E.Operands.empty())
    return unwrapCastVar(*E.Operands[0]);
  if (E.Kind == ExprKind::Cast && !E.Operands.empty())
    return unwrapCastVar(*E.Operands[0]);
  return {};
}

std::string HighCWriter::collapseHiLo(const HighExpr &Expr) {
  auto Result = tryCollapseHiLo(
      Expr, HiLoPairs, [this](const HighExpr &E) { return unwrapCastVar(E); });
  if (!Result.Collapsed.empty()) {
    Analysis.DeadVars.insert(Result.DeadLo);
    Analysis.DeadVars.insert(Result.DeadHi);
    Analysis.DeadStmts.insert(static_cast<const HighStmt *>(Result.DeadStmt));
    HasCIntrinsics = true;
  }
  return Result.Collapsed;
}

std::string HighCWriter::formatReturnExpr(const HighExpr &Expr) {
  if (FuncReturnType && FuncReturnType->Kind == NdTypeKind::Ptr) {
    const HighExpr *Inner = unwrapIntegerView(&Expr);
    if (Inner &&
        (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) &&
        Inner->Var.Kind == MedVar::Param) {
      const std::string Name = varName(Inner->Var);
      if (!IndirectReturnName.empty() && Name == IndirectReturnName)
        return Name;
    }
    if (Expr.Kind == ExprKind::Var) {
      auto Declared = declaredParamType(Expr.Var);
      if (Declared && equalSourceTypes(Declared, FuncReturnType))
        return varName(Expr.Var);
    }
    if (Expr.Type && equalSourceTypes(Expr.Type, FuncReturnType))
      return exprStr(Expr);
    return "(" + typeToC(FuncReturnType) + ")(uintptr_t)(" + exprStr(Expr) +
           ")";
  }

  auto HiLo = collapseHiLo(Expr);
  if (!HiLo.empty())
    return HiLo;

  if (FuncReturnType && FuncReturnType->Kind == NdTypeKind::Float) {
    // A call that returns this float is the value returned.
    if (const auto Call = floatCallResultText(Expr, FuncReturnType))
      return *Call;
    const HighExpr *Raw = &Expr;
    if (Raw->Kind == ExprKind::UnaryOp && Raw->Op == NdOp::INT_ZEXT &&
        !Raw->Operands.empty() && Raw->Operands[0] && Raw->Operands[0]->Type &&
        Raw->Operands[0]->Type->Size == FuncReturnType->Size)
      Raw = Raw->Operands[0].get();
    if (Raw->Type && Raw->Type->Kind == NdTypeKind::Int &&
        Raw->Type->Size == FuncReturnType->Size) {
      auto RawType = NdType::makeInt(FuncReturnType->Size, false);
      return "__builtin_bit_cast(" + typeToC(FuncReturnType) + ", (" +
             typeToC(RawType) + ")(" + exprStr(*Raw) + "))";
    }
  }

  if (!FuncReturnType || FuncReturnType->Kind != NdTypeKind::Int)
    return exprStr(Expr);

  // A string literal is an array: an integer return takes its address.
  if (const HighExpr *Inner = unwrapIntegerView(&Expr);
      Inner && Inner->Kind == ExprKind::Const)
    if (std::string Text = exprStr(*Inner); isStringLiteralText(Text))
      return "(" + typeToC(FuncReturnType) + ")(uintptr_t)(" + Text + ")";

  // EAX/RAX leftovers are Cast / same-width zext / SUBBYTES 0 around the
  // i32 add. A real widen (i16→i64) stays so narrowing the C return does
  // not turn zero extension into sign extension.
  auto PeelReturnViews = [&](const HighExpr *Cur) {
    for (unsigned Peel = 0; Peel < limits::kMaxIntegerViewUnwrapDepth && Cur;
         ++Peel) {
      // A view of an integer only: the bits of a floating value are no
      // view of it, and C would convert the value they were peeled from.
      if ((Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast) &&
          !Cur->Operands.empty() && Cur->Operands[0] &&
          (!Cur->Operands[0]->Type ||
           Cur->Operands[0]->Type->Kind == NdTypeKind::Int ||
           Cur->Operands[0]->Type->Kind == NdTypeKind::Ptr)) {
        Cur = Cur->Operands[0].get();
        continue;
      }
      if (Cur->Kind == ExprKind::UnaryOp &&
          (Cur->Op == NdOp::INT_ZEXT || Cur->Op == NdOp::INT_SEXT) &&
          !Cur->Operands.empty() && Cur->Operands[0] &&
          Cur->Operands[0]->Type &&
          Cur->Operands[0]->Type->Kind == NdTypeKind::Int &&
          Cur->Operands[0]->Type->Size == FuncReturnType->Size) {
        Cur = Cur->Operands[0].get();
        continue;
      }
      if (Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::SUBBYTES &&
          Cur->Operands.size() == 2 && Cur->Operands[0] && Cur->Operands[1] &&
          Cur->Operands[1]->Kind == ExprKind::Const &&
          Cur->Operands[1]->ConstVal == 0) {
        Cur = Cur->Operands[0].get();
        continue;
      }
      break;
    }
    return Cur;
  };
  const HighExpr *Inner = PeelReturnViews(&Expr);
  if (Inner && (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)) {
    const std::string Name = copyForwardName(varName(Inner->Var));
    if (auto Fwd = ValueForward.find(Name);
        Fwd != ValueForward.end() && Fwd->second && Fwd->second != Inner)
      Inner = PeelReturnViews(Fwd->second);
  }
  if (Inner && Inner != &Expr) {
    const bool SameWidth = Inner->Type &&
                           Inner->Type->Kind == NdTypeKind::Int &&
                           Inner->Type->Size == FuncReturnType->Size;
    const bool SameSign =
        SameWidth && Inner->Type->IsSigned == FuncReturnType->IsSigned;
    if (auto Converted = implicitIntegerConversion(*Inner, FuncReturnType))
      return *Converted;
    if (Inner->Kind == ExprKind::Const || SameSign)
      return exprStr(*Inner);
    return "(" + typeToC(FuncReturnType) + ")" + exprStr(*Inner, 99);
  }

  if (Expr.Kind == ExprKind::UnaryOp &&
      (Expr.Op == NdOp::INT_ZEXT || Expr.Op == NdOp::INT_SEXT) &&
      !Expr.Operands.empty()) {
    auto &Inner = *Expr.Operands[0];
    if (Inner.Type && Inner.Type->Kind == NdTypeKind::Int) {
      if (Inner.Type->Size == FuncReturnType->Size)
        return exprStr(Inner);
      // Keep the source-width interpretation of zext/sext when narrowing the
      // final C return type; a direct C cast from a signed input would turn
      // zero extension into sign extension.  An extension printed as the
      // return type already is the value returned.
      std::string Text = exprStr(Expr);
      if (const auto Printed = printedIntegerType(Expr);
          Printed && Printed->first == FuncReturnType->Size &&
          Printed->second == FuncReturnType->IsSigned)
        return Text;
      return "(" + typeToC(FuncReturnType) + ")(" + Text + ")";
    }
  }

  if (Expr.Type && Expr.Type->Kind == NdTypeKind::Int &&
      Expr.Type->Size > FuncReturnType->Size) {
    if (auto Converted = implicitIntegerConversion(Expr, FuncReturnType))
      return *Converted;
    return "(" + typeToC(FuncReturnType) + ")" + exprStr(Expr);
  }

  if (Expr.Type && Expr.Type->Kind == NdTypeKind::Int &&
      Expr.Type->Size == FuncReturnType->Size)
    if (auto Converted = implicitIntegerConversion(Expr, FuncReturnType))
      return *Converted;
  return exprStr(Expr);
}

std::string HighCWriter::printedForwardedVar(const std::string &Name,
                                             int ParentPrec,
                                             const HighExpr **PrintedFrom) {
  if (auto Fwd = FieldForward.find(Name); Fwd != FieldForward.end()) {
    if (auto Source = FieldForwardSources.find(Name);
        SourceRecorder && CurrentFunc && Source != FieldForwardSources.end())
      return SourceRecorder->expression(CurrentFunc->Entry, *Source->second,
                                        Fwd->second);
    return Fwd->second;
  }
  if (auto Fwd = ValueForward.find(Name);
      Fwd != ValueForward.end() && Fwd->second) {
    const HighExpr *Inner = peelIntegerViewOps(Fwd->second);
    if (Inner && Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
        Inner->Operands[0]) {
      if (auto Member = typedMemberAccess(*Inner->Operands[0]))
        return SourceRecorder && CurrentFunc
                   ? SourceRecorder->expression(CurrentFunc->Entry,
                                                *Fwd->second, *Member)
                   : *Member;
    }
    if (PrintedFrom)
      *PrintedFrom = Fwd->second;
    return exprStr(*Fwd->second, ParentPrec);
  }
  if (auto Fwd = CtorThisForward.find(Name);
      Fwd != CtorThisForward.end() && Fwd->second) {
    if (PrintedFrom)
      *PrintedFrom = Fwd->second;
    return exprStr(*Fwd->second, ParentPrec);
  }
  if (auto It = CallResultNames.find(Name); It != CallResultNames.end())
    return It->second;
  return Name;
}

std::string HighCWriter::copyForwardName(const std::string &Name) const {
  std::string Cur = Name;
  for (unsigned Depth = 0; Depth < limits::kMaxCopyForwardAliasDepth; ++Depth) {
    auto It = CopyForward.find(Cur);
    if (It == CopyForward.end())
      return Cur;
    Cur = It->second;
  }
  return Cur;
}

std::optional<std::string>
HighCWriter::forwardedStoreValue(const HighExpr &Addr,
                                 const std::string &Printed) const {
  auto Find = [&](const std::string &Key) -> std::optional<std::string> {
    auto It = Analysis.StoreFwd.find(Key);
    if (It == Analysis.StoreFwd.end())
      return std::nullopt;
    return It->second;
  };
  if (auto Hit = Find(Printed))
    return Hit;
  auto KeyIt = Analysis.AddressKeys.find(&Addr);
  if (KeyIt == Analysis.AddressKeys.end())
    return std::nullopt;
  if (auto Hit = Find(KeyIt->second))
    return Hit;
  auto Alias = Analysis.StoreFwdByAddressKey.find(KeyIt->second);
  if (Alias == Analysis.StoreFwdByAddressKey.end())
    return std::nullopt;
  return Alias->second;
}

bool HighCWriter::isCopyForwardDestination(const MedVar &V) const {
  return V.Kind == MedVar::Temp || V.RenameTag >= 0;
}

std::optional<va_t> HighCWriter::constAddress(const HighExpr &E) const {
  const HighExpr *Cur = unwrapIntegerView(&E);
  if (Cur && Cur->Kind == ExprKind::Const)
    return Cur->ConstVal;
  if (Cur && Cur->Kind == ExprKind::BinOp && Cur->Type &&
      Cur->Type->Kind == NdTypeKind::Int && Cur->Type->Size <= 8 &&
      Cur->Type->Size != 0 && Cur->Operands.size() == 2 &&
      (Cur->Op == NdOp::INT_ADD || Cur->Op == NdOp::INT_SUB)) {
    const auto Left = constAddress(*Cur->Operands[0]);
    const auto Right = constAddress(*Cur->Operands[1]);
    if (Left && Right) {
      uint64_t Value =
          Cur->Op == NdOp::INT_ADD ? *Left + *Right : *Left - *Right;
      if (Cur->Type->Size < 8)
        Value &= (uint64_t(1) << (Cur->Type->Size * 8)) - 1;
      return Value;
    }
  }
  return std::nullopt;
}

bool HighCWriter::isImageDataAddress(va_t Addr) const {
  if (!Opts.Image || Addr == 0 || Addr == InvalidVA)
    return false;
  if (Opts.Image->findImportAt(Addr))
    return false;
  const Segment *Seg = Opts.Image->getSegmentFor(Addr);
  if (!Seg || !Seg->isReadable())
    return false;
  // Instruction bytes are not data objects.  Read-only constants live in
  // .rdata / .rodata (readable, not writable, not executable).
  if (Seg->isExecutable() && !Seg->isWritable())
    return false;
  return true;
}

std::optional<uint64_t> HighCWriter::foldReadonlyScalar(va_t Addr,
                                                        uint16_t Size) const {
  if (!isImageDataAddress(Addr) || !Opts.Image ||
      DataSymbolBindings.count(Addr))
    return std::nullopt;
  const Segment *Seg = Opts.Image->getSegmentFor(Addr);
  if (!Seg || Seg->isWritable())
    return std::nullopt;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return std::nullopt;
  const uint8_t *Bytes = Opts.Image->readVA(Addr, Size);
  if (!Bytes)
    return std::nullopt;
  switch (Size) {
  case 1:
    return Bytes[0];
  case 2:
    return readLE<uint16_t>(Bytes);
  case 4:
    return readLE<uint32_t>(Bytes);
  case 8:
    return readLE<uint64_t>(Bytes);
  default:
    return std::nullopt;
  }
}

std::optional<std::string> HighCWriter::imageObjectName(va_t Addr) const {
  if (imageBackingAddress(Addr))
    return std::nullopt;
  auto It = ImageObjects.find(Addr);
  if (It == ImageObjects.end())
    return std::nullopt;
  return It->second.Name;
}

const HighExpr *HighCWriter::indexedImageBase(const HighExpr &Address) const {
  // The terms of a sum, through nested sums: `(i * 8 + &table) + 3` reads a
  // field of an entry.
  std::vector<const HighExpr *> Terms;
  std::function<bool(const HighExpr &, unsigned)> Collect =
      [&](const HighExpr &E, unsigned Depth) {
        const HighExpr *Inner = peelIntegerViewOps(&E);
        if (!Inner || Depth > limits::kMaxIntegerViewUnwrapDepth)
          return false;
        if (Inner->Kind == ExprKind::BinOp && Inner->Op == NdOp::INT_ADD &&
            Inner->Operands.size() == 2 && Inner->Operands[0] &&
            Inner->Operands[1])
          return Collect(*Inner->Operands[0], Depth + 1) &&
                 Collect(*Inner->Operands[1], Depth + 1);
        Terms.push_back(Inner);
        return true;
      };
  const HighExpr *Inner = peelIntegerViewOps(&Address);
  if (!Inner || Inner->Kind != ExprKind::BinOp || Inner->Op != NdOp::INT_ADD ||
      !Collect(Address, 0))
    return nullptr;
  // One term is the object's address and another varies.  A number equal to
  // an address is no address; an object without a size has no extent to
  // declare.
  const HighExpr *Base = nullptr;
  bool Varies = false;
  for (const HighExpr *Term : Terms) {
    if (Term->Kind != ExprKind::Const) {
      Varies = true;
      continue;
    }
    if (Term->ConstProvenance == ConstantAddressProvenance::Scalar ||
        Term->ConstProvenance == ConstantAddressProvenance::AddressFragment ||
        !isImageDataAddress(Term->ConstVal) || !sizedObjectAt(Term->ConstVal))
      continue;
    if (Base)
      return nullptr;
    Base = Term;
  }
  return Varies ? Base : nullptr;
}

std::optional<std::pair<va_t, uint64_t>>
HighCWriter::sizedObjectAt(va_t Addr) const {
  const auto After = llvm::upper_bound(
      SizedObjects, Addr,
      [](va_t A, const std::pair<va_t, uint64_t> &O) { return A < O.first; });
  // Walk back while an object at or before this one may still reach Addr:
  // the last object that holds it encloses the others.
  std::optional<std::pair<va_t, uint64_t>> Outermost;
  for (size_t I = After - SizedObjects.begin();
       I > 0 && SizedObjectReach[I - 1] > Addr; --I)
    if (Addr - SizedObjects[I - 1].first < SizedObjects[I - 1].second)
      Outermost = SizedObjects[I - 1];
  return Outermost;
}

std::optional<std::string> HighCWriter::imageBackingAddress(va_t Addr) const {
  for (const ImageBacking &Backing : ImageBackings) {
    if (Addr < Backing.Base)
      break;
    if (Addr < Backing.End)
      return "&" + Backing.Name + "[" +
             std::to_string(Backing.Pad + (Addr - Backing.Base)) + "]";
  }
  return std::nullopt;
}

void HighCWriter::noteImageObject(va_t Addr, const TypeRef &Ty, bool Written,
                                  bool MemoryAccess) {
  if (!isImageDataAddress(Addr))
    return;
  ImageObject &Obj = ImageObjects[Addr];
  if (MemoryAccess && Ty)
    Obj.MemoryWidths.insert(Ty->Size);
  if (Obj.Name.empty()) {
    std::string Raw;
    if (Opts.UserNames)
      if (auto It = Opts.UserNames->find(Addr); It != Opts.UserNames->end())
        Raw = It->second;
    if (Raw.empty() && Dbg) {
      if (auto Data = Dbg->resolveDataObject(Addr); Data && !Data->Name.empty())
        Raw = Data->Name;
    }
    if (Raw.empty() && Opts.Image) {
      if (const Symbol *Sym = Opts.Image->findSymbolAt(Addr);
          Sym && !Sym->IsFunc && !Sym->Name.empty() &&
          llvm::StringRef(Sym->Name).find(kAutoFuncPrefix) != 0) {
        // A mangled name keeps the underscores its scheme starts with
        // (`_ZTV8QDomNode` is `QDomNode_vtable`).  A comment reads a name the
        // identifier is spelled from: `vtable for QDomNode`, Go's
        // `main.Flags`.
        const llvm::StringRef CName =
            cNameOfSymbol(Sym->Name, Opts.Format, Opts.TheArch);
        Raw = symbolScheme(CName) != SymbolScheme::None
                  ? CName.str()
                  : stripLeadingUnderscores(Sym->Name).str();
        Obj.Readable = demangledComment(CName);
        Obj.Symbol = Sym->Name;
      }
    }
    if (!Raw.empty())
      Obj.Name = GlobalIdentifierAllocator.allocate(Raw, "g");
    if (Obj.Symbol.empty())
      Obj.Symbol = Raw;
  }
  auto NamedDisplayPointer = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Ptr && Type->Pointee &&
           !Type->Pointee->SourceName.empty();
  };
  if (Ty && Obj.WeakType) {
    Obj.Type = Ty;
    Obj.WeakType = false;
  } else if (Ty) {
    if (!Obj.Type)
      Obj.Type = Ty;
    else if (NamedDisplayPointer(Ty) && !NamedDisplayPointer(Obj.Type) &&
             Ty->Size >= Obj.Type->Size)
      Obj.Type = Ty;
    else if (!NamedDisplayPointer(Obj.Type) && Ty->Size > Obj.Type->Size)
      Obj.Type = Ty;
  }
  if (!Obj.Type)
    Obj.Type = NdType::makeInt(4);
  Obj.Written |= Written;
}

void HighCWriter::noteImageAddress(va_t Addr) {
  if (!isImageDataAddress(Addr))
    return;
  if (auto It = ImageObjects.find(Addr);
      It != ImageObjects.end() && It->second.Type)
    return;
  // The address alone stands in with an integer as wide as the object its
  // symbol sizes, which the type of any access replaces.
  const uint64_t Size = Opts.Image ? Opts.Image->dataObjectSizeAt(Addr) : 0;
  noteImageObject(
      Addr,
      NdType::makeInt(Size == 1 || Size == 2 || Size == 4 || Size == 8 ? Size
                                                                       : 2),
      false);
  ImageObjects[Addr].WeakType = true;
}

bool HighCWriter::isParamCopy(const HighExpr &E) const {
  const HighExpr *Cur = unwrapIntegerView(&E);
  return Cur && (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi) &&
         Cur->Var.Kind == MedVar::Param;
}

bool HighCWriter::isAddressTakenSlot(llvm::StringRef Name) const {
  if (Name.empty())
    return false;
  for (const auto &[_, Slot] : FrameSlots)
    if (Slot.AddressTaken && Slot.Name == Name)
      return true;
  return false;
}

bool HighCWriter::hidesAddressTakenParamHome(llvm::StringRef Slot,
                                             const HighExpr &Val) const {
  if (auto Src = copyForwardSource(Val)) {
    if (*Src == "result" || *Src == "this")
      return true;
    if (!IndirectReturnName.empty() && *Src == IndirectReturnName)
      return true;
  }
  if (Val.Type && Val.Type->Kind == NdTypeKind::Ptr)
    return true;
  for (const auto &[_, FrameSlot] : FrameSlots) {
    if (FrameSlot.Name == Slot && FrameSlot.Type &&
        !FrameSlot.Type->SourceName.empty())
      return true;
  }
  return false;
}

bool HighCWriter::isEmittedParamName(llvm::StringRef Name) const {
  if (Name.empty())
    return false;
  if (isReservedParamDisplayName(Name))
    return true;
  if (CurrentFunc) {
    for (const HighParam &Param : CurrentFunc->Params)
      if (!Param.Name.empty() && Name == Param.Name)
        return true;
  }
  for (const auto &[_, Display] : ParamDisplayNames)
    if (!Display.empty() && Name == Display)
      return true;
  if (Name.starts_with("arg") && Name.size() > 3 &&
      llvm::all_of(Name.drop_front(3), [](char Ch) {
        return std::isdigit(static_cast<unsigned char>(Ch));
      }))
    return true;
  return false;
}

bool HighCWriter::isReservedParamDisplayName(llvm::StringRef Name) const {
  if (Name.empty())
    return false;
  if (!IndirectReturnName.empty() && Name == IndirectReturnName)
    return true;
  if (CurrentFunc) {
    for (const HighParam &Param : CurrentFunc->Params)
      if (!Param.Name.empty() && Name == Param.Name)
        return true;
  }
  for (const auto &[_, Display] : ParamDisplayNames)
    if (!Display.empty() && Name == Display)
      return true;
  if (Dbg && CurrentFunc) {
    if (const auto FS = Dbg->resolveFunction(CurrentFunc->Entry); FS) {
      if (isMsvcIndirectReturn(FS->ReturnType) && Name == "result")
        return true;
      for (const auto &Param : FS->Params)
        if (!Param.first.empty() && Name == Param.first)
          return true;
    }
    // The names the debug signature gives the parameters where they arrive,
    // such as `p_8` for a record's second register.
    for (size_t I = 0; I < CurrentFunc->Params.size(); ++I)
      if (Name == debugParamName(*CurrentFunc, I))
        return true;
  }
  return false;
}

bool HighCWriter::isIncomingParamReuseAssign(const HighStmt &Stmt) const {
  if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val)
    return false;
  if (Stmt.Dst->Kind != ExprKind::Var && Stmt.Dst->Kind != ExprKind::Phi)
    return false;
  if (Stmt.Dst->Var.Kind != MedVar::Param)
    return false;
  const HighExpr *Src = unwrapIntegerView(Stmt.Val.get());
  if (!Src || (Src->Kind != ExprKind::Var && Src->Kind != ExprKind::Phi))
    return false;
  if (Src->Var.Kind == MedVar::Param)
    return Src->Var.Id != Stmt.Dst->Var.Id;
  return true;
}

bool HighCWriter::isCatchFuncletParentFrame(const MedVar &V) const {
  if (!CurrentFunc || Opts.TheArch != Arch::X64)
    return false;
  if (V.Kind != MedVar::Param || V.Id != 1)
    return false;
  // Inside a catch body, rdx is always the establisher frame, even when the
  // parent also has an rdx argument.
  if (InEHClauseBody)
    return true;
  // Whole-function walks (local decls) do not set InEHClauseBody. A Param 1
  // the parent does not own is the attached funclet frame pointer.
  return static_cast<size_t>(V.Id) >= CurrentFunc->Params.size();
}

const HighExpr *
HighCWriter::parentFrameStoredValue(const HighStmt &Stmt) const {
  if (!InEHClauseBody)
    return nullptr;
  const HighExpr *Addr = nullptr;
  const HighExpr *Val = nullptr;
  if (Stmt.Kind == StmtKind::Store && Stmt.StoreAddr && Stmt.StoreVal) {
    Addr = Stmt.StoreAddr.get();
    Val = Stmt.StoreVal.get();
  } else if (Stmt.Kind == StmtKind::Assign && Stmt.Dst && Stmt.Val &&
             Stmt.Dst->Kind == ExprKind::Load && !Stmt.Dst->Operands.empty()) {
    Addr = Stmt.Dst->Operands[0].get();
    Val = Stmt.Val.get();
  }
  if (!Addr || !Val)
    return nullptr;
  return frameDisplacement(*Addr) ? Val : nullptr;
}

std::optional<std::string>
HighCWriter::copyForwardSource(const HighExpr &E) const {
  if (E.Kind == ExprKind::Var || E.Kind == ExprKind::Phi)
    return copyForwardName(varName(E.Var));
  if (E.Kind == ExprKind::Load && !E.Operands.empty())
    if (auto Slot = namedFrameSlot(*E.Operands[0]))
      return copyForwardName(*Slot);
  return std::nullopt;
}

std::string HighCWriter::condStr(const HighExpr &E) {
  std::string Text = condStrImpl(E);
  return SourceRecorder && CurrentFunc
             ? SourceRecorder->expression(CurrentFunc->Entry, E,
                                          std::move(Text))
             : Text;
}

std::string HighCWriter::condStrImpl(const HighExpr &E) {
  const HighExpr *Cur = forwardedExpr(&E);
  Cur = unwrapIntegerView(Cur);
  auto IsZeroLike = [this](const ExprPtr &Op) {
    if (!Op)
      return false;
    const HighExpr *Z = unwrapIntegerView(Op.get());
    return Z && Z->Kind == ExprKind::Const && Z->ConstVal == 0;
  };
  if (Cur && Cur->Kind == ExprKind::BinOp && Cur->Operands.size() == 2 &&
      Cur->Operands[0] && Cur->Operands[1] && Cur->Op == NdOp::INT_NOTEQUAL) {
    if (IsZeroLike(Cur->Operands[1]))
      return condStr(*Cur->Operands[0]);
    if (IsZeroLike(Cur->Operands[0]))
      return condStr(*Cur->Operands[1]);
  }
  if (Cur && Cur->Kind == ExprKind::BinOp && Cur->Operands.size() == 2 &&
      Cur->Operands[0] && Cur->Operands[1] && Cur->Op == NdOp::INT_EQUAL) {
    const HighExpr *X = nullptr;
    if (IsZeroLike(Cur->Operands[1]))
      X = forwardedExpr(Cur->Operands[0].get());
    else if (IsZeroLike(Cur->Operands[0]))
      X = forwardedExpr(Cur->Operands[1].get());
    if (X) {
      if (const HighExpr *Call = typedCallResult(X))
        return "!" + exprStr(*Call, 99);
      X = unwrapIntegerView(X);
      if (X && X->Kind == ExprKind::Call)
        return "!" + exprStr(*X, 99);
    }
  }
  if (Cur && Cur->Kind == ExprKind::BinOp && Cur->Op == NdOp::SUBBYTES &&
      Cur->Operands.size() == 2 && Cur->Operands[0] && Cur->Operands[1] &&
      Cur->Operands[1]->Kind == ExprKind::Const &&
      Cur->Operands[1]->ConstVal == 0)
    return condStr(*Cur->Operands[0]);
  if (Cur && Cur->Kind == ExprKind::BinOp && Cur->Operands.size() == 2 &&
      Cur->Operands[0] && Cur->Operands[1] &&
      (Cur->Op == NdOp::BOOL_AND || Cur->Op == NdOp::BOOL_OR)) {
    if (Cur->Op == NdOp::BOOL_AND) {
      // `x && !(x < 0)` is the signed `test; jle` split. Print `x > 0`.
      auto ScalarOf = [&](const HighExpr *E) -> const HighExpr * {
        unsigned Depth = 0;
        while (E && Depth++ < 6) {
          E = forwardedExpr(E);
          E = unwrapIntegerView(E);
          if (!E)
            return nullptr;
          if (E->Kind == ExprKind::Var || E->Kind == ExprKind::Phi)
            return E;
          if (E->Kind == ExprKind::BinOp && E->Op == NdOp::INT_NOTEQUAL &&
              E->Operands.size() == 2) {
            if (IsZeroLike(E->Operands[1]) && E->Operands[0]) {
              E = E->Operands[0].get();
              continue;
            }
            if (IsZeroLike(E->Operands[0]) && E->Operands[1]) {
              E = E->Operands[1].get();
              continue;
            }
          }
          return nullptr;
        }
        return nullptr;
      };
      auto NotSignedNeg = [&](const HighExpr *E) -> const HighExpr * {
        E = forwardedExpr(E);
        E = unwrapIntegerView(E);
        if (!E || E->Kind != ExprKind::UnaryOp || E->Op != NdOp::BOOL_NOT ||
            E->Operands.empty() || !E->Operands[0])
          return nullptr;
        const HighExpr *Cmp = forwardedExpr(E->Operands[0].get());
        Cmp = unwrapIntegerView(Cmp);
        if (!Cmp || Cmp->Kind != ExprKind::BinOp ||
            Cmp->Op != NdOp::INT_SLESS || Cmp->Operands.size() != 2 ||
            !IsZeroLike(Cmp->Operands[1]) || !Cmp->Operands[0])
          return nullptr;
        return ScalarOf(Cmp->Operands[0].get());
      };
      auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
        if (!A || !B ||
            (A->Kind != ExprKind::Var && A->Kind != ExprKind::Phi) ||
            A->Kind != B->Kind)
          return false;
        if (A->Var.Kind == B->Var.Kind && A->Var.Id == B->Var.Id)
          return true;
        return copyForwardName(varName(A->Var)) ==
               copyForwardName(varName(B->Var));
      };
      const HighExpr *Pos = ScalarOf(Cur->Operands[0].get());
      const HighExpr *Neg = NotSignedNeg(Cur->Operands[1].get());
      if (!SameScalar(Pos, Neg)) {
        Pos = ScalarOf(Cur->Operands[1].get());
        Neg = NotSignedNeg(Cur->Operands[0].get());
      }
      if (SameScalar(Pos, Neg))
        return exprStr(*Pos) + " > 0";
    }
    std::string L = condStr(*Cur->Operands[0]);
    std::string R = condStr(*Cur->Operands[1]);
    if (Cur->Op == NdOp::BOOL_AND) {
      auto NotLessZero = [](const std::string &Expr, const std::string &Not) {
        return Not == "!(" + Expr + " < 0)" ||
               Not == "!((int32_t)" + Expr + " < 0)" ||
               Not == "!((int64_t)" + Expr + " < 0)";
      };
      if (NotLessZero(L, R))
        return L + " > 0";
      if (NotLessZero(R, L))
        return R + " > 0";
      if (L.find(" || ") != std::string::npos)
        L = "(" + L + ")";
      if (R.find(" || ") != std::string::npos)
        R = "(" + R + ")";
      return L + " && " + R;
    }
    return L + " || " + R;
  }
  // Boolean context: a leftover narrowing cast of a scalar / call is
  // `test eax`, not a sanitizer wrap. Keep wraps on add/sub/mul.
  if (Cur && Cur != &E &&
      (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi ||
       Cur->Kind == ExprKind::Call))
    return exprStr(*Cur);
  return exprStr(E);
}

std::optional<std::string>
HighCWriter::preferGreaterIfElseCond(const HighExpr &E) {
  const HighExpr *Cur = forwardedExpr(&E);
  Cur = unwrapIntegerView(Cur);
  if (!Cur || Cur->Kind != ExprKind::UnaryOp || Cur->Op != NdOp::BOOL_NOT ||
      Cur->Operands.empty() || !Cur->Operands[0])
    return std::nullopt;
  Cur = forwardedExpr(Cur->Operands[0].get());
  Cur = unwrapIntegerView(Cur);
  if (!Cur || Cur->Kind != ExprKind::BinOp || Cur->Operands.size() != 2 ||
      !Cur->Operands[0] || !Cur->Operands[1] ||
      (Cur->Op != NdOp::BOOL_AND && Cur->Op != NdOp::INT_AND))
    return std::nullopt;
  auto AsLe = [&](const HighExpr *Op) -> const HighExpr * {
    Op = forwardedExpr(Op);
    Op = unwrapIntegerView(Op);
    if (Op && Op->Kind == ExprKind::BinOp && Op->Operands.size() == 2 &&
        (Op->Op == NdOp::INT_LESSEQUAL || Op->Op == NdOp::INT_SLESSEQUAL))
      return Op;
    return nullptr;
  };
  auto AsNe = [&](const HighExpr *Op) -> const HighExpr * {
    Op = forwardedExpr(Op);
    Op = unwrapIntegerView(Op);
    if (Op && Op->Kind == ExprKind::BinOp && Op->Op == NdOp::INT_NOTEQUAL &&
        Op->Operands.size() == 2)
      return Op;
    return nullptr;
  };
  const HighExpr *Le = AsLe(Cur->Operands[0].get());
  const HighExpr *Ne = AsNe(Cur->Operands[1].get());
  if (!Le || !Ne) {
    Le = AsLe(Cur->Operands[1].get());
    Ne = AsNe(Cur->Operands[0].get());
  }
  if (!Le || !Ne || Le->Operands.size() != 2 || !Le->Operands[0] ||
      !Le->Operands[1])
    return std::nullopt;
  auto Peel = [&](const HighExpr *S) -> const HighExpr * {
    unsigned Depth = 0;
    while (S && Depth++ < 6) {
      S = forwardedExpr(S);
      S = unwrapIntegerView(S);
      if (!S || S->Kind != ExprKind::BinOp || S->Operands.size() != 2)
        return S;
      if (S->Op == NdOp::INT_EQUAL || S->Op == NdOp::INT_NOTEQUAL ||
          S->Op == NdOp::INT_LESS || S->Op == NdOp::INT_LESSEQUAL ||
          S->Op == NdOp::INT_SLESS || S->Op == NdOp::INT_SLESSEQUAL)
        return S;
      break;
    }
    return S;
  };
  auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
    A = Peel(A);
    B = Peel(B);
    if (!A || !B)
      return false;
    if (A == B)
      return true;
    if ((A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi) &&
        A->Kind == B->Kind)
      return A->Var.Kind == B->Var.Kind && A->Var.Id == B->Var.Id;
    return exprStr(*A, 99) == exprStr(*B, 99);
  };
  auto SamePair = [&](const HighExpr *A0, const HighExpr *A1,
                      const HighExpr *B0, const HighExpr *B1) {
    return (SameScalar(A0, B0) && SameScalar(A1, B1)) ||
           (SameScalar(A0, B1) && SameScalar(A1, B0));
  };
  if (!SamePair(Le->Operands[0].get(), Le->Operands[1].get(),
                Ne->Operands[0].get(), Ne->Operands[1].get()))
    return std::nullopt;
  auto MemberOrExpr = [&](const HighExpr &Op) {
    const HighExpr *Inner = peelIntegerViewOps(&Op);
    if (!Inner)
      Inner = &Op;
    if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
        Inner->Operands[0]) {
      if (auto Member = typedMemberAccess(*Inner->Operands[0],
                                          Inner->Type ? Inner->Type->Size : 0))
        return *Member;
      if (auto Member = typedMemberAccess(*Inner->Operands[0]))
        return *Member;
    }
    if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) {
      const std::string Raw = varName(Inner->Var);
      if (auto It = FieldForward.find(Raw); It != FieldForward.end())
        return It->second;
      const std::string Name = copyForwardName(Raw);
      if (auto It = FieldForward.find(Name); It != FieldForward.end())
        return It->second;
    }
    return exprStr(*Inner, 7);
  };
  return MemberOrExpr(*Le->Operands[1]) + " > " +
         MemberOrExpr(*Le->Operands[0]);
}

std::string HighCWriter::invertCondStr(const HighExpr &E) {
  std::string Text = invertCondStrImpl(E);
  return SourceRecorder && CurrentFunc
             ? SourceRecorder->expression(CurrentFunc->Entry, E,
                                          std::move(Text))
             : Text;
}

std::string HighCWriter::invertCondStrImpl(const HighExpr &E) {
  const HighExpr *Cur = forwardedExpr(&E);
  Cur = unwrapIntegerView(Cur);
  if (!Cur)
    return "!(" + exprStr(E) + ")";
  if (Cur->Kind == ExprKind::UnaryOp && Cur->Op == NdOp::BOOL_NOT &&
      !Cur->Operands.empty() && Cur->Operands[0])
    return exprStr(*Cur->Operands[0]);

  auto IsZeroLike = [this](const ExprPtr &Op) {
    if (!Op)
      return false;
    const HighExpr *Z = unwrapIntegerView(Op.get());
    return Z && Z->Kind == ExprKind::Const && Z->ConstVal == 0;
  };
  auto IsCompareOp = [](NdOp Op) {
    switch (Op) {
    case NdOp::INT_EQUAL:
    case NdOp::INT_NOTEQUAL:
    case NdOp::INT_LESS:
    case NdOp::INT_LESSEQUAL:
    case NdOp::INT_SLESS:
    case NdOp::INT_SLESSEQUAL:
    case NdOp::FLOAT_EQUAL:
    case NdOp::FLOAT_NOTEQUAL:
    case NdOp::FLOAT_LESS:
    case NdOp::FLOAT_LESSEQUAL:
    case NdOp::BOOL_AND:
    case NdOp::BOOL_OR:
    case NdOp::BOOL_XOR:
      return true;
    default:
      return false;
    }
  };
  const auto LooksBool = [&](auto &&Self, const HighExpr *Op,
                             unsigned Depth) -> bool {
    if (!Op || Depth > 8)
      return false;
    Op = forwardedExpr(Op);
    Op = unwrapIntegerView(Op);
    if (!Op)
      return false;
    if (Op->Kind == ExprKind::UnaryOp && Op->Op == NdOp::BOOL_NOT)
      return true;
    if (Op->Kind != ExprKind::BinOp || Op->Operands.size() != 2)
      return false;
    if (IsCompareOp(Op->Op))
      return true;
    if (Op->Op != NdOp::INT_OR && Op->Op != NdOp::INT_AND)
      return false;
    return Self(Self, Op->Operands[0].get(), Depth + 1) &&
           Self(Self, Op->Operands[1].get(), Depth + 1);
  };

  if (Cur->Kind == ExprKind::BinOp && Cur->Operands.size() == 2 &&
      Cur->Operands[0] && Cur->Operands[1]) {
    auto PeelScalar = [&](const HighExpr *S) -> const HighExpr * {
      unsigned Depth = 0;
      while (S && Depth++ < 6) {
        S = forwardedExpr(S);
        S = unwrapIntegerView(S);
        if (!S || S->Kind != ExprKind::BinOp || S->Operands.size() != 2)
          return S;
        if (S->Op == NdOp::INT_EQUAL || S->Op == NdOp::INT_NOTEQUAL ||
            S->Op == NdOp::INT_LESS || S->Op == NdOp::INT_LESSEQUAL ||
            S->Op == NdOp::INT_SLESS || S->Op == NdOp::INT_SLESSEQUAL)
          return S;
        if (IsZeroLike(S->Operands[1]) && S->Operands[0]) {
          S = S->Operands[0].get();
          continue;
        }
        if (IsZeroLike(S->Operands[0]) && S->Operands[1]) {
          S = S->Operands[1].get();
          continue;
        }
        return S;
      }
      return S;
    };
    auto SameScalar = [&](const HighExpr *A, const HighExpr *B) {
      A = PeelScalar(A);
      B = PeelScalar(B);
      if (!A || !B || A->Kind != B->Kind)
        return false;
      if (A->Kind == ExprKind::Var || A->Kind == ExprKind::Phi)
        return A->Var.Kind == B->Var.Kind && A->Var.Id == B->Var.Id;
      return false;
    };
    auto SamePair = [&](const HighExpr *A0, const HighExpr *A1,
                        const HighExpr *B0, const HighExpr *B1) {
      return (SameScalar(A0, B0) && SameScalar(A1, B1)) ||
             (SameScalar(A0, B1) && SameScalar(A1, B0));
    };
    auto PrintGe = [&](const HighExpr &Le) {
      std::string S = exprStr(Le);
      const auto Pos = S.find(" <= ");
      if (Pos != std::string::npos)
        S.replace(Pos, 4, " >= ");
      return S;
    };
    auto AsLe = [&](const HighExpr *Op) -> const HighExpr * {
      Op = forwardedExpr(Op);
      Op = unwrapIntegerView(Op);
      if (Op && Op->Kind == ExprKind::BinOp && Op->Operands.size() == 2 &&
          (Op->Op == NdOp::INT_LESSEQUAL || Op->Op == NdOp::INT_SLESSEQUAL))
        return Op;
      return nullptr;
    };
    auto AsNe = [&](const HighExpr *Op) -> const HighExpr * {
      Op = forwardedExpr(Op);
      Op = unwrapIntegerView(Op);
      if (Op && Op->Kind == ExprKind::BinOp && Op->Op == NdOp::INT_NOTEQUAL &&
          Op->Operands.size() == 2)
        return Op;
      return nullptr;
    };
    const HighExpr *Le = AsLe(Cur->Operands[0].get());
    const HighExpr *Ne = AsNe(Cur->Operands[1].get());
    if (!Le || !Ne) {
      Le = AsLe(Cur->Operands[1].get());
      Ne = AsNe(Cur->Operands[0].get());
    }
    if ((Cur->Op == NdOp::BOOL_AND || Cur->Op == NdOp::INT_AND) && Le && Ne &&
        SamePair(Le->Operands[0].get(), Le->Operands[1].get(),
                 Ne->Operands[0].get(), Ne->Operands[1].get()))
      return PrintGe(*Le);

    const bool BoolOr = Cur->Op == NdOp::BOOL_OR ||
                        (Cur->Op == NdOp::INT_OR &&
                         LooksBool(LooksBool, Cur->Operands[0].get(), 0) &&
                         LooksBool(LooksBool, Cur->Operands[1].get(), 0));
    const bool BoolAnd = Cur->Op == NdOp::BOOL_AND ||
                         (Cur->Op == NdOp::INT_AND &&
                          LooksBool(LooksBool, Cur->Operands[0].get(), 0) &&
                          LooksBool(LooksBool, Cur->Operands[1].get(), 0));
    if (BoolOr || BoolAnd) {
      std::string L = invertCondStr(*Cur->Operands[0]);
      std::string R = invertCondStr(*Cur->Operands[1]);
      if (BoolOr) {
        if (L.find(" || ") != std::string::npos)
          L = "(" + L + ")";
        if (R.find(" || ") != std::string::npos)
          R = "(" + R + ")";
        return L + " && " + R;
      }
      return L + " || " + R;
    }
    if (Cur->Op == NdOp::INT_EQUAL) {
      const HighExpr *X = nullptr;
      if (IsZeroLike(Cur->Operands[1]) && Cur->Operands[0])
        X = Cur->Operands[0].get();
      else if (IsZeroLike(Cur->Operands[0]) && Cur->Operands[1])
        X = Cur->Operands[1].get();
      if (X) {
        std::string S = exprStr(*X);
        const HighExpr *V = unwrapIntegerView(X);
        if (V && V->Kind == ExprKind::BinOp &&
            (V->Op == NdOp::BOOL_OR || V->Op == NdOp::BOOL_AND ||
             V->Op == NdOp::INT_OR || V->Op == NdOp::INT_AND))
          return "(" + S + ")";
        return S;
      }
    }
    NdOp Inv = NdOp::NOP;
    switch (Cur->Op) {
    case NdOp::INT_EQUAL:
      Inv = NdOp::INT_NOTEQUAL;
      break;
    case NdOp::INT_NOTEQUAL:
      Inv = NdOp::INT_EQUAL;
      break;
    default:
      break;
    }
    if (Inv != NdOp::NOP) {
      HighExpr Flipped = *Cur;
      Flipped.Op = Inv;
      return exprStr(Flipped);
    }
  }
  return "!(" + exprStr(*Cur) + ")";
}

bool HighCWriter::stmtHiddenFromC(const HighStmt &Stmt) const {
  if (Stmt.Kind == StmtKind::Assign && Stmt.Dst &&
      (Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) &&
      (AmbiguousFrameAliases.count(varName(Stmt.Dst->Var)) ||
       JoinPhiNames.count(varName(Stmt.Dst->Var))))
    return false;
  if (Analysis.DeadStmts.count(&Stmt) || Stmt.Kind == StmtKind::Nop)
    return true;
  if (Stmt.Kind == StmtKind::Block)
    return stmtsEffectivelyEmpty(Stmt.Body);

  const bool HideEHRuntimeMemory = CurrentFunc &&
                                   CurrentFunc->ExceptionMetadata.has_value() &&
                                   !preservesRegistrationMemory(*CurrentFunc);
  auto HiddenEH = [&](NdMemoryAddressSpace Space) {
    // x86 SEH registration is FS:[0].  x64 GS holds the TEB/TLS pointer and
    // must print when the value is used (NtCurrentTeb / __readgsqword).
    return HideEHRuntimeMemory && Space == NdMemoryAddressSpace::X86FS;
  };
  auto IsForeignFuncletParam = [&](const HighExpr &E) {
    if (!InEHClauseBody || !CurrentFunc || !isParamCopy(E))
      return false;
    const HighExpr *Src = unwrapIntegerView(&E);
    if (!Src)
      return false;
    const std::string Name = varName(Src->Var);
    for (const auto &Param : CurrentFunc->Params)
      if (Param.Name == Name)
        return false;
    return true;
  };

  if (Stmt.Kind == StmtKind::If)
    return !Stmt.Cond || stmtsEffectivelyEmpty(Stmt.Body);
  if (Stmt.Kind == StmtKind::IfElse)
    return !Stmt.Cond || (stmtsEffectivelyEmpty(Stmt.Body) &&
                          stmtsEffectivelyEmpty(Stmt.ElseBody));

  if (Stmt.Kind == StmtKind::Assign) {
    if (!Stmt.Dst || !Stmt.Val)
      return true;
    if (isIncomingParamReuseAssign(Stmt))
      return true;
    if (IsForeignFuncletParam(*Stmt.Val))
      return true;
    if (Stmt.Val->Kind == ExprKind::Load &&
        HiddenEH(Stmt.Val->MemoryAddressSpace))
      return true;
    if (Stmt.Dst->Kind == ExprKind::Load) {
      if (HiddenEH(Stmt.Dst->MemoryAddressSpace))
        return true;
      if (!Stmt.Dst->Operands.empty() &&
          Stmt.Dst->MemoryOrdering == NdMemoryOrdering::None &&
          Stmt.Dst->MemoryAddressSpace == NdMemoryAddressSpace::Default) {
        if (auto Slot = namedFrameSlot(*Stmt.Dst->Operands[0])) {
          if (isReservedParamDisplayName(*Slot))
            return true;
          if (isCompilerEHConstant(*Stmt.Val))
            return true;
          if (isParamCopy(*Stmt.Val)) {
            // Hide the Win64 sret/home write into an address-taken C
            // object (`CStringT var; f(&var)`).  An integer initializer
            // of `&slot` must stay (`var = arg0; return &var`).
            if (isAddressTakenSlot(*Slot) &&
                hidesAddressTakenParamHome(*Slot, *Stmt.Val))
              return true;
            if (auto Src = copyForwardSource(*Stmt.Val)) {
              auto It = CopyForward.find(*Slot);
              if (It != CopyForward.end() && It->second == *Src)
                return true;
            }
          }
        }
      }
    }
    if (isHiddenCopyForwardAssign(Stmt))
      return true;
    if ((Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi)) {
      const std::string Name = varName(Stmt.Dst->Var);
      if (!JoinPhiNames.count(Name) &&
          (FieldForward.count(Name) || ValueForward.count(Name) ||
           UnknownOnlyNames.count(Name)))
        return true;
    }
    if ((Stmt.Dst->Kind == ExprKind::Var || Stmt.Dst->Kind == ExprKind::Phi) &&
        Stmt.Val) {
      const std::string Name = varName(Stmt.Dst->Var);
      if (!AmbiguousFrameAliases.count(Name)) {
        auto Alias = FrameAliases.find(Name);
        if (Alias != FrameAliases.end()) {
          if (auto Disp = frameDisplacement(*Stmt.Val);
              Disp && *Disp == Alias->second)
            return true;
          // Funclet reuse of a parent FP temp for leftover EAX / flag bits
          // is not a second home.  Keep Var/Call copies so `v = dtor();`
          // can still fold onto the following return.
          if (InEHClauseBody && Stmt.Val->Kind != ExprKind::Var &&
              Stmt.Val->Kind != ExprKind::Phi &&
              Stmt.Val->Kind != ExprKind::Call &&
              Stmt.Val->Kind != ExprKind::Load)
            return true;
        }
      }
    }
    return false;
  }

  if (Stmt.Kind == StmtKind::Store) {
    if (!Stmt.StoreAddr || !Stmt.StoreVal)
      return true;
    if (IsForeignFuncletParam(*Stmt.StoreVal))
      return true;
    if (HiddenEH(Stmt.MemoryAddressSpace))
      return true;
    if (isCompilerEHConstant(*Stmt.StoreVal))
      return true;
    if (Stmt.MemoryOrdering == NdMemoryOrdering::None &&
        Stmt.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto Slot = namedFrameSlot(*Stmt.StoreAddr)) {
        if (isReservedParamDisplayName(*Slot))
          return true;
        if (isParamCopy(*Stmt.StoreVal)) {
          if (isAddressTakenSlot(*Slot) &&
              hidesAddressTakenParamHome(*Slot, *Stmt.StoreVal))
            return true;
          if (auto Src = copyForwardSource(*Stmt.StoreVal)) {
            auto It = CopyForward.find(*Slot);
            if (It != CopyForward.end() && It->second == *Src)
              return true;
          }
        }
      }
    }
    return false;
  }
  return false;
}

bool HighCWriter::isHiddenCopyForwardAssign(const HighStmt &Stmt) const {
  if (Stmt.Kind != StmtKind::Assign || !Stmt.Dst || !Stmt.Val)
    return false;
  if (Stmt.Dst->Kind != ExprKind::Var && Stmt.Dst->Kind != ExprKind::Phi)
    return false;
  if (!isCopyForwardDestination(Stmt.Dst->Var))
    return false;
  auto Src = copyForwardSource(*Stmt.Val);
  if (!Src)
    return false;
  auto It = CopyForward.find(varName(Stmt.Dst->Var));
  return It != CopyForward.end() && It->second == *Src;
}

bool HighCWriter::stmtsEffectivelyEmpty(
    const std::vector<HighStmt> &Stmts) const {
  for (const HighStmt &Stmt : Stmts) {
    if (Stmt.Addr != 0 && Stmt.Addr != InvalidVA &&
        GotoTargets.count(Stmt.Addr))
      return false;
    if (stmtHiddenFromC(Stmt))
      continue;
    if (InCxxCleanupBody && Stmt.Kind == StmtKind::Return)
      continue;
    return false;
  }
  return true;
}

void HighCWriter::markHiddenControlDead(const std::vector<HighStmt> &Stmts,
                                        bool Cleanup) {
  const bool SavedCleanup = InCxxCleanupBody;
  const bool SavedHandler = InEHClauseBody;
  InCxxCleanupBody = Cleanup;
  if (Cleanup)
    InEHClauseBody = true;
  for (const HighStmt &S : Stmts) {
    if ((S.Kind == StmtKind::If || S.Kind == StmtKind::IfElse) &&
        stmtHiddenFromC(S)) {
      Analysis.DeadStmts.insert(&S);
      std::function<void(const std::vector<HighStmt> &)> Kill =
          [&](const std::vector<HighStmt> &Inner) {
            for (const HighStmt &C : Inner) {
              Analysis.DeadStmts.insert(&C);
              Kill(C.Body);
              Kill(C.ElseBody);
              Kill(C.DefaultBody);
              for (const auto &Case : C.Cases)
                Kill(Case.Body);
            }
          };
      Kill(S.Body);
      Kill(S.ElseBody);
    }
    markHiddenControlDead(S.Body, Cleanup);
    markHiddenControlDead(S.ElseBody, Cleanup);
    markHiddenControlDead(S.DefaultBody, Cleanup);
    for (const auto &Case : S.Cases)
      markHiddenControlDead(Case.Body, Cleanup);
    for (size_t C = 0; C < S.EHClauseBodies.size(); ++C) {
      const bool ClauseCleanup =
          C < S.EHClauses.size() &&
          S.EHClauses[C].Kind == HighEHClauseKind::CxxCleanup;
      markHiddenControlDead(S.EHClauseBodies[C], ClauseCleanup);
    }
  }
  InCxxCleanupBody = SavedCleanup;
  InEHClauseBody = SavedHandler;
}

} // namespace neverd
