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
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/ErrorHandling.h"

#include <cstdint>

namespace neverd {

std::string HighCWriter::varName(const MedVar &V) const {
  if (CurrentFunc &&
      isSyntheticEntryStackPointer(V, *CurrentFunc, Opts.TheArch))
    return "frame_base";
  if (V.RenameTag >= 0)
    return "v" + std::to_string(V.RenameTag);
  switch (V.Kind) {
  case MedVar::Stack:
    if (Dbg && CurrentFunc) {
      const int64_t Candidates[] = {V.StackOff, V.StackOff + 4, V.StackOff - 4};
      for (int64_t Off : Candidates) {
        if (auto Var = Dbg->resolveVariable(CurrentFunc->Entry, Off);
            Var && !Var->Name.empty())
          return Var->Name;
      }
    }
    return "var_" + llvm::utohexstr(static_cast<uint64_t>(
                        V.StackOff < 0 ? -V.StackOff : V.StackOff));
  case MedVar::Param:
    if (Dbg && CurrentFunc) {
      if (auto FS = Dbg->resolveFunction(CurrentFunc->Entry);
          FS && V.Id >= 0 && static_cast<size_t>(V.Id) < FS->Params.size() &&
          !FS->Params[static_cast<size_t>(V.Id)].first.empty())
        return FS->Params[static_cast<size_t>(V.Id)].first;
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

std::string HighCWriter::constStr(uint64_t Val) {
  if (Val == 0)
    return "0";
  if (Val <= limits::kDecimalConstThreshold)
    return std::to_string(Val);

  if (Val == 0xFFFFFFFF || Val == 0xFFFFFFFFFFFFFFFFULL)
    return "-1";

  int64_t SV = static_cast<int64_t>(Val);
  if (SV < 0 && SV >= -static_cast<int64_t>(limits::kDecimalConstThreshold))
    return std::to_string(SV);

  int32_t SV32 = static_cast<int32_t>(Val & 0xFFFFFFFF);
  if (Val <= 0xFFFFFFFF && SV32 < 0 &&
      SV32 >= -static_cast<int32_t>(limits::kDecimalConstThreshold))
    return std::to_string(SV32);

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
  case NdOp::BOOL_NOT:
    return "!" + exprStr(*E.Operands[0], 99);
  case NdOp::INT_ZEXT: {
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const && Inner.ConstVal == 0)
      return "0";
    if (Inner.Type && E.Type && Inner.Type->Size == E.Type->Size)
      return exprStr(Inner, ParentPrec);
    if (Inner.Type)
      return "(" + typeToC(E.Type) + ")(" +
             typeToC(NdType::makeInt(Inner.Type->Size, false)) + ")" +
             exprStr(Inner, 99);
    return "(" + typeToC(E.Type) + ")" + exprStr(Inner, 99);
  }
  case NdOp::INT_SEXT: {
    auto &Inner = *E.Operands[0];
    if (Inner.Kind == ExprKind::Const && Inner.ConstVal == 0)
      return "0";
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

std::string HighCWriter::renderCallExpr(const HighExpr &E) {
  if (E.SourceCallHint)
    return renderSourceCallExpr(E);
  if (E.MemoryAddressSpace != NdMemoryAddressSpace::Default)
    llvm::report_fatal_error(
        "HighC cannot safely render a segmented-memory intrinsic");
  std::string Name = E.CallTarget;
  if (Name.empty())
    Name = (kAutoFuncPrefix + llvm::utohexstr(E.CallAddr)).str();

  if (E.IntrinsicId != Intrinsic::None) {
    auto Typed = renderX86TypedIntrinsicCall(
        Opts.TheArch, E, [this](const HighExpr &Expr) { return exprStr(Expr); },
        HasCIntrinsics);
    if (!Typed.empty())
      return Typed;

    std::vector<std::string> OpStrs;
    for (auto &Op : E.Operands)
      if (Op)
        OpStrs.push_back(exprStr(*Op));

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

    auto Rendered = renderIntrinsicCall(E.IntrinsicId, Opts.TheArch, OpStrs,
                                        HasCIntrinsics);
    if (!Rendered.empty())
      return Rendered;
  }

  if (E.IntrinsicId == Intrinsic::None)
    Name = functionIdentifier(Name);

  // A callee defined in this file has a prototype: convert between pointer
  // and integer arguments the way the machine passed them, in the register.
  const HighFunc *Defined = nullptr;
  if (E.IntrinsicId == Intrinsic::None && !E.CallTarget.empty())
    if (auto It = DefinedFuncs.find(E.CallTarget); It != DefinedFuncs.end())
      Defined = It->second;
  // Its printed signature also fixes how many arguments the call passes: a
  // value past its parameters is not read by it, and a parameter the call
  // site did not determine is an unknown value.
  const size_t ArgCount = Defined && !Defined->SourceTypeHint
                              ? emittedParamCount(*Defined)
                              : E.Operands.size();
  std::string S = Name + "(";
  for (size_t I = 0; I < ArgCount; ++I) {
    if (I > 0)
      S += ", ";
    if (I >= E.Operands.size()) {
      S += "0 /* unknown */";
      continue;
    }
    const HighExpr *Op = E.Operands[I].get();
    if (!Op) {
      S += "0";
      continue;
    }
    std::string Arg = exprStr(*Op);
    if (Defined && I < Defined->Params.size() && Defined->Params[I].Type &&
        Op->Type) {
      const TypeRef &Param = Defined->Params[I].Type;
      const bool ParamPtr = Param->Kind == NdTypeKind::Ptr;
      const bool ArgPtr = Op->Type->Kind == NdTypeKind::Ptr;
      if (ParamPtr != ArgPtr && (ParamPtr ? Op->Type->Kind == NdTypeKind::Int
                                          : Param->Kind == NdTypeKind::Int))
        Arg = "(" + typeToC(Param) + ")(uintptr_t)(" + Arg + ")";
    }
    S += Arg;
  }
  S += ")";
  return S;
}

TypeRef HighCWriter::declaredParamType(const MedVar &V) const {
  if (!CurrentFunc || V.Kind != MedVar::Param || V.RenameTag >= 0 || V.Id < 0 ||
      static_cast<size_t>(V.Id) >= CurrentFunc->Params.size())
    return nullptr;
  return CurrentFunc->Params[V.Id].Type;
}

bool HighCWriter::pointerNeedsIntegerView(const TypeRef &Ty) const {
  if (!Ty || Ty->Kind != NdTypeKind::Ptr)
    return false;
  // HighIR address math is in bytes.  A C byte pointer already has scale 1, so
  // `p + n` matches the IR.  Wider pointees would scale and must be viewed as
  // integers first; void* cannot be added at all.
  const TypeRef &Pointee = Ty->Pointee;
  return !Pointee || Pointee->Kind != NdTypeKind::Int || Pointee->Size != 1;
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
      if (isSyntheticEntryStackPointer(Cur->Var, *CurrentFunc, Opts.TheArch)) {
        // x64 SEH handlers are exceptional entries: LowIR models their RSP as
        // the function-entry value, but the unwinder has already established
        // the allocated frame.  Rebase onto a slot the try body already named,
        // unless SSA already placed handler RSP from the unwind operations.
        if (CurrentFunc->FrameSize > 0 &&
            !CurrentFunc->SEHHandlerFramesEstablished) {
          const int64_t Rebased = Acc - CurrentFunc->FrameSize;
          if (InEHClauseBody)
            return Rebased;
          if (CurrentFunc->ExceptionMetadata && FrameSlots.count(Rebased) &&
              !FrameSlots.count(Acc))
            return Rebased;
        }
        return Acc;
      }
      auto Alias = FrameAliases.find(varName(Cur->Var));
      if (Alias != FrameAliases.end())
        return Acc + Alias->second;
      // Incoming SP sometimes survives as an unassigned SSA 0 temp after
      // prologue lowering. Treat it as the frame so `sp+k` can name a slot.
      if (Acc != 0 && CurrentFunc->FrameSize > 0 && Cur->Var.SSAVer == 0 &&
          Cur->Var.RenameTag < 0 && Cur->Var.Kind != MedVar::Param &&
          !Analysis.AssignedVars.count(varName(Cur->Var)))
        return Acc;
      if (CurrentFunc->ExceptionMetadata && Cur->Var.Kind == MedVar::Reg &&
          Cur->Var.RenameTag < 0 &&
          Cur->Var.RegOff == getTargetRegInfo(Opts.TheArch).FramePointer) {
        const int64_t Slot =
            static_cast<int64_t>(getTargetRegInfo(Opts.TheArch).PointerSize);
        const int64_t Order[3] = {Acc - Slot, Acc, Acc + Slot};
        for (int64_t Adj : Order)
          if (FrameSlots.count(Adj))
            return Adj;
        return Acc;
      }
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
    std::string Name = copyForwardName(varName(E.Var));
    // A copy forwarded to a parameter prints as that parameter, so it has
    // the parameter's declared type.
    if (!DeclaredType && CurrentFunc)
      for (const HighParam &Param : CurrentFunc->Params)
        if (Param.Name == Name) {
          DeclaredType = Param.Type;
          break;
        }
    if (pointerNeedsIntegerView(DeclaredType))
      return "(uintptr_t)" + Name;
    return Name;
  }
  case ExprKind::Const:
    return constStr(E.ConstVal);
  case ExprKind::Undef:
    // Do not emit the former clobber-0 operand comment. Keep a short unknown
    // marker so ABI tests can still see that high bits were not invented.
    return "0 /* unknown */";
  case ExprKind::BinOp:
    if (auto Slot = namedFrameSlot(E))
      return "(uintptr_t)&" + *Slot;
    return renderBinOp(E, ParentPrec);
  case ExprKind::UnaryOp:
    return renderUnaryOp(E, ParentPrec);
  case ExprKind::Load: {
    if (E.Operands.empty())
      return "/* bad load */";
    std::string Addr = exprStr(*E.Operands[0]);
    if (E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default) {
      if (auto Fwd = forwardedStoreValue(*E.Operands[0], Addr))
        return "(" + typeToC(E.Type) + ")(" + *Fwd + ")";
      if (auto Slot = namedFrameSlot(*E.Operands[0]))
        return copyForwardName(*Slot);
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
    std::string Addr = exprStr(*E.Operands[0]);
    std::string Val = exprStr(*E.Operands[1]);
    if (E.MemoryOrdering == NdMemoryOrdering::None &&
        E.MemoryAddressSpace == NdMemoryAddressSpace::Default)
      if (auto Slot = namedFrameSlot(*E.Operands[0]))
        return *Slot + " = " + Val;
    return memoryStoreExpr(E.Operands[1]->Type, Addr, Val, E.MemoryOrdering,
                           E.MemoryAddressSpace);
  }
  case ExprKind::Call:
    return renderCallExpr(E);
  case ExprKind::Cast: {
    if (E.Operands.empty())
      return "/* bad cast */";
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
  auto It = ImageObjects.find(Addr);
  if (It == ImageObjects.end())
    return std::nullopt;
  return It->second.Name;
}

void HighCWriter::noteImageObject(va_t Addr, const TypeRef &Ty, bool Written) {
  if (!isImageDataAddress(Addr))
    return;
  ImageObject &Obj = ImageObjects[Addr];
  if (Obj.Name.empty()) {
    std::string Raw;
    if (Dbg) {
      for (const DataObjectSym &Data : Dbg->allDataObjects()) {
        if (Data.Addr == Addr && !Data.Name.empty()) {
          Raw = Data.Name;
          break;
        }
      }
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
  if (Ty && (!Obj.Type || Ty->Size > Obj.Type->Size))
    Obj.Type = Ty;
  if (!Obj.Type)
    Obj.Type = NdType::makeInt(4);
  (void)Written;
}

bool HighCWriter::isParamCopy(const HighExpr &E) const {
  const HighExpr *Cur = unwrapIntegerView(&E);
  return Cur && (Cur->Kind == ExprKind::Var || Cur->Kind == ExprKind::Phi) &&
         Cur->Var.Kind == MedVar::Param;
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

std::string HighCWriter::invertCondStr(const HighExpr &E) {
  if (E.Kind == ExprKind::UnaryOp && E.Op == NdOp::BOOL_NOT &&
      !E.Operands.empty() && E.Operands[0])
    return exprStr(*E.Operands[0]);
  if (E.Kind == ExprKind::BinOp && E.Operands.size() == 2 && E.Operands[0] &&
      E.Operands[1]) {
    NdOp Inv = NdOp::NOP;
    switch (E.Op) {
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
      HighExpr Flipped = E;
      Flipped.Op = Inv;
      return exprStr(Flipped);
    }
  }
  return "!(" + exprStr(E) + ")";
}

bool HighCWriter::stmtHiddenFromC(const HighStmt &Stmt) const {
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
          if (isCompilerEHConstant(*Stmt.Val))
            return true;
          if (isParamCopy(*Stmt.Val)) {
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
        if (isParamCopy(*Stmt.StoreVal)) {
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
    return false;
  }
  return true;
}

} // namespace neverd
