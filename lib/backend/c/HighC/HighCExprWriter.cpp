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

#include "HighCWriter.h"

#include "neverd/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
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
std::string frameStorageAddress(int64_t Displacement) {
  if (Displacement == 0)
    return "frame_base";
  const uint64_t Magnitude =
      Displacement < 0 ? uint64_t{0} - static_cast<uint64_t>(Displacement)
                       : static_cast<uint64_t>(Displacement);
  return "(frame_base " + std::string(Displacement < 0 ? "- " : "+ ") +
         std::to_string(Magnitude) + ")";
}
} // namespace

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
  return cDisplayType(Ty);
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
    if (Dbg && CurrentFunc) {
      if (auto FS = Dbg->resolveFunction(CurrentFunc->Entry); FS) {
        if (isMsvcIndirectReturn(FS->ReturnType)) {
          const bool HiddenSret =
              highIRIncludesIndirectReturn(*CurrentFunc, *FS);
          const int SretId = indirectReturnParamId(*FS);
          if (HiddenSret && V.Id == SretId)
            return "result";
          size_t DebugIdx = static_cast<size_t>(V.Id);
          if (HiddenSret && V.Id > SretId)
            DebugIdx = static_cast<size_t>(V.Id) - 1;
          if (V.Id >= 0 && DebugIdx < FS->Params.size() &&
              !FS->Params[DebugIdx].first.empty())
            return FS->Params[DebugIdx].first;
        } else if (V.Id >= 0 &&
                   static_cast<size_t>(V.Id) < FS->Params.size() &&
                   !FS->Params[static_cast<size_t>(V.Id)].first.empty()) {
          return FS->Params[static_cast<size_t>(V.Id)].first;
        }
      }
    }
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

std::string HighCWriter::constStr(uint64_t Val, TypeRef Type) {
  // A narrow bit pattern is negative only in a signed integer type.
  // Wider masks must retain their zero upper bytes.
  if (Type && Type->Kind == NdTypeKind::Int && Type->Size && Type->Size < 8) {
    const unsigned Bits = Type->Size * 8;
    const uint64_t Mask = (UINT64_C(1) << Bits) - 1;
    Val &= Mask;
    if (Type->IsSigned && (Val & (UINT64_C(1) << (Bits - 1))))
      Val |= ~Mask;
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

std::string HighCWriter::renderUnaryOp(const HighExpr &E, int ParentPrec) {
  if (E.Operands.empty())
    return "/* bad unary */";

  switch (E.Op) {
  case NdOp::INT_NOT:
  case NdOp::INT_NEGATE:
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
      return Cur && (Cur->Kind == ExprKind::Undef ||
                     (Cur->Kind == ExprKind::Const && Cur->ConstVal == 0));
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
      const uint16_t Sz =
          V && V->Type && V->Type->Size && V->Type->Size <= 4 ? V->Type->Size
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
      TypeRef Printed = Call->SourceCallHint ? Call->Type
                                            : knownCallReturnType(*Call);
      if (!Printed)
        Printed = Call->Type;
      if (Printed && Printed->Kind == NdTypeKind::Int &&
          !Printed->IsSigned && !E.Operands.empty() && E.Operands[0] &&
          E.Operands[0]->Type &&
          Printed->Size == E.Operands[0]->Type->Size)
        return exprStr(*Call, ParentPrec);
    }
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const)
      return exprStr(Inner, ParentPrec);
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    if (Inner.Type)
      return "(" + typeToC(E.Type) + ")(" +
             typeToC(NdType::makeInt(Inner.Type->Size, false)) + ")" +
             exprStr(Inner, 99);
    return "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99);
  }
  case NdOp::INT_SEXT: {
    if (const HighExpr *Call = typedCallResult(&E)) {
      TypeRef Printed = Call->SourceCallHint ? Call->Type
                                            : knownCallReturnType(*Call);
      if (!Printed)
        Printed = Call->Type;
      if (Printed && Printed->Kind == NdTypeKind::Int &&
          Printed->IsSigned && !E.Operands.empty() && E.Operands[0] &&
          E.Operands[0]->Type &&
          Printed->Size == E.Operands[0]->Type->Size)
        return exprStr(*Call, ParentPrec);
    }
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const)
      return exprStr(Inner, ParentPrec);
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    return "(" + typeToC(E.Type) + ")(" +
           (Inner.Type ? typeToC(NdType::makeInt(Inner.Type->Size, true))
                       : "int32_t") +
           ")" + exprStr(Inner, 99);
  }
  case NdOp::FLOAT_TRUNC:
    return "(" + typeToC(E.Type) + ")" + exprStr(*E.Operands[0], 99);
  case NdOp::POPCOUNT:
    return "__builtin_popcountll(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::LZCOUNT:
    return "__builtin_clzll(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_NEG:
    return "-" + exprStr(*E.Operands[0], 99);
  case NdOp::FLOAT_ABS:
    return "__builtin_fabs(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_SQRT:
    return "__builtin_sqrt(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_CEIL:
    return "__builtin_ceil(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_FLOOR:
    return "__builtin_floor(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ROUND:
    return "__builtin_round(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ROUNDEVEN:
    return "__builtin_nearbyint(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_ISNAN:
    return "__builtin_isnan(" + exprStr(*E.Operands[0]) + ")";
  case NdOp::FLOAT_INT2FLOAT:
  case NdOp::FLOAT_UINT2FLOAT:
  case NdOp::FLOAT_FLOAT2INT:
  case NdOp::FLOAT_FLOAT2UINT:
  case NdOp::FLOAT_FLOAT2FLOAT:
    return "(" + typeToC(E.Type) + ")" + exprStr(*E.Operands[0], 99);
  default:
    return "/* unary " + std::to_string(static_cast<int>(E.Op)) + " */ " +
           exprStr(*E.Operands[0]);
  }
}

std::string HighCWriter::resolvedCallTarget(const HighExpr &E) const {
  std::string Name = E.CallTarget;
  if (Name.empty() && E.CallAddr)
    Name = (kAutoFuncPrefix + llvm::utohexstr(E.CallAddr)).str();
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
    return Name;
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
      if (auto Data = Dbg->resolveDataObject(Slot);
          Data && !Data->Name.empty())
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
    return Name;
  }
  if (Name.empty() || llvm::StringRef(Name).starts_with(kAutoFuncPrefix) ||
      OrdinalName || llvm::StringRef(Name).starts_with("__imp_") ||
      llvm::StringRef(Name).starts_with("_imp_"))
    return DebugName;
  return Name;
}

std::string HighCWriter::renderCallExpr(const HighExpr &E) {
  if (E.SourceCallHint)
    return renderSourceCallExpr(E);
  if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error(
        "HighC cannot safely render a segmented-memory intrinsic");
  std::string Name = resolvedCallTarget(E);

  if (E.IntrinsicId != Intrinsic::None) {
    auto Typed = renderX86TypedIntrinsicCall(
        Opts.TheArch, E, [this](const HighExpr &Expr) { return exprStr(Expr); },
        HasCIntrinsics);
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
    auto CanOmit = [&](auto &&Self, const HighExpr *Op, unsigned Depth) -> bool {
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
    for (size_t I = 0; I < E.Operands.size(); ++I) {
      const auto &Op = E.Operands[I];
      if (!Op)
        continue;
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

    auto Rendered = renderIntrinsicCall(
        E.IntrinsicId, Opts.TheArch, OpStrs, E.Type ? E.Type->Size : 0,
        HasCIntrinsics);
    if (!Rendered.empty())
      return Rendered;
  }

  if (E.IntrinsicId == Intrinsic::None)
    Name = functionIdentifier(Name);
  if (E.IsIndirectCall && E.IndirectTarget &&
      (Name.empty() || Name == "indirect" || Name == "indirect_call"))
    Name = indirectCalleeStr(*E.IndirectTarget);

  std::string S = Name + "(";
  const auto Callee = debugCallee(E);
  const MsvcAtlCallee *Atl = msvcAtlCallee(Name);
  const size_t PrintedArgs = debugCallArgLimit(E);
  for (size_t I = 0; I < PrintedArgs; ++I) {
    if (I > 0)
      S += ", ";
    const HighExpr *Op = E.Operands[I].get();
    if (!Op) {
      S += "0";
      continue;
    }
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
      if (const TypeRef Expected = expectedDebugCallArgType(*Callee, I)) {
        S += exprStrAsTypedArg(*Op, Expected);
        continue;
      }
    }
    if (Atl) {
      if (const TypeRef Expected = I == 0
                                       ? msvcAtlSyntheticThis(Name, *Atl)
                                       : msvcAtlExpectedCallArgType(*Atl, I)) {
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
    if (const HighExpr *Imm = Immediate(Immediate, *Op, 0))
      S += exprStr(*Imm);
    else
      S += exprStr(*Op);
  }
  S += ")";
  return S;
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
      const std::string Name = functionIdentifier(resolvedCallTarget(E));
      if (auto It = DebugExternSigs.find(Name);
          It != DebugExternSigs.end() && !It->second.Params.empty())
        return It->second;
      return FS;
    }
  const std::string Name = functionIdentifier(resolvedCallTarget(E));
  if (auto It = DebugExternSigs.find(Name); It != DebugExternSigs.end())
    return It->second;
  return std::nullopt;
}

void HighCWriter::collectUnknownOnlyNames(const HighFunc &Func) {
  UnknownOnlyNames.clear();
  AssignedNames.clear();
  walkStmts(Func.Body, [&](const HighStmt &S) {
    const HighExpr *Call = S.Kind == StmtKind::Call ? S.CallExpr.get()
                                                   : S.Val.get();
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
  const std::string Name = functionIdentifier(resolvedCallTarget(E));
  if (const MsvcAtlCallee *Atl = msvcAtlCallee(Name))
    return msvcAtlSyntheticReturn(Atl->ReturnKind);
  if (const auto Callee = debugCallee(E))
    return Callee->ReturnType;
  return {};
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
      Inner->Operands.size() == 2 && Inner->Operands[0] &&
      Inner->Operands[1]) {
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
      }
      constexpr int AddPrec = 9;
      std::string B = addrStr(*Base, AddPrec, ProjectImageBacking);
      TypeRef BaseTy = Base->Type;
      if ((Base->Kind == ExprKind::Var || Base->Kind == ExprKind::Phi))
        if (auto Declared = declaredParamType(Base->Var))
          BaseTy = Declared;
      const bool BytePtr =
          BaseTy && BaseTy->Kind == NdTypeKind::Ptr && BaseTy->Pointee &&
          BaseTy->Pointee->Kind == NdTypeKind::Int && BaseTy->Pointee->Size == 1;
      if (!BytePtr && !llvm::StringRef(B).starts_with("(uintptr_t)"))
        B = "(uintptr_t)(" + B + ")";
      std::string S =
          B + (Minus ? " - " : " + ") + constStr(Off->ConstVal);
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
  if (Inner != &E)
    return exprStr(*Inner, ParentPrec);
  return exprStr(E, ParentPrec);
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
  const std::string Name = functionIdentifier(resolvedCallTarget(E));
  const MsvcAtlCallee *Atl = msvcAtlCallee(Name);
  auto UnknownAt = [&](size_t I) {
    return I < Have && isUnknownCallOperand(E.Operands[I].get());
  };
  auto KeepExtra = [&](size_t I) {
    return I < Have && isCtorDisplayOperand(E.Operands[I].get());
  };
  if (const auto Callee = debugCallee(E)) {
    const bool Indirect = isMsvcIndirectReturn(Callee->ReturnType);
    const bool Member = Indirect && isWin64MemberIndirectReturn(*Callee);
    size_t Limit = 0;
    if (Member)
      Limit = Callee->Params.empty() ? 1 : Callee->Params.size() + 1;
    else if (Indirect)
      Limit = Callee->Params.size() + 1;
    else
      Limit = Callee->Params.size();
    if (Limit == 0 && Atl && Atl->Kind == MsvcAtlCalleeKind::Dtor)
      Limit = Atl->MaxArgs;
    if (Limit > 0) {
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::Keep)
        return Have;
      if (Indirect &&
          (Member || isMsvcPointerEncodedClassReturn(Callee->ReturnType))) {
        const size_t SretIdx = Member ? 1 : 0;
        if (SretIdx < Have &&
            !looksLikeHiddenSretOperand(E.Operands[SretIdx].get()))
          --Limit;
      }
      const size_t Clamped = Clamp(Limit);
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::CtorDrop)
        return msvcAtlPrintedArgLimit(*Atl, Clamped, UnknownAt, KeepExtra);
      if (Atl && Atl->ArityKind == MsvcAtlArityKind::Fixed)
        return std::min(Clamped, static_cast<size_t>(Atl->MaxArgs));
      return Clamped;
    }
  }
  if (Atl)
    return msvcAtlPrintedArgLimit(*Atl, Have, UnknownAt, KeepExtra);
  if (auto Arity = libc::libcArity(Name);
      Arity && Arity->FpArgs == 0 && Arity->IntArgs >= 0)
    return Clamp(static_cast<size_t>(Arity->IntArgs));
  return Have;
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
  const std::string Name = functionIdentifier(resolvedCallTarget(Call));
  if (auto It = DebugExternSigs.find(Name); It != DebugExternSigs.end())
    if (TypeRef Ty = FromFS(It->second))
      return Ty;
  if (const MsvcAtlCallee *Atl = msvcAtlCallee(Name)) {
    if (Index == 0 || msvcAtlTypesCallArgAsPointer(*Atl, Index) ||
        (Atl->Kind == MsvcAtlCalleeKind::Ctor && Index == 1)) {
      TypeRef Ty = cDisplayType(msvcAtlSyntheticThis(Name, *Atl));
      if (isNamedPointerDisplay(Ty))
        return Ty;
    }
  }
  return nullptr;
}

TypeRef HighCWriter::expectedDebugCallArgType(const FunctionSym &FS,
                                              size_t Index) const {
  const bool Indirect = isMsvcIndirectReturn(FS.ReturnType);
  const bool Member = Indirect && isWin64MemberIndirectReturn(FS);
  TypeRef Sret;
  if (Indirect) {
    if (TypeRef Record = msvcIndirectReturnRecordType(FS.ReturnType))
      Sret = NdType::makePtr(cDisplayType(Record));
  }
  if (Member) {
    if (Index == 0)
      return FS.Params.empty() ? nullptr : FS.Params[0].second;
    if (Index == 1)
      return Sret;
    if (Index - 1 < FS.Params.size())
      return FS.Params[Index - 1].second;
    return nullptr;
  }
  if (Indirect) {
    if (Index == 0)
      return Sret;
    if (Index - 1 < FS.Params.size())
      return FS.Params[Index - 1].second;
    return nullptr;
  }
  if (Index < FS.Params.size())
    return FS.Params[Index].second;
  return nullptr;
}

std::string HighCWriter::exprStrAsTypedArg(const HighExpr &E,
                                           const TypeRef &Expected) {
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
    if (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi)
      return printedForwardedVar(copyForwardName(varName(Inner->Var)), 16);
    if (Inner->Kind == ExprKind::Addr)
      return exprStr(*Inner);
    if (Inner != &E)
      return exprStr(*Inner);
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
      if (auto It = ValueForward.find(Name); It != ValueForward.end() &&
          It->second && It->second->Kind == ExprKind::Const)
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

std::string HighCWriter::debugSignatureKey(const FunctionSym &FS) {
  std::string Key;
  if (FS.ReturnType)
    Key += typeToC(FS.ReturnType);
  Key += "/";
  for (const auto &Param : FS.Params) {
    Key += Param.first;
    Key += ":";
    if (Param.second)
      Key += typeToC(Param.second);
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
  TypeRef ReturnType = cDisplayType(FS.ReturnType);
  TypeRef SretPtr;
  const bool Indirect =
      debugExternUsesHiddenSret(FS, ExternName.empty() ? Identifier : ExternName);
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
  const MsvcAtlCallee *Atl = msvcAtlCallee(Identifier);
  auto Emit = [&](TypeRef Ty, std::string Name) {
    if (Atl && Atl->ArityKind == MsvcAtlArityKind::Fixed &&
        Emitted >= Atl->MaxArgs)
      return;
    Ty = cDisplayType(Ty);
    if (!Ty)
      Ty = NdType::makeInt(8);
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
  if (Atl && FS.Params.empty() && !Indirect)
    return msvcAtlSyntheticPrototype(
        Identifier, *Atl, Opts.TheArch == Arch::X64 && Atl->FastCall);
  if (Atl)
    ReturnType = msvcAtlSyntheticReturn(Atl->ReturnKind);
  if (Atl && Emitted == 0)
    Emit(msvcAtlSyntheticThis(Identifier, *Atl), "this");
  if (Atl && Emitted == 1 && msvcAtlTypesCallArgAsPointer(*Atl, 1))
    Emit(msvcAtlSyntheticThis(Identifier, *Atl), "src");
  if (Emitted == 0)
    Declarator += "void";
  std::string Prefix = "extern ";
  if (Opts.TheArch == Arch::X64 &&
      ((Atl && Atl->FastCall) || Member || Indirect ||
       FS.CallConv == DebugCallConv::Thiscall ||
       FS.CallConv == DebugCallConv::Fastcall))
    Prefix += "__fastcall ";
  return Prefix + declarationToC(ReturnType, Declarator + ")");
}

TypeRef HighCWriter::declaredParamType(const MedVar &V) const {
  if (!CurrentFunc || V.Kind != MedVar::Param || V.RenameTag >= 0 || V.Id < 0 ||
      static_cast<size_t>(V.Id) >= CurrentFunc->Params.size())
    return nullptr;
  return CurrentFunc->Params[V.Id].Type;
}

TypeRef HighCWriter::debugParamType(const MedVar &V) const {
  if (!Dbg || !CurrentFunc || V.Kind != MedVar::Param || V.RenameTag >= 0 ||
      V.Id < 0)
    return nullptr;
  auto FS = Dbg->resolveFunction(CurrentFunc->Entry);
  if (!FS)
    return nullptr;
  size_t DebugIdx = static_cast<size_t>(V.Id);
  if (isMsvcIndirectReturn(FS->ReturnType) &&
      highIRIncludesIndirectReturn(*CurrentFunc, *FS)) {
    const int SretId = indirectReturnParamId(*FS);
    if (V.Id == SretId)
      return nullptr;
    if (V.Id > SretId)
      DebugIdx = static_cast<size_t>(V.Id) - 1;
  }
  if (DebugIdx < FS->Params.size()) {
    TypeRef Ty = FS->Params[DebugIdx].second;
    if (Ty)
      Dbg->completeType(Ty);
    return Ty;
  }
  return nullptr;
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
  const bool IsParam = (Base.Kind == ExprKind::Var ||
                        Base.Kind == ExprKind::Phi) &&
                       Base.Var.Kind == MedVar::Param &&
                       Base.Var.RenameTag < 0;
  if (IsParam) {
    if (TypeRef Best =
            RicherRecord(Base.Type,
                         RicherRecord(declaredParamType(Base.Var),
                                      debugParamType(Base.Var))))
      return Best;
  }
  if (TypeRef FromExpr = RecordFrom(Base.Type))
    return FromExpr;
  if (Base.Kind == ExprKind::Load && !Base.Operands.empty() &&
      Base.Operands[0])
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
  if (auto It = ValueForward.find(FwdName); It != ValueForward.end() && It->second)
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
      return std::make_pair(L, uint64_t{1} << static_cast<unsigned>(R->ConstVal));
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
      Base->Operands[0] &&
      Base->MemoryOrdering == NdMemoryOrdering::None &&
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
HighCWriter::frameTypedMemberAccess(int64_t Disp,
                                     uint16_t AccessSize) const {
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
    if (Source && Source->Kind == ExprKind::Load &&
        !Source->Operands.empty() && Source->Operands[0])
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
    if (!Ambiguous && SourceType &&
        SourceType->Kind == NdTypeKind::Struct &&
        SourceType->Size == MachineSize)
      PrintedType = SourceType;
  }
  auto RawScalarStore = [&]() -> std::optional<std::string> {
    if (MachineSize != 1 && MachineSize != 2 && MachineSize != 4 &&
        MachineSize != 8 && MachineSize != 16)
      return std::nullopt;
    if (PrintedType && PrintedType->Kind == NdTypeKind::Ptr &&
        (MachineSize == 4 || MachineSize == 8))
      return "*(" + typeToC(PrintedType) + " *)&" +
             std::string(SlotName);
    if (!ConstInt && (!Val.Type || Val.Type->Kind != NdTypeKind::Int))
      return std::nullopt;
    return "*(" + typeToC(NdType::makeInt(MachineSize, false)) +
           " *)&" + std::string(SlotName);
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
          (ConstInt || (PrintedType &&
                        PrintedType->Kind == NdTypeKind::Int)))
        return std::string(SlotName) + ".types_";
      return RawScalarStore();
    }
    TypeRef FieldTy;
    for (size_t I = 0; I < Ty->FieldDisplayOffsets.size() &&
                       I < Ty->FieldDisplayTypes.size();
         ++I) {
      if (Ty->FieldDisplayOffsets[I] == 0 && Ty->FieldDisplayNames[I] == *Field) {
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
        return "((" + typeToC(Slot.CallType) + " *)&" + SlotName.str() +
               ")->" + Path->substr(Base.size());
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
      return "((" + typeToC(Slot.CallType) + " *)&" + Base.str() +
             ")->" + *Overlay;
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
  if (const auto OverlayDisp =
          frameOverlayDisplacement(*Base, Peeled->second))
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
        if (auto It = ValueForward.find(Raw); It != ValueForward.end() &&
            It->second &&
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

std::string HighCWriter::pointerObjectStr(const HighExpr &E) {
  const HighExpr *Inner = peelIntegerViewOps(&E);
  if (!Inner)
    Inner = &E;
  if (auto Member = typedMemberAccess(*Inner))
    return *Member;
  if (Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
      Inner->Operands[0] &&
      Inner->MemoryOrdering == NdMemoryOrdering::None &&
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
    return Name;
  }
  return exprStr(*Inner);
}

std::string HighCWriter::indirectCalleeStr(const HighExpr &E) {
  const unsigned PtrSize = Opts.TheArch == Arch::X86 ? 4u : 8u;
  const HighExpr *Cur = peelIntegerViewOps(&E);
  unsigned Depth = 0;
  const HighExpr *Base = Cur;
  while (Cur && Depth < 2) {
    if (Cur->Kind != ExprKind::Load || Cur->Operands.empty() ||
        !Cur->Operands[0] ||
        Cur->MemoryOrdering != NdMemoryOrdering::None ||
        Cur->MemoryAddressSpace != NdMemoryAddressSpace::Default)
      break;
    const uint16_t Size = Cur->Type ? Cur->Type->Size : 0;
    if (Size != 0 && Size != PtrSize)
      break;
    ++Depth;
    Base = Cur->Operands[0].get();
    Cur = peelIntegerViewOps(Base);
  }
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
        return "(*(void **)((uintptr_t)(" + SlotBase + ") + " +
               constStr(Off->ConstVal) + "))";
      }
    }
  }
  if (Depth == 0)
    return "(*" + exprStr(E) + ")";
  std::string B = pointerObjectStr(*Base);
  std::string Stars(Depth, '*');
  std::string Ptrs(Depth + 1, '*');
  return "(" + Stars + "(void " + Ptrs + ")(" + B + "))";
}

bool HighCWriter::pointerNeedsIntegerView(const TypeRef &Ty) const {
  // Machine carriers also participate in shifts and masks. Even byte
  // pointers, whose addition happens to have scale one, need an integer view.
  return Ty && Ty->Kind == NdTypeKind::Ptr;
}

bool HighCWriter::isSameWidthUnsigned(const HighExpr &E,
                                      uint16_t Width) const {
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
  if (!CurrentFunc)
    return std::nullopt;
  const auto &Slots = ProjectFrameAliasesIntoStorage ? FrameStorageSlots
                                                      : FrameSlots;
  const HighExpr *Cur = unwrapIntegerView(&E);
  int64_t Acc = 0;
  unsigned Depth = 0;
  while (Cur && Depth++ < limits::kMaxFrameDisplacementDepth) {
    Cur = unwrapIntegerView(Cur);
    if (!Cur)
      return std::nullopt;
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
        // x86 _except_handler3 re-enters with the established EBP
        // (`push ebp; mov ebp, esp`), which is entry ESP minus one slot.
        // Treating that EBP as displacement 0 makes [ebp-0x1c] an adjacent
        // slot from the try body's `(ESP-4)-0x1c`, and `v = EBP` aliases
        // poison the join load. Name EBP as that established frame.
        // This must run before the unassigned-SSA0 heuristic: incoming EBP
        // is also SSA 0 and would otherwise keep Acc as [ebp-k].
        if (Opts.TheArch == Arch::X86)
          return Acc - Slot;
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

std::optional<int64_t> HighCWriter::certifiedFrameStorageDisplacement(
    const HighExpr &E) const {
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
HighCWriter::namedFrameSlot(const HighExpr &E) const {
  const auto Disp = frameDisplacement(E);
  if (!Disp)
    return std::nullopt;
  const auto It = FrameSlots.find(*Disp);
  if (It == FrameSlots.end())
    return std::nullopt;
  return It->second.Name;
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

std::string HighCWriter::exprStr(const HighExpr &E, int ParentPrec) {
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
    if (auto Reach = ReachingCatchFields.find(Name);
        Reach != ReachingCatchFields.end())
      return Reach->second;
    if (auto Reach = ReachingCatchPtrs.find(Name);
        Reach != ReachingCatchPtrs.end())
      return Reach->second;
    if (InEHClauseBody && isCatchFuncletParentFrame(E.Var))
      return frameStorageAddress(CurrentFunc->FrameSize > 0
                                     ? -CurrentFunc->FrameSize
                                     : 0);
    // The registration handler's EBP already has an establisher identity in
    // frameDisplacement. Keep that identity when named slots are replaced by
    // byte storage; an independent SSA root does not make it an unknown input.
    if (FrameStorageActive && isRegistrationEstablisherFrame(E.Var))
      if (const auto Disp = frameDisplacement(E))
        return frameStorageAddress(*Disp);
    if (ProjectFrameAliasesIntoStorage) {
      if (CurrentFunc && isSyntheticEntryStackPointer(E.Var, *CurrentFunc,
                                                      Opts.TheArch))
        return "frame_base";
      // A frame pointer may be assigned only on a normal try path. Project
      // its certified displacement at each use so the exceptional edge also
      // addresses the single byte backing store, rather than reading that
      // potentially skipped C assignment.
      if (auto Alias = FrameAliases.find(RawName);
          Alias != FrameAliases.end())
        return frameStorageAddress(Alias->second);
      for (const auto &[Disp, Slot] : FrameStorageSlots)
        if (Slot.Name == Name && E.Var.Kind != MedVar::Param &&
            !isEmittedParamName(Name) && !isCxxCatchObjectName(Name))
          return memoryLoadExpr(Slot.Type ? Slot.Type : E.Type,
                                frameStorageAddress(Disp));
    }
    if (auto Printed = printedForwardedVar(Name, ParentPrec);
        Printed != Name)
      return Printed;
    // Definitions consisting only of unknown values are omitted from C.
    // Their observable uses must still fail, including renamed SSA temps.
    if (UnknownOnlyNames.count(Name))
      return "(__builtin_trap(), 0 /* unknown value */)";
    // An unassigned architectural register or flag can be a genuine unknown
    // live-in. Keep the failure at the point of use instead of emitting an
    // undeclared name or inventing zero.
    if ((E.Var.Kind == MedVar::Reg || E.Var.Kind == MedVar::Flag) &&
        E.Var.SSAVer == 0 && Name == RawName &&
        !AssignedNames.count(RawName) && !isEmittedParamName(RawName) &&
        !(CurrentFunc && isSyntheticEntryStackPointer(E.Var, *CurrentFunc,
                                                       Opts.TheArch)))
      return "(__builtin_trap(), 0 /* unknown register */)";
    if (auto Slot = namedFrameSlot(E)) {
      if (const auto Disp = frameDisplacement(E)) {
        auto It = FrameSlots.find(*Disp);
        if (It != FrameSlots.end() && It->second.UsedAsMemory)
          return "&" + *Slot;
      }
    }
    if (pointerNeedsIntegerView(DeclaredType))
      return "(uintptr_t)" + Name;
    return Name;
  }
  case ExprKind::Const: {
    bool AllowEmpty = false;
    if (Dbg) {
      if (auto Data = Dbg->resolveDataObject(E.ConstVal);
          Data && llvm::StringRef(Data->Name).starts_with("??_C@"))
        AllowEmpty = true;
    }
    if (auto Lit = imageStringLiteral(Opts.Image, E.ConstVal, AllowEmpty))
      return *Lit;
    // Preserve the existing exact-object spelling, but do not turn an
    // unrelated numeric immediate that happens to lie inside a backing range
    // into an address.
    if (ImageObjects.count(E.ConstVal) ||
        E.ConstProvenance == ConstantAddressProvenance::Address ||
        E.ConstProvenance == ConstantAddressProvenance::DataAddress)
      if (auto Backing = imageBackingAddress(E.ConstVal))
        return *Backing;
    if (auto Name = imageObjectName(E.ConstVal))
      return "&" + *Name;
    return constStr(E.ConstVal, E.Type);
  }
  case ExprKind::Undef:
    // Do not emit the former clobber-0 operand comment. Keep a short unknown
    // marker so ABI tests can still see that high bits were not invented.
    return "0 /* unknown */";
  case ExprKind::BinOp:
    if (ProjectFrameAliasesIntoStorage)
      if (const auto Disp = certifiedFrameStorageDisplacement(E))
        return frameStorageAddress(*Disp);
    if (auto Slot = namedFrameSlot(E))
      return "&" + *Slot;
    if (auto Member = typedMemberAddress(E))
      return "&" + *Member;
    return renderBinOp(E, ParentPrec);
  case ExprKind::UnaryOp:
    return renderUnaryOp(E, ParentPrec);
  case ExprKind::Load: {
    if (E.Operands.empty())
      return "/* bad load */";
    if (E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto VA = constAddress(*E.Operands[0])) {
        if (imageBackingAddress(*VA))
          return memoryLoadExpr(E.Type, addrStr(*E.Operands[0]),
                                E.MemoryOrdering, E.MemoryAddressSpace, true);
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
      if (auto VA = constAddress(*E.Operands[0])) {
        const uint16_t Size = E.Type ? E.Type->Size : 0;
        if (auto Imm = foldReadonlyScalar(*VA, Size))
          return constStr(*Imm);
        if (auto Name = imageObjectName(*VA))
          return *Name;
      }
      const TypeRef &AddressType = E.Operands[0]->Type;
      if (AddressType && AddressType->Kind == NdTypeKind::Ptr &&
          AddressType->Pointee && E.Type &&
          (E.Type->Kind != NdTypeKind::Int || E.Type->Size == 1 ||
           E.Type->Size == 2 || E.Type->Size == 4 || E.Type->Size == 8 ||
           E.Type->Size == 16 || E.Type->Size == 32 || E.Type->Size == 64) &&
          equalSourceTypes(AddressType->Pointee, E.Type))
        return "(*(" + memoryTypeName(E.Type) + " *)(" + Addr + "))";
    }
    return memoryLoadExpr(E.Type, Addr, E.MemoryOrdering, E.MemoryAddressSpace);
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
  case ExprKind::Call:
    return renderCallExpr(E);
  case ExprKind::Cast: {
    if (E.Operands.empty())
      return "/* bad cast */";
    if (const HighExpr *Call = typedCallResult(&E))
      return exprStr(*Call, ParentPrec);
    std::string Ty = E.CastTo ? typeToC(E.CastTo) : typeToC(E.Type);
    return "(" + Ty + ")" + exprStr(*E.Operands[0], 99);
  }
  case ExprKind::BitCast: {
    if (E.Operands.size() != 1 || !E.Operands[0] || !E.Type ||
        !E.Operands[0]->Type || E.Type->Size != E.Operands[0]->Type->Size)
      llvm::report_fatal_error("HighC cannot render an invalid bit cast");
    const HighExpr &Src = *E.Operands[0];
    if (E.Type->Kind == NdTypeKind::Int && Src.Type->Kind == NdTypeKind::Int) {
      if (E.Type->IsSigned == Src.Type->IsSigned)
        return exprStr(Src, ParentPrec);
      return "(" + typeToC(E.Type) + ")" + exprStr(Src, 99);
    }
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
      return "(" + typeToC(Operand.Type) + " *)(" +
             exprStr(*Operand.Operands[0]) + ")";
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
      Result += exprStr(*E.Operands[I]);
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
    if (Inner && (Inner->Kind == ExprKind::Var || Inner->Kind == ExprKind::Phi) &&
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

  // EAX/RAX leftovers are Cast / same-width zext / SUBBYTES 0 around the
  // i32 add. A real widen (i16→i64) stays so narrowing the C return does
  // not turn zero extension into sign extension.
  auto PeelReturnViews = [&](const HighExpr *Cur) {
    for (unsigned Peel = 0; Peel < limits::kMaxIntegerViewUnwrapDepth && Cur;
         ++Peel) {
      if ((Cur->Kind == ExprKind::Cast || Cur->Kind == ExprKind::BitCast) &&
          !Cur->Operands.empty() && Cur->Operands[0]) {
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
    const bool SameWidth =
        Inner->Type && Inner->Type->Kind == NdTypeKind::Int &&
        Inner->Type->Size == FuncReturnType->Size;
    const bool SameSign =
        SameWidth && Inner->Type->IsSigned == FuncReturnType->IsSigned;
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
      // zero extension into sign extension.
      return "(" + typeToC(FuncReturnType) + ")(" + exprStr(Expr) + ")";
    }
  }

  if (Expr.Type && Expr.Type->Kind == NdTypeKind::Int &&
      Expr.Type->Size > FuncReturnType->Size) {
    return "(" + typeToC(FuncReturnType) + ")" + exprStr(Expr);
  }

  return exprStr(Expr);
}

std::string HighCWriter::printedForwardedVar(const std::string &Name,
                                            int ParentPrec) {
  if (auto Fwd = FieldForward.find(Name); Fwd != FieldForward.end())
    return Fwd->second;
  if (auto Fwd = ValueForward.find(Name); Fwd != ValueForward.end() &&
      Fwd->second) {
    const HighExpr *Inner = peelIntegerViewOps(Fwd->second);
    if (Inner && Inner->Kind == ExprKind::Load && !Inner->Operands.empty() &&
        Inner->Operands[0]) {
      if (auto Member = typedMemberAccess(*Inner->Operands[0]))
        return *Member;
    }
    return exprStr(*Fwd->second, ParentPrec);
  }
  if (auto Fwd = CtorThisForward.find(Name); Fwd != CtorThisForward.end() &&
      Fwd->second)
    return exprStr(*Fwd->second, ParentPrec);
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
  if (!isImageDataAddress(Addr) || !Opts.Image)
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

std::optional<std::string> HighCWriter::imageBackingAddress(va_t Addr) const {
  for (const ImageBacking &Backing : ImageBackings) {
    if (Addr < Backing.Base)
      break;
    if (Addr < Backing.End)
      return "&" + Backing.Name + "[" + std::to_string(Addr - Backing.Base) +
             "]";
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
    if (Dbg) {
      if (auto Data = Dbg->resolveDataObject(Addr); Data && !Data->Name.empty())
        Raw = Data->Name;
    }
    if (Raw.empty() && Opts.Image) {
      if (const Symbol *Sym = Opts.Image->findSymbolAt(Addr);
          Sym && !Sym->IsFunc && !Sym->Name.empty() &&
          llvm::StringRef(Sym->Name).find(kAutoFuncPrefix) != 0)
        Raw = stripLeadingUnderscores(Sym->Name).str();
    }
    if (!Raw.empty())
      Obj.Name = GlobalIdentifierAllocator.allocate(Raw, "g");
  }
  auto NamedDisplayPointer = [](const TypeRef &Type) {
    return Type && Type->Kind == NdTypeKind::Ptr && Type->Pointee &&
           !Type->Pointee->SourceName.empty();
  };
  if (Ty) {
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
  (void)Written;
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

const HighExpr *HighCWriter::parentFrameStoredValue(const HighStmt &Stmt) const {
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
  const HighExpr *Cur = forwardedExpr(&E);
  Cur = unwrapIntegerView(Cur);
  auto IsZeroLike = [this](const ExprPtr &Op) {
    if (!Op)
      return false;
    const HighExpr *Z = unwrapIntegerView(Op.get());
    return Z && (Z->Kind == ExprKind::Undef ||
                 (Z->Kind == ExprKind::Const && Z->ConstVal == 0));
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
      if (auto Member = typedMemberAccess(
              *Inner->Operands[0], Inner->Type ? Inner->Type->Size : 0))
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
  return MemberOrExpr(*Le->Operands[1]) + " > " + MemberOrExpr(*Le->Operands[0]);
}

std::string HighCWriter::invertCondStr(const HighExpr &E) {
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
    return Z && (Z->Kind == ExprKind::Undef ||
                 (Z->Kind == ExprKind::Const && Z->ConstVal == 0));
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

    const bool BoolOr =
        Cur->Op == NdOp::BOOL_OR ||
        (Cur->Op == NdOp::INT_OR && LooksBool(LooksBool, Cur->Operands[0].get(), 0) &&
         LooksBool(LooksBool, Cur->Operands[1].get(), 0));
    const bool BoolAnd =
        Cur->Op == NdOp::BOOL_AND ||
        (Cur->Op == NdOp::INT_AND && LooksBool(LooksBool, Cur->Operands[0].get(), 0) &&
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

  const bool HideEHRuntimeMemory =
      CurrentFunc && CurrentFunc->ExceptionMetadata.has_value();
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
